#include "../BluetoothRecoveryPolicy.hpp"
#include <cassert>

int main()
{
	assert(IsRadioRecoveryCandidate(0x8007001Fu));
	assert(!IsRadioRecoveryCandidate(0x800705B4u));
	assert(!IsRadioRecoveryCandidate(0x00000000u));
	assert(!CanReconnectAfterRadioCycle(false, true));
	assert(!CanReconnectAfterRadioCycle(true, false));
	assert(CanReconnectAfterRadioCycle(true, true));
}
