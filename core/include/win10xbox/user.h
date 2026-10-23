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

// Writes ComposeLastPaintedFrame() as a 32-bit BMP. Used by host-win32 (WIN10XBOX_SCREENSHOT=
// <path>, written when the guest has exited) and by host-uwp (LocalState\screen.bmp).
bool WriteScreenshot(const std::wstring& path);

// Host swap chains on emulated windows (core/dispatch/swapchain.cpp, docs/slices/swapchain.md). HWND/HDC are
// passed as void*. GetWindowGeometry: screen rectangles of an emulated window (false: not one of ours, or the
// desktop). WindowOfDc: the emulated window a GetDC/BeginPaint DC belongs to (null for memory DCs and unknown
// handles). SetClientFrameSource: ComposeFrame calls fn under the module lock (fn must not call back into this
// module) after drawing that window, to overlay its client area: dst is the top-left of the visible part, w x h
// its size, (src_x, src_y) its offset inside the client area. fn null clears. NotifyClientFrame: a new frame
// exists (FrameSerial++).
struct HostRect { std::int32_t left, top, right, bottom; };
struct WindowGeometry { HostRect window, client; bool visible, top_level; };
using ClientFrameFn = bool (*)(void* ctx, std::uint32_t* dst, std::int32_t w, std::int32_t h, std::int32_t stride_bytes,
                               std::int32_t src_x, std::int32_t src_y);
bool GetWindowGeometry(void* hwnd, WindowGeometry* out);
void* WindowOfDc(void* hdc);
bool SetClientFrameSource(void* hwnd, ClientFrameFn fn, void* ctx);
void NotifyClientFrame();

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
