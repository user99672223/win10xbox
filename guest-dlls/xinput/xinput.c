/* xinput1_4.dll (also staged as xinput1_3.dll and xinput9_1_0.dll): the layer's own XInput for guests.
 *
 * Wine's xinput1_4 is PE code over hid/setupapi and a HID bus driver this layer does not have, so this is a small
 * DLL of our own. It holds no state: every call goes to the host's gamepad store through the "xinput1_4" unix
 * library (core/dispatch/unix_calls.cpp, ABI in core/include/win10xbox/xinput_abi.h), reached the way Wine's PE DLLs
 * reach their unix side: ntdll's __wine_unix_call_dispatcher, with the handle that NtQueryVirtualMemory
 * (MemoryWineLoadUnixLibByName) gives for the library name. Only kernel32 is imported; ntdll is reached with
 * GetProcAddress.
 *
 * User index 0 is the pad the host holds; 1..3 are never connected. XInputGetKeystroke reports no keystrokes (the
 * host keeps no event queue). The battery is reported as a wired device. */
#include <windows.h>
#include "win10xbox/xinput_abi.h"

#define W10X_MEMORY_WINE_LOAD_UNIX_LIB_BY_NAME 1002

typedef struct {
    USHORT Length, MaximumLength;
    PWSTR Buffer;
} W10X_UNICODE_STRING;

typedef LONG(WINAPI *NtQueryVirtualMemoryFn)(HANDLE, const void *, int, void *, SIZE_T, SIZE_T *);
typedef LONG(WINAPI *UnixCallFn)(unsigned long long handle, unsigned int code, void *args);

typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} PadGamepad;
typedef struct {
    DWORD dwPacketNumber;
    PadGamepad Gamepad;
} PadState;
typedef struct {
    WORD wLeftMotorSpeed, wRightMotorSpeed;
} PadVibration;
typedef struct {
    BYTE Type, SubType;
    WORD Flags;
    PadGamepad Gamepad;
    PadVibration Vibration;
} PadCapabilities;
typedef struct {
    BYTE BatteryType, BatteryLevel;
} PadBattery;
typedef struct {
    WORD VirtualKey;
    WCHAR Unicode;
    WORD Flags;
    BYTE UserIndex, HidCode;
} PadKeystroke;

#define PAD_ERROR_EMPTY 4306
#define PAD_USER_INDEX_ANY 0x000000FF
#define PAD_FLAG_GAMEPAD 0x01
#define PAD_BATTERY_TYPE_WIRED 1
#define PAD_BATTERY_LEVEL_FULL 3

static UnixCallFn g_unix_call;
static unsigned long long g_unix_handle;
static volatile LONG g_enabled = 1;

static DWORD unix_call(unsigned int code, void *args) {
    if (!g_unix_call || !g_unix_handle) return ERROR_DEVICE_NOT_CONNECTED;
    return (DWORD)g_unix_call(g_unix_handle, code, args);
}

/* __wine_init_unix_call, by hand: the dispatcher pointer is a data export of ntdll (the export is the address of
 * the pointer), the handle comes from NtQueryVirtualMemory. */
static void init_unix_call(void) {
    static const WCHAR name[] = L"xinput1_4";
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    NtQueryVirtualMemoryFn query;
    UnixCallFn *dispatcher;
    W10X_UNICODE_STRING us;
    unsigned long long handle = 0;
    if (!ntdll) return;
    query = (NtQueryVirtualMemoryFn)(void *)GetProcAddress(ntdll, "NtQueryVirtualMemory");
    dispatcher = (UnixCallFn *)(void *)GetProcAddress(ntdll, (LPCSTR)(ULONG_PTR)1455 /* unix call dispatcher: exported by ordinal only */);
    if (!query || !dispatcher || !*dispatcher) return;
    us.Length = sizeof(name) - sizeof(WCHAR);
    us.MaximumLength = sizeof(name);
    us.Buffer = (PWSTR)name;
    if (query(GetCurrentProcess(), &us, W10X_MEMORY_WINE_LOAD_UNIX_LIB_BY_NAME, &handle, sizeof(handle), NULL) < 0)
        return;
    g_unix_call = *dispatcher;
    g_unix_handle = handle;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        init_unix_call();
    }
    return TRUE;
}

static DWORD get_state(DWORD user, PadState *state) {
    W10xXInputGetState p;
    DWORD err;
    if (!state) return ERROR_BAD_ARGUMENTS;
    if (user >= W10X_XINPUT_MAX_PADS) return ERROR_BAD_ARGUMENTS;
    p.index = user;
    err = unix_call(W10X_XINPUT_GET_STATE, &p);
    if (err) return err;
    state->dwPacketNumber = p.state.packet;
    if (g_enabled) {
        state->Gamepad.wButtons = p.state.buttons;
        state->Gamepad.bLeftTrigger = p.state.left_trigger;
        state->Gamepad.bRightTrigger = p.state.right_trigger;
        state->Gamepad.sThumbLX = p.state.thumb_lx;
        state->Gamepad.sThumbLY = p.state.thumb_ly;
        state->Gamepad.sThumbRX = p.state.thumb_rx;
        state->Gamepad.sThumbRY = p.state.thumb_ry;
    } else { /* XInputEnable(FALSE): the pad reports neutral input */
        state->Gamepad.wButtons = 0;
        state->Gamepad.bLeftTrigger = state->Gamepad.bRightTrigger = 0;
        state->Gamepad.sThumbLX = state->Gamepad.sThumbLY = state->Gamepad.sThumbRX = state->Gamepad.sThumbRY = 0;
    }
    return ERROR_SUCCESS;
}

DWORD WINAPI XInputGetState(DWORD user, PadState *state) { return get_state(user, state); }

/* ordinal 100: the undocumented variant that also reports the Guide button; the host has no such button */
DWORD WINAPI XInputGetStateEx(DWORD user, PadState *state) { return get_state(user, state); }

/* XInputGetAudioDeviceIds (ordinal 10): the layer has no pad headset; a connected pad has no audio device, none
 * is ERROR_DEVICE_NOT_CONNECTED. */
DWORD WINAPI XInputGetAudioDeviceIds(DWORD user, LPWSTR render, UINT *render_count, LPWSTR capture, UINT *capture_count) {
    (void)render;
    (void)capture;
    if (user >= W10X_XINPUT_MAX_PADS) return ERROR_BAD_ARGUMENTS;
    if (render_count) *render_count = 0;
    if (capture_count) *capture_count = 0;
    return ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputSetState(DWORD user, PadVibration *vibration) {
    W10xXInputSetState p;
    if (!vibration) return ERROR_BAD_ARGUMENTS;
    if (user >= W10X_XINPUT_MAX_PADS) return ERROR_BAD_ARGUMENTS;
    p.index = user;
    p.left_motor = g_enabled ? vibration->wLeftMotorSpeed : 0;
    p.right_motor = g_enabled ? vibration->wRightMotorSpeed : 0;
    return unix_call(W10X_XINPUT_SET_STATE, &p);
}

DWORD WINAPI XInputGetCapabilities(DWORD user, DWORD flags, PadCapabilities *caps) {
    W10xXInputGetCaps p;
    DWORD err;
    if (!caps) return ERROR_BAD_ARGUMENTS;
    if (user >= W10X_XINPUT_MAX_PADS || (flags & ~(DWORD)PAD_FLAG_GAMEPAD)) return ERROR_BAD_ARGUMENTS;
    p.index = user;
    err = unix_call(W10X_XINPUT_GET_CAPS, &p);
    if (err) return err;
    caps->Type = p.type;
    caps->SubType = p.subtype;
    caps->Flags = p.flags;
    caps->Gamepad.wButtons = 0xF3FF; /* every XINPUT_GAMEPAD_* button the 1.4 layout has */
    caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
    caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)-64;
    caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 0xFF;
    return ERROR_SUCCESS;
}

/* ordinal 108 (XINPUT_CAPABILITIES_EX: the capabilities, then vendor/product ids); version must be 1 */
typedef struct {
    PadCapabilities Capabilities;
    WORD VendorId, ProductId, VersionNumber, unk1;
    DWORD unk2;
} PadCapabilitiesEx;
DWORD WINAPI XInputGetCapabilitiesEx(DWORD version, DWORD user, DWORD flags, PadCapabilitiesEx *caps) {
    DWORD err;
    if (!caps || version != 1) return ERROR_BAD_ARGUMENTS;
    err = XInputGetCapabilities(user, flags, &caps->Capabilities);
    if (err) return err;
    caps->VendorId = 0x045E; /* Microsoft */
    caps->ProductId = 0x028E; /* Xbox 360 controller */
    caps->VersionNumber = 0x0114;
    caps->unk1 = 0;
    caps->unk2 = 0;
    return ERROR_SUCCESS;
}

void WINAPI XInputEnable(BOOL enable) {
    int i;
    InterlockedExchange(&g_enabled, enable ? 1 : 0);
    if (!enable) { /* disabling stops the motors */
        for (i = 0; i < W10X_XINPUT_MAX_PADS; i++) {
            W10xXInputSetState p;
            p.index = (uint32_t)i;
            p.left_motor = p.right_motor = 0;
            unix_call(W10X_XINPUT_SET_STATE, &p);
        }
    }
}

DWORD WINAPI XInputGetBatteryInformation(DWORD user, BYTE devtype, PadBattery *battery) {
    W10xXInputGetCaps p;
    DWORD err;
    (void)devtype;
    if (!battery) return ERROR_BAD_ARGUMENTS;
    if (user >= W10X_XINPUT_MAX_PADS) return ERROR_BAD_ARGUMENTS;
    p.index = user;
    err = unix_call(W10X_XINPUT_GET_CAPS, &p);
    if (err) return err;
    battery->BatteryType = PAD_BATTERY_TYPE_WIRED;
    battery->BatteryLevel = PAD_BATTERY_LEVEL_FULL;
    return ERROR_SUCCESS;
}

DWORD WINAPI XInputGetKeystroke(DWORD user, DWORD reserved, PadKeystroke *keystroke) {
    W10xXInputGetCaps p;
    DWORD err;
    (void)reserved;
    if (!keystroke) return ERROR_BAD_ARGUMENTS;
    if (user >= W10X_XINPUT_MAX_PADS && user != PAD_USER_INDEX_ANY) return ERROR_BAD_ARGUMENTS;
    p.index = user == PAD_USER_INDEX_ANY ? 0 : user;
    err = unix_call(W10X_XINPUT_GET_CAPS, &p);
    if (err) return err;
    return PAD_ERROR_EMPTY;
}
