#pragma once

// Bluetooth source audio arrives through its own capture endpoint. Never touch
// the default render endpoint, which also carries every other PC application.
std::wstring BluetoothParentId(std::wstring_view deviceId)
{
	const auto begin = deviceId.find(L"BTHENUM#");
	const auto end = deviceId.rfind(L"#{");
	if (begin == std::wstring_view::npos || end == std::wstring_view::npos || end <= begin)
		return {};
	std::wstring parent(deviceId.substr(begin, end - begin));
	std::replace(parent.begin(), parent.end(), L'#', L'\\');
	return parent;
}

bool GetEndpointParentId(std::wstring_view endpointId, std::wstring& parentId)
{
	std::wstring instanceId = L"SWD\\MMDEVAPI\\";
	instanceId.append(endpointId);
	DEVINST endpoint = 0;
	if (CM_Locate_DevNodeW(&endpoint, instanceId.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
		return false;
	DEVINST parent = 0;
	if (CM_Get_Parent(&parent, endpoint, 0) != CR_SUCCESS)
		return false;
	ULONG size = 0;
	if (CM_Get_Device_ID_Size(&size, parent, 0) != CR_SUCCESS)
		return false;
	std::wstring id(size + 1, L'\0');
	if (CM_Get_Device_IDW(parent, id.data(), size + 1, 0) != CR_SUCCESS)
		return false;
	id.resize(size);
	parentId = std::move(id);
	return true;
}

bool FindPhoneCaptureEndpoint(std::wstring_view deviceId,
	winrt::com_ptr<IMMDevice>& selected, std::wstring& selectedId)
{
	const auto expectedParent = BluetoothParentId(deviceId);
	if (expectedParent.empty())
		return false;

	winrt::com_ptr<IMMDeviceEnumerator> enumerator;
	auto result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator), enumerator.put_void());
	if (FAILED(result))
	{
		AppendHresultLog(L"Create audio device enumerator failed", result);
		return false;
	}

	winrt::com_ptr<IMMDeviceCollection> endpoints;
	result = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, endpoints.put());
	if (FAILED(result))
	{
		AppendHresultLog(L"Enumerate active capture endpoints failed", result);
		return false;
	}
	UINT count = 0;
	if (FAILED(endpoints->GetCount(&count)))
		return false;
	for (UINT i = 0; i < count; ++i)
	{
		winrt::com_ptr<IMMDevice> endpoint;
		if (FAILED(endpoints->Item(i, endpoint.put())))
			continue;
		LPWSTR rawId = nullptr;
		if (FAILED(endpoint->GetId(&rawId)))
			continue;
		std::wstring endpointId(rawId);
		CoTaskMemFree(rawId);
		std::wstring parentId;
		if (!GetEndpointParentId(endpointId, parentId))
			continue;
		if (CompareStringOrdinal(parentId.c_str(), -1, expectedParent.c_str(), -1, TRUE) == CSTR_EQUAL)
		{
			selected = std::move(endpoint);
			selectedId = std::move(endpointId);
			return true;
		}
	}
	return false;
}

float MapPlaybackVolumeToEndpoint(float controlVolume, float referenceVolume)
{
	controlVolume = std::clamp(controlVolume, 0.0f, 1.0f);
	referenceVolume = std::clamp(referenceVolume, 0.0f, 1.0f);
	constexpr float neutral = static_cast<float>(DEFAULT_PLAYBACK_VOLUME);
	if (controlVolume <= neutral)
		return referenceVolume * (controlVolume / neutral);
	return referenceVolume + (1.0f - referenceVolume) *
		((controlVolume - neutral) / (1.0f - neutral));
}

bool SetPhoneCaptureVolume(std::wstring_view deviceId, float controlVolume, bool allowUnmute)
{
	winrt::com_ptr<IMMDevice> endpoint;
	std::wstring endpointId;
	if (!FindPhoneCaptureEndpoint(deviceId, endpoint, endpointId))
		return false;

	winrt::com_ptr<IAudioEndpointVolume> volume;
	auto result = endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
		nullptr, volume.put_void());
	if (FAILED(result))
	{
		AppendHresultLog(L"Activate phone capture volume failed", result);
		return false;
	}
	float currentVolume = 1.0f;
	result = volume->GetMasterVolumeLevelScalar(&currentVolume);
	if (FAILED(result))
	{
		AppendHresultLog(L"Read phone capture volume failed", result);
		return false;
	}

	float referenceVolume;
	{
		std::lock_guard lock(g_volumeMutex);
		auto [it, inserted] = g_captureVolumeReferences.try_emplace(endpointId,
			std::clamp(currentVolume, 0.0f, 1.0f));
		referenceVolume = it->second;
		if (inserted)
			AppendLog(L"Phone capture neutral reference: " + endpointId + L" at " +
				std::to_wstring(static_cast<int>(std::lround(referenceVolume * 100.0f))) + L"%");
	}
	if (allowUnmute && controlVolume > 0.0f)
	{
		result = volume->SetMute(FALSE, nullptr);
		if (FAILED(result))
			AppendHresultLog(L"Unmute phone capture endpoint failed", result);
	}
	const auto targetVolume = MapPlaybackVolumeToEndpoint(controlVolume, referenceVolume);
	result = volume->SetMasterVolumeLevelScalar(targetVolume, nullptr);
	if (FAILED(result))
	{
		AppendHresultLog(L"Set phone capture volume failed", result);
		return false;
	}
	return true;
}

winrt::fire_and_forget ApplyPlaybackVolume(bool logResult = false, bool allowUnmute = false)
{
	co_await winrt::resume_background();
	// Slider notifications may overlap. Read the newest value only after the
	// previous endpoint write finishes so an older task cannot win the race.
	std::lock_guard applyLock(g_volumeApplyMutex);
	std::vector<std::wstring> deviceIds;
	{
		std::lock_guard lock(g_connectionMutex);
		for (const auto& [deviceId, entry] : g_audioPlaybackConnections)
		{
			if (!entry.Connecting)
				deviceIds.push_back(deviceId);
		}
	}
	const auto controlVolume = static_cast<float>(g_playbackVolume.load());
	for (const auto& deviceId : deviceIds)
	{
		if (g_shuttingDown)
			co_return;
		if (SetPhoneCaptureVolume(deviceId, controlVolume, allowUnmute))
		{
			if (logResult)
				AppendLog(L"Applied volume to phone A2DP capture endpoint: " + deviceId +
					L" at " + std::to_wstring(static_cast<int>(std::lround(controlVolume * 100.0f))) + L"%");
		}
		else if (logResult)
			AppendLog(L"No matching active A2DP capture endpoint for: " + deviceId);
	}
}
