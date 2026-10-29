#pragma once

// The Bluetooth sink is rendered by an AudioSrv-owned session on a render
// endpoint. Its session identifier contains the associated A2DP capture
// endpoint GUID; the capture endpoint's own volume is not in the playback path.
inline std::wstring BluetoothParentId(std::wstring_view deviceId)
{
	const auto begin = deviceId.find(L"BTHENUM#");
	const auto end = deviceId.rfind(L"#{");
	if (begin == std::wstring_view::npos || end == std::wstring_view::npos || end <= begin)
		return {};
	std::wstring parent(deviceId.substr(begin, end - begin));
	std::replace(parent.begin(), parent.end(), L'#', L'\\');
	return parent;
}

inline bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right)
{
	return left.size() == right.size() &&
		CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
			right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

inline bool ContainsIgnoreCase(std::wstring_view text, std::wstring_view part)
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

inline bool GetPnPParentId(DEVINST child, std::wstring& parentId)
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

inline bool FindPhoneEndpointGuid(std::wstring_view deviceId, std::wstring& endpointGuid)
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

inline std::wstring FindCachedPhoneEndpointGuid(std::wstring_view deviceId)
{
	{
		std::lock_guard lock(g_volumeMutex);
		auto cached = g_phoneEndpointGuids.find(std::wstring(deviceId));
		if (cached != g_phoneEndpointGuids.end())
			return cached->second;
	}

	std::wstring endpointGuid;
	if (!FindPhoneEndpointGuid(deviceId, endpointGuid))
		return {};

	std::lock_guard lock(g_volumeMutex);
	g_phoneEndpointGuids.insert_or_assign(std::wstring(deviceId), endpointGuid);
	return endpointGuid;
}

struct PhoneAudioSession
{
	winrt::com_ptr<ISimpleAudioVolume> Volume;
	float Level{};
};

// The session is created by AudioSrv for the A2DP sink's render path. Windows
// keeps its volume across connections, so callers must restore any value they
// change even when the phone later pauses or disconnects.
inline bool FindPhoneAudioSession(std::wstring_view deviceId, PhoneAudioSession& session)
{
	const auto endpointGuid = FindCachedPhoneEndpointGuid(deviceId);
	if (endpointGuid.empty())
		return false;

	winrt::com_ptr<IMMDeviceEnumerator> enumerator;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator), enumerator.put_void())))
		return false;

	winrt::com_ptr<IMMDeviceCollection> outputs;
	if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, outputs.put())))
		return false;
	UINT outputCount = 0;
	if (FAILED(outputs->GetCount(&outputCount)))
		return false;

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
			winrt::com_ptr<IAudioSessionControl> control;
			if (FAILED(sessions->GetSession(sessionIndex, control.put())))
				continue;
			auto control2 = control.try_as<IAudioSessionControl2>();
			if (!control2)
				continue;
			AudioSessionState state{};
			if (FAILED(control2->GetState(&state)) || state == AudioSessionStateExpired)
				continue;
			LPWSTR rawSessionId = nullptr;
			if (FAILED(control2->GetSessionIdentifier(&rawSessionId)))
				continue;
			std::wstring sessionId(rawSessionId);
			CoTaskMemFree(rawSessionId);
			if (!ContainsIgnoreCase(sessionId, endpointGuid))
				continue;
			auto volume = control.try_as<ISimpleAudioVolume>();
			if (!volume)
				continue;
			float level = 0;
			if (FAILED(volume->GetMasterVolume(&level)))
				continue;
			session.Volume = volume;
			session.Level = std::clamp(level, 0.0f, 1.0f);
			return true;
		}
	}
	return false;
}

inline bool SetPhoneAudioSessionVolume(std::wstring_view deviceId, float level)
{
	PhoneAudioSession session;
	if (!FindPhoneAudioSession(deviceId, session))
		return false;

	level = std::clamp(level, 0.0f, 1.0f);
	if (std::abs(session.Level - level) < 0.001f)
		return true;
	const HRESULT result = session.Volume->SetMasterVolume(level, nullptr);
	if (FAILED(result))
	{
		AppendHresultLog(L"Set phone audio session volume failed", result);
		return false;
	}
	AppendLog(L"Phone audio session volume set to " +
		std::to_wstring(static_cast<int>(std::lround(level * 100.0f))) + L"% (was " +
		std::to_wstring(static_cast<int>(std::lround(session.Level * 100.0f))) + L"%): " + std::wstring(deviceId));
	return true;
}
