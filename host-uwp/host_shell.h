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
//   InstallCrashCapture   (1)+(2) View::Initialize, the first thing the view does: opens
//                         trace.log through host_diag (the capture keeps the handle it is given
//                         and cannot be given another later), installs the capture with that
//                         handle so its one-line crash summaries land in trace.log, and registers
//                         a second win10xbox::trace sink that feeds CrashOnTraceLine (the "guest
//                         entry reached" line writes entry-reached.txt). The sink is added before
//                         host.cpp adds diag::TraceSink; the core calls every sink for every line.
//   CaptureHostModules    (3) before Host::StartWorker(): afterwards process::Launch may hand
//                         the PEB loader lists to the guest, and the module report would
//                         describe the guest's modules instead of the host's.
//   ReplayInto            the lines of (1) and (3) belong in trace.log, which the worker takes over
//                         (diag::OpenLog on the same file) and starts writing a moment after it
//                         starts; they are written through Host::NoteLifecycle as soon as the
//                         worker's first line is in, so they follow the worker's own lines.
//   Detach                (4) see the note there.
//   EnsureModulesChannel  the entry marker rides on the core's `modules` trace channel; host.cpp
//                         calls this after it applied launch.json's "trace" (see below).
//
// Where the capture and host_diag meet (host_diag.h and host_diag.cpp have the details):
//   * trace.log   host_diag owns the handle and keeps it valid for the capture: it never closes
//                 it without detaching the capture first, and the worker's OpenLog keeps it;
//   * the unhandled-exception filter   diag::InstallCrashFilter (called by the worker, after the
//                 capture installed its own) chains to the capture's filter, so the UNHANDLED
//                 record, the dump and host_diag's FAIL rewrite of status.txt all happen;
//   * status.txt  CrashConfig::result_path is status.txt, so until the worker installs host_diag's
//                 filter (shell start-up) the capture's own filter is the one that replaces a stale
//                 status.txt of the previous run. Report::Flush() does NOT hand its text to the
//                 capture (SetCrashSnapshot), so crash.txt has no "status as last written"
//                 section; host_shell.cpp says why that is left alone.
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
        // Opens trace.log (host_diag), installs the capture with its handle and registers the
        // trace sink for the entry marker. Idempotent. Never throws.
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
        // host.NoteLifecycle() once the worker has started writing trace.log (it takes over the
        // file the shell opened, and its first line follows at once). Returns true once that has
        // happened; call it every pump iteration until then.
        bool ReplayInto(Host& host);
        bool Replayed() const;

        // Last act of the shell before the process is let go: the capture stops writing to
        // trace.log (DetachCrashTraceLog), the pairing with InstallCrashCapture. The handle itself
        // is host_diag's and stays open until the process ends; host_diag detaches the capture on
        // its own before it would ever close it (diag::CloseLog, or an OpenLog on another file).
        void Detach();

    private:
        bool installed_ = false;
        std::string install_state_;
        std::vector<std::string> modules_;

        mutable std::mutex mu_;
        bool replayed_ = false;             // guarded by mu_
        std::vector<std::string> notes_;    // guarded by mu_
    };

    // Makes sure the core traces its `modules` channel, whatever WIN10XBOX_DEBUG says. The line
    // "guest entry reached" (logged on that channel by core/dispatch right before NtContinue
    // switches to guest code) is what writes entry-reached.txt (crash.h), and a spec that leaves
    // the channel off would turn that marker off without a word. host.cpp REPLACES the variable
    // with launch.json's "trace" when the owner wrote one, so this is called after that and
    // before win10xbox::Initialize reads the variable.
    //
    // "+modules" becomes the LAST item of the spec (the core applies the items from left to
    // right, so a trailing +modules wins over an earlier -modules or -all) unless it is the last
    // item already; the rest of the spec is untouched. The module-load lines it adds are a few
    // dozen per run. If the result would be longer than the core reads (1023 characters; it
    // ignores the WHOLE variable otherwise) nothing is changed. Returns one line for trace.log
    // saying what the variable is now. Call it from one thread (the worker).
    std::string EnsureModulesChannel();

    // Copies payload\launch.example.json (every launch.json key with its default, documented)
    // from the package to LocalState\launch.example.json, replacing the previous copy: it is
    // documentation, not configuration. host.cpp's error hints point the owner at that file.
    // Best effort: a missing payload file is not an error.
    void PublishExampleConfig(const std::wstring& package_dir, const std::wstring& local_state);
}  // namespace w10x_uwp
