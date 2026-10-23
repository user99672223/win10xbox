// win10xbox user module (core/user): the win32k surface -- NtUser* / NtGdi* --
// that Wine's PE user32.dll and gdi32.dll reach through the syscall thunks of
// win32u.dll. In Wine that surface is implemented in win32u.so (the unix side)
// plus wineserver; neither exists on Windows, so the dispatcher routes every
// win32u syscall (ids 0x1000..) to the Emulate implementations in this module
// (docs/GUI.md, docs/ARCHITECTURE.md "EMULATE: win32k").
//
// What it owns: GDI objects and the shared handle table (PEB.GdiSharedHandleTable),
// device contexts and the software rasteriser, window classes, windows, z-order,
// per-thread message queues, timers, painting (BeginPaint / invalidation),
// system metrics, and the user-mode callbacks into user32 (KernelCallbackTable).
//
// What it does not own: pixels on a screen. Window surfaces are allocated from
// the winexbox display driver (winexbox/include/winexbox/driver.h), input comes
// from the same driver, and a host presents the composed frame (ComposeFrame)
// however it likes: host-win32 writes a .bmp, host-uwp copies it into a swap
// chain on the CoreWindow.
//
// Thread safety: every entry point takes the module's lock; the host may call
// ComposeFrame / WriteScreenshot from any thread at any time.
#pragma once
#include <cstdint>
#include <string>

namespace winexbox { class IWinexboxDriver; class IEventSink; }

namespace win10xbox::user {

// Host wiring, before process::Launch. Null (the default) means "no display":
// window creation fails. The pointer must stay valid for the life of the process.
void SetDisplayDriver(winexbox::IWinexboxDriver* driver);
winexbox::IWinexboxDriver* DisplayDriver();

// The module's input sink: turns driver input events (IEventSink::OnInput) into
// WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_KEYDOWN ... posted to the window under the
// cursor (or with the capture) and to the focus window of the foreground thread.
// Hosts set it on their driver (SetEventSink) or forward their own sink's calls
// to it. It only posts, so it may run on any thread; GUI threads also drain the
// driver queue themselves (ProcessEvents) while they wait for messages.
winexbox::IEventSink* InputSink();

// Composes the desktop (background colour + every visible top-level window,
// bottom to top in z-order) into a BGRA8888 top-down buffer of width x height
// pixels with `stride_bytes` per row. Returns false when there is nothing to
// compose (no driver, module never initialised). Any thread.
bool ComposeFrame(std::uint32_t* bgra, std::int32_t width, std::int32_t height, std::int32_t stride_bytes);

// The same composition for the frame as it was at the guest's most recent
// paint (the last surface flush), even if the windows have been hidden or
// destroyed since: what was last drawn on the virtual screen. This is what
// WriteScreenshot writes, so a program that tears its window down before it
// exits still leaves its last picture behind. Any thread.
bool ComposeLastPaintedFrame(std::uint32_t* bgra, std::int32_t width, std::int32_t height, std::int32_t stride_bytes);

// Monotonic counter of "something on screen changed" (a window surface was
// flushed, a window was shown/moved/destroyed). A presenter may skip frames
// while it does not move.
std::uint64_t FrameSerial();

// The virtual screen size the module composes for (the driver's primary
// monitor). Zero before the first NtUser call.
void ScreenSize(std::int32_t* width, std::int32_t* height);

// The one display mode every display answer of the layer comes from (EnumDisplaySettings, GetDeviceCaps
// HORZRES/VERTRES/VREFRESH/BITSPIXEL, GetSystemMetrics SM_CXSCREEN, the monitor rect, and the DXGI output of the
// emulated windows): the current mode of the driver's primary monitor, set by the host when it creates the driver.
// False (all zero) before a driver exists.
bool CurrentDisplayMode(std::uint32_t* width, std::uint32_t* height, std::uint32_t* refresh_hz,
                        std::uint32_t* bits_per_pixel);

// The sync interval of the guest's most recent IDXGISwapChain::Present (1 until it presents). The host-uwp presenter
// presents with tearing (VRR) whenever this is 0. Any thread.
void NoteGuestPresent(unsigned sync_interval);
unsigned LastGuestSyncInterval();

// Writes ComposeLastPaintedFrame() as a 32-bit BMP. Used by host-win32 (WIN10XBOX_SCREENSHOT=
// <path>, written when the guest has exited) and by host-uwp (LocalState\screen.bmp).
bool WriteScreenshot(const std::wstring& path);

// ---- Direct3D swap chains of a route-B guest (core/dispatch/dxgi_interpose.cpp) -----------------------------
// host-win32: after each successful Present of a swap chain on window `hwnd` the layer hands the pixels of the
// back buffer it presented (top-down BGRA, `stride_bytes` per row) here; ComposeFrame / the screenshot then
// show them in the window's client rectangle. Any thread; the pixels are copied.
bool NotifyClientFrame(void* hwnd, const std::uint32_t* bgra, std::int32_t width, std::int32_t height,
                       std::int32_t stride_bytes);

// host-uwp: creates the real swap chain for the guest on the presenter's CoreWindow instead of on the guest's
// window. `factory` is the real IDXGIFactory2*, `device` the guest's IUnknown* device, `desc1` a
// const DXGI_SWAP_CHAIN_DESC1*; *swap_chain receives an IDXGISwapChain1* (one reference). Returns an HRESULT.
// Null (the default): swap chains are made on the guest's own window. Set once by the host before Launch.
using SwapChainTargetFn = long (*)(void* factory, void* device, const void* desc1, void** swap_chain, void* ctx);
void SetSwapChainTarget(SwapChainTargetFn fn, void* ctx);
SwapChainTargetFn GetSwapChainTarget(void** ctx);

// Counters for tests and status files.
struct Stats {
    std::uint64_t windows_created = 0, windows_destroyed = 0;
    std::uint64_t messages_posted = 0, messages_dispatched = 0;
    std::uint64_t callbacks = 0;         // user-mode callbacks into user32
    std::uint64_t paints = 0;            // BeginPaint
    std::uint64_t flushes = 0;           // surface flushes (pixels changed)
    std::uint64_t gdi_objects = 0;       // live GDI objects
    std::uint64_t unimplemented = 0;     // win32u syscalls that returned STATUS_NOT_IMPLEMENTED
};
Stats GetStats();

}  // namespace win10xbox::user
