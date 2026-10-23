// Route B presentation (config gui = Native, docs/GUI.md "Route B: native win32k"): the guest's windows are
// real win32k windows of this process, so the frame user::ComposeFrame hands the hosts is composed from them:
// every visible top-level window created by a guest thread, bottom to top, copied out of its window DC (with
// DWM that is the window's redirection surface, whatever covers it on the real desktop) to its GetWindowRect
// position. Compiled always, selected at run time by user.cpp. Uses only the host's user32/gdi32, through the
// entry points dispatch resolved while PEB->Ldr was the host's (HostGuiFunctions).
//
// A guest's windows die with the thread that owns them, which is gone by the time host-win32 writes its
// screenshot; so every compose is also kept, and when no guest thread is left the last one is what is shown.
// The guest's NtUserEndPaint (dispatcher.cpp) composes too, so the cache holds the last painted picture.
#include "user_internal.h"
#include "win10xbox/config.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace win10xbox::user::detail {
namespace {

using dispatch::detail::HostGui;

// Namespace scope, trivially initialised (rule 3 in docs/GUI.md); the vectors are only touched under the lock.
SRWLOCK g_native_lock = SRWLOCK_INIT;
volatile LONG64 g_native_serial = 0;
std::vector<std::uint32_t>* g_last = nullptr;   // last composed frame, g_last_w x g_last_h
std::int32_t g_last_w = 0, g_last_h = 0;
std::size_t g_last_windows = static_cast<std::size_t>(-1);

// Frames a guest's Direct3D swap chain presented (NotifyClientFrame), one per window, newest copy; under g_native_lock.
struct ClientFrame {
    HWND hwnd;
    std::int32_t w, h;
    std::vector<std::uint32_t> px;
};
std::vector<ClientFrame>* g_client_frames = nullptr;

const ClientFrame* FindClientFrame(HWND hwnd) {
    if (!g_client_frames) return nullptr;
    for (const ClientFrame& f : *g_client_frames)
        if (f.hwnd == hwnd) return &f;
    return nullptr;
}

struct EnumState {
    const HostGui* h;
    std::vector<DWORD> guest_tids;
    std::vector<HWND> windows;   // top to bottom (EnumWindows order)
};

BOOL CALLBACK CollectGuestWindow(HWND hwnd, LPARAM lp) {
    auto* es = reinterpret_cast<EnumState*>(lp);
    DWORD pid = 0;
    const DWORD tid = es->h->GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    // A window that received a swap chain frame belongs to the guest even when its creating thread is not in the
    // guest thread list (route B: native windows, docs/slices/swapchain2.md).
    if (std::find(es->guest_tids.begin(), es->guest_tids.end(), tid) == es->guest_tids.end() && !FindClientFrame(hwnd))
        return TRUE;
    // The presenting window counts even when the guest's SW_SHOWDEFAULT resolved to the launcher's hidden startup
    // state (imgui_d3d11: visible 0 in CI): its swap chain frame is what the guest shows.
    if ((!es->h->IsWindowVisible(hwnd) && !FindClientFrame(hwnd)) || es->h->IsIconic(hwnd)) return TRUE;
    es->windows.push_back(hwnd);
    return TRUE;
}

// Composes the guest's windows into out (w x h, top-down BGRA, alpha 0xFF). False when no guest thread exists
// (nothing to compose live: the guest has not started or has exited).
bool ComposeLive(const HostGui& h, std::uint32_t* out, std::int32_t w, std::int32_t h_px, std::size_t* nwin) {
    EnumState es{&h, {}, {}};
    for (dispatch::detail::ThreadState* t : dispatch::detail::GuestThreads()) es.guest_tids.push_back(t->tid);
    if (es.guest_tids.empty()) return false;
    h.EnumWindows(&CollectGuestWindow, reinterpret_cast<LPARAM>(&es));
    *nwin = es.windows.size();
    if (es.windows.empty() && g_client_frames)
        for (const ClientFrame& f : *g_client_frames) {
            DWORD fpid = 0;
            const DWORD ftid = h.GetWindowThreadProcessId(f.hwnd, &fpid);
            TRACE(user, "native compose: frame for hwnd %p not composed: pid %lu (ours %lu) tid %lu visible %d iconic %d",
                  static_cast<void*>(f.hwnd), static_cast<unsigned long>(fpid), static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long>(ftid), h.IsWindowVisible(f.hwnd) ? 1 : 0, h.IsIconic(f.hwnd) ? 1 : 0);
        }
    std::memset(out, 0, static_cast<std::size_t>(w) * h_px * 4);
    if (!es.windows.empty()) {
        HDC mem = h.CreateCompatibleDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h_px;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bmp = mem ? h.CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (!bmp || !bits) {
            if (bmp) h.DeleteObject(bmp);
            if (mem) h.DeleteDC(mem);
            WARN(user, "native compose: no %dx%d DIB section", w, h_px);
            return false;
        }
        HGDIOBJ old = h.SelectObject(mem, bmp);
        std::memset(bits, 0, static_cast<std::size_t>(w) * h_px * 4);
        for (auto it = es.windows.rbegin(); it != es.windows.rend(); ++it) {   // bottom to top
            RECT r{};
            if (!h.GetWindowRect(*it, &r) || r.right <= r.left || r.bottom <= r.top) continue;
            HDC wdc = h.GetWindowDC(*it);
            if (!wdc) continue;
            h.BitBlt(mem, r.left, r.top, r.right - r.left, r.bottom - r.top, wdc, 0, 0, SRCCOPY);
            h.ReleaseDC(*it, wdc);
            // A Direct3D swap chain is presented to the compositor, not into the window's GDI surface: BitBlt of the
            // window DC then reads nothing. When the blit left the whole rectangle black, ask the compositor for the
            // window's content (PW_RENDERFULLCONTENT) into a private DIB and copy that over.
            const int rw = r.right - r.left, rh = r.bottom - r.top;
            // A swap chain's presented frame (dxgi_interpose.cpp) is the real content: it replaces the client area.
            const ClientFrame* cf = FindClientFrame(*it);
            POINT origin{0, 0};
            if (cf && h.ClientToScreen(*it, &origin)) {
                auto* dst = static_cast<std::uint32_t*>(bits);
                for (int y = 0; y < cf->h; ++y) {
                    const int sy = origin.y + y;
                    if (sy < 0 || sy >= h_px) continue;
                    for (int x = 0; x < cf->w; ++x) {
                        const int sx = origin.x + x;
                        if (sx >= 0 && sx < w) dst[static_cast<std::size_t>(sy) * w + sx] = cf->px[static_cast<std::size_t>(y) * cf->w + x];
                    }
                }
                continue;
            }
            if (h.PrintWindow && rw <= 16384 && rh <= 16384) {
                h.GdiFlush();
                bool blank = true;
                const auto* px = static_cast<const std::uint32_t*>(bits);
                for (int y = r.top < 0 ? 0 : r.top; blank && y < r.bottom && y < h_px; ++y)
                    for (int x = r.left < 0 ? 0 : r.left; x < r.right && x < w; ++x)
                        if (px[static_cast<std::size_t>(y) * w + x] & 0x00FFFFFFu) { blank = false; break; }
                if (blank) {
                    BITMAPINFO wi{};
                    wi.bmiHeader.biSize = sizeof wi.bmiHeader;
                    wi.bmiHeader.biWidth = rw;
                    wi.bmiHeader.biHeight = -rh;
                    wi.bmiHeader.biPlanes = 1;
                    wi.bmiHeader.biBitCount = 32;
                    void* wbits = nullptr;
                    HDC wmem = h.CreateCompatibleDC(nullptr);
                    HBITMAP wbmp = wmem ? h.CreateDIBSection(wmem, &wi, DIB_RGB_COLORS, &wbits, nullptr, 0) : nullptr;
                    if (wbmp && wbits) {
                        HGDIOBJ wold = h.SelectObject(wmem, wbmp);
                        if (h.PrintWindow(*it, wmem, 2 /* PW_RENDERFULLCONTENT */)) {
                            h.GdiFlush();
                            h.BitBlt(mem, r.left, r.top, rw, rh, wmem, 0, 0, SRCCOPY);
                        }
                        h.SelectObject(wmem, wold);
                    }
                    if (wbmp) h.DeleteObject(wbmp);
                    if (wmem) h.DeleteDC(wmem);
                }
            }
        }
        h.GdiFlush();
        std::memcpy(out, bits, static_cast<std::size_t>(w) * h_px * 4);
        h.SelectObject(mem, old);
        h.DeleteObject(bmp);
        h.DeleteDC(mem);
    }
    for (std::size_t i = 0, n = static_cast<std::size_t>(w) * h_px; i < n; ++i) out[i] |= 0xFF000000u;
    return true;
}

// Composes live into the cache (under the lock). Returns whether a live frame was composed.
bool RefreshLocked(const HostGui& h, std::int32_t w, std::int32_t h_px) {
    if (w <= 0 || h_px <= 0) return false;
    std::vector<std::uint32_t> frame(static_cast<std::size_t>(w) * h_px);
    std::size_t nwin = 0;
    if (!ComposeLive(h, frame.data(), w, h_px, &nwin)) return false;
    if (!g_last) g_last = new std::vector<std::uint32_t>;
    const bool changed = g_last_w != w || g_last_h != h_px || *g_last != frame;
    if (changed) InterlockedIncrement64(&g_native_serial);
    if (nwin != g_last_windows)
        TRACE(user, "native compose: %zu guest window(s) on a %dx%d screen%s", nwin, w, h_px, changed ? "" : " (unchanged)");
    g_last_windows = nwin;
    g_last->swap(frame);
    g_last_w = w;
    g_last_h = h_px;
    return true;
}

}  // namespace

bool NativeGui() { return config::Current().gui == config::Gui::Native && dispatch::detail::HostGuiFunctions(); }

void NativeScreenSize(std::int32_t* width, std::int32_t* height) {
    const HostGui* h = dispatch::detail::HostGuiFunctions();
    if (width) *width = h ? h->GetSystemMetrics(SM_CXSCREEN) : 0;
    if (height) *height = h ? h->GetSystemMetrics(SM_CYSCREEN) : 0;
}

std::uint64_t NativeFrameSerial() { return static_cast<std::uint64_t>(g_native_serial); }

bool NativeComposeFrame(std::uint32_t* bgra, std::int32_t width, std::int32_t height, std::int32_t stride_bytes) {
    const HostGui* h = dispatch::detail::HostGuiFunctions();
    if (!h || !bgra || width <= 0 || height <= 0 || stride_bytes < width * 4) return false;
    AcquireSRWLockExclusive(&g_native_lock);
    RefreshLocked(*h, width, height);   // falls back to the last frame when no guest thread is left
    const bool have = g_last && !g_last->empty();
    if (have) {
        const std::int32_t cw = std::min(width, g_last_w), ch = std::min(height, g_last_h);
        for (std::int32_t y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(bgra) +
                                                         static_cast<std::size_t>(y) * stride_bytes);
            if (y < ch) {
                std::memcpy(row, g_last->data() + static_cast<std::size_t>(y) * g_last_w, static_cast<std::size_t>(cw) * 4);
                std::fill(row + cw, row + width, 0xFF000000u);
            } else {
                std::fill(row, row + width, 0xFF000000u);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_native_lock);
    return have;
}

}  // namespace win10xbox::user::detail

namespace win10xbox::user {
namespace {
SwapChainTargetFn g_swap_target = nullptr;   // trivially initialised
void* g_swap_target_ctx = nullptr;
}  // namespace

namespace {
std::atomic<unsigned> g_guest_sync{1};
}  // namespace
void NoteGuestPresent(unsigned sync_interval) { g_guest_sync.store(sync_interval); }
unsigned LastGuestSyncInterval() { return g_guest_sync.load(); }

void SetSwapChainTarget(SwapChainTargetFn fn, void* ctx) {
    g_swap_target_ctx = ctx;
    g_swap_target = fn;
}

SwapChainTargetFn GetSwapChainTarget(void** ctx) {
    if (ctx) *ctx = g_swap_target_ctx;
    return g_swap_target;
}

bool NotifyClientFrame(void* hwnd, const std::uint32_t* bgra, std::int32_t width, std::int32_t height,
                       std::int32_t stride_bytes) {
    const dispatch::detail::HostGui* h = dispatch::detail::HostGuiFunctions();
    if (!h || !hwnd || !bgra || width <= 0 || height <= 0 || width > 16384 || height > 16384 || stride_bytes < width * 4)
        return false;
    std::vector<std::uint32_t> px(static_cast<std::size_t>(width) * height);
    for (std::int32_t y = 0; y < height; ++y)
        std::memcpy(px.data() + static_cast<std::size_t>(y) * width,
                    reinterpret_cast<const std::uint8_t*>(bgra) + static_cast<std::size_t>(y) * stride_bytes,
                    static_cast<std::size_t>(width) * 4);
    std::int32_t sw = 0, sh = 0;
    detail::NativeScreenSize(&sw, &sh);
    AcquireSRWLockExclusive(&detail::g_native_lock);
    if (!detail::g_client_frames) detail::g_client_frames = new std::vector<detail::ClientFrame>;
    auto& v = *detail::g_client_frames;
    detail::ClientFrame* slot = nullptr;
    for (auto& f : v)
        if (f.hwnd == static_cast<HWND>(hwnd)) slot = &f;
    if (!slot) {
        v.push_back({static_cast<HWND>(hwnd), 0, 0, {}});
        slot = &v.back();
    }
    slot->w = width;
    slot->h = height;
    slot->px.swap(px);
    detail::RefreshLocked(*h, sw, sh);   // the cache is what the screenshot shows once the window is gone
    InterlockedIncrement64(&detail::g_native_serial);
    ReleaseSRWLockExclusive(&detail::g_native_lock);
    return true;
}

}  // namespace win10xbox::user

namespace win10xbox::dispatch::detail {

void NativeGuestPainted() {
    const HostGui* h = HostGuiFunctions();
    if (!h) return;
    std::int32_t w = 0, hh = 0;
    user::detail::NativeScreenSize(&w, &hh);
    AcquireSRWLockExclusive(&user::detail::g_native_lock);
    user::detail::RefreshLocked(*h, w, hh);
    ReleaseSRWLockExclusive(&user::detail::g_native_lock);
}

}  // namespace win10xbox::dispatch::detail
