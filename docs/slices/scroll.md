status: ready

# Slice scroll: scroll bars in core/user

CI: https://github.com/user99672223/win10xbox/actions/runs/39884517479 (head c7952b4: linux-cross, windows (Debug),
windows (RelWithDebInfo) green, "Gate - nothing skipped" passed, `integration.guest_window_scroll` Passed on 48/48).
host-uwp (https://github.com/user99672223/win10xbox/actions/runs/39884517501): the msix package job is green with
`guest_window_scroll.exe` bundled; its `run` job on the console was still queued when this was written.

## What runs through the layer

`tests/guest/window_scroll.c` (builtin-only machine folder, Wine's user32/gdi32/win32u over core/user), a 300x200
`WS_POPUP|WS_VSCROLL|WS_HSCROLL` window; first output lines from the job's ctest-junit.xml:

    scroll: client=283x183
    scroll: v min=0 max=100 page=10 pos=20
    scroll: h min=0 max=50 pos=5
    scroll: vbar rect=383,100,400,283 state=0
    scroll: hittest down-arrow=7 hbar=6 client=1
    scroll: ScrollWindowEx before=404040 after=0000ff upd=0,0,283,30
    scroll: ScrollDC pixel=0000ff upd=0,0,283,10
    scroll: WM_VSCROLL code=1
    scroll: WM_VSCROLL code=8
    scroll: after click linedown=1 endscroll=1 pos=21
    scroll: done

Exit 42. The screenshot check (`WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=391,250,ffc8c8c8`) reads the bar's track colour
(COLOR_SCROLLBAR) between thumb and down arrow; the click is injected with `WIN10XBOX_TEST_INJECT_INPUT`
(`move 391,275; ldown; lup`) and reaches the guest as WM_NCLBUTTONDOWN(HTVSCROLL) -> WM_VSCROLL SB_LINEDOWN, SB_ENDSCROLL.

## Code

`core/user/win_scroll.cpp`/`.h`: per-window scroll state and Wine's SetScrollInfo / GetScrollInfo rules, bar geometry
(arrows, thumb), solid-colour drawing of the non-client bars from WM_NCPAINT (on top of main's `PaintNonClient`),
HTVSCROLL / HTHSCROLL in WM_NCHITTEST, WM_NCLBUTTONDOWN on a bar -> WM_xSCROLL (LINEUP/LINEDOWN/PAGEUP/PAGEDOWN) then
SB_ENDSCROLL, the `ScrollBar` control procedure (SBM_*), `ScrollDC`. Hooks: `win_defwnd.cpp`, `win_message.cpp`
(`NtUserMessageCall(ScrollBarWndProc)`), `win_window.cpp` (`NtUserCallHwndParam(GetScrollInfo)`).
`NtUserScrollWindowEx` was already in `win_paint.cpp` (pixels move, uncovered area invalidated); the guest checks it.

## Syscalls added (Todo -> Emulate)

NtUserSetScrollInfo, NtUserShowScrollBar, NtUserEnableScrollBar, NtUserGetScrollBarInfo, NtUserScrollDC
(`W10X_USER_SYSCALLS`, docs/SYSCALLS.md regenerated). NtUserCallHwndParam(GetScrollInfo) answers now.

## DLLs added to wine-pe/dlls.txt

None.

## History

The first run (39848913871) failed on both Windows jobs because the test was registered on the full machine folder
(docs/GUI.md rule 8): the guest died with 0x80000100 on `api-ms-win-core-crt-l2-1-0!_initterm_e` in native ntdll
before main. It is now registered with the other GUI tests under `WIN10XBOX_BUILTIN_MACHINE_DIR`. host-uwp's first
run also failed, on main's own `guest_spawn` target (fixed on main), then bundles the new guest.

## What still fails / not implemented

Thumb dragging and button auto-repeat (a click is a single event, no modal tracking loop), hover/pressed visuals,
WS_EX_LEFTSCROLLBAR, size grips, accessibility events. Scroll bars are drawn in solid colours (no themed look).
