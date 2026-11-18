#include "../PhoneAudioGain.hpp"
#include "../BluetoothRecoveryPolicy.hpp"
#include <cassert>
#include <cmath>

int main()
{
	assert(IsRadioRecoveryCandidate(0x8007001Fu));
	assert(!IsRadioRecoveryCandidate(0x800705B4u));
	assert(!IsRadioRecoveryCandidate(0x00000000u));
	assert(!CanReconnectAfterRadioCycle(false, true));
	assert(!CanReconnectAfterRadioCycle(true, false));
	assert(CanReconnectAfterRadioCycle(true, true));

	// 0-100% uses native session volume; 101-200% engages the boosted stream.
	assert(!IsBoostedVolume(0.0));
	assert(!IsBoostedVolume(0.5));
	assert(!IsBoostedVolume(1.0));
	assert(IsBoostedVolume(1.01));
	assert(IsBoostedVolume(2.0));

	assert(NativeSessionVolumeForSlider(0.0) == 0.0f);
	assert(NativeSessionVolumeForSlider(0.5) == 0.5f);
	assert(NativeSessionVolumeForSlider(1.0) == 1.0f);
	assert(NativeSessionVolumeForSlider(2.0) == 1.0f);

	assert(PlaybackGain(0.0f) == 0.0f);
	assert(PlaybackGain(1.0f) == 1.0f);
	assert(PlaybackGain(1.5f) == 1.5f);
	assert(PlaybackGain(2.0f) == 2.0f);
	assert(PlaybackGain(3.0f) == 2.0f);

	float quiet[] = { -0.1f, 0.1f, 0.0f };
	ProcessPhoneSamples(quiet, 3, 2.0f);
	assert(std::abs(quiet[0] + 0.2f) < 0.0001f);
	assert(std::abs(quiet[1] - 0.2f) < 0.0001f);
	assert(quiet[2] == 0.0f);

	float normal[] = { -0.95f, 0.95f };
	ProcessPhoneSamples(normal, 2, 1.0f);
	assert(normal[0] == -0.95f && normal[1] == 0.95f);

	float loud[] = { -0.9f, 0.9f, 0.0f };
	ProcessPhoneSamples(loud, 3, 2.0f);
	assert(loud[0] > -1.0f && loud[1] < 1.0f);
	assert(loud[0] < -0.8f && loud[1] > 0.8f);
	assert(loud[2] == 0.0f);

	int16_t pcmQuiet[] = { -3276, 3276, 0 };
	ProcessPhoneSamplesPcm16(pcmQuiet, 3, 2.0f);
	assert(pcmQuiet[0] <= -6500 && pcmQuiet[0] >= -6600);
	assert(pcmQuiet[1] >= 6500 && pcmQuiet[1] <= 6600);
	assert(pcmQuiet[2] == 0);
}
