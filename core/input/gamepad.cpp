// The host's gamepad store, see win10xbox/gamepad.h. One SRWLOCK (constant-initialised, no function-local statics)
// guards four pads; a guest XInputGetState call is a short critical section.
#include "win10xbox/gamepad.h"
#include <windows.h>

namespace win10xbox::input {
namespace {

struct Pad {
    bool connected = false;
    W10xPadState state{};
    std::uint16_t left_motor = 0, right_motor = 0;
};

SRWLOCK g_lock = SRWLOCK_INIT;
Pad g_pads[W10X_XINPUT_MAX_PADS];
std::uint64_t g_polls = 0;

bool SameInput(const W10xPadState& a, const W10xPadState& b) {
    return a.buttons == b.buttons && a.left_trigger == b.left_trigger && a.right_trigger == b.right_trigger &&
           a.thumb_lx == b.thumb_lx && a.thumb_ly == b.thumb_ly && a.thumb_rx == b.thumb_rx && a.thumb_ry == b.thumb_ry;
}

}  // namespace

bool SetGamepadState(unsigned index, const W10xPadState& state) {
    if (index >= W10X_XINPUT_MAX_PADS) return false;
    AcquireSRWLockExclusive(&g_lock);
    Pad& p = g_pads[index];
    if (!p.connected || !SameInput(p.state, state)) {
        const std::uint32_t packet = p.state.packet + 1;
        p.state = state;
        p.state.packet = packet;
    }
    p.connected = true;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

void DisconnectGamepad(unsigned index) {
    if (index >= W10X_XINPUT_MAX_PADS) return;
    AcquireSRWLockExclusive(&g_lock);
    Pad& p = g_pads[index];
    if (p.connected) {
        const std::uint32_t packet = p.state.packet;
        p = Pad();
        p.state.packet = packet;   // a pad that comes back continues the numbering
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool GetGamepadState(unsigned index, W10xPadState* out) {
    if (index >= W10X_XINPUT_MAX_PADS) return false;
    AcquireSRWLockExclusive(&g_lock);
    const Pad& p = g_pads[index];
    const bool ok = p.connected;
    if (ok) {
        *out = p.state;
        ++g_polls;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

bool GamepadConnected(unsigned index) {
    if (index >= W10X_XINPUT_MAX_PADS) return false;
    AcquireSRWLockExclusive(&g_lock);
    const bool ok = g_pads[index].connected;
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

bool GetGamepadVibration(unsigned index, std::uint16_t* left, std::uint16_t* right) {
    if (index >= W10X_XINPUT_MAX_PADS) return false;
    AcquireSRWLockExclusive(&g_lock);
    *left = g_pads[index].left_motor;
    *right = g_pads[index].right_motor;
    const bool ok = g_pads[index].connected;
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

bool SetGamepadVibration(unsigned index, std::uint16_t left, std::uint16_t right) {
    if (index >= W10X_XINPUT_MAX_PADS) return false;
    AcquireSRWLockExclusive(&g_lock);
    const bool ok = g_pads[index].connected;
    if (ok) {
        g_pads[index].left_motor = left;
        g_pads[index].right_motor = right;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

std::uint64_t GamepadPolls() {
    AcquireSRWLockExclusive(&g_lock);
    const std::uint64_t n = g_polls;
    ReleaseSRWLockExclusive(&g_lock);
    return n;
}

}  // namespace win10xbox::input
