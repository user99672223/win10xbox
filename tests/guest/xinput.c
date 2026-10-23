/* guest_xinput: XInput through the layer's own xinput1_4.dll (guest-dlls/xinput). A console program that imports
 * xinput1_4.dll through its import table (the import library of the xinput1_4 target) and reads the gamepad the
 * HOST holds. The host script (WIN10XBOX_TEST_INJECT_INPUT, host-win32/main.cpp) is
 *   pad 0 0 0 0 0 0 0;padwait 30000;sleep 300;pad 1000 1000 -2000 3000 -4000 128 255
 * i.e. it connects a neutral pad, waits until this program has read it, then presses A with stick and trigger values.
 *
 * Output lines (the integration test matches them):
 *   xinput: screen=1920x1080
 *   xinput: caps ok type=1 subtype=1
 *   xinput: idle packet=<n> buttons=0x0000
 *   xinput: packet changed <n> -> <m>
 *   xinput: buttons=0x1000 lx=1000 ly=-2000 rx=3000 ry=-4000 lt=128 rt=255
 *   xinput: users 1..3 not connected (1167 1167 1167)
 *   xinput: set state ok, battery type=1 level=3, bad index=160
 *   xinput: done
 * Exits 42 when everything held, 1..9 on the first thing that did not (the line before says which). */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>

#define WAIT_MS 30000

static int fail(int code, const char *what, DWORD err) {
    printf("xinput: FAIL %s (error %lu)\n", what, (unsigned long)err);
    fflush(stdout);
    return code;
}

int main(void) {
    XINPUT_CAPABILITIES caps;
    XINPUT_STATE st, first;
    XINPUT_VIBRATION vib;
    XINPUT_BATTERY_INFORMATION bat;
    DWORD err, t0, user;
    DWORD codes[3];

    /* A game imports user32 as well; the builtin-only machine folder test also wants to see Wine's user32 mapped. */
    printf("xinput: screen=%dx%d\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    fflush(stdout);

    /* the host connects the pad when its script starts: wait for it */
    t0 = GetTickCount();
    while ((err = XInputGetCapabilities(0, 0, &caps)) == ERROR_DEVICE_NOT_CONNECTED && GetTickCount() - t0 < WAIT_MS)
        Sleep(20);
    if (err != ERROR_SUCCESS) return fail(1, "XInputGetCapabilities(0)", err);
    if (caps.Type != XINPUT_DEVTYPE_GAMEPAD || caps.SubType != XINPUT_DEVSUBTYPE_GAMEPAD)
        return fail(2, "capabilities are not a gamepad", caps.Type);
    printf("xinput: caps ok type=%u subtype=%u\n", caps.Type, caps.SubType);

    err = XInputGetState(0, &first);
    if (err != ERROR_SUCCESS) return fail(3, "XInputGetState(0)", err);
    printf("xinput: idle packet=%lu buttons=0x%04x\n", (unsigned long)first.dwPacketNumber, first.Gamepad.wButtons);
    fflush(stdout);

    /* poll until the packet number moves: the host presses A after it has seen the read above */
    t0 = GetTickCount();
    do {
        Sleep(10);
        err = XInputGetState(0, &st);
    } while (err == ERROR_SUCCESS && st.dwPacketNumber == first.dwPacketNumber && GetTickCount() - t0 < WAIT_MS);
    if (err != ERROR_SUCCESS) return fail(4, "XInputGetState(0) while polling", err);
    if (st.dwPacketNumber == first.dwPacketNumber) return fail(5, "the packet number never changed", 0);
    printf("xinput: packet changed %lu -> %lu\n", (unsigned long)first.dwPacketNumber, (unsigned long)st.dwPacketNumber);
    printf("xinput: buttons=0x%04x lx=%d ly=%d rx=%d ry=%d lt=%u rt=%u\n", st.Gamepad.wButtons, st.Gamepad.sThumbLX,
           st.Gamepad.sThumbLY, st.Gamepad.sThumbRX, st.Gamepad.sThumbRY, st.Gamepad.bLeftTrigger, st.Gamepad.bRightTrigger);

    for (user = 1; user <= 3; user++) codes[user - 1] = XInputGetState(user, &st);
    printf("xinput: users 1..3 not connected (%lu %lu %lu)\n", (unsigned long)codes[0], (unsigned long)codes[1],
           (unsigned long)codes[2]);
    if (codes[0] != ERROR_DEVICE_NOT_CONNECTED || codes[1] != ERROR_DEVICE_NOT_CONNECTED ||
        codes[2] != ERROR_DEVICE_NOT_CONNECTED)
        return fail(6, "users 1..3 must be ERROR_DEVICE_NOT_CONNECTED", codes[0]);

    vib.wLeftMotorSpeed = 20000;
    vib.wRightMotorSpeed = 40000;
    err = XInputSetState(0, &vib);
    if (err != ERROR_SUCCESS) return fail(7, "XInputSetState(0)", err);
    err = XInputGetBatteryInformation(0, BATTERY_DEVTYPE_GAMEPAD, &bat);
    if (err != ERROR_SUCCESS) return fail(8, "XInputGetBatteryInformation(0)", err);
    err = XInputGetState(4, &st);
    printf("xinput: set state ok, battery type=%u level=%u, bad index=%lu\n", bat.BatteryType, bat.BatteryLevel,
           (unsigned long)err);
    if (err != ERROR_BAD_ARGUMENTS) return fail(9, "XInputGetState(4) must be ERROR_BAD_ARGUMENTS", err);

    printf("xinput: done\n");
    fflush(stdout);
    return 42;
}
