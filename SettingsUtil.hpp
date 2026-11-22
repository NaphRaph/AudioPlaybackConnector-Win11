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
	{
		std::lock_guard lock(g_volumeMutex);
		g_preferredOutputDeviceId.clear();
		g_phoneEndpointGuids.clear();
	}
	g_lastDevices.clear();
	g_rememberedDevices.clear();
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
		if (jsonObj.HasKey(L"reconnect"))
			g_reconnect = jsonObj.Lookup(L"reconnect").GetBoolean();

		// Version 6+ uses 1.0 (100%) as the phone's original loudness, and
		// version 7 extends the range to 0.0-2.0 (0%-200%).
		const auto volumeControlVersion = jsonObj.HasKey(L"volumeControlVersion") ?
			static_cast<int>(jsonObj.Lookup(L"volumeControlVersion").GetNumber()) : 1;
		if (volumeControlVersion >= 6 && jsonObj.HasKey(L"playbackVolume"))
		{
			g_playbackVolume = std::clamp(jsonObj.Lookup(L"playbackVolume").GetNumber(), 0.0, MAX_PLAYBACK_VOLUME);
			g_playbackVolumeExplicit = true;
		}

		if (jsonObj.HasKey(L"outputDeviceId") && jsonObj.Lookup(L"outputDeviceId").ValueType() == JsonValueType::String)
		{
			std::lock_guard lock(g_volumeMutex);
			g_preferredOutputDeviceId = std::wstring(jsonObj.Lookup(L"outputDeviceId").GetString());
		}

		if (jsonObj.HasKey(L"lastDevices"))
		{
			auto lastDevices = jsonObj.Lookup(L"lastDevices").GetArray();
			g_lastDevices.reserve(lastDevices.Size());
			for (const auto& i : lastDevices)
			{
				if (i.ValueType() == JsonValueType::String)
				{
					std::wstring id(i.GetString());
					if (!id.empty())
					{
						g_lastDevices.push_back(id);
						g_rememberedDevices.insert(std::move(id));
					}
				}
			}
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
		if (g_playbackVolumeExplicit)
		{
			jsonObj.Insert(L"playbackVolume",
				JsonValue::CreateNumberValue(std::clamp(g_playbackVolume.load(), 0.0, MAX_PLAYBACK_VOLUME)));
			jsonObj.Insert(L"volumeControlVersion", JsonValue::CreateNumberValue(VOLUME_CONTROL_VERSION));
		}
		else
		{
			jsonObj.Insert(L"volumeControlVersion", JsonValue::CreateNumberValue(5));
		}

		{
			std::lock_guard lock(g_volumeMutex);
			jsonObj.Insert(L"outputDeviceId", JsonValue::CreateStringValue(g_preferredOutputDeviceId));
		}

		JsonArray lastDevices;
		{
			std::lock_guard lock(g_connectionMutex);
			for (const auto& [deviceId, entry] : g_audioPlaybackConnections)
			{
				if (!entry.Connecting)
					g_rememberedDevices.insert(deviceId);
			}
			if (g_reconnect)
			{
				for (const auto& deviceId : g_rememberedDevices)
					lastDevices.Append(JsonValue::CreateStringValue(deviceId));
			}
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
