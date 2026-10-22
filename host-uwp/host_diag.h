// host_diag.h - the host's evidence trail in LocalState: trace.log (host lines +
// the core's trace), status.txt (verdict + facts, rewritten at every phase
// boundary) and a last-resort crash recorder. An AppContainer app that dies
// leaves nothing but a closed window, so these files are the only way to know
// what happened on a console.
//
// This module and the crash capture of crash.h (crash.txt, crash.dmp, entry-reached.txt and
// the one-line crash summaries) work side by side; host_shell.h says how the shell wires the
// capture in. They meet in three places, all implemented here:
//   * trace.log: the capture borrows the log's handle (LogHandle) for its summaries;
//   * the unhandled-exception filter: InstallCrashFilter chains to the capture's;
//   * status.txt: Report::Flush() also hands the text to the capture (SetCrashSnapshot).
//
// PEB note: process::Launch lets the guest take over PEB fields (loader lists,
// process parameters, loader lock) while it runs, and shell-side threads are
// not guarded against that (README.md, "Known limits"). Everything in the
// logging path therefore touches only an already open HANDLE (WriteFile,
// FlushFileBuffers), never a path, while the guest runs. Report::Flush() opens
// a path and is used at phase boundaries on the worker thread, before the
// guest starts and after it has exited.
#pragma once

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace w10x_uwp
{
namespace diag
{
    // ---- trace.log -------------------------------------------------------
    // Creates (truncates) the log. max_bytes == 0 means unlimited. Calling it again for the
    // file that is already open (same path) keeps the open handle and what was written to it,
    // and only sets the cap: the shell opens the log before the worker exists (see LogHandle),
    // and the worker's call must not replace the handle the crash capture holds.
    bool OpenLog(const std::wstring& path, std::uint64_t max_bytes);
    void SetLogCap(std::uint64_t max_bytes);
    // Flushes and closes the log. The crash capture is detached from the handle first.
    void CloseLog();

    // The open log's handle, for the crash capture (crash.h) to borrow. InstallCrashCapture
    // keeps a copy of the handle value and appends its one-line crash summaries through it with
    // raw WriteFile, and it cannot be given another handle later. So: open the log first, then
    // install the capture with LogHandle(). The copy stays valid because this module never
    // closes the handle without calling DetachCrashTraceLog() first (CloseLog, and an OpenLog
    // that has to replace the file) and OpenLog on the same path keeps it. The handle stays
    // owned by this module: do not close it. INVALID_HANDLE_VALUE when no log is open.
    HANDLE LogHandle();

    // "HH:MM:SS.mmm host: <text>\n". Safe on every thread.
    void Line(const char* fmt, ...);

    // win10xbox::trace sink: the core's lines verbatim. Called under the
    // core's trace mutex on any thread, so it only does a raw write.
    void TraceSink(const char* line, void* ctx);

    void FlushLog();                  // FlushFileBuffers on the log (handle only)
    std::uint64_t LogBytes();         // bytes written so far

    // ---- status.txt ------------------------------------------------------
    // "RESULT: <verdict>" followed by key=value lines. Thread-safe.
    class Report
    {
    public:
        explicit Report(std::wstring path);

        void Set(const std::string& key, const std::string& value);
        void SetVerdict(const std::string& verdict);
        // Rewrites the file and refreshes the crash recorder's snapshot (this module's own, and
        // the crash capture's, which crash.txt quotes).
        void Flush();

        // Keeps status.txt open so that Flush() rewrites it in place through the
        // handle. Used while the guest runs, when opening a path is not safe
        // (see the PEB note above): HoldOpen() just before process::Launch,
        // ReleaseHold() once the guest has exited.
        bool HoldOpen();
        void ReleaseHold();

    private:
        std::wstring path_;
        std::mutex mu_;
        std::string verdict_ = "INCOMPLETE";
        std::vector<std::pair<std::string, std::string>> kv_;
        HANDLE held_ = INVALID_HANDLE_VALUE;
    };

    // ---- crash recorder --------------------------------------------------
    // Installs the unhandled-exception filter of the host. It CHAINS to the filter that was
    // installed before it (the crash capture's, which the shell installs first thing: crash.txt
    // gets the UNHANDLED record, crash.dmp the dump, trace.log the one-line summary), then
    // rewrites status.txt as "RESULT: FAIL (the host process crashed)" followed by the last
    // status snapshot, and lets the OS continue. The order and the reasons are in host_diag.cpp.
    // Call it after the capture was installed, or there is nothing to chain to.
    void InstallCrashFilter(const std::wstring& status_path);

    // True once InstallCrashFilter found a filter installed before it and chains to it.
    bool CrashFilterChained();
}  // namespace diag
}  // namespace w10x_uwp
