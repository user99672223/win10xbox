// Emulated GUI + Direct3D: swap chains on core/user windows (docs/slices/swapchain.md).
//
// The guest's native dxgi presents with the NtGdiDdDDI* (D3DKMT) calls of win32u, which this layer forwards to the
// HOST's win32u (dispatcher.cpp UseHostD3dkmt). The host's win32k knows no emulated HWND, so:
//
//   * every emulated window that reaches a window-carrying D3DKMT call gets one HOST window (a popup, shown
//     without activation, far off-screen, created on a dedicated host thread started before the PEB take-over);
//   * NtGdiDdDDIPresent / OpenAdapterFromHdc / CheckOcclusion are translated: the emulated hWindow / hDc in the
//     argument structure is replaced by the host window / its DC for the duration of the call;
//   * after a successful present the host window is captured (PrintWindow with PW_RENDERFULLCONTENT, else BitBlt of
//     its window DC) into a DIB, and core/user overlays that DIB on the emulated window's client area whenever
//     it composes a frame (user::SetClientFrameSource), so host-win32's screenshot and host-uwp's presenter see it.
//
// Nothing is faked: a failed capture leaves the previous picture and is traced. All state is namespace-scope and
// trivially initialised (these functions run in the host view of win10xbox_dispatch, and the compose callback
// under core/user's lock: no function-local statics, no TLS, no calls back into core/user from the callback).
#include "internal.h"

#include "win10xbox/user.h"

#include <cstring>

namespace win10xbox::dispatch::detail {
namespace {

constexpr int kMaxBindings = 16;
constexpr UINT kMsgCreate = WM_APP + 1;   // lParam: Request*
constexpr UINT kMsgDestroy = WM_APP + 2;  // lParam: Request*
constexpr UINT kPrintFullContent = 0x2;   // PW_RENDERFULLCONTENT
constexpr std::uint64_t kStatusNotImplemented = 0xC0000002ull;
constexpr std::uint64_t kStatusInvalidParameter = 0xC000000Dull;

struct HostFns {
    ATOM(WINAPI* RegisterClassExW)(const WNDCLASSEXW*);
    HWND(WINAPI* CreateWindowExW)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
    BOOL(WINAPI* DestroyWindow)(HWND);
    BOOL(WINAPI* SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
    HDC(WINAPI* GetDC)(HWND);
    int(WINAPI* ReleaseDC)(HWND, HDC);
    BOOL(WINAPI* PrintWindow)(HWND, HDC, UINT);
    BOOL(WINAPI* PeekMessageW)(LPMSG, HWND, UINT, UINT, UINT);
    BOOL(WINAPI* GetMessageW)(LPMSG, HWND, UINT, UINT);
    BOOL(WINAPI* TranslateMessage)(const MSG*);
    LRESULT(WINAPI* DispatchMessageW)(const MSG*);
    LRESULT(WINAPI* DefWindowProcW)(HWND, UINT, WPARAM, LPARAM);
    BOOL(WINAPI* PostThreadMessageW)(DWORD, UINT, WPARAM, LPARAM);
    HDC(WINAPI* CreateCompatibleDC)(HDC);
    HBITMAP(WINAPI* CreateDIBSection)(HDC, const BITMAPINFO*, UINT, void**, HANDLE, DWORD);
    HGDIOBJ(WINAPI* SelectObject)(HDC, HGDIOBJ);
    BOOL(WINAPI* BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
    BOOL(WINAPI* DeleteDC)(HDC);
    BOOL(WINAPI* DeleteObject)(HGDIOBJ);
    BOOL(WINAPI* GdiFlush)();
};
HostFns g_fn;
bool g_fn_ok = false;
DWORD g_thread_id = 0;
HANDLE g_ready = nullptr;
const wchar_t kClassName[] = L"win10xbox.swapchain";

struct Request {
    HWND hwnd;
    int w, h;
    HANDLE done;
};

// One emulated window with a host window behind it.
struct Binding {
    void* guest;          // the emulated HWND as the guest holds it (null = free slot)
    HWND host;
    HDC host_dc;          // GetDC(host), for OpenAdapterFromHdc
    int host_w, host_h;   // size of the host window
    SRWLOCK lock;         // guards the capture DIB below against the compose callback
    HDC mem;
    HBITMAP dib;
    HGDIOBJ old_bitmap;
    std::uint32_t* bits;
    int w, h;             // DIB size
    LONG64 frames, failed;
};
Binding g_bind[kMaxBindings];
SRWLOCK g_bind_lock = SRWLOCK_INIT;

// The host win32u exports the overrides forward to (SetDdiHostFn).
using DdiFn = std::int32_t(NTAPI*)(void*);
DdiFn g_ddi_present, g_ddi_open_adapter_from_hdc, g_ddi_check_occlusion;
volatile LONG g_logged_present_dump = 0, g_logged_no_window = 0;

DWORD WINAPI WindowThread(void*) {
    MSG msg;
    g_fn.PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // creates this thread's queue
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = g_fn.DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    g_fn.RegisterClassExW(&wc);
    // Warm the host's window creation path while PEB->Ldr is still the host's (delay loads resolve against it).
    if (HWND warm = g_fn.CreateWindowExW(0, kClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr))
        g_fn.DestroyWindow(warm);
    SetEvent(g_ready);
    while (g_fn.GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kMsgCreate && !msg.hwnd) {
            auto* r = reinterpret_cast<Request*>(msg.lParam);
            // Popup, no activation, far off-screen: DWM still redirects it, which is what PrintWindow captures.
            r->hwnd = g_fn.CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kClassName,
                                           L"win10xbox swap chain", WS_POPUP | WS_VISIBLE, -32000, -32000, r->w, r->h,
                                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetEvent(r->done);
        } else if (msg.message == kMsgDestroy && !msg.hwnd) {
            auto* r = reinterpret_cast<Request*>(msg.lParam);
            if (r->hwnd) g_fn.DestroyWindow(r->hwnd);
            SetEvent(r->done);
        } else {
            g_fn.TranslateMessage(&msg);
            g_fn.DispatchMessageW(&msg);
        }
    }
    return 0;
}

HWND CreateHostWindow(int w, int h) {
    Request r{nullptr, w, h, CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (!r.done) return nullptr;
    if (g_fn.PostThreadMessageW(g_thread_id, kMsgCreate, 0, reinterpret_cast<LPARAM>(&r)))
        WaitForSingleObject(r.done, 10000);
    CloseHandle(r.done);
    return r.hwnd;
}

void DestroyHostWindow(HWND hwnd) {
    Request r{hwnd, 0, 0, CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (!r.done) return;
    if (g_fn.PostThreadMessageW(g_thread_id, kMsgDestroy, 0, reinterpret_cast<LPARAM>(&r)))
        WaitForSingleObject(r.done, 10000);
    CloseHandle(r.done);
}

bool ClientSize(void* guest, int* w, int* h, bool* visible) {
    user::WindowGeometry g;
    if (!user::GetWindowGeometry(guest, &g)) return false;
    *w = g.client.right - g.client.left;
    *h = g.client.bottom - g.client.top;
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    if (*w > 16384) *w = 16384;
    if (*h > 16384) *h = 16384;
    *visible = g.visible;
    return true;
}

// The compose callback (core/user lock held): the last captured frame over the client area.
bool FrameCb(void* ctx, std::uint32_t* dst, std::int32_t w, std::int32_t h, std::int32_t stride, std::int32_t sx,
             std::int32_t sy) {
    auto* b = static_cast<Binding*>(ctx);
    AcquireSRWLockShared(&b->lock);
    bool ok = b->bits && b->frames > 0;
    if (ok) {
        for (int y = 0; y < h && y + sy < b->h; ++y) {
            const std::uint32_t* src = b->bits + static_cast<std::ptrdiff_t>(y + sy) * b->w + sx;
            auto* row = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(dst) + static_cast<std::ptrdiff_t>(y) * stride);
            const int n = (sx + w <= b->w) ? w : (b->w - sx);
            for (int x = 0; x < n; ++x) row[x] = src[x] | 0xff000000u;
        }
    }
    ReleaseSRWLockShared(&b->lock);
    return ok;
}

void FreeBindingBitmap(Binding* b) {   // b->lock held exclusively
    if (b->mem && b->old_bitmap) g_fn.SelectObject(b->mem, b->old_bitmap);
    if (b->dib) g_fn.DeleteObject(b->dib);
    if (b->mem) g_fn.DeleteDC(b->mem);
    b->mem = nullptr; b->dib = nullptr; b->old_bitmap = nullptr; b->bits = nullptr; b->w = b->h = 0;
}

// The binding of an emulated window, creating it (and its host window) when `create`. Null: not an emulated window.
Binding* BindingFor(void* guest, bool create) {
    if (!guest || !g_fn_ok) return nullptr;
    AcquireSRWLockExclusive(&g_bind_lock);
    Binding* found = nullptr;
    Binding* freeslot = nullptr;
    for (Binding& b : g_bind) {
        if (b.guest == guest) { found = &b; break; }
        if (!b.guest && !freeslot) freeslot = &b;
    }
    if (!found && create) {
        int w = 0, h = 0;
        bool visible = false;
        if (ClientSize(guest, &w, &h, &visible)) {
            // Slots whose emulated window is gone are reused.
            if (!freeslot)
                for (Binding& b : g_bind) {
                    int w2, h2; bool v2;
                    if (!ClientSize(b.guest, &w2, &h2, &v2)) {
                        user::SetClientFrameSource(b.guest, nullptr, nullptr);
                        AcquireSRWLockExclusive(&b.lock);
                        FreeBindingBitmap(&b);
                        ReleaseSRWLockExclusive(&b.lock);
                        if (b.host_dc) g_fn.ReleaseDC(b.host, b.host_dc);
                        if (b.host) DestroyHostWindow(b.host);
                        b.guest = nullptr; b.host = nullptr; b.host_dc = nullptr; b.frames = b.failed = 0;
                        freeslot = &b;
                        break;
                    }
                }
            if (freeslot) {
                NoteNativeWin32uCall();   // the host's callbacks must route to the host's user32 before it makes a window
                HWND host = CreateHostWindow(w, h);
                if (host) {
                    freeslot->guest = guest;
                    freeslot->host = host;
                    freeslot->host_w = w; freeslot->host_h = h;
                    freeslot->host_dc = g_fn.GetDC(host);
                    freeslot->frames = freeslot->failed = 0;
                    user::SetClientFrameSource(guest, &FrameCb, freeslot);
                    found = freeslot;
                    TRACE(dispatch, "swapchain: emulated window %p -> host window %p (%dx%d, host dc %p)", guest,
                          static_cast<void*>(host), w, h, static_cast<void*>(found->host_dc));
                } else {
                    WARN(dispatch, "swapchain: the host refused a window for %p (error %lu)", guest, GetLastError());
                }
            } else {
                WARN(dispatch, "swapchain: %d windows already have swap chains", kMaxBindings);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_bind_lock);
    return found;
}

// After a present: size the host window to the emulated client area, then capture it.
void Capture(Binding* b) {
    int w = 0, h = 0;
    bool visible = false;
    if (!ClientSize(b->guest, &w, &h, &visible)) return;
    if (w != b->host_w || h != b->host_h) {
        g_fn.SetWindowPos(b->host, nullptr, -32000, -32000, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        b->host_w = w; b->host_h = h;
    }
    AcquireSRWLockExclusive(&b->lock);
    if (!b->dib || b->w != w || b->h != h) {
        FreeBindingBitmap(b);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        b->mem = g_fn.CreateCompatibleDC(nullptr);
        void* bits = nullptr;
        if (b->mem) b->dib = g_fn.CreateDIBSection(b->mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!b->dib) {
            FreeBindingBitmap(b);
            ReleaseSRWLockExclusive(&b->lock);
            WARN(dispatch, "swapchain: no %dx%d DIB for %p", w, h, b->guest);
            return;
        }
        b->old_bitmap = g_fn.SelectObject(b->mem, b->dib);
        b->bits = static_cast<std::uint32_t*>(bits);
        b->w = w; b->h = h;
    }
    bool ok = g_fn.PrintWindow(b->host, b->mem, kPrintFullContent) != 0;
    const char* how = "PrintWindow(PW_RENDERFULLCONTENT)";
    if (!ok && b->host_dc) {
        ok = g_fn.BitBlt(b->mem, 0, 0, w, h, b->host_dc, 0, 0, SRCCOPY) != 0;
        how = "BitBlt(window DC)";
    }
    g_fn.GdiFlush();
    std::uint32_t centre = 0, corner = 0;
    if (ok) { b->frames++; centre = b->bits[static_cast<std::ptrdiff_t>(h / 2) * w + w / 2]; corner = b->bits[0]; }
    else b->failed++;
    const LONG64 n = b->frames, bad = b->failed;
    ReleaseSRWLockExclusive(&b->lock);
    if (ok) user::NotifyClientFrame();
    if (!ok && bad <= 3)
        WARN(dispatch, "swapchain: capture of host window %p failed (%s, error %lu)", static_cast<void*>(b->host), how,
             GetLastError());
    if (ok && (n <= 3 || n % 60 == 0))
        TRACE(dispatch, "swapchain: frame %lld of %p captured via %s, %dx%d, centre %08x corner %08x", n, b->guest, how,
              w, h, centre, corner);
}

// ---- Overrides (host view; win10xbox_dispatch calls them with the guest's argument pointer) ----------------------

std::uint64_t NTAPI Ddi_Present(void* p) {
    if (!g_ddi_present) return kStatusNotImplemented;
    NoteNativeWin32uCall();
    if (!p) return static_cast<std::uint32_t>(g_ddi_present(p));
    // D3DKMT_PRESENT { union { hDevice; hContext; } (4 bytes); HWND hWindow (offset 8); ... }
    auto* slot = reinterpret_cast<void**>(static_cast<std::uint8_t*>(p) + 8);
    void* const guest = *slot;
    if (!InterlockedExchange(&g_logged_present_dump, 1)) {
        const auto* q = static_cast<const std::uint64_t*>(p);
        TRACE(dispatch, "swapchain: first NtGdiDdDDIPresent: D3DKMT_PRESENT qwords %016llx %016llx %016llx %016llx "
                        "(hWindow at +8 = %p)", static_cast<unsigned long long>(q[0]), static_cast<unsigned long long>(q[1]),
              static_cast<unsigned long long>(q[2]), static_cast<unsigned long long>(q[3]), guest);
    }
    Binding* b = BindingFor(guest, true);
    if (!b) {
        if (!InterlockedExchange(&g_logged_no_window, 1))
            WARN(dispatch, "swapchain: NtGdiDdDDIPresent with hWindow %p, which is no emulated window: passed as is", guest);
        return static_cast<std::uint32_t>(g_ddi_present(p));
    }
    *slot = b->host;
    const std::int32_t status = g_ddi_present(p);
    *slot = guest;
    if (status >= 0) Capture(b);
    return static_cast<std::uint32_t>(status);
}

// D3DKMT_OPENADAPTERFROMHDC { HDC hDc (offset 0); ... }: an emulated window DC becomes the host window's DC.
std::uint64_t NTAPI Ddi_OpenAdapterFromHdc(void* p) {
    if (!g_ddi_open_adapter_from_hdc) return kStatusNotImplemented;
    if (!p) return static_cast<std::uint32_t>(g_ddi_open_adapter_from_hdc(p));
    NoteNativeWin32uCall();
    auto* slot = reinterpret_cast<void**>(p);
    void* const hdc = *slot;
    void* const guest = user::WindowOfDc(hdc);
    Binding* b = guest ? BindingFor(guest, true) : nullptr;
    if (b && b->host_dc) *slot = b->host_dc;
    const std::int32_t status = g_ddi_open_adapter_from_hdc(p);
    *slot = hdc;
    TRACE(dispatch, "swapchain: OpenAdapterFromHdc(hdc %p, emulated window %p -> host dc %p) -> %08x", hdc, guest,
          b ? static_cast<void*>(b->host_dc) : nullptr, static_cast<unsigned>(status));
    return static_cast<std::uint32_t>(status);
}

// D3DKMT_CHECKOCCLUSION { HWND hWnd }: the answer for an emulated window is core/user's: a visible window is not
// occluded. (The host window is off-screen on purpose, so the host's own answer would always be "occluded".)
std::uint64_t NTAPI Ddi_CheckOcclusion(void* p) {
    if (!g_ddi_check_occlusion) return kStatusNotImplemented;
    if (!p) return static_cast<std::uint32_t>(g_ddi_check_occlusion(p));
    void* const guest = *static_cast<void**>(p);
    user::WindowGeometry g;
    if (guest && user::GetWindowGeometry(guest, &g)) {
        if (g.visible) return 0;
        Binding* b = BindingFor(guest, true);
        if (!b) return kStatusInvalidParameter;
        *static_cast<void**>(p) = b->host;
        const std::int32_t status = g_ddi_check_occlusion(p);
        *static_cast<void**>(p) = guest;
        return static_cast<std::uint32_t>(status);
    }
    return static_cast<std::uint32_t>(g_ddi_check_occlusion(p));
}

}  // namespace

NTSTATUS StartHostWindowThread(HMODULE user32, HMODULE gdi32) {
    bool ok = true;
#define U(fn) ok &= (g_fn.fn = reinterpret_cast<decltype(g_fn.fn)>(GetProcAddress(user32, #fn))) != nullptr
#define G(fn) ok &= (g_fn.fn = reinterpret_cast<decltype(g_fn.fn)>(GetProcAddress(gdi32, #fn))) != nullptr
    U(RegisterClassExW); U(CreateWindowExW); U(DestroyWindow); U(SetWindowPos); U(GetDC); U(ReleaseDC); U(PrintWindow);
    U(PeekMessageW); U(GetMessageW); U(TranslateMessage); U(DispatchMessageW); U(DefWindowProcW); U(PostThreadMessageW);
    G(CreateCompatibleDC); G(CreateDIBSection); G(SelectObject); G(BitBlt); G(DeleteDC); G(DeleteObject); G(GdiFlush);
#undef U
#undef G
    if (!ok) {
        ERR(dispatch, "swapchain: the host's user32/gdi32 lack an export the host window thread needs");
        return st::NotImplemented;
    }
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD tid = 0;
    HANDLE th = CreateThread(nullptr, 0, &WindowThread, nullptr, 0, &tid);
    if (!th || !g_ready) return st::Unsuccessful;
    CloseHandle(th);
    if (WaitForSingleObject(g_ready, 10000) != WAIT_OBJECT_0) {
        ERR(dispatch, "swapchain: the host window thread did not start");
        return st::Unsuccessful;
    }
    g_thread_id = tid;
    g_fn_ok = true;
    TRACE(dispatch, "swapchain: host window thread %lu running", tid);
    return st::Success;
}

void* DdiOverride(const char* name) {
    if (!std::strcmp(name, "NtGdiDdDDIPresent")) return reinterpret_cast<void*>(&Ddi_Present);
    if (!std::strcmp(name, "NtGdiDdDDIOpenAdapterFromHdc")) return reinterpret_cast<void*>(&Ddi_OpenAdapterFromHdc);
    if (!std::strcmp(name, "NtGdiDdDDICheckOcclusion")) return reinterpret_cast<void*>(&Ddi_CheckOcclusion);
    return nullptr;
}

void SetDdiHostFn(const char* name, void* fn) {
    if (!std::strcmp(name, "NtGdiDdDDIPresent")) g_ddi_present = reinterpret_cast<DdiFn>(fn);
    else if (!std::strcmp(name, "NtGdiDdDDIOpenAdapterFromHdc")) g_ddi_open_adapter_from_hdc = reinterpret_cast<DdiFn>(fn);
    else if (!std::strcmp(name, "NtGdiDdDDICheckOcclusion")) g_ddi_check_occlusion = reinterpret_cast<DdiFn>(fn);
}

}  // namespace win10xbox::dispatch::detail
