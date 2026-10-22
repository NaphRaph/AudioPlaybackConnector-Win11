#pragma once

// The Bluetooth sink is rendered by an AudioSrv-owned session on a render
// endpoint. Its session identifier contains the associated A2DP capture
// endpoint GUID; the capture endpoint's own volume is not in the playback path.
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

bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right)
{
	return left.size() == right.size() &&
		CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
			right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool ContainsIgnoreCase(std::wstring_view text, std::wstring_view part)
{
	if (part.empty() || part.size() > text.size())
		return false;
	for (size_t i = 0; i <= text.size() - part.size(); ++i)
	{
		if (EqualsIgnoreCase(text.substr(i, part.size()), part))
			return true;
	}
	return false;
}

bool GetPnPParentId(DEVINST child, std::wstring& parentId)
{
	DEVINST parent = 0;
	if (CM_Get_Parent(&parent, child, 0) != CR_SUCCESS)
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

bool FindPhoneEndpointGuid(std::wstring_view deviceId, std::wstring& endpointGuid)
{
	const auto expectedParent = BluetoothParentId(deviceId);
	if (expectedParent.empty())
		return false;
	constexpr auto filter = L"SWD\\MMDEVAPI";
	constexpr ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR;
	ULONG size = 0;
	if (CM_Get_Device_ID_List_SizeW(&size, filter, flags) != CR_SUCCESS || size < 2)
		return false;
	std::wstring ids(size, L'\0');
	if (CM_Get_Device_ID_ListW(filter, ids.data(), size, flags) != CR_SUCCESS)
		return false;
	for (wchar_t* id = ids.data(); *id; id += wcslen(id) + 1)
	{
		std::wstring_view instance(id);
		constexpr std::wstring_view capturePrefix = L"SWD\\MMDEVAPI\\{0.0.1.";
		if (instance.size() < capturePrefix.size() ||
			!EqualsIgnoreCase(instance.substr(0, capturePrefix.size()), capturePrefix))
			continue;
		DEVINST endpoint = 0;
		if (CM_Locate_DevNodeW(&endpoint, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
			continue;
		std::wstring parent;
		if (!GetPnPParentId(endpoint, parent) || !EqualsIgnoreCase(parent, expectedParent))
			continue;
		const auto guidStart = instance.rfind(L'{');
		if (guidStart == std::wstring_view::npos || instance.back() != L'}')
			continue;
		endpointGuid.assign(instance.substr(guidStart));
		AppendLog(L"Matched phone A2DP endpoint " + endpointGuid);
		return true;
	}
	return false;
}

float MapPlaybackVolumeToSession(float slider, float reference)
{
	slider = std::clamp(slider, 0.0f, 1.0f);
	reference = std::clamp(reference, 0.0f, 1.0f);
	constexpr float neutral = static_cast<float>(DEFAULT_PLAYBACK_VOLUME);
	if (slider <= neutral)
		return reference * (slider / neutral);
	return reference + (1.0f - reference) * ((slider - neutral) / (1.0f - neutral));
}

bool SetPhoneSessionVolume(std::wstring_view deviceId, float slider)
{
	std::wstring endpointGuid;
	{
		std::lock_guard lock(g_volumeMutex);
		auto cached = g_phoneEndpointGuids.find(std::wstring(deviceId));
		if (cached != g_phoneEndpointGuids.end())
			endpointGuid = cached->second;
	}
	if (endpointGuid.empty())
	{
		if (!FindPhoneEndpointGuid(deviceId, endpointGuid))
			return false;
		std::lock_guard lock(g_volumeMutex);
		g_phoneEndpointGuids.insert_or_assign(std::wstring(deviceId), endpointGuid);
	}

	winrt::com_ptr<IMMDeviceEnumerator> enumerator;
	auto result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator), enumerator.put_void());
	if (FAILED(result))
	{
		AppendHresultLog(L"Create audio device enumerator failed", result);
		return false;
	}
	winrt::com_ptr<IMMDeviceCollection> outputs;
	result = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, outputs.put());
	if (FAILED(result))
	{
		AppendHresultLog(L"Enumerate render endpoints failed", result);
		return false;
	}
	UINT outputCount = 0;
	if (FAILED(outputs->GetCount(&outputCount)))
		return false;
	bool found = false;
	for (UINT outputIndex = 0; outputIndex < outputCount; ++outputIndex)
	{
		winrt::com_ptr<IMMDevice> output;
		if (FAILED(outputs->Item(outputIndex, output.put())))
			continue;
		winrt::com_ptr<IAudioSessionManager2> manager;
		if (FAILED(output->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
			nullptr, manager.put_void())))
			continue;
		winrt::com_ptr<IAudioSessionEnumerator> sessions;
		if (FAILED(manager->GetSessionEnumerator(sessions.put())))
			continue;
		int sessionCount = 0;
		if (FAILED(sessions->GetCount(&sessionCount)))
			continue;
		for (int sessionIndex = 0; sessionIndex < sessionCount; ++sessionIndex)
		{
			winrt::com_ptr<IAudioSessionControl> session;
			if (FAILED(sessions->GetSession(sessionIndex, session.put())))
				continue;
			auto session2 = session.try_as<IAudioSessionControl2>();
			if (!session2)
				continue;
			AudioSessionState state{};
			if (FAILED(session2->GetState(&state)) || state == AudioSessionStateExpired)
				continue;
			LPWSTR rawSessionId = nullptr;
			if (FAILED(session2->GetSessionIdentifier(&rawSessionId)))
				continue;
			std::wstring sessionId(rawSessionId);
			CoTaskMemFree(rawSessionId);
			if (!ContainsIgnoreCase(sessionId, endpointGuid))
				continue;
			auto volume = session.try_as<ISimpleAudioVolume>();
			if (!volume)
				continue;
			float current = 1.0f;
			if (FAILED(volume->GetMasterVolume(&current)))
				continue;
			float reference;
			{
				std::lock_guard lock(g_volumeMutex);
				auto [it, inserted] = g_sessionVolumeReferences.try_emplace(endpointGuid,
					std::clamp(current, 0.0f, 1.0f));
				reference = it->second;
				if (inserted)
					AppendLog(L"Phone session neutral reference: " + endpointGuid + L" at " +
						std::to_wstring(static_cast<int>(std::lround(reference * 100.0f))) + L"%");
			}
			const auto target = MapPlaybackVolumeToSession(slider, reference);
			const bool changed = std::abs(current - target) > 0.001f;
			if (changed)
			{
				result = volume->SetMasterVolume(target, nullptr);
				if (FAILED(result))
				{
					AppendHresultLog(L"Set phone session volume failed", result);
					continue;
				}
			}
			found = true;
			if (changed)
				AppendLog(L"Applied phone-only session volume: slider " +
					std::to_wstring(static_cast<int>(std::lround(slider * 100.0f))) + L"%, session " +
					std::to_wstring(static_cast<int>(std::lround(target * 100.0f))) + L"%");
		}
	}
	if (found && slider >= static_cast<float>(DEFAULT_PLAYBACK_VOLUME))
	{
		std::lock_guard lock(g_volumeMutex);
		g_sessionVolumeReferences.erase(endpointGuid);
	}
	return found;
}
