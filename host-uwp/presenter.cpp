// presenter.cpp - see presenter.h. The swap chain follows the DirectX UWP DeviceResources
// pattern: CreateSwapChainForCoreWindow on the device's own DXGI factory, B8G8R8A8_UNORM,
// two buffers, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_SCALING_STRETCH, DXGI_ALPHA_MODE_IGNORE.
// No shaders and no render target view: the frame is a CPU image, so the whole "renderer" is
// UpdateSubresource + CopySubresourceRegion into the back buffer (blit.h) + Present(1, 0).
#include "presenter.h"

#include "blit.h"
#include "host_util.h"

#include "win10xbox/user.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace w10x_uwp
{
namespace
{
    constexpr ULONGLONG kRefreshMs = 500;      // compose + present at least this often, so a first frame appears
    constexpr ULONGLONG kShotMs = 5000;        // LocalState\screen.bmp while the guest runs
    constexpr std::uint32_t kBackground = 0xFF202020u;   // dark grey until the first composed frame
    constexpr int kMaxPresentFailures = 30;    // consecutive failed Presents before giving up (headless)

    std::string Hr(HRESULT hr) { return Hex32(static_cast<long>(hr)); }

    std::string LevelName(D3D_FEATURE_LEVEL fl)
    {
        const unsigned v = static_cast<unsigned>(fl);
        return Fmt("%u_%u", (v >> 12) & 0xFu, (v >> 8) & 0xFu);
    }

    std::string AdapterName(ID3D11Device* device)
    {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC d = {};
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(adapter->GetDesc(&d)))
            return "?";
        return Narrow(d.Description);
    }

    // What the device thread hands over to the UI thread.
    struct DeviceSlot
    {
        std::mutex mu;
        std::atomic<int> state{static_cast<int>(Presenter::Device::NotStarted)};
        ComPtr<ID3D11Device> device;         // guarded by mu
        ComPtr<ID3D11DeviceContext> context; // guarded by mu
        std::string kind;                    // "hardware" / "WARP", guarded by mu
        std::string note;                    // guarded by mu
    };

    // The device thread: the only place D3D11CreateDevice is called (never the UI thread, see
    // presenter.h). Hardware first, then WARP.
    void DeviceThread(std::shared_ptr<DeviceSlot> slot)
    {
        const ULONGLONG t0 = GetTickCount64();
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL fl = static_cast<D3D_FEATURE_LEVEL>(0);
        std::string kind = "hardware";
        std::string note;
        const HRESULT hw = CreateBlitDevice(D3D_DRIVER_TYPE_HARDWARE, &device, &context, &fl);
        HRESULT hr = hw;
        if (FAILED(hw))
        {
            kind = "WARP";
            hr = CreateBlitDevice(D3D_DRIVER_TYPE_WARP, &device, &context, &fl);
        }
        const unsigned long long ms = GetTickCount64() - t0;
        if (SUCCEEDED(hr))
            note = Fmt("D3D11 %s device, feature level %s, adapter '%s', created on the device thread in %llu ms%s",
                       kind.c_str(), LevelName(fl).c_str(), AdapterName(device.Get()).c_str(), ms,
                       FAILED(hw) ? (" (hardware D3D11CreateDevice failed with " + Hr(hw) + ")").c_str() : "");
        else
            note = Fmt("no D3D11 device: hardware D3D11CreateDevice %s, WARP %s (%llu ms): running headless, "
                       "screen.bmp is still written",
                       Hr(hw).c_str(), Hr(hr).c_str(), ms);
        std::lock_guard<std::mutex> lock(slot->mu);
        slot->device = device;
        slot->context = context;
        slot->kind = kind;
        slot->note = note;
        slot->state.store(static_cast<int>(SUCCEEDED(hr) ? Presenter::Device::Ready : Presenter::Device::Failed));
    }

    // 32-bit top-down BMP (BITMAPFILEHEADER + BITMAPINFOHEADER, BI_RGB), like user::WriteScreenshot.
    bool WriteBmp(HANDLE h, const std::vector<std::uint32_t>& pixels, int w, int hgt)
    {
        const std::uint32_t image_bytes = static_cast<std::uint32_t>(pixels.size() * 4u);
        std::uint8_t hdr[14 + 40] = {};
        auto put32 = [&](std::size_t off, std::uint32_t v) { std::memcpy(hdr + off, &v, 4); };
        auto put16 = [&](std::size_t off, std::uint16_t v) { std::memcpy(hdr + off, &v, 2); };
        hdr[0] = 'B';
        hdr[1] = 'M';
        put32(2, 14 + 40 + image_bytes);
        put32(10, 14 + 40);
        put32(14, 40);
        put32(18, static_cast<std::uint32_t>(w));
        put32(22, static_cast<std::uint32_t>(-hgt));
        put16(26, 1);
        put16(28, 32);
        put32(34, image_bytes);
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) return false;
        DWORD put = 0;
        if (!WriteFile(h, hdr, sizeof hdr, &put, nullptr) || put != sizeof hdr) return false;
        if (!WriteFile(h, pixels.data(), image_bytes, &put, nullptr) || put != image_bytes) return false;
        return SetEndOfFile(h) != FALSE;
    }
}  // namespace

struct Presenter::Impl
{
    void TakeDevice();
    void CreateSwapChain();
    void Present(ULONGLONG now);
    void Note(const std::string& s) { notes.push_back(s); }

    ComPtr<IUnknown> window;
    int win_w = 0, win_h = 0, scr_w = 0, scr_h = 0;

    std::shared_ptr<DeviceSlot> slot = std::make_shared<DeviceSlot>();
    bool device_reported = false;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::string kind;
    ComPtr<IDXGISwapChain1> swap;
    FrameBlitter blit;
    bool swap_failed = false;
    bool dead = false;                // device removed / Present keeps failing: headless from here on
    int present_failures = 0;
    HRESULT last_present_hr = S_OK;

    std::vector<std::uint32_t> frame;
    bool composed = false;            // frame holds at least one composed frame
    bool uploaded = false;            // frame is in the texture
    std::uint64_t last_serial = ~0ull;
    ULONGLONG last_compose = 0, last_present = 0, last_shot = 0;
    unsigned long long presents = 0, composes = 0, shots = 0;

    std::wstring shot_path;
    HANDLE shot = INVALID_HANDLE_VALUE;
    bool shot_failed_noted = false;

    FrameSource* source = nullptr;    // launcher screens, see SetFrameSource

    std::vector<std::string> notes;

    // A route-B guest's swap chain on the CoreWindow (user::SetSwapChainTarget, core/dispatch/dxgi_interpose.cpp):
    // a CoreWindow takes one swap chain, so the presenter's own is released and the guest's frames are what is on
    // screen from then on. The request comes from a guest thread, the release happens in Tick (UI thread).
    std::atomic<int> guest_request{0};   // 1: a guest asked for the CoreWindow
    std::atomic<int> guest_owns{0};      // 1: the presenter's swap chain is gone
    std::atomic<long> guest_hr{1};       // 1 = not created yet, else the HRESULT of the guest's CreateSwapChainForCoreWindow
    bool guest_noted = false;
};

void Presenter::Impl::TakeDevice()
{
    if (device_reported) return;
    const int st = slot->state.load();
    if (st != static_cast<int>(Device::Ready) && st != static_cast<int>(Device::Failed)) return;
    device_reported = true;
    {
        std::lock_guard<std::mutex> lock(slot->mu);
        device = slot->device;
        context = slot->context;
        kind = slot->kind;
        Note(slot->note);
    }
    if (!device) return;
    const HRESULT hr = blit.Init(device.Get(), context.Get(), scr_w, scr_h, frame.data());
    if (FAILED(hr))
    {
        Note(Fmt("cannot create the %dx%d BGRA screen texture (%s): running headless", scr_w, scr_h, Hr(hr).c_str()));
        dead = true;
        return;
    }
    uploaded = true;   // the texture was created from the current frame (background or composed)
}

void Presenter::Impl::CreateSwapChain()
{
    const int w = win_w > 0 ? win_w : scr_w;
    const int h = win_h > 0 ? win_h : scr_h;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = device.As(&dxgi);
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        swap_failed = true;
        Note("no IDXGIFactory2 from the D3D11 device (" + Hr(hr) + "): running headless");
        return;
    }
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.Stereo = FALSE;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = 0;
    hr = factory->CreateSwapChainForCoreWindow(device.Get(), window.Get(), &d, nullptr, swap.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        swap_failed = true;
        swap.Reset();
        Note(Fmt("CreateSwapChainForCoreWindow(%dx%d, B8G8R8A8, flip-sequential, 2 buffers) failed with %s: running "
                 "headless",
                 w, h, Hr(hr).c_str()));
        return;
    }
    Note(Fmt("swap chain on the CoreWindow: %dx%d B8G8R8A8 flip-sequential, 2 buffers; screen %dx%d%s", w, h, scr_w, scr_h,
             (w != scr_w || h != scr_h) ? " (sizes differ: the overlapping region is copied)" : ""));
}

void Presenter::Impl::Present(ULONGLONG now)
{
    last_present = now;
    if (!uploaded)
    {
        blit.Upload(frame.data(), scr_w * 4);
        uploaded = true;
    }
    ComPtr<ID3D11Texture2D> back;
    HRESULT hr = swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (SUCCEEDED(hr))
    {
        blit.CopyTo(back.Get());
        back.Reset();
        hr = swap->Present(1, 0);
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        dead = true;
        Note("Present: the device was removed (" + Hr(hr) + ", reason " + Hr(device->GetDeviceRemovedReason()) +
             "): running headless, the last frame stays on screen");
        return;
    }
    if (FAILED(hr))
    {
        if (hr != last_present_hr) Note("Present failed with " + Hr(hr));
        last_present_hr = hr;
        if (++present_failures >= kMaxPresentFailures)
        {
            dead = true;
            Note(Fmt("Present failed %d times in a row: running headless", present_failures));
        }
        return;
    }
    present_failures = 0;
    last_present_hr = hr;
    if (presents++ == 0) Note("first Present succeeded" + std::string(composed ? "" : " (background only: nothing composed yet)"));
}

// ----------------------------------------------------------------------
namespace
{
    // user::SwapChainTargetFn: runs on the guest's thread.
    long GuestSwapChainTarget(void* factory, void* device, const void* desc1, void** out, void* ctx)
    {
        auto* m = static_cast<Presenter::Impl*>(ctx);
        m->guest_request = 1;
        for (int i = 0; i < 300 && !m->guest_owns; ++i) Sleep(10);   // the UI thread releases its swap chain in Tick
        if (!m->guest_owns || !m->window) return E_FAIL;
        const HRESULT hr = static_cast<IDXGIFactory2*>(factory)->CreateSwapChainForCoreWindow(
            static_cast<IUnknown*>(device), m->window.Get(), static_cast<const DXGI_SWAP_CHAIN_DESC1*>(desc1), nullptr,
            reinterpret_cast<IDXGISwapChain1**>(out));
        m->guest_hr = static_cast<long>(hr);
        return static_cast<long>(hr);
    }
}  // namespace

Presenter::Presenter() : impl_(new Impl) {}

Presenter::~Presenter()
{
    if (impl_->shot != INVALID_HANDLE_VALUE) CloseHandle(impl_->shot);
}

void Presenter::Configure(IUnknown* core_window, int window_width, int window_height, int screen_width, int screen_height)
{
    Impl& m = *impl_;
    m.window = core_window;
    m.win_w = window_width;
    m.win_h = window_height;
    m.scr_w = screen_width > 0 ? screen_width : (window_width > 0 ? window_width : 1920);
    m.scr_h = screen_height > 0 ? screen_height : (window_height > 0 ? window_height : 1080);
    m.frame.assign(static_cast<std::size_t>(m.scr_w) * static_cast<std::size_t>(m.scr_h), kBackground);
    win10xbox::user::SetSwapChainTarget(&GuestSwapChainTarget, impl_.get());
}

void Presenter::OpenScreenshot(const std::wstring& path)
{
    Impl& m = *impl_;
    if (m.shot != INVALID_HANDLE_VALUE) return;
    m.shot_path = path;
    // CREATE_ALWAYS: a screen.bmp of the previous run must not pass for this one's. Shared for
    // reading (Device Portal) and writing (user::WriteScreenshot at the end opens it by path).
    m.shot = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m.shot == INVALID_HANDLE_VALUE)
        m.Note(Fmt("cannot create screen.bmp (error %lu): no periodic screenshots", GetLastError()));
}

void Presenter::SetFrameSource(FrameSource* source)
{
    impl_->source = source;
}

void Presenter::StartDevice()
{
    Impl& m = *impl_;
    if (m.slot->state.load() != static_cast<int>(Device::NotStarted)) return;
    m.slot->state.store(static_cast<int>(Device::Pending));
    try
    {
        std::thread(DeviceThread, m.slot).detach();   // abandoned if D3D11CreateDevice never returns
    }
    catch (...)
    {
        m.slot->note = "the device thread could not be created: running headless";
        m.slot->state.store(static_cast<int>(Device::Failed));
    }
}

Presenter::Device Presenter::DeviceState() const
{
    return static_cast<Device>(impl_->slot->state.load());
}

bool Presenter::Presenting() const
{
    if (impl_->guest_owns) return SUCCEEDED(impl_->guest_hr.load());   // the guest's swap chain is on the CoreWindow
    return impl_->swap && !impl_->dead && impl_->presents != 0 && SUCCEEDED(impl_->last_present_hr);
}

void Presenter::Tick(bool live)
{
    Impl& m = *impl_;
    m.TakeDevice();
    if (m.guest_request && !m.guest_owns)
    {
        if (m.context)
        {
            m.context->ClearState();
            m.context->Flush();
        }
        m.swap.Reset();
        m.guest_owns = 1;
        m.Note("a guest asked for a swap chain on the CoreWindow: the presenter's own swap chain is released");
    }
    if (m.guest_owns)
    {
        if (!m.guest_noted && m.guest_hr.load() != 1)
        {
            m.guest_noted = true;
            m.Note(Fmt("guest swap chain on the CoreWindow: CreateSwapChainForCoreWindow -> %s; its Present is on screen",
                       Hr(static_cast<HRESULT>(m.guest_hr.load())).c_str()));
        }
        return;   // the guest presents; nothing of the composed desktop is drawn over it
    }
    if (m.device && !m.dead && !m.swap && !m.swap_failed && m.window) m.CreateSwapChain();

    const ULONGLONG now = GetTickCount64();
    const bool overlay = m.source != nullptr && m.source->Active();
    const std::uint64_t serial = overlay ? m.source->Serial() : (live ? win10xbox::user::FrameSerial() : m.last_serial);
    const bool changed = (live || overlay) && serial != m.last_serial;
    const bool refresh = now - m.last_compose >= kRefreshMs;
    const bool shot_due = live && m.shot != INVALID_HANDLE_VALUE && now - m.last_shot >= kShotMs;
    if ((live || overlay) && (changed || refresh || shot_due))
    {
        m.last_serial = serial;
        m.last_compose = now;
        if (overlay) m.source->Draw(m.frame.data(), m.scr_w, m.scr_h);
        if (overlay || win10xbox::user::ComposeFrame(m.frame.data(), m.scr_w, m.scr_h, m.scr_w * 4))
        {
            if (m.composes++ == 0) m.Note(Fmt("first composed frame (%dx%d)", m.scr_w, m.scr_h));
            m.composed = true;
            m.uploaded = false;
        }
    }
    if (shot_due)
    {
        m.last_shot = now;
        if (m.composed)
        {
            if (WriteBmp(m.shot, m.frame, m.scr_w, m.scr_h))
            {
                if (m.shots++ == 0) m.Note("screen.bmp: first periodic screenshot written (every 5 s while the guest runs)");
            }
            else if (!m.shot_failed_noted)
            {
                m.shot_failed_noted = true;
                m.Note(Fmt("screen.bmp: write failed (error %lu)", GetLastError()));
            }
        }
    }
    if (m.swap && !m.dead && (!m.uploaded || now - m.last_present >= kRefreshMs)) m.Present(now);
}

void Presenter::FinalScreenshot()
{
    Impl& m = *impl_;
    if (m.shot_path.empty()) return;
    if (m.shot != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m.shot);
        m.shot = INVALID_HANDLE_VALUE;
    }
    // The guest has exited: the PEB is the host's again, so opening a path is safe.
    if (win10xbox::user::WriteScreenshot(m.shot_path))
    {
        m.Note("screen.bmp written by user::WriteScreenshot after the guest exited");
        return;
    }
    if (!m.composed)
    {
        DeleteFileW(m.shot_path.c_str());
        m.Note("no screen.bmp: nothing was ever composed (user::ComposeFrame never returned a frame)");
        return;
    }
    HANDLE h = CreateFileW(m.shot_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool ok = h != INVALID_HANDLE_VALUE && WriteBmp(h, m.frame, m.scr_w, m.scr_h);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    m.Note(ok ? "screen.bmp: user::WriteScreenshot had nothing to compose after the guest exited; wrote the last "
                "composed frame instead"
              : "screen.bmp: could not be written after the guest exited");
}

std::vector<std::string> Presenter::TakeNotes()
{
    std::vector<std::string> out;
    out.swap(impl_->notes);
    return out;
}

std::string Presenter::Describe() const
{
    const Impl& m = *impl_;
    std::string s;
    switch (DeviceState())
    {
    case Device::NotStarted: s = "not started"; break;
    case Device::Pending: s = "D3D11CreateDevice has not returned yet (device thread)"; break;
    case Device::Failed: s = "headless (no D3D11 device)"; break;
    case Device::Ready:
        if (!m.device_reported) s = "device ready";
        else if (m.dead) s = "headless (device lost or Present failing, last " + Hr(m.last_present_hr) + ")";
        else if (m.swap_failed) s = "headless (no swap chain)";
        else if (m.swap) s = "D3D11 " + m.kind + " swap chain";
        else s = "D3D11 " + m.kind + " device, no swap chain yet";
        break;
    }
    return s + Fmt("; screen %dx%d, window %dx%d px; presents=%llu composed=%llu screenshots=%llu", m.scr_w, m.scr_h,
                   m.win_w, m.win_h, m.presents, m.composes, m.shots);
}
}  // namespace w10x_uwp
