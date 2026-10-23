// The host's gamepad state, process-wide: what the guest's XInput (guest-dlls/xinput, through the "xinput1_4" unix
// library) reads. Hosts write it: host-win32 from its WIN10XBOX_TEST_INJECT_INPUT script ("pad" command),
// host-uwp from Windows.Gaming.Input. Pad 0 is the first pad; a pad is connected from its first SetGamepadState
// until DisconnectGamepad. Thread-safe.
#pragma once
#include <cstdint>
#include "win10xbox/xinput_abi.h"

namespace win10xbox::input {

// Stores the pad's state and marks it connected. `state.packet` is ignored: the packet number is the store's own and
// moves only when the state differs from the stored one (or the pad was not connected). False for a bad index.
bool SetGamepadState(unsigned index, const W10xPadState& state);
void DisconnectGamepad(unsigned index);

// False when the pad is not connected (or the index is bad); `out` is then untouched.
bool GetGamepadState(unsigned index, W10xPadState* out);
bool GamepadConnected(unsigned index);

// What the guest asked for with XInputSetState (0..65535 per motor); zero until it asks.
bool GetGamepadVibration(unsigned index, std::uint16_t* left, std::uint16_t* right);
bool SetGamepadVibration(unsigned index, std::uint16_t left, std::uint16_t right);

// Number of GetGamepadState calls that found a connected pad (tests: a host script waits for the guest to poll).
std::uint64_t GamepadPolls();

}  // namespace win10xbox::input
