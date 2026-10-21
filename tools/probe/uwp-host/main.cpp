// uwp-host/main.cpp - headless UWP host for the capability probe.
//
// We dropped the D3D11/D2D/DWrite scrolling renderer from the Xbox-targeted
// build: creating a hardware D3D11 device from a sandboxed UWP on Xbox Game
// partition hangs on the UI thread, which prevents the probe worker from ever
// being spawned (the thread is started inside View::Run after SetWindow's D3D
// init). The data we are after is the JSON report in LocalState, not the
// on-screen output - the diff tool and the on-screen tally were conveniences,
// and the JSON is what's actually load-bearing.
//
// Flow:
//   SetWindow()  -> immediately spawn the probe worker.
//   Run()        -> pump CoreWindow events; exit as soon as the worker finishes
//                   and the report is flushed. The window stays whatever color
//                   the shell paints; we never allocate a swapchain.
//
// The Xbox launcher activates XboxProbe.App (declared in App.idl + App.cpp);
// that activation calls make_view() below to get this IFrameworkView.
#include "pch.h"
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.Foundation.h>

#include <windows.h>
#include <atomic>
#include <string>
#include <thread>

#include "probe.h"

namespace winrt {
    using namespace Windows::ApplicationModel::Core;
    using namespace Windows::ApplicationModel;
    using namespace Windows::Storage;
    using namespace Windows::UI::Core;
    using namespace Windows::Foundation;
}

// Set true by the worker thread when probe::run_all + the final flush finish.
static std::atomic<bool> g_probe_done{false};

static std::wstring report_path() {
    try {
        return std::wstring(winrt::ApplicationData::Current().LocalFolder().Path().c_str()) +
               L"\\probe-report.json";
    } catch (...) {
        // No package identity (unexpected on UWP, but safe fallback).
        wchar_t buf[MAX_PATH];
        DWORD n = GetTempPathW(MAX_PATH, buf);
        std::wstring p = (n ? std::wstring(buf, n) : std::wstring(L"C:\\"));
        return p + L"probe-report.json";
    }
}

static void run_probe_thread() {
    // Make the LocalState path visible to category A's out-of-package DLL copy.
    try {
        auto path = winrt::ApplicationData::Current().LocalFolder().Path();
        SetEnvironmentVariableW(L"PROBE_LOCALSTATE", path.c_str());
    } catch (...) {}

    probe::Reporter reporter;
    auto env = probe::capture_env("uwp");
    if (env.environment == "uwp-unlabeled" || env.environment.empty())
        env.environment = "xbox-uwp";  // auto-labels when run on Xbox
    reporter.set_env(env);

    std::wstring json = report_path();
    reporter.set_json_path(json);

    reporter.set_on_emit([](const probe::Record& r) {
        // Mirror each record to the debugger - visible via Device Portal's
        // debug console (/debug/etw/ or VS debugger) even without a window.
        std::string line = std::string(probe::status_name(r.status)) + " " +
                           r.category + "/" + r.name;
        if (!r.detail.empty()) line += "  " + r.detail;
        line += "\n";
        OutputDebugStringA(line.c_str());
    });

    probe::run_all(reporter);
    reporter.flush();

    char posturl[1024];
    DWORD pn = GetEnvironmentVariableA("PROBE_POST", posturl, sizeof posturl);
    if (pn && pn < sizeof posturl)
        probe::http_post_report(std::string(posturl, pn), reporter.to_json());

    g_probe_done.store(true);
}

// --- CoreApplication view & source (headless) --------------------------------

namespace probe_host {

struct View : winrt::implements<View, winrt::IFrameworkView> {
    void Initialize(const winrt::CoreApplicationView&) {}
    void Uninitialize() {}
    void Load(const winrt::hstring&) {}

    void SetWindow(const winrt::CoreWindow& w) {
        window_ = w;
        // Spawn the probe worker BEFORE anything that could block the UI
        // thread; D3D init / adapter enumeration on Xbox Game can hang here.
        probe_thread_ = std::thread(run_probe_thread);
    }

    void Run() {
        auto dispatcher = winrt::CoreWindow::GetForCurrentThread().Dispatcher();
        // Pump events; exit as soon as the probe worker finishes and the JSON
        // is flushed to LocalState. We also hard-exit after 180s as a safety
        // net - no probe should take anywhere near that long.
        DWORD start = GetTickCount();
        while (!g_probe_done.load()) {
            dispatcher.ProcessEvents(winrt::CoreProcessEventsOption::ProcessAllIfPresent);
            Sleep(50);
            if (GetTickCount() - start > 180000) break;
        }
        if (probe_thread_.joinable()) probe_thread_.join();
        // Done - exit cleanly so Device Portal can pull the report and users
        // can see on-screen "app closed itself" as the "finished" signal.
        winrt::CoreApplication::Exit();
    }

    winrt::CoreWindow window_{nullptr};
    std::thread probe_thread_;
};

winrt::Windows::ApplicationModel::Core::IFrameworkView make_view() {
    return winrt::make<View>();
}

}  // namespace probe_host
