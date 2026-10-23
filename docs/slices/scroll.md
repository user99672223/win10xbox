status: wip

# Slice scroll: scroll bars in core/user

Code: `core/user/win_scroll.cpp` (+ `win_scroll.h`), hooks in `win_defwnd.cpp` (WM_NCHITTEST, WM_NCPAINT,
WM_NCLBUTTONDOWN), `win_message.cpp` (ScrollBar class procedure), `win_window.cpp` (GetScrollInfo through
`NtUserCallHwndParam`). `ScrollWindowEx` was already in `win_paint.cpp`. Gate: `tests/guest/window_scroll.c` as
`integration.guest_window_scroll` on the builtin-only machine folder.

History: the first run (39848913871) failed on both Windows jobs because the test was registered on the full
machine folder (docs/GUI.md rule 8): the guest died with 0x80000100 on `api-ms-win-core-crt-l2-1-0!_initterm_e`
in native ntdll before reaching main. Fix: registered with the other GUI tests under
`_guest_machine = WIN10XBOX_BUILTIN_MACHINE_DIR`.
