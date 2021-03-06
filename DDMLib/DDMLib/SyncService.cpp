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

#include "SyncService.h"
#include "../System/StreamReader.h"
#include "../System/StreamWriter.h"
#include "DdmPreferences.h"
#include "AdbHelper.h"
#include "ArrayHelper.h"

#define SYNC						_T("sync")

#define SYNC_DATA_MAX				64*1024
#define REMOTE_PATH_MAX_LENGTH	1024
#define SYNC_REQ_LENGTH			8

#define ID_OKAY "OKAY"
#define ID_FAIL "FAIL"
#define ID_STAT "STAT"
#define ID_RECV "RECV"
#define ID_DATA "DATA"
#define ID_DONE "DONE"
#define ID_SEND "SEND"

SyncService::NullSyncProgressMonitor* const SyncService::s_pNullSyncProgressMonitor = new NullSyncProgressMonitor();

SyncService::SyncService(const SocketAddress& address, Device* device)
{
	m_socketAddress = address;
	m_pDevice = device;
	m_pClient = NULL;
	m_pBuffer = NULL;
}

SyncService::~SyncService()
{
	Close();
	if (m_pBuffer != NULL)
	{
		delete[] m_pBuffer;
		m_pBuffer = NULL;
	}
}

bool SyncService::OpenSync()
{
	m_pClient = SocketClient::Open(m_socketAddress);
	if (m_pClient == NULL)
	{
		return false;
	}
	m_pClient->ConfigureBlocking(false);

	// target a specific device
	bool bRet = AdbHelper::SetDevice(m_pClient, m_pDevice);
	if (!bRet)
	{
		return false;
	}

	std::unique_ptr<const char[]> request(AdbHelper::FormAdbRequest("sync:")); //$NON-NLS-1$
	bRet = AdbHelper::Write(m_pClient, request.get(), -1, DdmPreferences::GetTimeOut());
	if (!bRet)
	{
		return false;
	}

	std::unique_ptr<AdbHelper::AdbResponse> resp(AdbHelper::ReadAdbResponse(m_pClient, false /* readDiagString */));

	if (!resp || !resp->okay)
	{
		LogWEx(SYNC, _T("Got unhappy response from ADB sync req: %s"), resp->message);
		m_pClient->Close();
		delete m_pClient;
		m_pClient = NULL;
		return false;
	}

	return true;
}

void SyncService::Close()
{
	if (m_pClient != NULL)
	{
		m_pClient->Close();
		delete m_pClient;
		m_pClient = NULL;
	}
}

SyncService::ISyncProgressMonitor* SyncService::GetNullProgressMonitor()
{
	return s_pNullSyncProgressMonitor;
}

bool SyncService::PushFile(const TString local, const TString remote, ISyncProgressMonitor* monitor)
{
	File file(local);
	if (!file.Exists())
	{
		return false;
	}

	if (file.IsDirectory())
	{
		return false;
	}

	monitor->Start(static_cast<int>(file.GetLength()));

	bool bRet = DoPushFile(file, remote, monitor);

	monitor->Stop();

	return bRet;
}

bool SyncService::PullFile(const TString remote, const TString local, ISyncProgressMonitor* monitor)
{
	FileStat* fileStat = NULL;
	bool bRet = StatFile(remote, &fileStat);
	if (!bRet)
	{
		return false;
	}
	if (fileStat->GetMode() == 0)
	{
		delete fileStat;
		return false;
	}
	delete fileStat;

	monitor->Start(0);
	//TODO: use the {@link FileListingService} to get the file size.

	bRet = DoPullFile(remote, local, monitor);

	monitor->Stop();

	return bRet;
}

bool SyncService::StatFile(const TString path, FileStat** fileStat)
{
	if (fileStat == NULL)
	{
		return false;
	}
	// create the stat request message.
	int len = 0;
	char* msg = CreateFileReq(ID_STAT, path, len);

	bool bRet = AdbHelper::Write(m_pClient, msg, len, DdmPreferences::GetTimeOut());
	delete[] msg;
	if (!bRet)
	{
		return false;
	}

	// read the result, in a byte array containing 4 ints
	// (id, mode, size, time)
	const int statLen = 16;
	char* statResult = new char[statLen];
	bRet = AdbHelper::Read(m_pClient, statResult, statLen, DdmPreferences::GetTimeOut());

	// check we have the proper data back
	if (!bRet || !CheckResult(statResult, ID_STAT))
	{
		return false;
	}

	const int mode = ArrayHelper::Swap32bitFromArray(statResult, 4);
	const int size = ArrayHelper::Swap32bitFromArray(statResult, 8);
	const int lastModifiedSecs = ArrayHelper::Swap32bitFromArray(statResult, 12);
	*fileStat = new FileStat(mode, size, lastModifiedSecs);
	return true;
}

bool SyncService::DoPushFile(const File& file, const TString remotePath, ISyncProgressMonitor* monitor)
{
	const int timeOut = DdmPreferences::GetTimeOut();

	int pathLen = _tcslen(remotePath);
	if (pathLen > REMOTE_PATH_MAX_LENGTH)
	{
		return false;
	}

	FileReadWrite fRead = file.GetRead();

	const int nBuffSize = 1024;
	// create the stream to read the file
	CharStreamReader fsr(fRead, SYNC_DATA_MAX);

	int len = 0;
	// create the header for the action
	char* msg = CreateSendFileReq(ID_SEND, remotePath, 0644, len);

	// and send it. We use a custom try/catch block to make the difference between
	// file and network IO exceptions.
	bool bRet = AdbHelper::Write(m_pClient, msg, len, timeOut);
	delete[] msg;
	if (!bRet)
	{
		return false;
	}

	strncpy(GetBuffer(), ID_DATA, _countof(ID_DATA));

	bool bError = false;
	// look while there is something to read
	while (true)
	{
		// check if we're canceled
		if (monitor->IsCanceled())
		{
			bError = true;
			break;
		}

		// read up to SYNC_DATA_MAX
		int readCount = fsr.ReadData(GetBuffer() + SYNC_REQ_LENGTH, SYNC_DATA_MAX);
		if (readCount == 0)
		{
			// we reached the end of the file
			break;
		}
		else if (readCount == -1)
		{
			// read error
			bError = true;
			break;
		}

		// now send the data to the device
		// first write the amount read
		ArrayHelper::Swap32bitsToArray(readCount, GetBuffer(), 4);

		// now write it
		bRet = AdbHelper::Write(m_pClient, GetBuffer(), readCount + SYNC_REQ_LENGTH, timeOut);
		if (!bRet)
		{
			// write error
			bError = true;
			break;
		}

		// and advance the monitor
		monitor->Advance(readCount);
	}

	// close the local file
	fRead.Close();
	fRead.Delete();

	if (bError)
	{
		return false;
	}

	// create the DONE message
	int time = static_cast<int>(file.GetLastModifiedTime() / 1000);
	len = 0;
	msg = CreateReq(ID_DONE, time, len);

	// and send it.
	bRet = AdbHelper::Write(m_pClient, msg, len, timeOut);
	delete[] msg;
	if (!bRet)
	{
		return false;
	}
	// read the result, in a byte array containing 2 ints
	// (id, size)
	char result[SYNC_REQ_LENGTH] = { 0 };
	bRet = AdbHelper::Read(m_pClient, result, SYNC_REQ_LENGTH, timeOut);

	if (!bRet || !CheckResult(result, ID_OKAY))
	{
		return false;
	}
 	return true;
}

bool SyncService::DoPullFile(const TString remotePath, const TString localPath, ISyncProgressMonitor* monitor)
{
	const int timeOut = DdmPreferences::GetTimeOut();

	int pathLen = _tcslen(remotePath);
	if (pathLen > REMOTE_PATH_MAX_LENGTH)
	{
		return false;
	}

	// create the full request message
	int len = 0;
	char* msg = CreateFileReq(ID_RECV, remotePath, len);

	// and send it.
	bool bRet = AdbHelper::Write(m_pClient, msg, len, timeOut);
	delete[] msg;
	if (!bRet)
	{
		return false;
	}

	// read the result, in a byte array containing 2 ints
	// (id, size)
	char pullResult[SYNC_REQ_LENGTH] = { 0 };
	bRet = AdbHelper::Read(m_pClient, pullResult, SYNC_REQ_LENGTH, timeOut);
	if (!bRet)
	{
		return false;
	}

	// check we have the proper data back
	if (!CheckResult(pullResult, ID_DATA) &&
		!CheckResult(pullResult, ID_DONE))
	{
		return false;
	}

	// access the destination file
	File f(localPath);

	// create the stream to write in the file. We use a new try/catch block to differentiate
	// between file and network io exceptions.
	FileReadWrite fWrite = f.GetWrite();
	CharStreamWriter fsw(fWrite, 0);

	// the buffer to read the data
	char data[SYNC_DATA_MAX] = { 0 };

	bool bError = false;
	// loop to get data until we're done.
	while (true)
	{
		// check if we're cancelled
		if (monitor->IsCanceled())
		{
			bError = true;
			break;
		}

		// if we're done, we stop the loop
		if (CheckResult(pullResult, ID_DONE))
		{
			break;
		}
		if (!CheckResult(pullResult, ID_DATA))
		{
			// hmm there's an error
			bError = true;
			break;
		}
		int length = ArrayHelper::Swap32bitFromArray(pullResult, 4);
		if (length > SYNC_DATA_MAX)
		{
			// buffer overrun!
			// error and exit
			bError = true;
			break;
		}

		// now read the length we received
		bRet = AdbHelper::Read(m_pClient, data, length, timeOut);
		if (!bRet)
		{
			bError = true;
			break;
		}

		// get the header for the next packet.
		bRet = AdbHelper::Read(m_pClient, pullResult, SYNC_REQ_LENGTH, timeOut);
		if (!bRet)
		{
			bError = true;
			break;
		}

		// write the content in the file
		long lWrite = fsw.WriteData(data, length);
		if (lWrite < 0)
		{
			bError = true;
			break;
		}

		monitor->Advance(length);
	}

	long lWrite = fsw.Flush();
	if (lWrite < 0)
	{
		bError = true;
	}

	// close the local file
	fWrite.Close();
	fWrite.Delete();

	if (bError)
	{
		return false;
	}
	return true;
}

char* SyncService::CreateReq(const char* command, int value, int& len)
{
	char* array = new char[SYNC_REQ_LENGTH];

	strncpy(array, command, 4);
	ArrayHelper::Swap32bitsToArray(value, array, 4);
	len = SYNC_REQ_LENGTH;

	return array;
}

char* SyncService::CreateFileReq(const char* command, const TString path, int& len)
{
	const char* filePath;
#ifdef _UNICODE
	std::string strPath;
	ConvertUtils::WstringToString(path, strPath);
	filePath = strPath.c_str();
#else
	filePath = remotePath;
#endif
	const int pathLength = strlen(filePath);
	len = SYNC_REQ_LENGTH + pathLength;
	char* array = new char[len];

	strncpy(array, command, 4);
	ArrayHelper::Swap32bitsToArray(pathLength, array, 4);
	strncpy(array + SYNC_REQ_LENGTH, filePath, pathLength);

	return array;
}

char* SyncService::CreateSendFileReq(const char* command, const TString path, int mode, int& len)
{
	// make the mode into a string
	std::ostringstream oss;
	oss << "," << (mode & 0777);
	std::string& modeStr = oss.str();
	const char* modeContent = modeStr.c_str();

	const char* remotePathContent;
#ifdef _UNICODE
	std::string strPath;
	ConvertUtils::WstringToString(path, strPath);
	remotePathContent = strPath.c_str();
#else
	remotePathContent = remotePath;
#endif

	const int pathLength = strlen(remotePathContent);
	const int modeLength = strlen(modeContent);
	len = SYNC_REQ_LENGTH + pathLength + modeLength;
	char* array = new char[len];

	strncpy(array, command, 4);
	ArrayHelper::Swap32bitsToArray(pathLength + modeLength, array, 4);
	strncpy(array + SYNC_REQ_LENGTH, remotePathContent, pathLength);
	strncpy(array + SYNC_REQ_LENGTH + pathLength, modeContent, modeLength);

	return array;
}

bool SyncService::CheckResult(char* result, char* code)
{
	return !(result[0] != code[0] ||
		result[1] != code[1] ||
		result[2] != code[2] ||
		result[3] != code[3]);
}

char* SyncService::GetBuffer()
{
	if (m_pBuffer == NULL)
	{
		// create the buffer used to read.
		// we read max SYNC_DATA_MAX, but we need 2 4 bytes at the beginning.
		m_pBuffer = new char[SYNC_DATA_MAX + SYNC_REQ_LENGTH];
	}
	return m_pBuffer;
}

//////////////////////////////////////////////////////////////////////////
// implements for FileStat

SyncService::FileStat::FileStat(int mode, int size, int lastModifiedSecs) :
	m_nMode(mode),
	m_nSize(size),
	m_tLastModified(static_cast<time_t>(lastModifiedSecs) * 1000)
{
}

int SyncService::FileStat::GetMode() const
{
	return m_nMode;
}

int SyncService::FileStat::GetSize() const
{
	return m_nSize;
}

time_t SyncService::FileStat::GetLastModified() const
{
	return m_tLastModified;
}
