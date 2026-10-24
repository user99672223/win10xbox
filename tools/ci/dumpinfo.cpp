// dumpinfo.cpp - prints what matters of a WER local dump (docs/slices/uwpswap.md round 11): the exception record (code,
// parameters; parameter 0 of a STATUS_FAIL_FAST_EXCEPTION is the FAST_FAIL_* code), rip/rsp/rcx.. of the faulting thread, and
// rip plus the top 32 stack words mapped to module+RVA through the dump's ModuleListStream (nearest module below the address,
// within SizeOfImage), falling back to the MemoryInfoListStream image region (allocation base + RVA) for modules a guest
// loader mapped outside the PEB list. Usage: dumpinfo <file.dmp> [stack words]. MSVC, dbghelp.
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#pragma comment(lib, "dbghelp.lib")

namespace {
struct Mod { std::uint64_t base; std::uint32_t size; std::wstring name; };
struct Reg { std::uint64_t base; std::uint64_t size; const unsigned char* data; };
std::vector<Mod> g_mods;
std::vector<Reg> g_mem;
std::vector<MINIDUMP_MEMORY_INFO> g_info;

bool Read64(std::uint64_t a, std::uint64_t* v)
{
    for (const Reg& r : g_mem)
        if (a >= r.base && a + 8 <= r.base + r.size) { std::memcpy(v, r.data + (a - r.base), 8); return true; }
    return false;
}

std::string Where(std::uint64_t a)
{
    char b[512];
    for (const Mod& m : g_mods)
        if (a >= m.base && a < m.base + m.size) {
            std::snprintf(b, sizeof b, "%ls+0x%llx", m.name.c_str(), (unsigned long long)(a - m.base));
            return b;
        }
    for (const MINIDUMP_MEMORY_INFO& i : g_info)
        if (a >= i.BaseAddress && a < i.BaseAddress + i.RegionSize && i.Type == MEM_IMAGE) {
            std::snprintf(b, sizeof b, "<image allocbase 0x%llx>+0x%llx", (unsigned long long)i.AllocationBase,
                          (unsigned long long)(a - i.AllocationBase));
            return b;
        }
    return "";
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { std::printf("usage: dumpinfo file.dmp [stack words]\n"); return 2; }
    int words = argc > 2 ? _wtoi(argv[2]) : 32;
    HANDLE f = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { std::printf("dumpinfo: cannot open %ls\n", argv[1]); return 1; }
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* view = m ? MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (!view) { std::printf("dumpinfo: cannot map %ls\n", argv[1]); return 1; }
    std::printf("dumpinfo: %ls (%lld bytes)\n", argv[1], (long long)sz.QuadPart);

    PMINIDUMP_DIRECTORY dir; PVOID s; ULONG n;
    if (MiniDumpReadDumpStream(view, ModuleListStream, &dir, &s, &n)) {
        auto* ml = (MINIDUMP_MODULE_LIST*)s;
        for (ULONG i = 0; i < ml->NumberOfModules; i++) {
            const MINIDUMP_MODULE& mm = ml->Modules[i];
            auto* nm = (MINIDUMP_STRING*)((char*)view + mm.ModuleNameRva);
            std::wstring full(nm->Buffer, nm->Length / 2);
            size_t sl = full.find_last_of(L"\\/");
            g_mods.push_back({mm.BaseOfImage, mm.SizeOfImage, sl == std::wstring::npos ? full : full.substr(sl + 1)});
            std::printf("  module %-28ls base 0x%llx size 0x%x\n", g_mods.back().name.c_str(), (unsigned long long)mm.BaseOfImage, mm.SizeOfImage);
        }
    }
    if (MiniDumpReadDumpStream(view, MemoryInfoListStream, &dir, &s, &n)) {
        auto* il = (MINIDUMP_MEMORY_INFO_LIST*)s;
        auto* p = (char*)s + il->SizeOfHeader;
        for (ULONG64 i = 0; i < il->NumberOfEntries; i++, p += il->SizeOfEntry) g_info.push_back(*(MINIDUMP_MEMORY_INFO*)p);
        // image regions that no module covers: one line per allocation base (the guest loader maps outside PEB->Ldr).
        std::uint64_t last = 0;
        for (const auto& i : g_info)
            if (i.Type == MEM_IMAGE && i.AllocationBase != last) {
                last = i.AllocationBase;
                if (Where(i.AllocationBase).find("<image") == 0) std::printf("  image allocbase 0x%llx (not in module list)\n", (unsigned long long)last);
            }
    }
    if (MiniDumpReadDumpStream(view, MemoryListStream, &dir, &s, &n)) {
        auto* ml = (MINIDUMP_MEMORY_LIST*)s;
        for (ULONG i = 0; i < ml->NumberOfMemoryRanges; i++) {
            auto& d = ml->MemoryRanges[i];
            g_mem.push_back({d.StartOfMemoryRange, d.Memory.DataSize, (unsigned char*)view + d.Memory.Rva});
        }
    }
    if (MiniDumpReadDumpStream(view, Memory64ListStream, &dir, &s, &n)) {
        auto* ml = (MINIDUMP_MEMORY64_LIST*)s;
        ULONG64 off = ml->BaseRva;
        for (ULONG64 i = 0; i < ml->NumberOfMemoryRanges; i++) {
            auto& d = ml->MemoryRanges[i];
            g_mem.push_back({d.StartOfMemoryRange, d.DataSize, (unsigned char*)view + off});
            off += d.DataSize;
        }
    }
    if (!MiniDumpReadDumpStream(view, ExceptionStream, &dir, &s, &n)) { std::printf("dumpinfo: no ExceptionStream\n"); return 1; }
    auto* ex = (MINIDUMP_EXCEPTION_STREAM*)s;
    const MINIDUMP_EXCEPTION& e = ex->ExceptionRecord;
    std::printf("exception: thread %lu code 0x%08lx flags 0x%lx address 0x%llx params %lu\n", ex->ThreadId, e.ExceptionCode,
                e.ExceptionFlags, (unsigned long long)e.ExceptionAddress, e.NumberParameters);
    for (DWORD i = 0; i < e.NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
        std::printf("  param[%lu] = 0x%llx\n", i, (unsigned long long)e.ExceptionInformation[i]);
    if (e.ExceptionCode == 0xC0000409 && e.NumberParameters >= 1)
        std::printf("  STATUS_STACK_BUFFER_OVERRUN: FAST_FAIL code %llu\n", (unsigned long long)e.ExceptionInformation[0]);
    if (e.ExceptionCode == 0xC0000602)
        std::printf("  STATUS_FAIL_FAST_EXCEPTION (FAST_FAIL code is in the fail-fast record, see rcx/param0 below)\n");
    auto* c = (CONTEXT*)((char*)view + ex->ThreadContext.Rva);
    std::printf("context: rip=0x%llx (%s) rsp=0x%llx rcx=0x%llx rdx=0x%llx r8=0x%llx r9=0x%llx rax=0x%llx rbx=0x%llx\n",
                (unsigned long long)c->Rip, Where(c->Rip).c_str(), (unsigned long long)c->Rsp, (unsigned long long)c->Rcx,
                (unsigned long long)c->Rdx, (unsigned long long)c->R8, (unsigned long long)c->R9, (unsigned long long)c->Rax,
                (unsigned long long)c->Rbx);
    std::printf("  exception address -> %s\n", Where(e.ExceptionAddress).c_str());
    int shown = 0;
    for (int i = 0; i < 4096 && shown < words; i++) {
        std::uint64_t v;
        if (!Read64(c->Rsp + 8ull * i, &v)) { if (i == 0) std::printf("stack: rsp not in dump memory\n"); break; }
        std::string w = Where(v);
        if (w.empty()) continue;
        std::printf("  stack[rsp+0x%x] = 0x%llx  %s\n", i * 8, (unsigned long long)v, w.c_str());
        shown++;
    }
    return 0;
}
