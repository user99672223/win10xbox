// host_shell.cpp - see host_shell.h.
//
// Notes on how the copied crash capture (crash.h, a byte-for-byte copy of
// host-uwp-lite/crash.cpp: namespace uwp_lite is kept so the two stay diffable) coexists
// with host_diag.h, which host.cpp's worker uses for its own evidence trail:
//
//   trace.log   crash.h appends its one-line crash summaries to a borrowed HANDLE. The
//               trace.log handle belongs to host_diag.cpp (opened by the worker, private),
//               so INVALID_HANDLE_VALUE is passed and the summaries are simply not written
//               to trace.log. crash.txt (full records), crash.dmp and entry-reached.txt do
//               not depend on it.
//   status.txt  CrashConfig::result_path is status.txt, the file host_diag's Report owns.
//               While the worker has not installed its own crash filter yet (shell start-up,
//               the first milliseconds of the worker) the capture's unhandled-exception
//               filter is the active one, and it replaces a stale status.txt of the previous
//               run with "RESULT: FAIL (the host process crashed)". From the worker's
//               diag::InstallCrashFilter on, host_diag's filter takes over: it does the
//               same rewrite, with the Report's own snapshot of the facts so far.
//   filters     SetUnhandledExceptionFilter keeps one filter; host_diag's replaces the
//               capture's. The capture's last-chance VECTORED handler is unaffected and
//               still records every fatal-looking exception (crash.txt, and crash.dmp for
//               the first one on a thread that runs guest code). What is lost is only the
//               capture's "UNHANDLED" record and dump for an exception on a host thread
//               that nothing handled.
//   snapshot    Report::Flush() refreshes host_diag's own crash snapshot, not crash.h's
//               (SetCrashSnapshot), so crash.txt has no "status as last written" section.
//
// All three are consequences of not changing host.cpp / host_diag.cpp. Removing the
// diag::InstallCrashFilter call from host.cpp (or making the two filters chain) and passing
// the log handle would lift them; see the report that accompanied this file.
#include "host_shell.h"

#include "crash.h"
#include "host.h"
#include "host_diag.h"
#include "host_stage.h"
#include "host_util.h"

#include "win10xbox/core.h"
#include "win10xbox/trace.h"

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

namespace w10x_uwp
{
namespace
{
    // win10xbox::trace sink: every enabled trace line of the core, under the core's trace
    // mutex and on any thread, so it must not call back into the trace API (CrashOnTraceLine
    // does not). The line that announces the guest's entry writes entry-reached.txt.
    void CrashSink(const char* line, void*)
    {
        uwp_lite::CrashOnTraceLine(line);
    }

    // The "guest entry reached" line is logged on the core's `modules` channel (TRACE(modules,
    // ...) in core/dispatch/nt_section.cpp), and a channel that WIN10XBOX_DEBUG leaves off
    // silently turns entry-reached.txt off. When the environment does not set WIN10XBOX_DEBUG
    // at all, the quiet default (err + fixme) gets +modules added (a few dozen lines per run).
    // A launch.json "trace" spec still replaces the variable (host.cpp), so it has to contain
    // +modules to keep the marker; the packaged payload\launch.json does.
    void EnsureModulesChannel()
    {
        wchar_t probe[8];
        if (GetEnvironmentVariableW(L"WIN10XBOX_DEBUG", probe, 8) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
            SetEnvironmentVariableW(L"WIN10XBOX_DEBUG", L"+modules");
    }
}  // namespace

void ShellDiagnostics::InstallCrashCapture(const std::wstring& local_state)
{
    if (installed_) return;
    installed_ = true;
    try
    {
        EnsureModulesChannel();

        const win10xbox::Version ver = win10xbox::GetVersion();
        uwp_lite::CrashConfig cc;
        cc.local_state = local_state;
        cc.result_path = local_state + L"\\status.txt";
        cc.build = Fmt("host-uwp %s %s, core %d.%d.%d (%s)", __DATE__, __TIME__, ver.major, ver.minor, ver.patch,
                       ver.git ? ver.git : "?");
        install_state_ = uwp_lite::InstallCrashCapture(cc, INVALID_HANDLE_VALUE);

        win10xbox::trace::AddSink(&CrashSink, nullptr);
    }
    catch (...)
    {
        install_state_ = "crash capture could not be installed (C++ exception)";
    }
}

void ShellDiagnostics::CaptureHostModules()
{
    try
    {
        modules_ = uwp_lite::DescribeHostModules();
    }
    catch (...)
    {
        modules_.clear();
    }
}

void ShellDiagnostics::Note(Host* host, const std::string& text)
{
    bool direct = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        direct = host != nullptr && replayed_;
        if (!direct) notes_.push_back(text);
    }
    if (direct) host->NoteLifecycle(text.c_str());
}

bool ShellDiagnostics::ReplayInto(Host& host)
{
    std::vector<std::string> notes;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (replayed_) return true;
        // OpenLog is the first thing the worker does and the first Line() follows at once, so
        // a non-zero byte count means "trace.log is open and the worker is at its start-up".
        // Before that every Line() is dropped by host_diag (no handle yet).
        if (diag::LogBytes() == 0) return false;
        replayed_ = true;
        notes.swap(notes_);
    }
    if (!install_state_.empty()) host.NoteLifecycle(install_state_.c_str());
    for (const std::string& m : modules_) host.NoteLifecycle(("host " + m).c_str());
    for (const std::string& n : notes) host.NoteLifecycle(n.c_str());
    return true;
}

bool ShellDiagnostics::Replayed() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return replayed_;
}

void ShellDiagnostics::Detach()
{
    uwp_lite::DetachCrashTraceLog();
}

void PublishExampleConfig(const std::wstring& package_dir, const std::wstring& local_state)
{
    unsigned long long bytes = 0;
    DWORD err = 0;
    CopyFileTo(package_dir + L"\\payload\\launch.example.json", local_state + L"\\launch.example.json", /*always=*/true,
               &bytes, &err);
}
}  // namespace w10x_uwp
