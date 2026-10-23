status: wip

# Slice: menu

Menu bar and popup menus in core/user (`core/user/win_menu.cpp`): `CreateMenu`, `CreatePopupMenu`, the
`NtUserThunkedMenuItemInfo` family, `SetMenu` (reserves `SM_CYMENU` under the caption), the painted bar,
`TrackPopupMenu` and `WM_COMMAND` from a click. Gate: `integration.guest_window_menu`
(`tests/guest/window_menu.c`, host-injected clicks, pixel readback, screenshot pixels) on linux-cross and both MSVC
configurations, and the package check of host-uwp.yml.

Work in progress: first push, coordinates in the test are predicted from the layout constants and are to be confirmed
by the printed values of the first CI run.
