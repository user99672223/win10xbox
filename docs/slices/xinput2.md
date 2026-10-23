status: in-progress

# Slice: xinput2

Handoff (10 lines): xinput1_4.dll now exports the table below (Wine's xinput1_4.spec, which mirrors the real DLL:
`objdump -p` of Wine's own PE xinput1_4 was checked; ordinals 100 and 108 are NONAME as on Windows); added
XInputGetAudioDeviceIds (10, always 1167) and XInputGetCapabilitiesEx (108). tests/guest/xinput.c also covers ordinal 100
by GetProcAddress, users 1..3 for SetState/Battery/Keystroke/Ex (1167), XInputEnable, wait 30 s -> 10 s.
`integration.guest_xinput_native_d3d` runs the guest on machine-native-d3d and asserts from the +modules trace that
xinput1_4.dll mapped from Windows\builtin and never from a host System32 (native Windows builds only, like
guest_d3d11). `unit.xinput_exports` (tests/unit/check_dll_exports.cmake) compares the export table to the lines below.
Unproven: the Windows jobs; ordinals 101-104 (WaitForGuideButton etc.) of the real Windows 10 DLL are not exported (not in
Wine's table, no Windows DLL here to confirm). The console (host-uwp) still has to prove two pads, rumble, guide button.

Expected exports (`export: <ordinal> <name|->`, - = by ordinal only):

export: 1 DllMain
export: 2 XInputGetState
export: 3 XInputSetState
export: 4 XInputGetCapabilities
export: 5 XInputEnable
export: 7 XInputGetBatteryInformation
export: 8 XInputGetKeystroke
export: 10 XInputGetAudioDeviceIds
export: 100 -
export: 108 -
