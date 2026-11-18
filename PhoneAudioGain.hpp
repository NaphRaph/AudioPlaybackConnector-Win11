#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

constexpr double MAX_PLAYBACK_VOLUME = 2.0;

constexpr bool IsBoostedVolume(double volume) noexcept
{
	return volume > 1.005;
}

constexpr float NativeSessionVolumeForSlider(double volume) noexcept
{
	return static_cast<float>(std::clamp(volume, 0.0, 1.0));
}

inline float PlaybackGain(float sliderGain) noexcept
{
	return std::clamp(sliderGain, 0.0f, static_cast<float>(MAX_PLAYBACK_VOLUME));
}

inline float MeasurePeakMagnitude(const float* samples, size_t count) noexcept
{
	float peak = 0.0f;
	for (size_t i = 0; i < count; ++i)
	{
		const float magnitude = std::abs(samples[i]);
		if (magnitude > peak)
			peak = magnitude;
	}
	return peak;
}

inline float MeasurePeakMagnitudePcm16(const int16_t* samples, size_t count) noexcept
{
	float peak = 0.0f;
	for (size_t i = 0; i < count; ++i)
	{
		const float normalized = std::abs(static_cast<float>(samples[i]) / 32768.0f);
		if (normalized > peak)
			peak = normalized;
	}
	return peak;
}

inline float SoftLimitSample(float sample, float gain) noexcept
{
	const float magnitude = std::abs(sample);
	if (gain > 1.0f && magnitude > 0.8f)
	{
		// A soft knee avoids the hard clipping of a straight 2x multiply.
		const float limited = 0.8f + 0.18f * std::tanh((magnitude - 0.8f) / 0.18f);
		sample = std::copysign(limited, sample);
	}
	return std::clamp(sample, -1.0f, 1.0f);
}

inline void ProcessPhoneSamples(float* samples, size_t count, float sliderGain) noexcept
{
	const float gain = PlaybackGain(sliderGain);
	for (size_t i = 0; i < count; ++i)
	{
		samples[i] = SoftLimitSample(samples[i] * gain, gain);
	}
}

inline void ProcessPhoneSamplesPcm16(int16_t* samples, size_t count, float sliderGain) noexcept
{
	const float gain = PlaybackGain(sliderGain);
	for (size_t i = 0; i < count; ++i)
	{
		const float normalized = static_cast<float>(samples[i]) / 32768.0f;
		const float processed = SoftLimitSample(normalized * gain, gain);
		samples[i] = static_cast<int16_t>(std::clamp(std::lround(processed * 32767.0f), -32768l, 32767l));
	}
}
