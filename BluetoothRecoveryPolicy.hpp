#pragma once

#include <cstdint>

constexpr bool IsRadioRecoveryCandidate(uint32_t errorCode) noexcept
{
	return errorCode == 0x8007001Fu;
}

constexpr bool CanReconnectAfterRadioCycle(bool observedOff, bool observedOn) noexcept
{
	return observedOff && observedOn;
}
