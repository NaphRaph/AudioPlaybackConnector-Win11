#pragma once

constexpr auto CONFIG_NAME = L"AudioPlaybackConnector.json";
constexpr auto BUFFER_SIZE = 4096;

inline fs::path GetSettingsPath()
{
	return GetAppDataDirectory() / CONFIG_NAME;
}

inline fs::path GetLegacySettingsPath()
{
	return GetModuleFsPath(g_hInst).remove_filename() / CONFIG_NAME;
}

inline void MigrateLegacySettings()
{
	try
	{
		auto newPath = GetSettingsPath();
		auto oldPath = GetLegacySettingsPath();
		if (newPath == oldPath || fs::exists(newPath) || !fs::exists(oldPath))
			return;

		if (MoveFileExW(oldPath.c_str(), newPath.c_str(),
			MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			AppendLog(L"Migrated settings from the application folder");
		else
			AppendHresultLog(L"Settings migration failed", HRESULT_FROM_WIN32(GetLastError()));
	}
	catch (...)
	{
		AppendLog(L"Settings migration failed with an exception");
	}
}

inline void DefaultSettings()
{
	g_reconnect = false;
	g_playbackVolume = DEFAULT_PLAYBACK_VOLUME;
	g_phoneEndpointGuids.clear();
	g_lastDevices.clear();
}

inline void LoadSettings()
{
	try
	{
		DefaultSettings();
		MigrateLegacySettings();

		wil::unique_hfile hFile(CreateFileW(GetSettingsPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_LAST_ERROR_IF(!hFile);

		std::string string;
		while (1)
		{
			size_t size = string.size();
			string.resize(size + BUFFER_SIZE);
			DWORD read = 0;
			THROW_IF_WIN32_BOOL_FALSE(ReadFile(hFile.get(), string.data() + size, BUFFER_SIZE, &read, nullptr));
			string.resize(size + read);
			if (read == 0)
				break;
		}

		std::wstring utf16 = Utf8ToUtf16(string);
		auto jsonObj = JsonObject::Parse(utf16);
		g_reconnect = jsonObj.Lookup(L"reconnect").GetBoolean();
		// Version 6 stores the Windows playback session volume. Older versions
		// used a digital gain whose 50% was the neutral point, so their value is
		// not carried over: the app adopts the session's current volume instead,
		// which keeps the loudness unchanged after an upgrade.
		const auto volumeControlVersion = jsonObj.HasKey(L"volumeControlVersion") ?
			static_cast<int>(jsonObj.Lookup(L"volumeControlVersion").GetNumber()) : 1;
		if (volumeControlVersion >= 6 && jsonObj.HasKey(L"playbackVolume"))
		{
			g_playbackVolume = std::clamp(jsonObj.Lookup(L"playbackVolume").GetNumber(), 0.0, 1.0);
			g_playbackVolumeExplicit = true;
		}

		auto lastDevices = jsonObj.Lookup(L"lastDevices").GetArray();
		g_lastDevices.reserve(lastDevices.Size());
		for (const auto& i : lastDevices)
		{
			if (i.ValueType() == JsonValueType::String)
				g_lastDevices.push_back(std::wstring(i.GetString()));
		}
	}
	CATCH_LOG();
}

inline void SaveSettings()
{
	try
	{
		JsonObject jsonObj;
		jsonObj.Insert(L"reconnect", JsonValue::CreateBooleanValue(g_reconnect));
		// Only persist the slider once its value is meaningful. Writing a
		// placeholder would make the next launch force that level on connect.
		if (g_playbackVolumeExplicit)
		{
			jsonObj.Insert(L"playbackVolume", JsonValue::CreateNumberValue(g_playbackVolume.load()));
			jsonObj.Insert(L"volumeControlVersion", JsonValue::CreateNumberValue(VOLUME_CONTROL_VERSION));
		}
		else
		{
			jsonObj.Insert(L"volumeControlVersion", JsonValue::CreateNumberValue(5));
		}

		JsonArray lastDevices;
		{
			std::lock_guard lock(g_connectionMutex);
			for (const auto& i : g_audioPlaybackConnections)
				lastDevices.Append(JsonValue::CreateStringValue(i.first));
		}
		jsonObj.Insert(L"lastDevices", lastDevices);

		wil::unique_hfile hFile(CreateFileW(GetSettingsPath().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_LAST_ERROR_IF(!hFile);

		std::string utf8 = Utf16ToUtf8(jsonObj.Stringify());
		DWORD written = 0;
		THROW_IF_WIN32_BOOL_FALSE(WriteFile(hFile.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr));
		THROW_HR_IF(E_FAIL, written != utf8.size());
	}
	CATCH_LOG();
}
