// host_diag.cpp - see host_diag.h. The trace sink, the line logger, the report
// and the crash filter are lifted from host-uwp-lite/smoke.cpp; the additions are
// the log size cap, a lock that makes closing the log safe against late
// writers, a thread-safe Report, and the points where this module meets the crash
// capture of crash.h: the log handle it borrows, the status text it quotes and
// the unhandled-exception filter this module's one is chained to.
#include "host_diag.h"

#include "crash.h"
#include "host_util.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace w10x_uwp
{
namespace diag
{
namespace
{
    SRWLOCK g_log_lock = SRWLOCK_INIT;      // shared: writers; exclusive: open/close
    HANDLE g_log = INVALID_HANDLE_VALUE;
    wchar_t g_log_path[1024];               // the file g_log is open on (exclusive lock)
    std::atomic<std::uint64_t> g_log_bytes{0};
    std::atomic<std::uint64_t> g_log_cap{0};
    std::atomic<bool> g_log_capped{false};

    void RawLog(const char* data, std::size_t len)
    {
        AcquireSRWLockShared(&g_log_lock);
        HANDLE h = g_log;
        if (h != INVALID_HANDLE_VALUE)
        {
            const std::uint64_t cap = g_log_cap.load();
            const std::uint64_t before = g_log_bytes.fetch_add(len);
            if (cap != 0 && before + len > cap)
            {
                if (!g_log_capped.exchange(true))
                {
                    static const char kNote[] =
                        "[trace.log reached its size cap (launch.json: trace_log_max_mb); further lines are dropped]\n";
                    WriteAll(h, kNote, sizeof kNote - 1);
                }
            }
            else
            {
                WriteAll(h, data, len);
            }
        }
        ReleaseSRWLockShared(&g_log_lock);
    }

    // Snapshot of the last status text, for the crash filter (no heap in there).
    char g_snapshot[8192];
    std::size_t g_snapshot_len = 0;
    wchar_t g_crash_status_path[1024];
    volatile LONG g_in_crash = 0;
}  // namespace

bool OpenLog(const std::wstring& path, std::uint64_t max_bytes)
{
    AcquireSRWLockExclusive(&g_log_lock);
    bool ok = true;
    if (g_log != INVALID_HANDLE_VALUE && std::wcscmp(g_log_path, path.c_str()) == 0)
    {
        // Already open on this very file. The shell opens trace.log BEFORE it installs the crash
        // capture, because the capture keeps a copy of the handle value (LogHandle) and cannot be
        // given another one later; closing and reopening here would leave it with a dead handle.
        // So the handle and what was written to it stay; only the cap is taken over.
    }
    else
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_log != INVALID_HANDLE_VALUE)
        {
            uwp_lite::DetachCrashTraceLog();   // the capture may hold the old handle: it must not outlive it
            CloseHandle(g_log);
        }
        g_log = h;
        g_log_path[0] = 0;
        if (h != INVALID_HANDLE_VALUE) lstrcpynW(g_log_path, path.c_str(), static_cast<int>(sizeof g_log_path / sizeof(wchar_t)));
        g_log_bytes.store(0);
        g_log_capped.store(false);
        ok = h != INVALID_HANDLE_VALUE;
    }
    g_log_cap.store(max_bytes);
    ReleaseSRWLockExclusive(&g_log_lock);
    return ok;
}

HANDLE LogHandle()
{
    AcquireSRWLockShared(&g_log_lock);
    const HANDLE h = g_log;
    ReleaseSRWLockShared(&g_log_lock);
    return h;
}

void SetLogCap(std::uint64_t max_bytes)
{
    g_log_cap.store(max_bytes);
}

void CloseLog()
{
    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log != INVALID_HANDLE_VALUE)
    {
        uwp_lite::DetachCrashTraceLog();   // first: the capture may hold this handle (see LogHandle)
        FlushFileBuffers(g_log);
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
        g_log_path[0] = 0;
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

void FlushLog()
{
    AcquireSRWLockShared(&g_log_lock);
    if (g_log != INVALID_HANDLE_VALUE) FlushFileBuffers(g_log);
    ReleaseSRWLockShared(&g_log_lock);
}

std::uint64_t LogBytes()
{
    return g_log_bytes.load();
}

void TraceSink(const char* line, void*)
{
    if (line != nullptr) RawLog(line, std::strlen(line));
}

void Line(const char* fmt, ...)
{
    char msg[1536];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (static_cast<std::size_t>(n) >= sizeof msg) n = static_cast<int>(sizeof msg) - 1;
    SYSTEMTIME st;
    GetSystemTime(&st);
    char head[48];
    const int h = std::snprintf(head, sizeof head, "%02u:%02u:%02u.%03u host: ", static_cast<unsigned>(st.wHour),
                                static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
                                static_cast<unsigned>(st.wMilliseconds));
    std::string line(head, h > 0 ? static_cast<std::size_t>(h) : 0);
    line.append(msg, static_cast<std::size_t>(n));
    line.push_back('\n');
    RawLog(line.data(), line.size());
}

// ----------------------------------------------------------------------
// Report
// ----------------------------------------------------------------------

Report::Report(std::wstring path) : path_(std::move(path)) {}

void Report::Set(const std::string& key, const std::string& value)
{
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& kv : kv_)
    {
        if (kv.first == key)
        {
            kv.second = value;
            return;
        }
    }
    kv_.emplace_back(key, value);
}

void Report::SetVerdict(const std::string& verdict)
{
    std::lock_guard<std::mutex> lock(mu_);
    verdict_ = verdict;
}

void Report::Flush()
{
    std::lock_guard<std::mutex> lock(mu_);
    std::string text = "RESULT: " + verdict_ + "\n";
    for (const auto& kv : kv_) text += kv.first + "=" + kv.second + "\n";
    g_snapshot_len = text.size() < sizeof g_snapshot ? text.size() : sizeof g_snapshot;
    std::memcpy(g_snapshot, text.data(), g_snapshot_len);
    uwp_lite::SetCrashSnapshot(text.data(), text.size());   // crash.txt quotes the latest text as well (crash.h)

    if (held_ != INVALID_HANDLE_VALUE)
    {
        // Rewrite in place: handle operations only.
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (SetFilePointerEx(held_, zero, nullptr, FILE_BEGIN))
        {
            WriteAll(held_, text.data(), text.size());
            SetEndOfFile(held_);
            FlushFileBuffers(held_);
        }
        return;
    }
    HANDLE h = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteAll(h, text.data(), text.size());
    CloseHandle(h);
}

bool Report::HoldOpen()
{
    std::lock_guard<std::mutex> lock(mu_);
    if (held_ != INVALID_HANDLE_VALUE) return true;
    held_ = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    return held_ != INVALID_HANDLE_VALUE;
}

void Report::ReleaseHold()
{
    std::lock_guard<std::mutex> lock(mu_);
    if (held_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(held_);
        held_ = INVALID_HANDLE_VALUE;
    }
}

// ----------------------------------------------------------------------
// Crash recorder: no heap use in here (the crash may be inside the heap).
// ----------------------------------------------------------------------
namespace
{
    char* PutStr(char* p, const char* s)
    {
        while (*s) *p++ = *s++;
        return p;
    }

    char* PutHex(char* p, unsigned long long v, int digits)
    {
        static const char kDigits[] = "0123456789abcdef";
        for (int i = digits - 1; i >= 0; --i) *p++ = kDigits[(v >> (i * 4)) & 0xf];
        return p;
    }

    // The unhandled-exception filter that was installed before ours: the crash capture's
    // (crash.cpp), which the shell installs first thing, or nullptr. SetUnhandledExceptionFilter
    // keeps ONE filter, so installing ours without remembering it turns the capture's off: no
    // UNHANDLED record in crash.txt, no dump for the exception that ends the process, no line
    // in trace.log.
    std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_capture_filter{nullptr};

    // The host's top-level filter. It runs the crash capture's filter FIRST and its own report
    // SECOND. Both always run, and the order is deliberate:
    //
    //  * The capture is the hardened one. With little stack left (a stack overflow) it hands its
    //    whole report to a thread of its own, and on a guest thread it first puts the host's
    //    TLS/FLS back. Anything that ran before it could use up the few KiB of stack that are
    //    left, or hang, and then crash.txt would not be written at all. Whatever happens in the
    //    best-effort part below, the capture has already done its work.
    //  * The capture writes its crash.txt record and its trace.log line BEFORE its slow step, the
    //    minidump (it suspends every other thread and can take seconds). So only the report below
    //    waits for the dump. That report is the rewrite of status.txt, and it is the one the capture
    //    cannot do while the guest runs: Report::HoldOpen() keeps status.txt open for writing and
    //    the capture reopens it with FILE_SHARE_READ only (a sharing violation). Only this module
    //    also has the text for it (the Report's snapshot). It runs last, so its status.txt is the
    //    one that stays.
    //  * Nothing is written twice. crash.txt, the dump and the trace.log line come from the capture
    //    alone, so this filter writes the trace.log line itself only when it has no capture to
    //    chain to. status.txt is replaced by each writer, never appended to.
    //  * The guard makes the body run once per process and never nested. A fault inside the capture
    //    (or in the code below) re-enters this filter on the same thread; chaining again would make
    //    the capture's retry loop (200 x 10 ms for its g_busy lock) wait for a lock that very thread
    //    still holds. A second thread dying while the first one reports is skipped too: the first
    //    report is the one that matters, and the capture's vectored handler has recorded the rest.
    //  * The result is the capture's (EXCEPTION_CONTINUE_SEARCH: the OS goes on and ends the
    //    process), as it was before this filter chained to it.
    LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
    {
        if (InterlockedExchange(&g_in_crash, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;

        const LPTOP_LEVEL_EXCEPTION_FILTER capture = g_capture_filter.load();
        const LONG verdict = capture != nullptr ? capture(ep) : EXCEPTION_CONTINUE_SEARCH;

        if (ep == nullptr || ep->ExceptionRecord == nullptr) return verdict;
        char line[768];
        char* p = line;
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        p = PutStr(p, "CRASH: host unhandled exception code=0x");
        p = PutHex(p, er->ExceptionCode, 8);
        p = PutStr(p, " address=0x");
        p = PutHex(p, reinterpret_cast<std::uintptr_t>(er->ExceptionAddress), 16);
        p = PutStr(p, " thread=0x");
        p = PutHex(p, GetCurrentThreadId(), 4);
        if (er->NumberParameters >= 2)
        {
            p = PutStr(p, " info0=0x");
            p = PutHex(p, er->ExceptionInformation[0], 16);
            p = PutStr(p, " info1=0x");
            p = PutHex(p, er->ExceptionInformation[1], 16);
        }
        if (const CONTEXT* c = ep->ContextRecord)
        {
            p = PutStr(p, " rip=0x");
            p = PutHex(p, c->Rip, 16);
            p = PutStr(p, " rsp=0x");
            p = PutHex(p, c->Rsp, 16);
            p = PutStr(p, " rbp=0x");
            p = PutHex(p, c->Rbp, 16);
            p = PutStr(p, " rax=0x");
            p = PutHex(p, c->Rax, 16);
            p = PutStr(p, " rcx=0x");
            p = PutHex(p, c->Rcx, 16);
            p = PutStr(p, " rdx=0x");
            p = PutHex(p, c->Rdx, 16);
        }
        *p++ = '\n';
        // trace.log: a chained capture has written the same line itself (it borrows the log handle,
        // see LogHandle), so this one is for the case without a capture. The log handle is read
        // without the lock on purpose: this may run on a thread that holds it.
        if (capture == nullptr && g_log != INVALID_HANDLE_VALUE) WriteAll(g_log, line, static_cast<std::size_t>(p - line));
        if (g_crash_status_path[0] != 0)
        {
            HANDLE h = CreateFileW(g_crash_status_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                static const char kHead[] = "RESULT: FAIL (the host process crashed)\n";
                WriteAll(h, kHead, sizeof kHead - 1);
                WriteAll(h, line, static_cast<std::size_t>(p - line));
                if (capture != nullptr)
                {
                    // The capture is done by now, the dump included.
                    static const char kFiles[] =
                        "crash records: crash.txt; minidump: crash.dmp (when that file exists, it is complete)\n";
                    WriteAll(h, kFiles, sizeof kFiles - 1);
                }
                static const char kTail[] = "--- state before the crash ---\n";
                WriteAll(h, kTail, sizeof kTail - 1);
                WriteAll(h, g_snapshot, g_snapshot_len);
                CloseHandle(h);
            }
        }
        return verdict;
    }
}  // namespace

void InstallCrashFilter(const std::wstring& status_path)
{
    lstrcpynW(g_crash_status_path, status_path.c_str(), static_cast<int>(sizeof g_crash_status_path / sizeof(wchar_t)));
    // Keep what SetUnhandledExceptionFilter returns: the filter installed before ours (see
    // CrashFilter). Installed twice, the "previous" one would be ours: never chain to ourselves.
    // (Between the two statements an unhandled exception on another thread would see no chain;
    // that is a few instructions at the start of the worker, and the capture's vectored handler
    // is not affected.)
    const LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(&CrashFilter);
    if (previous != &CrashFilter) g_capture_filter.store(previous);
}

bool CrashFilterChained()
{
    return g_capture_filter.load() != nullptr;
}

}  // namespace diag
}  // namespace w10x_uwp
