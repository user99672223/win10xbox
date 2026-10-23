/* The ABI between the layer's own xinput1_4.dll (guest-dlls/xinput) and the host: the "xinput1_4" unix library
 * (core/dispatch/unix_calls.cpp) that the DLL reaches through ntdll's __wine_unix_call_dispatcher, and the host's
 * gamepad store (win10xbox/gamepad.h). Plain C: included by the guest DLL (MSVC / llvm-mingw, C) and by the core.
 *
 * Every unix call returns a Win32 error code (0 = ERROR_SUCCESS, 1167 = ERROR_DEVICE_NOT_CONNECTED,
 * 87 = ERROR_INVALID_PARAMETER) and takes a pointer to one of the structs below. */
#ifndef WIN10XBOX_XINPUT_ABI_H
#define WIN10XBOX_XINPUT_ABI_H

#include <stdint.h>

#define W10X_XINPUT_UNIXLIB_NAME L"xinput1_4"   /* MemoryWineLoadUnixLibByName key */
#define W10X_XINPUT_MAX_PADS 4

enum {
    W10X_XINPUT_GET_STATE = 0,   /* W10xXInputGetState */
    W10X_XINPUT_SET_STATE = 1,   /* W10xXInputSetState */
    W10X_XINPUT_GET_CAPS = 2     /* W10xXInputGetCaps */
};

/* The first 16 bytes of the guest's XINPUT_STATE, field for field (dwPacketNumber, then XINPUT_GAMEPAD). */
typedef struct W10xPadState {
    uint32_t packet;
    uint16_t buttons;          /* XINPUT_GAMEPAD_* bits */
    uint8_t left_trigger;
    uint8_t right_trigger;
    int16_t thumb_lx, thumb_ly, thumb_rx, thumb_ry;
} W10xPadState;

typedef struct W10xXInputGetState {
    uint32_t index;            /* in */
    W10xPadState state;        /* out */
} W10xXInputGetState;

typedef struct W10xXInputSetState {
    uint32_t index;            /* in */
    uint16_t left_motor, right_motor;
} W10xXInputSetState;

typedef struct W10xXInputGetCaps {
    uint32_t index;            /* in */
    uint8_t type;              /* out: XINPUT_DEVTYPE_GAMEPAD */
    uint8_t subtype;           /* out: XINPUT_DEVSUBTYPE_GAMEPAD */
    uint16_t flags;            /* out: XINPUT_CAPS_* */
} W10xXInputGetCaps;

#endif
