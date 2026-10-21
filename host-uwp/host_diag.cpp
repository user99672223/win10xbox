// host_diag.cpp - see host_diag.h. The trace sink, the line logger, the report
// and the crash filter are lifted from host-uwp-lite/smoke.cpp; the additions are
// the log size cap, a lock that makes closing the log safe against late
// writers, and a thread-safe Report.
#include "host_diag.h"

#include "host_util.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace w10x_uwp
{
namespace diag
{
namespace
{
    SRWLOCK g_log_lock = SRWLOCK_INIT;      // shared: writers; exclusive: open/close
    HANDLE g_log = INVALID_HANDLE_VALUE;
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
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    g_log = h;
    g_log_bytes.store(0);
    g_log_cap.store(max_bytes);
    g_log_capped.store(false);
    ReleaseSRWLockExclusive(&g_log_lock);
    return h != INVALID_HANDLE_VALUE;
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
        FlushFileBuffers(g_log);
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
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

    LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
    {
        if (InterlockedExchange(&g_in_crash, 1) != 0 || ep == nullptr || ep->ExceptionRecord == nullptr)
            return EXCEPTION_CONTINUE_SEARCH;
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
        // The log handle is read without the lock on purpose: this may run on a
        // thread that holds it.
        if (g_log != INVALID_HANDLE_VALUE) WriteAll(g_log, line, static_cast<std::size_t>(p - line));
        if (g_crash_status_path[0] != 0)
        {
            HANDLE h = CreateFileW(g_crash_status_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                static const char kHead[] = "RESULT: FAIL (the host process crashed)\n";
                WriteAll(h, kHead, sizeof kHead - 1);
                WriteAll(h, line, static_cast<std::size_t>(p - line));
                static const char kTail[] = "--- state before the crash ---\n";
                WriteAll(h, kTail, sizeof kTail - 1);
                WriteAll(h, g_snapshot, g_snapshot_len);
                CloseHandle(h);
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}  // namespace

void InstallCrashFilter(const std::wstring& status_path)
{
    lstrcpynW(g_crash_status_path, status_path.c_str(), static_cast<int>(sizeof g_crash_status_path / sizeof(wchar_t)));
    SetUnhandledExceptionFilter(&CrashFilter);
}

}  // namespace diag
}  // namespace w10x_uwp
