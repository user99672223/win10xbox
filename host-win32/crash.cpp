// crash.cpp - crash.txt / crash.dmp / entry-reached.txt and the host module report.
// See crash.h for what each file is and why it exists.
//
// Rules for everything that can run inside an exception handler (the records, the
// dump request, the entry marker):
//   * raw Win32 only: no CRT formatting, no heap, no std::string. The crash may be
//     inside the heap, and on a guest thread the TEB shows the guest ntdll's TLS and
//     FLS (win10xbox::dispatch::HostModeScope puts the host's back for the duration);
//   * every read of memory that is not ours goes through SafeCopy (VirtualQuery
//     first), so a wild pointer in an exception parameter or on the stack cannot
//     make the handler fault;
//   * one writer at a time (g_busy): an exception inside a handler, or on another
//     thread while one is being written, is skipped instead of recursing;
//   * the minidump is written by a thread created at startup (before the guest
//     takes over the PEB); the faulting thread waits for it. When the faulting
//     thread is nearly out of stack (a stack overflow, say a runaway recursion
//     through the dispatcher) that thread also formats the record, because it has
//     a stack of its own.
#include "crash.h"

#include "win10xbox/dispatch.h"

#include <dbghelp.h>
#include <delayimp.h>
#include <intrin.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace uwp_lite
{
namespace
{

// ---------------------------------------------------------------------------
// State. Plain arrays, filled at install time (or under g_busy).
// ---------------------------------------------------------------------------

constexpr std::size_t kPathChars = 1024;

wchar_t g_path_crash_txt[kPathChars];
wchar_t g_path_crash_dmp[kPathChars];
wchar_t g_path_entry[kPathChars];
wchar_t g_path_result[kPathChars];
char g_build[160];
HANDLE g_trace = INVALID_HANDLE_VALUE;   // borrowed from the harness

char g_snapshot[8192];                   // latest result.txt text (SetCrashSnapshot)
volatile LONG g_snapshot_len = 0;
char g_host_modules[2048];               // DescribeHostModules() output, for crash.txt

volatile LONG g_installed = 0;
volatile LONG g_busy = 0;                // one record writer at a time
volatile LONG g_uef_done = 0;            // the unhandled-exception filter rewrites result.txt once
volatile LONG g_entry_written = 0;
volatile LONG g_records = 0;             // fatal-looking exceptions written (not counting suppressed ones)
volatile LONG g_foreign_records = 0;     // ... of which on non-guest threads
volatile LONG g_dump_state = 0;          // 0 none yet, 1 being written, 2 written, 3 failed
volatile LONG g_dump_attempted = 0;
volatile LONG g_dump_error = 0;
HRESULT g_dbghelp_hr = E_FAIL;           // result of binding dbghelp.dll at install time

// The crash thread, created at install time (before the guest takes the PEB over). It writes
// the minidump and, when the faulting thread is too low on stack to do it itself (a stack
// overflow), the crash.txt record as well. The faulting thread waits for it.
struct CrashJob
{
    EXCEPTION_POINTERS* ep;
    DWORD tid;          // the faulting thread
    bool unhandled;
    bool guest;         // the faulting thread runs guest code
    bool record;        // build and append the crash.txt record (else: only the dump)
};
HANDLE g_job_go = nullptr;               // crash thread: work available
HANDLE g_job_done = nullptr;             // crash thread: work finished
volatile DWORD g_crash_tid = 0;          // id of the crash thread (its own exceptions are ignored)
CrashJob g_job;

constexpr LONG kMaxRecords = 40;         // crash.txt stops growing after this many records
constexpr LONG kMaxForeignRecords = 8;   // exceptions on threads that never ran guest code

// Last record written, to suppress an exception that repeats at the same place.
volatile std::uint64_t g_last_addr = 0;
volatile DWORD g_last_code = 0;
volatile LONG g_last_repeat = 0;

char g_out_buf[48 * 1024];               // the record under construction (under g_busy)
char g_res_buf[2048];                    // the FAIL text for result.txt (under g_busy)

struct ModEntry
{
    std::uint64_t base;
    std::uint32_t size;
    char name[72];                       // tail of the full path, printable ASCII
};
constexpr unsigned kMaxMods = 48;
ModEntry g_mods[kMaxMods];
unsigned g_nmods = 0;

// ---------------------------------------------------------------------------
// Tiny formatter: appends to a fixed buffer, never overflows, never allocates.
// ---------------------------------------------------------------------------

class Out
{
public:
    Out(char* buf, std::size_t cap) : buf_(buf), cap_(cap) {}

    void Ch(char c)
    {
        if (n_ + 1 < cap_) buf_[n_++] = c;
    }
    void Str(const char* s)
    {
        while (*s) Ch(*s++);
    }
    void Nl() { Ch('\n'); }
    void Hex(unsigned long long v, int digits)
    {
        static const char kDigits[] = "0123456789abcdef";
        for (int i = digits - 1; i >= 0; --i) Ch(kDigits[(v >> (i * 4)) & 0xf]);
    }
    void Addr(unsigned long long v)
    {
        Str("0x");
        Hex(v, 16);
    }
    // 0x-prefixed, no leading zeros.
    void HexMin(unsigned long long v)
    {
        int digits = 1;
        while (digits < 16 && (v >> (digits * 4)) != 0) ++digits;
        Str("0x");
        Hex(v, digits);
    }
    void Hex32(unsigned long v)
    {
        Str("0x");
        Hex(v, 8);
    }
    void Dec(unsigned long long v)
    {
        char t[24];
        int i = 0;
        do
        {
            t[i++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v != 0);
        while (i > 0) Ch(t[--i]);
    }
    void Dec2(unsigned v)
    {
        Ch(static_cast<char>('0' + (v / 10) % 10));
        Ch(static_cast<char>('0' + v % 10));
    }
    // Printable ASCII only; anything else becomes '?'. Stops at NUL or after `max` chars.
    void Ascii(const char* s, std::size_t max)
    {
        for (std::size_t i = 0; i < max && s[i] != 0; ++i) Ch(s[i] >= 0x20 && s[i] < 0x7f ? s[i] : '?');
    }
    // One line of text: printable ASCII up to the first CR, LF or NUL.
    void Line(const char* s, std::size_t max)
    {
        for (std::size_t i = 0; i < max && s[i] != 0 && s[i] != '\n' && s[i] != '\r'; ++i)
            Ch(s[i] >= 0x20 && s[i] < 0x7f ? s[i] : '?');
    }
    void Wide(const wchar_t* w, std::size_t count)
    {
        for (std::size_t i = 0; i < count && w[i] != 0; ++i) Ch(w[i] >= 0x20 && w[i] < 0x7f ? static_cast<char>(w[i]) : '?');
    }
    void Time()
    {
        SYSTEMTIME st;
        GetSystemTime(&st);
        Dec(st.wYear);
        Ch('-');
        Dec2(st.wMonth);
        Ch('-');
        Dec2(st.wDay);
        Ch(' ');
        Dec2(st.wHour);
        Ch(':');
        Dec2(st.wMinute);
        Ch(':');
        Dec2(st.wSecond);
        Ch('.');
        Dec(st.wMilliseconds / 100);
        Dec2(st.wMilliseconds % 100);
        Str(" UTC");
    }

    const char* data() const { return buf_; }
    std::size_t size() const { return n_; }

private:
    char* buf_;
    std::size_t cap_;
    std::size_t n_ = 0;
};

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

// Appends to (or creates) a file without any of the CRT. Best effort.
void AppendFile(const wchar_t* path, const char* data, std::size_t len)
{
    if (path[0] == 0) return;
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteAll(h, data, len);
    FlushFileBuffers(h);
    CloseHandle(h);
}

void ReplaceFile(const wchar_t* path, const char* data, std::size_t len)
{
    if (path[0] == 0) return;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteAll(h, data, len);
    FlushFileBuffers(h);
    CloseHandle(h);
}

bool Contains(const char* hay, const char* needle)
{
    const std::size_t n = std::strlen(needle);
    for (; *hay != 0; ++hay)
        if (*hay == *needle && std::strncmp(hay, needle, n) == 0) return true;
    return false;
}

std::string Fmt(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return std::string();
    if (static_cast<std::size_t>(n) >= sizeof buf) n = static_cast<int>(sizeof buf) - 1;
    return std::string(buf, static_cast<std::size_t>(n));
}

// ---------------------------------------------------------------------------
// Reading memory that may be garbage
// ---------------------------------------------------------------------------

bool ReadableProtect(DWORD protect)
{
    if (protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    switch (protect & 0xff)
    {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool ExecutableProtect(DWORD protect)
{
    switch (protect & 0xff)
    {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

// Copies up to n bytes, stopping at the first page that is not committed and
// readable. Returns the number of bytes copied.
std::size_t SafeCopy(const void* src, void* dst, std::size_t n)
{
    std::size_t done = 0;
    while (done < n)
    {
        const unsigned char* p = static_cast<const unsigned char*>(src) + done;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof mbi) == 0) break;
        if (mbi.State != MEM_COMMIT || !ReadableProtect(mbi.Protect)) break;
        const std::size_t avail =
            static_cast<std::size_t>(static_cast<const unsigned char*>(mbi.BaseAddress) + mbi.RegionSize - p);
        const std::size_t take = (n - done) < avail ? (n - done) : avail;
        std::memcpy(static_cast<unsigned char*>(dst) + done, p, take);
        done += take;
    }
    return done;
}

// ---------------------------------------------------------------------------
// What an address is
// ---------------------------------------------------------------------------

const char* CodeName(DWORD code)
{
    switch (code)
    {
    case 0xC0000005: return "EXCEPTION_ACCESS_VIOLATION";
    case 0xC0000006: return "EXCEPTION_IN_PAGE_ERROR";
    case 0xC000001D: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case 0xC0000025: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case 0xC0000026: return "EXCEPTION_INVALID_DISPOSITION";
    case 0xC000008C: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case 0xC0000094: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case 0xC0000095: return "EXCEPTION_INT_OVERFLOW";
    case 0xC0000096: return "EXCEPTION_PRIV_INSTRUCTION";
    case 0xC00000FD: return "EXCEPTION_STACK_OVERFLOW";
    case 0xC0000135: return "STATUS_DLL_NOT_FOUND";
    case 0xC0000139: return "STATUS_ENTRYPOINT_NOT_FOUND";
    case 0xC0000142: return "STATUS_DLL_INIT_FAILED";
    case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
    case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN (fail-fast)";
    case 0xC0000417: return "STATUS_INVALID_CRUNTIME_PARAMETER";
    case 0xC0000604: return "STATUS_DYNAMIC_CODE_BLOCKED";
    case 0x80000001: return "EXCEPTION_GUARD_PAGE";
    case 0x80000002: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case 0x80000003: return "EXCEPTION_BREAKPOINT";
    case 0x80000004: return "EXCEPTION_SINGLE_STEP";
    case 0x80000100: return "EXCEPTION_WINE_STUB (a call through an import Wine could not resolve)";
    case 0x80000101: return "EXCEPTION_WINE_ASSERTION";
    case 0x40010006: return "DBG_PRINTEXCEPTION_C";
    case 0x4001000A: return "DBG_PRINTEXCEPTION_WIDE_C";
    case 0x406D1388: return "MS_VC_EXCEPTION (thread name)";
    case 0xE06D7363: return "C++ exception";
    case 0xC06D007E: return "delay-load: module not found";
    case 0xC06D007F: return "delay-load: procedure not found";
    default: return "";
    }
}

// Exceptions that, when they reach this handler, are very probably the end of the
// run. The vectored handler sees first-chance exceptions that a frame-based handler
// may still absorb, so this is a heuristic: records are cheap, the dump is taken once.
bool FatalLooking(DWORD code)
{
    switch (code)
    {
    case 0xC0000005:   // access violation
    case 0xC0000006:   // in-page error
    case 0xC000001D:   // illegal instruction
    case 0xC0000025:   // noncontinuable exception
    case 0xC0000026:   // invalid disposition
    case 0xC000008C:   // array bounds
    case 0xC0000094:   // integer divide by zero
    case 0xC0000096:   // privileged instruction
    case 0xC00000FD:   // stack overflow
    case 0xC0000374:   // heap corruption
    case 0xC0000409:   // fail-fast
    case 0xC0000417:   // invalid CRT parameter
    case 0xC0000604:   // dynamic code blocked
    case 0x80000100:   // Wine: stub called
    case 0x80000101:   // Wine: assertion
    case 0xC06D007E:   // delay-load failures
    case 0xC06D007F:
        return true;
    default:
        return false;
    }
}

// Module list of the process as the loader lists describe it right now: while the
// guest runs this is Wine's list (PEB->Ldr belongs to the guest after its
// loader_init), which is exactly the list that names the guest's modules.
// PEB 0x18 = Ldr; PEB_LDR_DATA 0x10 = InLoadOrderModuleList; LDR_DATA_TABLE_ENTRY:
// 0x30 DllBase, 0x40 SizeOfImage, 0x48 FullDllName (UNICODE_STRING).
void SnapshotModules()
{
    g_nmods = 0;
    const unsigned char* peb = reinterpret_cast<const unsigned char*>(__readgsqword(0x60));
    std::uint64_t ldr = 0;
    if (peb == nullptr || SafeCopy(peb + 0x18, &ldr, 8) != 8 || ldr == 0) return;
    const std::uint64_t head = ldr + 0x10;
    std::uint64_t cur = 0;
    if (SafeCopy(reinterpret_cast<const void*>(head), &cur, 8) != 8) return;
    for (unsigned guard = 0; cur != 0 && cur != head && guard < 96 && g_nmods < kMaxMods; ++guard)
    {
        unsigned char e[0x58];
        if (SafeCopy(reinterpret_cast<const void*>(cur), e, sizeof e) != sizeof e) break;
        ModEntry& m = g_mods[g_nmods];
        m.base = 0;
        m.size = 0;
        m.name[0] = 0;
        std::memcpy(&m.base, e + 0x30, 8);
        std::memcpy(&m.size, e + 0x40, 4);
        USHORT len = 0;
        std::uint64_t buf = 0;
        std::memcpy(&len, e + 0x48, 2);
        std::memcpy(&buf, e + 0x50, 8);
        wchar_t w[160];
        std::size_t chars = len / sizeof(wchar_t);
        if (chars > 159) chars = 159;
        std::size_t got = buf != 0 ? SafeCopy(reinterpret_cast<const void*>(buf), w, chars * sizeof(wchar_t)) / sizeof(wchar_t) : 0;
        // keep the tail of the path
        std::size_t start = got > sizeof m.name - 1 ? got - (sizeof m.name - 1) : 0;
        std::size_t k = 0;
        for (std::size_t i = start; i < got && k < sizeof m.name - 1; ++i)
            m.name[k++] = (w[i] >= 0x20 && w[i] < 0x7f) ? static_cast<char>(w[i]) : '?';
        m.name[k] = 0;
        ++g_nmods;
        std::memcpy(&cur, e, 8);   // Flink
    }
}

// "smoke.exe+0x1a2b", or the memory region the address is in.
void AnnotateAddress(Out& o, std::uint64_t a)
{
    for (unsigned i = 0; i < g_nmods; ++i)
    {
        const ModEntry& m = g_mods[i];
        if (m.base != 0 && a >= m.base && a < m.base + m.size)
        {
            // basename only
            const char* name = m.name;
            for (const char* p = m.name; *p; ++p)
                if (*p == '\\' || *p == '/') name = p + 1;
            o.Ascii(name, 40);
            o.Ch('+');
            o.HexMin(a - m.base);
            return;
        }
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (a == 0 || VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof mbi) == 0)
    {
        o.Str(a == 0 ? "null" : "unmapped");
        return;
    }
    if (mbi.State == MEM_FREE)
    {
        o.Str("free address space");
        return;
    }
    o.Str(mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped" : "private");
    if (mbi.State == MEM_RESERVE)
    {
        o.Str(" reserved");
    }
    else
    {
        const DWORD p = mbi.Protect & 0xff;
        o.Ch(' ');
        o.Str(p == PAGE_NOACCESS ? "---"
              : p == PAGE_READONLY ? "r--"
              : p == PAGE_READWRITE || p == PAGE_WRITECOPY ? "rw-"
              : p == PAGE_EXECUTE ? "--x"
              : p == PAGE_EXECUTE_READ ? "r-x"
              : "rwx");
        if (mbi.Protect & PAGE_GUARD) o.Str(" guard");
    }
    o.Str(" region ");
    o.Addr(reinterpret_cast<std::uintptr_t>(mbi.BaseAddress));
    o.Ch('+');
    o.HexMin(mbi.RegionSize);
}

void ReadCString(Out& o, std::uint64_t p)
{
    char s[96];
    std::size_t got = SafeCopy(reinterpret_cast<const void*>(p), s, sizeof s - 1);
    s[got] = 0;
    if (got == 0)
    {
        o.Str("<unreadable>");
        return;
    }
    o.Ch('"');
    o.Ascii(s, sizeof s - 1);
    o.Ch('"');
}

// ---------------------------------------------------------------------------
// One crash.txt record
// ---------------------------------------------------------------------------

void PutReg(Out& o, const char* name, std::uint64_t v)
{
    o.Str(name);
    o.Ch('=');
    o.Addr(v);
}

void BuildRecord(Out& o, EXCEPTION_POINTERS* ep, DWORD tid, const char* kind, bool guest_thread, bool with_modules)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    SnapshotModules();

    o.Str("================================================================\n");
    o.Str("crash record #");
    o.Dec(static_cast<unsigned long long>(g_records));
    o.Str("   ");
    o.Time();
    o.Nl();
    o.Str(kind);
    o.Nl();
    o.Str("build: ");
    o.Ascii(g_build, sizeof g_build);
    o.Nl();
    o.Str("process 0x");
    o.Hex(GetCurrentProcessId(), 4);
    o.Str("  thread 0x");
    o.Hex(tid, 4);
    o.Str("  runs guest code: ");
    o.Str(guest_thread ? "yes" : "no");
    o.Str("  tick ");
    o.Dec(GetTickCount64());
    o.Nl();

    o.Str("exception: ");
    o.Hex32(er->ExceptionCode);
    const char* name = CodeName(er->ExceptionCode);
    if (*name != 0)
    {
        o.Ch(' ');
        o.Str(name);
    }
    o.Str("   flags 0x");
    o.Hex(er->ExceptionFlags, 1);
    if (er->ExceptionFlags & EXCEPTION_NONCONTINUABLE) o.Str(" (noncontinuable)");
    o.Nl();
    o.Str("  at ");
    o.Addr(reinterpret_cast<std::uintptr_t>(er->ExceptionAddress));
    o.Str("  = ");
    AnnotateAddress(o, reinterpret_cast<std::uintptr_t>(er->ExceptionAddress));
    o.Nl();

    if (er->ExceptionCode == 0xC0000005 || er->ExceptionCode == 0xC0000006)
    {
        if (er->NumberParameters >= 2)
        {
            const ULONG_PTR kind_code = er->ExceptionInformation[0];
            o.Str("  access violation: ");
            o.Str(kind_code == 0 ? "reading" : kind_code == 1 ? "writing" : kind_code == 8 ? "executing (DEP)" : "accessing");
            o.Ch(' ');
            o.Addr(er->ExceptionInformation[1]);
            o.Str("  = ");
            AnnotateAddress(o, er->ExceptionInformation[1]);
            o.Nl();
        }
    }
    else if (er->ExceptionCode == 0x80000100 && er->NumberParameters >= 2)
    {
        // Wine's stub_entry_point(dll, name, ret_addr): the call went through an import
        // that Wine's loader could not resolve (ExceptionAddress is the caller).
        o.Str("  unresolved import: dll ");
        ReadCString(o, er->ExceptionInformation[0]);
        o.Str(" function ");
        ReadCString(o, er->ExceptionInformation[1]);
        o.Str("  (called from ");
        AnnotateAddress(o, reinterpret_cast<std::uintptr_t>(er->ExceptionAddress));
        o.Str(")\n");
    }
    for (DWORD i = 0; i < er->NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i)
    {
        o.Str("  param[");
        o.Dec(i);
        o.Str("] = ");
        o.Addr(er->ExceptionInformation[i]);
        o.Nl();
    }

    if (c != nullptr)
    {
        o.Str("registers:\n  ");
        PutReg(o, "rip", c->Rip);
        o.Str("  ");
        PutReg(o, "rsp", c->Rsp);
        o.Str("  ");
        PutReg(o, "rbp", c->Rbp);
        o.Str("  eflags=0x");
        o.Hex(c->EFlags, 8);
        o.Str("\n  ");
        PutReg(o, "rax", c->Rax);
        o.Str("  ");
        PutReg(o, "rbx", c->Rbx);
        o.Str("  ");
        PutReg(o, "rcx", c->Rcx);
        o.Str("\n  ");
        PutReg(o, "rdx", c->Rdx);
        o.Str("  ");
        PutReg(o, "rsi", c->Rsi);
        o.Str("  ");
        PutReg(o, "rdi", c->Rdi);
        o.Str("\n  ");
        PutReg(o, "r8 ", c->R8);
        o.Str("  ");
        PutReg(o, "r9 ", c->R9);
        o.Str("  ");
        PutReg(o, "r10", c->R10);
        o.Str("\n  ");
        PutReg(o, "r11", c->R11);
        o.Str("  ");
        PutReg(o, "r12", c->R12);
        o.Str("  ");
        PutReg(o, "r13", c->R13);
        o.Str("\n  ");
        PutReg(o, "r14", c->R14);
        o.Str("  ");
        PutReg(o, "r15", c->R15);
        o.Nl();

        unsigned char code[24];
        o.Str("bytes at rip-8 .. rip+16: ");
        std::size_t got = SafeCopy(reinterpret_cast<const void*>(c->Rip - 8), code, sizeof code);
        if (got == sizeof code)
        {
            for (std::size_t i = 0; i < sizeof code; ++i)
            {
                if (i == 8) o.Str("| ");
                o.Hex(code[i], 2);
                o.Ch(' ');
            }
        }
        else
        {
            got = SafeCopy(reinterpret_cast<const void*>(c->Rip), code, 16);
            if (got == 0) o.Str("<unreadable>");
            for (std::size_t i = 0; i < got; ++i)
            {
                o.Hex(code[i], 2);
                o.Ch(' ');
            }
        }
        o.Nl();

        o.Str("stack at rsp (return addresses and pointers into modules or code are annotated):\n");
        std::uint64_t words[32];
        const std::size_t have = SafeCopy(reinterpret_cast<const void*>(c->Rsp), words, sizeof words) / sizeof words[0];
        if (have == 0) o.Str("  <unreadable>\n");
        for (std::size_t i = 0; i < have; ++i)
        {
            o.Str("  [rsp+0x");
            o.Hex(i * 8, 3);
            o.Str("] ");
            o.Addr(words[i]);
            MEMORY_BASIC_INFORMATION mbi;
            bool annotate = false;
            for (unsigned m = 0; m < g_nmods && !annotate; ++m)
                annotate = g_mods[m].base != 0 && words[i] >= g_mods[m].base && words[i] < g_mods[m].base + g_mods[m].size;
            if (!annotate && words[i] > 0x10000 && VirtualQuery(reinterpret_cast<const void*>(words[i]), &mbi, sizeof mbi) != 0)
                annotate = mbi.State == MEM_COMMIT && ExecutableProtect(mbi.Protect);
            if (annotate)
            {
                o.Str("  ");
                AnnotateAddress(o, words[i]);
            }
            o.Nl();
        }
    }

    if (with_modules)
    {
        o.Str("modules in the loader lists of this process (PEB->Ldr, in load order; while the guest runs this is the guest's list):\n");
        if (g_nmods == 0) o.Str("  <none readable>\n");
        for (unsigned i = 0; i < g_nmods; ++i)
        {
            o.Str("  ");
            o.Addr(g_mods[i].base);
            o.Str("  size 0x");
            o.Hex(g_mods[i].size, 8);
            o.Str("  ");
            o.Ascii(g_mods[i].name, sizeof g_mods[i].name);
            o.Nl();
        }
        o.Str("host module paths (GetModuleFileNameW, taken when the harness started):\n");
        o.Str(g_host_modules[0] != 0 ? g_host_modules : "  <not recorded>\n");
        const LONG snap = g_snapshot_len;
        if (snap > 0)
        {
            o.Str("result.txt as the harness last wrote it:\n");
            for (LONG i = 0; i < snap && i < static_cast<LONG>(sizeof g_snapshot); ++i)
            {
                if (i == 0 || g_snapshot[i - 1] == '\n') o.Str("  | ");
                o.Ch(g_snapshot[i]);
            }
            o.Nl();
        }
    }
    o.Nl();
}

// The one-line summary in trace.log (the same shape the old crash filter wrote).
void WriteTraceLine(const char* prefix, EXCEPTION_POINTERS* ep, DWORD tid)
{
    if (g_trace == INVALID_HANDLE_VALUE) return;
    char line[512];
    Out o(line, sizeof line);
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    o.Str(prefix);
    o.Str(" code=0x");
    o.Hex(er->ExceptionCode, 8);
    o.Str(" address=0x");
    o.Hex(reinterpret_cast<std::uintptr_t>(er->ExceptionAddress), 16);
    o.Str(" thread=0x");
    o.Hex(tid, 4);
    if (er->NumberParameters >= 2)
    {
        o.Str(" info0=0x");
        o.Hex(er->ExceptionInformation[0], 16);
        o.Str(" info1=0x");
        o.Hex(er->ExceptionInformation[1], 16);
    }
    if (const CONTEXT* c = ep->ContextRecord)
    {
        o.Str(" rip=0x");
        o.Hex(c->Rip, 16);
        o.Str(" rsp=0x");
        o.Hex(c->Rsp, 16);
        o.Str(" rbp=0x");
        o.Hex(c->Rbp, 16);
        o.Str(" rax=0x");
        o.Hex(c->Rax, 16);
        o.Str(" rcx=0x");
        o.Hex(c->Rcx, 16);
        o.Str(" rdx=0x");
        o.Hex(c->Rdx, 16);
    }
    o.Str(" (details in crash.txt)\n");
    WriteAll(g_trace, o.data(), o.size());
}

// ---------------------------------------------------------------------------
// The minidump: a thread created at startup does the work
// ---------------------------------------------------------------------------

// Binds the delay-loaded dbghelp.dll (CMake links dbghelp.lib with /DELAYLOAD) while
// the process is still in its normal state. A hard import would stop the whole
// package from starting if the console could not load dbghelp.dll in the AppContainer;
// this way the failure is a line in trace.log and no crash.dmp. POD locals only, so
// __try is allowed.
HRESULT BindDbghelp()
{
    HRESULT hr = E_FAIL;
    __try
    {
        hr = __HrLoadAllImportsForDll("dbghelp.dll");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        hr = HRESULT_FROM_WIN32(GetExceptionCode());
    }
    return hr;
}

// MiniDumpWriteDump under SEH (dbghelp walks foreign memory). POD locals only.
BOOL CallMiniDump(HANDLE file, MINIDUMP_TYPE type, MINIDUMP_EXCEPTION_INFORMATION* mei, DWORD* seh_code)
{
    BOOL ok = FALSE;
    __try
    {
        ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, mei, nullptr, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *seh_code = GetExceptionCode();
        ok = FALSE;
    }
    return ok;
}

void WriteMiniDump(EXCEPTION_POINTERS* ep, DWORD faulting_tid)
{
    InterlockedExchange(&g_dump_attempted, 1);
    HANDLE f = CreateFileW(g_path_crash_dmp, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        InterlockedExchange(&g_dump_error, static_cast<LONG>(GetLastError()));
        InterlockedExchange(&g_dump_state, 3);
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = faulting_tid;
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    // Stacks, thread and module lists, the data segments and every private read-write
    // page (heaps, the guest's .data/.bss, its stacks): the guest's code pages are
    // private RX memory that is not included, but the images are on disk in
    // LocalState\machine and crash.txt lists their bases.
    const MINIDUMP_TYPE rich = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithPrivateReadWriteMemory |
                                                          MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                                          MiniDumpWithFullMemoryInfo | MiniDumpIgnoreInaccessibleMemory);
    DWORD seh = 0;
    BOOL ok = CallMiniDump(f, rich, &mei, &seh);
    DWORD err = ok ? 0 : (seh != 0 ? seh : GetLastError());
    if (!ok)
    {
        // Start over with the smallest dump that is still useful.
        SetFilePointer(f, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(f);
        seh = 0;
        ok = CallMiniDump(f, static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo), &mei, &seh);
        if (!ok && err == 0) err = seh != 0 ? seh : GetLastError();
    }
    FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok)
    {
        DeleteFileW(g_path_crash_dmp);
        InterlockedExchange(&g_dump_error, static_cast<LONG>(err));
        InterlockedExchange(&g_dump_state, 3);
    }
    else
    {
        InterlockedExchange(&g_dump_state, 2);
    }
}

bool Enter() { return InterlockedCompareExchange(&g_busy, 1, 0) == 0; }
void Leave() { InterlockedExchange(&g_busy, 0); }

// Takes the process dump once, on the calling thread (which must be the crash thread:
// MiniDumpWriteDump suspends every other thread). Returns true if crash.dmp exists.
bool TakeDump(EXCEPTION_POINTERS* ep, DWORD tid)
{
    if (FAILED(g_dbghelp_hr)) return false;
    if (InterlockedCompareExchange(&g_dump_state, 1, 0) != 0) return g_dump_state == 2;   // one dump per run
    WriteMiniDump(ep, tid);
    return g_dump_state == 2;
}

// result.txt is what the owner reads first: after an unhandled exception it says so (once),
// followed by the harness's last text.
void WriteResultFail(EXCEPTION_POINTERS* ep, DWORD tid)
{
    if (g_path_result[0] == 0 || InterlockedExchange(&g_uef_done, 1) != 0) return;
    Out o(g_res_buf, sizeof g_res_buf);
    o.Str("RESULT: FAIL (the host process crashed)\n");
    o.Str("exception=0x");
    o.Hex(ep->ExceptionRecord->ExceptionCode, 8);
    o.Str(" address=0x");
    o.Hex(reinterpret_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionAddress), 16);
    o.Str(" thread=0x");
    o.Hex(tid, 4);
    o.Str("\ncrash records: crash.txt; a minidump is written to crash.dmp (if it exists, it is complete)\n");
    o.Str("--- state before the crash ---\n");
    const LONG snap = g_snapshot_len;
    ReplaceFile(g_path_result, o.data(), o.size());
    if (snap > 0) AppendFile(g_path_result, g_snapshot, static_cast<std::size_t>(snap));
}

// What the crash thread does with a job posted by a faulting thread.
void RecordCore(EXCEPTION_POINTERS* ep, DWORD tid, bool unhandled, bool guest, bool on_crash_thread);

DWORD WINAPI CrashThreadProc(void*)
{
    for (;;)
    {
        if (WaitForSingleObject(g_job_go, INFINITE) != WAIT_OBJECT_0) return 0;
        const CrashJob j = g_job;
        if (j.record) RecordCore(j.ep, j.tid, j.unhandled, j.guest, /*on_crash_thread=*/true);
        else TakeDump(j.ep, j.tid);
        SetEvent(g_job_done);
    }
}

// Hands a job to the crash thread and waits for it. The dump suspends every other thread,
// this one included, so waiting is all this thread has to do.
bool PostJob(const CrashJob& j)
{
    if (g_job_go == nullptr || g_job_done == nullptr || g_crash_tid == 0) return false;
    g_job = j;
    ResetEvent(g_job_done);
    SetEvent(g_job_go);
    if (WaitForSingleObject(g_job_done, 180000) != WAIT_OBJECT_0)
    {
        InterlockedExchange(&g_dump_error, static_cast<LONG>(WAIT_TIMEOUT));
        return false;
    }
    return true;
}

// Is there too little stack left to format a record on this thread? (TEB.DeallocationStack
// is the bottom of the reserved stack; the region below it plus the guard pages is what a
// stack overflow runs into, so what is left of the reservation is the real headroom.)
bool LowOnStack()
{
    const std::uint64_t sp = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
    const std::uint64_t bottom = __readgsqword(0x1478);   // TEB.DeallocationStack
    return bottom == 0 || sp < bottom || sp - bottom < 0x14000;   // under 80 KiB
}

// The record for one fatal-looking exception: dedupe and caps, the crash.txt text, the one-line
// trace.log summary, then (once per run) the minidump. The record reaches disk BEFORE the dump
// starts (the dump can take seconds and the process may die during it).
void RecordCore(EXCEPTION_POINTERS* ep, DWORD tid, bool unhandled, bool guest, bool on_crash_thread)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;

    // The same exception at the same place again and again (a loop re-raising it): stop after 3.
    const std::uint64_t addr = reinterpret_cast<std::uintptr_t>(er->ExceptionAddress);
    if (!unhandled && addr == g_last_addr && er->ExceptionCode == g_last_code)
    {
        if (InterlockedIncrement(&g_last_repeat) > 3) return;
    }
    else
    {
        g_last_addr = addr;
        g_last_code = er->ExceptionCode;
        InterlockedExchange(&g_last_repeat, 0);
    }

    if (!unhandled)
    {
        if (g_records >= kMaxRecords) return;
        if (!guest && InterlockedIncrement(&g_foreign_records) > kMaxForeignRecords) return;
    }
    InterlockedIncrement(&g_records);

    Out o(g_out_buf, sizeof g_out_buf);
    BuildRecord(o, ep, tid,
                unhandled ? "kind: UNHANDLED - the top-level unhandled-exception filter; the process is about to be terminated"
                          : "kind: first-chance, seen by the last vectored handler (a frame-based handler, e.g. the guest's own SEH, may still handle it)",
                guest, /*with_modules=*/unhandled || g_records <= 3);
    AppendFile(g_path_crash_txt, o.data(), o.size());
    WriteTraceLine(unhandled ? "CRASH: host unhandled exception" : "FATAL-LOOKING first-chance exception", ep, tid);
    if (unhandled) WriteResultFail(ep, tid);

    // The dump: once, for the first fatal-looking exception on a thread that runs guest code
    // (the process may end without the unhandled-exception filter ever running), or for the
    // unhandled exception if nothing was dumped before.
    if (unhandled || guest)
    {
        const bool had_dump = g_dump_state != 0;
        if (on_crash_thread) TakeDump(ep, tid);
        else PostJob(CrashJob{ep, tid, unhandled, guest, /*record=*/false});
        Out d(g_out_buf, sizeof g_out_buf);
        d.Str("minidump for record #");
        d.Dec(static_cast<unsigned long long>(g_records));
        d.Str(": ");
        if (g_dump_state == 2) d.Str(had_dump ? "crash.dmp was taken for an earlier exception" : "written to crash.dmp");
        else if (FAILED(g_dbghelp_hr))
        {
            d.Str("not available, dbghelp.dll could not be bound (HRESULT ");
            d.Hex32(static_cast<unsigned long>(g_dbghelp_hr));
            d.Ch(')');
        }
        else if (g_dump_state == 3)
        {
            d.Str("FAILED, error/code ");
            d.Hex32(static_cast<unsigned long>(g_dump_error));
        }
        else
        {
            d.Str("not taken (still in progress or timed out)");
        }
        d.Str("\n\n");
        AppendFile(g_path_crash_txt, d.data(), d.size());
    }
}

// The entry for both handlers. Cheap work (and the thread-local question) first, on the
// faulting thread; the record itself runs here, or on the crash thread when this thread is
// nearly out of stack.
void Report(EXCEPTION_POINTERS* ep, bool unhandled)
{
    const bool guest = win10xbox::process::IsGuestThread();
    const DWORD tid = GetCurrentThreadId();
    if (LowOnStack() && g_crash_tid != 0)
    {
        PostJob(CrashJob{ep, tid, unhandled, guest, /*record=*/true});
        return;
    }
    // The host's TLS/FLS in the TEB while host code runs on a guest thread.
    win10xbox::dispatch::HostModeScope host;
    RecordCore(ep, tid, unhandled, guest, /*on_crash_thread=*/false);
}

LONG CALLBACK LastChanceVeh(PEXCEPTION_POINTERS ep)
{
    if (ep == nullptr || ep->ExceptionRecord == nullptr || !FatalLooking(ep->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;
    if (GetCurrentThreadId() == g_crash_tid) return EXCEPTION_CONTINUE_SEARCH;   // dbghelp's own probing
    if (!Enter()) return EXCEPTION_CONTINUE_SEARCH;                              // nested, or another thread is writing
    Report(ep, /*unhandled=*/false);
    Leave();
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    if (ep == nullptr || ep->ExceptionRecord == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    if (GetCurrentThreadId() == g_crash_tid) return EXCEPTION_CONTINUE_SEARCH;
    bool wrote = false;
    // Another record may be in flight (the vectored handler on this very exception, if the
    // process is dying right under it): wait a moment instead of skipping the final record.
    for (int i = 0; i < 200 && !wrote; ++i)
    {
        if (Enter())
        {
            Report(ep, /*unhandled=*/true);
            Leave();
            wrote = true;
        }
        else
        {
            Sleep(10);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string InstallCrashCapture(const CrashConfig& cfg, HANDLE trace_log)
{
    if (InterlockedExchange(&g_installed, 1) != 0) return "already installed";

    auto set = [](wchar_t* dst, const std::wstring& dir, const wchar_t* leaf) {
        lstrcpynW(dst, (dir + L"\\" + leaf).c_str(), static_cast<int>(kPathChars));
    };
    set(g_path_crash_txt, cfg.local_state, L"crash.txt");
    set(g_path_crash_dmp, cfg.local_state, L"crash.dmp");
    set(g_path_entry, cfg.local_state, L"entry-reached.txt");
    lstrcpynW(g_path_result, cfg.result_path.c_str(), static_cast<int>(kPathChars));
    std::snprintf(g_build, sizeof g_build, "%s", cfg.build.c_str());
    g_trace = trace_log;

    // Each run replaces the previous run's evidence (trace.log and result.txt are replaced too).
    DeleteFileW(g_path_crash_txt);
    DeleteFileW(g_path_crash_dmp);
    DeleteFileW(g_path_entry);

    // Bind dbghelp now, while the PEB's loader state is the host's own, so the crash
    // path never has to load a DLL.
    g_dbghelp_hr = BindDbghelp();

    // The crash thread: stack of its own for the records of a stack overflow, and the one
    // that writes the minidump. Created even if dbghelp is unavailable.
    std::string thread_state;
    g_job_go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_job_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    {
        DWORD tid = 0;
        HANDLE t = (g_job_go != nullptr && g_job_done != nullptr) ? CreateThread(nullptr, 0, &CrashThreadProc, nullptr, 0, &tid)
                                                                   : nullptr;
        if (t != nullptr)
        {
            g_crash_tid = tid;
            CloseHandle(t);
            thread_state = Fmt("crash thread 0x%04lx ready", static_cast<unsigned long>(tid));
        }
        else
        {
            thread_state = Fmt("crash thread could not be created (error %lu): no minidump, records on the faulting thread only",
                               static_cast<unsigned long>(GetLastError()));
        }
    }
    const std::string dump_state =
        SUCCEEDED(g_dbghelp_hr) && g_crash_tid != 0
            ? "dbghelp.dll bound: crash.dmp enabled"
            : (FAILED(g_dbghelp_hr) ? Fmt("dbghelp.dll could not be bound (HRESULT 0x%08lx): crash.dmp disabled",
                                          static_cast<unsigned long>(g_dbghelp_hr))
                                    : std::string("crash.dmp disabled"));

    // Last in the vectored list (the core's own trap handler goes first), plus the
    // top-level filter for what nothing else handled.
    PVOID veh = AddVectoredExceptionHandler(0, &LastChanceVeh);
    SetUnhandledExceptionFilter(&CrashFilter);
    return std::string("crash capture installed (last-chance vectored handler ") + (veh != nullptr ? "added" : "NOT added") +
           ", unhandled-exception filter set); " + thread_state + "; " + dump_state;
}

void DetachCrashTraceLog()
{
    g_trace = INVALID_HANDLE_VALUE;
}

void SetCrashSnapshot(const char* text, std::size_t len)
{
    if (text == nullptr) return;
    if (len > sizeof g_snapshot) len = sizeof g_snapshot;
    std::memcpy(g_snapshot, text, len);
    InterlockedExchange(&g_snapshot_len, static_cast<LONG>(len));
}

void CrashOnTraceLine(const char* line)
{
    if (line == nullptr || g_entry_written != 0 || !Contains(line, "guest entry reached")) return;
    if (InterlockedExchange(&g_entry_written, 1) != 0) return;
    char buf[3072];
    Out o(buf, sizeof buf);
    o.Str("guest entry reached\n");
    o.Str("written ");
    o.Time();
    o.Str(" by the trace sink, immediately before the core switches to guest code (NtContinue).\n");
    o.Str("If the process dies after this point the guest was running: see crash.txt / crash.dmp, and trace.log for the last syscalls.\n");
    o.Str("process 0x");
    o.Hex(GetCurrentProcessId(), 4);
    o.Str("  thread 0x");
    o.Hex(GetCurrentThreadId(), 4);
    o.Str("  tick ");
    o.Dec(GetTickCount64());
    o.Nl();
    o.Str("trace line: ");
    o.Line(line, 400);
    o.Nl();
    o.Str("host module paths (taken when the harness started):\n");
    o.Str(g_host_modules[0] != 0 ? g_host_modules : "  <not recorded>\n");
    ReplaceFile(g_path_entry, o.data(), o.size());
}

std::vector<std::string> DescribeHostModules()
{
    static const wchar_t* const kNames[] = {L"kernel32.dll", L"user32.dll", L"gdi32full.dll", L"shell32.dll", L"comctl32.dll"};
    std::vector<std::string> lines;
    for (const wchar_t* name : kNames)
    {
        char narrow[32];
        std::size_t k = 0;
        for (const wchar_t* p = name; *p != 0 && k + 1 < sizeof narrow; ++p) narrow[k++] = static_cast<char>(*p);
        narrow[k] = 0;

        SetLastError(0);
        HMODULE h = GetModuleHandleW(name);
        if (h == nullptr)
        {
            lines.push_back(Fmt("module %s: absent (not loaded in this process; GetModuleHandleW error %lu)", narrow,
                                static_cast<unsigned long>(GetLastError())));
            continue;
        }
        wchar_t path[MAX_PATH * 2];
        SetLastError(0);
        const DWORD n = GetModuleFileNameW(h, path, MAX_PATH * 2);
        if (n == 0 || n >= MAX_PATH * 2)
        {
            lines.push_back(Fmt("module %s: loaded at %p, GetModuleFileNameW failed (error %lu)", narrow,
                                static_cast<void*>(h), static_cast<unsigned long>(GetLastError())));
            continue;
        }
        char utf8[MAX_PATH * 4];
        int len = WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(n), utf8, static_cast<int>(sizeof utf8) - 1, nullptr, nullptr);
        if (len < 0) len = 0;
        utf8[len] = 0;
        lines.push_back(Fmt("module %s: %s (base %p)", narrow, utf8, static_cast<void*>(h)));
    }

    // The same text for crash.txt (static buffer: the crash path may not allocate).
    std::size_t pos = 0;
    g_host_modules[0] = 0;
    for (const std::string& l : lines)
    {
        const std::string row = "  " + l + "\n";
        if (pos + row.size() + 1 >= sizeof g_host_modules) break;
        std::memcpy(g_host_modules + pos, row.data(), row.size());
        pos += row.size();
        g_host_modules[pos] = 0;
    }
    return lines;
}

CrashStats GetCrashStats()
{
    CrashStats s;
    s.entry_reached = g_entry_written != 0;
    s.fatal_records = g_records;
    s.dump_written = g_dump_state == 2;
    s.dump_attempted = g_dump_attempted != 0;
    s.dump_error = static_cast<unsigned long>(g_dump_error);
    return s;
}

}  // namespace uwp_lite
