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
| `user.cpp` | module state, `user.h` API, per-thread record (`TEB.Win32ClientInfo[61]`), user-mode callbacks, `NtCallbackReturn`, `UserImpls()` registration table |
| `user_internal.h` | shared internals (lock, thread record, callback helper) |
| `gdi_objects.cpp` | `GDI_SHARED_MEMORY` at `PEB.GdiSharedHandleTable` (allocated once, 0x10000 entries, Wine's handle encoding and stock-object slots), brushes, pens, fonts (metrics only), bitmaps/DIB sections, regions (rect lists), `NtGdiCreate*`, `NtGdiDeleteObjectApp`, `NtGdiExtGetObjectW`, `NtGdiSelect*`, `NtGdiGetDCObject`, `NtGdiGetStockObject` |
| `gdi_dc.cpp` | DCs (display, window, memory), `NtGdiOpenDCW`, `NtGdiCreateCompatibleDC`, `NtGdiGetDeviceCaps`, `NtGdiGetAndSetDCDword`, `NtGdiSaveDC`/`RestoreDC`, clipping (`NtGdiIntersectClipRect`, `NtGdiGetAppClipBox`), coordinate state |
| `gdi_draw.cpp` | the rasteriser on BGRA32 surfaces: `NtGdiPatBlt`, `NtGdiBitBlt`, `NtGdiStretchBlt`, `NtGdiSetPixel`/`GetPixel`, `NtGdiRectangle`, `NtGdiLineTo`/`MoveTo`, `NtGdiFillRgn`?, `NtGdiSetDIBitsToDeviceInternal`, `NtGdiGetDIBitsInternal`, `NtGdiExtTextOutW` (no font engine yet: draw nothing, return TRUE, FIXME once) |
| `win_class.cpp` | classes (`NtUserRegisterClassExWOW`, `NtUserGetClassInfoEx`, `NtUserGetClassName`, class longs), the builtin classes win32u registers itself (`register_builtin_classes` in Wine's `class.c`: Button, ComboBox, ..., `#32769` desktop, `#32768` menu, `Message`, `Static`, `Edit`...) using the procs from `NtUserInitializeClientPfnArrays` |
| `win_window.cpp` | windows: handle table, parent/owner/children/z-order, rects (window/client in screen coords), style/exstyle/id/userdata/text/props, `NtUserCreateWindowEx` (Wine's message sequence), `NtUserDestroyWindow`, `NtUserShowWindow`, `NtUserSetWindowPos`, `NtUserMoveWindow`, `NtUserSetWindowLong[Ptr]`, `NtUserCallHwnd[Param]` codes, `NtUserCallNoParam`/`OneParam`/`TwoParam` codes, `NtUserFindWindowEx`, `NtUserBuildHwndList`, `NtUserQueryWindow`, `NtUserSetParent`, `NtUserGetAncestor`, `NtUserWindowFromPoint`, focus/active/capture/foreground |
| `win_message.cpp` | per-thread queues: posted messages, sent messages (same thread: direct; other thread: queue + wait), paint and timer pseudo-messages, `NtUserPostMessage`, `NtUserPostThreadMessage`, `NtUserPostQuitMessage`, `NtUserGetMessage`, `NtUserPeekMessage`, `NtUserMsgWaitForMultipleObjectsEx`, `NtUserWaitMessage`, `NtUserGetQueueStatus`, `NtUserMessageCall` (codes above), `NtUserDispatchMessage`, `NtUserTranslateMessage`, `NtUserSetTimer`/`KillTimer`, `NtUserGetThreadState`, `NtUserRegisterWindowMessage`, `NtUserGetKeyState`/`AsyncKeyState`/`KeyboardState`, input events from the driver (`IEventSink::OnInput` -> WM_MOUSEMOVE/WM_LBUTTONDOWN/WM_KEYDOWN... to the window under the cursor / focus) |
| `win_paint.cpp` | update regions, `NtUserInvalidateRect`/`Rgn`, `NtUserValidateRect`, `NtUserRedrawWindow`, `NtUserGetUpdateRect`/`Rgn`, `NtUserBeginPaint`/`EndPaint`, `NtUserGetDC`/`GetDCEx`/`GetWindowDC`/`ReleaseDC`, window surfaces (allocated from the driver per **top-level** window, child windows draw into their top-level's surface with an offset and a clip), `ComposeFrame`, `FrameSerial`, erase background (WM_ERASEBKGND with the class brush) |
| `win_sysparams.cpp` | `NtUserSystemParametersInfo[ForDpi]`, system metrics, system colours/brushes/pens, DPI (`96`, `NTUSER_DPI_UNAWARE` contexts), monitors (`NtUserEnumDisplayMonitors`, `GetMonitorInfo`, `MonitorFromRect/Window/Point`, `NtUserEnumDisplaySettings`, `NtUserEnumDisplayDevices`, `NtUserGetDisplayConfigBufferSizes`...) from `IWinexboxDriver::QueryDisplay()` |

Any syscall not implemented stays **Todo** in the dispatcher: it traces `unimplemented syscall
NtUserFoo` once per call site and returns `STATUS_NOT_IMPLEMENTED` (which user32 sees as 0/FALSE).
Implement what the guest hits, in the order it hits it; do not pre-build the long tail.

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
4. **Handles**: HWND = `(index + 1) | (generation << 16)`, fits 32 bits (`UlongToHandle` is used
   everywhere in ntuser.h), never 0; the desktop window is the first one. HGDIOBJ = Wine's
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
   After `EndPaint` / `ReleaseDC` of a window DC that drew something: `surface->Flush(dirty)`,
   `driver->Present()`, `++FrameSerial`.
7. **Messages** follow Wine's `window.c`/`winpos.c`/`defwnd.c` sequences where it matters for
   real programs (WM_NCCREATE, WM_NCCALCSIZE, WM_CREATE, WM_SIZE, WM_MOVE, WM_SHOWWINDOW,
   WM_WINDOWPOSCHANGING/CHANGED, WM_ACTIVATE, WM_SETFOCUS, WM_ERASEBKGND, WM_PAINT, WM_DESTROY,
   WM_NCDESTROY). `NtUserDefWindowProc` (via `NtUserMessageCall`) implements the default
   behaviour for what we send: WM_NCCREATE stores the text, WM_NCCALCSIZE keeps the client equal
   to the window rect for now (no non-client area is drawn yet: **frameless windows first**),
   WM_ERASEBKGND fills with the class brush, WM_CLOSE destroys, WM_PAINT validates.
8. **The first target is `tests/guest/window.c`** (`integration.guest_window`): RegisterClass,
   CreateWindowEx(WS_POPUP|WS_VISIBLE, 400x300 at 100,100), WM_PAINT with two FillRects, GetPixel
   read-back, PostMessage/SetTimer, GetMessage loop, exit 42; plus the screenshot pixels. It must
   pass under Wine locally (`ctest --test-dir build-mingw -R guest_window`) and on both Windows
   configs in CI. Then `hello.exe` of host-uwp, then a real third-party program.

## Local workflow

```
tools/dev/build-mingw.sh              # configure + build (llvm-mingw, /opt/llvm-mingw)
# builtins: wine-pe/out/builtin exists (wine-pe/build.sh); NLS: see HANDOFF trap 1
ctest --test-dir build-mingw -R guest_window --output-on-failure
WIN10XBOX_DEBUG=+user,+gdi,+relay wine build-mingw/bin/win10xbox.exe --machine Z:/.../build-mingw/bin/machine C:\guest\guest_window.exe
```

`WIN10XBOX_DEBUG=+relay` shows every syscall with arguments and status (the win32u ones included),
which is the fastest way to see what user32 asks for next; `+user,+gdi` are this module's
channels; `WIN10XBOX_WINEDEBUG=+win,+msg` turns on Wine's own user32 traces.
