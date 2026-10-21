# winexbox

Display and input backend for the Wine-derived `win32u` (docs/ARCHITECTURE.md,
decision E5; backend choice D8). Milestone M3 lands the **interface** and a
**headless backend**. The real Xbox backend (CoreWindow + D3D12) and the
binding that plugs the interface into win32u are deliberately deferred until
Xbox probe data exists (Probe I).

```
 guest user32/gdi32  ->  win32u (window manager, message queues, dibdrv GDI)
                              |  struct user_driver_funcs   (Wine C ABI)
                        winexbox.drv           <- DEFERRED: thin C adapter
                              |  winexbox::IWinexboxDriver  (this directory)
              +---------------+----------------+
        headless backend (done)        D3D12/CoreWindow backend (DEFERRED)
```

## Layout

| Path | What |
| --- | --- |
| `include/winexbox/driver.h` | `IWinexboxDriver`, `ISurface`, `IEventSink`, value types, factory declarations |
| `src/headless.cpp` | Headless backend + test/introspection helpers |
| `src/registry_stub.cpp` | `SelectBackend()`: backend selection from `config::display_backend` |
| `CMakeLists.txt` | static library `winexbox` (depends on `win10xbox_core` for trace + config; no UWP) |
| `tests/unit/test_winexbox.cpp` | unit tests (ctest name `unit.winexbox`; run `test_core winexbox_`) |

Trace channel: `winex` (`WIN10XBOX_DEBUG=+winex`).

## The interface

`driver.h` has no `<windows.h>` and no Wine headers; handles (HWND, HCURSOR,
HDC) cross it as `uint64_t`. Method names avoid Win32 API names on purpose
(`WindowCreate`, `CursorSet`, ...) because `<windows.h>` `#define`s several of
them to their `...W` variants.

Every method carries a phase tag in the header:

* **M3-REQUIRED**: needed to create a window and pump messages (what headless implements fully).
* **PRESENT**: only matters once pixels reach a screen.
* **DEFERRED**: Wine has a hook, but the null driver's default is acceptable for now.

### Key design point: the driver does not rasterise

win32u already contains Wine's DIB engine (`dlls/win32u/dibdrv/`). GDI calls on a
window DC are rendered by dibdrv into the bitmap behind that window's
`struct window_surface`. A display driver therefore supplies only:

1. the **pixel memory** (`CreateWindowSurface` -> `ISurface::Bits()`, BGRA32 top-down), and
2. a **notification when pixels changed** (`ISurface::Flush`, driven by win32u after painting).

Presentation is the only thing that differs between backends.

### Methods and the win32u call they serve

| `IWinexboxDriver` method | Wine `user_driver_funcs` entry | Called from (win32u) | Phase |
| --- | --- | --- | --- |
| `WindowCreate` | `pCreateWindow` | `window.c` NtUserCreateWindowEx | M3 |
| `WindowDestroy` | `pDestroyWindow` | `window.c` destroy_window / cleanup | M3 |
| `DesktopSetWindow`, `DesktopCreate` | `pSetDesktopWindow`, `pCreateDesktop` | `window.c`, `winstation.c` | M3 |
| `WindowPosChanging` | `pWindowPosChanging` | `window.c` apply_window_pos | M3 |
| `WindowPosChanged` | `pWindowPosChanged` | `window.c` apply_window_pos | M3 |
| `WindowShow` | `pShowWindow` | `window.c` show_window | M3 |
| `WindowSetParent` | `pSetParent` | `window.c` set_parent | M3 |
| `WindowSetStyle` | `pSetWindowStyle` | `window.c` NtUserSetWindowLong | M3 |
| `WindowSetText` | `pSetWindowText` | `defwnd.c` WM_SETTEXT | M3 |
| `WindowSetRegion` | `pSetWindowRgn` | `window.c` NtUserSetWindowRgn | M3 |
| `WindowActivate`, `WindowSetCapture` | `pActivateWindow`, `pSetCapture` | `input.c` | M3 |
| `WindowSetLayered` | `pSetLayeredWindowAttributes` (+`pUpdateLayeredWindow`) | `window.c` | PRESENT |
| `CreateWindowSurface`, `WindowSurface` | `pCreateWindowSurface` | `dce.c` / `window.c` | M3 |
| `QueryDeviceCaps` | `dc_funcs.pGetDeviceCaps` (and `pCreateDC`/`pDeleteDC` for "DISPLAY") | gdi `dc.c` | M3 |
| `DcAcquire`, `DcRelease` | `pGetDC`, `pReleaseDC` | `dce.c` | M3 |
| `Present`, `Invalidate`, `ISurface::Flush` | `window_surface_funcs::flush` | win32u paint path | PRESENT |
| `SetEventSink`, `ProcessEvents` | `pProcessEvents` | `message.c` process_driver_events (QS_DRIVER) | M3 |
| `SetWakeCallback`, `WakeHandle` | (queue wake) set `QS_DRIVER` internal bit | `message.c` wait_message | M3 |
| `ThreadDetach` | `pThreadDetach` | `sysparams.c` thread teardown | M3 |
| `InjectInput` (+ sink `OnInput`) | feeds `NtUserSendHardwareInput` | `input.c` | M3 |
| `CursorGetPos`, `CursorSetPos`, `CursorClip` | `pGetCursorPos`, `pSetCursorPos`, `pClipCursor` | `input.c`, `message.c` | M3 |
| `CursorSet`, `CursorDestroy` | `pSetCursor`, `pDestroyCursorIcon` | `cursoricon.c` | PRESENT |
| `QueryDisplay` | `pUpdateDisplayDevices` (`gdi_device_manager`: add_gpu/add_source/add_monitor/add_modes) | `sysparams.c` update_display_cache | M3 |
| `ChangeDisplayMode` | `pChangeDisplaySettings` | `sysparams.c` | M3 |

Not in the interface yet (null-driver defaults are acceptable; add when needed):
`pFlashWindowEx`, `pSetWindowIcons`, `pSysCommand`, `pWindowMessage`,
`pDesktopWindowProc`, `pGetWindowStyleMasks`, `pGetWindowStateUpdates`,
`pMoveWindowBits`, `pScrollDC`, keyboard layout hooks (`pToUnicodeEx`,
`pMapVirtualKeyEx`, `pGetKeyNameText`, `pKbdLayerDescriptor`: the null driver
returns "use default" so win32u's tables are used), IME, clipboard, systray,
hotkeys, wintab, `pSystemParametersInfo`, `pBeep`, `pOpenGLInit`, `pVulkanInit`.
Note Wine 11.9 no longer has `__wine_get_wgl_driver`; GL/Vulkan are reached
through `pOpenGLInit`/`pVulkanInit`.

### Message pump contract

`InjectInput()` is the only thread-safe producer. It queues the event, sets the
wake event (`WakeHandle()`, manual-reset) and calls the wake callback. Delivery
happens **only inside `ProcessEvents()`** on the window thread, into
`IEventSink::OnInput`. When the queue drains the wake event is reset. A mouse
move updates the tracked cursor position at injection time (clamped to the clip
rect or monitor); key events without a target go to the focus window
(`WindowActivate`), mouse events to the capture window else focus.

## Headless backend

`CreateHeadlessDriver(cfg)`; also returned by `SelectBackend()`.

* Windows are in-memory records (parent, rects, style, text, region, visibility, surface).
* Surfaces are plain heap BGRA32 buffers (zero-initialised, dirty rect tracking); `Flush` and `Present` only count and clear dirty state.
* One virtual primary monitor (`\\.\DISPLAY1`), 1920x1080@60 by default, modes 3840x2160/2560x1440/1920x1080/1280x720; `ChangeDisplayMode` switches among them (else `DISP_CHANGE_BADMODE`, -2). GPU is reported as the Microsoft Basic Render Driver ids (1414:008c).
* Input comes only from `InjectInput`/`InjectWindowEvent`.
* Introspection for tests/tools: `GetHeadlessStats`, `GetHeadlessWindowInfo`, `FillSurfaceRect`, `ReadSurfacePixel`.
* No thread_local, no compiler builtins; builds with llvm-mingw (clang) and MSVC.

`SelectBackend(cfg)` maps `DisplayBackend::Headless` to headless and
`DisplayBackend::D3D12` to headless **plus a FIXME** until the D3D12 backend
exists (D8).

## What the Xbox backend (CoreWindow + D3D12) implements

Same interface, different guts. All of the following is **deferred** until Probe I
reports D3D12 feature level, d3d11on12/GLOn12 availability and swap chain on CoreWindow:

* `CreateWindowSurface`: still returns a system-memory BGRA32 `ISurface` (dibdrv draws on the CPU). Optionally an upload-heap-backed buffer to avoid a copy.
* `ISurface::Flush` / `Present`: copy dirty rects into a texture and composite onto the swap chain of the app's single `CoreWindow` (a UWP app has exactly one top-level surface, so guest top-level windows are composited into it by z-order from `WindowPosChanged`, honouring `WindowSetRegion` and layered alpha).
* `InjectInput`: fed by the CoreWindow/`Windows.Gaming.Input` pump: `KeyDown/KeyUp` (vk + scan), pointer to `MouseMove/Button/Wheel`, gamepad to either XInput state (outside this interface) or synthesized keys. Activation/size changes arrive as `InjectWindowEvent`.
* `QueryDisplay` / `QueryDeviceCaps` / `ChangeDisplayMode`: from `DisplayInformation` / `HdmiDisplayInformation` (4K mode switch), HDR flag, DPI from scale factor.
* `CursorSet`: CoreWindow `PointerCursor`; `CursorClip`: pointer capture.
* `SetWakeCallback`/`WakeHandle`: the same contract; the pump thread signals when CoreWindow events arrive.
* Later (not in the interface yet): `pOpenGLInit` (GLOn12), `pVulkanInit`, clipboard, keyboard layout.

## win32u binding plan (deferred)

1. Build `winexbox.drv` as a tiny PE module (or in-process static code, per E5) whose init does what
   Wine drivers do: fill a `user_driver_funcs`, start from `__wine_set_user_driver` semantics
   (`WINE_GDI_DRIVER_VERSION` = 108 in the vendored tree; it must match exactly) and register it.
   win32u picks the driver in `driver.c` (`load_desktop_driver` reading the `GraphicsDriver`
   value, then `NtUserLoadDriver` callback), so winexbox is registered under that registry value.
2. Each `user_driver_funcs` slot becomes a trampoline that converts HWND/HRGN/RECT/`window_rects`
   into the types in `driver.h` and calls the global `IWinexboxDriver`. Unwired slots keep
   the null driver's function (copy `null_user_driver`, then override).
3. `pCreateWindowSurface`: call `IWinexboxDriver::CreateWindowSurface`, wrap its `Bits()` in a
   section-less DIB and `window_surface_create()` (gdi_driver.h) with `window_surface_funcs.flush`
   calling `ISurface::Flush`. Keep the `shared_ptr<ISurface>` alive for the surface's lifetime.
4. `pProcessEvents` calls `ProcessEvents`; the sink's `OnInput` calls
   `NtUserSendHardwareInput(hwnd, 0, &INPUT, 0)`; the wake callback sets the queue's `QS_DRIVER`
   internal bit (the in-process port of `server/queue.c` replaces `set_queue_bits`).
5. `pUpdateDisplayDevices` forwards `QueryDisplay` to `gdi_device_manager.add_gpu/add_source/add_monitor/add_modes`.
6. First end-to-end target: guest `CreateWindowExW` + `GetMessage` loop + a `GetDC`/`FillRect`
   round trip, read back through `IWinexboxDriver::WindowSurface`, on the headless backend, in CI.
