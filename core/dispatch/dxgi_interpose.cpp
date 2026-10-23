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

#include "win10xbox/user.h"

#include <d3d11.h>
#include <dxgi1_6.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif
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
        const bool cap = (flags & DXGI_PRESENT_TEST) == 0 && Capture();
        const HRESULT hr = sc0_->Present(sync, flags);
        W10X_TR( "dxgi: IDXGISwapChain::Present(%u, 0x%x) -> %08lx%s", sync, flags, static_cast<unsigned long>(hr),
              cap ? " (back buffer copied)" : "");
        if (cap && SUCCEEDED(hr)) Deliver();
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
        return hr;
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return sc1_ ? sc1_->IsTemporaryMonoSupported() : FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override { return sc1_ ? sc1_->GetRestrictToOutput(o) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override { return sc1_ ? sc1_->SetBackgroundColor(c) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override { return sc1_ ? sc1_->GetBackgroundColor(c) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override { return sc1_ ? sc1_->SetRotation(r) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override { return sc1_ ? sc1_->GetRotation(r) : E_NOTIMPL; }

private:
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
    ID3D11Texture2D* staging_ = nullptr;
    UINT st_w_ = 0, st_h_ = 0;
    DXGI_FORMAT st_fmt_ = DXGI_FORMAT_UNKNOWN;
    std::vector<std::uint32_t> pix_;
    std::int32_t pix_w_ = 0, pix_h_ = 0;
};

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
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT i, IDXGIAdapter** a) override { return f0_->EnumAdapters(i, a); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND h, UINT fl) override { W10X_TRACE_HR("IDXGIFactory::MakeWindowAssociation", f0_->MakeWindowAssociation(h, fl)); }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* h) override { return f0_->GetWindowAssociation(h); }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) override {
        if (!d || !out) return f0_->CreateSwapChain(dev, d, out);
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
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT i, IDXGIAdapter1** a) override { return f1_ ? f1_->EnumAdapters1(i, a) : DXGI_ERROR_UNSUPPORTED; }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return f1_ ? f1_->IsCurrent() : TRUE; }
    // IDXGIFactory2
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return f2_ ? f2_->IsWindowedStereoEnabled() : FALSE; }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrict_out,
                                                     IDXGISwapChain1** out) override {
        if (!f2_) return DXGI_ERROR_UNSUPPORTED;
        if (!d || !out) return f2_->CreateSwapChainForHwnd(dev, hwnd, d, fs, restrict_out, out);
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
    const HRESULT hr = CreateWrappedFactory(g_real_create_factory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&u)), u, riid, pp);
    W10X_TRACE_HR("CreateDXGIFactory", hr);
}

HRESULT WINAPI W10x_CreateDXGIFactory1(REFIID riid, void** pp) {
    if (!g_real_create_factory1) return E_FAIL;
    if (CalledFromInternal(W10X_RETADDR())) return g_real_create_factory1(riid, pp);
    if (!pp || !IsWrappedFactoryIid(riid)) W10X_TRACE_HR("CreateDXGIFactory1 (not wrapped)", g_real_create_factory1(riid, pp));
    IUnknown* u = nullptr;
    const HRESULT hr = CreateWrappedFactory(g_real_create_factory1(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&u)), u, riid, pp);
    W10X_TRACE_HR("CreateDXGIFactory1", hr);
}

HRESULT WINAPI W10x_CreateDXGIFactory2(UINT flags, REFIID riid, void** pp) {
    if (!g_real_create_factory2) return E_FAIL;
    if (CalledFromInternal(W10X_RETADDR())) return g_real_create_factory2(flags, riid, pp);
    if (!pp || !IsWrappedFactoryIid(riid)) W10X_TRACE_HR("CreateDXGIFactory2 (not wrapped)", g_real_create_factory2(flags, riid, pp));
    IUnknown* u = nullptr;
    const HRESULT hr = CreateWrappedFactory(g_real_create_factory2(flags, __uuidof(IDXGIFactory), reinterpret_cast<void**>(&u)), u, riid, pp);
    W10X_TRACE_HR("CreateDXGIFactory2", hr);
}

HRESULT WINAPI W10x_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                                  const D3D_FEATURE_LEVEL* levels, UINT n_levels, UINT sdk,
                                                  const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swap,
                                                  ID3D11Device** out_dev, D3D_FEATURE_LEVEL* out_level,
                                                  ID3D11DeviceContext** out_ctx) {
    if (!g_real_create_device) return E_FAIL;
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
    if (FAILED(hr) || !dev) return FAILED(hr) ? hr : E_FAIL;
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
        // D3D11CreateDevice is only read (the layer calls it), D3D11CreateDeviceAndSwapChain is replaced.
        void* real_device_and_swap = nullptr;
        const Replace list[] = {
            {"D3D11CreateDeviceAndSwapChain", reinterpret_cast<const void*>(&W10x_D3D11CreateDeviceAndSwapChain), &real_device_and_swap},
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
        const std::size_t n = ReplaceExports(img, list, 1, "d3d11.dll");
        g_real_create_device_and_swap = reinterpret_cast<CreateDeviceAndSwapFn>(real_device_and_swap);
        return n != 0;
    }
    return false;
}

bool IsDxgiInterposedModule(const std::wstring& id) { return IsModule(id, L"dxgi.dll") || IsModule(id, L"d3d11.dll"); }

}  // namespace win10xbox::dispatch::detail
