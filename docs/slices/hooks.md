status: ready

# Slice: hooks

In-process `SetWindowsHookEx` (WH_KEYBOARD, WH_MOUSE, WH_GETMESSAGE, WH_CALLWNDPROC, WH_CALLWNDPROCRET, WH_MSGFILTER,
WH_SYSMSGFILTER) and `SetTimer` precision, in `core/user` (`win_hook.cpp`, call-outs in `win_message.cpp`). Design and
the list of what is not implemented: docs/GUI.md item 9a.

Gate: `integration.guest_window_hooks` (tests/guest/window_hooks.c, builtin-only machine folder, host-injected input
`sleep 2000;move 150,150;ldown;lup;key 0x41;rdown;rup;key 0x42`), linux-cross and both MSVC configs, and the package
check of host-uwp.yml.

Syscalls added (Todo -> Emulate): NtUserSetWindowsHookEx, NtUserUnhookWindowsHookEx, NtUserUnhookWindowsHook,
NtUserCallNextHookEx, NtUserCallMsgFilter. No DLLs added to wine-pe/dlls.txt.

CI run: https://github.com/user99672223/win10xbox/actions/runs/39930010818 (ci.yml green: linux-cross, windows Debug,
windows RelWithDebInfo, nothing skipped; head 515ab6d). host-uwp.yml green on the same head:
https://github.com/user99672223/win10xbox/actions/runs/39930010838 (guest_window_hooks.exe is in HOST_UWP_GUESTS, the
GUI foreach and the package check list).

What runs through the layer: `integration.guest_window_hooks` (C:\guest\guest_window_hooks.exe, exit 42), first
output lines from the windows (Debug) job's ctest-junit.xml:

    hooks: installed 0000000000010001 0000000000010002 0000000000010003 0000000000010004
    hooks: PASS injected input arrived
    hooks: PASS WH_MOUSE sees LBUTTONDOWN before the window procedure
    hooks: PASS WH_KEYBOARD sees vk 41 down and up
    hooks: timer 50 ms x20: total 1000 ms, shortest interval 49.7 ms
    hooks: PASS SetTimer 50 ms cadence
    hooks: 12 passed, 0 failed

Timer tolerance (documented in docs/GUI.md 9a): 20 ticks of a 50 ms timer take 950..1400 ms, no gap under 40 ms.

The merge with main removed the stub NtUserCallMsgFilter of win_window.cpp (win_hook.cpp implements it) and three
stray list lines in host-uwp/CMakeLists.txt that main carried.

Still failing / not done: WH_KEYBOARD_LL, WH_MOUSE_LL, WH_CBT, WH_SHELL, WH_DEBUG, journal hooks, WinEvent hooks and
hooks in other processes (docs/GUI.md 9a); WH_CALLWNDPROC cannot modify the message.
