// crash.h - what host-uwp-lite leaves behind when the host process dies, and the
// facts about the host process the harness records at startup.
//
// An AppContainer app that dies leaves nothing behind but a closed window, and the
// first on-console run did exactly that: the trace ended at the guest's entry and
// the process was gone. Everything here writes plain files in LocalState and uses
// only raw Win32 in the exception paths (no CRT formatting, no heap), because the
// crash may be inside the heap or on a thread whose TEB shows the guest's TLS/FLS.
//
//   crash.txt          readable exception records (appended): code and name, address
//                      and what it points into, access-violation details, registers,
//                      the bytes at RIP, the stack words with module+offset, the
//                      guest's module list, the host module paths, and result.txt as
//                      it was. For Wine's EXCEPTION_WINE_STUB (a guest call through
//                      an import that did not resolve) the DLL and function names.
//   crash.dmp          a minidump (MiniDumpWriteDump, from a thread created at
//                      startup) of the first fatal-looking exception on a guest
//                      thread, or of the unhandled exception that ends the process.
//   entry-reached.txt  written the moment the core announces that control is about
//                      to enter the guest ("guest entry reached" on the trace sink),
//                      i.e. immediately before NtContinue switches to guest code.
//
// Two hooks feed crash.txt: a vectored handler added LAST (it sees what the core's
// own handlers did not claim, before any frame-based handler, including the guest's
// own SEH and Wine's thread-start filter, which may end the process without the
// unhandled-exception filter ever running) and the unhandled-exception filter.
#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace uwp_lite
{
    struct CrashConfig
    {
        std::wstring local_state;   // crash.txt, crash.dmp, entry-reached.txt are created here
        std::wstring result_path;   // result.txt: an unhandled exception rewrites it as "FAIL (the host process crashed)"
        std::string  build;         // build tag for the crash.txt header
    };

    // Installs the handlers and creates the dump thread. Idempotent. `trace_log` is the
    // harness's trace.log handle: crash lines are appended with raw WriteFile (the
    // handle is borrowed, see DetachCrashTraceLog). Returns a one-line description of
    // what is available (dbghelp / MiniDumpWriteDump bound or not) for trace.log.
    std::string InstallCrashCapture(const CrashConfig& cfg, HANDLE trace_log);

    // Forget the trace.log handle (call right before closing it). The handlers stay
    // installed and keep writing the files in LocalState.
    void DetachCrashTraceLog();

    // The harness's current result.txt text; crash.txt quotes the latest one.
    void SetCrashSnapshot(const char* text, std::size_t len);

    // Every line the core's trace emits goes through here (call it from the trace
    // sink: it must not call back into the trace API and does not). The line that
    // announces the guest's entry writes entry-reached.txt, once.
    void CrashOnTraceLine(const char* line);

    // GetModuleFileNameW of kernel32, user32, gdi32full, shell32 and comctl32 in THIS
    // (host) process, one line each, "absent" when a module is not loaded. Also kept
    // for crash.txt. Call it before the guest launches: from then on the PEB's loader
    // lists belong to the guest.
    std::vector<std::string> DescribeHostModules();

    struct CrashStats
    {
        bool entry_reached = false;
        long fatal_records = 0;          // fatal-looking exceptions written to crash.txt
        bool dump_written = false;       // crash.dmp exists
        bool dump_attempted = false;
        unsigned long dump_error = 0;    // Win32 error / exception code of a failed dump
    };
    CrashStats GetCrashStats();
}  // namespace uwp_lite
