#pragma once

constexpr uintmax_t MAX_LOG_SIZE = 1024 * 1024;

fs::path GetLogDirectory()
{
	auto path = GetAppDataDirectory() / L"Logs";
	fs::create_directories(path);
	return path;
}

fs::path GetLogPath()
{
	return GetLogDirectory() / L"AudioPlaybackConnector.log";
}

void AppendLog(std::wstring_view message)
{
	try
	{
		std::lock_guard lock(g_logMutex);
		auto path = GetLogPath();
		std::error_code error;
		if (fs::exists(path, error) && fs::file_size(path, error) >= MAX_LOG_SIZE)
		{
			auto oldPath = path;
			oldPath += L".old";
			MoveFileExW(path.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
		}

		SYSTEMTIME time{};
		GetLocalTime(&time);
		wchar_t prefix[64]{};
		swprintf_s(prefix, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [T%lu] ",
			time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
			time.wSecond, time.wMilliseconds, GetCurrentThreadId());

		std::wstring line(prefix);
		line.append(message);
		line.append(L"\r\n");
		OutputDebugStringW(line.c_str());

		wil::unique_hfile file(CreateFileW(path.c_str(), FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr));
		if (!file)
			return;

		auto utf8 = Utf16ToUtf8(line);
		DWORD written = 0;
		WriteFile(file.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
	}
	catch (...)
	{
		// Logging must never interfere with Bluetooth audio or shutdown.
	}
}

void AppendHresultLog(std::wstring_view action, HRESULT result)
{
	wchar_t code[16]{};
	swprintf_s(code, L"0x%08X", static_cast<uint32_t>(result));
	std::wstring message(action);
	message.append(L": ");
	message.append(code);
	AppendLog(message);
}
