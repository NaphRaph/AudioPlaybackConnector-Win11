#pragma once

bool SetProcessAudioSessionVolume(float volume)
{
	volume = std::clamp(volume, 0.0f, 1.0f);
	winrt::com_ptr<IMMDeviceEnumerator> deviceEnumerator;
	auto result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator), deviceEnumerator.put_void());
	if (FAILED(result))
	{
		AppendHresultLog(L"Create audio device enumerator failed", result);
		return false;
	}

	winrt::com_ptr<IMMDeviceCollection> devices;
	result = deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put());
	if (FAILED(result))
	{
		AppendHresultLog(L"Enumerate audio endpoints failed", result);
		return false;
	}

	UINT deviceCount = 0;
	devices->GetCount(&deviceCount);
	bool found = false;
	for (UINT deviceIndex = 0; deviceIndex < deviceCount; ++deviceIndex)
	{
		winrt::com_ptr<IMMDevice> device;
		if (FAILED(devices->Item(deviceIndex, device.put())))
			continue;

		winrt::com_ptr<IAudioSessionManager2> sessionManager;
		if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
			sessionManager.put_void())))
			continue;

		winrt::com_ptr<IAudioSessionEnumerator> sessions;
		if (FAILED(sessionManager->GetSessionEnumerator(sessions.put())))
			continue;

		int sessionCount = 0;
		sessions->GetCount(&sessionCount);
		for (int sessionIndex = 0; sessionIndex < sessionCount; ++sessionIndex)
		{
			winrt::com_ptr<IAudioSessionControl> session;
			if (FAILED(sessions->GetSession(sessionIndex, session.put())))
				continue;

			winrt::com_ptr<IAudioSessionControl2> session2;
			if (FAILED(session->QueryInterface(__uuidof(IAudioSessionControl2), session2.put_void())))
				continue;

			DWORD processId = 0;
			if (FAILED(session2->GetProcessId(&processId)) || processId != GetCurrentProcessId())
				continue;

			winrt::com_ptr<ISimpleAudioVolume> sessionVolume;
			if (FAILED(session->QueryInterface(__uuidof(ISimpleAudioVolume), sessionVolume.put_void())))
				continue;

			if (SUCCEEDED(sessionVolume->SetMasterVolume(volume, nullptr)))
				found = true;
		}
	}
	return found;
}

winrt::fire_and_forget ApplyPlaybackVolume(bool retryForNewSession = false)
{
	co_await winrt::resume_background();
	const auto attempts = retryForNewSession ? 10 : 1;
	for (int attempt = 0; attempt < attempts && !g_shuttingDown; ++attempt)
	{
		if (SetProcessAudioSessionVolume(static_cast<float>(g_playbackVolume.load())))
		{
			if (retryForNewSession)
				AppendLog(L"Applied playback volume to audio session");
			co_return;
		}
		if (attempt + 1 < attempts)
			co_await winrt::resume_after(std::chrono::milliseconds(250));
	}
	if (retryForNewSession)
		AppendLog(L"No application audio session was found for volume control");
}
