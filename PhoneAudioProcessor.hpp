#pragma once

#include <cstring>
#include <future>
#include <thread>

// One worker owns one phone's capture and render clients. All workers are in
// this process; the shared slider affects only samples copied from that phone.
class PhoneAudioProcessor : public std::enable_shared_from_this<PhoneAudioProcessor>
{
public:
	PhoneAudioProcessor(std::wstring deviceId, uint64_t generation)
		: m_deviceId(std::move(deviceId)), m_generation(generation),
		m_stoppedFuture(m_stopped.get_future().share()) {}

	void Start()
	{
		std::thread([self = shared_from_this()] { self->Run(); }).detach();
	}

	void Stop() noexcept { m_stop = true; }
	bool WaitStopped() const
	{
		return m_stoppedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
	}

private:
	static void Check(HRESULT result)
	{
		if (FAILED(result))
			throw winrt::hresult_error(result);
	}

	static bool IsFloatFormat(const WAVEFORMATEX* format)
	{
		if (format->wBitsPerSample != 32 ||
			format->nBlockAlign != format->nChannels * sizeof(float) ||
			format->nChannels == 0)
			return false;
		if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
			return true;
		if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
			format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			return false;
		const GUID floatSubtype = { 3, 0, 0x10, { 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71 } };
		return IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat,
			floatSubtype);
	}

	static std::wstring FormatError(std::wstring_view stage, HRESULT error)
	{
		wchar_t code[16]{};
		swprintf_s(code, L"0x%08X", static_cast<uint32_t>(error));
		return std::wstring(stage) + L": " + code;
	}

	void Run()
	{
		const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		bool muted = false;
		float originalVolume = 1.0f;
		std::wstring failureReason;
		std::wstring_view stage = L"Initialize audio worker";
		try
		{
			Check(apartment);
			winrt::com_ptr<IMMDeviceEnumerator> enumerator;
			stage = L"Create device enumerator";
			Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
				__uuidof(IMMDeviceEnumerator), enumerator.put_void()));

			winrt::com_ptr<IMMDevice> input;
			// A paused phone can leave its A2DP capture endpoint unplugged even
			// though Bluetooth OpenAsync and State both report success. Keep the
			// connection open and start the stream when playback activates it.
			bool waitingLogged = false;
			bool checkedLeftoverMute = false;
			const auto waitStarted = std::chrono::steady_clock::now();
			while (!m_stop && !g_shuttingDown)
			{
				// A previous run that ended while the phone was connected can leave
				// the Windows playback session muted, which makes the normal path
				// silent. Put it back before this worker silences it itself.
				if (!checkedLeftoverMute)
				{
					PhoneAudioSession existing;
					if (FindPhoneAudioSession(m_deviceId, existing))
					{
						checkedLeftoverMute = true;
						if (existing.Level <= 0.001f)
						{
							SetPhoneAudioSessionVolume(m_deviceId, 1.0f);
							AppendLog(L"Recovered phone session muted by a previous run: " + m_deviceId);
						}
					}
				}

				stage = L"Find phone audio endpoint";
				std::wstring endpointGuid = FindCachedPhoneEndpointGuid(m_deviceId);
				if (!endpointGuid.empty())
				{
					stage = L"Open phone audio endpoint";
					const std::wstring captureId = L"{0.0.1.00000000}." + endpointGuid;
					const HRESULT endpointResult = enumerator->GetDevice(captureId.c_str(), input.put());
					if (SUCCEEDED(endpointResult))
					{
						DWORD state = 0;
						if (SUCCEEDED(input->GetState(&state)) && state == DEVICE_STATE_ACTIVE)
							break;
						input = nullptr;
					}
					else
					{
						std::lock_guard lock(g_volumeMutex);
						g_phoneEndpointGuids.erase(std::wstring(m_deviceId));
					}
				}
				if (!waitingLogged &&
					std::chrono::steady_clock::now() - waitStarted >= std::chrono::seconds(1))
				{
					AppendLog(L"Waiting for phone audio playback: " + m_deviceId);
					waitingLogged = true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
			}
			if (m_stop || g_shuttingDown || !input)
				throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED));

			winrt::com_ptr<IAudioClient> captureClient;
			Check(input->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
				nullptr, captureClient.put_void()));

			WAVEFORMATEX* rawFormat = nullptr;
			stage = L"Read phone audio format";
			Check(captureClient->GetMixFormat(&rawFormat));
			std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(rawFormat, CoTaskMemFree);
			if (!IsFloatFormat(format.get()))
				throw winrt::hresult_error(AUDCLNT_E_UNSUPPORTED_FORMAT);

			winrt::com_ptr<IMMDevice> output;
			stage = L"Find playback output";
			Check(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, output.put()));
			LPWSTR rawOutputId = nullptr;
			Check(output->GetId(&rawOutputId));
			std::wstring outputId(rawOutputId);
			CoTaskMemFree(rawOutputId);
			winrt::com_ptr<IAudioClient> renderClient;
			stage = L"Open playback output";
			Check(output->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
				nullptr, renderClient.put_void()));

			stage = L"Initialize phone capture";
			Check(captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
				2'000'000, 0, format.get(), nullptr));
			stage = L"Initialize phone playback";
			Check(renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
				AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
				3'000'000, 0, format.get(), nullptr));

			winrt::com_ptr<IAudioCaptureClient> capture;
			winrt::com_ptr<IAudioRenderClient> render;
			stage = L"Get audio buffers";
			Check(captureClient->GetService(__uuidof(IAudioCaptureClient), capture.put_void()));
			Check(renderClient->GetService(__uuidof(IAudioRenderClient), render.put_void()));
			UINT32 renderCapacity = 0;
			Check(renderClient->GetBufferSize(&renderCapacity));
			const UINT32 preroll = (std::min)(renderCapacity / 2,
				static_cast<UINT32>(format->nSamplesPerSec / 20));
			if (preroll > 0)
			{
				BYTE* silence = nullptr;
				Check(render->GetBuffer(preroll, &silence));
				Check(render->ReleaseBuffer(preroll, AUDCLNT_BUFFERFLAGS_SILENT));
			}

			stage = L"Start audio streams";
			Check(renderClient->Start());
			try { Check(captureClient->Start()); }
			catch (...) { renderClient->Stop(); throw; }
			AppendLog(L"Phone-only audio processor started: " + m_deviceId);

			auto nextSessionCheck = std::chrono::steady_clock::now();
			auto nextOutputCheck = nextSessionCheck;
			while (!m_stop && !g_shuttingDown)
			{
				const auto now = std::chrono::steady_clock::now();
				if (now >= nextOutputCheck)
				{
					stage = L"Check playback output";
					winrt::com_ptr<IMMDevice> currentOutput;
					Check(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia,
						currentOutput.put()));
					LPWSTR currentId = nullptr;
					Check(currentOutput->GetId(&currentId));
					const bool changed = outputId != currentId;
					CoTaskMemFree(currentId);
					if (changed)
						throw winrt::hresult_error(AUDCLNT_E_DEVICE_INVALIDATED);
					nextOutputCheck = now + std::chrono::milliseconds(250);
				}
				stage = L"Capture phone audio";
				UINT32 packetFrames = 0;
				Check(capture->GetNextPacketSize(&packetFrames));
				while (packetFrames > 0 && !m_stop)
				{
					BYTE* source = nullptr;
					UINT32 frames = 0;
					DWORD flags = 0;
					UINT64 position = 0;
					UINT64 timestamp = 0;
					Check(capture->GetBuffer(&source, &frames, &flags, &position, &timestamp));
					const bool audible = !(flags & AUDCLNT_BUFFERFLAGS_SILENT) && frames > 0;

					// Silence the Windows-owned copy only once this worker has real
					// audio to play. A failed takeover therefore leaves the normal
					// playback path audible instead of a connected-but-silent phone.
					if (!muted && audible && std::chrono::steady_clock::now() >= nextSessionCheck)
					{
						nextSessionCheck = std::chrono::steady_clock::now() +
							std::chrono::milliseconds(250);
						PhoneAudioSession session;
						if (FindPhoneAudioSession(m_deviceId, session))
						{
							originalVolume = session.Level <= 0.001f ? 1.0f : session.Level;
							muted = SetPhoneAudioSessionVolume(m_deviceId, 0.0f);
							if (muted)
								AppendLog(L"Phone-only audio takeover started: " + m_deviceId);
						}
					}

					HRESULT processing = S_OK;
					if (muted && audible)
					{
						UINT32 padding = 0;
						processing = renderClient->GetCurrentPadding(&padding);
						if (SUCCEEDED(processing))
						{
							const UINT32 writeFrames = (std::min)(frames, renderCapacity - padding);
							if (writeFrames > 0)
							{
								BYTE* destination = nullptr;
								processing = render->GetBuffer(writeFrames, &destination);
								if (SUCCEEDED(processing))
								{
									std::memcpy(destination, source,
										static_cast<size_t>(writeFrames) * format->nBlockAlign);
									ProcessPhoneSamples(reinterpret_cast<float*>(destination),
										static_cast<size_t>(writeFrames) * format->nChannels,
										static_cast<float>(g_playbackVolume.load()));
									processing = render->ReleaseBuffer(writeFrames, 0);
								}
							}
						}
					}
					Check(capture->ReleaseBuffer(frames));
					stage = L"Render phone audio";
					Check(processing);
					Check(capture->GetNextPacketSize(&packetFrames));
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			captureClient->Stop();
			renderClient->Stop();
		}
		catch (const winrt::hresult_error& error)
		{
			const auto reason = FormatError(stage, error.code());
			if (!m_stop && !g_shuttingDown)
			{
				AppendLog(L"Phone audio processor failed: " + m_deviceId + L"; " + reason);
				failureReason = reason;
			}
		}
		catch (...)
		{
			const std::wstring reason = std::wstring(stage) + L": unexpected failure";
			if (!m_stop && !g_shuttingDown)
			{
				AppendLog(L"Phone audio processor failed: " + m_deviceId + L"; " + reason);
				failureReason = reason;
			}
		}
		if (muted)
		{
			// Windows keeps this session volume across connections. Restore the
			// value captured before the takeover so the next normal playback is
			// neither muted nor louder than the user left it.
			if (SetPhoneAudioSessionVolume(m_deviceId, originalVolume))
				AppendLog(L"Phone audio session volume restored: " + m_deviceId);
		}
		if (SUCCEEDED(apartment))
			CoUninitialize();
		m_stopped.set_value();
		if (!failureReason.empty() && !m_stop && !g_shuttingDown)
		{
			try { QueuePhoneAudioFailure(m_deviceId, m_generation, std::move(failureReason)); }
			catch (...) { AppendLog(L"Could not report phone audio failure"); }
		}
	}

	std::wstring m_deviceId;
	uint64_t m_generation{};
	std::atomic_bool m_stop{ false };
	std::promise<void> m_stopped;
	std::shared_future<void> m_stoppedFuture;
};
