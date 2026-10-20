// probe_M_system.cpp - category M: system / memory / processor features / firmware.
//
// Read-only measurements of the memory budget, CPU topology and feature bits,
// large-page availability, firmware table access (SMBIOS / ACPI) and timers.
// The one allocation attempted (a single large page) is freed immediately.
//
// No link libs: the APIs that may be absent or sandbox-gated (logical processor
// info, large-page minimum, firmware tables) are resolved from kernel32 /
// kernelbase at runtime, so a missing export classifies as MISSING instead of
// breaking the link on either host.
#include "probe.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace probe {
namespace {

using GetLogicalProcessorInformationEx_t =
    BOOL(WINAPI*)(LOGICAL_PROCESSOR_RELATIONSHIP, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, PDWORD);
using GetLargePageMinimum_t = SIZE_T(WINAPI*)(void);
using GetSystemFirmwareTable_t = UINT(WINAPI*)(DWORD, DWORD, PVOID, DWORD);
using EnumSystemFirmwareTables_t = UINT(WINAPI*)(DWORD, PVOID, DWORD);

template <class F>
F find_api(const char* sym) {
    static const char* const mods[] = {"kernel32.dll", "kernelbase.dll"};
    for (const char* m : mods)
        if (void* p = resolve(m, sym)) return reinterpret_cast<F>(p);
    return nullptr;
}

std::string fmt(const char* f, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

typedef unsigned long long u64;
u64 mb(u64 bytes) { return bytes >> 20; }

constexpr DWORD kSigRSMB = ((DWORD)'R' << 24) | ((DWORD)'S' << 16) | ((DWORD)'M' << 8) | (DWORD)'B';
constexpr DWORD kSigACPI = ((DWORD)'A' << 24) | ((DWORD)'C' << 16) | ((DWORD)'P' << 8) | (DWORD)'I';

// Map a failed firmware / large-page call's GetLastError() to a probe status.
Status err_status(DWORD e) {
    switch (e) {
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return Status::DENIED;
    case ERROR_NOT_FOUND:
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return Status::MISSING;
    case ERROR_INVALID_FUNCTION:
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
    case ERROR_INVALID_PARAMETER:
        return Status::NOTIMPL;
    default:
        return Status::WARN;
    }
}

void fail(Record& rec, DWORD e, const char* what) {
    rec.errorcode = e;
    rec.status = e ? err_status(e) : Status::WARN;
    rec.detail = std::string(what) + " failed: " + (e ? error_text(e) : std::string("returned 0 with no error"));
}

void missing(Record& rec, const char* api) {
    rec.status = Status::MISSING;
    rec.detail = std::string(api) + " not exported by kernel32/kernelbase";
}

unsigned popcount(ULONG_PTR m) {
    unsigned n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

std::string sig4(DWORD id) {
    char s[5] = {};
    memcpy(s, &id, 4);
    for (int i = 0; i < 4; i++)
        if (s[i] < 0x20 || s[i] > 0x7e) s[i] = '.';
    return s;
}

}  // namespace

void register_M_system(Reporter& r) {
    // 1. memory budget (the Series S ceiling matters)
    r.run("M", "memory-status", [](Record& rec) {
        MEMORYSTATUSEX m = {};
        m.dwLength = sizeof(m);
        if (!GlobalMemoryStatusEx(&m)) return fail(rec, GetLastError(), "GlobalMemoryStatusEx");
        rec.status = Status::OK;
        rec.value = fmt("totalphys_mb=%llu;availphys_mb=%llu;totalvirt_mb=%llu;availvirt_mb=%llu;load=%lu",
                        mb(m.ullTotalPhys), mb(m.ullAvailPhys), mb(m.ullTotalVirtual), mb(m.ullAvailVirtual),
                        (unsigned long)m.dwMemoryLoad);
        rec.detail = fmt("totalpage_mb=%llu availpage_mb=%llu availextvirt_mb=%llu (sandboxed processes may see a per-app budget, not machine RAM)",
                         mb(m.ullTotalPageFile), mb(m.ullAvailPageFile), mb(m.ullAvailExtendedVirtual));
    });

    // 2. native system info (max application address matters for the loader)
    r.run("M", "system-info", [](Record& rec) {
        SYSTEM_INFO si = {};
        GetNativeSystemInfo(&si);
        const char* arch;
        switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: arch = "x64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86"; break;
        case PROCESSOR_ARCHITECTURE_ARM:   arch = "arm"; break;
        case PROCESSOR_ARCHITECTURE_ARM64: arch = "arm64"; break;
        case PROCESSOR_ARCHITECTURE_IA64:  arch = "ia64"; break;
        default:                           arch = "unknown"; break;
        }
        const u64 lo = (u64)(ULONG_PTR)si.lpMinimumApplicationAddress;
        const u64 hi = (u64)(ULONG_PTR)si.lpMaximumApplicationAddress;
        rec.status = Status::OK;
        rec.value = fmt("arch=%s;pagesize=%lu;numproc=%lu;minappaddr=0x%llx;maxappaddr=0x%llx;activeprocmask=0x%llx",
                        arch, (unsigned long)si.dwPageSize, (unsigned long)si.dwNumberOfProcessors, lo, hi,
                        (u64)si.dwActiveProcessorMask);
        rec.detail = fmt("archid=%u allocgran=%lu level=%u revision=0x%04x; user VA span ~%llu GiB (maxappaddr is the loader's VA ceiling)",
                         (unsigned)si.wProcessorArchitecture, (unsigned long)si.dwAllocationGranularity,
                         (unsigned)si.wProcessorLevel, (unsigned)si.wProcessorRevision, (hi + 1 - lo) >> 30);
    });

    // 3. logical processor topology
    r.run("M", "logical-proc-info", [](Record& rec) {
        auto fn = find_api<GetLogicalProcessorInformationEx_t>("GetLogicalProcessorInformationEx");
        if (!fn) return missing(rec, "GetLogicalProcessorInformationEx");
        DWORD len = 0;
        BOOL ok = fn(RelationAll, nullptr, &len);
        DWORD e = GetLastError();
        if (ok || e != ERROR_INSUFFICIENT_BUFFER || len == 0)
            return fail(rec, ok ? 0 : e, "GetLogicalProcessorInformationEx(RelationAll) size query");
        std::vector<BYTE> buf(len, 0);
        if (!fn(RelationAll, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len))
            return fail(rec, GetLastError(), "GetLogicalProcessorInformationEx(RelationAll)");
        unsigned groups = 0, cores = 0, logical = 0, pkgs = 0, numa = 0, caches = 0;
        std::map<unsigned, unsigned long> cache_kb;  // (level<<4|type) -> size in KB
        for (DWORD off = 0; off + sizeof(DWORD) * 2 <= len;) {
            auto* p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
            if (p->Size == 0 || off + p->Size > len) break;
            switch (p->Relationship) {
            case RelationProcessorCore:
                cores++;
                for (WORD g = 0; g < p->Processor.GroupCount; g++) logical += popcount(p->Processor.GroupMask[g].Mask);
                break;
            case RelationProcessorPackage: pkgs++; break;
            case RelationNumaNode:         numa++; break;
            case RelationCache:
                caches++;
                cache_kb[((unsigned)p->Cache.Level << 4) | (unsigned)p->Cache.Type] = p->Cache.CacheSize >> 10;
                break;
            case RelationGroup:
                groups = p->Group.ActiveGroupCount;
                break;
            default: break;
            }
            off += p->Size;
        }
        std::string cs;
        static const char* const tnames[] = {"U", "I", "D", "T"};
        for (auto& kv : cache_kb)
            cs += fmt("%sL%u%s=%luKB", cs.empty() ? "" : ",", kv.first >> 4, tnames[(kv.first & 0xF) & 3], kv.second);
        rec.status = Status::OK;
        rec.value = fmt("bytes=%lu;groups=%u;cores=%u;logical=%u;packages=%u;numa=%u", (unsigned long)len, groups,
                        cores, logical, pkgs, numa);
        rec.detail = fmt("%u cache record(s); sizes: %s", caches, cs.c_str());
    });

    // 4. processor feature sweep - symbols are named so the compiler validates them
    r.run("M", "processor-features", [](Record& rec) {
        struct Feat { DWORD id; const char* sym; };
#define PFE(sym) { sym, #sym }
        static const Feat feats[] = {
            PFE(PF_COMPARE_EXCHANGE_DOUBLE),
            PFE(PF_MMX_INSTRUCTIONS_AVAILABLE),
            PFE(PF_XMMI_INSTRUCTIONS_AVAILABLE),
            PFE(PF_RDTSC_INSTRUCTION_AVAILABLE),
            PFE(PF_PAE_ENABLED),
            PFE(PF_XMMI64_INSTRUCTIONS_AVAILABLE),
            PFE(PF_SSE_DAZ_MODE_AVAILABLE),
            PFE(PF_NX_ENABLED),
            PFE(PF_SSE3_INSTRUCTIONS_AVAILABLE),
            PFE(PF_COMPARE_EXCHANGE128),
            PFE(PF_COMPARE64_EXCHANGE128),
            PFE(PF_XSAVE_ENABLED),
            PFE(PF_SECOND_LEVEL_ADDRESS_TRANSLATION),
            PFE(PF_VIRT_FIRMWARE_ENABLED),
            PFE(PF_RDWRFSGSBASE_AVAILABLE),
            PFE(PF_FASTFAIL_AVAILABLE),
            PFE(PF_RDRAND_INSTRUCTION_AVAILABLE),
            PFE(PF_RDTSCP_INSTRUCTION_AVAILABLE),
            PFE(PF_RDPID_INSTRUCTION_AVAILABLE),
            PFE(PF_MONITORX_INSTRUCTION_AVAILABLE),
            PFE(PF_SSSE3_INSTRUCTIONS_AVAILABLE),
            PFE(PF_SSE4_1_INSTRUCTIONS_AVAILABLE),
            PFE(PF_SSE4_2_INSTRUCTIONS_AVAILABLE),
            PFE(PF_AVX_INSTRUCTIONS_AVAILABLE),
            PFE(PF_AVX2_INSTRUCTIONS_AVAILABLE),
            PFE(PF_AVX512F_INSTRUCTIONS_AVAILABLE),
            PFE(PF_ERMS_AVAILABLE),
        };
#undef PFE
        std::string value, yes, no;
        unsigned n_yes = 0;
        for (const Feat& f : feats) {
            const bool on = IsProcessorFeaturePresent(f.id) != FALSE;
            value += fmt("%sPF%lu=%d", value.empty() ? "" : ";", (unsigned long)f.id, on ? 1 : 0);
            std::string& dst = on ? yes : no;
            dst += (dst.empty() ? "" : ",") + std::string(f.sym + 3);  // drop "PF_"
            n_yes += on;
        }
        rec.status = Status::OK;
        rec.value = value;
        rec.detail = fmt("%u/%u present. present: %s | absent: %s", n_yes, (unsigned)(sizeof(feats) / sizeof(feats[0])),
                         yes.c_str(), no.c_str());
    });

    // 5. large-page minimum
    r.run("M", "large-page-minimum", [](Record& rec) {
        auto fn = find_api<GetLargePageMinimum_t>("GetLargePageMinimum");
        if (!fn) return missing(rec, "GetLargePageMinimum");
        const SIZE_T n = fn();
        rec.status = n ? Status::OK : Status::INFO;
        rec.value = fmt("bytes=%llu", (u64)n);
        rec.detail = n ? "large pages supported by the processor/OS"
                       : "0 => large pages unavailable";
    });

    // 6. try to commit exactly one large page (needs SeLockMemoryPrivilege)
    r.run("M", "large-page-alloc", [](Record& rec) {
        auto fn = find_api<GetLargePageMinimum_t>("GetLargePageMinimum");
        if (!fn) return missing(rec, "GetLargePageMinimum");
        const SIZE_T n = fn();
        if (!n) {
            rec.status = Status::SKIPPED;
            rec.detail = "GetLargePageMinimum()==0, large pages unavailable";
            return;
        }
        void* p = VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
        if (!p) {
            const DWORD e = GetLastError();
            fail(rec, e, "VirtualAlloc(MEM_LARGE_PAGES, 1 page)");
            if (e == ERROR_PRIVILEGE_NOT_HELD) rec.detail += " (SeLockMemoryPrivilege not held)";
            rec.value = fmt("bytes=%llu;ok=0", (u64)n);
            return;
        }
        VirtualFree(p, 0, MEM_RELEASE);
        rec.status = Status::OK;
        rec.value = fmt("bytes=%llu;ok=1", (u64)n);
        rec.detail = "large page committed and released";
    });

    // 7. raw SMBIOS table (size + header only; no table contents are captured)
    r.run("M", "firmware-smbios", [](Record& rec) {
        auto fn = find_api<GetSystemFirmwareTable_t>("GetSystemFirmwareTable");
        if (!fn) return missing(rec, "GetSystemFirmwareTable");
        const UINT n = fn(kSigRSMB, 0, nullptr, 0);
        if (!n) return fail(rec, GetLastError(), "GetSystemFirmwareTable('RSMB')");
        rec.status = Status::OK;
        rec.value = fmt("smbios_bytes=%u", n);
        if (n <= (4u << 20)) {
            std::vector<BYTE> b(n, 0);
            const UINT got = fn(kSigRSMB, 0, b.data(), n);
            if (got >= 8) {
                DWORD tlen;
                memcpy(&tlen, &b[4], 4);
                rec.detail = fmt("RawSMBIOSData header: smbios=%u.%u dmirev=%u tablelen=%lu calling20=%u (contents not captured)",
                                 (unsigned)b[1], (unsigned)b[2], (unsigned)b[3], (unsigned long)tlen, (unsigned)b[0]);
            } else {
                rec.status = Status::WARN;
                rec.errorcode = GetLastError();
                rec.detail = fmt("size query gave %u but fetch returned %u bytes", n, got);
            }
        } else {
            rec.detail = "table larger than 4 MiB; fetch skipped";
        }
    });

    // 8. firmware table enumeration (RSMB provider)
    r.run("M", "firmware-enum", [](Record& rec) {
        auto fn = find_api<EnumSystemFirmwareTables_t>("EnumSystemFirmwareTables");
        if (!fn) return missing(rec, "EnumSystemFirmwareTables");
        const UINT n = fn(kSigRSMB, nullptr, 0);
        if (!n) return fail(rec, GetLastError(), "EnumSystemFirmwareTables('RSMB')");
        std::string ids;
        if (n <= 1024) {
            std::vector<DWORD> v(n / sizeof(DWORD) + 1, 0);
            const UINT got = fn(kSigRSMB, v.data(), n);
            for (UINT i = 0; i < got / sizeof(DWORD); i++) ids += fmt("%s0x%lx", i ? "," : "", (unsigned long)v[i]);
        }
        rec.status = Status::OK;
        rec.value = fmt("tables=%u;bytes=%u", n / (unsigned)sizeof(DWORD), n);
        rec.detail = "RSMB table ids: " + ids;
    });

    // 9. ACPI tables: size query with id 0, then enumerate and size each table
    r.run("M", "firmware-acpi", [](Record& rec) {
        auto get = find_api<GetSystemFirmwareTable_t>("GetSystemFirmwareTable");
        auto enm = find_api<EnumSystemFirmwareTables_t>("EnumSystemFirmwareTables");
        if (!get) return missing(rec, "GetSystemFirmwareTable");
        // The spec'd query: id 0 is not a real ACPI table, so a failure is expected
        // and says nothing about access; record it but judge access by enumeration.
        const UINT n0 = get(kSigACPI, 0, nullptr, 0);
        const DWORD e0 = n0 ? 0 : GetLastError();
        std::string note = n0 ? fmt("id0 size=%u", n0) : fmt("id0 query err=%lu (%s)", (unsigned long)e0, error_text(e0).c_str());
        if (!enm) {
            rec.status = n0 ? Status::OK : err_status(e0);
            rec.errorcode = e0;
            rec.value = fmt("acpi_size=%u", n0);
            rec.detail = note + "; EnumSystemFirmwareTables not exported";
            return;
        }
        const UINT nb = enm(kSigACPI, nullptr, 0);
        if (!nb) {
            const DWORD e = GetLastError();
            fail(rec, e, "EnumSystemFirmwareTables('ACPI')");
            rec.value = fmt("acpi_size=%u", n0);
            rec.detail += "; " + note;
            return;
        }
        const UINT count = nb / (UINT)sizeof(DWORD);
        std::vector<DWORD> ids(count + 1, 0);
        const UINT got = enm(kSigACPI, ids.data(), nb);
        std::string list;
        unsigned long long total = 0;
        unsigned sized = 0;
        for (UINT i = 0; i < got / sizeof(DWORD); i++) {
            const UINT sz = get(kSigACPI, ids[i], nullptr, 0);
            if (sz) { total += sz; sized++; }
            if (i < 16) list += fmt("%s%s:%u", i ? "," : "", sig4(ids[i]).c_str(), sz);
        }
        rec.status = Status::OK;
        rec.value = fmt("acpi_size=%u;tables=%u;sized=%u;total_bytes=%llu", n0, count, sized, total);
        rec.detail = note + "; tables (sig:size, first 16): " + list;
    });

    // 10. timers
    r.run("M", "tick-count", [](Record& rec) {
        const ULONGLONG ms = GetTickCount64();
        LARGE_INTEGER f = {}, c = {};
        const BOOL okf = QueryPerformanceFrequency(&f);
        const DWORD ef = okf ? 0 : GetLastError();
        const BOOL okc = okf && QueryPerformanceCounter(&c);
        rec.value = fmt("tick64_ms=%llu;qpf_hz=%lld;qpc=%lld", (u64)ms, (long long)f.QuadPart, (long long)c.QuadPart);
        const u64 s = ms / 1000;
        rec.detail = fmt("uptime %llud %lluh %llum %llus", s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
        if (okf && okc) {
            rec.status = Status::OK;
        } else {
            rec.status = Status::WARN;
            rec.errorcode = okf ? GetLastError() : ef;
            rec.detail += "; QueryPerformance* failed";
        }
    });
}

}  // namespace probe
