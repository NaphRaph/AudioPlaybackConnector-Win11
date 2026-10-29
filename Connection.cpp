#include "pch.h"
#include "AudioPlaybackConnector.h"

// Bluetooth connection lifecycle, audio repair and radio recovery.

bool IsCurrentConnection(std::wstring_view deviceId, uint64_t generation)
{
	std::lock_guard lock(g_connectionMutex);
	auto it = g_audioPlaybackConnections.find(std::wstring(deviceId));
	return it != g_audioPlaybackConnections.end() && it->second.Generation == generation;
}

bool IsCurrentConnectionOpened(std::wstring_view deviceId, uint64_t generation)
{
	AudioPlaybackConnection connection = nullptr;
	{
		std::lock_guard lock(g_connectionMutex);
		auto it = g_audioPlaybackConnections.find(std::wstring(deviceId));
		if (it == g_audioPlaybackConnections.end() || it->second.Generation != generation)
			return false;
		connection = it->second.Connection;
	}

	try
	{
		return connection.State() == AudioPlaybackConnectionState::Opened;
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
		return false;
	}
}

winrt::Windows::Foundation::IAsyncOperation<bool> WaitForDeviceConnection(
	std::wstring deviceId, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!g_shuttingDown && std::chrono::steady_clock::now() < deadline)
	{
		{
			std::lock_guard lock(g_connectionMutex);
			auto it = g_audioPlaybackConnections.find(deviceId);
			if (it != g_audioPlaybackConnections.end() && !it->second.Connecting)
				co_return true;
			if (it == g_audioPlaybackConnections.end() &&
				g_deviceErrorMessages.find(deviceId) != g_deviceErrorMessages.end())
				co_return false;
		}
		co_await winrt::resume_after(AUDIO_REPAIR_POLL_INTERVAL);
	}
	co_return false;
}

bool IsDeviceConnectionInProgress(std::wstring_view deviceId)
{
	std::lock_guard lock(g_connectionMutex);
	auto it = g_audioPlaybackConnections.find(std::wstring(deviceId));
	return it != g_audioPlaybackConnections.end() && it->second.Connecting;
}

void QueueConnectionStateChanged(std::wstring deviceId, uint64_t generation)
{
	// This callback may run on a non-UI thread. Only post a message here;
	// the connection table is intentionally touched by the UI thread alone.
	if (g_shuttingDown || !IsWindow(g_hWnd))
		return;

	auto message = std::make_unique<ConnectionStateChangedMessage>();
	message->deviceId = std::move(deviceId);
	message->generation = generation;
	if (PostMessageW(g_hWnd, WM_CONNECTION_STATE_CHANGED, 0, reinterpret_cast<LPARAM>(message.get())))
		message.release();
}

namespace
{
	std::thread g_volumeApplierThread;
	std::atomic_bool g_volumeApplierStop = false;
}

// The slider controls the Windows playback session that carries each phone's
// audio. Windows keeps that session volume across connections and recreates the
// session when the phone switches outputs, so a light poll keeps it in sync.
void RunPhoneVolumeApplier()
{
	while (!g_volumeApplierStop && !g_shuttingDown)
	{
		std::vector<std::wstring> devices;
		{
			std::lock_guard lock(g_connectionMutex);
			for (const auto& [deviceId, entry] : g_audioPlaybackConnections)
			{
				if (!entry.Connecting)
					devices.push_back(deviceId);
			}
		}

		const auto volume = static_cast<float>(g_playbackVolume.load());
		for (const auto& deviceId : devices)
			SetPhoneAudioSessionVolume(deviceId, volume);

		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}
}

void StartPhoneVolumeApplier()
{
	if (g_volumeApplierThread.joinable())
		return;
	g_volumeApplierStop = false;
	g_volumeApplierThread = std::thread(RunPhoneVolumeApplier);
}

void StopPhoneVolumeApplier()
{
	g_volumeApplierStop = true;
	if (g_volumeApplierThread.joinable())
		g_volumeApplierThread.join();
}

void CloseCurrentConnection(std::wstring_view deviceId, uint64_t generation)
{
	std::lock_guard lock(g_connectionMutex);
	auto it = g_audioPlaybackConnections.find(std::wstring(deviceId));
	if (it == g_audioPlaybackConnections.end() || it->second.Generation != generation)
		return;

	it->second.Connection.Close();
	g_audioPlaybackConnections.erase(it);
	g_lastConnectionCloseTimes.insert_or_assign(std::wstring(deviceId), std::chrono::steady_clock::now());
}

// Takes ownership of the message allocated by QueueConnectionStateChanged().
void HandleConnectionStateChanged(ConnectionStateChangedMessage* raw)
{
	std::unique_ptr<ConnectionStateChangedMessage> stateChanged(raw);
	if (!stateChanged || g_shuttingDown)
		return;

	std::lock_guard lock(g_connectionMutex);
	auto it = g_audioPlaybackConnections.find(stateChanged->deviceId);
	if (it == g_audioPlaybackConnections.end() || it->second.Generation != stateChanged->generation)
		return;
	g_audioPlaybackConnections.erase(it);
	g_lastConnectionCloseTimes.insert_or_assign(stateChanged->deviceId, std::chrono::steady_clock::now());
	AppendLog(L"Bluetooth audio connection closed: " + stateChanged->deviceId);
	QueueDeviceListRefresh();
}

winrt::fire_and_forget ConnectDevice(std::wstring deviceId)
{
	if (g_radioRecoveryInProgress)
		co_return;
	{
		std::lock_guard lock(g_connectionMutex);
		g_deviceErrorMessages.erase(deviceId);
		g_radioRecoveryCandidates.erase(deviceId);
	}
	QueueDeviceListRefresh();
	// DeviceWatcher callbacks can run outside the XAML apartment. Recreate the
	// device on the UI apartment before starting the audio connection, matching
	// the object flow used by the original DevicePicker implementation.
	auto uiContext = winrt::apartment_context();
	try
	{
		auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
		co_await uiContext;
		if (g_shuttingDown || g_radioRecoveryInProgress)
			co_return;
		if (device)
		{
			ConnectDevice(device);
			co_return;
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		if (g_shuttingDown || g_radioRecoveryInProgress)
			co_return;

		std::wstring errorMessage = ex.message().c_str();
		errorMessage += L" (0x";
		wchar_t errorCode[9]{};
		swprintf_s(errorCode, L"%08X", static_cast<uint32_t>(ex.code()));
		errorMessage += errorCode;
		errorMessage += L")";
		{
			std::lock_guard lock(g_connectionMutex);
			g_deviceErrorMessages[deviceId] = std::move(errorMessage);
			if (IsRadioRecoveryCandidate(static_cast<uint32_t>(ex.code())))
				g_radioRecoveryCandidates.insert(deviceId);
		}
		QueueDeviceListRefresh();
		LOG_CAUGHT_EXCEPTION();
		co_return;
	}
	catch (...)
	{
		if (g_shuttingDown || g_radioRecoveryInProgress)
			co_return;
	}

	{
		std::lock_guard lock(g_connectionMutex);
		g_deviceErrorMessages[deviceId] = _(L"Unknown error");
	}
	QueueDeviceListRefresh();
}

winrt::fire_and_forget ConnectDevice(DeviceInformation device)
{
	if (g_radioRecoveryInProgress)
		co_return;
	// The original project performed the complete StartAsync/OpenAsync sequence
	// from the picker UI apartment. Preserve that behavior while the custom UI
	// remains responsible only for displaying device rows.
	auto uiContext = winrt::apartment_context();
	const auto deviceId = std::wstring(device.Id());
	AppendLog(L"Connection requested: " + std::wstring(device.Name()) + L" [" + deviceId + L"]");
	{
		std::lock_guard lock(g_connectionMutex);
		g_deviceErrorMessages.erase(deviceId);
		g_radioRecoveryCandidates.erase(deviceId);
	}
	QueueDeviceListRefresh();

	{
		std::lock_guard lock(g_connectionMutex);
		auto existing = g_audioPlaybackConnections.find(deviceId);
		if (existing != g_audioPlaybackConnections.end())
			co_return;
	}

	// Closing AudioPlaybackConnection only begins releasing the underlying A2DP
	// transport. Recreating it immediately can reuse a half-closed Windows
	// Bluetooth session which reports Opened but never carries audio.
	std::chrono::milliseconds releaseDelay{};
	{
		std::lock_guard lock(g_connectionMutex);
		auto lastClose = g_lastConnectionCloseTimes.find(deviceId);
		if (lastClose != g_lastConnectionCloseTimes.end())
		{
			const auto elapsed = std::chrono::steady_clock::now() - lastClose->second;
			if (elapsed < CONNECTION_RELEASE_COOLDOWN)
				releaseDelay = std::chrono::duration_cast<std::chrono::milliseconds>(CONNECTION_RELEASE_COOLDOWN - elapsed);
		}
	}
	if (releaseDelay.count() > 0)
	{
		co_await winrt::resume_after(releaseDelay);
		co_await uiContext;
		if (g_shuttingDown)
			co_return;
	}
	{
		std::lock_guard lock(g_connectionMutex);
		if (g_audioPlaybackConnections.find(deviceId) != g_audioPlaybackConnections.end())
			co_return;
	}

	bool success = false;
	bool radioRecoveryCandidate = false;
	std::wstring errorMessage;
	AudioPlaybackConnection connection = nullptr;
	uint64_t generation = 0;
	std::wstring_view connectionStage = L"TryCreateFromId";

	try
	{
		connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (!connection)
		{
			errorMessage = _(L"Unknown error");
		}
		else
		{
			generation = ++g_nextConnectionGeneration;
			{
				std::lock_guard lock(g_connectionMutex);
				g_audioPlaybackConnections.insert_or_assign(deviceId, AudioPlaybackConnectionEntry{
					connection, generation, true
				});
			}
			QueueDeviceListRefresh();

			connection.StateChanged([deviceId, generation](const auto& sender, const auto&) {
				if (sender.State() == AudioPlaybackConnectionState::Closed)
					QueueConnectionStateChanged(deviceId, generation);
			});

			// StartAsync is tied to this connection instance. Every newly created
			// connection must be enabled before it is opened.
			connectionStage = L"StartAsync";
			AppendLog(L"Connection StartAsync: " + deviceId);
			co_await connection.StartAsync();
			co_await uiContext;
			if (g_shuttingDown || !IsCurrentConnection(deviceId, generation))
				co_return;

			// Open immediately after StartAsync, as required by the documented
			// AudioPlaybackConnection lifecycle. An arbitrary delay here can leave
			// an already-playing source waiting on a half-open A2DP transport.
			connectionStage = L"OpenAsync";
			AppendLog(L"Connection OpenAsync: " + deviceId);
			auto result = co_await connection.OpenAsync();
			co_await uiContext;
			if (g_shuttingDown || !IsCurrentConnection(deviceId, generation))
				co_return;
			AppendLog(L"Connection OpenAsync returned status " +
				std::to_wstring(static_cast<int>(result.Status())) + L": " + deviceId);
			connectionStage = L"WaitForOpenedState";

			switch (result.Status())
			{
			case AudioPlaybackConnectionOpenResultStatus::Success:
			{
				// OpenAsync success only means Windows accepted the request. On the
				// affected vivo/Windows combination it can return before the A2DP
				// transport reaches Opened, which previously produced a false
				// "Connected" row with no sound. Wait for the actual audio state.
				const auto stateDeadline =
					std::chrono::steady_clock::now() + CONNECTION_OPENED_TIMEOUT;
				while (!g_shuttingDown && IsCurrentConnection(deviceId, generation) &&
					std::chrono::steady_clock::now() < stateDeadline)
				{
					if (IsCurrentConnectionOpened(deviceId, generation))
					{
						success = true;
						break;
					}
					co_await winrt::resume_after(AUDIO_REPAIR_POLL_INTERVAL);
					co_await uiContext;
				}
				if (!success)
					errorMessage = _(L"The Bluetooth audio channel did not become ready");
				break;
			}
			case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
				success = false;
				errorMessage = _(L"The request timed out");
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				success = false;
				errorMessage = _(L"The operation was denied by the system");
				break;
			case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				success = false;
				{
					const auto extendedError = result.ExtendedError();
					radioRecoveryCandidate = IsRadioRecoveryCandidate(static_cast<uint32_t>(extendedError));
					LOG_HR(extendedError);
					wchar_t errorCode[16]{};
					swprintf_s(errorCode, L" (0x%08X)", static_cast<uint32_t>(extendedError));
					errorMessage = _(L"Unknown error");
					errorMessage += errorCode;
				}
				break;
			}
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		success = false;
		radioRecoveryCandidate = IsRadioRecoveryCandidate(static_cast<uint32_t>(ex.code()));
		errorMessage.resize(64);
		while (1)
		{
			auto result = swprintf(errorMessage.data(), errorMessage.size(), L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
			if (result < 0)
				errorMessage.resize(errorMessage.size() * 2);
			else
			{
				errorMessage.resize(result);
				break;
			}
		}
		AppendLog(L"Connection exception during " + std::wstring(connectionStage) +
			L": " + deviceId);
		LOG_CAUGHT_EXCEPTION();
	}
	catch (...)
	{
		success = false;
		errorMessage = _(L"Unknown error");
		LOG_CAUGHT_EXCEPTION();
	}
	if (g_radioRecoveryInProgress)
		co_return;
	if (generation != 0 && !IsCurrentConnection(deviceId, generation))
	{
		// A user disconnect or device removal can complete an older OpenAsync
		// after a newer attempt has started. Never overwrite the newer state.
		AppendLog(L"Obsolete connection completion ignored: " + deviceId);
		co_return;
	}
	if (success && IsCurrentConnection(deviceId, generation))
	{
		bool priming = false;
		{
			std::lock_guard lock(g_connectionMutex);
			priming = g_primedDevices.insert(deviceId).second;
		}
		if (priming)
		{
			// The first A2DP sink connection after the app starts can report
			// Opened while carrying no samples. Reopen once, exactly like the
			// manual Repair audio action, so the link that stays open is a
			// freshly negotiated one.
			AppendLog(L"Priming first connection: " + deviceId);
			CloseCurrentConnection(deviceId, generation);
			co_await winrt::resume_after(CONNECTION_RELEASE_COOLDOWN);
			co_await uiContext;
			if (g_shuttingDown)
				co_return;
			ConnectDevice(device);
			co_return;
		}

	}

	if (success && IsCurrentConnection(deviceId, generation))
	{
		{
			std::lock_guard lock(g_connectionMutex);
			auto it = g_audioPlaybackConnections.find(deviceId);
			if (it == g_audioPlaybackConnections.end())
				co_return;
			it->second.Connecting = false;
			g_deviceErrorMessages.erase(deviceId);
			g_autoRadioRecoveryAttempted.erase(deviceId);
		}
		AppendLog(L"Connection opened: " + deviceId);
	}
	else if (!g_shuttingDown)
	{
		if (generation != 0 && IsCurrentConnection(deviceId, generation))
			CloseCurrentConnection(deviceId, generation);

		// A cold Windows Bluetooth stack can reject the first open with
		// 0x8007001F until the radio is cycled. Try that once per device and
		// only fall back to the visible error when the recovery did not help.
		bool startRadioRecovery = false;
		{
			std::lock_guard lock(g_connectionMutex);
			startRadioRecovery = radioRecoveryCandidate &&
				g_autoRadioRecoveryAttempted.insert(deviceId).second;
			if (radioRecoveryCandidate)
				g_radioRecoveryCandidates.insert(deviceId);
			if (!startRadioRecovery)
				g_deviceErrorMessages[deviceId] = errorMessage.empty() ? _(L"Unknown error") : errorMessage;
		}
		AppendLog(L"Connection failed: " + deviceId + L"; " +
			(errorMessage.empty() ? std::wstring(L"Unknown error") : errorMessage));
		if (startRadioRecovery)
		{
			AppendLog(L"Automatic Bluetooth radio recovery after 0x8007001F: " + deviceId);
			RecoverBluetoothRadio(deviceId);
			co_return;
		}
	}

	QueueDeviceListRefresh();
}

winrt::fire_and_forget AutoConnectDevice(std::wstring deviceId)
{
	if (g_radioRecoveryInProgress)
		co_return;
	auto uiContext = winrt::apartment_context();
	{
		std::lock_guard lock(g_connectionMutex);
		if (!g_deviceOperations.emplace(deviceId, DeviceOperation::Repairing).second)
			co_return;
	}
	auto autoConnectGuard = wil::scope_exit([deviceId] {
		{
			std::lock_guard lock(g_connectionMutex);
			g_deviceOperations.erase(deviceId);
		}
		QueueDeviceListRefresh();
	});

	AppendLog(L"Automatic connection started: " + deviceId);
	QueueDeviceListRefresh();

	try
	{
		// Windows may still be bringing Bluetooth and the audio graph online when
		// this tray application starts during sign-in. Avoid racing those services.
		co_await winrt::resume_after(AUTO_CONNECT_STARTUP_DELAY);
		co_await uiContext;
		if (g_shuttingDown || g_radioRecoveryInProgress)
			co_return;

		// Keep the first opened connection. The audio worker waits for playback
		// if the phone has not yet activated its capture endpoint.
		ConnectDevice(deviceId);
		const auto connected = co_await WaitForDeviceConnection(
			deviceId, std::chrono::duration_cast<std::chrono::milliseconds>(
				AUDIO_REPAIR_CONNECTION_TIMEOUT));
		co_await uiContext;
		if (g_shuttingDown)
			co_return;
		if (!connected)
		{
			// A timeout is not an OpenAsync failure. Do not abort a pending open.
			AppendLog((IsDeviceConnectionInProgress(deviceId) ?
				L"Automatic connection still opening; leaving it alone: " :
				L"Automatic connection failed: ") + deviceId);
			co_return;
		}
		AppendLog(L"Automatic connection completed: " + deviceId);
	}
	catch (...)
	{
		AppendLog(L"Automatic connection failed with an exception: " + deviceId);
		LOG_CAUGHT_EXCEPTION();
	}
}

winrt::fire_and_forget DisconnectDevice(std::wstring deviceId)
{
	auto uiContext = winrt::apartment_context();
	AppendLog(L"Disconnect requested: " + deviceId);
	uint64_t generation = 0;
	{
		std::lock_guard lock(g_connectionMutex);
		auto it = g_audioPlaybackConnections.find(deviceId);
		if (it == g_audioPlaybackConnections.end())
			co_return;
		generation = it->second.Generation;
		if (!g_deviceOperations.emplace(deviceId, DeviceOperation::Disconnecting).second)
			co_return;
	}
	auto guard = wil::scope_exit([deviceId] {
		std::lock_guard lock(g_connectionMutex);
		g_deviceOperations.erase(deviceId);
		QueueDeviceListRefresh();
	});
	co_await uiContext;
	if (g_shuttingDown)
		co_return;
	CloseCurrentConnection(deviceId, generation);
	{
		std::lock_guard lock(g_connectionMutex);
		g_deviceErrorMessages.erase(deviceId);
	}
	QueueDeviceListRefresh();
}

winrt::fire_and_forget RepairDeviceAudio(std::wstring deviceId)
{
	if (g_radioRecoveryInProgress)
		co_return;
	auto uiContext = winrt::apartment_context();
	{
		std::lock_guard lock(g_connectionMutex);
		if (!g_deviceOperations.emplace(deviceId, DeviceOperation::Repairing).second)
			co_return;
	}
	auto repairGuard = wil::scope_exit([deviceId] {
		{
			std::lock_guard lock(g_connectionMutex);
			g_deviceOperations.erase(deviceId);
		}
		QueueDeviceListRefresh();
	});

	AppendLog(L"Audio repair requested: " + deviceId);
	QueueDeviceListRefresh();

	// Retry with bounded release delays while Bluetooth cannot open. Keep the
	// first opened connection; its worker waits if the phone is paused.
	constexpr std::chrono::milliseconds releaseDelays[AUDIO_REPAIR_MAX_ATTEMPTS] = {
		std::chrono::seconds(3), std::chrono::seconds(5), std::chrono::seconds(8)
	};

	try
	{
		for (int attempt = 0; attempt < AUDIO_REPAIR_MAX_ATTEMPTS && !g_shuttingDown && !g_radioRecoveryInProgress; ++attempt)
		{
			co_await uiContext;
			if (IsDeviceConnectionInProgress(deviceId))
			{
				AppendLog(L"Audio repair deferred while connection is still opening: " + deviceId);
				co_return;
			}
			co_await uiContext;
			if (g_shuttingDown)
				co_return;
			{
				std::lock_guard lock(g_connectionMutex);
				auto it = g_audioPlaybackConnections.find(deviceId);
				if (it != g_audioPlaybackConnections.end())
				{
					it->second.Connection.Close();
					g_audioPlaybackConnections.erase(it);
					g_lastConnectionCloseTimes.insert_or_assign(deviceId, std::chrono::steady_clock::now());
				}
				g_deviceErrorMessages.erase(deviceId);
			}
			QueueDeviceListRefresh();

			AppendLog(L"Audio repair attempt " + std::to_wstring(attempt + 1) +
				L"/" + std::to_wstring(AUDIO_REPAIR_MAX_ATTEMPTS) +
				L", release wait " + std::to_wstring(releaseDelays[attempt].count()) +
				L" ms: " + deviceId);
			co_await winrt::resume_after(releaseDelays[attempt]);
			co_await uiContext;
			if (g_shuttingDown)
				co_return;

			ConnectDevice(deviceId);

			const auto opened = co_await WaitForDeviceConnection(
				deviceId, std::chrono::duration_cast<std::chrono::milliseconds>(
					AUDIO_REPAIR_CONNECTION_TIMEOUT));

			if (!opened)
			{
				AppendLog((IsDeviceConnectionInProgress(deviceId) ?
					L"Audio repair connection still opening on attempt " :
					L"Audio repair connection failed on attempt ") +
					std::to_wstring(attempt + 1) + L": " + deviceId);
				if (IsDeviceConnectionInProgress(deviceId))
					co_return;
				continue;
			}

			AppendLog(L"Audio repair completed on attempt " +
				std::to_wstring(attempt + 1) + L": " + deviceId);
			co_return;
		}
	}
	catch (...)
	{
		AppendLog(L"Audio repair failed with an exception: " + deviceId);
		LOG_CAUGHT_EXCEPTION();
	}
}

winrt::Windows::Foundation::IAsyncOperation<bool> WaitForRadioState(
	winrt::Windows::Devices::Radios::Radio radio,
	winrt::Windows::Devices::Radios::RadioState expected,
	std::chrono::seconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!g_shuttingDown && std::chrono::steady_clock::now() < deadline)
	{
		if (radio.State() == expected)
			co_return true;
		co_await winrt::resume_after(AUDIO_REPAIR_POLL_INTERVAL);
	}
	co_return false;
}

winrt::fire_and_forget RecoverBluetoothRadio(std::wstring deviceId)
{
	using namespace winrt::Windows::Devices::Radios;
	auto uiContext = winrt::apartment_context();
	if (g_radioRecoveryInProgress.exchange(true))
		co_return;
	{
		std::lock_guard lock(g_connectionMutex);
		g_deviceOperations.insert_or_assign(deviceId, DeviceOperation::Repairing);
	}
	auto guard = wil::scope_exit([deviceId] {
		g_radioRecoveryInProgress = false;
		{
			std::lock_guard lock(g_connectionMutex);
			g_deviceOperations.erase(deviceId);
		}
		QueueDeviceListRefresh();
		if (g_exitPendingAfterRadioRecovery.exchange(false) && IsWindow(g_hWnd))
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});
	QueueDeviceListRefresh();
	AppendLog(L"User-confirmed Bluetooth radio recovery started: " + deviceId);

	Radio radio = nullptr;
	bool offRequested = false;
	bool observedOff = false;
	bool observedOn = false;
	try
	{
		for (const auto& candidate : co_await Radio::GetRadiosAsync())
		{
			if (candidate.Kind() == RadioKind::Bluetooth)
			{
				radio = candidate;
				break;
			}
		}
		co_await uiContext;
		if (!radio || radio.State() != RadioState::On)
			throw winrt::hresult_error(E_FAIL, L"Bluetooth radio is unavailable or not on");

		const auto access = co_await Radio::RequestAccessAsync();
		co_await uiContext;
		if (access != RadioAccessStatus::Allowed)
		{
			AppendLog(L"Bluetooth radio control denied: " + std::to_wstring(static_cast<int>(access)));
			throw winrt::hresult_error(E_ACCESSDENIED, L"Windows denied Bluetooth radio control");
		}
		if (g_shuttingDown)
			co_return;

		// The radio reset affects every Bluetooth link. Release our own audio
		// connections first; never close a connection while its worker is active.
		std::vector<std::pair<std::wstring, uint64_t>> connections;
		{
			std::lock_guard lock(g_connectionMutex);
			for (const auto& [id, entry] : g_audioPlaybackConnections)
				connections.emplace_back(id, entry.Generation);
		}
		for (const auto& [id, generation] : connections)
		{
			co_await uiContext;
			if (g_shuttingDown)
				co_return;
			CloseCurrentConnection(id, generation);
		}

		offRequested = true;
		AppendLog(L"Requesting Bluetooth radio off");
		if ((co_await radio.SetStateAsync(RadioState::Off)) != RadioAccessStatus::Allowed)
			throw winrt::hresult_error(E_ACCESSDENIED, L"Windows rejected Bluetooth radio off");
		observedOff = co_await WaitForRadioState(radio, RadioState::Off, std::chrono::seconds(10));
		if (!observedOff)
			throw winrt::hresult_error(E_FAIL, L"Bluetooth radio did not turn off");

		AppendLog(L"Requesting Bluetooth radio on");
		if ((co_await radio.SetStateAsync(RadioState::On)) != RadioAccessStatus::Allowed)
			throw winrt::hresult_error(E_ACCESSDENIED, L"Windows rejected Bluetooth radio on");
		observedOn = co_await WaitForRadioState(radio, RadioState::On, std::chrono::seconds(15));
		if (!observedOn)
			throw winrt::hresult_error(E_FAIL, L"Bluetooth radio did not turn on");
	}
	catch (...)
	{
		AppendLog(L"Bluetooth radio recovery failed: " + deviceId);
		LOG_CAUGHT_EXCEPTION();
	}

	// If the radio was switched off but the normal on step failed, make one
	// best-effort request to restore it before asking for manual intervention.
	if (offRequested && !observedOn && radio)
	{
		try
		{
			AppendLog(L"Attempting to restore Bluetooth radio on");
			if ((co_await radio.SetStateAsync(RadioState::On)) == RadioAccessStatus::Allowed)
				observedOn = co_await WaitForRadioState(radio, RadioState::On, std::chrono::seconds(15));
		}
		catch (...)
		{
			LOG_CAUGHT_EXCEPTION();
		}
	}
	co_await uiContext;
	if (g_shuttingDown)
		co_return;
	if (!CanReconnectAfterRadioCycle(observedOff, observedOn))
	{
		{
			std::lock_guard lock(g_connectionMutex);
			g_deviceErrorMessages[deviceId] = _(L"Bluetooth recovery was not completed");
		}
		AppendLog(L"Bluetooth radio recovery incomplete; opening Windows settings: " + deviceId);
		MessageBoxW(g_hWnd,
			_(L"Windows could not restart Bluetooth. Open Bluetooth Settings to switch it off and on manually, then reconnect the phone."),
			_(L"Bluetooth recovery was not completed"), MB_OK | MB_ICONWARNING | MB_TOPMOST);
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
		co_return;
	}

	AppendLog(L"Bluetooth radio cycled; reconnecting phone: " + deviceId);
	co_await winrt::resume_after(std::chrono::seconds(2));
	co_await uiContext;
	if (g_shuttingDown)
		co_return;
	{
		std::lock_guard lock(g_connectionMutex);
		g_deviceErrorMessages.erase(deviceId);
		g_radioRecoveryCandidates.erase(deviceId);
		g_deviceOperations.erase(deviceId);
	}
	g_radioRecoveryInProgress = false;
	QueueDeviceListRefresh();
	ConnectDevice(deviceId);
}

