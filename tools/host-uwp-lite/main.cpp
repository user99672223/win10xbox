// main.cpp - headless CoreApplication view for host-uwp-lite.
//
// Same shape as the capability probe's tools/probe/uwp-host/main.cpp, which is
// the known-good template on the console:
//   * no renderer, no swap chain, no D3D (probe lesson: D3D11CreateDevice blocks
//     the UI thread of a Game-bucket app);
//   * all heavy work (staging tens of MB, the whole M1/M2 smoke) runs on a WORKER
//     thread, the CoreWindow thread only pumps events;
//   * the app closes itself with CoreApplication::Exit() when the worker is done.
// Two deliberate differences from the probe:
//   * CoreWindow::Activate() is called (the probe never does): the standard way to
//     tell the shell the view is up and to dismiss the splash screen, before any
//     activation timeout could fire while the worker is busy staging;
//   * the worker is started only after activation has settled, so no shell-side
//     WinRT activation work overlaps the guest's PEB take-over (process::Launch
//     swaps Peb->Ldr, ProcessParameters, the loader lock... while the guest runs;
//     that is why the real host-uwp is M5 and this one is a throwaway).
//
// The smoke itself is in smoke.cpp and has no WinRT dependency.
#include "pch.h"
#include "App.h"
#include "smoke.h"

#include <atomic>
#include <string>
#include <thread>
#include <utility>

namespace wac = winrt::Windows::ApplicationModel::Core;
namespace wui = winrt::Windows::UI::Core;

namespace uwp_lite
{
namespace
{
    // Set by the worker as its very last action.
    std::atomic<bool> g_done{false};

    std::wstring ExeDirectory()
    {
        wchar_t buf[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
        if (n == 0 || n >= MAX_PATH * 2) return std::wstring();
        std::wstring p(buf, n);
        const std::size_t slash = p.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
    }

    // All WinRT calls happen here, on the UI thread, before the worker exists.
    HostInfo CollectHostInfo()
    {
        HostInfo info;
        info.package_dir = ExeDirectory();
        try
        {
            info.local_state = std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str());
        }
        catch (...)
        {
            // No package identity (unexpected on UWP): fall back like the probe does.
            wchar_t tmp[MAX_PATH];
            const DWORD n = GetTempPathW(MAX_PATH, tmp);
            info.local_state = n != 0 && n < MAX_PATH ? std::wstring(tmp, n) : std::wstring(L"C:\\");
            while (info.local_state.size() > 3 && info.local_state.back() == L'\\') info.local_state.pop_back();
        }
        try
        {
            info.package_full_name = winrt::to_string(winrt::Windows::ApplicationModel::Package::Current().Id().FullName());
        }
        catch (...)
        {
        }
        return info;
    }

    void WorkerMain(HostInfo info)
    {
        try
        {
            RunSmoke(info);
        }
        catch (...)
        {
        }
        g_done.store(true);
    }

    struct View : winrt::implements<View, wac::IFrameworkView>
    {
        void Initialize(wac::CoreApplicationView const&) {}
        void Uninitialize() {}
        void Load(winrt::hstring const&) {}
        void SetWindow(wui::CoreWindow const&) {}

        void Run()
        {
            auto window = wui::CoreWindow::GetForCurrentThread();
            try
            {
                window.Activate();
            }
            catch (...)
            {
            }
            auto dispatcher = window.Dispatcher();

            // Let activation settle (about half a second of event pumping) before the
            // worker starts; staging takes a while anyway before the PEB is touched.
            for (int i = 0; i < 10; ++i)
            {
                dispatcher.ProcessEvents(wui::CoreProcessEventsOption::ProcessAllIfPresent);
                Sleep(50);
            }

            worker_ = std::thread(WorkerMain, CollectHostInfo());

            // Pump events until the smoke has written its verdict. A hung guest is
            // handled by the watchdog inside RunSmoke (it ends the process itself).
            while (!g_done.load())
            {
                dispatcher.ProcessEvents(wui::CoreProcessEventsOption::ProcessAllIfPresent);
                Sleep(100);
            }
            if (worker_.joinable()) worker_.join();

            // Done: the app closes itself, which is the "finished" signal on the
            // console (the verdict is in LocalState\result.txt).
            wac::CoreApplication::Exit();
        }

        std::thread worker_;
    };
}  // namespace

wac::IFrameworkView MakeView()
{
    return winrt::make<View>();
}
}  // namespace uwp_lite
