// host.h - the host-uwp shell logic: everything between "the CoreWindow is up"
// and "the guest has exited", driven only through the public win10xbox API
// (core.h, config.h, dispatch.h, trace.h, vfs.h) and the winexbox backend
// interface (winexbox/driver.h). Plain Win32 + STL, no WinRT: main.cpp is the
// only translation unit that sees WinRT and talks to this class.
//
// Threads
//   UI thread (the CoreWindow thread): constructs the Host, starts the worker,
//     pumps CoreWindow events, forwards input (InjectInput/PumpDisplay) and
//     calls the Note*/Request* methods from the lifecycle events.
//   worker thread (started after CoreWindow activation, DECISIONS.md "UWP host
//     (M5) lessons"): config, staging, win10xbox::Initialize, process::Launch,
//     waiting for the guest, the final status. All heavy work lives here.
#pragma once

#include "winexbox/driver.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace w10x_uwp
{
    class Launcher;

    // The display mode of this run: what the console (or, on the runner, DisplayInformation / the CoreWindow) provides
    // after launch.json "display_mode" has been applied. It is the one mode the guest sees (winexbox backend, core/user,
    // DXGI output) and the size of the presenter's swap chain.
    struct DisplayInfo
    {
        std::uint32_t width = 1920;
        std::uint32_t height = 1080;
        std::uint32_t refresh_hz = 60;
        bool hdr = false;                 // HDR10 (BT.2020) is the console's active colour space
        unsigned supported = 0;           // modes in HdmiDisplayInformation::GetSupportedDisplayModes (0 off the console)
        std::string source = "default";   // hdmi | displayinformation | corewindow | default
        // The guest's screen (launch.json "guest_display"; a program's own .display is applied at its launch): what core/user and
        // DXGI report and the size of the composed frame. "auto" = the output mode above.
        std::uint32_t guest_width = 1920;
        std::uint32_t guest_height = 1080;
        std::uint32_t guest_refresh_hz = 60;
        std::string guest_requested = "auto";
        std::string output_scaling = "system";   // launch.json "output_scaling" as asked; the presenter's outcome goes through NoteOutputScaling
        std::string requested = "auto";   // launch.json "display_mode"
        std::string result = "kept";      // what the request came to
        std::vector<std::string> notes;   // lines for trace.log ("display: ..."), supported list included
    };

    inline std::string DescribeGuestDisplay(const DisplayInfo& d)
    {
        return std::to_string(d.guest_width) + "x" + std::to_string(d.guest_height) + "@" + std::to_string(d.guest_refresh_hz) +
               " requested=" + d.guest_requested;
    }

    // "display=2560x1440@120 hdr=0 supported=12 requested=2560x1440@120 result=set": status.txt and the report header.
    inline std::string DescribeDisplay(const DisplayInfo& d)
    {
        return std::to_string(d.width) + "x" + std::to_string(d.height) + "@" + std::to_string(d.refresh_hz) +
               " hdr=" + (d.hdr ? "1" : "0") + " supported=" + std::to_string(d.supported) + " requested=" + d.requested +
               " result=" + d.result;
    }

    // Facts the shell collects with WinRT on the UI thread before the worker exists.
    struct HostInfo
    {
        std::wstring local_state;        // ApplicationData LocalFolder: the only writable place
        std::wstring package_dir;        // folder of the running exe; payload\ lives below it
        std::string package_full_name;   // "win10xbox.host_1.0.0.5_x64__xhe8jecrsz3yr"; may be empty
        std::string package_version;     // "1.0.0.5"; may be empty
        DisplayInfo display;             // size of the virtual monitor handed to the winexbox backend, in
                                         // physical pixels (the swap chain and ComposeFrame use the same)
    };

    // launch.json "display_mode" ("auto" or "WxH@Hz") as the Host will read it later (LocalState first, then the packaged
    // default); "auto" when there is none or it does not parse (the Host reports that error itself). UI thread, start-up.
    void ReadDisplaySettings(const std::wstring& local_state, const std::wstring& package_dir, std::string* display_mode,
                             std::string* guest_display, std::string* scaling);

    class Host
    {
    public:
        // Creates the winexbox display backend (D8: headless) sized from info.display and hands
        // it to core/user (win10xbox::user::SetDisplayDriver), whose composed frame the shell's
        // presenter puts on the screen.
        explicit Host(const HostInfo& info);
        ~Host();
        Host(const Host&) = delete;
        Host& operator=(const Host&) = delete;

        // ---- worker -------------------------------------------------------
        bool StartWorker();             // false if the thread cannot be created
        bool Finished() const;          // the worker has ended (guest exited, or failed to start)
        bool ExitWhenDone() const;      // launch.json "exit_when_done" (valid once Finished())
        bool GuestRunning() const;
        void JoinWorker();
        // Asks the worker to end the run. While the guest runs this ends the whole
        // process after the final status is written (the guest owns the PEB, so
        // returning into WinRT code would not be safe).
        void RequestStop(const char* reason);
        bool StopRequested() const;
        // Last resort when the worker does not react: write the status, flush, TerminateProcess.
        [[noreturn]] void EmergencyExit(const char* reason);

        // The on-console launcher (launcher.h). Optional; set by the shell before StartWorker().
        // Without it launch.json "picker" / "autoselect" are ignored.
        void AttachLauncher(Launcher* launcher);

        // Restart-per-guest (docs/HANDOFF.md "Launcher"): core allows one guest per process, so after a guest
        // has run the worker asks the shell to restart the app with an argument. `run` and `select` come from
        // the restart argument of this activation (0 and "" on a normal start). `fn(arg)` is called on the
        // worker thread, calls CoreApplication::RequestRestartAsync(arg) and returns only on failure, with the
        // reason (on success the process ends). Set before StartWorker().
        using RestartFn = std::function<std::string(const std::string& arg)>;
        // `all_k` is the "Run every program" chain position of the argument (-1 = no such chain), `restarted` says
        // that this activation carried a valid restart argument (the launcher then shows even if launch.json
        // does not ask for it).
        void SetRestart(bool restarted, int run, const std::string& select, int all_k, RestartFn fn);

        // ---- display / input backend (UI thread) --------------------------
        // The winexbox backend. M5 only creates it and counts the input that reaches it;
        // the win32u binding (winexbox.drv, deferred) will take over its event sink.
        winexbox::IWinexboxDriver& Display();
        bool InputPumpEnabled() const;
        void InjectInput(const winexbox::InputEvent& ev);   // CoreWindow keyboard/pointer, UI thread
        void PumpDisplay();                                 // drains the backend's event queue, UI thread

        // ---- shell lifecycle notes (UI thread) -----------------------------
        void NoteLifecycle(const char* what);   // one line in trace.log
        void NoteSuspending();                  // trace line + flush: a suspended app may be terminated unannounced
        // The presenter's state (presenter.h) as "display.presenter" in status.txt. Any thread
        // (status.txt is rewritten through its held handle while the guest runs).
        void NoteDisplay(const std::string& presenter_state);
        // "system" or "layer (system refused: 0x...)": the outcome of launch.json "output_scaling" (status.txt, report header).
        void NoteOutputScaling(const std::string& state);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace w10x_uwp
