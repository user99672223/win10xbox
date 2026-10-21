// winexbox display/input backend contract (milestone M3, docs/DECISIONS.md D8/E5).
//
// This is the interface between the Wine-derived win32u (window manager, GDI,
// message queues; ported in-process per E5) and whatever actually owns the
// screen and the input devices. It is a C++ rendering of the subset of Wine's
// `struct user_driver_funcs` (include/wine/gdi_driver.h, WINE_GDI_DRIVER_VERSION
// 108, Wine 11.9) that matters for "windows exist, messages pump, pixels land
// somewhere". The glue that adapts win32u's C function table onto this
// interface (`winexbox.drv`) is deferred; see winexbox/README.md.
//
// Design rules
//  * No <windows.h>, no Wine headers: only <cstdint>/STL, so the interface can
//    be implemented and tested without a GDI stack and compiled by MSVC and
//    llvm-mingw alike. Handles cross the boundary as 64-bit integers.
//  * GDI rasterisation is NOT the driver's job. Wine's dibdrv (win32u/dibdrv/)
//    draws into the bitmap of a `window_surface`; the driver only supplies the
//    pixel memory (CreateWindowSurface) and is told when pixels changed
//    (ISurface::Flush). That is why this interface is small.
//  * Every method is tagged with the phase it belongs to:
//      [M3-REQUIRED]   needed to create a window and pump messages (headless)
//      [PRESENT]       needed once pixels must reach a screen (D3D12 backend)
//      [DEFERRED]      Wine has a hook, the null driver's default is acceptable
//                      until a real backend needs it
//  * Threading: methods are called by win32u on the thread that owns the
//    window, except InjectInput()/WakeHandle() which may be called from any
//    thread (input pump, UI thread, tests).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "win10xbox/config.h"

namespace winexbox {

// ---------------------------------------------------------------------------
// Basic types
// ---------------------------------------------------------------------------

// An HWND, widened. 0 means "no window" (HWND_DESKTOP / NULL).
using WindowId = std::uint64_t;
// A cursor handle (HCURSOR) as seen by win32u; opaque to the driver.
using CursorId = std::uint64_t;

struct Rect {                       // same layout and semantics as RECT
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
    constexpr std::int32_t Width() const { return right - left; }
    constexpr std::int32_t Height() const { return bottom - top; }
    constexpr bool Empty() const { return right <= left || bottom <= top; }
    friend constexpr bool operator==(const Rect& a, const Rect& b) {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }
};

struct Point { std::int32_t x = 0, y = 0; };

// struct window_rects: all in virtual-screen coordinates.
struct WindowRects {
    Rect window;    // including non-client frame
    Rect client;    // excluding non-client frame
    Rect visible;   // part currently visible on the host screen, backed by a surface
};

// A window region (SetWindowRgn): a union of rectangles, as from GetRegionData.
struct Region { std::vector<Rect> rects; };

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

struct DisplayMode {                // DEVMODEW subset
    std::uint32_t width = 0, height = 0;
    std::uint32_t bits_per_pixel = 32;
    std::uint32_t refresh_hz = 60;
    friend bool operator==(const DisplayMode& a, const DisplayMode& b) {
        return a.width == b.width && a.height == b.height &&
               a.bits_per_pixel == b.bits_per_pixel && a.refresh_hz == b.refresh_hz;
    }
};

struct GpuInfo {                    // gdi_device_manager::add_gpu
    std::string   name;
    std::uint16_t pci_vendor = 0, pci_device = 0, pci_subsystem = 0, pci_revision = 0;
};

struct MonitorInfo {                // gdi_monitor + add_source / add_modes
    std::string               device_name;    // "\\.\DISPLAY1"
    Rect                      monitor;        // rc_monitor, virtual-screen coords
    Rect                      work;           // rc_work
    std::uint32_t             dpi = 96;
    bool                      primary = false;
    bool                      hdr_enabled = false;
    DisplayMode               current;
    std::vector<DisplayMode>  modes;
};

struct DisplayConfig {              // everything UpdateDisplayDevices() reports
    GpuInfo                  gpu;
    std::vector<MonitorInfo> monitors;
};

// Subset of GetDeviceCaps() answers for the display DC (dc_funcs.pGetDeviceCaps).
struct DeviceCaps {
    std::int32_t horz_res = 0, vert_res = 0;          // HORZRES / VERTRES
    std::int32_t horz_size_mm = 0, vert_size_mm = 0;  // HORZSIZE / VERTSIZE
    std::int32_t bits_pixel = 32, planes = 1;         // BITSPIXEL / PLANES
    std::int32_t log_pixels_x = 96, log_pixels_y = 96;
    std::int32_t vref_hz = 60;                        // VREFRESH
};

// ---------------------------------------------------------------------------
// Surfaces (what dibdrv draws into)
// ---------------------------------------------------------------------------

enum class PixelFormat : std::uint8_t {
    Bgra8888,       // 32 bpp, B,G,R,A bytes; the only format the headless driver hands out
};

// A window's backing store. Corresponds to Wine's `struct window_surface`
// (gdi_driver.h): `rect` is the surface rectangle, `bits` the DIB memory the
// win32u binding wraps in a section-less DIB so dibdrv can draw into it, and
// Flush() is `window_surface_funcs::flush`.
class ISurface {
public:
    virtual ~ISurface() = default;

    // Pixel memory, top-down, Stride() bytes per row. Valid until the surface
    // is destroyed. Never null for a surface with a non-empty rect.
    virtual std::uint8_t* Bits() = 0;
    virtual std::int32_t  Width() const = 0;
    virtual std::int32_t  Height() const = 0;
    virtual std::int32_t  Stride() const = 0;
    virtual PixelFormat   Format() const = 0;
    // The window-space rectangle this surface covers (surface->rect).
    virtual Rect          SurfaceRect() const = 0;

    // [PRESENT] Pixels in `dirty` (surface-local coordinates; null = whole
    // surface) changed; make them visible. Headless: records and discards.
    // This is where a CoreWindow/D3D12 backend copies into its swap chain.
    virtual void Flush(const Rect* dirty) = 0;
};

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

enum class InputKind : std::uint8_t {
    KeyDown, KeyUp,                 // vk + scan (+ extended flag)
    MouseMove,                      // absolute x,y in virtual-screen coordinates
    MouseButtonDown, MouseButtonUp, // button = MouseButton
    MouseWheel,                     // wheel_delta in WHEEL_DELTA (120) units
};

enum class MouseButton : std::uint8_t { Left = 1, Right = 2, Middle = 3, X1 = 4, X2 = 5 };

enum InputFlags : std::uint32_t {
    kInputExtendedKey = 0x1,        // KEYEVENTF_EXTENDEDKEY
    kInputHorizontal  = 0x2,        // wheel is horizontal (HWHEEL)
};

// A hardware-level event, the data win32u wants in NtUserSendHardwareInput's
// `INPUT` (keybd: wVk/wScan/dwFlags; mouse: dx,dy,mouseData,dwFlags).
struct InputEvent {
    InputKind     kind = InputKind::KeyDown;
    WindowId      target = 0;       // 0 = whatever has focus / is under the cursor
    std::uint16_t vk = 0;           // virtual key (keyboard)
    std::uint16_t scan = 0;         // set-1 scan code (keyboard)
    MouseButton   button = MouseButton::Left;
    std::int32_t  x = 0, y = 0;     // mouse position
    std::int32_t  wheel_delta = 0;
    std::uint32_t flags = 0;        // InputFlags
    std::uint32_t time_ms = 0;      // 0 = stamp with driver clock on injection
};

// ---------------------------------------------------------------------------
// Events from the driver up into win32u
// ---------------------------------------------------------------------------

// Implemented by the win32u binding (winexbox.drv). The driver calls it ONLY
// from inside ProcessEvents() (i.e. on the window thread, like X11's
// process_events), never from InjectInput()'s caller thread.
class IEventSink {
public:
    virtual ~IEventSink() = default;
    // Binding turns this into NtUserSendHardwareInput(hwnd, 0, &INPUT, lparam).
    virtual void OnInput(const InputEvent& ev) = 0;
    // The host moved/resized/closed a window behind win32u's back. The binding
    // answers with NtUserSetWindowPos / WM_SYSCOMMAND(SC_CLOSE), the usual
    // pGetWindowStateUpdates / WM_CLOSE paths. Optional; headless never emits
    // anything but what tests inject through InjectWindowEvent().
    enum class WindowEvent : std::uint8_t { CloseRequested, Moved, Resized, Focus, Unfocus };
    virtual void OnWindowEvent(WindowId, WindowEvent, const Rect&) {}
};

// Called (possibly from any thread) when ProcessEvents() has work. The binding
// uses it to set the QS_DRIVER queue bit on the window thread's queue.
using WakeCallback = void (*)(void* ctx);

// ---------------------------------------------------------------------------
// The driver
// ---------------------------------------------------------------------------

enum class ShowCmd : std::int32_t {  // SW_*
    Hide = 0, ShowNormal = 1, Minimize = 2, Maximize = 3, ShowNoActivate = 4, Show = 5,
    Restore = 9, ShowDefault = 10,
};

// Return value of IWinexboxDriver::ShowWindow meaning "I did nothing, apply the
// default SetWindowPos flags" (the `~0` of user_driver_funcs::pShowWindow).
inline constexpr std::uint32_t kShowDefault = ~0u;

class IWinexboxDriver {
public:
    virtual ~IWinexboxDriver() = default;

    virtual const char* Name() const = 0;       // "headless", "d3d12"

    // ===== Window lifecycle ===================================================

    // [M3-REQUIRED] user_driver_funcs::pCreateWindow(HWND). Called from
    // NtUserCreateWindowEx (window.c) for every window, including the desktop
    // window. Return false to fail the CreateWindow. message-only windows
    // (HWND_MESSAGE parent) are also announced; a backend may ignore them.
    virtual bool CreateWindow(WindowId hwnd) = 0;

    // [M3-REQUIRED] pDestroyWindow(HWND). Release every resource for `hwnd`,
    // including its surface reference.
    virtual void DestroyWindow(WindowId hwnd) = 0;

    // [M3-REQUIRED] pSetDesktopWindow(HWND) / pCreateDesktop(name, w, h).
    // The desktop window is the root whose rect is the virtual screen.
    virtual void SetDesktopWindow(WindowId hwnd) = 0;
    virtual bool CreateDesktop(const std::u16string& name, std::uint32_t width, std::uint32_t height) = 0;

    // [M3-REQUIRED] pWindowPosChanging(hwnd, swp_flags, shaped, rects): about
    // to change geometry. Return false => win32u does not allocate a surface
    // for this window (offscreen / iconic / zero-sized).
    virtual bool WindowPosChanging(WindowId hwnd, std::uint32_t swp_flags, bool shaped,
                                   const WindowRects& rects) = 0;

    // [M3-REQUIRED] pWindowPosChanged(hwnd, insert_after, owner_hint,
    // swp_flags, rects, surface): geometry/z-order is now final; `surface` is
    // the (possibly new) surface for the visible rect, or null.
    virtual void WindowPosChanged(WindowId hwnd, WindowId insert_after, WindowId owner_hint,
                                  std::uint32_t swp_flags, const WindowRects& rects,
                                  ISurface* surface) = 0;

    // [M3-REQUIRED] pShowWindow(hwnd, cmd, &rect, swp): return kShowDefault or
    // the SWP flags to use instead.
    virtual std::uint32_t ShowWindow(WindowId hwnd, ShowCmd cmd, Rect* new_rect, std::uint32_t swp) = 0;

    // [M3-REQUIRED] pSetParent(hwnd, new_parent, old_parent).
    virtual void SetParent(WindowId hwnd, WindowId new_parent, WindowId old_parent) = 0;

    // [M3-REQUIRED] pSetWindowStyle(hwnd, offset(GWL_STYLE/GWL_EXSTYLE), style).
    virtual void SetWindowStyle(WindowId hwnd, std::int32_t offset, std::uint32_t new_style,
                                std::uint32_t old_style) = 0;

    // [M3-REQUIRED] pSetWindowText(hwnd, text).
    virtual void SetWindowText(WindowId hwnd, const std::u16string& text) = 0;

    // [M3-REQUIRED] pSetWindowRgn(hwnd, hrgn, redraw). `region` null clears it.
    virtual void SetWindowRgn(WindowId hwnd, const Region* region, bool redraw) = 0;

    // [PRESENT] pSetLayeredWindowAttributes / pUpdateLayeredWindow: layered
    // (per-window alpha / colour key) composition. Headless stores nothing.
    virtual void SetLayeredWindowAttributes(WindowId hwnd, std::uint32_t color_key,
                                            std::uint8_t alpha, std::uint32_t flags) = 0;

    // [M3-REQUIRED] pActivateWindow(hwnd, previous): foreground change.
    virtual void ActivateWindow(WindowId hwnd, WindowId previous) = 0;
    // [M3-REQUIRED] pSetCapture(hwnd, gui_flags).
    virtual void SetCapture(WindowId hwnd, std::uint32_t gui_flags) = 0;
    // [DEFERRED] pFlashWindowEx, pSetWindowIcons, pSysCommand, pWindowMessage,
    // pDesktopWindowProc, pGetWindowStyleMasks, pGetWindowStateUpdates,
    // pMoveWindowBits: null-driver defaults are fine until a host shell exists.

    // ===== Surfaces / DC bring-up ============================================

    // [M3-REQUIRED] pCreateWindowSurface(hwnd, layered, surface_rect, &surface).
    // Called by win32u (dce.c, apply_window_pos) whenever a window needs
    // backing pixels. Returns a BGRA32 surface of surface_rect's size, or null
    // for "no surface" (win32u then does not draw the window). The driver keeps
    // its own reference too (WindowSurface(hwnd)).
    virtual std::shared_ptr<ISurface> CreateWindowSurface(WindowId hwnd, bool layered,
                                                          const Rect& surface_rect) = 0;
    // The surface currently attached to `hwnd` (null if none). Used by tests and
    // by the presentation path to find what to flush.
    virtual std::shared_ptr<ISurface> WindowSurface(WindowId hwnd) const = 0;

    // [M3-REQUIRED] dc_funcs.pCreateDC / pDeleteDC / pGetDeviceCaps for the
    // "DISPLAY" device: the driver reports caps; win32u/dibdrv does the drawing.
    virtual DeviceCaps GetDeviceCaps() const = 0;

    // [M3-REQUIRED] pGetDC(hdc, hwnd, top_win, win_rect, top_rect, flags) /
    // pReleaseDC(hwnd, hdc): a window DC was handed out / returned. The
    // headless driver tracks the count only; X11 uses it to pick a drawable.
    virtual void GetDC(std::uint64_t hdc, WindowId hwnd, WindowId top_win,
                       const Rect& win_rect, const Rect& top_rect, std::uint32_t flags) = 0;
    virtual void ReleaseDC(WindowId hwnd, std::uint64_t hdc) = 0;

    // [PRESENT] Flush every dirty surface to the screen. In Wine this is
    // window_surface_funcs::flush driven by win32u after painting; the
    // binding calls it at the end of a paint batch. Headless: bookkeeping only.
    virtual void Present() = 0;
    // [PRESENT] Mark a window-local rect dirty (what the host shows is stale).
    // Equivalent of window_surface->bounds growing after a draw.
    virtual void Invalidate(WindowId hwnd, const Rect* window_rect) = 0;

    // ===== Message pump / wake ===============================================

    // [M3-REQUIRED] Where ProcessEvents() delivers. Set before the first pump.
    virtual void SetEventSink(IEventSink* sink) = 0;

    // [M3-REQUIRED] pProcessEvents(mask): drain pending driver events and hand
    // each to the sink. Called from win32u's message loop (message.c,
    // process_driver_events) when QS_DRIVER is set. `mask` is the QS_* mask
    // the caller is waiting for. Returns true if any event was delivered.
    virtual bool ProcessEvents(std::uint32_t mask) = 0;

    // [M3-REQUIRED] How win32u learns there is something to pump. Either:
    //  - callback: invoked from InjectInput()'s thread; the binding sets the
    //    QS_DRIVER bit on the queue (wine: set_queue_bits), or
    //  - WakeHandle(): a manual-reset kernel event (HANDLE as void*) that is
    //    signalled while events are pending, usable as an extra wait handle in
    //    NtUserMsgWaitForMultipleObjectsEx. Reset when the queue drains.
    virtual void SetWakeCallback(WakeCallback cb, void* ctx) = 0;
    virtual void* WakeHandle() const = 0;

    // [M3-REQUIRED] pThreadDetach(): the calling thread's window thread exited.
    virtual void ThreadDetach() = 0;

    // ===== Input ==============================================================

    // [M3-REQUIRED] Thread-safe producer side. Queues `ev` for the next
    // ProcessEvents() and fires the wake callback / signals WakeHandle().
    // This is the entry point for the host's input pump (CoreWindow key and
    // pointer events, XInput/Windows.Gaming.Input mapping) and for tests.
    virtual void InjectInput(const InputEvent& ev) = 0;
    // Test/host hook: queue a window event for IEventSink::OnWindowEvent.
    virtual void InjectWindowEvent(WindowId hwnd, IEventSink::WindowEvent ev, const Rect& rect) = 0;

    // [M3-REQUIRED] Pointer: pGetCursorPos / pSetCursorPos / pClipCursor.
    virtual bool GetCursorPos(Point* pt) = 0;
    virtual bool SetCursorPos(std::int32_t x, std::int32_t y) = 0;
    virtual bool ClipCursor(const Rect* clip, bool reset) = 0;
    // [PRESENT] pSetCursor(hwnd, hcursor) / pDestroyCursorIcon(hcursor): which
    // cursor image to show. Headless remembers the id only.
    virtual void SetCursor(WindowId hwnd, CursorId cursor) = 0;
    virtual void DestroyCursorIcon(CursorId cursor) = 0;
    virtual CursorId CurrentCursor() const = 0;
    // [DEFERRED] Keyboard layout hooks (pActivateKeyboardLayout, pToUnicodeEx,
    // pMapVirtualKeyEx, pGetKeyNameText, pKbdLayerDescriptor...): win32u falls
    // back to its built-in US tables when these report "use default".
    // [DEFERRED] IME (pImeToAsciiEx...), clipboard (pUpdateClipboard,
    // pClipboardWindowProc), systray, wintab, Beep, hotkeys.

    // ===== Display / monitors =================================================

    // [M3-REQUIRED] pUpdateDisplayDevices(manager, param): the monitor and GPU
    // topology win32u publishes into the registry/display cache.
    virtual DisplayConfig QueryDisplay() const = 0;

    // [M3-REQUIRED] pChangeDisplaySettings(displays, primary, hwnd, flags,
    // lparam). Returns DISP_CHANGE_* (0 = success, -2 = DISP_CHANGE_BADMODE).
    // `device_name` empty = primary monitor.
    virtual std::int32_t ChangeDisplayMode(const std::string& device_name, const DisplayMode& mode) = 0;

    // [DEFERRED] OpenGL / Vulkan: pOpenGLInit(version, opengl_funcs, &driver)
    // and pVulkanInit(version, handle, &driver). Wine 11.9 replaced the old
    // __wine_get_wgl_driver export with these; the D3D12 backend serves GL via
    // GLOn12 and Vulkan not at all (D8).
};

// ---------------------------------------------------------------------------
// Factory / selection
// ---------------------------------------------------------------------------

struct BackendConfig {
    win10xbox::config::DisplayBackend backend = win10xbox::config::DisplayBackend::Headless;
    std::uint32_t width = 1920, height = 1080;   // virtual monitor size (headless)
    std::uint32_t refresh_hz = 60;
};

// Always available: in-memory windows, system-memory BGRA surfaces, programmatic
// input, one virtual monitor, no presentation.
std::unique_ptr<IWinexboxDriver> CreateHeadlessDriver(const BackendConfig& cfg = {});

// Picks the backend from `cfg.backend` (mirrors config::display_backend, D8).
// DisplayBackend::D3D12 is not implemented yet: it logs a FIXME and returns
// the headless driver, so callers can already pass the real config through.
std::unique_ptr<IWinexboxDriver> SelectBackend(const BackendConfig& cfg);
// Convenience: derive from the process-wide win10xbox::config::Current().
std::unique_ptr<IWinexboxDriver> SelectBackend();


// ---------------------------------------------------------------------------
// Headless introspection (tests, tools). Valid only for drivers whose Name()
// is "headless"; other backends yield zeros / false.
// ---------------------------------------------------------------------------

struct HeadlessStats {
    std::uint64_t windows_created = 0, windows_destroyed = 0;
    std::uint64_t surfaces_created = 0;
    std::uint64_t flushes = 0;          // ISurface::Flush calls (the no-op "present")
    std::uint64_t presents = 0;         // Present() calls
    std::uint64_t events_injected = 0, events_delivered = 0;
    std::uint64_t dcs_outstanding = 0;  // GetDC minus ReleaseDC
};
HeadlessStats GetHeadlessStats(const IWinexboxDriver& drv);

struct HeadlessWindowInfo {
    WindowId      parent = 0;
    WindowRects   rects;
    bool          visible = false;
    bool          has_region = false;
    std::u16string text;
    std::uint32_t style = 0, ex_style = 0;
    bool          is_desktop = false;
};
bool GetHeadlessWindowInfo(const IWinexboxDriver& drv, WindowId hwnd, HeadlessWindowInfo* out);

// Software helper shared by tests and the future win32u binding's smoke path:
// fill a surface-local rect (clipped) with a BGRA pixel value 0xAARRGGBB and
// mark it dirty. Stands in for what dibdrv does to the window bitmap.
void FillSurfaceRect(ISurface& surface, const Rect& rect, std::uint32_t argb);
// Read one pixel (0xAARRGGBB); returns 0 for out-of-range.
std::uint32_t ReadSurfacePixel(const ISurface& surface, std::int32_t x, std::int32_t y);

}  // namespace winexbox
