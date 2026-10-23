status: wip

# Slice: xinput

A guest's XInput through the layer: the layer's own `xinput1_4.dll` (also `xinput1_3.dll`, `xinput9_1_0.dll`;
`guest-dlls/xinput/xinput.c`, design in docs/GUI.md "Gamepad / XInput") over the "xinput1_4" unix library and the host's
gamepad store (`core/input/gamepad.cpp`). Gate: `integration.guest_xinput` (tests/guest/xinput.c) on the builtin-only
machine folder, fed by host-win32's inject script (`pad`, `padwait`).
