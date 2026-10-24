// DXGI interposition for a route-B guest (docs/slices/swapchain2.md). On the native-GUI machine the guest's windows are
// real host windows, so a swap chain made for one by the native d3d11/dxgi is a real swap chain; what is missing is
// the layer seeing its frames. The layer therefore sits between the guest and its dxgi at the IMPORT level:
//   dxgi.dll   CreateDXGIFactory, CreateDXGIFactory1, CreateDXGIFactory2 -> layer functions that call the dxgi
//              image's own (original) export and return a wrapper IDXGIFactory2;
//   d3d11.dll  D3D11CreateDeviceAndSwapChain -> a layer function that calls the d3d11 image's own D3D11CreateDevice
//              and makes the swap chain through a wrapper of the device's factory.
// The interposition rewrites those exports of the guest's native image while it is still writable (the same
// mechanism as native_imports.cpp), so every importer (d3d11's own import of dxgi, the guest's GetProcAddress) gets
// the layer's function. The "real" function is the one in the SAME guest image: a factory from the host's dxgi.dll
// could not create a swap chain for a device of the guest's d3d11.
//
// The wrapper factory forwards everything but CreateSwapChain / CreateSwapChainForHwnd; the wrapper swap chain
// forwards everything (GetBuffer returns the real back buffer) and, on Present,
//   host-win32: copies the back buffer to a staging texture BEFORE the real Present (after it a flip-model buffer 0
//               is the next buffer) and, when Present succeeded, hands the pixels to user::NotifyClientFrame, so
//               ComposeFrame / the screenshot show the window's client area;
//   host-uwp:   user::SetSwapChainTarget is set by the host: the real swap chain is created on the presenter's
//               CoreWindow instead (the guest's frames are then the presenter's swap chain), no copy is made.
// Known gaps: a factory reached through device -> adapter -> GetParent (not through CreateDXGIFactory*) is the real
// one, so a swap chain made on it is not seen; IDXGIFactory3.. / IDXGISwapChain2.. QueryInterface fails with
// E_NOINTERFACE on the wrappers (callers fall back); multisampled or non-8-bit back buffers are not captured.
// Every wrapper call is traced at +dispatch with its HRESULT. No function-local statics on these guest-reachable paths.
#include "internal.h"

#include "win10xbox/probe.h"
#include "win10xbox/user.h"

#include <d3d11.h>
#include <dxgi1_6.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace win10xbox::dispatch::detail {
namespace {

using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                         UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateDeviceAndSwapFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                                UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                                ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

// The guest images' original exports, set once while the image is mapped (one mapping at a time per image).
// Image ranges of the interposed native dxgi / d3d11: factories requested from inside them (d3d11 creating its own
// factory for its internals) must be the real objects, because those internals cast them to private types.
#if defined(_MSC_VER) && !defined(__clang__)
#define W10X_RETADDR() _ReturnAddress()
#else
#define W10X_RETADDR() __builtin_return_address(0)
#endif
struct Range { std::uintptr_t base = 0, size = 0; };
Range g_internal[2];
bool CalledFromInternal(void* ret) {
    const auto a = reinterpret_cast<std::uintptr_t>(ret);
    for (const Range& r : g_internal)
        if (r.base && a >= r.base && a - r.base < r.size) return true;
    return false;
}
CreateFactoryFn g_real_create_factory = nullptr;
CreateFactoryFn g_real_create_factory1 = nullptr;
CreateFactory2Fn g_real_create_factory2 = nullptr;
CreateDeviceFn g_real_create_device = nullptr;
CreateDeviceAndSwapFn g_real_create_device_and_swap = nullptr;
// WIN10XBOX_TEST_PRESENT_LIMIT (latched in Launch): the N-th successful Present on any swap chain ends the guest.
std::atomic<unsigned> g_present_limit{0};
std::atomic<unsigned> g_presents{0};

// The layer's functions below run on guest threads under the guest's TLS view: anything touching host C++ runtime
// state (trace, allocation, the compositor hand-off) runs in a HostModeScope; calls into the guest's native
// d3d11/dxgi objects stay in the guest view.
#define W10X_TR(...) do { HostModeScope w10x_host_; TRACE(dispatch, __VA_ARGS__); } while (0)
#define W10X_WN(...) do { HostModeScope w10x_host_; WARN(dispatch, __VA_ARGS__); } while (0)
#define W10X_HOST_NEW_DELETE                                                                        \
    static void* operator new(std::size_t n) { HostModeScope w10x_host_; return ::operator new(n); } \
    static void operator delete(void* p) { HostModeScope w10x_host_; ::operator delete(p); }

#define W10X_TRACE_HR(name, hr_expr)                                                    \
    do {                                                                                \
        const HRESULT w10x_hr_ = (hr_expr);                                             \
        W10X_TR( "dxgi: " name " -> %08lx", static_cast<unsigned long>(w10x_hr_)); \
        return w10x_hr_;                                                                \
    } while (0)

// ---- Exceptions raised while the native dxgi/d3d11 run (diagnostic) -------------------------------------------
// The native runtimes guard their API entry points with catch(...), which turns an SEH exception (EXCEPTION_WINE_STUB
// from a Wine stub or an unresolved import, an access violation) into E_UNEXPECTED without a trace. A first-position
// vectored handler records the last exceptions of the process in a ring (plain globals: it runs on whatever thread
// raised, under whichever TLS view); a failing factory/device call prints the ring.
struct ExcRec {
    DWORD code, nparams;
    std::uintptr_t addr, p[3];
    char s0[48], s1[64];   // EXCEPTION_WINE_STUB: the dll and function names of the stub
    unsigned nstk;         // 0x6ba / C++ throw: the code-looking words on the stack at the raise (who called)
    std::uintptr_t stk[24];
    std::uintptr_t obj[16];   // C++ throw: the first 128 bytes of the thrown object (a wil ResultException carries the HRESULT)
    bool has_obj;
};
constexpr unsigned kExcRing = 24;
ExcRec g_exc[kExcRing];
volatile LONG g_exc_n = 0;
volatile LONG g_exc_hook = 0;

void CopyName(char* to, std::size_t cap, std::uintptr_t from) {
    if (!from || probe::CopyFromUser(to, reinterpret_cast<const void*>(from), cap - 1) != 0) {
        // a short string at the end of a page: byte by byte
        std::size_t i = 0;
        for (; from && i + 1 < cap; ++i)
            if (probe::CopyFromUser(to + i, reinterpret_cast<const void*>(from + i), 1) != 0 || !to[i]) break;
        to[i] = 0;
        return;
    }
    to[cap - 1] = 0;
}

// OutputDebugString text of the native runtimes (DBG_PRINTEXCEPTION_C / _WIDE_C): the last few lines, kept apart so
// that they do not push the real exceptions out of the ring.
constexpr unsigned kDbgRing = 12;
char g_dbg[kDbgRing][160];
volatile LONG g_dbg_n = 0;

void DumpExceptions(const char* what, HRESULT hr);
const char* Where(std::uintptr_t a, char* buf, std::size_t cap);

// A fail-fast (RaiseFailFastException 0xC0000602 / __fastfail 0xC0000409) ends the process at once, so the ring would
// never be printed: dump it here with the faulting rip and the code-looking words on the stack (round 10).
void DumpFailFast(PEXCEPTION_POINTERS info) {
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    char w[48];
    const std::uintptr_t rip = info->ContextRecord->Rip, rsp = info->ContextRecord->Rsp;
    W10X_TR("dxgi: fail-fast exception %08lx at %s rsp %p, %lu parameter(s) %p %p", static_cast<unsigned long>(r->ExceptionCode),
            Where(rip, w, sizeof w), reinterpret_cast<void*>(rsp), static_cast<unsigned long>(r->NumberParameters),
            r->NumberParameters > 0 ? reinterpret_cast<void*>(r->ExceptionInformation[0]) : nullptr,
            r->NumberParameters > 1 ? reinterpret_cast<void*>(r->ExceptionInformation[1]) : nullptr);
    DumpExceptions("fail-fast", static_cast<HRESULT>(r->ExceptionCode));
    unsigned shown = 0;
    for (unsigned i = 0; i < 640 && shown < 32; ++i) {
        std::uintptr_t v = 0;
        if (probe::CopyFromUser(&v, reinterpret_cast<const void*>(rsp + 8u * i), sizeof v) != 0) break;
        MEMORY_BASIC_INFORMATION mbi{};
        if (v < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(v), &mbi, sizeof mbi)) continue;
        if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) continue;
        W10X_TR("dxgi:   stack[%u] code %s (image base %p)", i, Where(v, w, sizeof w), mbi.AllocationBase);
        ++shown;
    }
}

LONG CALLBACK ExcRecorder(PEXCEPTION_POINTERS info) {
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    if (r->ExceptionCode == 0xC0000602 || r->ExceptionCode == 0xC0000409) DumpFailFast(info);
    if (r->ExceptionCode == 0x40010006 && r->NumberParameters >= 2) {   // DBG_PRINTEXCEPTION_C: (length with NUL, ANSI text)
        char* line = g_dbg[static_cast<unsigned>(InterlockedIncrement(&g_dbg_n) - 1) % kDbgRing];
        CopyName(line, sizeof g_dbg[0], r->ExceptionInformation[1]);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (r->ExceptionCode == 0x4001000A) return EXCEPTION_CONTINUE_SEARCH;   // _WIDE_C: the same text, raised first
    ExcRec& e = g_exc[static_cast<unsigned>(InterlockedIncrement(&g_exc_n) - 1) % kExcRing];
    e = ExcRec{};
    e.code = r->ExceptionCode;
    e.nparams = r->NumberParameters;
    e.addr = reinterpret_cast<std::uintptr_t>(r->ExceptionAddress);
    for (DWORD i = 0; i < 3 && i < r->NumberParameters; ++i) e.p[i] = r->ExceptionInformation[i];
    if (r->ExceptionCode == 0x000006ba || r->ExceptionCode == 0xe06d7363) {
        const std::uintptr_t rsp = info->ContextRecord->Rsp;
        if (r->ExceptionCode == 0xe06d7363 && r->NumberParameters >= 2 &&
            probe::CopyFromUser(e.obj, reinterpret_cast<const void*>(r->ExceptionInformation[1]), sizeof e.obj) == 0)
            e.has_obj = true;
        for (unsigned i = 0; i < 900 && e.nstk < 24; ++i) {
            std::uintptr_t v = 0;
            if (probe::CopyFromUser(&v, reinterpret_cast<const void*>(rsp + 8u * i), sizeof v) != 0) break;
            MEMORY_BASIC_INFORMATION mbi{};
            if (v < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(v), &mbi, sizeof mbi)) continue;
            if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) continue;
            e.stk[e.nstk++] = v;
        }
    }
    if (r->ExceptionCode == 0x80000100 && r->NumberParameters >= 2) {
        CopyName(e.s0, sizeof e.s0, r->ExceptionInformation[0]);
        CopyName(e.s1, sizeof e.s1, r->ExceptionInformation[1]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallExcRecorder() {
    if (InterlockedCompareExchange(&g_exc_hook, 1, 0) == 0) AddVectoredExceptionHandler(1, ExcRecorder);
}

// "dxgi+0x1234" for an address inside the native dxgi/d3d11, else the raw address.
const char* Where(std::uintptr_t a, char* buf, std::size_t cap) {
    const char* const kName[2] = {"dxgi", "d3d11"};
    for (int i = 0; i < 2; ++i)
        if (g_internal[i].base && a >= g_internal[i].base && a - g_internal[i].base < g_internal[i].size) {
            std::snprintf(buf, cap, "%s.dll+0x%llx", kName[i], static_cast<unsigned long long>(a - g_internal[i].base));
            return buf;
        }
    MEMORY_BASIC_INFORMATION mbi{};
    // the image's own name from its export directory (the layer's loader keeps no list the handler may read); the
    // guest's modules are mapped by the layer and are not MEM_IMAGE, so any region with a PE header at its base counts
    std::uint16_t mz = 0;
    if (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) && mbi.AllocationBase &&
        probe::CopyFromUser(&mz, mbi.AllocationBase, 2) == 0 && mz == 0x5a4d) {
        char nm[32] = "exe";
        const auto base = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
        std::uint32_t lfa = 0, exp_rva = 0, name_rva = 0;
        if (probe::CopyFromUser(&lfa, reinterpret_cast<const void*>(base + 0x3c), 4) == 0 && lfa < 0x1000 &&
            probe::CopyFromUser(&exp_rva, reinterpret_cast<const void*>(base + lfa + 0x88), 4) == 0 && exp_rva &&
            probe::CopyFromUser(&name_rva, reinterpret_cast<const void*>(base + exp_rva + 12), 4) == 0 && name_rva)
            CopyName(nm, sizeof nm, base + name_rva);
        std::snprintf(buf, cap, "%s@%llx+0x%llx", nm, static_cast<unsigned long long>(base), static_cast<unsigned long long>(a - base));
    }
    else
        std::snprintf(buf, cap, "0x%llx", static_cast<unsigned long long>(a));
    return buf;
}

void DumpExceptions(const char* what, HRESULT hr) {
    const auto* teb = reinterpret_cast<const std::uint8_t*>(NtCurrentTeb());
    const DWORD last_status = *reinterpret_cast<const DWORD*>(teb + 0x1250), last_error = *reinterpret_cast<const DWORD*>(teb + 0x68);
    const LONG n = g_exc_n;
    W10X_TR("dxgi: %s failed %08lx: TEB.LastStatusValue %08lx LastErrorValue %lu; %ld exception(s) raised in the process, last %u:",
            what, static_cast<unsigned long>(hr), static_cast<unsigned long>(last_status), static_cast<unsigned long>(last_error),
            static_cast<long>(n), n < static_cast<LONG>(kExcRing) ? static_cast<unsigned>(n) : kExcRing);
    const LONG nd = g_dbg_n;
    for (LONG i = nd > static_cast<LONG>(kDbgRing) ? nd - static_cast<LONG>(kDbgRing) : 0; i < nd; ++i)
        W10X_TR("dxgi:   debug string #%ld: %.150s", static_cast<long>(i), g_dbg[static_cast<unsigned>(i) % kDbgRing]);
    for (LONG i = n > static_cast<LONG>(kExcRing) ? n - static_cast<LONG>(kExcRing) : 0; i < n; ++i) {
        const ExcRec e = g_exc[static_cast<unsigned>(i) % kExcRing];
        char w[48], w2[48];
        if (e.code == 0x80000100)
            W10X_TR("dxgi:   exc #%ld %08lx at %s: Wine stub %s!%s called from %s", static_cast<long>(i), static_cast<unsigned long>(e.code),
                    Where(e.addr, w, sizeof w), e.s0, e.s1, Where(e.p[2], w2, sizeof w2));
        else
            W10X_TR("dxgi:   exc #%ld %08lx at %s params %llx %llx %llx", static_cast<long>(i), static_cast<unsigned long>(e.code),
                    Where(e.addr, w, sizeof w), static_cast<unsigned long long>(e.p[0]), static_cast<unsigned long long>(e.p[1]),
                    static_cast<unsigned long long>(e.p[2]));
        for (unsigned k = 0; k < e.nstk; ++k) W10X_TR("dxgi:   exc #%ld stk[%u] %s", static_cast<long>(i), k, Where(e.stk[k], w2, sizeof w2));
        if (e.has_obj)
            for (unsigned k = 0; k < 16; ++k) {
                char str[72] = "";   // a readable string behind the word (ANSI or UTF-16), for wil's file / function / code text
                if (e.obj[k] > 0x10000) {
                    char tmp[72] = {};
                    CopyName(tmp, sizeof tmp, e.obj[k]);
                    unsigned n = 0;
                    while (n < sizeof tmp - 1 && tmp[n] >= 0x20 && tmp[n] < 0x7f) ++n;
                    if (n >= 4) { std::memcpy(str, tmp, n); str[n] = 0; }
                    else if (n == 1 && tmp[1] == 0) {   // UTF-16 text: every second byte is 0
                        unsigned m = 0;
                        while (m < 34 && tmp[2 * m] >= 0x20 && tmp[2 * m] < 0x7f && tmp[2 * m + 1] == 0) { str[m] = tmp[2 * m]; ++m; }
                        str[m] = 0;
                    }
                }
                W10X_TR("dxgi:   exc #%ld obj[%u] = %016llx %s", static_cast<long>(i), k, static_cast<unsigned long long>(e.obj[k]), str);
            }
    }
}

bool IsIid(REFIID a, REFIID b) { return IsEqualGUID(a, b) != FALSE; }

// IIDs the wrappers answer for themselves; the newer factory / swap chain interfaces are refused so that every
// swap chain keeps going through the wrappers.
bool IsNewerFactory(REFIID r) {
    return IsIid(r, __uuidof(IDXGIFactory3)) || IsIid(r, __uuidof(IDXGIFactory4)) || IsIid(r, __uuidof(IDXGIFactory5)) ||
           IsIid(r, __uuidof(IDXGIFactory6)) || IsIid(r, __uuidof(IDXGIFactory7));
}
bool IsNewerSwapChain(REFIID r) {
    return IsIid(r, __uuidof(IDXGISwapChain2)) || IsIid(r, __uuidof(IDXGISwapChain3)) ||
           IsIid(r, __uuidof(IDXGISwapChain4));
}
bool IsWrappedFactoryIid(REFIID r) {
    return IsIid(r, __uuidof(IUnknown)) || IsIid(r, __uuidof(IDXGIObject)) || IsIid(r, __uuidof(IDXGIFactory)) ||
           IsIid(r, __uuidof(IDXGIFactory1)) || IsIid(r, __uuidof(IDXGIFactory2));
}

// ---- Wrapper swap chain -----------------------------------------------------------------------------------

class WrapSwap final : public IDXGISwapChain1 {
public:
    W10X_HOST_NEW_DELETE
    // Takes over the caller's reference to `sc`. `hwnd` is the window to report frames for (null: no capture).
    WrapSwap(IDXGISwapChain* sc, HWND hwnd) : sc0_(sc), hwnd_(hwnd) {
        sc0_->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1_));
    }
    ~WrapSwap() {
        if (staging_) staging_->Release();
        if (sc1_) sc1_->Release();
        if (sc0_) sc0_->Release();
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (IsIid(riid, __uuidof(IUnknown)) || IsIid(riid, __uuidof(IDXGIObject)) ||
            IsIid(riid, __uuidof(IDXGIDeviceSubObject)) || IsIid(riid, __uuidof(IDXGISwapChain)) ||
            (sc1_ && IsIid(riid, __uuidof(IDXGISwapChain1)))) {
            *pp = static_cast<IDXGISwapChain1*>(this);
            AddRef();
            W10X_TRACE_HR("IDXGISwapChain::QueryInterface(wrapped)", S_OK);
        }
        if (IsNewerSwapChain(riid) || IsIid(riid, __uuidof(IDXGISwapChain1))) {
            *pp = nullptr;
            W10X_TRACE_HR("IDXGISwapChain::QueryInterface(newer)", E_NOINTERFACE);
        }
        W10X_TRACE_HR("IDXGISwapChain::QueryInterface(forwarded)", sc0_->QueryInterface(riid, pp));
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT s, const void* d) override { return sc0_->SetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* u) override { return sc0_->SetPrivateDataInterface(n, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* s, void* d) override { return sc0_->GetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID r, void** pp) override { return sc0_->GetParent(r, pp); }
    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID r, void** pp) override { return sc0_->GetDevice(r, pp); }
    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override {
        if ((flags & DXGI_PRESENT_TEST) == 0) {
            { HostModeScope w10x_host_; user::NoteGuestPresent(sync); }
            // VRR / FreeSync: a swap chain created with ALLOW_TEARING (the host's target does that when the system
            // supports it) presents interval-0 frames with tearing.
            if (sync == 0 && !(flags & DXGI_PRESENT_ALLOW_TEARING) && sc1_) {
                if (!tear_checked_) {
                    DXGI_SWAP_CHAIN_DESC1 d1{};
                    tear_ = SUCCEEDED(sc1_->GetDesc1(&d1)) && (d1.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
                    tear_checked_ = true;
                }
                if (tear_) flags |= DXGI_PRESENT_ALLOW_TEARING;
            }
        }
        const bool cap = (flags & DXGI_PRESENT_TEST) == 0 && Capture();
        const HRESULT hr = sc0_->Present(sync, flags);
        W10X_TR( "dxgi: IDXGISwapChain::Present(%u, 0x%x) -> %08lx%s", sync, flags, static_cast<unsigned long>(hr),
              cap ? " (back buffer copied)" : "");
        if (cap && SUCCEEDED(hr)) Deliver();
        if ((flags & DXGI_PRESENT_TEST) == 0 && SUCCEEDED(hr)) CountPresent();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT i, REFIID r, void** pp) override {
        W10X_TRACE_HR("IDXGISwapChain::GetBuffer", sc0_->GetBuffer(i, r, pp));
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fs, IDXGIOutput* o) override {
        W10X_TRACE_HR("IDXGISwapChain::SetFullscreenState", sc0_->SetFullscreenState(fs, o));
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fs, IDXGIOutput** o) override { return sc0_->GetFullscreenState(fs, o); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override { W10X_TRACE_HR("IDXGISwapChain::GetDesc", sc0_->GetDesc(d)); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) override {
        if (staging_) { staging_->Release(); staging_ = nullptr; }
        W10X_TRACE_HR("IDXGISwapChain::ResizeBuffers", sc0_->ResizeBuffers(n, w, h, f, fl));
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* m) override { W10X_TRACE_HR("IDXGISwapChain::ResizeTarget", sc0_->ResizeTarget(m)); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o) override {
        W10X_TRACE_HR("IDXGISwapChain::GetContainingOutput", sc0_->GetContainingOutput(o));
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override { return sc0_->GetFrameStatistics(s); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override { return sc0_->GetLastPresentCount(c); }
    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override { return sc1_ ? sc1_->GetDesc1(d) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override { return sc1_ ? sc1_->GetFullscreenDesc(d) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override { return sc1_ ? sc1_->GetHwnd(h) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID r, void** pp) override { return sc1_ ? sc1_->GetCoreWindow(r, pp) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) override {
        if (!sc1_) return E_NOTIMPL;
        const bool cap = (flags & DXGI_PRESENT_TEST) == 0 && Capture();
        const HRESULT hr = sc1_->Present1(sync, flags, p);
        W10X_TR( "dxgi: IDXGISwapChain1::Present1(%u, 0x%x) -> %08lx%s", sync, flags, static_cast<unsigned long>(hr),
              cap ? " (back buffer copied)" : "");
        if (cap && SUCCEEDED(hr)) Deliver();
        if ((flags & DXGI_PRESENT_TEST) == 0 && SUCCEEDED(hr)) CountPresent();
        return hr;
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return sc1_ ? sc1_->IsTemporaryMonoSupported() : FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override { return sc1_ ? sc1_->GetRestrictToOutput(o) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override { return sc1_ ? sc1_->SetBackgroundColor(c) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override { return sc1_ ? sc1_->GetBackgroundColor(c) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override { return sc1_ ? sc1_->SetRotation(r) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override { return sc1_ ? sc1_->GetRotation(r) : E_NOTIMPL; }

private:
    // Present-limit harness: after the limit-th presented frame (already handed to the compositor) log one line and
    // end the guest like ExitProcess(42); the host takes its screenshot of the last composed frame at exit.
    void CountPresent() {
        const unsigned lim = g_present_limit.load();
        if (!lim || g_presents.fetch_add(1) + 1 != lim) return;
        DXGI_SWAP_CHAIN_DESC d{};
        sc0_->GetDesc(&d);
        char adapter[128] = "unknown";
        IDXGIDevice* dev = nullptr;
        if (SUCCEEDED(sc0_->GetDevice(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dev))) && dev) {
            IDXGIAdapter* ad = nullptr;
            DXGI_ADAPTER_DESC add{};
            if (SUCCEEDED(dev->GetAdapter(&ad)) && ad) {
                if (SUCCEEDED(ad->GetDesc(&add)))
                    std::snprintf(adapter, sizeof adapter, "%ls", add.Description);
                ad->Release();
            }
            dev->Release();
        }
        W10X_TR( "present-limit: %u frames presented, %ux%u, %s", lim, d.BufferDesc.Width, d.BufferDesc.Height, adapter);
        { HostModeScope w10x_host_; std::printf("present-limit: %u frames presented, %ux%u, %s\n", lim,
                                              d.BufferDesc.Width, d.BufferDesc.Height, adapter); std::fflush(stdout); }
        HostModeScope w10x_host_;   // GuestExit does not return
        process::GuestExit(42);
    }
    // Copies back buffer 0 into pix_ (top-down BGRA). False when there is no window to report for or the buffer
    // cannot be read (traced once).
    bool Capture() {
        if (!hwnd_ || no_capture_) return false;
        DXGI_SWAP_CHAIN_DESC d{};
        if (FAILED(sc0_->GetDesc(&d))) return false;
        bool swap_rb = false;
        switch (d.BufferDesc.Format) {
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM: break;
            case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: swap_rb = true; break;
            default: return Refuse("back buffer format %u is not captured", static_cast<unsigned>(d.BufferDesc.Format));
        }
        if (d.SampleDesc.Count > 1) return Refuse("a multisampled back buffer is not captured");
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        ID3D11Texture2D* back = nullptr;
        bool ok = false;
        if (SUCCEEDED(sc0_->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev) {
            dev->GetImmediateContext(&ctx);
            if (ctx && SUCCEEDED(sc0_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) && back) {
                D3D11_TEXTURE2D_DESC td{};
                back->GetDesc(&td);
                if (staging_ && (st_w_ != td.Width || st_h_ != td.Height || st_fmt_ != td.Format)) {
                    staging_->Release();
                    staging_ = nullptr;
                }
                if (!staging_) {
                    st_w_ = td.Width; st_h_ = td.Height; st_fmt_ = td.Format;
                    td.Usage = D3D11_USAGE_STAGING;
                    td.BindFlags = 0;
                    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    td.MiscFlags = 0;
                    const HRESULT hr = dev->CreateTexture2D(&td, nullptr, &staging_);
                    if (FAILED(hr)) { staging_ = nullptr; Refuse("staging texture failed with %08lx", static_cast<unsigned long>(hr)); }
                }
                D3D11_MAPPED_SUBRESOURCE m{};
                if (staging_) {
                    ctx->CopyResource(staging_, back);
                    if (SUCCEEDED(ctx->Map(staging_, 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
                        pix_w_ = static_cast<std::int32_t>(td.Width);
                        pix_h_ = static_cast<std::int32_t>(td.Height);
                        { HostModeScope w10x_host_; pix_.resize(static_cast<std::size_t>(pix_w_) * pix_h_); }
                        for (std::int32_t y = 0; y < pix_h_; ++y) {
                            const auto* src = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(m.pData) +
                                                                                    static_cast<std::size_t>(y) * m.RowPitch);
                            std::uint32_t* dst = pix_.data() + static_cast<std::size_t>(y) * pix_w_;
                            for (std::int32_t x = 0; x < pix_w_; ++x) {
                                std::uint32_t v = src[x];
                                if (swap_rb) v = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
                                dst[x] = v | 0xFF000000u;
                            }
                        }
                        ctx->Unmap(staging_, 0);
                        ok = true;
                    }
                }
            }
        }
        if (back) back->Release();
        if (ctx) ctx->Release();
        if (dev) dev->Release();
        return ok;
    }
    void Deliver() { HostModeScope w10x_host_; user::NotifyClientFrame(hwnd_, pix_.data(), pix_w_, pix_h_, pix_w_ * 4); }
    bool Refuse(const char* fmt, ...) {
        if (!no_capture_) {
            no_capture_ = true;
            char buf[160];
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(buf, sizeof buf, fmt, ap);
            va_end(ap);
            W10X_WN( "dxgi: swap chain on window %p: %s: its frames are not handed to the compositor", static_cast<void*>(hwnd_), buf);
        }
        return false;
    }

    LONG ref_ = 1;
    IDXGISwapChain* sc0_;
    IDXGISwapChain1* sc1_ = nullptr;
    HWND hwnd_;
    bool no_capture_ = false;
    bool tear_checked_ = false, tear_ = false;
    ID3D11Texture2D* staging_ = nullptr;
    UINT st_w_ = 0, st_h_ = 0;
    DXGI_FORMAT st_fmt_ = DXGI_FORMAT_UNKNOWN;
    std::vector<std::uint32_t> pix_;
    std::int32_t pix_w_ = 0, pix_h_ = 0;
};

// ---- Wrapper adapter / output (the display mode of the emulated windows) --------------------------------------------
// On host-uwp (a swap-chain target is set) the guest's windows are the layer's, so the display its DXGI reports must be
// the layer's one display mode (user::CurrentDisplayMode), not whatever the host's real output says. The factory's
// adapters are wrapped; their outputs answer GetDesc / GetDisplayModeList / FindClosestMatchingMode with that mode and
// forward everything else. An adapter without an output (the runner's WARP) forwards DXGI_ERROR_NOT_FOUND unchanged.
// A wrapped adapter handed back to D3D11CreateDevice* is unwrapped there (kUnwrapAdapterIid); D3D12CreateDevice is not
// interposed, so a D3D12 guest on host-uwp would receive the wrapper (see docs/slices/displaymode.md).
const GUID kUnwrapAdapterIid = {0x7a1f0c52, 0x3b9e, 0x4d16, {0x9c, 0x0a, 0x5e, 0x21, 0xb4, 0x7d, 0x88, 0x31}};

bool LayerMode(UINT* w, UINT* h, UINT* hz) {
    std::uint32_t mw = 0, mh = 0, mhz = 0, bpp = 0;
    bool ok;
    { HostModeScope w10x_host_; ok = user::CurrentDisplayMode(&mw, &mh, &mhz, &bpp); }
    if (!ok || !mw || !mh) return false;
    *w = mw; *h = mh; *hz = mhz ? mhz : 60;
    return true;
}

class WrapOutput final : public IDXGIOutput {
public:
    W10X_HOST_NEW_DELETE
    explicit WrapOutput(IDXGIOutput* o) : o_(o) {}
    ~WrapOutput() { if (o_) o_->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (IsIid(riid, __uuidof(IUnknown)) || IsIid(riid, __uuidof(IDXGIObject)) || IsIid(riid, __uuidof(IDXGIOutput))) {
            *pp = static_cast<IDXGIOutput*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;   // IDXGIOutput1..6: callers fall back to IDXGIOutput
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT s, const void* d) override { return o_->SetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* u) override { return o_->SetPrivateDataInterface(n, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* s, void* d) override { return o_->GetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID r, void** pp) override { return o_->GetParent(r, pp); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* d) override {
        const HRESULT hr = o_->GetDesc(d);
        UINT w, h, hz;
        if (SUCCEEDED(hr) && d && LayerMode(&w, &h, &hz)) {
            d->DesktopCoordinates = RECT{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
            d->AttachedToDesktop = TRUE;
            d->Rotation = DXGI_MODE_ROTATION_IDENTITY;
        }
        W10X_TRACE_HR("IDXGIOutput::GetDesc", hr);
    }
    HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT fmt, UINT flags, UINT* num, DXGI_MODE_DESC* modes) override {
        UINT w, h, hz;
        if (!num || !LayerMode(&w, &h, &hz)) return o_->GetDisplayModeList(fmt, flags, num, modes);
        if (modes) {
            if (*num < 1) { *num = 1; W10X_TRACE_HR("IDXGIOutput::GetDisplayModeList(layer mode)", DXGI_ERROR_MORE_DATA); }
            modes[0] = MakeMode(fmt, w, h, hz);
        }
        *num = 1;
        W10X_TRACE_HR("IDXGIOutput::GetDisplayModeList(layer mode)", S_OK);
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC* in, DXGI_MODE_DESC* closest, IUnknown* dev) override {
        UINT w, h, hz;
        if (!in || !closest || !LayerMode(&w, &h, &hz)) return o_->FindClosestMatchingMode(in, closest, dev);
        *closest = MakeMode(in->Format, w, h, hz);
        W10X_TRACE_HR("IDXGIOutput::FindClosestMatchingMode(layer mode)", S_OK);
    }
    HRESULT STDMETHODCALLTYPE WaitForVBlank() override { return o_->WaitForVBlank(); }
    HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown* d, BOOL e) override { return o_->TakeOwnership(d, e); }
    void STDMETHODCALLTYPE ReleaseOwnership() override { o_->ReleaseOwnership(); }
    HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES* c) override { return o_->GetGammaControlCapabilities(c); }
    HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL* c) override { return o_->SetGammaControl(c); }
    HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL* c) override { return o_->GetGammaControl(c); }
    HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface* s) override { return o_->SetDisplaySurface(s); }
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface* s) override { return o_->GetDisplaySurfaceData(s); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override { return o_->GetFrameStatistics(s); }

private:
    static DXGI_MODE_DESC MakeMode(DXGI_FORMAT fmt, UINT w, UINT h, UINT hz) {
        DXGI_MODE_DESC m{};
        m.Width = w;
        m.Height = h;
        m.RefreshRate = DXGI_RATIONAL{hz, 1};
        m.Format = fmt == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_R8G8B8A8_UNORM : fmt;
        m.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
        m.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        return m;
    }
    LONG ref_ = 1;
    IDXGIOutput* o_;
};

class WrapAdapter final : public IDXGIAdapter1 {
public:
    W10X_HOST_NEW_DELETE
    explicit WrapAdapter(IDXGIAdapter* a) : a0_(a) { a0_->QueryInterface(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&a1_)); }
    ~WrapAdapter() {
        if (a1_) a1_->Release();
        if (a0_) a0_->Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (IsIid(riid, kUnwrapAdapterIid)) {   // the real adapter, one reference (UnwrapAdapter)
            a0_->AddRef();
            *pp = a0_;
            return S_OK;
        }
        if (IsIid(riid, __uuidof(IUnknown)) || IsIid(riid, __uuidof(IDXGIObject)) || IsIid(riid, __uuidof(IDXGIAdapter)) ||
            (a1_ && IsIid(riid, __uuidof(IDXGIAdapter1)))) {
            *pp = static_cast<IDXGIAdapter1*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;   // IDXGIAdapter2.. and private interfaces: refused so the object stays a plain adapter
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT s, const void* d) override { return a0_->SetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* u) override { return a0_->SetPrivateDataInterface(n, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* s, void* d) override { return a0_->GetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID r, void** pp) override { return a0_->GetParent(r, pp); }
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT i, IDXGIOutput** out) override {
        IDXGIOutput* real = nullptr;
        const HRESULT hr = a0_->EnumOutputs(i, out ? &real : nullptr);
        if (FAILED(hr) || !real || !out) return hr;
        *out = new WrapOutput(real);
        W10X_TRACE_HR("IDXGIAdapter::EnumOutputs(layer mode)", S_OK);
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* d) override { return a0_->GetDesc(d); }
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID g, LARGE_INTEGER* v) override { return a0_->CheckInterfaceSupport(g, v); }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* d) override { return a1_ ? a1_->GetDesc1(d) : E_NOTIMPL; }

private:
    LONG ref_ = 1;
    IDXGIAdapter* a0_;
    IDXGIAdapter1* a1_ = nullptr;
};

// The adapter a layer-made wrapper stands for (one reference, *owned), or `a` itself (*owned null).
IDXGIAdapter* UnwrapAdapter(IDXGIAdapter* a, IDXGIAdapter** owned) {
    *owned = nullptr;
    if (!a) return nullptr;
    IDXGIAdapter* real = nullptr;
    if (SUCCEEDED(a->QueryInterface(kUnwrapAdapterIid, reinterpret_cast<void**>(&real))) && real) {
        *owned = real;
        return real;
    }
    return a;
}

// ---- Virtual swap chain (emulated windows, docs/slices/uwpswap.md) -----------------------------------------
// A window of core/user's own window manager cannot be the target of a real HWND swap chain (the native dxgi would
// need a window of the host's win32k, which a UWP process cannot create). The swap chain is then ordinary: ONE
// render-target texture on the guest's device is the back buffer (D3D11 keeps buffer 0 the current back buffer
// across a flip-model Present, so one texture serves every effect and buffer count), and Present copies it to a
// staging texture and hands the pixels to user::NotifyClientFrame, which core/user composes at the window's client
// rectangle. GetContainingOutput is a stub, SetFullscreenState is only recorded.
class VirtSwap final : public IDXGISwapChain1 {
public:
    W10X_HOST_NEW_DELETE
    static HRESULT Create(IUnknown* dev_unk, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& d, BOOL windowed, IDXGIFactory* parent,
                          IDXGISwapChain1** out) {
        ID3D11Device* dev = nullptr;
        if (!dev_unk || FAILED(dev_unk->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev)
            return DXGI_ERROR_UNSUPPORTED;
        auto* v = new VirtSwap(dev, hwnd, d, windowed, parent);
        const HRESULT hr = v->MakeBuffer();
        if (FAILED(hr)) { v->Release(); return hr; }
        *out = v;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (IsIid(riid, __uuidof(IUnknown)) || IsIid(riid, __uuidof(IDXGIObject)) ||
            IsIid(riid, __uuidof(IDXGIDeviceSubObject)) || IsIid(riid, __uuidof(IDXGISwapChain)) ||
            IsIid(riid, __uuidof(IDXGISwapChain1))) {
            *pp = static_cast<IDXGISwapChain1*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT* s, void*) override { if (s) *s = 0; return DXGI_ERROR_NOT_FOUND; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID r, void** pp) override {
        return parent_ ? parent_->QueryInterface(r, pp) : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID r, void** pp) override { return dev_->QueryInterface(r, pp); }

    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override { return DoPresent("IDXGISwapChain::Present", sync, flags); }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS*) override {
        return DoPresent("IDXGISwapChain1::Present1", sync, flags);
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT i, REFIID r, void** pp) override {
        if (!pp) return E_POINTER;
        *pp = nullptr;
        const HRESULT hr = (i == 0 && tex_) ? tex_->QueryInterface(r, pp) : DXGI_ERROR_INVALID_CALL;
        W10X_TRACE_HR("virtual IDXGISwapChain::GetBuffer", hr);
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fs, IDXGIOutput*) override {
        fullscreen_ = fs;
        W10X_TR("dxgi: virtual IDXGISwapChain::SetFullscreenState(%d) recorded only", static_cast<int>(fs));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fs, IDXGIOutput** o) override {
        if (fs) *fs = fullscreen_;
        if (o) *o = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override {
        if (!d) return E_POINTER;
        *d = {};
        d->BufferDesc.Width = d1_.Width;
        d->BufferDesc.Height = d1_.Height;
        d->BufferDesc.Format = d1_.Format;
        d->BufferDesc.RefreshRate = {60, 1};
        d->SampleDesc = d1_.SampleDesc;
        d->BufferUsage = d1_.BufferUsage;
        d->BufferCount = d1_.BufferCount;
        d->OutputWindow = hwnd_;
        d->Windowed = windowed_;
        d->SwapEffect = d1_.SwapEffect;
        d->Flags = d1_.Flags;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) override {
        if (n) d1_.BufferCount = n;
        if (f != DXGI_FORMAT_UNKNOWN) d1_.Format = f;
        d1_.Width = w;
        d1_.Height = h;
        d1_.Flags = fl;
        if (tex_) { tex_->Release(); tex_ = nullptr; }
        if (resolve_) { resolve_->Release(); resolve_ = nullptr; }
        if (staging_) { staging_->Release(); staging_ = nullptr; }
        W10X_TRACE_HR("virtual IDXGISwapChain::ResizeBuffers", MakeBuffer());
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o) override {
        if (o) *o = nullptr;
        W10X_TRACE_HR("virtual IDXGISwapChain::GetContainingOutput (stub)", DXGI_ERROR_UNSUPPORTED);
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override {
        if (s) *s = {};
        return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override { if (c) *c = presents_; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override { if (!d) return E_POINTER; *d = d1_; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override {
        if (!d) return E_POINTER;
        *d = {};
        d->RefreshRate = {60, 1};
        d->Windowed = windowed_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override { if (!h) return E_POINTER; *h = hwnd_; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID, void** pp) override { if (pp) *pp = nullptr; return DXGI_ERROR_INVALID_CALL; }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override { if (o) *o = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override { if (c) *c = {}; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override { if (r) *r = DXGI_MODE_ROTATION_IDENTITY; return S_OK; }

private:
    VirtSwap(ID3D11Device* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& d, BOOL windowed, IDXGIFactory* parent)
        : dev_(dev), hwnd_(hwnd), d1_(d), windowed_(windowed), parent_(parent) {
        if (parent_) parent_->AddRef();
        if (d1_.BufferCount == 0) d1_.BufferCount = 1;
    }
    ~VirtSwap() {
        if (staging_) staging_->Release();
        if (resolve_) resolve_->Release();
        if (tex_) tex_->Release();
        if (parent_) parent_->Release();
        if (dev_) dev_->Release();
    }

    HRESULT MakeBuffer() {
        if (!d1_.Width || !d1_.Height) {
            std::int32_t w = 0, h = 0;
            { HostModeScope w10x_host_; user::GetEmulatedClientSize(hwnd_, &w, &h); }
            if (!d1_.Width) d1_.Width = w > 0 ? static_cast<UINT>(w) : 1;
            if (!d1_.Height) d1_.Height = h > 0 ? static_cast<UINT>(h) : 1;
        }
        if (d1_.Format == DXGI_FORMAT_UNKNOWN) d1_.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = d1_.Width;
        td.Height = d1_.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = d1_.Format;
        td.SampleDesc = d1_.SampleDesc;
        if (td.SampleDesc.Count == 0) td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (d1_.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &tex_);
        if (FAILED(hr) && (td.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) {
            td.BindFlags &= ~static_cast<UINT>(D3D11_BIND_UNORDERED_ACCESS);
            hr = dev_->CreateTexture2D(&td, nullptr, &tex_);
        }
        W10X_TR("dxgi: virtual swap chain back buffer %ux%u fmt %u samples %u -> %08lx", d1_.Width, d1_.Height,
                static_cast<unsigned>(d1_.Format), td.SampleDesc.Count, static_cast<unsigned long>(hr));
        if (FAILED(hr)) tex_ = nullptr;
        return hr;
    }

    // Copies the (resolved) back buffer into pix_ as top-down BGRA. False when it cannot be read.
    bool Grab() {
        bool swap_rb = false;
        switch (d1_.Format) {
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM: break;
            case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: swap_rb = true; break;
            default:
                if (!warned_) { warned_ = true; W10X_WN("dxgi: virtual swap chain format %u is not captured", static_cast<unsigned>(d1_.Format)); }
                return false;
        }
        ID3D11DeviceContext* ctx = nullptr;
        dev_->GetImmediateContext(&ctx);
        if (!ctx) return false;
        ID3D11Texture2D* src = tex_;
        D3D11_TEXTURE2D_DESC td{};
        tex_->GetDesc(&td);
        if (td.SampleDesc.Count > 1) {
            if (!resolve_) {
                D3D11_TEXTURE2D_DESC rd = td;
                rd.SampleDesc = {1, 0};
                rd.BindFlags = 0;
                if (FAILED(dev_->CreateTexture2D(&rd, nullptr, &resolve_))) resolve_ = nullptr;
            }
            if (!resolve_) { ctx->Release(); return false; }
            ctx->ResolveSubresource(resolve_, 0, tex_, 0, td.Format);
            src = resolve_;
        }
        if (!staging_) {
            D3D11_TEXTURE2D_DESC sd = td;
            sd.SampleDesc = {1, 0};
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            if (FAILED(dev_->CreateTexture2D(&sd, nullptr, &staging_))) staging_ = nullptr;
        }
        bool ok = false;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (staging_) {
            ctx->CopyResource(staging_, src);
            if (SUCCEEDED(ctx->Map(staging_, 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
                pix_w_ = static_cast<std::int32_t>(td.Width);
                pix_h_ = static_cast<std::int32_t>(td.Height);
                { HostModeScope w10x_host_; pix_.resize(static_cast<std::size_t>(pix_w_) * pix_h_); }
                for (std::int32_t y = 0; y < pix_h_; ++y) {
                    const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(m.pData) +
                                                                            static_cast<std::size_t>(y) * m.RowPitch);
                    std::uint32_t* dst = pix_.data() + static_cast<std::size_t>(y) * pix_w_;
                    for (std::int32_t x = 0; x < pix_w_; ++x) {
                        std::uint32_t v = row[x];
                        if (swap_rb) v = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
                        dst[x] = v | 0xFF000000u;
                    }
                }
                ctx->Unmap(staging_, 0);
                ok = true;
            }
        }
        ctx->Release();
        return ok;
    }

    HRESULT DoPresent(const char* what, UINT sync, UINT flags) {
        if (flags & DXGI_PRESENT_TEST) return S_OK;
        if (!tex_) return DXGI_ERROR_INVALID_CALL;
        const bool got = Grab();
        bool sent = false;
        {
            HostModeScope w10x_host_;
            if (got) sent = user::NotifyClientFrame(hwnd_, pix_.data(), pix_w_, pix_h_, pix_w_ * 4);
            // vsync: one refresh (16 ms) per sync interval, so a loop that presents every frame runs at ~60 fps
            if (sync) {
                const ULONGLONG now = GetTickCount64(), due = last_present_ + 16ull * (sync > 4 ? 4 : sync);
                if (last_present_ && now < due) Sleep(static_cast<DWORD>(due - now));
            }
            last_present_ = GetTickCount64();
            ++presents_;
            if (presents_ == 1 || presents_ % 600 == 0)
                TRACE(dispatch, "dxgi: virtual %s(%u, 0x%x) #%u -> 00000000 %s", what, sync, flags, presents_,
                      sent ? "(back buffer composed)" : "(NOT handed to the compositor)");
        }
        return S_OK;
    }

    LONG ref_ = 1;
    ID3D11Device* dev_;
    HWND hwnd_;
    DXGI_SWAP_CHAIN_DESC1 d1_;
    BOOL windowed_;
    IDXGIFactory* parent_;
    ID3D11Texture2D* tex_ = nullptr;
    ID3D11Texture2D* resolve_ = nullptr;
    ID3D11Texture2D* staging_ = nullptr;
    BOOL fullscreen_ = FALSE;
    bool warned_ = false;
    UINT presents_ = 0;
    ULONGLONG last_present_ = 0;
    std::vector<std::uint32_t> pix_;
    std::int32_t pix_w_ = 0, pix_h_ = 0;
};

bool IsVirtualWindow(HWND hwnd) {
    HostModeScope w10x_host_;
    return user::IsEmulatedWindow(hwnd);
}

// ---- Wrapper factory ----------------------------------------------------------------------------------------

class WrapFactory final : public IDXGIFactory2 {
public:
    W10X_HOST_NEW_DELETE
    // Takes over the caller's reference to `f0` (the real IDXGIFactory); queries the newer interfaces itself.
    explicit WrapFactory(IDXGIFactory* f0) : f0_(f0) {
        f0_->QueryInterface(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f1_));
        f0_->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&f2_));
    }
    ~WrapFactory() {
        if (f2_) f2_->Release();
        if (f1_) f1_->Release();
        if (f0_) f0_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (IsIid(riid, __uuidof(IUnknown)) || IsIid(riid, __uuidof(IDXGIObject)) || IsIid(riid, __uuidof(IDXGIFactory)) ||
            (f1_ && IsIid(riid, __uuidof(IDXGIFactory1))) || (f2_ && IsIid(riid, __uuidof(IDXGIFactory2)))) {
            *pp = static_cast<IDXGIFactory2*>(this);
            AddRef();
            W10X_TRACE_HR("IDXGIFactory::QueryInterface(wrapped)", S_OK);
        }
        if (IsNewerFactory(riid) || IsWrappedFactoryIid(riid)) {
            *pp = nullptr;
            W10X_TRACE_HR("IDXGIFactory::QueryInterface(newer)", E_NOINTERFACE);
        }
        W10X_TRACE_HR("IDXGIFactory::QueryInterface(forwarded)", f0_->QueryInterface(riid, pp));
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT s, const void* d) override { return f0_->SetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* u) override { return f0_->SetPrivateDataInterface(n, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* s, void* d) override { return f0_->GetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID r, void** pp) override { return f0_->GetParent(r, pp); }
    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT i, IDXGIAdapter** a) override {
        IDXGIAdapter* real = nullptr;
        const LONG n0 = g_exc_n;
        const HRESULT hr = f0_->EnumAdapters(i, a ? &real : nullptr);
        W10X_TR("dxgi: EnumAdapters(%u) -> %08lx, %ld exception(s) during the call", i, static_cast<unsigned long>(hr), static_cast<long>(g_exc_n - n0));
        if (g_exc_n != n0) DumpExceptions("EnumAdapters", hr);
        if (FAILED(hr) || !real || !a) return hr;
        *a = user::GetSwapChainTarget(nullptr) ? static_cast<IDXGIAdapter*>(new WrapAdapter(real)) : real;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND h, UINT fl) override {
        if (h && IsVirtualWindow(h)) return S_OK;   // nothing to hook in an emulated window
        W10X_TRACE_HR("IDXGIFactory::MakeWindowAssociation", f0_->MakeWindowAssociation(h, fl));
    }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* h) override { return f0_->GetWindowAssociation(h); }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) override {
        if (!d || !out) return f0_->CreateSwapChain(dev, d, out);
        if (IsVirtualWindow(d->OutputWindow)) {   // an emulated window: a virtual swap chain (VirtSwap)
            DXGI_SWAP_CHAIN_DESC1 v{};
            v.Width = d->BufferDesc.Width;
            v.Height = d->BufferDesc.Height;
            v.Format = d->BufferDesc.Format;
            v.SampleDesc = d->SampleDesc;
            v.BufferUsage = d->BufferUsage;
            v.BufferCount = d->BufferCount;
            v.SwapEffect = d->SwapEffect;
            v.Flags = d->Flags;
            IDXGISwapChain1* vs = nullptr;
            const HRESULT vhr = VirtSwap::Create(dev, d->OutputWindow, v, d->Windowed, f0_, &vs);
            if (SUCCEEDED(vhr)) *out = vs;
            W10X_TR("dxgi: IDXGIFactory::CreateSwapChain(window %p, %ux%u fmt %u, %u buffers, effect %u) virtual -> %08lx",
                    static_cast<void*>(d->OutputWindow), d->BufferDesc.Width, d->BufferDesc.Height,
                    static_cast<unsigned>(d->BufferDesc.Format), d->BufferCount, static_cast<unsigned>(d->SwapEffect),
                    static_cast<unsigned long>(vhr));
            return vhr;
        }
        void* ctx = nullptr;
        const user::SwapChainTargetFn target = user::GetSwapChainTarget(&ctx);
        IDXGISwapChain* real = nullptr;
        HRESULT hr;
        if (target && f2_) {
            DXGI_SWAP_CHAIN_DESC1 d1{};
            d1.Width = d->BufferDesc.Width;
            d1.Height = d->BufferDesc.Height;
            d1.Format = d->BufferDesc.Format;
            d1.SampleDesc = d->SampleDesc;
            d1.BufferUsage = d->BufferUsage;
            d1.BufferCount = d->BufferCount < 2 ? 2 : d->BufferCount;
            d1.Scaling = DXGI_SCALING_STRETCH;
            d1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;   // the only effect a CoreWindow swap chain takes
            d1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            d1.Flags = d->Flags & ~static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
            IDXGISwapChain1* r1 = nullptr;
            hr = static_cast<HRESULT>(target(f2_, dev, &d1, reinterpret_cast<void**>(&r1), ctx));
            real = r1;
        } else {
            hr = f0_->CreateSwapChain(dev, d, &real);
        }
        if (SUCCEEDED(hr) && real) *out = new WrapSwap(real, target ? nullptr : d->OutputWindow);
        W10X_TR( "dxgi: IDXGIFactory::CreateSwapChain(window %p, %ux%u fmt %u, %u buffers, effect %u)%s -> %08lx",
              static_cast<void*>(d->OutputWindow), d->BufferDesc.Width, d->BufferDesc.Height,
              static_cast<unsigned>(d->BufferDesc.Format), d->BufferCount, static_cast<unsigned>(d->SwapEffect),
              target ? " on the host's target" : "", static_cast<unsigned long>(hr));
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE m, IDXGIAdapter** a) override { return f0_->CreateSoftwareAdapter(m, a); }
    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT i, IDXGIAdapter1** a) override {
        if (!f1_) return DXGI_ERROR_UNSUPPORTED;
        IDXGIAdapter1* real = nullptr;
        const HRESULT hr = f1_->EnumAdapters1(i, a ? &real : nullptr);
        if (FAILED(hr) || !real || !a) return hr;
        if (user::GetSwapChainTarget(nullptr)) {
            *a = new WrapAdapter(real);
        } else {
            *a = real;
        }
        return hr;
    }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return f1_ ? f1_->IsCurrent() : TRUE; }
    // IDXGIFactory2
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return f2_ ? f2_->IsWindowedStereoEnabled() : FALSE; }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrict_out,
                                                     IDXGISwapChain1** out) override {
        if (!f2_) return DXGI_ERROR_UNSUPPORTED;
        if (!d || !out) return f2_->CreateSwapChainForHwnd(dev, hwnd, d, fs, restrict_out, out);
        if (IsVirtualWindow(hwnd)) {   // an emulated window: a virtual swap chain (VirtSwap)
            const HRESULT vhr = VirtSwap::Create(dev, hwnd, *d, fs ? fs->Windowed : TRUE, f0_, out);
            W10X_TR("dxgi: IDXGIFactory2::CreateSwapChainForHwnd(window %p, %ux%u fmt %u, %u buffers, effect %u) virtual -> %08lx",
                    static_cast<void*>(hwnd), d->Width, d->Height, static_cast<unsigned>(d->Format), d->BufferCount,
                    static_cast<unsigned>(d->SwapEffect), static_cast<unsigned long>(vhr));
            return vhr;
        }
        void* ctx = nullptr;
        const user::SwapChainTargetFn target = user::GetSwapChainTarget(&ctx);
        IDXGISwapChain1* real = nullptr;
        HRESULT hr;
        if (target) {
            DXGI_SWAP_CHAIN_DESC1 d1 = *d;
            d1.Scaling = DXGI_SCALING_STRETCH;
            d1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            if (d1.BufferCount < 2) d1.BufferCount = 2;
            d1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            hr = static_cast<HRESULT>(target(f2_, dev, &d1, reinterpret_cast<void**>(&real), ctx));
        } else {
            hr = f2_->CreateSwapChainForHwnd(dev, hwnd, d, fs, restrict_out, &real);
        }
        if (SUCCEEDED(hr) && real) *out = new WrapSwap(real, target ? nullptr : hwnd);
        W10X_TR( "dxgi: IDXGIFactory2::CreateSwapChainForHwnd(window %p, %ux%u fmt %u, %u buffers, effect %u)%s -> %08lx",
              static_cast<void*>(hwnd), d->Width, d->Height, static_cast<unsigned>(d->Format), d->BufferCount,
              static_cast<unsigned>(d->SwapEffect), target ? " on the host's target" : "", static_cast<unsigned long>(hr));
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* dev, IUnknown* win, const DXGI_SWAP_CHAIN_DESC1* d,
                                                           IDXGIOutput* ro, IDXGISwapChain1** out) override {
        if (!f2_) return DXGI_ERROR_UNSUPPORTED;
        IDXGISwapChain1* real = nullptr;
        const HRESULT hr = f2_->CreateSwapChainForCoreWindow(dev, win, d, ro, out ? &real : nullptr);
        if (SUCCEEDED(hr) && real && out) *out = new WrapSwap(real, nullptr);
        W10X_TRACE_HR("IDXGIFactory2::CreateSwapChainForCoreWindow", hr);
    }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE r, LUID* l) override {
        return f2_ ? f2_->GetSharedResourceAdapterLuid(r, l) : DXGI_ERROR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND w, UINT m, DWORD* c) override {
        return f2_ ? f2_->RegisterStereoStatusWindow(w, m, c) : DXGI_ERROR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE e, DWORD* c) override {
        return f2_ ? f2_->RegisterStereoStatusEvent(e, c) : DXGI_ERROR_UNSUPPORTED;
    }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD c) override { if (f2_) f2_->UnregisterStereoStatus(c); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND w, UINT m, DWORD* c) override {
        return f2_ ? f2_->RegisterOcclusionStatusWindow(w, m, c) : DXGI_ERROR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE e, DWORD* c) override {
        return f2_ ? f2_->RegisterOcclusionStatusEvent(e, c) : DXGI_ERROR_UNSUPPORTED;
    }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD c) override { if (f2_) f2_->UnregisterOcclusionStatus(c); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* ro,
                                                            IDXGISwapChain1** out) override {
        if (!f2_) return DXGI_ERROR_UNSUPPORTED;
        IDXGISwapChain1* real = nullptr;
        const HRESULT hr = f2_->CreateSwapChainForComposition(dev, d, ro, out ? &real : nullptr);
        if (SUCCEEDED(hr) && real && out) *out = new WrapSwap(real, nullptr);
        W10X_TRACE_HR("IDXGIFactory2::CreateSwapChainForComposition", hr);
    }

private:
    LONG ref_ = 1;
    IDXGIFactory* f0_;
    IDXGIFactory1* f1_ = nullptr;
    IDXGIFactory2* f2_ = nullptr;
};

// Wraps `real` (one reference, consumed) and returns the wrapper's interface `riid` in *pp.
HRESULT WrapFactoryFor(IDXGIFactory* real, REFIID riid, void** pp) {
    auto* w = new WrapFactory(real);
    const HRESULT hr = w->QueryInterface(riid, pp);
    w->Release();
    return hr;
}

// ---- The functions the guest's imports are bound to ---------------------------------------------------------

HRESULT CreateWrappedFactory(HRESULT real_hr, IUnknown* real_unk, REFIID riid, void** pp) {
    if (FAILED(real_hr) || !real_unk) return real_hr;
    IDXGIFactory* f = nullptr;
    const HRESULT qhr = real_unk->QueryInterface(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&f));
    real_unk->Release();
    if (FAILED(qhr) || !f) return qhr;
    return WrapFactoryFor(f, riid, pp);
}

HRESULT WINAPI W10x_CreateDXGIFactory(REFIID riid, void** pp) {
    if (!g_real_create_factory) return E_FAIL;
    if (CalledFromInternal(W10X_RETADDR())) return g_real_create_factory(riid, pp);
    if (!pp || !IsWrappedFactoryIid(riid)) W10X_TRACE_HR("CreateDXGIFactory (not wrapped)", g_real_create_factory(riid, pp));
    IUnknown* u = nullptr;
    const HRESULT real_hr = g_real_create_factory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&u));
    const HRESULT hr = CreateWrappedFactory(real_hr, u, riid, pp);
    if (FAILED(hr)) DumpExceptions(FAILED(real_hr) ? "dxgi!CreateDXGIFactory" : "CreateDXGIFactory (wrapping)", hr);
    W10X_TRACE_HR("CreateDXGIFactory", hr);
}

HRESULT WINAPI W10x_CreateDXGIFactory1(REFIID riid, void** pp) {
    if (!g_real_create_factory1) return E_FAIL;
    if (CalledFromInternal(W10X_RETADDR())) return g_real_create_factory1(riid, pp);
    if (!pp || !IsWrappedFactoryIid(riid)) W10X_TRACE_HR("CreateDXGIFactory1 (not wrapped)", g_real_create_factory1(riid, pp));
    IUnknown* u = nullptr;
    const HRESULT real_hr = g_real_create_factory1(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&u));
    const HRESULT hr = CreateWrappedFactory(real_hr, u, riid, pp);
    if (FAILED(hr)) DumpExceptions(FAILED(real_hr) ? "dxgi!CreateDXGIFactory1" : "CreateDXGIFactory1 (wrapping)", hr);
    else if (g_exc_n) DumpExceptions("CreateDXGIFactory1 succeeded; exceptions raised inside it (round 12 comparison, both machines)", hr);
    W10X_TRACE_HR("CreateDXGIFactory1", hr);
}

HRESULT WINAPI W10x_CreateDXGIFactory2(UINT flags, REFIID riid, void** pp) {
    if (!g_real_create_factory2) return E_FAIL;
    if (CalledFromInternal(W10X_RETADDR())) return g_real_create_factory2(flags, riid, pp);
    if (!pp || !IsWrappedFactoryIid(riid)) W10X_TRACE_HR("CreateDXGIFactory2 (not wrapped)", g_real_create_factory2(flags, riid, pp));
    IUnknown* u = nullptr;
    const HRESULT real_hr = g_real_create_factory2(flags, __uuidof(IDXGIFactory), reinterpret_cast<void**>(&u));
    const HRESULT hr = CreateWrappedFactory(real_hr, u, riid, pp);
    if (FAILED(hr)) DumpExceptions(FAILED(real_hr) ? "dxgi!CreateDXGIFactory2" : "CreateDXGIFactory2 (wrapping)", hr);
    W10X_TRACE_HR("CreateDXGIFactory2", hr);
}

HRESULT WINAPI W10x_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                      const D3D_FEATURE_LEVEL* levels, UINT n_levels, UINT sdk, ID3D11Device** out_dev,
                                      D3D_FEATURE_LEVEL* out_level, ID3D11DeviceContext** out_ctx) {
    if (!g_real_create_device) return E_FAIL;
    IDXGIAdapter* owned = nullptr;
    IDXGIAdapter* real = UnwrapAdapter(adapter, &owned);
    const HRESULT hr = g_real_create_device(real, type, sw, flags, levels, n_levels, sdk, out_dev, out_level, out_ctx);
    if (owned) owned->Release();
    return hr;
}

HRESULT WINAPI W10x_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter_in, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                                  const D3D_FEATURE_LEVEL* levels, UINT n_levels, UINT sdk,
                                                  const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swap,
                                                  ID3D11Device** out_dev, D3D_FEATURE_LEVEL* out_level,
                                                  ID3D11DeviceContext** out_ctx) {
    if (!g_real_create_device) return E_FAIL;
    IDXGIAdapter* owned_adapter = nullptr;
    IDXGIAdapter* adapter = UnwrapAdapter(adapter_in, &owned_adapter);
    struct Release { IDXGIAdapter* a; ~Release() { if (a) a->Release(); } } release_adapter{owned_adapter};
    if (!desc || !swap) {   // device only: the d3d11's own function (its original export, when it has one)
        if (g_real_create_device_and_swap)
            W10X_TRACE_HR("D3D11CreateDeviceAndSwapChain (no swap chain)",
                          g_real_create_device_and_swap(adapter, type, sw, flags, levels, n_levels, sdk, desc, swap, out_dev, out_level, out_ctx));
        W10X_TRACE_HR("D3D11CreateDeviceAndSwapChain (no swap chain)",
                      g_real_create_device(adapter, type, sw, flags, levels, n_levels, sdk, out_dev, out_level, out_ctx));
    }
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL level = static_cast<D3D_FEATURE_LEVEL>(0);
    HRESULT hr = g_real_create_device(adapter, type, sw, flags, levels, n_levels, sdk, &dev, &level, &ctx);
    W10X_TR( "dxgi: D3D11CreateDevice (for D3D11CreateDeviceAndSwapChain, driver type %d) -> %08lx feature level 0x%x",
          static_cast<int>(type), static_cast<unsigned long>(hr), static_cast<unsigned>(level));
    if (FAILED(hr) || !dev) {
        DumpExceptions("d3d11!D3D11CreateDevice", FAILED(hr) ? hr : E_FAIL);
        return FAILED(hr) ? hr : E_FAIL;
    }
    IDXGIFactory* real_factory = nullptr;
    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* ad = nullptr;
    hr = dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev));
    if (SUCCEEDED(hr)) hr = dxgi_dev->GetAdapter(&ad);
    if (SUCCEEDED(hr)) hr = ad->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&real_factory));
    if (ad) ad->Release();
    if (dxgi_dev) dxgi_dev->Release();
    if (FAILED(hr) || !real_factory) {
        dev->Release();
        if (ctx) ctx->Release();
        DumpExceptions("D3D11CreateDeviceAndSwapChain (device -> factory)", FAILED(hr) ? hr : E_FAIL);
        W10X_TRACE_HR("D3D11CreateDeviceAndSwapChain (device -> factory)", FAILED(hr) ? hr : E_FAIL);
    }
    auto* wf = new WrapFactory(real_factory);
    DXGI_SWAP_CHAIN_DESC d = *desc;
    IDXGISwapChain* sc = nullptr;
    hr = wf->CreateSwapChain(dev, &d, &sc);
    wf->Release();
    if (FAILED(hr) || !sc) {
        dev->Release();
        if (ctx) ctx->Release();
        DumpExceptions("D3D11CreateDeviceAndSwapChain (CreateSwapChain)", FAILED(hr) ? hr : E_FAIL);
        W10X_TRACE_HR("D3D11CreateDeviceAndSwapChain", FAILED(hr) ? hr : E_FAIL);
    }
    *swap = sc;
    if (out_dev) *out_dev = dev; else dev->Release();
    if (out_ctx) *out_ctx = ctx; else if (ctx) ctx->Release();
    if (out_level) *out_level = level;
    W10X_TRACE_HR("D3D11CreateDeviceAndSwapChain", S_OK);
}

// ---- Export rewriting ----------------------------------------------------------------------------------------

bool IsModule(const std::wstring& id, const wchar_t* name) {
    const std::size_t n = std::wcslen(name);
    if (id.size() < n || _wcsicmp(id.c_str() + id.size() - n, name) != 0) return false;
    return id.size() == n || id[id.size() - n - 1] == L'\\' || id[id.size() - n - 1] == L'/';
}

struct Replace {
    const char* name;
    const void* layer_fn;
    void** original_out;   // receives the image's own export (address as a function pointer), may be null
};

// Points the named code exports of the (still writable, unrelocated) image at `jmp [abs64]` slots that live within
// 4 GiB above the image and jump to the layer's functions. Returns how many were replaced.
std::size_t ReplaceExports(const loader::MappedImage& img, const Replace* list, std::size_t count, const char* label) {
    auto* base = static_cast<std::uint8_t*>(img.base);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress || !dd.Size) return 0;
    auto* dir = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dd.VirtualAddress);
    auto* functions = reinterpret_cast<DWORD*>(base + dir->AddressOfFunctions);
    auto* names = reinterpret_cast<DWORD*>(base + dir->AddressOfNames);
    auto* ords = reinterpret_cast<WORD*>(base + dir->AddressOfNameOrdinals);
    constexpr std::size_t kSlot = 16;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity, b = reinterpret_cast<std::uintptr_t>(base);
    const std::size_t total = count * kSlot;
    std::uint8_t* mem = nullptr;
    for (std::uintptr_t a = (b + img.size + gran - 1) & ~(gran - 1); a + total - b < 0xFFFF0000ull && !mem; a += gran) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) || mbi.State != MEM_FREE) continue;
        mem = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(a), total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    if (!mem) {
        W10X_WN( "dxgi: %s: no room within 4 GiB of the image for the interposition slots", label);
        return 0;
    }
    std::size_t done = 0;
    std::string names_done;
    for (std::size_t k = 0; k < count; ++k) {
        for (DWORD i = 0; i < dir->NumberOfNames; ++i) {
            if (std::strcmp(reinterpret_cast<const char*>(base + names[i]), list[k].name) != 0) continue;
            const DWORD rva = functions[ords[i]];
            if (rva >= dd.VirtualAddress && rva < dd.VirtualAddress + dd.Size) break;   // a forwarder: not code
            if (list[k].original_out) *list[k].original_out = base + rva;
            std::uint8_t* slot = mem + done * kSlot;
            const std::uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
            std::memcpy(slot, jmp, sizeof jmp);
            const std::uint64_t target = reinterpret_cast<std::uint64_t>(list[k].layer_fn);
            std::memcpy(slot + 6, &target, 8);
            std::memset(slot + 14, 0xCC, kSlot - 14);
            functions[ords[i]] = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(slot) - b);
            ++done;
            (names_done += names_done.empty() ? "" : ", ") += list[k].name;
            break;
        }
    }
    DWORD old = 0;
    VirtualProtect(mem, total, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, total);
    W10X_TR( "dxgi: %s: %zu export(s) interposed (%s)", label, done, names_done.c_str());
    return done;
}

}  // namespace

bool InterposeDxgiImage(const loader::MappedImage& img, const std::wstring& id) {
    if (IsModule(id, L"dxgi.dll")) {
        InstallExcRecorder();
        g_internal[0] = {reinterpret_cast<std::uintptr_t>(img.base), static_cast<std::uintptr_t>(img.size)};
        const Replace list[] = {
            {"CreateDXGIFactory", reinterpret_cast<const void*>(&W10x_CreateDXGIFactory), reinterpret_cast<void**>(&g_real_create_factory)},
            {"CreateDXGIFactory1", reinterpret_cast<const void*>(&W10x_CreateDXGIFactory1), reinterpret_cast<void**>(&g_real_create_factory1)},
            {"CreateDXGIFactory2", reinterpret_cast<const void*>(&W10x_CreateDXGIFactory2), reinterpret_cast<void**>(&g_real_create_factory2)},
        };
        return ReplaceExports(img, list, 3, "dxgi.dll") != 0;
    }
    if (IsModule(id, L"d3d11.dll")) {
        g_internal[1] = {reinterpret_cast<std::uintptr_t>(img.base), static_cast<std::uintptr_t>(img.size)};
        // D3D11CreateDevice's own code is read first (the layer calls it); both exports are then replaced (a wrapped
        // adapter is unwrapped on the way in).
        void* real_device_and_swap = nullptr;
        const Replace list[] = {
            {"D3D11CreateDeviceAndSwapChain", reinterpret_cast<const void*>(&W10x_D3D11CreateDeviceAndSwapChain), &real_device_and_swap},
            {"D3D11CreateDevice", reinterpret_cast<const void*>(&W10x_D3D11CreateDevice), nullptr},
        };
        // The original D3D11CreateDevice: look it up before the table is touched.
        auto* base = static_cast<std::uint8_t*>(img.base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dd.VirtualAddress) return false;
        auto* dir = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dd.VirtualAddress);
        auto* names = reinterpret_cast<DWORD*>(base + dir->AddressOfNames);
        auto* ords = reinterpret_cast<WORD*>(base + dir->AddressOfNameOrdinals);
        auto* functions = reinterpret_cast<DWORD*>(base + dir->AddressOfFunctions);
        for (DWORD i = 0; i < dir->NumberOfNames; ++i)
            if (!std::strcmp(reinterpret_cast<const char*>(base + names[i]), "D3D11CreateDevice") &&
                !(functions[ords[i]] >= dd.VirtualAddress && functions[ords[i]] < dd.VirtualAddress + dd.Size))
                g_real_create_device = reinterpret_cast<CreateDeviceFn>(base + functions[ords[i]]);
        if (!g_real_create_device) {
            W10X_WN( "dxgi: d3d11.dll has no D3D11CreateDevice code export: D3D11CreateDeviceAndSwapChain not interposed");
            return false;
        }
        const std::size_t n = ReplaceExports(img, list, 2, "d3d11.dll");
        g_real_create_device_and_swap = reinterpret_cast<CreateDeviceAndSwapFn>(real_device_and_swap);
        return n != 0;
    }
    return false;
}

void SetPresentLimitImpl(unsigned n) { g_present_limit.store(n); g_presents.store(0); }

bool IsDxgiInterposedModule(const std::wstring& id) { return IsModule(id, L"dxgi.dll") || IsModule(id, L"d3d11.dll"); }

}  // namespace win10xbox::dispatch::detail

namespace win10xbox::dispatch::detail {
// Called by NtRaiseException before the host kernel is asked to raise a fail-fast record (it bypasses every handler).
void DumpFailFastRaise(const EXCEPTION_RECORD* rec, const CONTEXT* ctx) {
    if (!rec || !ctx) return;
    EXCEPTION_POINTERS ep{const_cast<EXCEPTION_RECORD*>(rec), const_cast<CONTEXT*>(ctx)};
    DumpFailFast(&ep);
}
}  // namespace win10xbox::dispatch::detail

namespace win10xbox::dispatch {
namespace detail { void SetPresentLimitImpl(unsigned n); }
void SetPresentLimit(unsigned n) { detail::SetPresentLimitImpl(n); }
}  // namespace win10xbox::dispatch
