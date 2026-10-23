status: ready

# Slice: menu

Menu bar and popup menus in core/user (`core/user/win_menu.cpp`): `CreateMenu`, `CreatePopupMenu`, the
`NtUserThunkedMenuItemInfo` family, `SetMenu` (reserves `SM_CYMENU` under the caption), the painted bar,
`TrackPopupMenu` and `WM_COMMAND` from a click. Gate: `integration.guest_window_menu`
(`tests/guest/window_menu.c`, host-injected clicks, pixel readback, screenshot pixels) on linux-cross and both MSVC
configurations, and the package check of host-uwp.yml.

Run: https://github.com/user99672223/win10xbox/actions/runs/39930060309 (ci.yml green on linux-cross, windows Debug and
windows RelWithDebInfo, "Gate - nothing skipped" passed; host-uwp.yml run 39930060418 green, with guest_window_menu.exe in
the package check).

What runs through the layer: `guest_window_menu.exe` (tests/guest/window_menu.c), printed (windows-Debug-logs junit):
```
menu: api items=4 id0=101 checked=1 grayed=1
menu: client height before=261 after=241 delta=20 SM_CYMENU=20
menu: bar rect=108,131,492,151
menu: item File rect=108,131,156,150
menu: pixels bg=00f0f0f0 glyph=00000000 first_text_pixel=116,134
menu: WM_INITMENUPOPUP popup=1
menu: popup items open=109,154,219,176 exit=109,185,219,207
menu: WM_COMMAND id=102
menu: TrackPopupMenu returned 101
menu: loops enter=2 exit=2 init=2
menu: done
```
The bar sits under the ncframe caption (frame 8, caption 23) and spans the frame's inner width; non-client scroll bars lie
right of / below the client area. `WM_NCLBUTTONDOWN` on `HTMENU` goes through the ncframe `NcHit` switch.

Syscalls added (Emulate, core/user/win_menu.cpp): NtUserCreateMenu, NtUserCreatePopupMenu, NtUserDestroyMenu,
NtUserRemoveMenu, NtUserDeleteMenu, NtUserCheckMenuItem, NtUserEnableMenuItem, NtUserSetMenuDefaultItem,
NtUserHiliteMenuItem, NtUserGetMenuItemRect, NtUserMenuItemFromPoint, NtUserGetMenuBarInfo,
NtUserThunkedMenuItemInfo, NtUserThunkedMenuInfo, NtUserTrackPopupMenuEx, NtUserEndMenu; the popup menu window
procedure (`#32768`) is dispatched by NtUserMessageCall. DLLs added to wine-pe/dlls.txt: none.

Still not done: cascading submenus of popup items, keyboard mnemonics / Alt and F10 activation, accelerator tables,
owner-draw and bitmap items, `GetSystemMenu` / `SetSystemMenu`, menu bar wrapping, scrolling of tall popups,
`MIM_BACKGROUND`, the class menu name of `RegisterClass`.
