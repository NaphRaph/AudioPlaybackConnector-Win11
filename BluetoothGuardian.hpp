#pragma once

// Silent guardian for Windows Bluetooth A2DP/AVRCP registry and USB power
// settings. Protects against Windows Update or driver updates resetting:
// 1. AVRCP DisableAbsoluteVolume = 1 (keeps phone & PC volumes independent)
// 2. A2DP Sink Enabled = 1
// 3. Fast Startup HiberbootEnabled = 0 (prevents hybrid-boot radio lockups)
// 4. USB Selective Suspend / EnhancedPowerManagementEnabled = 0 on BT adapters

constexpr auto GUARDIAN_TASK_NAME = L"AudioPlaybackConnector-BluetoothGuardian";
constexpr auto REG_AVRCP_CT = LR"(SYSTEM\CurrentControlSet\Control\Bluetooth\Audio\AVRCP\CT)";
constexpr auto REG_AVRCP_TG = LR"(SYSTEM\CurrentControlSet\Control\Bluetooth\Audio\AVRCP\TG)";
constexpr auto REG_A2DP_SINK = LR"(SYSTEM\CurrentControlSet\Control\Bluetooth\Audio\A2dp\Sink)";
constexpr auto REG_POWER = LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Power)";
constexpr auto REG_BT_CLASS = LR"(SYSTEM\CurrentControlSet\Control\Class\{e0cbf06c-cd8b-4647-bb8a-263b43f0f974})";
constexpr auto BT_CLASS_GUID_STR = L"{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}";

inline bool ReadRegDwordValue(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD& outValue) noexcept
{
	DWORD data = 0;
	DWORD cbData = sizeof(data);
	if (RegGetValueW(root, subKey, valueName, RRF_RT_REG_DWORD, nullptr, &data, &cbData) != ERROR_SUCCESS)
		return false;
	outValue = data;
	return true;
}

inline bool WriteRegDwordValue(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD value) noexcept
{
	HKEY hKey = nullptr;
	if (RegCreateKeyExW(root, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
		KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
		return false;
	const LSTATUS status = RegSetValueExW(hKey, valueName, 0, REG_DWORD,
		reinterpret_cast<const BYTE*>(&value), sizeof(value));
	RegCloseKey(hKey);
	return status == ERROR_SUCCESS;
}

inline std::vector<std::wstring> FindBluetoothUsbInstanceIds()
{
	std::vector<std::wstring> usbIds;
	auto addUnique = [&usbIds](std::wstring_view candidate) {
		constexpr std::wstring_view usbPrefix = L"USB\\";
		if (candidate.size() <= usbPrefix.size() ||
			CompareStringOrdinal(candidate.data(), static_cast<int>(usbPrefix.size()),
				usbPrefix.data(), static_cast<int>(usbPrefix.size()), TRUE) != CSTR_EQUAL)
			return;
		for (const auto& existing : usbIds)
		{
			if (CompareStringOrdinal(existing.data(), static_cast<int>(existing.size()),
				candidate.data(), static_cast<int>(candidate.size()), TRUE) == CSTR_EQUAL)
				return;
		}
		usbIds.emplace_back(candidate);
	};

	ULONG size = 0;
	constexpr ULONG flags = CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT;
	if (CM_Get_Device_ID_List_SizeW(&size, BT_CLASS_GUID_STR, flags) == CR_SUCCESS && size >= 2)
	{
		std::wstring buffer(size, L'\0');
		if (CM_Get_Device_ID_ListW(BT_CLASS_GUID_STR, buffer.data(), size, flags) == CR_SUCCESS)
		{
			for (wchar_t* p = buffer.data(); *p; p += wcslen(p) + 1)
			{
				addUnique(p);
				DEVINST devInst = 0;
				if (CM_Locate_DevNodeW(&devInst, p, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
				{
					DEVINST current = devInst;
					for (int depth = 0; depth < 4; ++depth)
					{
						DEVINST parent = 0;
						if (CM_Get_Parent(&parent, current, 0) != CR_SUCCESS)
							break;
						ULONG idLen = 0;
						if (CM_Get_Device_ID_Size(&idLen, parent, 0) == CR_SUCCESS && idLen > 0)
						{
							std::wstring parentId(idLen + 1, L'\0');
							if (CM_Get_Device_IDW(parent, parentId.data(), idLen + 1, 0) == CR_SUCCESS)
							{
								parentId.resize(idLen);
								addUnique(parentId);
							}
						}
						current = parent;
					}
				}
			}
		}
	}
	return usbIds;
}

inline bool IsBluetoothSystemGuardHealthy()
{
	DWORD disableAbsVol = 0;
	if (!ReadRegDwordValue(HKEY_LOCAL_MACHINE, REG_AVRCP_CT, L"DisableAbsoluteVolume", disableAbsVol) ||
		disableAbsVol != 1)
		return false;

	DWORD sinkEnabled = 0;
	if (!ReadRegDwordValue(HKEY_LOCAL_MACHINE, REG_A2DP_SINK, L"Enabled", sinkEnabled) ||
		sinkEnabled != 1)
		return false;

	DWORD hiberboot = 1;
	if (ReadRegDwordValue(HKEY_LOCAL_MACHINE, REG_POWER, L"HiberbootEnabled", hiberboot) &&
		hiberboot != 0)
		return false;

	for (const auto& usbId : FindBluetoothUsbInstanceIds())
	{
		const std::wstring paramKey = L"SYSTEM\\CurrentControlSet\\Enum\\" + usbId + L"\\Device Parameters";
		DWORD epm = 1;
		DWORD ss = 1;
		if (!ReadRegDwordValue(HKEY_LOCAL_MACHINE, paramKey.c_str(), L"EnhancedPowerManagementEnabled", epm) || epm != 0)
			return false;
		if (!ReadRegDwordValue(HKEY_LOCAL_MACHINE, paramKey.c_str(), L"SelectiveSuspendEnabled", ss) || ss != 0)
			return false;
	}
	return true;
}

inline bool RunHiddenCommand(std::wstring commandLine, DWORD timeoutMs, DWORD* exitCodeOut = nullptr)
{
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		return false;

	WaitForSingleObject(pi.hProcess, timeoutMs);
	DWORD exitCode = 1;
	GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	if (exitCodeOut)
		*exitCodeOut = exitCode;
	return exitCode == 0;
}

inline bool ApplyBluetoothSystemGuard(bool installScheduledTask)
{
	DWORD prevAbsVol = 0;
	const bool hadAbsVolOne = ReadRegDwordValue(HKEY_LOCAL_MACHINE, REG_AVRCP_CT, L"DisableAbsoluteVolume", prevAbsVol) &&
		prevAbsVol == 1;

	bool ok = true;
	ok &= WriteRegDwordValue(HKEY_LOCAL_MACHINE, REG_AVRCP_CT, L"DisableAbsoluteVolume", 1);
	WriteRegDwordValue(HKEY_LOCAL_MACHINE, REG_AVRCP_TG, L"DisableAbsoluteVolume", 1);
	ok &= WriteRegDwordValue(HKEY_LOCAL_MACHINE, REG_A2DP_SINK, L"Enabled", 1);
	WriteRegDwordValue(HKEY_LOCAL_MACHINE, REG_POWER, L"HiberbootEnabled", 0);

	for (const auto& usbId : FindBluetoothUsbInstanceIds())
	{
		const std::wstring paramKey = L"SYSTEM\\CurrentControlSet\\Enum\\" + usbId + L"\\Device Parameters";
		WriteRegDwordValue(HKEY_LOCAL_MACHINE, paramKey.c_str(), L"EnhancedPowerManagementEnabled", 0);
		WriteRegDwordValue(HKEY_LOCAL_MACHINE, paramKey.c_str(), L"SelectiveSuspendEnabled", 0);
		WriteRegDwordValue(HKEY_LOCAL_MACHINE, paramKey.c_str(), L"AllowIdleIrpInD3", 0);
	}

	for (int i = 0; i < 16; ++i)
	{
		wchar_t subKey[256]{};
		swprintf_s(subKey, L"%s\\%04d", REG_BT_CLASS, i);
		HKEY hKey = nullptr;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, KEY_READ | KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
		{
			const DWORD zero = 0;
			RegSetValueExW(hKey, L"EnhancedPowerManagementEnabled", 0, REG_DWORD,
				reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
			RegSetValueExW(hKey, L"SelectiveSuspendEnabled", 0, REG_DWORD,
				reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
			RegCloseKey(hKey);
		}
	}

	if (installScheduledTask)
	{
		const auto exePath = GetModuleFsPath(g_hInst).wstring();
		std::wstring createCmd = L"schtasks.exe /Create /TN \"";
		createCmd += GUARDIAN_TASK_NAME;
		createCmd += L"\" /TR \"\\\"";
		createCmd += exePath;
		createCmd += L"\\\" --guard\" /SC ONLOGON /RL HIGHEST /RU SYSTEM /F";
		RunHiddenCommand(createCmd, 8000);
	}

	if (!hadAbsVolOne)
	{
		// Restart the user-mode AVCTP/AVRCP service so DisableAbsoluteVolume=1
		// reloads immediately without dropping Bluetooth HID mice/keyboards.
		std::wstring restartCmd =
			L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command "
			L"\"Restart-Service BthAvctpSvc -Force -ErrorAction SilentlyContinue; "
			L"Start-Service bthserv,BTAGService -ErrorAction SilentlyContinue\"";
		RunHiddenCommand(restartCmd, 8000);
	}

	AppendLog(std::wstring(L"Bluetooth system guard applied (ok=") + (ok ? L"true" : L"false") +
		L", task=" + (installScheduledTask ? L"installed" : L"kept") + L")");
	return ok;
}

inline void EnsureBluetoothSystemGuardSync()
{
	if (IsBluetoothSystemGuardHealthy())
	{
		AppendLog(L"Bluetooth system guard verified OK");
		return;
	}

	AppendLog(L"Bluetooth system guard detected unguarded settings; invoking silent guardian");

	// First try direct write in case the process already has sufficient access.
	if (ApplyBluetoothSystemGuard(false) && IsBluetoothSystemGuardHealthy())
		return;

	// Next trigger the pre-registered SYSTEM scheduled task silently (no UAC popup).
	std::wstring runTaskCmd = L"schtasks.exe /Run /TN \"";
	runTaskCmd += GUARDIAN_TASK_NAME;
	runTaskCmd += L"\"";
	if (RunHiddenCommand(runTaskCmd, 5000))
	{
		for (int i = 0; i < 12; ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			if (IsBluetoothSystemGuardHealthy())
			{
				AppendLog(L"Bluetooth system guard restored silently via scheduled task");
				return;
			}
		}
	}

	// One-time bootstrap if the scheduled task is not registered yet.
	const auto exePath = GetModuleFsPath(g_hInst).wstring();
	SHELLEXECUTEINFOW sei{};
	sei.cbSize = sizeof(sei);
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	sei.lpVerb = L"runas";
	sei.lpFile = exePath.c_str();
	sei.lpParameters = L"--install-guard";
	sei.nShow = SW_HIDE;
	if (ShellExecuteExW(&sei) && sei.hProcess)
	{
		WaitForSingleObject(sei.hProcess, 15000);
		CloseHandle(sei.hProcess);
		if (IsBluetoothSystemGuardHealthy())
			AppendLog(L"Bluetooth system guard bootstrap completed and verified");
		else
			AppendLog(L"Bluetooth system guard bootstrap finished but some settings remain unverified");
	}
	else
	{
		AppendLog(L"Bluetooth system guard elevation was declined or unavailable");
	}
}

inline void EnsureBluetoothSystemGuardAsync()
{
	std::thread([] {
		try
		{
			EnsureBluetoothSystemGuardSync();
		}
		catch (...)
		{
			AppendLog(L"Bluetooth system guard check failed with an exception");
		}
	}).detach();
}
