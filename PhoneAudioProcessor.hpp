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
		: m_deviceId(std::move(deviceId)), m_generation(generation) {}

	std::future<std::wstring> Start()
	{
		auto started = m_started.get_future();
		std::thread([self = shared_from_this()] { self->Run(); }).detach();
		return started;
	}

	void Stop() noexcept { m_stop = true; }

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
		bool started = false;
		bool muted = false;
		std::wstring_view stage = L"Initialize audio worker";
		try
		{
			Check(apartment);
			winrt::com_ptr<IMMDeviceEnumerator> enumerator;
			stage = L"Create device enumerator";
			Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
				__uuidof(IMMDeviceEnumerator), enumerator.put_void()));

			std::wstring endpointGuid;
			winrt::com_ptr<IMMDevice> input;
			HRESULT endpointError = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
			// Windows can report Opened before its A2DP capture endpoint is
			// published by MMDevAPI. Wait here, off the UI thread, without closing
			// the still-healthy Bluetooth connection.
			for (int attempt = 0; attempt < 80 && !m_stop && !g_shuttingDown; ++attempt)
			{
				stage = L"Find phone audio endpoint";
				if (endpointGuid.empty())
					FindPhoneEndpointGuid(m_deviceId, endpointGuid);
				if (!endpointGuid.empty())
				{
					stage = L"Open phone audio endpoint";
					const std::wstring captureId = L"{0.0.1.00000000}." + endpointGuid;
					endpointError = enumerator->GetDevice(captureId.c_str(), input.put());
					if (SUCCEEDED(endpointError))
						break;
					endpointGuid.clear();
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (!input)
				throw winrt::hresult_error(endpointError);
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
			m_started.set_value(std::wstring{});
			started = true;
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
				if (now >= nextSessionCheck)
				{
					std::lock_guard lock(g_volumeApplyMutex);
					if (!m_stop)
						muted = SetPhoneSessionVolume(m_deviceId, 0.0f, true, false) || muted;
					nextSessionCheck = now + std::chrono::milliseconds(500);
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
					HRESULT processing = S_OK;
					if (muted && !(flags & AUDCLNT_BUFFERFLAGS_SILENT))
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
			AppendLog(L"Phone audio processor failed: " + m_deviceId + L"; " + reason);
			if (!started)
				m_started.set_value(reason);
			else if (!m_stop && !g_shuttingDown)
				QueuePhoneAudioFailure(m_deviceId, m_generation, reason);
		}
		catch (...)
		{
			const std::wstring reason = std::wstring(stage) + L": unexpected failure";
			AppendLog(L"Phone audio processor failed: " + m_deviceId + L"; " + reason);
			if (!started)
				m_started.set_value(reason);
			else if (!m_stop && !g_shuttingDown)
				QueuePhoneAudioFailure(m_deviceId, m_generation, reason);
		}
		if (muted)
		{
			std::lock_guard lock(g_volumeApplyMutex);
			SetPhoneSessionVolume(m_deviceId, static_cast<float>(DEFAULT_PLAYBACK_VOLUME), false, true);
		}
		if (SUCCEEDED(apartment))
			CoUninitialize();
	}

	std::wstring m_deviceId;
	uint64_t m_generation{};
	std::atomic_bool m_stop{ false };
	std::promise<std::wstring> m_started;
};
