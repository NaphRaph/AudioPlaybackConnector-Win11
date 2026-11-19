#pragma once

// https://msdn.microsoft.com/en-us/magazine/mt763237
inline std::wstring Utf8ToUtf16(std::string_view utf8)
{
	if (utf8.empty())
	{
		return {};
	}

	constexpr DWORD kFlags = MB_ERR_INVALID_CHARS;
	const int utf8Length = static_cast<int>(utf8.length());
	const int utf16Length = MultiByteToWideChar(
		CP_UTF8,
		kFlags,
		utf8.data(),
		utf8Length,
		nullptr,
		0
	);
	THROW_LAST_ERROR_IF(utf16Length == 0);

	std::wstring utf16(utf16Length, L'\0');
	const int result = MultiByteToWideChar(
		CP_UTF8,
		kFlags,
		utf8.data(),
		utf8Length,
		utf16.data(),
		utf16Length
	);
	THROW_LAST_ERROR_IF(result == 0);

	return utf16;
}

inline std::string Utf16ToUtf8(std::wstring_view utf16)
{
	if (utf16.empty())
	{
		return {};
	}

	constexpr DWORD kFlags = WC_ERR_INVALID_CHARS;
	const int utf16Length = static_cast<int>(utf16.length());
	const int utf8Length = WideCharToMultiByte(
		CP_UTF8,
		kFlags,
		utf16.data(),
		utf16Length,
		nullptr,
		0,
		nullptr, nullptr
	);
	THROW_LAST_ERROR_IF(utf8Length == 0);

	std::string utf8(utf8Length, '\0');
	const int result = WideCharToMultiByte(
		CP_UTF8,
		kFlags,
		utf16.data(),
		utf16Length,
		utf8.data(),
		utf8Length,
		nullptr, nullptr
	);
	THROW_LAST_ERROR_IF(result == 0);

	return utf8;
}

// https://docs.microsoft.com/en-us/windows/uwp/cpp-and-winrt-apis/author-coclasses#add-helper-types-and-functions
// License: see the https://github.com/MicrosoftDocs/windows-uwp/blob/docs/LICENSE-CODE file
inline auto GetModuleFsPath(HMODULE hModule)
{
	std::wstring path(MAX_PATH, L'\0');
	DWORD actualSize;

	while (1)
	{
		actualSize = GetModuleFileNameW(hModule, path.data(), static_cast<DWORD>(path.size()));

		if (static_cast<size_t>(actualSize) + 1 > path.size())
			path.resize(path.size() * 2);
		else
			break;
	}

	path.resize(actualSize);
	return fs::path(path);
}

inline fs::path GetAppDataDirectory()
{
	PWSTR localAppData = nullptr;
	winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData));
	fs::path path(localAppData);
	CoTaskMemFree(localAppData);
	path /= L"AudioPlaybackConnector";
	fs::create_directories(path);
	return path;
}

inline constexpr wchar_t STARTUP_REG_PATH[] = LR"(Software\Microsoft\Windows\CurrentVersion\Run)";
inline constexpr wchar_t STARTUP_APPROVED_REG_PATH[] = LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)";
inline constexpr wchar_t STARTUP_VALUE_NAME[] = L"AudioPlaybackConnector";

inline bool IsStartOnBootEnabled()
{
	DWORD size = 0;
	if (RegGetValueW(HKEY_CURRENT_USER, STARTUP_REG_PATH, STARTUP_VALUE_NAME,
		RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
		size <= sizeof(wchar_t))
	{
		return false;
	}

	BYTE approved[32]{};
	DWORD approvedSize = sizeof(approved);
	if (RegGetValueW(HKEY_CURRENT_USER, STARTUP_APPROVED_REG_PATH, STARTUP_VALUE_NAME,
		RRF_RT_REG_BINARY, nullptr, approved, &approvedSize) == ERROR_SUCCESS &&
		approvedSize >= 1)
	{
		// Task Manager marks disabled startup entries with an odd first byte (e.g. 0x03).
		if ((approved[0] & 0x01) != 0)
			return false;
	}
	return true;
}

inline bool SetStartOnBootEnabled(bool enable)
{
	if (enable)
	{
		const auto exePath = GetModuleFsPath(nullptr).wstring();
		if (exePath.empty())
			return false;
		const std::wstring command = L"\"" + exePath + L"\"";
		const auto status = RegSetKeyValueW(
			HKEY_CURRENT_USER,
			STARTUP_REG_PATH,
			STARTUP_VALUE_NAME,
			REG_SZ,
			command.c_str(),
			static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
		if (status != ERROR_SUCCESS)
			return false;
		RegDeleteKeyValueW(HKEY_CURRENT_USER, STARTUP_APPROVED_REG_PATH, STARTUP_VALUE_NAME);
		return true;
	}

	const auto status = RegDeleteKeyValueW(HKEY_CURRENT_USER, STARTUP_REG_PATH, STARTUP_VALUE_NAME);
	RegDeleteKeyValueW(HKEY_CURRENT_USER, STARTUP_APPROVED_REG_PATH, STARTUP_VALUE_NAME);
	return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}
