/*
AdbWinGui (Android Debug Bridge Windows GUI)
Copyright (C) 2017  singun

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// MainTab.cpp : implement of the MainTab class
//
/////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include <Uxtheme.h>
#include "MainTab.h"
#include "MessageDefine.h"
#include "MessageTaskDlg.h"
#include "Config/ConfigManager.h"
#include "Utils/ShellHelper.h"
#include "InstallNotifyDlg.h"
#include "FileExistsDlg.h"

#define SMALL_ICON_SIZE 24

MainTab::MainTab() : m_ddmLibWrapper(DdmLibWrapper::GetInstance())
{
	m_bIsCancelled = FALSE;
	m_emStatus = em_Status_Idle;
}

MainTab::~MainTab()
{
	if (m_taskInstall.valid())
	{
		m_taskInstall.get();
	}
	if (m_taskCopy.valid())
	{
		m_taskCopy.get();
	}
}

BOOL MainTab::PreTranslateMessage(MSG* pMsg)
{
	return IsDialogMessage(pMsg);
}

BOOL MainTab::OnInitDialog(CWindow wndFocus, LPARAM lInitParam)
{
	DlgResize_Init(false, false, WS_CHILD);
	PrepareAdb();
	InitControls();
	return TRUE;
}

void MainTab::OnDropFiles(HDROP hDropInfo)
{
	TCHAR szFilePathName[MAX_PATH] = { 0 };
	::DragQueryFile(hDropInfo, 0, szFilePathName, MAX_PATH);
	::DragFinish(hDropInfo);

	LPCTSTR lpszExt = ::PathFindExtension(szFilePathName);
	if (_tcsicmp(_T(".APK"), lpszExt) == 0)
	{
		InstallNotify emNotify = ConfigManager::GetInstance().GetInstallNotifyConfig();
		switch (emNotify)
		{
		case em_InstallDefault:
			OnDefaultInstallDialog(szFilePathName);
			break;
		case em_InstallDirect:
			OnInstallApkDirect(szFilePathName);
			break;
		case em_InstallWithCopy:
			OnCopyAndInstallApk(szFilePathName);
			break;
		}
	}
	else
	{
		MessageTaskDlg dlg(IDS_NOT_SUPPORTED_FILE, IDS_ONLY_APK_SUPPORTED, MB_ICONWARNING);
		dlg.DoModal(m_hWnd);
	}
}

void MainTab::OnEditFilterChange(UINT uNotifyCode, int nID, CWindow wndCtl)
{
	CString strFilter;
	m_ediFilter.GetWindowText(strFilter);
	BOOL bEmpty = strFilter.IsEmpty();

	m_lvApkDir.DeleteAllItems();
	for (int i = 0; i < m_arrApkPath.GetSize(); i++)
	{
		CString& file = m_arrApkPath[i];
		if (bEmpty || file.Find(strFilter) >= 0)
		{
			m_lvApkDir.AddItem(i, 0, file, 0);
		}
	}
}

void MainTab::OnBtnRefreshClick(UINT uNotifyCode, int nID, CWindow wndCtl)
{
	m_ediFilter.SetWindowText(_T(""));
	RefreshApkDirectory();
}

void MainTab::OnBtnStopInstallClick(UINT uNotifyCode, int nID, CWindow wndCtl)
{
	m_bIsCancelled = TRUE;
}

LRESULT MainTab::OnListKeyDown(LPNMHDR pnmh)
{
	LPNMLVKEYDOWN pnkd = (LPNMLVKEYDOWN)pnmh;
	if (pnkd->wVKey == VK_DELETE)
	{
		int nIndex = m_lvApkDir.GetSelectedIndex();
		if (nIndex >= 0)
		{
			CString strApkPath;
			if (GetListItemApkPath(nIndex, strApkPath))
			{
				strApkPath += _T('\0');
				INT nRet = ShellHelper::DeleteFile(strApkPath);
				if (nRet == 0)
				{
					m_lvApkDir.DeleteItem(nIndex);
				}
				else if (nRet != -1)
				{
					ShowDeleteFailDialog(::GetLastError());
				}
			}
		}
	}

	return 0;
}

LRESULT MainTab::OnListDblClick(LPNMHDR pnmh)
{
	LPNMITEMACTIVATE lpnmitem = (LPNMITEMACTIVATE)pnmh;
	if (lpnmitem->iItem >= 0)
	{
		CString strApkPath;
		if (GetListItemApkPath(lpnmitem->iItem, strApkPath))
		{
			OnInstallApkDirect(strApkPath);
		}
	}

	return 0;
}

LRESULT MainTab::OnRequestApkInstall(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	LPCTSTR lpszApkPath = (LPCTSTR)lParam;
	if (m_emStatus == em_Status_Idle)
	{
		OnInstallApkDirect(lpszApkPath, FALSE);
	}
	else
	{
		MessageTaskDlg dlg(IDS_INSTALL_NOT_IDLE, IDS_WAIT_INSTALL_FINISH, MB_ICONERROR);
		dlg.DoModal(m_hWnd);
	}
	return 0;
}

LRESULT MainTab::OnApkInstalled(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	TCHAR* szApkPath = (TCHAR*)lParam;
	int nRet = (int)wParam;
	SwitchToIdleMode();
	if (nRet == 0 && m_strErrMsg.IsEmpty())
	{
		// install success
		MessageTaskDlg dlg(IDS_INSTALL_SUCCESS, szApkPath, MB_ICONINFORMATION);
		dlg.DoModal(m_hWnd);
	}
	else if (!m_bIsCancelled)
	{
		CString strMsg;
		strMsg.LoadString(IDS_INSTALL_FAILED);
		if (!m_strErrMsg.IsEmpty())
		{
			strMsg.AppendFormat(_T("\r\n[%s]"), m_strErrMsg);
		}
		MessageTaskDlg dlg(strMsg, szApkPath, MB_ICONERROR);
		dlg.DoModal(m_hWnd);
	}
	// need to free string here
	delete[] szApkPath;
	return 0;
}

LRESULT MainTab::OnApkCopied(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	TCHAR* lpszDesc = (TCHAR*)lParam;
	if (m_taskCopy.valid())
	{
		BOOL bRet = m_taskCopy.get();
		if (bRet)
		{
			RefreshApkDirectory();
			OnInstallApkDirect(lpszDesc);
		}
		else
		{
			SwitchToIdleMode();
			DWORD dwErrCode = (DWORD)wParam;
			ShowCopyFailDialog(dwErrCode);
		}
	}
	else
	{
		SwitchToIdleMode();
		MessageTaskDlg dlg(IDS_FILE_COPY_FAILED, IDS_THREAD_EXCEPTION, MB_ICONERROR);
		dlg.DoModal(m_hWnd);
	}
	// need to free string here
	delete[] lpszDesc;
	return 0;
}

LRESULT MainTab::OnInstallPush(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	CString strWinText;
	strWinText.LoadString(IDS_NOTICE_PUSHING);
	m_stcNoticeApk.SetWindowText(strWinText);
	m_pgbInstall.ModifyStyle(PBS_MARQUEE, 0);
	m_pgbInstall.SetMarquee(FALSE);
	m_pgbInstall.SetRange(0, 100);
	m_pgbInstall.Invalidate();
	return 0;
}

LRESULT MainTab::OnInstallProgress(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	int progress = (int)wParam;
	int nPos = m_pgbInstall.GetPos();
	if (progress - nPos >= 1)
	{
		CString strWinText;
		strWinText.LoadString(IDS_NOTICE_PUSHING);
		strWinText.AppendFormat(_T("%d%%"), progress);
		m_stcNoticeApk.SetWindowText(strWinText);

		m_pgbInstall.SetPos(progress);
		m_pgbInstall.Invalidate();
	}
	return 0;
}

LRESULT MainTab::OnInstallRun(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	CString strWinText;
	strWinText.LoadString(IDS_NOTICE_INSTALLING);
	m_stcNoticeApk.SetWindowText(strWinText);
	m_pgbInstall.ModifyStyle(0, PBS_MARQUEE);
	m_pgbInstall.SetMarquee(TRUE, 10);
	m_btnInstallApk.EnableWindow(FALSE);
	return 0;
}

void MainTab::OnPush()
{
	PostMessage(MSG_INSTALL_STEP_PUSH);
}

void MainTab::OnProgress(int progress)
{
	PostMessage(MSG_INSTALL_STEP_PROGRESS, (WPARAM)progress);
}

void MainTab::OnInstall()
{
	PostMessage(MSG_INSTALL_STEP_INSTALL);
}

void MainTab::OnRemove()
{
	// nothing to do
}

void MainTab::OnErrorMessage(const TString errMsg)
{
	m_strErrMsg = errMsg;
}

bool MainTab::IsCancelled()
{
	return m_bIsCancelled ? true : false;
}

void MainTab::PrepareAdb()
{
	GetParent().PostMessage(MSG_MAIN_PREPARE_ADB);
}

void MainTab::InitControls()
{
	m_stcNoticeApk.Attach(GetDlgItem(IDC_STATIC_APK));
	m_btnInstallApk.Attach(GetDlgItem(IDC_BUTTON_INSTALL));
	m_stcFilter.Attach(GetDlgItem(IDC_STATIC_FILTER));
	m_ediFilter.Attach(GetDlgItem(IDC_EDIT_APK_FILTER));
	m_btnRefresh.Attach(GetDlgItem(IDC_BUTTON_REFRESH));

	m_pgbInstall.Attach(GetDlgItem(IDC_PROGRESS_INSTALL));
	m_chkReistall.Attach(GetDlgItem(IDC_CHECK_REINSTALL));
	m_stcListInstall.Attach(GetDlgItem(IDC_STATIC_NOTICE_LIST));
	m_lvApkDir.Attach(GetDlgItem(IDC_LIST_APK));

	m_bmpApkIco = (HBITMAP)::LoadImage(ModuleHelper::GetResourceInstance(),
		MAKEINTRESOURCE(IDB_BITMAP_APK), IMAGE_BITMAP, SMALL_ICON_SIZE, SMALL_ICON_SIZE, NULL);
	m_ilApkIcon.Create(SMALL_ICON_SIZE, SMALL_ICON_SIZE, ILC_COLOR32 | ILC_HIGHQUALITYSCALE, 1, 1);
	m_ilApkIcon.Add(m_bmpApkIco);
	m_lvApkDir.SetImageList(m_ilApkIcon, LVSIL_SMALL);

	::SetWindowTheme(m_lvApkDir, _T("Explorer"), NULL);

	m_bmpRefresh = (HBITMAP)::LoadImage(ModuleHelper::GetResourceInstance(),
		MAKEINTRESOURCE(IDB_BITMAP_REFRESH), IMAGE_BITMAP, SMALL_ICON_SIZE, SMALL_ICON_SIZE, NULL);
	m_btnRefresh.SetBitmap(m_bmpRefresh);

	m_chkReistall.SetCheck(TRUE);

	RefreshApkDirectory();
	SwitchToIdleMode();
}

void MainTab::OnDefaultInstallDialog(LPCTSTR lpszApkPath)
{
	InstallNotify emNotify = em_InstallDefault;
	BOOL bChecked = FALSE;
	InstallNotifyDlg dlg(lpszApkPath);
	int nClick = dlg.DoModal(m_hWnd, &bChecked);
	switch (nClick)
	{
	case InstallNotifyDlg::em_Button_Install_Direct:
		OnInstallApkDirect(lpszApkPath);
		emNotify = em_InstallDirect;
		break;
	case InstallNotifyDlg::em_Button_Install_With_Copy:
		OnCopyAndInstallApk(lpszApkPath);
		emNotify = em_InstallWithCopy;
		break;
	case InstallNotifyDlg::em_Button_Install_Cancel:
		break;
	}
	if (bChecked)
	{
		ConfigManager::GetInstance().SetInstallNotifyConfig(emNotify);
	}
}

void MainTab::OnInstallApkDirect(LPCTSTR lpszApkPath, BOOL bNotifyMainFrame)
{
	m_bIsCancelled = FALSE;
	m_strErrMsg.Empty();

	TCHAR* szApkPath = new TCHAR[MAX_PATH];
	_tcsncpy_s(szApkPath, MAX_PATH, lpszApkPath, MAX_PATH);
	if (bNotifyMainFrame)
	{
		// notify main frame
		GetParent().SendMessage(MSG_MAIN_INSTALL_APK, 0, (LPARAM)szApkPath);
	}

	IDevice* pDevice = m_ddmLibWrapper.GetSelectedDevice();
	if (pDevice != NULL)
	{
		SwitchToInstallingMode();
		BOOL bReInstall = m_chkReistall.GetCheck();
		HWND& hWnd = m_hWnd;
		// create a thread to run install task
		IDevice::IInstallNotify* pNotify = this;
		std::packaged_task<int(TCHAR*, IDevice*, BOOL)> ptInstall([&hWnd, pNotify](TCHAR* szApkPath, IDevice* pDevice, BOOL bReInstall)
		{
			int nRet = pDevice->InstallPackage(szApkPath, bReInstall ? true : false, NULL, 0, pNotify);
			::PostMessage(hWnd, MSG_INSTALL_APK_FIISH, (WPARAM)nRet, (LPARAM)szApkPath);
			return nRet;
		});
		m_taskInstall = ptInstall.get_future();
		std::thread(std::move(ptInstall), szApkPath, pDevice, bReInstall).detach();
	}
	else
	{
		SwitchToIdleMode();
		MessageTaskDlg dlg(IDS_NO_AVAILABLE_DEVICE, IDS_CONNECT_AND_RETRY, MB_ICONERROR);
		dlg.DoModal(m_hWnd);
		delete[] szApkPath;
	}
}

void MainTab::OnCopyAndInstallApk(LPCTSTR lpszApkPath)
{
	m_bIsCancelled = FALSE;
	m_strErrMsg.Empty();

	TCHAR* szSrcPath = new TCHAR[MAX_PATH];
	_tcsncpy_s(szSrcPath, MAX_PATH, lpszApkPath, MAX_PATH);

	LPCTSTR lpszFileName = ::PathFindFileName(lpszApkPath);
	LPCTSTR lpszApkDir = ConfigManager::GetInstance().GetApkDir();
	TCHAR* szDescPath = new TCHAR[MAX_PATH];
	_tcsncpy_s(szDescPath, MAX_PATH, lpszApkDir, MAX_PATH);
	::PathAppend(szDescPath, lpszFileName);

	if (_tcscmp(lpszApkPath, szDescPath) == 0)
	{
		// the same file
		ShowCopyFailDialog(ERROR_FILE_EXISTS);
		delete[] szSrcPath;
		delete[] szDescPath;
		return;
	}
	if (::PathFileExists(szDescPath))
	{
		// file exists
		if (!CheckAndShowReplaceDialog(lpszApkPath, szDescPath))
		{
			delete[] szSrcPath;
			delete[] szDescPath;
			return;
		}
	}

	// notify main frame
	GetParent().SendMessage(MSG_MAIN_INSTALL_APK, 0, (LPARAM)szDescPath);
	SwitchToCopyingMode();
	HWND& hWnd = m_hWnd;
	// create a thread to run copy task
	std::packaged_task<BOOL(TCHAR*, TCHAR*)> ptCopy([&hWnd](TCHAR* lpszSrc, TCHAR* lpszDesc)
	{
		DWORD dwErrCode = 0;
		BOOL bRet = ::CopyFile(lpszSrc, lpszDesc, FALSE);
		if (!bRet)
		{
			dwErrCode = ::GetLastError();
		}
		::PostMessage(hWnd, MSG_INSTALL_APK_COPYED, (WPARAM)dwErrCode, (LPARAM)lpszDesc);
		// need to free string here
		delete[] lpszSrc;
		return bRet;
	});
	m_taskCopy = ptCopy.get_future();
	std::thread(std::move(ptCopy), szSrcPath, szDescPath).detach();
}

void MainTab::ShowFileOperationFailDialog(int nId, DWORD dwErrCode)
{
	LPCTSTR lpszErrMsg = ShellHelper::GetErrorMessage(dwErrCode);
	MessageTaskDlg dlg(nId, lpszErrMsg, MB_ICONERROR);
	dlg.DoModal(m_hWnd);
}

void MainTab::ShowCopyFailDialog(DWORD dwErrCode)
{
	ShowFileOperationFailDialog(IDS_FILE_COPY_FAILED, dwErrCode);
}

void MainTab::ShowDeleteFailDialog(DWORD dwErrCode)
{
	ShowFileOperationFailDialog(IDS_DELETE_FILE_FAILED, dwErrCode);
}

BOOL MainTab::CheckAndShowReplaceDialog(LPCTSTR lpszFromPath, LPCTSTR lpszToPath)
{
	ConfigManager& cfgManager = ConfigManager::GetInstance();
	if (cfgManager.GetForceReplace())
	{
		::DeleteFile(lpszToPath);
	}
	else
	{
		// notify file exist
		BOOL bCheck = FALSE;
		FileExistsDlg dlg(lpszFromPath, lpszToPath);
		if (dlg.DoModal(m_hWnd, &bCheck) == IDOK)
		{
			::DeleteFile(lpszToPath);
			if (bCheck)
			{
				cfgManager.SetForceReplace(TRUE);
			}
		}
		else
		{
			return FALSE;
		}
	}
	return TRUE;
}

void MainTab::SwitchToCopyingMode()
{
	CString strWinText;
	strWinText.LoadString(IDS_NOTICE_COPYING);
	m_stcNoticeApk.SetWindowText(strWinText);
	m_pgbInstall.ModifyStyle(0, PBS_MARQUEE);
	m_pgbInstall.SetMarquee(TRUE, 10);
	m_pgbInstall.ShowWindow(SW_SHOW);
	m_pgbInstall.Invalidate();
	m_btnInstallApk.EnableWindow(TRUE);
	m_btnInstallApk.ShowWindow(SW_SHOW);
	m_chkReistall.EnableWindow(FALSE);
	m_stcFilter.EnableWindow(FALSE);
	m_ediFilter.EnableWindow(FALSE);
	m_btnRefresh.EnableWindow(FALSE);
	m_stcListInstall.EnableWindow(FALSE);
	m_lvApkDir.EnableWindow(FALSE);
	DragAcceptFiles(FALSE);

	m_emStatus = em_Status_Copying;
}

void MainTab::SwitchToInstallingMode()
{
	m_pgbInstall.ModifyStyle(0, PBS_MARQUEE);
	m_pgbInstall.SetMarquee(TRUE, 10);
	m_pgbInstall.ShowWindow(SW_SHOW);
	m_pgbInstall.Invalidate();
	m_btnInstallApk.EnableWindow(TRUE);
	m_btnInstallApk.ShowWindow(SW_SHOW);
	m_chkReistall.EnableWindow(FALSE);
	m_stcFilter.EnableWindow(FALSE);
	m_ediFilter.EnableWindow(FALSE);
	m_btnRefresh.EnableWindow(FALSE);
	m_stcListInstall.EnableWindow(FALSE);
	m_lvApkDir.EnableWindow(FALSE);
	DragAcceptFiles(FALSE);

	m_emStatus = em_Status_Installing;
}

void MainTab::SwitchToIdleMode()
{
	CString strWinText;
	strWinText.LoadString(IDS_NOTICE_DRAG_DROP);
	m_stcNoticeApk.SetWindowText(strWinText);
	m_pgbInstall.ModifyStyle(PBS_MARQUEE, 0);
	m_pgbInstall.SetMarquee(FALSE);
	m_pgbInstall.ShowWindow(SW_HIDE);
	m_btnInstallApk.ShowWindow(SW_HIDE);
	m_chkReistall.EnableWindow(TRUE);
	m_stcFilter.EnableWindow(TRUE);
	m_ediFilter.EnableWindow(TRUE);
	m_btnRefresh.EnableWindow(TRUE);
	m_stcListInstall.EnableWindow(TRUE);
	m_lvApkDir.EnableWindow(TRUE);
	DragAcceptFiles(TRUE);

	m_emStatus = em_Status_Idle;
}

BOOL MainTab::RefreshApkDirectory()
{
	return RefreshApkDirectoryWithArray(m_arrApkPath);
}

BOOL MainTab::RefreshApkDirectoryWithArray(CSimpleArray<CString>& arrApkPath)
{
	m_lvApkDir.DeleteAllItems();

	CString strApkDirectory = ConfigManager::GetInstance().GetApkDir();
	LPTSTR szFindData = strApkDirectory.GetBuffer(MAX_PATH);
	::PathAppend(szFindData, _T("*.apk"));
	strApkDirectory.ReleaseBuffer();

	arrApkPath.RemoveAll();
	BOOL bRet = ShellHelper::GetFilesInDirectory(strApkDirectory, m_arrApkPath);
	if (bRet)
	{
		for (int i = 0; i < arrApkPath.GetSize(); i++)
		{
			CString& file = arrApkPath[i];
			m_lvApkDir.AddItem(i, 0, file, 0);
		}
	}
	return bRet;
}

BOOL MainTab::GetListItemApkPath(int nIndex, CString& strPath)
{
	if (nIndex >= 0 && nIndex < m_lvApkDir.GetItemCount())
	{
		CString strApkName;
		m_lvApkDir.GetItemText(nIndex, 0, strApkName);

		strPath = ConfigManager::GetInstance().GetApkDir();
		LPTSTR szApkPath = strPath.GetBuffer(MAX_PATH);
		::PathAppend(szApkPath, strApkName);
		strPath.ReleaseBuffer();

		return TRUE;
	}
	return FALSE;
}
