#pragma once

#include <cstring>
#include <future>
#include <thread>

// Per-phone audio controller. Manages both:
// 1. 0%-100%: native Windows playback session volume (zero latency, no extra stream,
//    no WASAPI capture client opened).
// 2. 101%-200%: shared-mode WASAPI capture + boosted render with soft peak limiter,
//    automatically releasing the render stream and unmuting the native session
//    whenever the phone is paused or silent.
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
	enum class SampleFormat
	{
		Unsupported,
		Float32,
		Pcm16
	};

	static SampleFormat DetectSampleFormat(const WAVEFORMATEX* format) noexcept
	{
		if (!format || format->nChannels == 0)
			return SampleFormat::Unsupported;

		if (format->wBitsPerSample == 32 &&
			format->nBlockAlign == format->nChannels * sizeof(float))
		{
			if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
				return SampleFormat::Float32;
			if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
				format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			{
				const GUID floatSubtype = { 3, 0, 0x10, { 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71 } };
				if (IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat, floatSubtype))
					return SampleFormat::Float32;
			}
		}

		if (format->wBitsPerSample == 16 &&
			format->nBlockAlign == format->nChannels * sizeof(int16_t))
		{
			if (format->wFormatTag == WAVE_FORMAT_PCM)
				return SampleFormat::Pcm16;
			if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
				format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			{
				const GUID pcmSubtype = { 1, 0, 0x10, { 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71 } };
				if (IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat, pcmSubtype))
					return SampleFormat::Pcm16;
			}
		}

		return SampleFormat::Unsupported;
	}

	void SyncNativeSessionVolume(bool& checkedLeftoverMute)
	{
		PhoneAudioSession session;
		if (!FindPhoneAudioSession(m_deviceId, session))
			return;

		if (!checkedLeftoverMute)
		{
			checkedLeftoverMute = true;
			if (session.Level <= 0.001f && g_playbackVolume.load() > 0.001)
			{
				const float recovered = NativeSessionVolumeForSlider(g_playbackVolume.load());
				SetPhoneAudioSessionVolume(m_deviceId, recovered);
				AppendLog(L"Recovered phone session muted by a previous run: " + m_deviceId);
				return;
			}
		}

		if (!g_playbackVolumeExplicit.load())
		{
			const double adopted = session.Level > 0.001f ? static_cast<double>(session.Level) : 1.0;
			g_playbackVolume.store(adopted);
			g_playbackVolumeExplicit = true;
			AppendLog(L"Adopted phone session volume: " +
				std::to_wstring(static_cast<int>(std::lround(adopted * 100.0))) + L"%");
			if (IsWindow(g_hWnd))
				PostMessageW(g_hWnd, WM_VOLUME_CHANGED, 0, 0);
			return;
		}

		const float target = NativeSessionVolumeForSlider(g_playbackVolume.load());
		if (g_volumeApplyPending.load() || std::abs(session.Level - target) >= 0.01f)
		{
			if (SetPhoneAudioSessionVolume(m_deviceId, target))
				g_volumeApplyPending = false;
		}
	}

	bool OpenRenderStream(
		IMMDeviceEnumerator* enumerator,
		const std::wstring& targetOutputId,
		const WAVEFORMATEX* format,
		winrt::com_ptr<IAudioClient>& renderClient,
		winrt::com_ptr<IAudioRenderClient>& render,
		std::wstring& renderOutputId,
		UINT32& renderCapacity)
	{
		render = nullptr;
		renderClient = nullptr;
		renderOutputId.clear();
		renderCapacity = 0;

		winrt::com_ptr<IMMDevice> output;
		if (!targetOutputId.empty())
		{
			if (FAILED(enumerator->GetDevice(targetOutputId.c_str(), output.put())))
				output = nullptr;
		}
		if (!output && FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, output.put())))
			return false;

		LPWSTR rawOutputId = nullptr;
		if (FAILED(output->GetId(&rawOutputId)))
			return false;
		renderOutputId.assign(rawOutputId);
		CoTaskMemFree(rawOutputId);

		if (FAILED(output->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, renderClient.put_void())))
			return false;

		if (FAILED(renderClient->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
			2'000'000, 0, format, nullptr)))
		{
			renderClient = nullptr;
			return false;
		}

		if (FAILED(renderClient->GetService(__uuidof(IAudioRenderClient), render.put_void())) ||
			FAILED(renderClient->GetBufferSize(&renderCapacity)))
		{
			render = nullptr;
			renderClient = nullptr;
			return false;
		}

		const UINT32 preroll = (std::min)(renderCapacity / 4, static_cast<UINT32>(format->nSamplesPerSec / 40));
		if (preroll > 0)
		{
			BYTE* silence = nullptr;
			if (SUCCEEDED(render->GetBuffer(preroll, &silence)))
				render->ReleaseBuffer(preroll, AUDCLNT_BUFFERFLAGS_SILENT);
		}

		if (FAILED(renderClient->Start()))
		{
			render = nullptr;
			renderClient = nullptr;
			return false;
		}
		return true;
	}

	void Run()
	{
		const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		HANDLE mmcss = nullptr;
		bool boostMuted = false;
		bool checkedLeftoverMute = false;

		auto restoreMuteOnExit = wil::scope_exit([&] {
			if (boostMuted)
			{
				const float restoreLevel = NativeSessionVolumeForSlider(g_playbackVolume.load());
				SetPhoneAudioSessionVolume(m_deviceId, restoreLevel);
				AppendLog(L"Phone audio session volume restored on worker exit: " + m_deviceId);
				boostMuted = false;
			}
			if (mmcss)
				AvRevertMmThreadCharacteristics(mmcss);
			if (SUCCEEDED(apartment))
				CoUninitialize();
			m_stopped.set_value();
		});

		if (FAILED(apartment))
			return;

		DWORD mmcssTask = 0;
		mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &mmcssTask);
		if (mmcss)
			AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);

		winrt::com_ptr<IMMDeviceEnumerator> enumerator;
		if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
			__uuidof(IMMDeviceEnumerator), enumerator.put_void())))
			return;

		while (!m_stop && !g_shuttingDown)
		{
			SyncNativeSessionVolume(checkedLeftoverMute);

			std::wstring targetOutputId;
			bool matchesDefault = true;
			ResolveTargetRenderDevice(enumerator.get(), targetOutputId, matchesDefault);
			const bool needsTakeover = IsBoostedVolume(g_playbackVolume.load()) || !matchesDefault;

			// In 0%-100% mode on the system default output, rely purely on the
			// native Windows playback session without opening any capture stream.
			if (!needsTakeover)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}

			winrt::com_ptr<IMMDevice> input;
			const std::wstring endpointGuid = FindCachedPhoneEndpointGuid(m_deviceId);
			if (!endpointGuid.empty())
			{
				const std::wstring captureId = L"{0.0.1.00000000}." + endpointGuid;
				if (SUCCEEDED(enumerator->GetDevice(captureId.c_str(), input.put())))
				{
					DWORD state = 0;
					if (FAILED(input->GetState(&state)) || state != DEVICE_STATE_ACTIVE)
						input = nullptr;
				}
				else
				{
					InvalidateCachedPhoneEndpointGuid(m_deviceId);
				}
			}

			if (!input)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}

			winrt::com_ptr<IAudioClient> captureClient;
			if (FAILED(input->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, captureClient.put_void())))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				continue;
			}

			WAVEFORMATEX* rawFormat = nullptr;
			if (FAILED(captureClient->GetMixFormat(&rawFormat)) || !rawFormat)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				continue;
			}
			std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(rawFormat, CoTaskMemFree);
			const SampleFormat sampleFormat = DetectSampleFormat(format.get());
			if (sampleFormat == SampleFormat::Unsupported)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				continue;
			}

			wil::unique_handle captureEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
			if (!captureEvent.get() ||
				FAILED(captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
					AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 2'000'000, 0, format.get(), nullptr)) ||
				FAILED(captureClient->SetEventHandle(captureEvent.get())))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				continue;
			}

			winrt::com_ptr<IAudioCaptureClient> capture;
			if (FAILED(captureClient->GetService(__uuidof(IAudioCaptureClient), capture.put_void())) ||
				FAILED(captureClient->Start()))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				continue;
			}

			winrt::com_ptr<IAudioClient> renderClient;
			winrt::com_ptr<IAudioRenderClient> render;
			std::wstring renderOutputId;
			UINT32 renderCapacity = 0;

			auto releaseBoostStream = [&] {
				if (renderClient)
				{
					renderClient->Stop();
					render = nullptr;
					renderClient = nullptr;
					renderOutputId.clear();
					renderCapacity = 0;
				}
				if (boostMuted)
				{
					const float restoreLevel = NativeSessionVolumeForSlider(g_playbackVolume.load());
					SetPhoneAudioSessionVolume(m_deviceId, restoreLevel);
					boostMuted = false;
					AppendLog(L"Phone audio boost ended; restored native session volume: " + m_deviceId);
				}
			};
			auto streamGuard = wil::scope_exit([&] {
				captureClient->Stop();
				releaseBoostStream();
			});

			auto lastAudibleTime = std::chrono::steady_clock::now();
			auto nextCheckTime = lastAudibleTime;

			while (!m_stop && !g_shuttingDown)
			{
				WaitForSingleObject(captureEvent.get(), 15);
				if (m_stop || g_shuttingDown)
					break;

				const auto now = std::chrono::steady_clock::now();
				const double currentSlider = g_playbackVolume.load();
				if (now >= nextCheckTime)
				{
					ResolveTargetRenderDevice(enumerator.get(), targetOutputId, matchesDefault);
				}
				const bool needsTakeoverNow = IsBoostedVolume(currentSlider) || !matchesDefault;
				if (!needsTakeoverNow)
				{
					// Returned to 0%-100% on system default output; exit capture
					// loop and let the outer loop manage the native session.
					break;
				}

				UINT32 packetFrames = 0;
				if (FAILED(capture->GetNextPacketSize(&packetFrames)))
					break;

				while (packetFrames > 0 && !m_stop && !g_shuttingDown)
				{
					BYTE* source = nullptr;
					UINT32 frames = 0;
					DWORD flags = 0;
					if (FAILED(capture->GetBuffer(&source, &frames, &flags, nullptr, nullptr)))
						break;

					float peak = 0.0f;
					if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && frames > 0 && source)
					{
						const size_t sampleCount = static_cast<size_t>(frames) * format->nChannels;
						peak = (sampleFormat == SampleFormat::Float32)
							? MeasurePeakMagnitude(reinterpret_cast<const float*>(source), sampleCount)
							: MeasurePeakMagnitudePcm16(reinterpret_cast<const int16_t*>(source), sampleCount);
					}
					const bool audible = peak > 1e-4f;
					if (audible)
						lastAudibleTime = now;

					if (!renderClient && audible)
					{
						ResolveTargetRenderDevice(enumerator.get(), targetOutputId, matchesDefault);
						OpenRenderStream(enumerator.get(), targetOutputId, format.get(),
							renderClient, render, renderOutputId, renderCapacity);
					}
					if (renderClient && !boostMuted && audible)
					{
						if (SetPhoneAudioSessionVolume(m_deviceId, 0.0f))
						{
							boostMuted = true;
							AppendLog(std::wstring(IsBoostedVolume(currentSlider) ?
								L"Phone audio boost takeover started at " :
								L"Phone audio output reroute started at ") +
								std::to_wstring(static_cast<int>(std::lround(currentSlider * 100.0))) +
								L"% -> " + renderOutputId + L": " + m_deviceId);
						}
					}
					if (renderClient && boostMuted && frames > 0)
					{
						UINT32 padding = 0;
						if (SUCCEEDED(renderClient->GetCurrentPadding(&padding)) && renderCapacity > padding)
						{
							const UINT32 writeFrames = (std::min)(frames, renderCapacity - padding);
							BYTE* destination = nullptr;
							if (writeFrames > 0 && SUCCEEDED(render->GetBuffer(writeFrames, &destination)))
							{
								if (audible)
								{
									std::memcpy(destination, source,
										static_cast<size_t>(writeFrames) * format->nBlockAlign);
									const size_t count = static_cast<size_t>(writeFrames) * format->nChannels;
									if (sampleFormat == SampleFormat::Float32)
										ProcessPhoneSamples(reinterpret_cast<float*>(destination), count, static_cast<float>(currentSlider));
									else
										ProcessPhoneSamplesPcm16(reinterpret_cast<int16_t*>(destination), count, static_cast<float>(currentSlider));
									render->ReleaseBuffer(writeFrames, 0);
								}
								else
								{
									render->ReleaseBuffer(writeFrames, AUDCLNT_BUFFERFLAGS_SILENT);
								}
							}
						}
					}

					capture->ReleaseBuffer(frames);
					if (FAILED(capture->GetNextPacketSize(&packetFrames)))
						break;
				}

				if (now >= nextCheckTime)
				{
					nextCheckTime = now + std::chrono::milliseconds(200);

					DWORD endpointState = 0;
					if (FAILED(input->GetState(&endpointState)) || endpointState != DEVICE_STATE_ACTIVE)
						break;

					if (boostMuted)
					{
						if (now - lastAudibleTime >= std::chrono::milliseconds(1500))
						{
							// Release boost takeover after 1.5s of silence/pause so the
							// native session is restored to 100% whenever idle.
							releaseBoostStream();
						}
						else
						{
							const bool outputChanged = !targetOutputId.empty() && !EqualsIgnoreCase(renderOutputId, targetOutputId);
							if (outputChanged)
							{
								AppendLog(L"Target audio output changed; rebinding renderer: " + m_deviceId);
								releaseBoostStream();
								continue;
							}
							SetPhoneAudioSessionVolume(m_deviceId, 0.0f);
						}
					}
					else
					{
						SyncNativeSessionVolume(checkedLeftoverMute);
					}
				}
			}
		}
	}

	std::wstring m_deviceId;
	uint64_t m_generation{};
	std::atomic_bool m_stop{ false };
	std::promise<void> m_stopped;
	std::shared_future<void> m_stoppedFuture;
};
