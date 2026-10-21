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
#include <memory>
#include <string>

namespace w10x_uwp
{
    struct DisplayInfo
    {
        std::uint32_t width = 1920;
        std::uint32_t height = 1080;
        std::uint32_t refresh_hz = 60;
    };

    // Facts the shell collects with WinRT on the UI thread before the worker exists.
    struct HostInfo
    {
        std::wstring local_state;        // ApplicationData LocalFolder: the only writable place
        std::wstring package_dir;        // folder of the running exe; payload\ lives below it
        std::string package_full_name;   // "win10xbox.host_1.0.0.5_x64__xhe8jecrsz3yr"; may be empty
        std::string package_version;     // "1.0.0.5"; may be empty
        DisplayInfo display;             // size of the virtual monitor handed to the winexbox backend
    };

    class Host
    {
    public:
        // Creates the winexbox display backend (D8: headless) sized from info.display.
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

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace w10x_uwp
