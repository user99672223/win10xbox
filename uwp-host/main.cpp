// uwp-host/main.cpp - the C++/WinRT host.
#include "pch.h"
//
// Runs as a CoreApplication-based UWP, which gives us:
//  - a native CoreWindow (works identically on desktop UWP and Xbox Series S
//    Dev Mode - the whole point of this environment);
//  - access to ApplicationData::Current()->LocalFolder for the JSON report
//    (retrievable from the Xbox over Device Portal at :11443);
//  - the AppContainer sandbox we are actually trying to measure.
//
// Rendering is Direct2D + DirectWrite on a swapchain bound to the CoreWindow;
// that is intentionally close to what the eventual compat layer will do, and
// gives us a scrolling on-screen record list without a console. The probe core
// runs on a worker thread so the UI thread stays responsive; each emitted
// record is handed back through a lock-protected buffer.
//
// Build is driven by the parent CMake and finished by package.cmd in the same
// directory, which copies the exe + payload DLL + manifest + assets into
// AppX\, builds the PRI, calls makeappx, and signs with a dev cert.
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>

#include <windows.h>
#include <d3d11_1.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "probe.h"

namespace winrt {
    using namespace Windows::ApplicationModel::Core;
    using namespace Windows::ApplicationModel;
    using namespace Windows::Storage;
    using namespace Windows::UI::Core;
    using namespace Windows::UI::ViewManagement;
    using namespace Windows::System;
    using namespace Windows::Foundation;
}
using Microsoft::WRL::ComPtr;

// --- shared state between probe thread and UI thread ------------------------
struct SharedUi {
    std::mutex mu;
    std::deque<std::string> lines;   // newest at back
    size_t scroll_offset = 0;        // 0 = pin to newest
    std::atomic<int> ok{0}, denied{0}, missing{0}, notimpl{0}, crash{0}, other{0};
    std::atomic<bool> done{false};
};

static SharedUi g_ui;

static std::wstring report_path() {
    try {
        auto folder = winrt::ApplicationData::Current().LocalFolder();
        std::wstring p = folder.Path().c_str();
        return p + L"\\probe-report.json";
    } catch (...) {
        // Not in a package: fall back to the exe directory.
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring p(exe);
        size_t s = p.find_last_of(L"\\/");
        return (s == std::wstring::npos ? std::wstring(L".") : p.substr(0, s)) +
               L"\\probe-report.json";
    }
}

static std::string char_status(probe::Status s) {
    switch (s) {
        case probe::Status::OK:      return "OK ";
        case probe::Status::DENIED:  return "DEN";
        case probe::Status::NOTIMPL: return "NIM";
        case probe::Status::MISSING: return "MIS";
        case probe::Status::CRASH:   return "CRS";
        case probe::Status::INFO:    return "INF";
        case probe::Status::WARN:    return "WRN";
        case probe::Status::SKIPPED: return "SKP";
    }
    return "???";
}

static void bump_counter(probe::Status s) {
    switch (s) {
        case probe::Status::OK:      g_ui.ok++;      break;
        case probe::Status::DENIED:  g_ui.denied++;  break;
        case probe::Status::NOTIMPL: g_ui.notimpl++; break;
        case probe::Status::MISSING: g_ui.missing++; break;
        case probe::Status::CRASH:   g_ui.crash++;   break;
        default:                     g_ui.other++;   break;
    }
}

// --- the probe worker -------------------------------------------------------
static void run_probe_thread() {
    // Expose LocalState to module A for its out-of-package DLL copy test.
    try {
        auto path = winrt::ApplicationData::Current().LocalFolder().Path();
        SetEnvironmentVariableW(L"PROBE_LOCALSTATE", path.c_str());
    } catch (...) {}

    probe::Reporter reporter;
    auto env = probe::capture_env("uwp");
    if (env.environment == "uwp-unlabeled" || env.environment.empty())
        env.environment = "laptop-uwp";  // overridable via PROBE_ENVIRONMENT
    reporter.set_env(env);

    std::wstring json = report_path();
    reporter.set_json_path(json);

    reporter.set_on_emit([](const probe::Record& r) {
        bump_counter(r.status);
        std::string line = char_status(r.status) + " " + r.category + "/" + r.name;
        if (!r.detail.empty()) line += "  " + r.detail;
        OutputDebugStringA((line + "\n").c_str());
        {
            std::lock_guard<std::mutex> lk(g_ui.mu);
            g_ui.lines.push_back(line);
            if (g_ui.lines.size() > 4096) g_ui.lines.pop_front();
        }
    });

    probe::run_all(reporter);
    reporter.flush();

    char posturl[1024];
    DWORD pn = GetEnvironmentVariableA("PROBE_POST", posturl, sizeof posturl);
    if (pn && pn < sizeof posturl)
        probe::http_post_report(std::string(posturl, pn), reporter.to_json());

    g_ui.done.store(true);
}

// --- Direct2D renderer ------------------------------------------------------
class Renderer {
public:
    void create(const winrt::CoreWindow& win) {
        // D3D11 device for the swapchain.
        D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                    D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL got;
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, _countof(want),
            D3D11_SDK_VERSION, dev.GetAddressOf(), &got, ctx.GetAddressOf());
        if (FAILED(hr)) {
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, _countof(want),
                              D3D11_SDK_VERSION, dev.GetAddressOf(), &got,
                              ctx.GetAddressOf());
        }
        dev.As(&d3d_);
        ctx.As(&d3dctx_);

        // DXGI swapchain for the CoreWindow.
        ComPtr<IDXGIDevice1> dxdev;
        d3d_.As(&dxdev);
        ComPtr<IDXGIAdapter> adapter;
        dxdev->GetAdapter(&adapter);
        ComPtr<IDXGIFactory2> factory;
        adapter->GetParent(IID_PPV_ARGS(&factory));

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 0; sd.Height = 0;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.Stereo = FALSE;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        factory->CreateSwapChainForCoreWindow(
            d3d_.Get(), winrt::get_unknown(win), &sd, nullptr, &swap_);

        // D2D factory + device + context on top of the D3D device.
        D2D1_FACTORY_OPTIONS fo{};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                          __uuidof(ID2D1Factory1), &fo, (void**)&d2d_);
        d2d_->CreateDevice(dxdev.Get(), &d2ddev_);
        d2ddev_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dctx_);

        // DWrite factory + text format for the record lines.
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                            __uuidof(IDWriteFactory1), (IUnknown**)&dw_);
        dw_->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              16.0f, L"en-US", &fmt_);
        fmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        fmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        dw_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              20.0f, L"en-US", &header_fmt_);

        attach_backbuffer();
    }

    void attach_backbuffer() {
        bitmap_.Reset();
        ComPtr<IDXGISurface> surf;
        swap_->GetBuffer(0, IID_PPV_ARGS(&surf));
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_IGNORE));
        d2dctx_->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &bitmap_);
        d2dctx_->SetTarget(bitmap_.Get());
    }

    void draw(const std::vector<std::string>& snapshot, size_t scroll_offset) {
        if (!d2dctx_) return;
        d2dctx_->BeginDraw();
        d2dctx_->Clear(D2D1::ColorF(0x101418));

        ComPtr<ID2D1SolidColorBrush> white, green, yellow, red, grey, orange;
        d2dctx_->CreateSolidColorBrush(D2D1::ColorF(0xECEFF4), &white);
        d2dctx_->CreateSolidColorBrush(D2D1::ColorF(0x66D47A), &green);
        d2dctx_->CreateSolidColorBrush(D2D1::ColorF(0xF0C14A), &yellow);
        d2dctx_->CreateSolidColorBrush(D2D1::ColorF(0xE26E6E), &red);
        d2dctx_->CreateSolidColorBrush(D2D1::ColorF(0x8A94A6), &grey);
        d2dctx_->CreateSolidColorBrush(D2D1::ColorF(0xE69A5A), &orange);

        D2D1_SIZE_F sz = d2dctx_->GetSize();
        const float pad = 16.0f;

        // Header: counts.
        wchar_t head[256];
        swprintf_s(head, L"Xbox Dev Mode capability probe   OK=%d  DEN=%d  MIS=%d  NIM=%d  CRS=%d  OTH=%d%s",
                   g_ui.ok.load(), g_ui.denied.load(), g_ui.missing.load(),
                   g_ui.notimpl.load(), g_ui.crash.load(), g_ui.other.load(),
                   g_ui.done.load() ? L"   [done]" : L"   [running]");
        D2D1_RECT_F head_r{pad, pad, sz.width - pad, pad + 32};
        d2dctx_->DrawTextW(head, (UINT32)wcslen(head), header_fmt_.Get(), head_r,
                           white.Get());

        // Record lines - 18px tall. Pin to bottom unless scroll_offset > 0.
        const float line_h = 18.0f;
        float y_bottom = sz.height - pad;
        size_t n = snapshot.size();
        size_t visible = (size_t)((y_bottom - (pad + 44)) / line_h);
        size_t start = (n > visible + scroll_offset) ? n - visible - scroll_offset : 0;
        float y = y_bottom - std::min<size_t>(n - start, visible) * line_h;

        for (size_t i = start; i < n && y + line_h < y_bottom; ++i) {
            const std::string& s = snapshot[i];
            ID2D1SolidColorBrush* b = white.Get();
            if (s.rfind("OK ", 0) == 0) b = green.Get();
            else if (s.rfind("DEN", 0) == 0) b = red.Get();
            else if (s.rfind("MIS", 0) == 0) b = orange.Get();
            else if (s.rfind("NIM", 0) == 0) b = yellow.Get();
            else if (s.rfind("CRS", 0) == 0) b = red.Get();
            else if (s.rfind("INF", 0) == 0 || s.rfind("WRN", 0) == 0 ||
                     s.rfind("SKP", 0) == 0) b = grey.Get();
            // Narrow to wide for DWrite.
            std::wstring w(s.begin(), s.end());
            D2D1_RECT_F r{pad, y, sz.width - pad, y + line_h};
            d2dctx_->DrawTextW(w.c_str(), (UINT32)w.size(), fmt_.Get(), r, b);
            y += line_h;
        }

        HRESULT hr = d2dctx_->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            attach_backbuffer();
            return;
        }
        DXGI_PRESENT_PARAMETERS pp{};
        swap_->Present1(1, 0, &pp);
    }

    void resize() {
        if (!swap_) return;
        d2dctx_->SetTarget(nullptr);
        bitmap_.Reset();
        swap_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
        attach_backbuffer();
    }

private:
    ComPtr<ID3D11Device1>        d3d_;
    ComPtr<ID3D11DeviceContext1> d3dctx_;
    ComPtr<IDXGISwapChain1>      swap_;
    ComPtr<ID2D1Factory1>        d2d_;
    ComPtr<ID2D1Device>          d2ddev_;
    ComPtr<ID2D1DeviceContext>   d2dctx_;
    ComPtr<ID2D1Bitmap1>         bitmap_;
    ComPtr<IDWriteFactory1>      dw_;
    ComPtr<IDWriteTextFormat>    fmt_;
    ComPtr<IDWriteTextFormat>    header_fmt_;
};

// --- CoreApplication view & source ------------------------------------------
struct View : winrt::implements<View, winrt::IFrameworkView> {
    void Initialize(const winrt::CoreApplicationView&) {}
    void Uninitialize() {}
    void Load(const winrt::hstring&) {}
    void SetWindow(const winrt::CoreWindow& w) {
        window_ = w;
        renderer_.create(w);
        w.SizeChanged([this](auto&&, auto&&) { renderer_.resize(); });
        w.KeyDown([](auto&& /*w*/, const winrt::KeyEventArgs& a) {
            auto key = (int)a.VirtualKey();
            std::lock_guard<std::mutex> lk(g_ui.mu);
            // VK_PRIOR=0x21 PageUp, VK_NEXT=0x22 PageDown, VK_HOME=0x24, VK_END=0x23
            if (key == 0x21 || key == 0x26 /*up*/) g_ui.scroll_offset += 10;
            else if (key == 0x22 || key == 0x28 /*down*/) {
                g_ui.scroll_offset = (g_ui.scroll_offset > 10) ? g_ui.scroll_offset - 10 : 0;
            } else if (key == 0x23 /*End*/) g_ui.scroll_offset = 0;
            else if (key == 0x24 /*Home*/) g_ui.scroll_offset = (size_t)-1;
        });
    }
    void Run() {
        auto dispatcher = winrt::CoreWindow::GetForCurrentThread().Dispatcher();
        probe_thread_ = std::thread(run_probe_thread);
        // Pump + render loop.
        while (true) {
            dispatcher.ProcessEvents(winrt::CoreProcessEventsOption::ProcessAllIfPresent);
            std::vector<std::string> snapshot;
            size_t off;
            {
                std::lock_guard<std::mutex> lk(g_ui.mu);
                snapshot.assign(g_ui.lines.begin(), g_ui.lines.end());
                off = g_ui.scroll_offset;
            }
            renderer_.draw(snapshot, off);
            // No Sleep - swapchain Present1 with vsync gates us.
        }
    }

    winrt::CoreWindow window_{nullptr};
    Renderer renderer_;
    std::thread probe_thread_;
};

// Entry point lives in XboxProbe::App (App.cpp/App.h), which the UWP toolchain
// activates via WinRT metadata. App::CreateView() calls make_view() below.
namespace probe_host {
    winrt::Windows::ApplicationModel::Core::IFrameworkView make_view() {
        return winrt::make<View>();
    }
}
