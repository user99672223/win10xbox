// main.cpp - headless CoreApplication view of host-uwp: the one translation unit (with
// App.cpp) that sees WinRT and talks to the Host (host.h).
//
// Same shape as host-uwp-lite/main.cpp, which is the CI-proven template (itself modelled on
// the capability probe's tools/probe/uwp-host/main.cpp, the known-good template on the console):
//   * the winexbox display backend stays the headless one (D8); what reaches the TV is the
//     frame core/user composes from it (win10xbox::user::ComposeFrame), copied into a D3D11
//     swap chain on the CoreWindow by the Presenter (presenter.h). Probe lesson, measured on the
//     console: D3D11CreateDevice HANGS on the UI thread of a Game-bucket app, so the device is
//     created on the presenter's own thread after activation has settled; the swap chain is
//     created and Presented on the UI thread, from the pump below. All of it happens before the
//     Host's worker starts (bounded by kDeviceWaitMs), so the D3D/DXGI DLLs are loaded before
//     the guest owns the PEB. Any failure leaves the app headless with a "display:" line;
//   * all heavy work (config, staging, Initialize, process::Launch, waiting for the guest)
//     runs on the Host's WORKER thread; the CoreWindow thread only pumps events;
//   * CoreWindow::Activate() is called, and the worker is started only after activation has
//     settled, so no shell-side WinRT activation work overlaps the guest's PEB take-over
//     (process::Launch swaps Peb->Ldr, ProcessParameters, the loader lock... while the guest
//     runs);
//   * the app closes itself with CoreApplication::Exit() when the worker is done.
//
// What the real host adds over the throwaway:
//   * the Host instead of the smoke: the facts it needs are collected here, with WinRT, on
//     the UI thread, before the worker exists (HostInfo);
//   * CoreWindow keyboard and pointer input goes to the winexbox backend through
//     Host::InjectInput, and the backend's queue is drained with Host::PumpDisplay;
//   * lifecycle events become lines in trace.log (Host::NoteLifecycle / NoteSuspending),
//     and closing the window asks the worker to stop (Host::RequestStop; EmergencyExit if
//     it does not react) because a guest that is still running owns the PEB and the process
//     must end without returning into WinRT code;
//   * "exit_when_done": false in launch.json keeps the app open after the guest has exited;
//   * crash capture (crash.h) is installed first thing in View::Initialize and fed from the
//     trace sink; the wiring is in host_shell.h.
//
// Threading: everything in View runs on the UI thread, except that the CoreApplication
// Suspending / Resuming handlers are written so that they are also correct on another thread
// (they only read host_ and call thread-safe Host methods).
#include "pch.h"
#include "App.h"
#include "host.h"
#include "host_diag.h"
#include "host_shell.h"
#include "host_util.h"
#include "launcher.h"
#include "presenter.h"

#include "win10xbox/user.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>

namespace wac = winrt::Windows::ApplicationModel::Core;
namespace wam = winrt::Windows::ApplicationModel;
namespace wact = winrt::Windows::ApplicationModel::Activation;

namespace
{
// CoreApplication::RequestRestartAsync(arg): ends this process and starts the app again with `arg` as the launch
// argument. Called on the host worker thread after a guest has exited. Returns only on failure, with the reason
// (AppRestartFailureReason: RestartPending, InvalidUser, Other, or the exception text); never polls the
// operation with a blocking get() on a dispatcher thread.
std::string RequestAppRestart(const std::string& arg)
{
    try
    {
        auto op = winrt::Windows::ApplicationModel::Core::CoreApplication::RequestRestartAsync(winrt::to_hstring(arg));
        for (int i = 0; i < 300 && op.Status() == winrt::Windows::Foundation::AsyncStatus::Started; ++i) Sleep(100);
        if (op.Status() == winrt::Windows::Foundation::AsyncStatus::Started) return "RequestRestartAsync did not complete within 30 s";
        if (op.Status() != winrt::Windows::Foundation::AsyncStatus::Completed)
        {
            char b[16];
            snprintf(b, sizeof b, "%08x", static_cast<unsigned>(op.ErrorCode().value));
            return std::string("RequestRestartAsync failed (HRESULT 0x") + b + ")";
        }
        switch (op.GetResults())
        {
        case wac::AppRestartFailureReason::None: return "";   // granted: the process is about to end
        case wac::AppRestartFailureReason::RestartPending: return "AppRestartFailureReason::RestartPending (a restart is already pending)";
        case wac::AppRestartFailureReason::NotInForeground: return "AppRestartFailureReason::NotInForeground";
        case wac::AppRestartFailureReason::InvalidUser: return "AppRestartFailureReason::InvalidUser";
        case wac::AppRestartFailureReason::Other: return "AppRestartFailureReason::Other";
        }
        return "RequestRestartAsync returned an unknown reason";
    }
    catch (const winrt::hresult_error& e)
    {
        return "RequestRestartAsync threw: " + winrt::to_string(e.message());
    }
    catch (...)
    {
        return "RequestRestartAsync threw";
    }
}
}  // namespace
namespace wui = winrt::Windows::UI::Core;
namespace wuin = winrt::Windows::UI::Input;
namespace wf = winrt::Windows::Foundation;

namespace w10x_uwp
{
namespace
{
    // Let activation settle (about half a second of event pumping) before the worker
    // starts, as host-uwp-lite does.
    constexpr int kSettleRounds = 10;
    constexpr DWORD kSettleMs = 50;

    // How often the UI thread wakes up to pump CoreWindow events, the winexbox backend's
    // queue and the stop checks (about 60 Hz, so input is not delayed visibly).
    constexpr DWORD kPumpMs = 16;

    // After a stop request (the window was closed) the worker gets this long to write the
    // final status and end the process (while the guest runs it does that itself within one
    // 250 ms poll) before the shell pulls the plug with Host::EmergencyExit.
    constexpr ULONGLONG kStopGraceMs = 10000;

    // How long the shell waits for the presenter's device thread (D3D11CreateDevice) before it
    // starts the Host anyway, headless; a device that turns up later is still used.
    constexpr ULONGLONG kDeviceWaitMs = 5000;

    std::wstring ExeDirectory()
    {
        wchar_t buf[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
        if (n == 0 || n >= MAX_PATH * 2) return std::wstring();
        std::wstring p(buf, n);
        const std::size_t slash = p.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
    }

    // ApplicationData LocalFolder: the only writable place (D15).
    std::wstring LocalStatePath()
    {
        try
        {
            return std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str());
        }
        catch (...)
        {
            // No package identity (unexpected on UWP): fall back like host-uwp-lite and the probe do.
            wchar_t tmp[MAX_PATH];
            const DWORD n = GetTempPathW(MAX_PATH, tmp);
            std::wstring p = n != 0 && n < MAX_PATH ? std::wstring(tmp, n) : std::wstring(L"C:\\");
            while (p.size() > 3 && p.back() == L'\\') p.pop_back();
            return p;
        }
    }

    // The CoreWindow's size in physical pixels and the view-pixel -> pixel factor: what the
    // swap chain, the virtual monitor (HostInfo.display) and ComposeFrame all use, and what the
    // pointer positions are scaled by.
    struct WindowPixels
    {
        int width = 0;
        int height = 0;
        float scale = 1.0f;
    };

    // All WinRT calls the Host needs happen here, on the UI thread, before the worker exists.
    HostInfo CollectHostInfo(wui::CoreWindow const& window, WindowPixels* px)
    {
        HostInfo info;
        info.package_dir = ExeDirectory();
        info.local_state = LocalStatePath();
        try
        {
            const wam::PackageId id = wam::Package::Current().Id();
            info.package_full_name = winrt::to_string(id.FullName());
            const wam::PackageVersion v = id.Version();
            info.package_version = Fmt("%u.%u.%u.%u", static_cast<unsigned>(v.Major), static_cast<unsigned>(v.Minor),
                                       static_cast<unsigned>(v.Build), static_cast<unsigned>(v.Revision));
        }
        catch (...)
        {
            // No package identity: both stay empty, the Host copes (it only reports them).
        }
        try
        {
            // Bounds() is in view pixels (DIPs); the Xbox scales UWP views, so the factor is
            // read rather than assumed (RawPixelsPerViewPixel, else LogicalDpi / 96).
            const winrt::Windows::Graphics::Display::DisplayInformation di =
                winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
            double f = di.RawPixelsPerViewPixel();
            if (!(f >= 0.25 && f <= 8.0)) f = di.LogicalDpi() / 96.0;
            if (f >= 0.25 && f <= 8.0) px->scale = static_cast<float>(f);
        }
        catch (...)
        {
        }
        try
        {
            // The virtual monitor of the headless backend is as large as the CoreWindow, in
            // physical pixels (the swap chain is the same size, and the pointer positions
            // forwarded below are scaled to match). The refresh rate stays the HostInfo default:
            // WinRT has no portable way to ask for it. A window that has no size yet keeps the
            // default 1920x1080.
            const wf::Rect b = window.Bounds();
            if (b.Width >= 1.0f && b.Height >= 1.0f)
            {
                px->width = static_cast<int>(b.Width * px->scale + 0.5f);
                px->height = static_cast<int>(b.Height * px->scale + 0.5f);
                info.display.width = static_cast<std::uint32_t>(px->width);
                info.display.height = static_cast<std::uint32_t>(px->height);
            }
        }
        catch (...)
        {
        }
        return info;
    }

    // The Host could not be created or started before it had a status file of its own:
    // leave the owner a verdict anyway (a stale status.txt of the previous run would lie).
    void ReportShellFailure(const std::wstring& local_state, const std::string& what)
    {
        try
        {
            diag::Report report(local_state + L"\\status.txt");
            report.Set("tool", "win10xbox host-uwp (shell)");
            report.Set("phase", "shell start-up");
            report.Set("error", what);
            report.SetVerdict("FAIL (the shell could not start the host: " + what + ")");
            report.Flush();
        }
        catch (...)
        {
        }
    }

    const char* ExecutionStateName(wact::ApplicationExecutionState s)
    {
        switch (s)
        {
        case wact::ApplicationExecutionState::NotRunning: return "NotRunning";
        case wact::ApplicationExecutionState::Running: return "Running";
        case wact::ApplicationExecutionState::Suspended: return "Suspended";
        case wact::ApplicationExecutionState::Terminated: return "Terminated";
        case wact::ApplicationExecutionState::ClosedByUser: return "ClosedByUser";
        }
        return "?";
    }

    const char* WindowActivationName(wui::CoreWindowActivationState s)
    {
        switch (s)
        {
        case wui::CoreWindowActivationState::CodeActivated: return "window activated (by code)";
        case wui::CoreWindowActivationState::Deactivated: return "window deactivated";
        case wui::CoreWindowActivationState::PointerActivated: return "window activated (by pointer)";
        }
        return "window activation changed";
    }

    // The button a pointer press / release is about; touch and pen contacts report a left
    // button update kind, and anything unknown is treated as the left button as well.
    winexbox::MouseButton ButtonOf(wuin::PointerUpdateKind k)
    {
        switch (k)
        {
        case wuin::PointerUpdateKind::RightButtonPressed:
        case wuin::PointerUpdateKind::RightButtonReleased: return winexbox::MouseButton::Right;
        case wuin::PointerUpdateKind::MiddleButtonPressed:
        case wuin::PointerUpdateKind::MiddleButtonReleased: return winexbox::MouseButton::Middle;
        case wuin::PointerUpdateKind::XButton1Pressed:
        case wuin::PointerUpdateKind::XButton1Released: return winexbox::MouseButton::X1;
        case wuin::PointerUpdateKind::XButton2Pressed:
        case wuin::PointerUpdateKind::XButton2Released: return winexbox::MouseButton::X2;
        default: return winexbox::MouseButton::Left;
        }
    }

    struct View : winrt::implements<View, wac::IFrameworkView>
    {
        // ---- IFrameworkView ----------------------------------------------------------
        void Initialize(wac::CoreApplicationView const& view)
        {
            // First thing the view does: crash capture and the guest-entry marker (host_shell.h).
            try
            {
                diagnostics_.InstallCrashCapture(LocalStatePath());
            }
            catch (...)
            {
            }
            try
            {
                view.Activated([this](auto&&, wact::IActivatedEventArgs const& args) { OnActivated(args); });
                wac::CoreApplication::Suspending([this](auto&&, auto&&) { OnSuspending(); });
                wac::CoreApplication::Resuming([this](auto&&, auto&&) { Note("resuming"); });
            }
            catch (...)
            {
            }
        }

        void Uninitialize() {}
        void Load(winrt::hstring const&) {}

        void SetWindow(wui::CoreWindow const& window)
        {
            try
            {
                window.Closed([this](auto&&, auto&&) { closed_.store(true); });
                window.VisibilityChanged([this](auto&&, wui::VisibilityChangedEventArgs const& a)
                                         { Note(a.Visible() ? "window visible" : "window hidden"); });
                window.Activated([this](auto&&, wui::WindowActivatedEventArgs const& a)
                                 { Note(WindowActivationName(a.WindowActivationState())); });
                window.KeyDown([this](auto&&, wui::KeyEventArgs const& a) { OnKey(true, a); });
                window.KeyUp([this](auto&&, wui::KeyEventArgs const& a) { OnKey(false, a); });
                window.PointerMoved([this](auto&&, wui::PointerEventArgs const& a) { OnPointerMoved(a); });
                window.PointerPressed([this](auto&&, wui::PointerEventArgs const& a) { OnPointerButton(a, true); });
                window.PointerReleased([this](auto&&, wui::PointerEventArgs const& a) { OnPointerButton(a, false); });
                window.PointerWheelChanged([this](auto&&, wui::PointerEventArgs const& a) { OnPointerWheel(a); });
            }
            catch (...)
            {
            }
        }

        void Run()
        {
            try
            {
                RunShell();
            }
            catch (const std::exception& e)
            {
                FailShell(e.what());
            }
            catch (...)
            {
                FailShell("unknown exception");
            }

            // Done: the app closes itself, which is the "finished" signal on the console
            // (the verdict is in LocalState\status.txt).
            diagnostics_.Detach();
            wac::CoreApplication::Exit();
        }

    private:
        // ---- the shell's own flow ------------------------------------------------------
        void RunShell()
        {
            wui::CoreWindow window = wui::CoreWindow::GetForCurrentThread();
            try
            {
                window.Activate();
            }
            catch (...)
            {
            }
            wui::CoreDispatcher dispatcher = window.Dispatcher();

            for (int i = 0; i < kSettleRounds; ++i)
            {
                dispatcher.ProcessEvents(wui::CoreProcessEventsOption::ProcessAllIfPresent);
                Sleep(kSettleMs);
            }

            if (StartHost(window, dispatcher)) PumpUntilDone(dispatcher);
        }

        // Builds the HostInfo, creates the Host, brings the presenter up and starts the Host's
        // worker. host_ is set only on success; a failure leaves a verdict in status.txt.
        bool StartHost(wui::CoreWindow const& window, wui::CoreDispatcher const& dispatcher)
        {
            WindowPixels px;
            const HostInfo info = CollectHostInfo(window, &px);
            pointer_scale_ = px.scale;
            try
            {
                PublishExampleConfig(info.package_dir, info.local_state);

                // Leaked on purpose: the process owns the Host until it exits (see ~Host). The
                // constructor hands the display backend to core/user; the worker is not running yet.
                Host* const host = new Host(info);
                host->AttachLauncher(&launcher_);          // launcher.h: picker / autoselect in launch.json
                host->SetRestart(restart_.run, restart_.select, [](const std::string& arg) { return RequestAppRestart(arg); });
                presenter_.SetFrameSource(&launcher_);

                StartPresenter(window, dispatcher, info, px);
                host->NoteDisplay(presenter_.Describe());

                // Before the worker exists, hence before the guest can: from process::Launch on
                // the PEB loader lists are the guest's and the report would describe its modules.
                // After the presenter, so the D3D/DXGI modules are in the list.
                diagnostics_.CaptureHostModules();

                if (!host->StartWorker()) return false;   // Host::StartWorker has written the verdict
                host_.store(host);
                return true;
            }
            catch (const std::exception& e)
            {
                ReportShellFailure(info.local_state, e.what());
            }
            catch (...)
            {
                ReportShellFailure(info.local_state, "unknown exception");
            }
            return false;
        }

        // The presenter (presenter.h), before the Host's worker exists: the device thread, a
        // bounded wait for it (pumping CoreWindow events, never blocking the UI thread on D3D),
        // then the swap chain and the first Present on this, the UI thread.
        void StartPresenter(wui::CoreWindow const& window, wui::CoreDispatcher const& dispatcher, const HostInfo& info,
                            const WindowPixels& px)
        {
            try
            {
                std::int32_t sw = 0, sh = 0;
                win10xbox::user::ScreenSize(&sw, &sh);   // the backend's monitor: info.display after the Host's clamp
                presenter_.Configure(static_cast<IUnknown*>(winrt::get_unknown(window)), px.width, px.height, sw, sh);
                presenter_.OpenScreenshot(info.local_state + L"\\screen.bmp");
                presenter_.StartDevice();
                const ULONGLONG t0 = GetTickCount64();
                while (presenter_.DeviceState() == Presenter::Device::Pending && GetTickCount64() - t0 < kDeviceWaitMs &&
                       !closed_.load())
                {
                    try
                    {
                        dispatcher.ProcessEvents(wui::CoreProcessEventsOption::ProcessAllIfPresent);
                    }
                    catch (...)
                    {
                        closed_.store(true);
                    }
                    Sleep(kPumpMs);
                }
                if (presenter_.DeviceState() == Presenter::Device::Pending)
                    Note(Fmt("display: D3D11CreateDevice has not returned after %llu ms on the device thread: starting the "
                             "guest headless; the device is used if it turns up later",
                             static_cast<unsigned long long>(GetTickCount64() - t0)));
                presenter_.Tick(true);   // swap chain + first Present, while no guest runs
                Note(Fmt("display: CoreWindow %dx%d px (scale %.2f), %s", px.width, px.height, static_cast<double>(px.scale),
                         presenter_.Describe().c_str()));
                ForwardPresenterNotes();
            }
            catch (...)
            {
                Note("display: exception while starting the presenter: headless");
            }
        }

        // The presenter's notes become "display:" lines in trace.log, its state a key in status.txt.
        void ForwardPresenterNotes()
        {
            const std::vector<std::string> notes = presenter_.TakeNotes();
            if (notes.empty()) return;
            for (const std::string& n : notes) Note("display: " + n);
            if (Host* const host = host_.load()) host->NoteDisplay(presenter_.Describe());
        }

        // Pumps until the worker has ended (and, with "exit_when_done": false, until the window
        // is closed). The pump never blocks, so the stop and watchdog logic of the Host and the
        // 250 ms polls of its worker keep their meaning.
        void PumpUntilDone(wui::CoreDispatcher const& dispatcher)
        {
            Host& host = *host_.load();
            bool stop_issued = false;
            bool final_shot = false;
            ULONGLONG stop_tick = 0;
            for (;;)
            {
                try
                {
                    dispatcher.ProcessEvents(wui::CoreProcessEventsOption::ProcessAllIfPresent);
                }
                catch (...)
                {
                    closed_.store(true);   // the window is gone: nothing is left to pump
                }
                host.PumpDisplay();
                launcher_.PollGamepad();   // Windows.Gaming.Input, for the launcher's screens
                diagnostics_.ReplayInto(host);

                // The frame: composed while the guest may change it, the last one kept on screen
                // after it has exited (also while the app stays open with "exit_when_done": false).
                const bool finished = host.Finished();
                try
                {
                    presenter_.Tick(!finished);
                    if (finished && !final_shot)
                    {
                        final_shot = true;
                        presenter_.FinalScreenshot();
                    }
                    ForwardPresenterNotes();
                }
                catch (...)
                {
                }

                if (closed_.load() && !stop_issued)
                {
                    stop_issued = true;
                    stop_tick = GetTickCount64();
                    host.RequestStop(host.GuestRunning() ? "the window was closed while the guest was running"
                                                         : "the window was closed");
                }
                if (host.Finished())
                {
                    if (host.ExitWhenDone() || closed_.load()) break;
                }
                else if (stop_issued && GetTickCount64() - stop_tick > kStopGraceMs)
                {
                    host.EmergencyExit("the host worker did not end within 10 s of the stop request");   // does not return
                }
                Sleep(kPumpMs);
            }
            host.JoinWorker();
        }

        // An exception escaped the shell. While the guest runs it owns the PEB, so returning
        // into WinRT code is not an option: write the status and end the process instead.
        void FailShell(const char* what)
        {
            Host* const host = host_.load();
            if (host != nullptr && !host->Finished())
                host->EmergencyExit((std::string("exception in the shell: ") + what).c_str());   // does not return
            ReportShellFailure(LocalStatePath(), std::string("exception in the shell: ") + what);
        }

        // ---- lifecycle -------------------------------------------------------------------
        void Note(std::string const& text)
        {
            diagnostics_.Note(host_.load(), text);
        }

        void OnActivated(wact::IActivatedEventArgs const& args)
        {
            try
            {
                // The app's own restart (host.h SetRestart) comes back as the launch argument. Activation runs on
                // the UI thread before StartHost (RunShell pumps the settle rounds first).
                if (args.Kind() == wact::ActivationKind::Launch && !restart_.valid)
                {
                    const std::string a = winrt::to_string(args.as<wact::ILaunchActivatedEventArgs>().Arguments());
                    const RestartArgs ra = ParseRestartArgs(a);
                    if (ra.valid) restart_ = ra;
                    Note(Fmt("launch arguments: \"%s\"%s", a.c_str(), ra.valid ? " (restart)" : ""));
                }
                Note(Fmt("activated: kind=%d, previous state %s", static_cast<int>(args.Kind()),
                         ExecutionStateName(args.PreviousExecutionState())));
            }
            catch (...)
            {
            }
        }

        // A suspended app may be terminated unannounced: get the trace and the guest's output to disk.
        void OnSuspending()
        {
            try
            {
                if (Host* const host = host_.load()) host->NoteSuspending();
            }
            catch (...)
            {
            }
        }

        // ---- input: CoreWindow -> winexbox backend (UI thread) ------------------------------
        // VirtualKey values are the Win32 VK_ codes (gamepad buttons arrive as the
        // VK_GAMEPAD_* range), the scan code and the extended flag come from the key status.
        void OnKey(bool down, wui::KeyEventArgs const& args)
        {
            // The launcher's list / result screen is up: the keys are for it, not for a guest.
            try
            {
                if (launcher_.Consumes())
                {
                    if (down) launcher_.OnKey(static_cast<unsigned>(static_cast<std::int32_t>(args.VirtualKey())));
                    return;
                }
            }
            catch (...)
            {
            }
            Host* const host = host_.load();
            if (host == nullptr || !host->InputPumpEnabled()) return;
            try
            {
                winexbox::InputEvent ev;
                ev.kind = down ? winexbox::InputKind::KeyDown : winexbox::InputKind::KeyUp;
                ev.vk = static_cast<std::uint16_t>(static_cast<std::int32_t>(args.VirtualKey()));
                const wui::CorePhysicalKeyStatus status = args.KeyStatus();
                ev.scan = static_cast<std::uint16_t>(status.ScanCode);
                if (status.IsExtendedKey) ev.flags |= winexbox::kInputExtendedKey;
                host->InjectInput(ev);
            }
            catch (...)
            {
            }
        }

        void OnPointerMoved(wui::PointerEventArgs const& args)
        {
            Host* const host = host_.load();
            if (host == nullptr || !host->InputPumpEnabled()) return;
            try
            {
                const wf::Point pos = args.CurrentPoint().Position();
                winexbox::InputEvent ev;
                ev.kind = winexbox::InputKind::MouseMove;
                ev.x = static_cast<std::int32_t>(pos.X * pointer_scale_);
                ev.y = static_cast<std::int32_t>(pos.Y * pointer_scale_);
                host->InjectInput(ev);
            }
            catch (...)
            {
            }
        }

        void OnPointerButton(wui::PointerEventArgs const& args, bool down)
        {
            Host* const host = host_.load();
            if (host == nullptr || !host->InputPumpEnabled()) return;
            try
            {
                const wuin::PointerPoint point = args.CurrentPoint();
                const wf::Point pos = point.Position();
                winexbox::InputEvent ev;
                ev.kind = down ? winexbox::InputKind::MouseButtonDown : winexbox::InputKind::MouseButtonUp;
                ev.button = ButtonOf(point.Properties().PointerUpdateKind());
                ev.x = static_cast<std::int32_t>(pos.X * pointer_scale_);
                ev.y = static_cast<std::int32_t>(pos.Y * pointer_scale_);
                host->InjectInput(ev);
            }
            catch (...)
            {
            }
        }

        // MouseWheelDelta is in WHEEL_DELTA (120) units, like InputEvent::wheel_delta.
        void OnPointerWheel(wui::PointerEventArgs const& args)
        {
            Host* const host = host_.load();
            if (host == nullptr || !host->InputPumpEnabled()) return;
            try
            {
                const wuin::PointerPoint point = args.CurrentPoint();
                const wuin::PointerPointProperties props = point.Properties();
                const wf::Point pos = point.Position();
                winexbox::InputEvent ev;
                ev.kind = winexbox::InputKind::MouseWheel;
                ev.wheel_delta = props.MouseWheelDelta();
                if (props.IsHorizontalMouseWheel()) ev.flags |= winexbox::kInputHorizontal;
                ev.x = static_cast<std::int32_t>(pos.X * pointer_scale_);
                ev.y = static_cast<std::int32_t>(pos.Y * pointer_scale_);
                host->InjectInput(ev);
            }
            catch (...)
            {
            }
        }

        ShellDiagnostics diagnostics_;
        Presenter presenter_;          // UI thread only (its device thread is internal)
        RestartArgs restart_;          // the restart argument of this activation (launch_config.h), valid == false on a normal start
        Launcher launcher_;            // launcher.h: screens drawn by presenter_, input from the pump and OnKey
        float pointer_scale_ = 1.0f;   // view pixels -> physical pixels (UI thread)
        std::atomic<Host*> host_{nullptr};
        std::atomic<bool> closed_{false};
    };
}  // namespace

wac::IFrameworkView MakeView()
{
    return winrt::make<View>();
}
}  // namespace w10x_uwp
