// host_shell.cpp - see host_shell.h.
//
// Notes on how the copied crash capture (crash.h, a byte-for-byte copy of
// host-uwp-lite/crash.cpp: namespace uwp_lite is kept so the two stay diffable) coexists
// with host_diag.h, which host.cpp's worker uses for its own evidence trail:
//
//   trace.log   crash.h appends its one-line crash summaries to a borrowed HANDLE that it keeps
//               from InstallCrashCapture on and cannot be given again, and trace.log belongs to
//               host_diag. So the shell opens trace.log (diag::OpenLog) BEFORE it installs the
//               capture and passes it diag::LogHandle(), as host-uwp-lite's smoke does with its
//               own handle. The worker's diag::OpenLog on the same path then keeps that handle
//               (a reopen would close the one the capture holds), and host_diag detaches the
//               capture before it ever closes the handle. If the log cannot be opened this early
//               the capture gets INVALID_HANDLE_VALUE, which it handles safely: the summaries are
//               then missing from trace.log, crash.txt / crash.dmp / entry-reached.txt are not.
//   status.txt  CrashConfig::result_path is status.txt, the file host_diag's Report owns. While
//               the worker has not installed its own crash filter yet (shell start-up, the first
//               milliseconds of the worker) the capture's unhandled-exception filter is the
//               active one, and it replaces a stale status.txt of the previous run with
//               "RESULT: FAIL (the host process crashed)". From diag::InstallCrashFilter on,
//               host_diag's filter is the top-level one and chains to the capture's (the order
//               and the reasons are in host_diag.cpp): the capture writes the UNHANDLED record,
//               the dump and, when it can, the same rewrite; host_diag's rewrite comes last and
//               carries the Report's facts.
//   snapshot    Report::Flush() refreshes host_diag's own crash snapshot, not crash.h's
//               (SetCrashSnapshot), so crash.txt has no "status as last written" section. Feeding
//               it is one line in Report::Flush(), but crash.cpp's WriteResultFail then does this
//               while the guest runs: its rewrite of status.txt fails (Report::HoldOpen() holds the
//               file open for writing) and its append of the snapshot to the same file succeeds, so
//               status.txt carries the text twice until host_diag's rewrite replaces it. That
//               cannot be fixed here (crash.cpp is a verbatim copy), hence it is left out.
#include "host_shell.h"

#include "crash.h"
#include "host.h"
#include "host_diag.h"
#include "host_stage.h"
#include "host_util.h"

#include "win10xbox/core.h"
#include "win10xbox/trace.h"

#include <windows.h>

#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace w10x_uwp
{
namespace
{
    // win10xbox::trace sink: every enabled trace line of the core, under the core's trace
    // mutex and on any thread, so it must not call back into the trace API (CrashOnTraceLine
    // does not). The line that announces the guest's entry writes entry-reached.txt; it is only
    // logged when the `modules` channel is on, which is what EnsureModulesChannel() sees to.
    void CrashSink(const char* line, void*)
    {
        uwp_lite::CrashOnTraceLine(line);
    }
}  // namespace

std::string EnsureModulesChannel()
{
    // core/src/trace.cpp reads WIN10XBOX_DEBUG into a 1024-byte buffer and ignores the whole
    // variable when it does not fit, so a spec may not grow past 1023 characters (launch.json
    // "trace" is capped at 900, launch_config.cpp, which leaves room for the item added here).
    constexpr std::size_t kCoreMaxChars = 1023;
    static const wchar_t kChannel[] = L"+modules";

    wchar_t cur[1024];
    const DWORD n = GetEnvironmentVariableW(L"WIN10XBOX_DEBUG", cur, 1024);
    if (n >= 1024)
    {
        // n is the size the value needs. It does not fit the core's buffer either, so the core
        // ignores it already; appending to it would not make it readable.
        return Fmt("WIN10XBOX_DEBUG is longer than the core reads (%lu characters) and is left as it is: "
                   "+modules was NOT added",
                   static_cast<unsigned long>(n - 1));
    }
    std::wstring spec(cur, n);   // n == 0: unset, or empty

    // The core applies the items left to right: a spec whose last item is +modules has the channel on.
    const wchar_t* last_item = spec.c_str();
    for (const wchar_t* p = last_item; *p != 0; ++p)
        if (*p == L',') last_item = p + 1;
    if (std::wcscmp(last_item, kChannel) == 0)
        return "WIN10XBOX_DEBUG=" + Narrow(spec) + " (+modules is the last item already: the guest-entry marker rides on that channel)";

    const std::wstring add = spec.empty() ? std::wstring(kChannel) : std::wstring(L",") + kChannel;
    if (spec.size() + add.size() > kCoreMaxChars)
    {
        return Fmt("WIN10XBOX_DEBUG is left as it is (%u characters): adding +modules would make it longer than the core "
                   "reads (%u), so entry-reached.txt is written only if the spec itself leaves the modules channel on",
                   static_cast<unsigned>(spec.size()), static_cast<unsigned>(kCoreMaxChars));
    }
    spec += add;
    if (!SetEnvironmentVariableW(L"WIN10XBOX_DEBUG", spec.c_str()))
    {
        return Fmt("WIN10XBOX_DEBUG could not be updated (error %lu): +modules was NOT added",
                   static_cast<unsigned long>(GetLastError()));
    }
    return "WIN10XBOX_DEBUG=" + Narrow(spec) + " (+modules is always added: the guest-entry marker rides on that channel)";
}

void ShellDiagnostics::InstallCrashCapture(const std::wstring& local_state)
{
    if (installed_) return;
    installed_ = true;
    try
    {
        // trace.log FIRST, like host-uwp-lite's smoke: the capture keeps a copy of the handle it
        // is given here. host_diag owns the log; the worker's own OpenLog on the same path keeps
        // this handle, and the worker sets the real size cap then.
        const bool log_open = diag::OpenLog(local_state + L"\\trace.log", /*max_bytes=*/0);

        const win10xbox::Version ver = win10xbox::GetVersion();
        uwp_lite::CrashConfig cc;
        cc.local_state = local_state;
        cc.result_path = local_state + L"\\status.txt";
        cc.build = Fmt("host-uwp %s %s, core %d.%d.%d (%s)", __DATE__, __TIME__, ver.major, ver.minor, ver.patch,
                       ver.git ? ver.git : "?");
        install_state_ = uwp_lite::InstallCrashCapture(cc, diag::LogHandle());
        install_state_ += log_open ? "; its one-line crash summaries go to trace.log"
                                   : "; trace.log could not be opened yet, so its crash summaries will not reach it";

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
        // The shell opens trace.log, but (almost) nothing is written to it until the worker
        // runs: its OpenLog, then its filter, then its first Line() follow one another at once,
        // so a non-zero byte count means "the worker is at its start-up", and what is replayed
        // then follows the worker's own lines. Until then notes are held back in notes_.
        if (diag::LogBytes() == 0) return false;
        replayed_ = true;
        notes.swap(notes_);
    }
    if (!install_state_.empty()) host.NoteLifecycle(install_state_.c_str());
    host.NoteLifecycle(diag::CrashFilterState());
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
