// Function-level forwarding of guest ntdll exports (list: rtl_forward.inc).
#include "internal.h"
#include <cstring>

namespace win10xbox::dispatch::detail {
namespace {

constexpr std::size_t kJmpSize = 5;          // E9 rel32
constexpr std::size_t kSlotSize = 16;        // FF 25 00000000 <abs64>, padded
constexpr std::size_t kBlockSize = 64 * 1024;

struct Block { std::uint8_t* base; std::size_t used; };
SRWLOCK g_block_lock = SRWLOCK_INIT;
std::vector<Block>& Blocks() { static std::vector<Block> v; return v; }

bool InRel32(const void* from_next, const void* to) {
    auto d = reinterpret_cast<std::intptr_t>(to) - reinterpret_cast<std::intptr_t>(from_next);
    return d >= INT32_MIN && d <= INT32_MAX;
}

// A 64 KiB executable block within +-2 GiB of `near`.
std::uint8_t* AllocNear(const void* near_addr) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity;
    const std::uintptr_t origin = reinterpret_cast<std::uintptr_t>(near_addr) & ~(gran - 1);
    const std::uintptr_t range = 0x7ff00000;
    const std::uintptr_t lo = origin > range ? origin - range : gran;
    const std::uintptr_t hi = origin + range;
    for (std::uintptr_t step = gran; step < range; step += gran) {
        for (int dir = 0; dir < 2; ++dir) {
            std::uintptr_t a = dir ? origin + step : origin - step;
            if (a < lo || a > hi || a < reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress) ||
                a > reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress))
                continue;
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) || mbi.State != MEM_FREE) continue;
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), kBlockSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return static_cast<std::uint8_t*>(p);
        }
    }
    return nullptr;
}

// Writes an absolute-jump slot near `from` and returns its address.
std::uint8_t* MakeTrampoline(const void* from, const void* to) {
    AcquireSRWLockExclusive(&g_block_lock);
    Block* blk = nullptr;
    for (auto& b : Blocks())
        if (b.used + kSlotSize <= kBlockSize && InRel32(static_cast<const std::uint8_t*>(from) + kJmpSize, b.base) &&
            InRel32(static_cast<const std::uint8_t*>(from) + kJmpSize, b.base + kBlockSize))
            { blk = &b; break; }
    if (!blk) {
        std::uint8_t* p = AllocNear(from);
        if (!p) { ReleaseSRWLockExclusive(&g_block_lock); return nullptr; }
        Blocks().push_back(Block{p, 0});
        blk = &Blocks().back();
    }
    std::uint8_t* slot = blk->base + blk->used;
    blk->used += kSlotSize;
    DWORD old;
    VirtualProtect(blk->base, kBlockSize, PAGE_READWRITE, &old);
    const std::uint8_t code[6] = {0xFF, 0x25, 0, 0, 0, 0};   // jmp [rip+0]
    std::memcpy(slot, code, sizeof code);
    std::uint64_t target = reinterpret_cast<std::uint64_t>(to);
    std::memcpy(slot + 6, &target, sizeof target);
    std::memset(slot + 14, 0xCC, kSlotSize - 14);
    VirtualProtect(blk->base, kBlockSize, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, kSlotSize);
    ReleaseSRWLockExclusive(&g_block_lock);
    return slot;
}

// True when the function at `rva` can take a 5-byte jump without overwriting
// its neighbour: checked against .pdata (functions with unwind info, and the
// next function's start for leaf functions without it).
bool HasRoom(const std::uint8_t* base, std::uint32_t rva) {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!d.VirtualAddress || d.Size < sizeof(RUNTIME_FUNCTION)) return true;
    auto* rf = reinterpret_cast<const RUNTIME_FUNCTION*>(base + d.VirtualAddress);
    std::size_t n = d.Size / sizeof(RUNTIME_FUNCTION);
    std::uint32_t next = UINT32_MAX;
    for (std::size_t i = 0; i < n; ++i) {
        if (rf[i].BeginAddress == rva) {
            if (rf[i].EndAddress - rf[i].BeginAddress >= kJmpSize) return true;
        }
        if (rf[i].BeginAddress > rva && rf[i].BeginAddress < next) next = rf[i].BeginAddress;
    }
    return next == UINT32_MAX || next - rva >= kJmpSize;
}

}  // namespace

bool PatchJump(void* from, const void* to) {
    auto* p = static_cast<std::uint8_t*>(from);
    const void* target = to;
    if (!InRel32(p + kJmpSize, to)) {
        target = MakeTrampoline(from, to);
        if (!target) return false;
    }
    std::int32_t rel = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target) -
                                                 reinterpret_cast<std::intptr_t>(p + kJmpSize));
    std::uint8_t code[kJmpSize] = {0xE9};
    std::memcpy(code + 1, &rel, sizeof rel);

    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(p, &mbi, sizeof mbi);
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    DWORD old = 0;
    bool reprotect = !(mbi.Protect & writable);
    // D11 (write-then-protect): make the page RW, never RWX.
    if (reprotect && !VirtualProtect(p, kJmpSize, PAGE_READWRITE, &old)) return false;
    std::memcpy(p, code, sizeof code);
    if (reprotect) VirtualProtect(p, kJmpSize, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, kJmpSize);
    return true;
}

ForwardResult ApplyRtlForwarding(const loader::MappedImage& img) {
    ForwardResult r;
    auto* base = static_cast<std::uint8_t*>(img.base);
    auto patch_export = [&](const char* name, std::uint16_t ordinal, const void* target, const char* what) {
        std::string fwd;
        void* guest = ordinal ? loader::FindExportByOrdinal(img.base, ordinal, &fwd) : loader::FindExport(img.base, name, &fwd);
        if (!guest) {
            WARN(dispatch, "forward %s: no guest export%s%s%s", name, fwd.empty() ? "" : " (forwarder ", fwd.c_str(),
                 fwd.empty() ? "" : ")");
            ++r.skipped;
            return;
        }
        if (!target) {
            WARN(dispatch, "forward %s: %s has no such function, keeping Wine's", name, what);
            ++r.skipped;
            return;
        }
        auto rva = static_cast<std::uint32_t>(static_cast<std::uint8_t*>(guest) - base);
        if (!HasRoom(base, rva)) {
            WARN(dispatch, "forward %s: function too small for a jump", name);
            ++r.skipped;
            return;
        }
        if (!PatchJump(guest, target)) {
            ERR(dispatch, "forward %s: patch failed", name);
            ++r.skipped;
            return;
        }
        ++r.forwarded;
    };
    auto patch = [&](const char* name, const void* target, const char* what) { patch_export(name, 0, target, what); };
#define RTL_FORWARD(name) patch(#name, HostExport(#name), "the host ntdll");
#define WINE_OVERRIDE(name, fn) patch(#name, reinterpret_cast<const void*>(&fn), "core");
#define WINE_OVERRIDE_ORD(ordinal, what, fn) patch_export(what, ordinal, reinterpret_cast<const void*>(&fn), "core");
#include "rtl_forward.inc"
    if (NativeGuiActive()) {
        // Route B: what the native user32 needs from ntdll and Wine's does not provide in a form it accepts
        // (win32k_native.cpp: the client-procedure tables, DefWindowProc forwarders, the csrss connection).
        for (const NamedTarget& t : NativeUserOverrides()) patch(t.name, t.fn, "core (gui native)");
    }
    TRACE(dispatch, "guest ntdll: %zu functions forwarded, %zu skipped", r.forwarded, r.skipped);
    return r;
}

namespace {

struct ExportView {
    const std::uint8_t* base = nullptr;
    const IMAGE_EXPORT_DIRECTORY* dir = nullptr;
    std::uint32_t dir_rva = 0, dir_size = 0;
    const DWORD* functions = nullptr;
    const DWORD* names = nullptr;
    const WORD* ordinals = nullptr;
    bool Open(const void* image) {
        base = static_cast<const std::uint8_t*>(image);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!d.VirtualAddress || !d.Size) return false;
        dir_rva = d.VirtualAddress;
        dir_size = d.Size;
        dir = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir_rva);
        functions = reinterpret_cast<const DWORD*>(base + dir->AddressOfFunctions);
        names = reinterpret_cast<const DWORD*>(base + dir->AddressOfNames);
        ordinals = reinterpret_cast<const WORD*>(base + dir->AddressOfNameOrdinals);
        return true;
    }
    const char* Name(std::uint32_t i) const { return reinterpret_cast<const char*>(base + names[i]); }
    bool Has(const char* name) const {   // names are sorted (binary search, like every loader)
        std::uint32_t lo = 0, hi = dir->NumberOfNames;
        while (lo < hi) {
            const std::uint32_t mid = (lo + hi) / 2;
            const int c = std::strcmp(Name(mid), name);
            if (!c) return true;
            if (c < 0) lo = mid + 1; else hi = mid;
        }
        return false;
    }
};

bool IsCodeRva(const std::uint8_t* base, std::uint32_t rva) {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (rva >= sec[i].VirtualAddress && rva < sec[i].VirtualAddress + sec[i].Misc.VirtualSize)
            return (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    return false;
}

}  // namespace

std::size_t ExtendGuestNtdllExports(const loader::MappedImage& img) {
    ExportView guest, host;
    if (!guest.Open(img.base) || !host.Open(HostNtdll())) {
        WARN(dispatch, "ntdll exports: no export directory to extend");
        return 0;
    }
    struct Add { const char* name; const void* target; };
    std::vector<Add> add;
    for (std::uint32_t i = 0; i < host.dir->NumberOfNames; ++i) {
        const char* name = host.Name(i);
        const std::uint32_t fn_rva = host.functions[host.ordinals[i]];
        if (fn_rva >= host.dir_rva && fn_rva < host.dir_rva + host.dir_size) continue;   // forwarder
        if (!IsCodeRva(host.base, fn_rva)) continue;                                      // data: cannot be a jump
        if (guest.Has(name)) continue;
        // A Wine host's own unix-side plumbing (development runs only) is not the guest's to call.
        if (!std::strncmp(name, "__wine", 6) || !std::strncmp(name, "wine_", 5)) continue;
        add.push_back({name, host.base + fn_rva});
    }
    if (add.empty()) return 0;
    const std::uint32_t old_funcs = guest.dir->NumberOfFunctions, old_names = guest.dir->NumberOfNames;
    std::size_t strings = 0;
    for (auto& a : add) strings += std::strlen(a.name) + 1;
    const std::size_t thunks = add.size() * kSlotSize;
    const std::size_t total = thunks + sizeof(IMAGE_EXPORT_DIRECTORY) + 4 * (old_funcs + add.size()) +
                              4 * (old_names + add.size()) + 2 * (old_names + add.size()) + strings + 64;
    // One allocation within 4 GiB above the image: export RVAs are 32-bit and unsigned.
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(img.base);
    const std::uintptr_t gran = si.dwAllocationGranularity;
    std::uint8_t* mem = nullptr;
    for (std::uintptr_t a = (base + img.size + gran - 1) & ~(gran - 1); a + total - base < 0xFFFF0000ull && !mem;
         a += gran) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) || mbi.State != MEM_FREE) continue;
        mem = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(a), total, MEM_RESERVE | MEM_COMMIT,
                                                      PAGE_READWRITE));
    }
    if (!mem) {
        WARN(dispatch, "ntdll exports: no room within 4 GiB of the guest ntdll for %zu more exports", add.size());
        return 0;
    }
    auto rva_of = [&](const void* p) {
        return static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(p) - base);
    };
    // Jumps first (outside the directory's range, or the loader would take them for forwarder strings).
    std::uint8_t* t = mem;
    std::vector<DWORD> new_fn_rva;
    for (auto& a : add) {
        const std::uint8_t code[6] = {0xFF, 0x25, 0, 0, 0, 0};   // jmp [rip+0]
        std::memcpy(t, code, sizeof code);
        const std::uint64_t target = reinterpret_cast<std::uint64_t>(a.target);
        std::memcpy(t + 6, &target, 8);
        std::memset(t + 14, 0xCC, kSlotSize - 14);
        new_fn_rva.push_back(rva_of(t));
        t += kSlotSize;
    }
    auto* dir = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(mem + thunks);
    *dir = *guest.dir;
    auto* functions = reinterpret_cast<DWORD*>(dir + 1);
    std::memcpy(functions, guest.functions, 4 * old_funcs);
    for (std::size_t k = 0; k < add.size(); ++k) functions[old_funcs + k] = new_fn_rva[k];
    auto* names = functions + old_funcs + add.size();
    auto* ords = reinterpret_cast<WORD*>(names + old_names + add.size());
    char* str = reinterpret_cast<char*>(ords + old_names + add.size());
    // Merge the two sorted name lists (the host's are sorted too, and `add` keeps its order).
    std::uint32_t gi = 0, out = 0;
    std::size_t ai = 0;
    while (gi < old_names || ai < add.size()) {
        const bool take_guest = ai == add.size() || (gi < old_names && std::strcmp(guest.Name(gi), add[ai].name) < 0);
        if (take_guest) {
            names[out] = guest.names[gi];
            ords[out] = guest.ordinals[gi];
            ++gi;
        } else {
            const std::size_t n = std::strlen(add[ai].name) + 1;
            std::memcpy(str, add[ai].name, n);
            names[out] = rva_of(str);
            ords[out] = static_cast<WORD>(old_funcs + ai);
            str += n;
            ++ai;
        }
        ++out;
    }
    dir->NumberOfFunctions = static_cast<DWORD>(old_funcs + add.size());
    dir->NumberOfNames = static_cast<DWORD>(old_names + add.size());
    dir->AddressOfFunctions = rva_of(functions);
    dir->AddressOfNames = rva_of(names);
    dir->AddressOfNameOrdinals = rva_of(ords);
    // Guest functions whose RVA lies in the OLD directory's range would have been forwarders; the new range no
    // longer covers them.
    std::size_t old_forwarders = 0;
    for (std::uint32_t i = 0; i < old_funcs; ++i)
        if (guest.functions[i] >= guest.dir_rva && guest.functions[i] < guest.dir_rva + guest.dir_size) ++old_forwarders;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(static_cast<std::uint8_t*>(img.base) + dos->e_lfanew);
    auto& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const DWORD dir_rva = rva_of(dir);
    const DWORD dir_size = static_cast<DWORD>(reinterpret_cast<std::uint8_t*>(str) - reinterpret_cast<std::uint8_t*>(dir));
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(&d, &mbi, sizeof mbi);
    DWORD old = 0;
    const bool flip = !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
    if (flip) VirtualProtect(&d, sizeof d, PAGE_READWRITE, &old);
    d.VirtualAddress = dir_rva;
    d.Size = dir_size;
    if (flip) VirtualProtect(&d, sizeof d, old, &old);
    VirtualProtect(mem, thunks, PAGE_EXECUTE_READ, &old);
    VirtualProtect(mem + thunks, total - thunks, PAGE_READONLY, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, thunks);
    std::string sample;
    for (std::size_t k = 0; k < add.size() && k < 6; ++k) (sample += k ? ", " : "") += add[k].name;
    TRACE(dispatch, "guest ntdll: %zu host ntdll code exports added as jumps (%s%s); export directory now at %p%s",
          add.size(), sample.c_str(), add.size() > 6 ? ", ..." : "", static_cast<void*>(dir),
          old_forwarders ? " -- WARNING: the old directory had forwarders" : "");
    return add.size();
}

}  // namespace win10xbox::dispatch::detail

// Guest RtlDeleteCriticalSection (rtl_forward.inc). A section whose debug record lies inside one of our mapped images
// (Wine's DEFINE_CRITICAL_SECTION) never entered the host's list or pool: tear it down here and keep the record out of
// the host's free list. Anything else is the host's.
extern "C" NTSTATUS NTAPI w10x_RtlDeleteCriticalSection(RTL_CRITICAL_SECTION* cs) {
    using Fn = NTSTATUS(NTAPI*)(RTL_CRITICAL_SECTION*);
    static Fn host = reinterpret_cast<Fn>(win10xbox::dispatch::HostExport("RtlDeleteCriticalSection"));
    RTL_CRITICAL_SECTION_DEBUG* dbg = cs ? cs->DebugInfo : nullptr;
    if (dbg && win10xbox::dispatch::detail::FindView(dbg, nullptr) &&
        dbg->ProcessLocksList.Flink == &dbg->ProcessLocksList && dbg->ProcessLocksList.Blink == &dbg->ProcessLocksList) {
        if (cs->LockSemaphore) CloseHandle(cs->LockSemaphore);
        cs->DebugInfo = nullptr;
        cs->LockSemaphore = nullptr;
        cs->LockCount = -1;
        cs->RecursionCount = 0;
        cs->OwningThread = nullptr;
        cs->SpinCount = 0;
        return 0;
    }
    return host(cs);
}
