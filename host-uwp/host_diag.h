// host_diag.h - the host's evidence trail in LocalState: trace.log (host lines +
// the core's trace), status.txt (verdict + facts, rewritten at every phase
// boundary) and a last-resort crash recorder. An AppContainer app that dies
// leaves nothing but a closed window, so these files are the only way to know
// what happened on a console.
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
    // Creates (truncates) the log. max_bytes == 0 means unlimited.
    bool OpenLog(const std::wstring& path, std::uint64_t max_bytes);
    void SetLogCap(std::uint64_t max_bytes);
    void CloseLog();

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
        // Rewrites the file and refreshes the crash recorder's snapshot.
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
    // Records an unhandled host exception in trace.log and status.txt
    // (preceded by the last status snapshot), then lets the OS continue.
    void InstallCrashFilter(const std::wstring& status_path);
}  // namespace diag
}  // namespace w10x_uwp
