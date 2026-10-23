status: ready

# Slice: xinput

A guest's XInput through the layer: the layer's own `xinput1_4.dll` (also staged as `xinput1_3.dll` and
`xinput9_1_0.dll`; `guest-dlls/xinput/xinput.c`, design in docs/GUI.md "Gamepad / XInput") over a registered
"xinput1_4" unix library and the host's gamepad store (`core/input/gamepad.cpp`). Wine's own xinput1_4 is PE code over
hid/setupapi and a HID bus driver we do not have, so it is not used.

CI run (head 7aaf177: linux-cross, windows (Debug), windows (RelWithDebInfo) green, nothing skipped; host-uwp,
host-win32-smoke, host-win32-appcontainer and uwp-lite green too):
https://github.com/user99672223/win10xbox/actions/runs/39892440055 (ci), host-uwp
https://github.com/user99672223/win10xbox/actions/runs/39892440089. The head after it only merges main (14 slices) and
adds this file; its runs are named in the report.

What runs: `integration.guest_xinput` (tests/guest/xinput.c, builtin-only machine folder, exit 42). The guest imports
`xinput1_4.dll` through its import table; host-win32 runs `WIN10XBOX_TEST_INJECT_INPUT=pad 0 0 0 0 0 0 0;padwait
30000;sleep 300;pad 1000 1000 -2000 3000 -4000 128 255` (connect a neutral pad, wait until the guest has read it, press
A with stick and trigger values). Printed by the guest through the layer (windows Debug job):

    xinput: screen=1920x1080
    xinput: caps ok type=1 subtype=1
    xinput: idle packet=1 buttons=0x0000
    xinput: packet changed 1 -> 2
    xinput: buttons=0x1000 lx=1000 ly=-2000 rx=3000 ry=-4000 lt=128 rt=255
    xinput: users 1..3 not connected (1167 1167 1167)
    xinput: set state ok, battery type=1 level=3, bad index=160
    xinput: done

Added:

* Unix library `xinput1_4` (core/dispatch/unix_calls.cpp, ABI core/include/win10xbox/xinput_abi.h): `get_state`,
  `set_state`, `get_caps`. No syscall added or changed (`NtQueryVirtualMemory` MemoryWineLoadUnixLibByName and
  `__wine_unix_call_dispatcher` already existed).
* Host gamepad store `win10xbox::input::{Set,Get}GamepadState`, vibration, `GamepadPolls` (core/input/gamepad.cpp,
  core/include/win10xbox/gamepad.h); host-win32 inject commands `pad` and `padwait`; host-uwp `Launcher::PollGamepad`
  feeds pad 0 from Windows.Gaming.Input (buttons, sticks, triggers) and hands the guest's `XInputSetState` to the pad's
  Vibration (not exercised: no pad on a runner).
* DLLs of our own (not Wine's, so nothing in wine-pe/dlls.txt): `xinput1_4.dll`, `xinput1_3.dll`, `xinput9_1_0.dll`,
  one binary under three names, exports XInputGetState/SetState/GetCapabilities/Enable/GetBatteryInformation/
  GetKeystroke and XInputGetStateEx (ordinal 100). Imports kernel32 only (MSVC /MT). Staged into `Windows/builtin` of
  both machine folders (tests/integration/prepare_machine.cmake) and the host-uwp package's `payload/builtin`; the three
  names are always builtin (core/vfs/vfs.cpp, unit test in test_vfs.cpp).

What still fails or is not done: `XInputGetKeystroke` reports no keystrokes (ERROR_EMPTY, the host keeps no event
queue); users 1..3 are never connected; the battery is reported as wired and full; `Windows.Gaming.Input` for guests
(WinRT) and a keyboard-to-pad mapping are not done; XInputGetDSoundAudioDeviceGuids and the other undocumented ordinals
are not exported.
