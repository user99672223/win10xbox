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
    auto patch = [&](const char* name, const void* target, const char* what) {
        std::string fwd;
        void* guest = loader::FindExport(img.base, name, &fwd);
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
#define RTL_FORWARD(name) patch(#name, HostExport(#name), "the host ntdll");
#define WINE_OVERRIDE(name, fn) patch(#name, reinterpret_cast<const void*>(&fn), "core");
#include "rtl_forward.inc"
    TRACE(dispatch, "guest ntdll: %zu functions forwarded, %zu skipped", r.forwarded, r.skipped);
    return r;
}

}  // namespace win10xbox::dispatch::detail
