# GUI: the path to pixels

This is the design for `core/user`, the module that makes Wine's PE `user32.dll` and `gdi32.dll`
work on top of our dispatcher, and for the hosts that put the result on a screen. It replaces the
`winexbox.drv` binding plan in `winexbox/README.md` (kept there as history).

## The facts this design rests on (verified in `wine-pe/wine-src`, Wine 11.9)

* **`win32u.dll` is a PE of syscall thunks only.** `dlls/win32u/Makefile.in` declares
  `UNIXLIB = win32u.so`; `win32u.spec` has 1541 `-syscall` entries and nothing else; every one of
  `window.c`, `message.c`, `class.c`, `dce.c`, `dibdrv/*`, `gdiobj.c`, `sysparams.c` is compiled
  into the **unix** library, which also talks to wineserver for windows, queues and classes. We
  never build or ship the unix side (`wine-pe/README.md`). So the package contains the thunks and
  **no window manager, no message queues, no GDI engine**. HANDOFF.md's statement that win32u is a
  userspace PE containing them was wrong and is withdrawn in STATUS.md.
* **The thunks already reach our dispatcher.** The loader rewrites every `Nt*` export of every
  mapped Wine builtin (`core/loader/stubs.cpp`), win32u included; the thunk ids are Wine's own
  numbering `0x1000..0x1603` (`dlls/win32u/win32syscalls.h`, generated from the same spec that
  generates the thunks). `tools/gen_syscalls.py` now emits them as `WIN32U_SYSCALL(id, name,
  nargs)` into `core/dispatch/syscall_table.gen.inc`; the dispatcher marks an entry **Emulate**
  when `core/user` registers an implementation for its name (`UserImpls()`) and **Todo**
  otherwise. Nothing passes through: the host kernel has no win32k for an AppContainer.
* **The user32/gdi32 PE side is thin and its contract is two headers:** `include/ntuser.h` and
  `include/ntgdi.h`. They define every `NtUser*`/`NtGdi*` prototype, the `NtUserCall{No,One,Two}Param`
  / `NtUserCallHwnd[Param]` code enums, `NtUserMessageCall` codes, `struct win_proc_params`, the
  `KernelCallbackTable` ids (`NtUserCallWinProc` = index 4, ...), `struct ntuser_thread_info`
  (read straight out of `TEB.Win32ClientInfo`), `struct ntuser_client_procs_table`, and
  `GDI_HANDLE_ENTRY` / `GDI_SHARED_MEMORY` (gdi32 decodes every HGDIOBJ by reading
  `PEB.GdiSharedHandleTable`: `LOWORD(handle)` is the index, `HIWORD` must equal `entry.Unique`).
  These are "not compatible with Windows" by Wine's own comment: we implement **Wine's** contract,
  because the DLLs are Wine's.
* **user32 initialisation is tiny** (`dlls/user32/user_main.c`): `DllMain` sets
  `PEB.KernelCallbackTable`, calls `winproc_init()` (-> `NtUserInitializeClientPfnArrays`, the only
  syscall) and `SYSPARAMS_Init()` (-> `NtUserGetSystemDpiForProcess`, `LdrQueryImageFileExecutionOptions`,
  `QueryActCtxSettingsW`, `NtUserSetProcessDpiAwarenessContext`). `gdi32`'s `DllMain` does nothing.
* **Window procedures run through the kernel callback table.** win32u (ours) calls
  `KernelCallbackTable[NtUserCallWinProc](struct win_proc_params*, size)`; user32's
  `User32CallWinProc` (`dlls/user32/winproc.c`) calls the procedure and ends with
  `NtCallbackReturn(&result, sizeof(result), STATUS_SUCCESS)`. `DispatchMessageW` avoids the
  callback: it asks `NtUserMessageCall(..., NtUserGetDispatchParams)` to fill `win_proc_params`
  and calls the procedure itself. `call_window_proc` in user32 takes a **raw** `WNDPROC`
  (`procA`/`procW`), so we never hand out Wine's `WINPROC_HANDLE` encodings: `func`, `procA`,
  `procW` carry the real function pointer (`ansi` = the window is ANSI, `ansi_dst` = the
  procedure is ANSI; user32 converts when they differ).

## Architecture

```
guest EXE -> user32.dll / gdi32.dll (Wine PE)          guest view
                |  win32u.dll thunks (ids 0x1000..)      |
                v                                        v
core/dispatch  win10xbox_dispatch -> Emulate -> core/user::Nt*   (host view, user lock)
                                                   |  window surfaces, input, display config
                                                   v
                                      winexbox::IWinexboxDriver (headless: system-memory BGRA)
                                                   |
                 user::ComposeFrame(): desktop + windows by z-order -> BGRA frame
                        |                                   |
          host-win32: WIN10XBOX_SCREENSHOT=x.bmp     host-uwp: D3D11 swap chain on the CoreWindow
```

`core/user` **is** the win32k layer: window manager, message queues, GDI objects, a small
software rasteriser. `winexbox` stays what it is (pixel memory, input queue, display modes); the
`winexbox.drv` C adapter is not needed, because there is no Wine `user_driver_funcs` caller any
more. Presentation is the host's: it only needs `ComposeFrame`.

## Module layout (`core/user/`)

| File | Owns |
| --- | --- |
| `ntuser_abi.h` | the contract as data: KernelCallbackTable ids, `ntuser_thread_info`, `win_proc_params`, the `NtUserCall*` / `NtUserMessageCall` / `NtGdiGetDCDword` codes, `GDI_HANDLE_ENTRY`, `DC_ATTR` (every layout user32/gdi32 read from memory has a `static_assert`) |
| `user_syscalls.h` | every implemented syscall's prototype and `W10X_USER_SYSCALLS`, the registration list `UserImpls()` is built from |
| `user.cpp` | module state (`S()`, allocated under the lock), `user.h` API, per-thread record (`TEB.Win32ClientInfo[61]`), user-mode callbacks, `NtCallbackReturn`, the guest last error of a syscall (`SetLastErr`), screenshots |
| `user_internal.h` | shared internals (lock, objects, windows, queues, helpers) |
| `gdi_objects.cpp` | `GDI_SHARED_MEMORY` at `PEB.GdiSharedHandleTable` (0x10000 entries, Wine's encoding, stock objects in `init_stock_objects` order, system colour brushes/pens), regions (non-overlapping rectangle lists), brushes, pens, fonts (LOGFONT only), bitmaps / DIB sections / DIB conversion, palettes, `NtGdiCreate*`, `NtGdiDeleteObjectApp`, `NtGdiExtGetObjectW`, `Get/SetDIBits`, region syscalls |
| `gdi_dc.cpp` | DCs (screen, window, memory) and their `DC_ATTR` (guest-visible pool), `NtGdiCreateCompatibleDC`, `NtGdiOpenDCW`, `NtGdiGetDeviceCaps`, `NtGdiGetAndSetDCDword`, `NtGdiSelect*`, `SaveDC`/`RestoreDC`, clipping, logical->device mapping (MM_TEXT / ISOTROPIC / ANISOTROPIC, world transform translation), what a DC draws on (`ResolveDrawTarget`), the text metrics of the built-in face (`TextCellOf`, `GetTextMetrics`, `GetTextExtentExW`, `GetCharWidth*`) |
| `gdi_draw.cpp` | the rasteriser on BGRA32: `PatBlt`, `BitBlt` / `StretchBlt` with any ternary ROP (colour<->mono conversion), `Get/SetPixel`, `Rectangle`, `Ellipse`, `MoveTo`/`LineTo`, `PolyPolyDraw` (polygons, polylines, Béziers), `Fill/Frame/InvertRgn`, `SetDIBitsToDevice`, `StretchDIBits`, `ExtTextOutW` (the built-in 8x16 face: `font8x16.h`) |
| `win_class.cpp` | classes and the user atom table (`NtUserRegisterClassExWOW`, `NtUserGetClassInfoEx`, `NtUserGetClassName`, `NtUserGetAtomName`, `NtUserRegisterWindowMessage`, class longs), the builtin classes (Button ... `#32769`, `Message`) with the procs from `NtUserInitializeClientPfnArrays`; procedures are kept as raw (A, W) pairs |
| `win_window.cpp` | handle table, tree and z-order, rects, `NtUserCreateWindowEx` (Wine's message sequence), `ShowWindow`, `SetWindowPos` (WINDOWPOSCHANGING / NCCALCSIZE / WINDOWPOSCHANGED), `DestroyWindow` (WM_DESTROY parent first, WM_NCDESTROY children first), activation and focus, window longs, props, `NtUserCall{Hwnd,HwndParam,NoParam,OneParam,TwoParam}` codes, `FindWindowEx`, `BuildHwndList`, `WindowFromPoint`, capture, enable |
| `win_defwnd.cpp` | the default window procedure (`NtUserMessageCall(NtUserDefWindowProc)`): text, WM_NCCALCSIZE from the style (Windows 10 frame metrics), WM_NCHITTEST, WM_NCPAINT, WM_ERASEBKGND with the class brush, WM_CTLCOLOR*, WM_CLOSE, WM_WINDOWPOSCHANGED -> WM_MOVE/WM_SIZE, WM_ACTIVATE -> focus, Alt+F4 |
| `win_message.cpp` | per-thread queues (posted, input, cross-thread sent), WM_PAINT and WM_TIMER pseudo-messages, `GetMessage`, `PeekMessage`, `MsgWaitForMultipleObjectsEx`, `WaitMessage`, `GetQueueStatus`, `NtUserMessageCall` codes, `DispatchMessage` (WM_PAINT / timer procs), `TranslateMessage` (US layout), timers, the input sink and the thread-side processing of mouse input (hit test, activation, `WM_SETCURSOR`, double clicks), key state, `SendInput`, cursor, caret (tracked, not drawn) |
| `win_paint.cpp` | update regions (bounding rectangle + erase / frame / internal-paint flags), `InvalidateRect`/`Rgn`, `ValidateRect`/`Rgn`, `RedrawWindow`, `GetUpdateRect`/`Rgn`, `BeginPaint`/`EndPaint`, `GetDC`/`GetDCEx`/`GetWindowDC`/`ReleaseDC`, `ScrollWindowEx`, the non-client frame (solid colours), window surfaces from the driver, flush, `ComposeFrame` / `ComposeLastPaintedFrame` |
| `win_sysparams.cpp` | `GetSystemMetrics` (Windows 10 at 96 DPI), `SystemParametersInfo[ForDpi]`, DPI awareness, monitors (`EnumDisplayMonitors` via the callback, `GetMonitorInfo`, `MonitorFromRect/Window`), `EnumDisplaySettings`, `EnumDisplayDevices`, `SetSysColors` |

Any syscall not implemented stays **Todo** in the dispatcher: it traces `unimplemented syscall NtUserFoo`, sets the
guest's last error to `ERROR_CALL_NOT_IMPLEMENTED` and returns **0** -- the failure value of every win32u call
except the few that return an `NTSTATUS` (`NtUserBuildHwndList`, `NtUserInitializeClientPfnArrays`, the
`NtGdiDdDDI*` family, ...), which get `STATUS_NOT_IMPLEMENTED`. (The first version returned
`STATUS_NOT_IMPLEMENTED` everywhere, which user32 read as a valid HWND from `NtUserCreateWindowEx` and as
`TRUE`/`-1` from BOOL calls.) Implement what the guest hits, in the order it hits it; do not pre-build the long tail.

### What is implemented (2026-10-22)

235 win32u syscalls are registered (`W10X_USER_SYSCALLS`, listed as Emulate in docs/SYSCALLS.md; plus the ntdll-side `NtCallbackReturn`): enough for
`tests/guest/window.c` (frameless popup, FillRect, GetPixel, timers, PostMessage) and `tests/guest/window2.c`
(captioned overlapped window, AdjustWindowRectEx, Set/GetWindowText, ShowWindow/UpdateWindow, memory DC +
CreateCompatibleBitmap + BitBlt, InvalidateRect, KillTimer, DestroyWindow, PeekMessage on
MsgWaitForMultipleObjectsEx). Both pass under Wine (`integration.guest_window`, `integration.guest_window2`).

Not implemented yet (they stay Todo or are documented FIXMEs), roughly in the order a typical program reaches them:

* **Text**: one built-in face, a classic-VGA-style 8x16 bitmap font (`font8x16.h`) for ASCII 0x20-0x7E; every
  other code point (control characters, Latin-1 above 0x7E, anything outside the BMP) draws a box glyph, and 0xA0
  a blank cell. The cell is 8 px wide and 16 tall with the baseline at row 12, so every font reports
  `tmHeight` 16, `tmAscent` 12, `tmDescent` 4, `tmAveCharWidth` = `tmMaxCharWidth` 8 and a fixed pitch. A LOGFONT
  of height >= 32 (either sign) scales the glyphs by `height / 16` (whole factors, at most 64); anything smaller
  keeps the plain cell, whatever face name, weight or width was asked for. `ExtTextOut` draws ETO_OPAQUE,
  ETO_CLIPPED, ETO_PDY and the `dx` array, the OPAQUE/TRANSPARENT background mode, `TA_LEFT/CENTER/RIGHT`,
  `TA_TOP/BASELINE/BOTTOM` and `TA_UPDATECP`, clipped to the DC's clip, with alpha 0xFF; `TextOutW(x, y)` puts the
  first cell's top-left at `(x, y)` with the default alignment. `GetTextExtentExW` (width 8 per character plus the
  DC's extra spacing, partial extents, `nfit`), `GetCharWidth*`, `GetCharABCWidths*` (A = C = 0), `GetCharWidthInfo`
  (no bearings), `GetTextFaceW` (the LOGFONT's face name) and `GetTextCharsetInfo` agree with it, so user32's
  `DrawText` (which lays out from those extents) centres and wraps correctly. No bold, italic, underline or
  strike-out styling, no kerning, no justification (`SetTextJustification` is accepted and ignored), no
  `GetGlyphIndices` beyond the identity map. `GetFontData`, `GetOutlineTextMetrics`, `EnumFontFamilies` and
  `AddFontResource` stay Todo / failing.
* **Menus** (`NtUserCreateMenu`, `NtUserThunkedMenuItemInfo`, `TrackPopupMenu`...): Todo; `LoadMenu` fails, so
  windows get no menu bar. **Icons and cursors** (`NtUserSetCursorIconData`, `NtUserDrawIconEx`, `LoadImage` of
  icons/cursors): Todo. **Scroll bars** (`NtUserSetScrollInfo`, the scroll bar window procedure): Todo (WS_VSCROLL
  only reserves the space). **Clipboard**, **hooks** (`SetWindowsHookEx`, `SetWinEventHook`), **IME**,
  **raw input**, **layered windows** (`SetLayeredWindowAttributes`, `UpdateLayeredWindow`), **window regions**,
  `AlphaBlend` / `TransparentBlt` / `GradientFill` / `MaskBlt`, `PrintWindow`, `ScrollDC`, `DeferWindowPos`,
  window stations / desktops: Todo.
* Window dragging / resizing from the frame (WM_NCLBUTTONDOWN on the caption or border) is not implemented; the
  frame is drawn in solid colours (COLOR_ACTIVEBORDER / COLOR_ACTIVECAPTION, a close-button square), without
  caption text.
* Update regions are a bounding rectangle per window (BeginPaint clips to it), not true regions.
* Cross-thread `SendMessage` works (queued and answered while the receiver pumps); `SendMessageTimeout` ignores
  the timeout. `ShowWindowAsync` runs synchronously. `DestroyWindow` of another thread's window fails.
* Drawing into 1 bpp bitmaps and non-32 bpp DIB sections works for blits (pixel by pixel) but not for the
  line / polygon primitives; drawing on the screen DC (`GetDC(NULL)`) is dropped with a FIXME.
* DPI: everything is 96 DPI; `SetProcessDpiAwarenessContext` is recorded, nothing is scaled.

## Rules

1. **One lock** (`detail::UserLock()`), held for the duration of a syscall and released while
   waiting for messages and while a user-mode callback runs. Never call back into guest code
   with the lock held (the window procedure makes syscalls that take it).
2. **Callbacks** go through `detail::UserModeCallback(id, args, len, &ret, &ret_len)`. It switches
   the thread to the guest TLS view for the duration (`CallInGuestView`), so everything the
   callback needs is resolved before it is called, and nothing host-side runs between
   `LeaveToGuestView` and the call. `NtCallbackReturn` copies the result into the thread's
   `CallbackFrame` while the procedure's stack is still alive.
3. **No function-local statics reachable from a guest thread's first touch** without the module
   lock already held (MSVC magic statics read TLS; see dispatcher.cpp). Namespace-scope,
   trivially initialised state, or allocate under the lock.
4. **Handles**: HWND = `(0x20 + slot) | (generation << 16)`, generation 1..0x7fff: fits 32 bits with bit 31
   clear (`UlongToHandle` is used everywhere in ntuser.h), never 0, never one of `HWND_TOP/BOTTOM/TOPMOST/
   NOTOPMOST/MESSAGE/BROADCAST`; the desktop window is the first one. Truncated (16-bit) handles resolve by slot. HGDIOBJ = Wine's
   encoding (`LOWORD` index into the shared table, `HIWORD` = `Unique`, type bits as
   `NTGDI_OBJ_*`), stock objects at `FIRST_GDI_HANDLE + n` in exactly the order of Wine's
   `init_stock_objects` (`dlls/win32u/gdiobj.c`), because `GetStockObject` is a table lookup in
   gdi32.
5. **PEB/TEB**: `PEB.GdiSharedHandleTable` is set when the module initialises (the dispatcher
   saves and restores it around the guest's life, `process.cpp`); `PEB.KernelCallbackTable` is
   user32's and only read. `TEB.Win32ClientInfo[0..]` is `struct ntuser_thread_info` and is kept
   current (`message_time`, `message_pos`, `top_window`, `msg_window`, `dpi_context`,
   `receive_flags`); `[61]` is our per-thread pointer.
6. **Pixels**: surfaces are `winexbox::ISurface` BGRA8888 top-down, alpha byte written as `0xFF`
   by every drawing primitive. The rasteriser clips to the DC's clip box (client rect
   intersected with the surface, intersected with `IntersectClipRect`/`SelectClipRgn` if set).
   After `EndPaint` / `ReleaseDC` of a window DC that drew something, and whenever a thread goes idle in
   `GetMessage`/`PeekMessage`: `surface->Flush(dirty)`, `driver->Present()`, `++FrameSerial`. Every flush also
   records the composition (surfaces + screen rectangles) as the *last painted frame*: `WriteScreenshot`
   writes that one (`ComposeLastPaintedFrame`), so a program that hides or destroys its window before exiting
   still leaves its picture; `ComposeFrame` is always the live state (what host-uwp presents). Windows that are
   still alive when the guest process exits stay in the composition.
7. **Messages** follow Wine's `window.c`/`winpos.c`/`defwnd.c` sequences where it matters for
   real programs (WM_NCCREATE, WM_NCCALCSIZE, WM_CREATE, WM_SIZE, WM_MOVE, WM_SHOWWINDOW,
   WM_WINDOWPOSCHANGING/CHANGED, WM_ACTIVATE, WM_SETFOCUS, WM_ERASEBKGND, WM_PAINT, WM_DESTROY,
   WM_NCDESTROY). `NtUserDefWindowProc` (via `NtUserMessageCall`) implements the default
   behaviour for what we send: WM_NCCREATE stores the text, WM_NCCALCSIZE keeps the client equal
   to the window rect for now (no non-client area is drawn yet: **frameless windows first**),
   WM_ERASEBKGND fills with the class brush, WM_CLOSE destroys, WM_PAINT validates.
8. **Wine's GUI stack is always builtin** (DECISIONS.md E1, extended): `win32u`, `user32`, `gdi32` and
   `imm32` load from `<machine>\Windows\builtin` whatever `dll_overrides` says, like ntdll/kernelbase/kernel32.
   The machine folder of the Windows CI job holds the runner's own user32/gdi32/win32u (`machine/guest_dlls.txt`)
   and `native,builtin` loaded them: native win32u then made Windows-numbered win32k syscalls into Wine's table.
   (If route B -- forwarding to the host's win32k, measured in STATUS.md -- is ever taken, this is the switch to
   revisit.)
   That alone is not enough on a machine folder full of *other* native DLLs: Wine's user32/gdi32 import
   advapi32 and sechost, and the runner's build-26100 advapi32/sechost/rpcrt4 import ntdll exports Wine 11.9 lacks
   and resolve `api-ms-win-core-crt-l2-1-0` to `kernelbase.dll` through the host's apiset schema (the guest shares
   the host's PEB, so `PEB.ApiSetMap` is the host's), i.e. to Wine's kernelbase: the first call through such an
   import raised 0x80000100. The GUI tests therefore run on `bin/machine-builtin`, the console's builtin-only DLL
   set (docs/TESTING.md).
9. **Input**: `user::InputSink()` is an `IEventSink`; the host installs it on its driver (host-win32) or forwards its
   own sink's calls to it (host-uwp). It runs on whichever thread drains the driver (`ProcessEvents`, one thread at a
   time), takes the module lock, and only *queues*, as wineserver does: it updates the asynchronous key state
   (`GetAsyncKeyState`) and the cursor, then
   * keys: WM_KEYDOWN/UP (WM_SYSKEYDOWN/UP with Alt held without Ctrl, for Alt itself and F10, and when nothing has the
     focus) to the focus window of the foreground thread; lParam = repeat 1 | scan << 16 | extended << 24 | context
     << 29 | previous state << 30 | transition << 31; the L/R modifier keys arrive as VK_SHIFT/CONTROL/MENU;
   * pointer events: a *raw* message (base WM_* message, screen position, MK_* flags in wParam) for the thread that owns
     the window under the cursor (the deepest child's thread, via `WindowsFromPoint`) or the capture window.
   The thread that takes a raw mouse message off its queue finishes it (`ProcessMouseMessage`, Wine's
   `process_mouse_message`): the hit test (`WM_NCHITTEST` to this thread's windows from the front, HTTRANSPARENT passes
   the point on, a disabled popup is HTERROR), client or non-client message (wParam = hit test, screen coordinates), double
   click translation (`CS_DBLCLKS`, `GetDoubleClickTime`, `SM_CXDOUBLECLK`), `WM_PARENTNOTIFY`, `WM_MOUSEACTIVATE` and
   activation of the top-level window (WM_NCACTIVATE / WM_ACTIVATE(WA_CLICKACTIVE) / focus), then `WM_SETCURSOR`; under
   capture none of the last three. `GetKeyState` / `GetKeyboardState` / `TranslateMessage` use the state as of the last
   input message an application *took* (`input_key_state`), `GetAsyncKeyState` the hardware's, so a queued shift+A
   translates to 'A' however fast it was injected. 0x01 (toggle) is kept for CapsLock/NumLock/ScrollLock only.
   Message times are the guest's own tick count (KUSER_SHARED_DATA, what `GetTickCount` reads), `GetMessagePos` is the
   screen position, both set per delivered message in `ntuser_thread_info`. GUI threads drain the driver themselves
   while they pump or wait (`GetMessage`, `PeekMessage`, `MsgWaitForMultipleObjectsEx`, `GetQueueStatus`,
   `GetAsyncKeyState`; the driver's `WakeHandle` is one of the wait handles). The wheel goes to the window under the
   cursor, like Wine; input with no window of ours under the cursor / no focus is dropped (traced as `+user`).
   Not done: WM_MOUSELEAVE/HOVER, raw input, hooks, dead keys and other layouts (only the plain US layout), the
   journaling and key-repeat counts.
10. **The first target is `tests/guest/window.c`** (`integration.guest_window`): RegisterClass,
   CreateWindowEx(WS_POPUP|WS_VISIBLE, 400x300 at 100,100), WM_PAINT with two FillRects, GetPixel
   read-back, PostMessage/SetTimer, GetMessage loop, exit 42; plus the screenshot pixels. It must
   pass under Wine locally (`ctest --test-dir build-mingw -R guest_window`) and on both Windows
   configs in CI. Then `tests/guest/window2.c` (done), then a real third-party program.

## Gamepad / XInput

The guest's XInput is our own small DLL, `guest-dlls/xinput/xinput.c` (`xinput1_4.dll`, staged under the names
`xinput1_3.dll` and `xinput9_1_0.dll` too; always builtin, rule 8): Wine's is PE code over hid/setupapi and a HID bus
driver we do not have. The DLL holds no state. It reaches the host like Wine's PE DLLs reach their unix side: ntdll's
`__wine_unix_call_dispatcher` (a data export, found with GetProcAddress) and the handle that `NtQueryVirtualMemory`
`MemoryWineLoadUnixLibByName` gives for `L"xinput1_4"`, a unix library registered in `core/dispatch/unix_calls.cpp`
with `get_state`, `set_state`, `get_caps` (structs in `core/include/win10xbox/xinput_abi.h`, results are Win32 error
codes). They read and write the process-wide pad store `win10xbox::input::SetGamepadState/GetGamepadState`
(`core/input/gamepad.cpp`, four pads, one lock, a packet number that moves when the state changes). User 0 is the host's
pad; users 1..3 and a pad nobody has set are ERROR_DEVICE_NOT_CONNECTED. Writers: host-win32's inject script (`pad
BUTTONS LX LY RX RY LT RT`, `padwait N`), host-uwp's `Launcher::PollGamepad` (Windows.Gaming.Input pad 0, and the
guest's `XInputSetState` goes back to its `Vibration`). `XInputGetKeystroke` reports no keystrokes (ERROR_EMPTY).

## Local workflow

```
tools/dev/build-mingw.sh              # configure + build (llvm-mingw, /opt/llvm-mingw)
# builtins: wine-pe/out/builtin exists (wine-pe/build.sh); NLS: see HANDOFF trap 1
ctest --test-dir build-mingw -R guest_window --output-on-failure
WIN10XBOX_DEBUG=+user,+gdi,+relay wine build-mingw/bin/win10xbox.exe --machine Z:/.../build-mingw/bin/machine-builtin C:\guest\guest_window.exe
```

`WIN10XBOX_DEBUG=+relay` shows every syscall with arguments and status (the win32u ones included),
which is the fastest way to see what user32 asks for next; `+user,+gdi` are this module's
channels; `WIN10XBOX_WINEDEBUG=+win,+msg` turns on Wine's own user32 traces.
