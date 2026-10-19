#pragma once

bool GetDefaultRenderEndpoint(winrt::com_ptr<IMMDevice>& device)
{
	winrt::com_ptr<IMMDeviceEnumerator> deviceEnumerator;
	auto result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator), deviceEnumerator.put_void());
	if (FAILED(result))
	{
		AppendHresultLog(L"Create audio device enumerator failed", result);
		return false;
	}

	result = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put());
	if (FAILED(result))
	{
		AppendHresultLog(L"Get default render endpoint failed", result);
		return false;
	}
	return true;
}

bool GetEndpointIdentityAndVolume(IMMDevice* device, std::wstring& endpointId, float& volume)
{
	LPWSTR rawId = nullptr;
	auto result = device->GetId(&rawId);
	if (FAILED(result))
	{
		AppendHresultLog(L"Get default render endpoint ID failed", result);
		return false;
	}
	auto freeRawId = wil::scope_exit([&rawId] { CoTaskMemFree(rawId); });

	winrt::com_ptr<IAudioEndpointVolume> endpointVolume;
	result = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
		endpointVolume.put_void());
	if (FAILED(result))
	{
		AppendHresultLog(L"Activate endpoint volume failed", result);
		return false;
	}

	result = endpointVolume->GetMasterVolumeLevelScalar(&volume);
	if (FAILED(result))
	{
		AppendHresultLog(L"Read endpoint volume failed", result);
		return false;
	}

	endpointId.assign(rawId);
	volume = std::clamp(volume, 0.0f, 1.0f);
	return true;
}

void InitializePlaybackVolumeControl()
{
	winrt::com_ptr<IMMDevice> device;
	if (!GetDefaultRenderEndpoint(device))
		return;

	std::wstring endpointId;
	float currentVolume = 1.0f;
	if (!GetEndpointIdentityAndVolume(device.get(), endpointId, currentVolume))
		return;

	std::lock_guard lock(g_volumeMutex);
	if (!g_playbackReferenceReady || g_playbackEndpointId != endpointId)
	{
		g_playbackEndpointId = std::move(endpointId);
		g_playbackReferenceVolume = currentVolume;
		g_playbackReferenceReady = true;
		AppendLog(L"Playback volume neutral reference captured at " +
			std::to_wstring(static_cast<int>(std::lround(currentVolume * 100.0f))) + L"%");
	}
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

bool SetDefaultRenderEndpointVolume(float controlVolume, bool allowUnmute)
{
	winrt::com_ptr<IMMDevice> device;
	if (!GetDefaultRenderEndpoint(device))
		return false;

	std::wstring endpointId;
	float currentVolume = 1.0f;
	if (!GetEndpointIdentityAndVolume(device.get(), endpointId, currentVolume))
		return false;

	float referenceVolume = currentVolume;
	{
		std::lock_guard lock(g_volumeMutex);
		if (!g_playbackReferenceReady || g_playbackEndpointId != endpointId)
		{
			g_playbackEndpointId = endpointId;
			g_playbackReferenceVolume = currentVolume;
			g_playbackReferenceReady = true;
			AppendLog(L"Playback output changed; neutral reference captured at " +
				std::to_wstring(static_cast<int>(std::lround(currentVolume * 100.0f))) + L"%");
		}
		referenceVolume = g_playbackReferenceVolume;
	}

	winrt::com_ptr<IAudioEndpointVolume> endpointVolume;
	auto result = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
		endpointVolume.put_void());
	if (FAILED(result))
	{
		AppendHresultLog(L"Activate endpoint volume failed", result);
		return false;
	}

	const auto targetVolume = MapPlaybackVolumeToEndpoint(controlVolume, referenceVolume);
	if (allowUnmute && controlVolume > 0.0f)
	{
		result = endpointVolume->SetMute(FALSE, nullptr);
		if (FAILED(result))
			AppendHresultLog(L"Unmute default render endpoint failed", result);
	}

	result = endpointVolume->SetMasterVolumeLevelScalar(targetVolume, nullptr);
	if (FAILED(result))
	{
		AppendHresultLog(L"Set endpoint volume failed", result);
		return false;
	}
	return true;
}

winrt::fire_and_forget ApplyPlaybackVolume(bool logResult = false, bool allowUnmute = false)
{
	co_await winrt::resume_background();
	const auto controlVolume = static_cast<float>(g_playbackVolume.load());
	if (SetDefaultRenderEndpointVolume(controlVolume, allowUnmute))
	{
		if (logResult)
			AppendLog(L"Applied playback volume to the default render endpoint: " +
				std::to_wstring(static_cast<int>(std::lround(controlVolume * 100.0f))) + L"%");
	}
	else if (logResult)
	{
		AppendLog(L"Could not apply playback volume to the default render endpoint");
	}
}
