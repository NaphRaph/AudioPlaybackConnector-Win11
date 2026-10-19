#pragma once

constexpr auto CONFIG_NAME = L"AudioPlaybackConnector.json";
constexpr auto BUFFER_SIZE = 4096;

fs::path GetSettingsPath()
{
	return GetAppDataDirectory() / CONFIG_NAME;
}

fs::path GetLegacySettingsPath()
{
	return GetModuleFsPath(g_hInst).remove_filename() / CONFIG_NAME;
}

void MigrateLegacySettings()
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

void DefaultSettings()
{
	g_reconnect = false;
	g_playbackVolume = DEFAULT_PLAYBACK_VOLUME;
	g_captureVolumeReferences.clear();
	g_lastDevices.clear();
}

void LoadSettings()
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
		// Versions 1 and 2 controlled the wrong audio path. Discard their slider
		// positions and start at the phone capture endpoint's neutral 50%.
		const auto volumeControlVersion = jsonObj.HasKey(L"volumeControlVersion") ?
			static_cast<int>(jsonObj.Lookup(L"volumeControlVersion").GetNumber()) : 1;
		if (volumeControlVersion >= VOLUME_CONTROL_VERSION && jsonObj.HasKey(L"playbackVolume"))
			g_playbackVolume = std::clamp(jsonObj.Lookup(L"playbackVolume").GetNumber(), 0.0, 1.0);
		if (volumeControlVersion >= VOLUME_CONTROL_VERSION &&
			jsonObj.HasKey(L"captureVolumeReferences"))
		{
			for (const auto& item : jsonObj.Lookup(L"captureVolumeReferences").GetArray())
			{
				if (item.ValueType() != JsonValueType::Object)
					continue;
				auto reference = item.GetObject();
				if (reference.HasKey(L"endpointId") && reference.HasKey(L"referenceVolume"))
					g_captureVolumeReferences.emplace(
						std::wstring(reference.Lookup(L"endpointId").GetString()),
						static_cast<float>(std::clamp(reference.Lookup(L"referenceVolume").GetNumber(), 0.0, 1.0)));
			}
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

void SaveSettings()
{
	try
	{
		JsonObject jsonObj;
		jsonObj.Insert(L"reconnect", JsonValue::CreateBooleanValue(g_reconnect));
		jsonObj.Insert(L"playbackVolume", JsonValue::CreateNumberValue(g_playbackVolume.load()));
		jsonObj.Insert(L"volumeControlVersion", JsonValue::CreateNumberValue(VOLUME_CONTROL_VERSION));
		{
			std::lock_guard lock(g_volumeMutex);
			JsonArray references;
			for (const auto& [endpointId, referenceVolume] : g_captureVolumeReferences)
			{
				JsonObject reference;
				reference.Insert(L"endpointId", JsonValue::CreateStringValue(endpointId));
				reference.Insert(L"referenceVolume", JsonValue::CreateNumberValue(referenceVolume));
				references.Append(reference);
			}
			jsonObj.Insert(L"captureVolumeReferences", references);
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
