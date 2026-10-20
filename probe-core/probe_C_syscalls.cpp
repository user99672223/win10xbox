// probe_C_syscalls.cpp - category C: the NT syscall surface.
//
// PART 1 resolves a fixed list of native Nt* exports from ntdll.dll through
// probe::resolve() and records each one as OK (addr=...) or MISSING. Nothing is
// called in this part.
//
// PART 2 invokes a deliberately small subset with benign arguments: read-only
// system / process / token queries, plus a reserve+commit and release of one
// page in our own address space. It never creates processes or threads, writes
// files or registry, raises hard errors, or issues device I/O; those calls are
// only resolved in PART 1. Each result is classified by classify().
//
// Every probe goes through Reporter::run, which supplies SEH + try/catch.
// ntdll is reached via probe::resolve, so no link libraries are needed beyond
// the MSVC defaults. All other symbols come from windows.h.

#include "probe.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace probe {
namespace {

// ---------------------------------------------------------------------------
// NTSTATUS values we classify against (ntstatus.h). Unsigned so they work as
// switch labels against the uint32_t result of each call.
using NtStatusU = std::uint32_t;
constexpr NtStatusU kStatusSuccess            = 0x00000000u;
constexpr NtStatusU kStatusNotImplemented     = 0xC0000002u;
constexpr NtStatusU kStatusInvalidHandle      = 0xC0000008u;
constexpr NtStatusU kStatusInvalidParameter   = 0xC000000Du;
constexpr NtStatusU kStatusInfoLengthMismatch = 0xC0000004u;
constexpr NtStatusU kStatusBufferTooSmall     = 0xC0000023u;
constexpr NtStatusU kStatusAccessDenied       = 0xC0000022u;
constexpr NtStatusU kStatusNotSupported       = 0xC00000BBu;
constexpr NtStatusU kStatusPrivilegeNotHeld   = 0xC0000061u;

// Map an NTSTATUS onto a probe Status.
//   0, BUFFER_TOO_SMALL, INFO_LENGTH_MISMATCH -> OK     (reached kernel, worked or validated)
//   NOT_IMPLEMENTED, NOT_SUPPORTED            -> NOTIMPL
//   ACCESS_DENIED, PRIVILEGE_NOT_HELD         -> DENIED
//   anything else                             -> INFO   (reached kernel, expected error)
Status classify(NtStatusU st) {
    switch (st) {
        case kStatusSuccess:
        case kStatusBufferTooSmall:
        case kStatusInfoLengthMismatch:
            return Status::OK;
        case kStatusNotImplemented:
        case kStatusNotSupported:
            return Status::NOTIMPL;
        case kStatusAccessDenied:
        case kStatusPrivilegeNotHeld:
            return Status::DENIED;
        default:
            return Status::INFO;
    }
}

std::string hex(unsigned long long v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llX", v);
    return b;
}

// Fill a Record from an NT call result: status, errorcode, value, detail.
// 'extra' (optional) is appended to value after the status token.
void finish(Record& rec, NtStatusU st, const std::string& what,
            const std::string& extra = std::string()) {
    char b[32];
    std::snprintf(b, sizeof b, "status=0x%08X", static_cast<unsigned>(st));
    rec.status    = classify(st);
    rec.errorcode = static_cast<uint32_t>(st);
    rec.value     = b;
    if (!extra.empty()) rec.value += " " + extra;
    rec.detail    = what + ": " + error_text(static_cast<uint32_t>(st));
}

void set_missing(Record& rec, const char* what) {
    rec.status = Status::MISSING;
    rec.detail = std::string(what) + " not exported by ntdll.dll";
}

void set_skipped(Record& rec, const char* why) {
    rec.status = Status::SKIPPED;
    rec.detail = why;
}

// ---------------------------------------------------------------------------
// Function-pointer types for the Nt* exports we call. The NTSTATUS return is
// spelled LONG (same type) so nothing depends on ntdef.h.
using PFN_NtQuerySystemInformation  = LONG (__stdcall*)(ULONG, PVOID, ULONG, ULONG*);
using PFN_NtQueryInformationProcess = LONG (__stdcall*)(HANDLE, ULONG, PVOID, ULONG, ULONG*);
using PFN_NtOpenProcessToken        = LONG (__stdcall*)(HANDLE, ACCESS_MASK, HANDLE*);
using PFN_NtQueryInformationToken   = LONG (__stdcall*)(HANDLE, ULONG, PVOID, ULONG, ULONG*);
using PFN_NtClose                   = LONG (__stdcall*)(HANDLE);
using PFN_NtAllocateVirtualMemory   = LONG (__stdcall*)(HANDLE, PVOID*, ULONG_PTR, SIZE_T*, ULONG, ULONG);
using PFN_NtFreeVirtualMemory       = LONG (__stdcall*)(HANDLE, PVOID*, SIZE_T*, ULONG);
using PFN_NtQueryVirtualMemory      = LONG (__stdcall*)(HANDLE, PVOID, ULONG, PVOID, SIZE_T, SIZE_T*);

template <class Fn>
Fn resolve_nt(const char* name) {
    return reinterpret_cast<Fn>(probe::resolve("ntdll.dll", name));
}

// SYSTEM_BASIC_INFORMATION (SystemInformationClass 0). The kernel requires this
// exact size for class 0, so the struct must match the real layout.
struct SysBasicInfo {
    ULONG     Reserved;
    ULONG     TimerResolution;
    ULONG     PageSize;
    ULONG     NumberOfPhysicalPages;
    ULONG     LowestPhysicalPageNumber;
    ULONG     HighestPhysicalPageNumber;
    ULONG     AllocationGranularity;
    ULONG_PTR MinimumUserModeAddress;
    ULONG_PTR MaximumUserModeAddress;
    ULONG_PTR ActiveProcessorsAffinityMask;
    char      NumberOfProcessors;
};
static_assert(sizeof(SysBasicInfo) == (sizeof(void*) == 8 ? 64 : 44),
              "SYSTEM_BASIC_INFORMATION layout");

// PROCESS_BASIC_INFORMATION (ProcessInformationClass 0), declared locally.
struct ProcBasicInfo {
    LONG      ExitStatus;
    PVOID     PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG      BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
};
static_assert(sizeof(ProcBasicInfo) == 6 * sizeof(void*),
              "PROCESS_BASIC_INFORMATION layout");

// ---------------------------------------------------------------------------
// PART 1: resolution only.
void register_resolution(Reporter& r) {
    static const char* const kNames[] = {
        "NtCreateUserProcess", "NtCreateProcessEx", "NtCreateThreadEx",
        "NtCreateSection", "NtMapViewOfSection", "NtUnmapViewOfSection",
        "NtProtectVirtualMemory", "NtAllocateVirtualMemory", "NtFreeVirtualMemory",
        "NtReadVirtualMemory", "NtWriteVirtualMemory",
        "NtCreateFile", "NtOpenFile", "NtReadFile",
        "NtQueryInformationFile", "NtQueryDirectoryFileEx",
        "NtOpenKey", "NtCreateKey", "NtQueryValueKey", "NtSetValueKey",
        "NtQuerySystemInformation", "NtQueryInformationProcess",
        "NtOpenProcessToken", "NtQueryInformationToken",
        "NtClose", "NtQueryVirtualMemory", "NtDeviceIoControlFile",
        "NtRaiseHardError", "NtSetInformationProcess",
    };

    for (const char* name : kNames) {
        r.run("C", std::string("resolve:") + name, [name](Record& rec) {
            void* p = probe::resolve("ntdll.dll", name);
            if (!p) {
                set_missing(rec, name);
                return;
            }
            rec.status = Status::OK;
            rec.value  = "addr=" + hex(reinterpret_cast<std::uintptr_t>(p));
            rec.detail = std::string(name) + " exported by ntdll.dll";
        });
    }
}

// ---------------------------------------------------------------------------
// PART 2: a small set of benign calls, each classified.
void run_safe_calls(Reporter& r) {
    // (a) System information.
    r.run("C", "call:NtQuerySystemInformation", [](Record& rec) {
        auto fn = resolve_nt<PFN_NtQuerySystemInformation>("NtQuerySystemInformation");
        if (!fn) { set_missing(rec, "NtQuerySystemInformation"); return; }

        SysBasicInfo sbi{};
        ULONG ret = 0;
        const NtStatusU st = static_cast<NtStatusU>(
            fn(0 /*SystemBasicInformation*/, &sbi, static_cast<ULONG>(sizeof sbi), &ret));

        std::string extra;
        if (st == kStatusSuccess) {
            extra = "pagesize=" + hex(sbi.PageSize) +
                    " granularity=" + hex(sbi.AllocationGranularity) +
                    " procs=" + std::to_string(static_cast<unsigned>(
                                    static_cast<unsigned char>(sbi.NumberOfProcessors))) +
                    " user_max=" + hex(sbi.MaximumUserModeAddress);
        }
        finish(rec, st, "NtQuerySystemInformation(SystemBasicInformation)", extra);
    });

    r.run("C", "call:NtQuerySystemInformation/SystemProcessInformation-len0", [](Record& rec) {
        auto fn = resolve_nt<PFN_NtQuerySystemInformation>("NtQuerySystemInformation");
        if (!fn) { set_missing(rec, "NtQuerySystemInformation"); return; }

        // NULL buffer, zero length: expect INFO_LENGTH_MISMATCH with the needed size.
        ULONG ret = 0;
        const NtStatusU st = static_cast<NtStatusU>(
            fn(5 /*SystemProcessInformation*/, nullptr, 0, &ret));
        finish(rec, st, "NtQuerySystemInformation(SystemProcessInformation, NULL/0)",
               "needed=" + std::to_string(ret));
    });

    r.run("C", "call:NtQuerySystemInformation/SystemKernelDebuggerInformation", [](Record& rec) {
        auto fn = resolve_nt<PFN_NtQuerySystemInformation>("NtQuerySystemInformation");
        if (!fn) { set_missing(rec, "NtQuerySystemInformation"); return; }

        // SYSTEM_KERNEL_DEBUGGER_INFORMATION is two BOOLEANs; the kernel wants exactly 2 bytes.
        unsigned char kd[2] = {0, 0};
        ULONG ret = 0;
        const NtStatusU st = static_cast<NtStatusU>(
            fn(35 /*SystemKernelDebuggerInformation*/, kd, static_cast<ULONG>(sizeof kd), &ret));

        std::string extra;
        if (st == kStatusSuccess) {
            char b[16];
            std::snprintf(b, sizeof b, "bytes=%02X%02X",
                          static_cast<unsigned>(kd[0]), static_cast<unsigned>(kd[1]));
            extra = b;
        }
        finish(rec, st, "NtQuerySystemInformation(SystemKernelDebuggerInformation)", extra);
    });

    // (b) Process information for our own process.
    r.run("C", "call:NtQueryInformationProcess", [](Record& rec) {
        auto fn = resolve_nt<PFN_NtQueryInformationProcess>("NtQueryInformationProcess");
        if (!fn) { set_missing(rec, "NtQueryInformationProcess"); return; }

        ProcBasicInfo pbi{};
        ULONG ret = 0;
        const NtStatusU st = static_cast<NtStatusU>(
            fn(GetCurrentProcess(), 0 /*ProcessBasicInformation*/, &pbi,
               static_cast<ULONG>(sizeof pbi), &ret));

        std::string extra;
        if (st == kStatusSuccess) {
            extra = "pid=" + hex(pbi.UniqueProcessId) +
                    " ppid=" + hex(pbi.InheritedFromUniqueProcessId);
        }
        finish(rec, st, "NtQueryInformationProcess(ProcessBasicInformation, self)", extra);
    });

    // (c) Open our process token, then close it. The handle is shared between the two probes.
    HANDLE tok = nullptr;
    r.run("C", "call:NtOpenProcessToken", [&tok](Record& rec) {
        auto fn = resolve_nt<PFN_NtOpenProcessToken>("NtOpenProcessToken");
        if (!fn) { set_missing(rec, "NtOpenProcessToken"); return; }

        HANDLE h = nullptr;
        const NtStatusU st = static_cast<NtStatusU>(fn(GetCurrentProcess(), TOKEN_QUERY, &h));
        if (st == kStatusSuccess) tok = h;
        finish(rec, st, "NtOpenProcessToken(self, TOKEN_QUERY)",
               std::string("opened=") + (tok ? "yes" : "no"));
    });

    r.run("C", "call:NtClose", [&tok](Record& rec) {
        if (!tok) { set_skipped(rec, "no token handle from NtOpenProcessToken"); return; }
        auto fn = resolve_nt<PFN_NtClose>("NtClose");
        if (!fn) { set_missing(rec, "NtClose"); return; }

        const NtStatusU st = static_cast<NtStatusU>(fn(tok));
        if (st == kStatusSuccess) tok = nullptr;
        finish(rec, st, "NtClose(token handle from NtOpenProcessToken)");
    });

    // (d) Token statistics. Opens its own token, queries TokenStatistics (class 10), closes it.
    r.run("C", "call:NtQueryInformationToken", [](Record& rec) {
        auto fOpen  = resolve_nt<PFN_NtOpenProcessToken>("NtOpenProcessToken");
        auto fQuery = resolve_nt<PFN_NtQueryInformationToken>("NtQueryInformationToken");
        auto fClose = resolve_nt<PFN_NtClose>("NtClose");
        if (!fOpen)  { set_missing(rec, "NtOpenProcessToken"); return; }
        if (!fQuery) { set_missing(rec, "NtQueryInformationToken"); return; }
        if (!fClose) { set_missing(rec, "NtClose"); return; }

        HANDLE h = nullptr;
        const NtStatusU so = static_cast<NtStatusU>(fOpen(GetCurrentProcess(), TOKEN_QUERY, &h));
        if (so != kStatusSuccess) {
            finish(rec, so, "NtOpenProcessToken (prerequisite for TokenStatistics query)");
            return;
        }

        // TOKEN_STATISTICS is 56 bytes on x64. Buffer is larger so the length is never short.
        unsigned char buf[128] = {};
        ULONG ret = 0;
        const NtStatusU st = static_cast<NtStatusU>(
            fQuery(h, 10 /*TokenStatistics*/, buf, static_cast<ULONG>(sizeof buf), &ret));

        std::string extra;
        if (st == kStatusSuccess && ret >= 32) {
            // TOKEN_STATISTICS: LUID TokenId(8), LUID AuthenticationId(8),
            // LARGE_INTEGER ExpirationTime(8), TOKEN_TYPE TokenType at offset 24 (1=primary, 2=impersonation).
            ULONG tokenType = 0;
            std::memcpy(&tokenType, buf + 24, sizeof tokenType);
            extra = "tokentype=" + std::to_string(tokenType);
        }
        extra += std::string(" len=") + std::to_string(ret);

        const NtStatusU sc = static_cast<NtStatusU>(fClose(h));
        extra += " close=" + hex(sc);

        finish(rec, st, "NtQueryInformationToken(TokenStatistics)", extra);
    });

    // (e) Reserve+commit one page in our own address space, then release it.
    PVOID alloc_base = nullptr;
    r.run("C", "call:NtAllocateVirtualMemory", [&alloc_base](Record& rec) {
        auto fn = resolve_nt<PFN_NtAllocateVirtualMemory>("NtAllocateVirtualMemory");
        if (!fn) { set_missing(rec, "NtAllocateVirtualMemory"); return; }

        PVOID base = nullptr;
        SIZE_T size = 0x1000;
        const NtStatusU st = static_cast<NtStatusU>(
            fn(GetCurrentProcess(), &base, 0, &size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

        std::string extra;
        if (st == kStatusSuccess) {
            alloc_base = base;
            extra = "base=" + hex(reinterpret_cast<std::uintptr_t>(base)) +
                    " size=" + hex(static_cast<unsigned long long>(size));
        }
        finish(rec, st, "NtAllocateVirtualMemory(self, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE)", extra);
    });

    r.run("C", "call:NtFreeVirtualMemory", [&alloc_base](Record& rec) {
        if (!alloc_base) { set_skipped(rec, "no allocation from NtAllocateVirtualMemory"); return; }
        auto fn = resolve_nt<PFN_NtFreeVirtualMemory>("NtFreeVirtualMemory");
        if (!fn) { set_missing(rec, "NtFreeVirtualMemory"); return; }

        PVOID base = alloc_base;
        SIZE_T size = 0;   // MEM_RELEASE requires RegionSize == 0
        const NtStatusU st = static_cast<NtStatusU>(
            fn(GetCurrentProcess(), &base, &size, MEM_RELEASE));
        if (st == kStatusSuccess) alloc_base = nullptr;
        finish(rec, st, "NtFreeVirtualMemory(self, MEM_RELEASE)");
    });

    // (f) Query the region that contains ntdll's image in our own address space.
    r.run("C", "call:NtQueryVirtualMemory", [](Record& rec) {
        auto fn = resolve_nt<PFN_NtQueryVirtualMemory>("NtQueryVirtualMemory");
        if (!fn) { set_missing(rec, "NtQueryVirtualMemory"); return; }

        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) { set_skipped(rec, "GetModuleHandleW(ntdll.dll) returned NULL"); return; }

        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T ret = 0;
        const NtStatusU st = static_cast<NtStatusU>(
            fn(GetCurrentProcess(), reinterpret_cast<PVOID>(ntdll), 0 /*MemoryBasicInformation*/,
               &mbi, static_cast<SIZE_T>(sizeof mbi), &ret));

        std::string extra;
        if (st == kStatusSuccess) {
            extra = "base=" + hex(reinterpret_cast<std::uintptr_t>(mbi.BaseAddress)) +
                    " region=" + hex(static_cast<unsigned long long>(mbi.RegionSize)) +
                    " type=" + hex(mbi.Type) +
                    " state=" + hex(mbi.State);
        }
        finish(rec, st, "NtQueryVirtualMemory(self, ntdll base, MemoryBasicInformation)", extra);
    });
}

}  // namespace

// ---------------------------------------------------------------------------
void register_C_syscalls(Reporter& r) {
    register_resolution(r);  // PART 1: 29 resolve:<Nt...> records
    run_safe_calls(r);       // PART 2: 10 call:<Nt...> records
}

}  // namespace probe
