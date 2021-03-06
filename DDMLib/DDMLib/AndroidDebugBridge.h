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

#pragma once

#include "CommonDefine.h"
#include "DeviceMonnitor.h"
#include "AdbVersion.h"
#include "IDevice.h"
#include "../System/SocketAddress.h"
#include "../System/Process.h"

// define class
class DeviceMonitor;

class AndroidDebugBridge
{
public:
	interface IDebugBridgeChangeListener
	{
		virtual void BridgeChanged(AndroidDebugBridge* pBridge) = 0;
	};
	interface IDeviceChangeListener
	{
		virtual void DeviceConnected(const IDevice* device) = 0;
		virtual void DeviceDisconnected(const IDevice* device) = 0;
		virtual void DeviceChanged(const IDevice* device, int changeMask) = 0;
	};
private:
	static std::recursive_mutex s_lockClass;
	static std::mutex s_lockMember;
	static AdbVersion* s_pCurVersion;
	static AndroidDebugBridge* s_pThis;
	static bool s_bInitialized;
	static bool s_bClientSupport;

	static int s_nAdbServerPort;
	static SocketAddress s_addSocket;

	std::tstring m_strAdbLocation;

	bool m_bVersionCheck;
	bool m_bStarted;
	DeviceMonitor* m_pDeviceMonitor;

	static std::set<IDebugBridgeChangeListener*> s_setBridgeListeners;
	static std::set<IDeviceChangeListener*> s_setDeviceListeners;

private:
	AndroidDebugBridge(const TString szLocation);
	AndroidDebugBridge();
	void CheckAdbVersion();

	static AdbVersion* GetAdbVersion(const TString adb);
public:
	~AndroidDebugBridge();

public:
	static AndroidDebugBridge& CreateBridge(const TString szLocation, bool forceNewBridge = false);
	static AndroidDebugBridge& CreateBridge();
	static void DisconnectBridge();
	static AndroidDebugBridge& GetBridge();
	static void AddDebugBridgeChangeListener(IDebugBridgeChangeListener* listener);
	static void RemoveDebugBridgeChangeListener(IDebugBridgeChangeListener* listener);
	static void AddDeviceChangeListener(IDeviceChangeListener* listener);
	static void RemoveDeviceChangeListener(IDeviceChangeListener* listener);
	static bool GetClientSupport();
	static const SocketAddress& GetSocketAddress();

	static bool InitIfNeeded(bool clientSupport);
	static bool Init(bool clientSupport);
	static void InitAdbSocketAddr();
	static void Terminate();
	static int GetAdbServerPort();

	const IDevice* GetDevices() const;

	bool Start();
	bool Stop();
	bool Restart();

	void DeviceConnected(const IDevice* device);
	void DeviceDisconnected(const IDevice* device);
	void DeviceChanged(const IDevice* device, int changeMask);

public:
	bool StartAdb();
	void GetAdbLaunchCommand(const TString option, std::vector<std::tstring>& vecCommand);
	int GrabProcessOutput(Process& process, std::vector<std::tstring>* pOutput);
	bool StopAdb();
};