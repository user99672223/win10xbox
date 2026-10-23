status: wip

# Slice: hooks

In-process `SetWindowsHookEx` (WH_KEYBOARD, WH_MOUSE, WH_GETMESSAGE, WH_CALLWNDPROC, WH_CALLWNDPROCRET, WH_MSGFILTER,
WH_SYSMSGFILTER) and `SetTimer` precision, in `core/user` (`win_hook.cpp`, call-outs in `win_message.cpp`). Design and
the list of what is not implemented: docs/GUI.md item 9a.

Gate: `integration.guest_window_hooks` (tests/guest/window_hooks.c, builtin-only machine folder, host-injected input
`sleep 2000;move 150,150;ldown;lup;key 0x41;rdown;rup;key 0x42`), linux-cross and both MSVC configs, and the package
check of host-uwp.yml.

Syscalls added (Todo -> Emulate): NtUserSetWindowsHookEx, NtUserUnhookWindowsHookEx, NtUserUnhookWindowsHook,
NtUserCallNextHookEx, NtUserCallMsgFilter. No DLLs added to wine-pe/dlls.txt.

CI run: (pending)
