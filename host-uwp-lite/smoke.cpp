// smoke.cpp - host-uwp-lite: the on-console M1/M2 smoke, driven only through the
// public win10xbox API (core.h, config.h, dispatch.h, trace.h).
//
// The sequence is host-win32/main.cpp's wmain body, wrapped in what an
// AppContainer host additionally has to do:
//   1. stage the machine folder in LocalState (Wine builtins from the package,
//      genuine NLS data / DLLs read BY NAME from the console's own System32);
//   2. point the guest's stdout/stderr at LocalState\stdout.txt and the core's
//      trace at LocalState\trace.log;
//   3. Initialize -> process::Launch -> dispatch::Shutdown -> Shutdown;
//   4. write LocalState\result.txt (PASS iff Initialize == 0 and the guest
//      exits with 42).
//
// Everything runs on the caller's worker thread (probe lesson, DECISIONS.md:
// never block the CoreWindow thread). Nothing here touches the loader,
// dispatcher or kernel code: it is an integration harness only.
#include "smoke.h"

#include "win10xbox/core.h"
#include "win10xbox/dispatch.h"
#include "win10xbox/trace.h"

#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace uwp_lite
{
namespace
{

constexpr unsigned kDefaultTimeoutSec = 600;   // watchdog for the guest (UWP_LITE_TIMEOUT_SEC overrides)
constexpr int kExpectedGuestExit = 42;         // smoke_guest.c returns this

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

std::string Fmt(const char* fmt, ...)
{
    char buf[1536];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return std::string();
    if (static_cast<std::size_t>(n) >= sizeof buf) n = static_cast<int>(sizeof buf) - 1;
    return std::string(buf, static_cast<std::size_t>(n));
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

void WriteAll(HANDLE h, const char* data, std::size_t len)
{
    while (h != INVALID_HANDLE_VALUE && h != nullptr && len != 0)
    {
        DWORD chunk = len > 0x1000000u ? 0x1000000u : static_cast<DWORD>(len);
        DWORD written = 0;
        if (!WriteFile(h, data, chunk, &written, nullptr) || written == 0) return;
        data += written;
        len -= written;
    }
}

bool FileSize(const std::wstring& path, unsigned long long* size)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    *size = (static_cast<unsigned long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return true;
}

bool EnsureDir(const std::wstring& dir, int depth = 0)
{
    if (dir.empty() || depth > 32) return false;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) EnsureDir(dir.substr(0, slash), depth + 1);
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// A bare file name: no separators, no drive, no "..".
bool IsLeafName(const std::wstring& n)
{
    if (n.empty() || n.size() > 128) return false;
    if (n.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
    return n != L"." && n != L"..";
}

// Optional owner-editable text files in LocalState: '#' comments and blank
// lines skipped, whitespace trimmed, UTF-8 BOM tolerated.
std::vector<std::string> ReadLines(const std::wstring& path)
{
    std::vector<std::string> out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    std::string text;
    char buf[4096];
    DWORD got = 0;
    while (text.size() < (1u << 20) && ReadFile(h, buf, sizeof buf, &got, nullptr) && got != 0) text.append(buf, got);
    CloseHandle(h);
    std::size_t pos = (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                       static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
                          ? 3
                          : 0;
    while (pos <= text.size())
    {
        std::size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        std::size_t b = line.find_first_not_of(" \t\r");
        std::size_t e = line.find_last_not_of(" \t\r");
        if (b == std::string::npos) continue;
        line = line.substr(b, e - b + 1);
        if (line[0] == '#') continue;
        out.push_back(line);
    }
    return out;
}

std::vector<std::wstring> ListFiles(const std::wstring& dir)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.emplace_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// ---------------------------------------------------------------------------
// trace.log: the core's trace and the harness's own lines share one file
//
// trace.h: "hosts may add a sink". win10xbox::trace::AddSink(fn, ctx) calls
// `fn(line, ctx)` for every enabled trace line (the line already ends in '\n'),
// in addition to stderr (dead in a UWP process) and OutputDebugString. Sinks
// are called under the trace mutex, so TraceSink only does a raw WriteFile
// and must never call back into the trace API. Which channels/classes are
// enabled is WIN10XBOX_DEBUG (Wine syntax, trace.cpp ApplySpec) plus
// Config::trace_default_on; RunSmoke sets "+all" so the per-syscall `relay`
// channel is on. core's Shutdown() clears all sinks, hence the harness lines
// below go straight to the handle and keep working after it.
// ---------------------------------------------------------------------------

HANDLE g_trace = INVALID_HANDLE_VALUE;

void TraceSink(const char* line, void*)
{
    if (line != nullptr) WriteAll(g_trace, line, std::strlen(line));
}

void HLogV(const char* fmt, va_list ap)
{
    if (g_trace == INVALID_HANDLE_VALUE) return;
    char msg[1536];
    int n = std::vsnprintf(msg, sizeof msg, fmt, ap);
    if (n < 0) return;
    if (static_cast<std::size_t>(n) >= sizeof msg) n = static_cast<int>(sizeof msg) - 1;
    SYSTEMTIME st;
    GetSystemTime(&st);
    char head[48];
    int h = std::snprintf(head, sizeof head, "%02u:%02u:%02u.%03u harness: ", static_cast<unsigned>(st.wHour),
                          static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
                          static_cast<unsigned>(st.wMilliseconds));
    std::string line(head, h > 0 ? static_cast<std::size_t>(h) : 0);
    line.append(msg, static_cast<std::size_t>(n));
    line.push_back('\n');
    WriteAll(g_trace, line.data(), line.size());
}

void HLog(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    HLogV(fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------------------
// result.txt: rewritten at every phase boundary, so a process that dies half way
// still tells the owner how far it got ("INCOMPLETE ... phase=...").
// ---------------------------------------------------------------------------

char g_snapshot[8192];            // last text written to result.txt, for the crash filter
std::size_t g_snapshot_len = 0;

class Report
{
public:
    explicit Report(std::wstring path) : path_(std::move(path)) {}

    void Set(const std::string& key, const std::string& value)
    {
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

    void SetVerdict(const std::string& v) { verdict_ = v; }

    void Flush()
    {
        std::string text = "RESULT: " + verdict_ + "\n";
        for (const auto& kv : kv_) text += kv.first + "=" + kv.second + "\n";
        g_snapshot_len = text.size() < sizeof g_snapshot ? text.size() : sizeof g_snapshot;
        std::memcpy(g_snapshot, text.data(), g_snapshot_len);
        HANDLE h = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        WriteAll(h, text.data(), text.size());
        CloseHandle(h);
    }

private:
    std::wstring path_;
    std::string verdict_ = "INCOMPLETE";
    std::vector<std::pair<std::string, std::string>> kv_;
};

// ---------------------------------------------------------------------------
// Host crash recorder. An AppContainer app that dies leaves nothing behind but
// a closed window, so install a last-resort filter that records the exception
// in trace.log and result.txt. No heap use in here (the crash may be inside
// the heap); the guest installs its own filter later, which then reports
// through Wine's err channel into the same trace.log.
// ---------------------------------------------------------------------------

wchar_t g_crash_result_path[1024];
volatile LONG g_in_crash = 0;

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
    WriteAll(g_trace, line, static_cast<std::size_t>(p - line));
    if (g_crash_result_path[0] != 0)
    {
        HANDLE h = CreateFileW(g_crash_result_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
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

// ---------------------------------------------------------------------------
// Staging the machine folder (the guest's C:\) in LocalState
//
//   <LocalState>\machine\machine.json                 repo's machine.default.json (package payload)
//   <LocalState>\machine\Windows\builtin\*.dll        Wine PE builtins, shipped INSIDE the package
//   <LocalState>\machine\Windows\System32\*.nls ...   genuine files read BY NAME from the console's
//                                                     own C:\Windows\System32 (directory listing of
//                                                     System32 is not relied on, DECISIONS.md D17)
//   <LocalState>\machine\Windows\Globalization\Sorting\sortdefault.nls   (from C:\Windows\Globalization)
//   <LocalState>\machine\guest\*.exe                  the guest programs, from the package
//
// ntdll / kernelbase / kernel32 are hard-forced to the Wine builtin by the VFS
// (DECISIONS.md E1), so they are never read from the console: the probe even
// shows C:\Windows\system32\kernel32.dll is not openable there. Every other
// module follows "native,builtin": a genuine copy staged in System32 wins,
// otherwise the Wine builtin is used.
// ---------------------------------------------------------------------------

enum class Copy { Copied, UpToDate, Missing, Failed };

std::vector<char>& CopyBuffer()
{
    static std::vector<char> buf(1u << 20);
    return buf;
}

Copy CopyFileTo(const std::wstring& src, const std::wstring& dst, unsigned long long* bytes, DWORD* err)
{
    *err = 0;
    *bytes = 0;
    HANDLE in = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE)
    {
        *err = GetLastError();
        return (*err == ERROR_FILE_NOT_FOUND || *err == ERROR_PATH_NOT_FOUND) ? Copy::Missing : Copy::Failed;
    }
    LARGE_INTEGER size;
    size.QuadPart = 0;
    if (!GetFileSizeEx(in, &size))
    {
        *err = GetLastError();
        CloseHandle(in);
        return Copy::Failed;
    }
    *bytes = static_cast<unsigned long long>(size.QuadPart);
    unsigned long long have = 0;
    if (*bytes != 0 && FileSize(dst, &have) && have == *bytes)
    {
        CloseHandle(in);
        return Copy::UpToDate;
    }
    const std::wstring tmp = dst + L".part";
    HANDLE out = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE)
    {
        *err = GetLastError();
        CloseHandle(in);
        return Copy::Failed;
    }
    std::vector<char>& buf = CopyBuffer();
    bool ok = true;
    for (;;)
    {
        DWORD got = 0;
        if (!ReadFile(in, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr))
        {
            *err = GetLastError();
            ok = false;
            break;
        }
        if (got == 0) break;
        DWORD put = 0;
        if (!WriteFile(out, buf.data(), got, &put, nullptr) || put != got)
        {
            *err = GetLastError();
            ok = false;
            break;
        }
    }
    CloseHandle(in);
    CloseHandle(out);
    if (ok && !MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        *err = GetLastError();
        ok = false;
    }
    if (!ok)
    {
        DeleteFileW(tmp.c_str());
        return Copy::Failed;
    }
    return Copy::Copied;
}

struct GroupStats
{
    int copied = 0;
    int up_to_date = 0;
    int missing = 0;
    int failed = 0;
    unsigned long long bytes = 0;
    std::vector<std::string> bad;   // "name(missing)" / "name(error N)"

    std::string Summary() const
    {
        return Fmt("copied=%d up-to-date=%d missing=%d failed=%d bytes=%llu", copied, up_to_date, missing, failed,
                   bytes);
    }
    std::string BadList() const
    {
        std::string s;
        for (const auto& b : bad)
        {
            if (!s.empty()) s += ",";
            s += b;
        }
        return s;
    }
};

Copy StageOne(GroupStats& g, const char* group, const std::wstring& src, const std::wstring& dst,
              const std::wstring& label)
{
    unsigned long long bytes = 0;
    DWORD err = 0;
    const Copy c = CopyFileTo(src, dst, &bytes, &err);
    const std::string name = Narrow(label);
    switch (c)
    {
    case Copy::Copied:
        ++g.copied;
        g.bytes += bytes;
        HLog("stage[%s] %s: copied %llu bytes", group, name.c_str(), bytes);
        break;
    case Copy::UpToDate:
        ++g.up_to_date;
        HLog("stage[%s] %s: up to date (%llu bytes)", group, name.c_str(), bytes);
        break;
    case Copy::Missing:
        ++g.missing;
        g.bad.push_back(name + "(missing)");
        HLog("stage[%s] %s: MISSING at source (error %lu) <- %s", group, name.c_str(), static_cast<unsigned long>(err),
             Narrow(src).c_str());
        break;
    case Copy::Failed:
        ++g.failed;
        g.bad.push_back(name + Fmt("(error %lu)", static_cast<unsigned long>(err)));
        HLog("stage[%s] %s: FAILED (error %lu) <- %s", group, name.c_str(), static_cast<unsigned long>(err),
             Narrow(src).c_str());
        break;
    }
    return c;
}

// Genuine NLS data Wine's ntdll needs at locale init (emulated
// NtGetNlsSectionPtr, core/dispatch/nt_nls.cpp). Required = the en-US ANSI/OEM
// code pages plus the data files; the rest is a common-case code page set.
// More names can be added without a rebuild via LocalState\stage_extra.txt.
const wchar_t* const kNlsRequired[] = {
    L"c_1252.nls", L"c_437.nls", L"l_intl.nls",   L"locale.nls",  L"normidna.nls",
    L"normnfc.nls", L"normnfd.nls", L"normnfkc.nls", L"normnfkd.nls",
};
const unsigned kNlsCodePages[] = {
    850, 874, 932, 936, 949, 950, 1250, 1251, 1253, 1254, 1255, 1256, 1257, 1258, 10000, 20127, 28591,
};

// Genuine DLLs staged next to them (kept tiny on purpose): they are what the
// smoke guest's native-DLL probe looks at. Everything else the guest might
// import resolves to the Wine builtin shipped in the package.
const wchar_t* const kNativeDlls[] = {L"ucrtbase.dll", L"msvcrt.dll"};

// Read-only inventory: which of the machine/guest_dlls.txt modules can be
// opened BY NAME in the console's System32 (nothing is copied). The answer is
// exactly the D17 question for the real host-uwp (M5).
const wchar_t* const kInventory[] = {
    L"msvcrt.dll",   L"ucrtbase.dll",   L"vcruntime140.dll", L"vcruntime140_1.dll", L"msvcp140.dll", L"advapi32.dll",
    L"sechost.dll",  L"rpcrt4.dll",     L"cryptbase.dll",    L"bcrypt.dll",         L"bcryptprimitives.dll",
    L"user32.dll",   L"win32u.dll",     L"gdi32.dll",        L"gdi32full.dll",      L"msvcp_win.dll",
    L"imm32.dll",    L"comctl32.dll",   L"comdlg32.dll",     L"shell32.dll",        L"shlwapi.dll",
    L"shcore.dll",   L"windows.storage.dll", L"profapi.dll", L"powrprof.dll",       L"umpdc.dll",
    L"ole32.dll",    L"oleaut32.dll",   L"combase.dll",      L"version.dll",        L"winmm.dll",
    L"ws2_32.dll",   L"nsi.dll",        L"setupapi.dll",     L"cfgmgr32.dll",       L"devobj.dll",
    L"wintrust.dll", L"crypt32.dll",    L"msasn1.dll",       L"userenv.dll",        L"netapi32.dll",
    L"psapi.dll",    L"dbghelp.dll",    L"apisetschema.dll", L"kernel32.dll",       L"kernelbase.dll",
    L"ntdll.dll",
};

const wchar_t* const kMachineDirs[] = {
    L"Windows\\builtin",
    L"Windows\\System32",
    L"Windows\\System32\\config",
    L"Windows\\Globalization\\Sorting",
    L"Windows\\Temp",
    L"Users\\User\\AppData\\Local\\Temp",
    L"Users\\User\\AppData\\Roaming",
    L"Users\\Public",
    L"ProgramData",
    L"Program Files",
    L"Program Files (x86)",
    L"guest",
};

struct StageResult
{
    GroupStats builtin, guest, nls, dll, extra;
    bool have_ntdll = false;
    std::vector<std::string> required_missing;
    int inventory_ok = 0;
    int inventory_total = 0;
    std::string inventory_bad;
};

StageResult StageMachine(const std::wstring& local_state, const std::wstring& machine, const std::wstring& payload)
{
    StageResult r;
    std::wstring sys32(MAX_PATH, L'\0'), windir(MAX_PATH, L'\0');
    UINT n = GetSystemDirectoryW(&sys32[0], MAX_PATH);
    sys32.resize(n > 0 && n < MAX_PATH ? n : 0);
    if (sys32.empty()) sys32 = L"C:\\Windows\\System32";
    n = GetWindowsDirectoryW(&windir[0], MAX_PATH);
    windir.resize(n > 0 && n < MAX_PATH ? n : 0);
    if (windir.empty()) windir = L"C:\\Windows";
    HLog("stage: machine=%s payload=%s", Narrow(machine).c_str(), Narrow(payload).c_str());
    HLog("stage: reading genuine files BY NAME from %s and %s", Narrow(sys32).c_str(), Narrow(windir).c_str());

    EnsureDir(machine);
    for (const wchar_t* d : kMachineDirs) EnsureDir(machine + L"\\" + d);

    // machine.json: the repo's machine.default.json (22H2 defaults); never overwritten
    {
        const std::wstring dst = machine + L"\\machine.json";
        unsigned long long sz = 0;
        if (!FileSize(dst, &sz))
        {
            GroupStats g;
            StageOne(g, "config", payload + L"\\machine.json", dst, L"machine.json");
        }
    }

    // Wine PE builtins (inside the package)
    for (const std::wstring& f : ListFiles(payload + L"\\builtin"))
        StageOne(r.builtin, "builtin", payload + L"\\builtin\\" + f, machine + L"\\Windows\\builtin\\" + f, f);
    {
        unsigned long long sz = 0;
        r.have_ntdll = FileSize(machine + L"\\Windows\\builtin\\ntdll.dll", &sz) && sz != 0;
    }

    // Guest programs (inside the package)
    for (const std::wstring& f : ListFiles(payload + L"\\guest"))
        StageOne(r.guest, "guest", payload + L"\\guest\\" + f, machine + L"\\guest\\" + f, f);

    // Genuine NLS data, by name
    const std::wstring dst_sys32 = machine + L"\\Windows\\System32\\";
    for (const wchar_t* name : kNlsRequired)
    {
        Copy c = StageOne(r.nls, "nls", sys32 + L"\\" + name, dst_sys32 + name, name);
        if (c == Copy::Missing || c == Copy::Failed) r.required_missing.push_back(Narrow(name));
    }
    for (unsigned cp : kNlsCodePages)
    {
        wchar_t name[32];
        std::swprintf(name, 32, L"c_%03u.nls", cp);
        StageOne(r.nls, "nls", sys32 + L"\\" + name, dst_sys32 + name, name);
    }
    {
        // sortdefault.nls lives in C:\Windows\Globalization\Sorting (System32 as fallback, same as the emulation)
        const std::wstring dst = machine + L"\\Windows\\Globalization\\Sorting\\sortdefault.nls";
        Copy c = StageOne(r.nls, "nls", windir + L"\\Globalization\\Sorting\\sortdefault.nls", dst, L"sortdefault.nls");
        if (c == Copy::Missing || c == Copy::Failed)
        {
            c = StageOne(r.nls, "nls", sys32 + L"\\sortdefault.nls", dst_sys32 + L"sortdefault.nls",
                         L"sortdefault.nls(System32)");
            if (c == Copy::Missing || c == Copy::Failed) r.required_missing.push_back("sortdefault.nls");
        }
    }

    // Genuine DLLs, by name
    for (const wchar_t* name : kNativeDlls)
        StageOne(r.dll, "dll", sys32 + L"\\" + name, dst_sys32 + name, name);

    // Owner extras: LocalState\stage_extra.txt, one System32 file name per line
    for (const std::string& line : ReadLines(local_state + L"\\stage_extra.txt"))
    {
        const std::wstring name = Widen(line);
        if (!IsLeafName(name))
        {
            HLog("stage_extra: ignoring '%s' (not a bare file name)", line.c_str());
            continue;
        }
        StageOne(r.extra, "extra", sys32 + L"\\" + name, dst_sys32 + name, name);
    }

    // Read-only inventory of what System32 lets us open by name
    for (const wchar_t* name : kInventory)
    {
        ++r.inventory_total;
        HANDLE h = CreateFileW((sys32 + L"\\" + name).c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER sz;
            sz.QuadPart = 0;
            GetFileSizeEx(h, &sz);
            CloseHandle(h);
            ++r.inventory_ok;
            HLog("inventory: %s: readable by name, %lld bytes", Narrow(name).c_str(),
                 static_cast<long long>(sz.QuadPart));
        }
        else
        {
            const DWORD err = GetLastError();
            if (!r.inventory_bad.empty()) r.inventory_bad += ",";
            r.inventory_bad += Narrow(name) + Fmt("(%lu)", static_cast<unsigned long>(err));
            HLog("inventory: %s: NOT readable by name (error %lu)", Narrow(name).c_str(),
                 static_cast<unsigned long>(err));
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// Environment knobs
// ---------------------------------------------------------------------------

void SetEnvIfUnset(const wchar_t* name, const wchar_t* value)
{
    wchar_t probe[8];
    if (GetEnvironmentVariableW(name, probe, 8) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
        SetEnvironmentVariableW(name, value);
}

// LocalState\smoke.env: NAME=VALUE lines applied to the host environment before
// the core is initialised, e.g. WIN10XBOX_DEBUG=+relay,+loader or
// UWP_LITE_TIMEOUT_SEC=120 (the owner's way to retune a run without a rebuild).
int ApplyEnvFile(const std::wstring& path)
{
    int applied = 0;
    for (const std::string& line : ReadLines(path))
    {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::wstring name = Widen(line.substr(0, eq));
        const std::wstring value = Widen(line.substr(eq + 1));
        if (SetEnvironmentVariableW(name.c_str(), value.c_str()))
        {
            ++applied;
            HLog("smoke.env: %s=%s", line.substr(0, eq).c_str(), line.substr(eq + 1).c_str());
        }
    }
    return applied;
}

DWORD TimeoutMs()
{
    wchar_t b[32];
    DWORD n = GetEnvironmentVariableW(L"UWP_LITE_TIMEOUT_SEC", b, 32);
    unsigned long sec = (n != 0 && n < 32) ? std::wcstoul(b, nullptr, 10) : 0;
    if (sec == 0) sec = kDefaultTimeoutSec;
    if (sec > 7200) sec = 7200;
    return static_cast<DWORD>(sec * 1000ul);
}

HANDLE OpenForGuest(const std::wstring& path, DWORD access, DWORD disposition, DWORD share)
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;
    return CreateFileW(path.c_str(), access, share, &sa, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool FileContains(const std::wstring& path, const char* needle)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string text;
    char buf[4096];
    DWORD got = 0;
    while (text.size() < (1u << 20) && ReadFile(h, buf, sizeof buf, &got, nullptr) && got != 0) text.append(buf, got);
    CloseHandle(h);
    return text.find(needle) != std::string::npos;
}

std::string Hex32(long v)
{
    return Fmt("0x%08lx", static_cast<unsigned long>(v));
}

// ---------------------------------------------------------------------------
// The smoke
// ---------------------------------------------------------------------------

bool RunSmokeImpl(const HostInfo& host, Report& rep)
{
    const std::wstring ls = host.local_state;
    const std::wstring machine = ls + L"\\machine";

    // ---- 0. outputs: trace.log first (so staging is captured), result.txt live
    g_trace = CreateFileW((ls + L"\\trace.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    lstrcpynW(g_crash_result_path, (ls + L"\\result.txt").c_str(), static_cast<int>(sizeof g_crash_result_path / sizeof(wchar_t)));
    SetUnhandledExceptionFilter(&CrashFilter);

    const win10xbox::Version ver = win10xbox::GetVersion();
    HLog("host-uwp-lite (built %s %s) on core %d.%d.%d (%s)", __DATE__, __TIME__, ver.major, ver.minor, ver.patch,
         ver.git ? ver.git : "?");
    HLog("package=%s", host.package_full_name.c_str());
    HLog("local_state=%s", Narrow(ls).c_str());
    HLog("package_dir=%s", Narrow(host.package_dir).c_str());

    rep.Set("tool", "host-uwp-lite (throwaway on-console M1/M2 validator)");
    rep.Set("build", Fmt("%s %s", __DATE__, __TIME__));
    rep.Set("core", Fmt("%d.%d.%d (%s)", ver.major, ver.minor, ver.patch, ver.git ? ver.git : "?"));
    rep.Set("package", host.package_full_name);
    rep.Set("local_state", Narrow(ls));
    rep.SetVerdict("INCOMPLETE (still running, or the process died: see 'phase' below and trace.log)");
    rep.Set("phase", "start");
    rep.Flush();

    // ---- 1. optional owner knobs
    const int env_applied = ApplyEnvFile(ls + L"\\smoke.env");
    // LocalState\guest.txt: "<file in machine\guest> [arguments]", e.g. "smoke_cui.exe" or
    // "smoke.exe load". Default: smoke.exe with no arguments.
    std::wstring guest_name = L"smoke.exe";
    std::wstring guest_args;
    {
        const std::vector<std::string> g = ReadLines(ls + L"\\guest.txt");
        if (!g.empty())
        {
            const std::wstring w = Widen(g[0]);
            const std::size_t sp = w.find_first_of(L" \t");
            const std::wstring nm = sp == std::wstring::npos ? w : w.substr(0, sp);
            std::wstring ar;
            if (sp != std::wstring::npos)
            {
                const std::size_t b = w.find_first_not_of(L" \t", sp);
                if (b != std::wstring::npos) ar = w.substr(b);
            }
            if (IsLeafName(nm))
            {
                guest_name = nm;
                guest_args = ar;
            }
            else
            {
                HLog("guest.txt: ignoring '%s' (the first word must be a bare file name)", g[0].c_str());
            }
        }
    }
    rep.Set("guest", Narrow(guest_name) + (guest_args.empty() ? std::string() : " " + Narrow(guest_args)));
    rep.Set("smoke_env_applied", Fmt("%d", env_applied));

    // ---- 2. stage the machine folder
    rep.Set("phase", "staging");
    rep.Flush();
    StageResult st = StageMachine(ls, machine, host.package_dir + L"\\payload");
    rep.Set("stage.builtin", st.builtin.Summary());
    rep.Set("stage.guest", st.guest.Summary());
    rep.Set("stage.nls", st.nls.Summary());
    rep.Set("stage.dll", st.dll.Summary());
    if (st.extra.copied + st.extra.up_to_date + st.extra.missing + st.extra.failed != 0)
        rep.Set("stage.extra", st.extra.Summary());
    if (!st.nls.bad.empty()) rep.Set("stage.nls.not_staged", st.nls.BadList());
    if (!st.dll.bad.empty()) rep.Set("stage.dll.not_staged", st.dll.BadList());
    rep.Set("builtin.ntdll", st.have_ntdll ? "present" : "MISSING");
    {
        std::string req;
        for (const auto& m : st.required_missing) req += (req.empty() ? "" : ",") + m;
        rep.Set("nls.required_missing", req.empty() ? "none" : req);
    }
    rep.Set("system32.readable_by_name", Fmt("%d/%d", st.inventory_ok, st.inventory_total));
    if (!st.inventory_bad.empty()) rep.Set("system32.not_readable", st.inventory_bad);
    HLog("stage: done. builtin[%s] guest[%s] nls[%s] dll[%s]", st.builtin.Summary().c_str(),
         st.guest.Summary().c_str(), st.nls.Summary().c_str(), st.dll.Summary().c_str());
    rep.Flush();

    // ---- 3. stdout/stderr -> LocalState\stdout.txt, stdin -> an empty file.
    // The guest inherits the HOST's standard handles (BuildProcessParameters in
    // core/dispatch/process.cpp copies them out of the host's process
    // parameters), and SetStdHandle writes exactly those fields. A plain file
    // handle sidesteps the real-Windows pipe-capture problem in docs/STATUS.md.
    const std::wstring stdout_path = ls + L"\\stdout.txt";
    HANDLE h_out = OpenForGuest(stdout_path, GENERIC_WRITE, CREATE_ALWAYS, FILE_SHARE_READ);
    HANDLE h_in = OpenForGuest(ls + L"\\stdin.txt", GENERIC_READ, OPEN_ALWAYS, FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (h_out == INVALID_HANDLE_VALUE)
    {
        HLog("cannot create stdout.txt (error %lu): the guest's output will be lost", GetLastError());
    }
    else
    {
        SetStdHandle(STD_OUTPUT_HANDLE, h_out);
        SetStdHandle(STD_ERROR_HANDLE, h_out);
    }
    if (h_in != INVALID_HANDLE_VALUE) SetStdHandle(STD_INPUT_HANDLE, h_in);

    // ---- 4. tracing: full channels, sink -> trace.log, plus Wine's own loaddll
    // channel so the log says which modules were loaded native vs builtin.
    SetEnvIfUnset(L"WIN10XBOX_DEBUG", L"+all");
    SetEnvIfUnset(L"WIN10XBOX_WINEDEBUG", L"+loaddll,warn+module");
    win10xbox::trace::AddSink(&TraceSink, nullptr);

    // ---- 5. Initialize (config defaults = 22H2 version spoof ON, D18 CPU
    // identity, AuthenticAMD, hypervisor hidden: nothing is switched off here)
    rep.Set("phase", "initialize");
    rep.Flush();
    win10xbox::config::Config cfg;
    cfg.machine_root = machine;
    cfg.trace_default_on = true;
    const int init_rc = win10xbox::Initialize(cfg);
    rep.Set("initialize", Fmt("%d", init_rc));
    HLog("win10xbox::Initialize -> %d", init_rc);

    // ---- 6. Launch. host-win32 uses wait=true; here wait=false + WaitForGuest(timeout)
    // is the same thing with a watchdog (Launch(wait) is just `return WaitForGuest(INFINITE)`).
    int launch_rc = 0;
    int exit_code = -1;
    bool timed_out = false;
    const DWORD timeout_ms = TimeoutMs();
    if (init_rc == 0)
    {
        win10xbox::process::LaunchOptions opts;
        opts.image_path = L"C:\\guest\\" + guest_name;
        opts.command_line = L"\"" + opts.image_path + L"\"" + (guest_args.empty() ? std::wstring() : L" " + guest_args);
        opts.run_on_new_thread = true;   // this worker thread must come back
        opts.wait = false;
        rep.Set("phase", "launching the guest");
        rep.Set("watchdog_sec", Fmt("%lu", static_cast<unsigned long>(timeout_ms / 1000)));
        rep.Flush();
        HLog("process::Launch(%s) ...", Narrow(opts.image_path).c_str());
        launch_rc = win10xbox::process::Launch(opts);
        HLog("process::Launch -> %d (%s)", launch_rc, Hex32(launch_rc).c_str());
        if (launch_rc == 0)
        {
            rep.Set("phase", "guest running");
            rep.Flush();
            const ULONGLONG t0 = GetTickCount64();
            exit_code = win10xbox::process::WaitForGuest(timeout_ms);
            const ULONGLONG waited = GetTickCount64() - t0;
            timed_out = (static_cast<DWORD>(exit_code) == static_cast<DWORD>(STATUS_TIMEOUT)) && waited + 250 >= timeout_ms;
            HLog("guest %s after %llu ms, exit code %d", timed_out ? "TIMED OUT" : "exited",
                 static_cast<unsigned long long>(waited), exit_code);
        }
    }
    else
    {
        launch_rc = init_rc;
    }

    // ---- 7. wind down (not after a timeout: the guest is still running on the PEB)
    if (!timed_out)
    {
        if (init_rc == 0) win10xbox::dispatch::Shutdown();   // dumps the per-syscall relay statistics into trace.log
        win10xbox::Shutdown();                               // also detaches the trace sink
    }
    if (h_out != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(h_out);
        CloseHandle(h_out);
    }
    if (h_in != INVALID_HANDLE_VALUE) CloseHandle(h_in);

    // ---- 8. verdict
    unsigned long long out_bytes = 0;
    FileSize(stdout_path, &out_bytes);
    const bool hello = FileContains(stdout_path, "hello from guest");
    const bool pass = (init_rc == 0) && (launch_rc == 0) && !timed_out && (exit_code == kExpectedGuestExit);

    // exit_code is what process::Launch(wait=true) would have returned (the guest's exit code).
    // launch_status is Launch(wait=false)'s own result: 0 = the guest was started, else the bootstrap NTSTATUS.
    rep.Set("exit_code", launch_rc == 0 && init_rc == 0 ? Fmt("%d", exit_code) : std::string("n/a"));
    rep.Set("expected_exit_code", Fmt("%d", kExpectedGuestExit));
    rep.Set("launch_status", init_rc != 0 ? std::string("not attempted (Initialize failed)")
                                          : Fmt("%d (%s)", launch_rc, Hex32(launch_rc).c_str()));
    rep.Set("stdout_bytes", Fmt("%llu", out_bytes));
    rep.Set("stdout_has_hello", hello ? "yes" : "no");
    rep.Set("phase", "finished");

    std::string hint;
    if (!pass)
    {
        if (!st.have_ntdll)
            hint = "no Wine PE builtins in the package (the CI could not fetch the wine-pe-builtin artifact): copy "
                   "wine-pe/out/builtin/*.dll into LocalState\\machine\\Windows\\builtin and relaunch";
        else if (!st.required_missing.empty())
            hint = "required NLS data could not be read from the console's System32 (see stage[nls] lines in trace.log)";
        else if (init_rc != 0)
            hint = "win10xbox::Initialize failed";
        else if (launch_rc != 0)
            hint = "process::Launch failed before the guest ran; see the 'process' and 'loader' lines in trace.log";
        else if (timed_out)
            hint = "the guest did not exit within the watchdog; the last relay lines in trace.log show where it is stuck";
        else
            hint = "the guest ran but did not exit with 42; read stdout.txt and trace.log, then try guest.txt = smoke_cui.exe "
                   "(console subsystem) or 'smoke.exe min' (no DLL probe)";
    }
    if (!hint.empty()) rep.Set("hint", hint);

    rep.SetVerdict(pass ? "PASS" : (timed_out ? "FAIL (guest watchdog timeout)" : "FAIL"));
    HLog("verdict: %s", pass ? "PASS" : "FAIL");
    rep.Flush();

    if (g_trace != INVALID_HANDLE_VALUE) FlushFileBuffers(g_trace);

    if (timed_out)
    {
        // The guest still owns the PEB (loader lists, process parameters...):
        // do not return into WinRT code, just end the process.
        HLog("terminating the process (guest still running)");
        CloseHandle(g_trace);
        g_trace = INVALID_HANDLE_VALUE;
        TerminateProcess(GetCurrentProcess(), 0x7E57);
    }
    return pass;
}

}  // namespace

bool RunSmoke(const HostInfo& host)
{
    if (host.local_state.empty()) return false;
    Report rep(host.local_state + L"\\result.txt");
    bool pass = false;
    try
    {
        pass = RunSmokeImpl(host, rep);
    }
    catch (const std::exception& e)
    {
        HLog("FATAL: C++ exception in the harness: %s", e.what());
        rep.Set("error", std::string("C++ exception: ") + e.what());
        rep.SetVerdict("FAIL (harness exception)");
        rep.Flush();
    }
    catch (...)
    {
        HLog("FATAL: unknown exception in the harness");
        rep.SetVerdict("FAIL (harness exception)");
        rep.Flush();
    }
    if (g_trace != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(g_trace);
        CloseHandle(g_trace);
        g_trace = INVALID_HANDLE_VALUE;
    }
    return pass;
}

}  // namespace uwp_lite
