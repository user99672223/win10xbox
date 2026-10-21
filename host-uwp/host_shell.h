// host_shell.h - the plain-Win32 half of the shell (main.cpp is the WinRT half): the crash
// capture of crash.h wired into the host from the shell side, and the first-run files.
// Plain Win32 + STL + the win10xbox public API, no WinRT, so it is compiled without pch.h.
//
// Why this exists. host-uwp-lite's smoke harness owned both the worker flow and the crash
// capture, so it could (1) call InstallCrashCapture first thing, (2) feed CrashOnTraceLine
// from its own trace sink, (3) describe the host modules before the guest launches and
// (4) detach the trace.log handle before closing it. In host-uwp the worker flow is host.cpp,
// which has its own evidence trail (host_diag.h: trace.log, status.txt and a crash filter)
// and does not know crash.h. The same four hooks are therefore wired from the shell, which
// runs before the worker exists and next to it, and only through the public APIs of the
// existing modules:
//
//   InstallCrashCapture   (1)+(2) View::Initialize, the first thing the view does: installs
//                         the capture and registers a second win10xbox::trace sink that feeds
//                         CrashOnTraceLine (the "guest entry reached" line writes
//                         entry-reached.txt). The sink is added before host.cpp adds
//                         diag::TraceSink; the core calls every sink for every line.
//   CaptureHostModules    (3) before Host::StartWorker(): afterwards process::Launch may hand
//                         the PEB loader lists to the guest, and the module report would
//                         describe the guest's modules instead of the host's.
//   ReplayInto            the lines of (1) and (3) belong in trace.log, which the worker only
//                         opens (and truncates) a moment after it starts; they are written
//                         through Host::NoteLifecycle as soon as the log is open.
//   Detach                (4) see the note there.
//
// What the shell cannot do, and what that costs (the details are in host_shell.cpp):
//   * crash.h wants the trace.log HANDLE; it is private to host_diag.cpp, so none is passed
//     and crash.txt / crash.dmp / entry-reached.txt are the crash capture's only outputs;
//   * host.cpp calls diag::InstallCrashFilter after the shell installed the capture, which
//     replaces the capture's unhandled-exception filter (the vectored handler stays);
//   * Report::Flush() of host_diag.h does not call SetCrashSnapshot().
#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace w10x_uwp
{
    class Host;

    class ShellDiagnostics
    {
    public:
        ShellDiagnostics() = default;
        ShellDiagnostics(const ShellDiagnostics&) = delete;
        ShellDiagnostics& operator=(const ShellDiagnostics&) = delete;

        // First thing the shell does, on the UI thread, before the Host and its worker exist.
        // Idempotent. Never throws.
        void InstallCrashCapture(const std::wstring& local_state);

        // Records which kernel32 / user32 / gdi32full / shell32 / comctl32 this (host)
        // process has and from where. Call it before Host::StartWorker(): from process::Launch
        // on, the PEB loader lists belong to the guest. Also keeps the text for crash.txt and
        // entry-reached.txt (crash.h).
        void CaptureHostModules();

        // A line for trace.log that arises before the log is open (e.g. the activation kind);
        // written by ReplayInto(). If the log is already open it goes straight through
        // host->NoteLifecycle(). Any thread.
        void Note(Host* host, const std::string& text);

        // Writes the crash-capture state, the host module lines and the queued notes through
        // host.NoteLifecycle() once trace.log is open (the worker opens it first thing).
        // Returns true once that has happened; call it every pump iteration until then.
        bool ReplayInto(Host& host);
        bool Replayed() const;

        // Last act of the shell before the process is let go. The shell does not hand
        // crash.h a trace.log handle (see above), so this releases nothing today; it keeps the
        // InstallCrashCapture / DetachCrashTraceLog pairing explicit for the day host_diag
        // exposes its handle.
        void Detach();

    private:
        bool installed_ = false;
        std::string install_state_;
        std::vector<std::string> modules_;

        mutable std::mutex mu_;
        bool replayed_ = false;             // guarded by mu_
        std::vector<std::string> notes_;    // guarded by mu_
    };

    // Copies payload\launch.example.json (every launch.json key with its default, documented)
    // from the package to LocalState\launch.example.json, replacing the previous copy: it is
    // documentation, not configuration. host.cpp's error hints point the owner at that file.
    // Best effort: a missing payload file is not an error.
    void PublishExampleConfig(const std::wstring& package_dir, const std::wstring& local_state);
}  // namespace w10x_uwp
