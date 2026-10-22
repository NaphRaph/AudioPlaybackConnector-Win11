#include "../PhoneAudioGain.hpp"
#include <cassert>
#include <cmath>

int main()
{
	// The midpoint preserves the captured phone signal; the upper half must not saturate at unity.
	assert(PlaybackGain(0.0f) == 0.0f);
	assert(PlaybackGain(0.25f) == 0.5f);
	assert(PlaybackGain(0.5f) == 1.0f);
	assert(PlaybackGain(0.75f) == 1.5f);
	assert(PlaybackGain(1.0f) == 2.0f);

	float quiet[] = { -0.1f, 0.1f, 0.0f };
	ProcessPhoneSamples(quiet, 3, 1.0f);
	assert(std::abs(quiet[0] + 0.2f) < 0.0001f);
	assert(std::abs(quiet[1] - 0.2f) < 0.0001f);
	assert(quiet[2] == 0.0f);

	float normal[] = { -0.95f, 0.95f };
	ProcessPhoneSamples(normal, 2, 0.5f);
	assert(normal[0] == -0.95f && normal[1] == 0.95f);

	float loud[] = { -0.9f, 0.9f, 0.0f };
	ProcessPhoneSamples(loud, 3, 1.0f);
	assert(loud[0] > -1.0f && loud[1] < 1.0f);
	assert(loud[0] < -0.8f && loud[1] > 0.8f);
	assert(loud[2] == 0.0f);
}
