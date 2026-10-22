#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

inline float PlaybackGain(float slider) noexcept
{
	return 2.0f * std::clamp(slider, 0.0f, 1.0f);
}

inline void ProcessPhoneSamples(float* samples, size_t count, float slider) noexcept
{
	const float gain = PlaybackGain(slider);
	for (size_t i = 0; i < count; ++i)
	{
		float sample = samples[i] * gain;
		const float magnitude = std::abs(sample);
		if (gain > 1.0f && magnitude > 0.8f)
		{
			// A soft knee avoids the hard clipping of a straight 2x multiply.
			const float limited = 0.8f + 0.18f * std::tanh((magnitude - 0.8f) / 0.18f);
			sample = std::copysign(limited, sample);
		}
		samples[i] = sample;
	}
}
