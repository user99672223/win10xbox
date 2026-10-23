status: wip

# Slice: menu

Menu bar and popup menus in core/user (`core/user/win_menu.cpp`): `CreateMenu`, `CreatePopupMenu`, the
`NtUserThunkedMenuItemInfo` family, `SetMenu` (reserves `SM_CYMENU` under the caption), the painted bar,
`TrackPopupMenu` and `WM_COMMAND` from a click. Gate: `integration.guest_window_menu`
(`tests/guest/window_menu.c`, host-injected clicks, pixel readback, screenshot pixels) on linux-cross and both MSVC
configurations, and the package check of host-uwp.yml.

Work in progress: merged with main (ncframe caption, scroll bars, dialog, comctl, clipboard, seh). The bar rectangle is
the frame's inner width (`FrameInset`) between the ncframe caption's bottom line and the client top, so non-client
scroll bars (right of / below the client area) do not touch it; `WM_NCLBUTTONDOWN` on `HTMENU` goes through the
`NcHit` switch of the ncframe code. The coordinates in the test are predicted from the layout constants (frame 8,
caption 23, bar 20) and are to be confirmed by the printed values of the CI run.
