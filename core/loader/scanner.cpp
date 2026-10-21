// win10xbox loader: code scanner. Finds instructions that must be virtualised
// (cpuid, rdtsc, rdtscp, syscall, sysenter, int 2Eh, optional absolute reads of
// KUSER_SHARED_DATA) with Zydis and redirects each one to the registered
// SiteHandler.
//
// Strategy (a) INLINE: overwrite the site and the whole instructions that follow
// it (>= 5 bytes in total) with `jmp rel32` to a per-site thunk placed within
// +/-2GiB. The thunk saves every GPR, the flags and xmm0-15, builds a ScanRegs
// and calls the handler; then it either skips the original instruction (handler
// returned true) or executes it, then runs the relocated displaced instructions
// and jumps back.
//
// Strategy (b) TRAP: write 0xCC over the first site byte and handle the
// breakpoint in a first-chance vectored handler.
//
// Thunk code is emitted as machine-code bytes at runtime (no inline asm: MSVC
// x64 has none). See docs/LOADER.md for the byte-level layout and the limits.
#include "internal.h"

#include <Zydis/Zydis.h>

#include <algorithm>
#include <cstring>

#include "win10xbox/trace.h"

WIN10XBOX_DECLARE_CHANNEL(scan);

namespace win10xbox::loader {

namespace {

using u8  = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u64 kKuserLo = 0x7FFE0000ull, kKuserHi = 0x7FFE1000ull;
constexpr u64 kReach = 0x7FF00000ull;       // max distance between a site and its thunk
constexpr std::size_t kChunk = 0x10000;     // arena allocation unit (64 KiB)
constexpr std::size_t kMinJmp = 5;          // bytes needed for `jmp rel32`

// ---------------------------------------------------------------------------
// Machine-code emitter
// ---------------------------------------------------------------------------
class Emit {
public:
    std::vector<u8> b;
    std::size_t pos() const { return b.size(); }
    void B(u8 v) { b.push_back(v); }
    template <class... T>
    void Bytes(T... v) { (b.push_back(static_cast<u8>(v)), ...); }
    void D(u32 v) { for (int i = 0; i < 4; ++i) B(static_cast<u8>(v >> (8 * i))); }
    void Q(u64 v) { for (int i = 0; i < 8; ++i) B(static_cast<u8>(v >> (8 * i))); }
    void Raw(const void* p, std::size_t n) {
        const u8* s = static_cast<const u8*>(p);
        b.insert(b.end(), s, s + n);
    }
    void PatchD(std::size_t at, u32 v) { for (int i = 0; i < 4; ++i) b[at + i] = static_cast<u8>(v >> (8 * i)); }
};

// ScanRegs layout (loader.h): rax rbx rcx rdx rsi rdi rbp rsp r8..r15 rflags rip.
// hardware register number -> slot index
constexpr int kSlot[16] = {0, 2, 3, 1, 7, 6, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr int kSlotRflags = 16, kSlotRip = 17;
constexpr u32 kShadow = 0x20;                    // Windows x64 home space
constexpr u32 kRegsOff = kShadow;                // ScanRegs inside the frame
constexpr u32 kRegsSize = sizeof(ScanRegs);      // 144
constexpr u32 kXmmOff = kRegsOff + kRegsSize;    // 16 x 16 bytes
constexpr u32 kFrame = 0x1C0;                    // >= kXmmOff + 256, multiple of 16
static_assert(sizeof(ScanRegs) == 18 * 8, "ScanRegs layout is baked into the thunk");
static_assert(kXmmOff + 256 <= kFrame && kFrame % 16 == 0, "frame layout");

u32 SlotOff(int slot) { return kRegsOff + static_cast<u32>(slot) * 8; }

void MovStore(Emit& e, u32 disp, int hw) {          // mov [rsp+disp32], r64
    e.Bytes(0x48 | (hw >= 8 ? 4 : 0), 0x89, 0x84 | ((hw & 7) << 3), 0x24);
    e.D(disp);
}
void MovLoad(Emit& e, int hw, u32 disp) {           // mov r64, [rsp+disp32]
    e.Bytes(0x48 | (hw >= 8 ? 4 : 0), 0x8B, 0x84 | ((hw & 7) << 3), 0x24);
    e.D(disp);
}
void Movups(Emit& e, bool store, int xmm, u32 disp) {   // movups [rsp+disp32], xmmN / xmmN, [rsp+disp32]
    if (xmm >= 8) e.B(0x44);
    e.Bytes(0x0F, store ? 0x11 : 0x10, 0x84 | ((xmm & 7) << 3), 0x24);
    e.D(disp);
}
void JmpAbs(Emit& e, u64 target) {                  // jmp [rip+0] ; dq target
    e.Bytes(0xFF, 0x25, 0, 0, 0, 0);
    e.Q(target);
}

// ---------------------------------------------------------------------------
// Decoding helpers
// ---------------------------------------------------------------------------
struct Decoded {
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
};

const ZydisDecoder& Decoder() {
    static ZydisDecoder dec;
    static bool init = [] {
        ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        return true;
    }();
    (void)init;
    return dec;
}

bool Decode(const u8* p, std::size_t avail, Decoded* d) {
    return avail != 0 && ZYAN_SUCCESS(ZydisDecoderDecodeFull(&Decoder(), p, avail, &d->ins, d->ops));
}

bool IsJcc(const Decoded& d) {
    if (d.ins.meta.category != ZYDIS_CATEGORY_COND_BR) return false;
    switch (d.ins.mnemonic) {
    case ZYDIS_MNEMONIC_JRCXZ: case ZYDIS_MNEMONIC_JECXZ: case ZYDIS_MNEMONIC_JCXZ:
    case ZYDIS_MNEMONIC_LOOP: case ZYDIS_MNEMONIC_LOOPE: case ZYDIS_MNEMONIC_LOOPNE:
        return false;
    default:
        return true;
    }
}

const ZydisDecodedOperand* FindRelImm(const Decoded& d) {
    for (unsigned i = 0; i < d.ins.operand_count; ++i)
        if (d.ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && d.ops[i].imm.is_relative) return &d.ops[i];
    return nullptr;
}
const ZydisDecodedOperand* FindRipMem(const Decoded& d) {
    for (unsigned i = 0; i < d.ins.operand_count; ++i)
        if (d.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY && d.ops[i].mem.base == ZYDIS_REGISTER_RIP) return &d.ops[i];
    return nullptr;
}

bool IsKuserRead(const Decoded& d) {
    if (d.ins.mnemonic == ZYDIS_MNEMONIC_NOP || d.ins.meta.category == ZYDIS_CATEGORY_PREFETCH) return false;
    for (unsigned i = 0; i < d.ins.operand_count_visible; ++i) {
        const auto& o = d.ops[i];
        if (o.type != ZYDIS_OPERAND_TYPE_MEMORY || o.mem.type != ZYDIS_MEMOP_TYPE_MEM) continue;
        if (o.mem.base != ZYDIS_REGISTER_NONE || o.mem.index != ZYDIS_REGISTER_NONE) continue;
        if (o.mem.segment == ZYDIS_REGISTER_FS || o.mem.segment == ZYDIS_REGISTER_GS) continue;
        if (!o.mem.disp.size) continue;
        u64 v = static_cast<u64>(o.mem.disp.value);
        if (v >= kKuserLo && v < kKuserHi) return true;
    }
    return false;
}

bool IsControlTransfer(const Decoded& d) {
    switch (d.ins.meta.category) {
    case ZYDIS_CATEGORY_CALL: case ZYDIS_CATEGORY_RET: case ZYDIS_CATEGORY_UNCOND_BR:
    case ZYDIS_CATEGORY_COND_BR:
        return true;
    default:
        break;
    }
    switch (d.ins.mnemonic) {
    case ZYDIS_MNEMONIC_IRET: case ZYDIS_MNEMONIC_IRETD: case ZYDIS_MNEMONIC_IRETQ:
    case ZYDIS_MNEMONIC_XBEGIN:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Global state: trap sites, thunk arenas, the vectored handler
// ---------------------------------------------------------------------------
struct TrapSite {
    u64         addr;
    u8          len;
    SiteKind    kind;
    SiteHandler handler;
    void*       ctx;
    u64         stub;        // original instruction + jmp back (used when handler returns false)
};
struct ArenaRec { u64 base; std::size_t size; u64 owner_lo, owner_hi; };

SRWLOCK                g_lock = SRWLOCK_INIT;
std::vector<TrapSite>& Traps() { static std::vector<TrapSite> v; return v; }
std::vector<ArenaRec>& Arenas() { static std::vector<ArenaRec> v; return v; }
PVOID                  g_veh = nullptr;

struct LockEx {
    LockEx() { AcquireSRWLockExclusive(&g_lock); }
    ~LockEx() { ReleaseSRWLockExclusive(&g_lock); }
};
struct LockSh {
    LockSh() { AcquireSRWLockShared(&g_lock); }
    ~LockSh() { ReleaseSRWLockShared(&g_lock); }
};

LONG CALLBACK TrapVeh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    // Hosts differ on whether Rip points at or past the int3; the address of
    // the exception itself is authoritative, the others are fallbacks.
    const u64 cand[3] = {reinterpret_cast<u64>(ep->ExceptionRecord->ExceptionAddress), c->Rip, c->Rip - 1};
    TrapSite site{};
    bool found = false;
    {
        LockSh lk;
        auto& v = Traps();
        for (u64 a : cand) {
            auto it = std::lower_bound(v.begin(), v.end(), a, [](const TrapSite& s, u64 x) { return s.addr < x; });
            if (it != v.end() && it->addr == a) {
                site = *it;
                found = true;
                break;
            }
        }
    }
    if (!found) return EXCEPTION_CONTINUE_SEARCH;

    ScanRegs r;
    r.rax = c->Rax; r.rbx = c->Rbx; r.rcx = c->Rcx; r.rdx = c->Rdx;
    r.rsi = c->Rsi; r.rdi = c->Rdi; r.rbp = c->Rbp; r.rsp = c->Rsp;
    r.r8 = c->R8; r.r9 = c->R9; r.r10 = c->R10; r.r11 = c->R11;
    r.r12 = c->R12; r.r13 = c->R13; r.r14 = c->R14; r.r15 = c->R15;
    r.rflags = c->EFlags;
    r.rip = site.addr;
    bool handled = site.handler(site.kind, &r, site.ctx);
    // rsp and rip are informational (same as the inline path).
    c->Rax = r.rax; c->Rbx = r.rbx; c->Rcx = r.rcx; c->Rdx = r.rdx;
    c->Rsi = r.rsi; c->Rdi = r.rdi; c->Rbp = r.rbp;
    c->R8 = r.r8; c->R9 = r.r9; c->R10 = r.r10; c->R11 = r.r11;
    c->R12 = r.r12; c->R13 = r.r13; c->R14 = r.r14; c->R15 = r.r15;
    c->EFlags = static_cast<DWORD>(r.rflags);
    c->Rip = handled ? site.addr + site.len : site.stub;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ---------------------------------------------------------------------------
// Thunk arena
// ---------------------------------------------------------------------------
u8* AllocNear(u64 lo, u64 hi, std::size_t size) {
    const u64 max_end = lo + kReach;
    const u64 min_base = hi > kReach ? hi - kReach : 0x10000;
    // Upwards from the end of the range.
    u64 a = detail::RoundUp(hi, kChunk);
    while (a + size <= max_end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) break;
        u64 rb = reinterpret_cast<u64>(mbi.BaseAddress), re = rb + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            u64 cand = detail::RoundUp(std::max(a, rb), kChunk);
            if (cand + size <= re && cand + size <= max_end) {
                void* p = VirtualAlloc(reinterpret_cast<void*>(cand), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (p) return static_cast<u8*>(p);
            }
        }
        if (re <= a) break;
        a = re;
    }
    // Downwards from the start of the range.
    a = detail::RoundDown(lo, kChunk);
    while (a > min_base + size) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a - 1), &mbi, sizeof(mbi))) break;
        u64 rb = reinterpret_cast<u64>(mbi.BaseAddress), re = rb + mbi.RegionSize;
        if (mbi.State == MEM_FREE && re >= size) {
            u64 top = std::min(re, a);
            u64 cand = detail::RoundDown(top - size, kChunk);
            if (cand >= rb && cand >= min_base) {
                void* p = VirtualAlloc(reinterpret_cast<void*>(cand), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (p) return static_cast<u8*>(p);
            }
        }
        if (rb == 0 || rb >= a) break;
        a = rb;
    }
    return nullptr;
}

// Bump allocator over RW chunks that are sealed (made RX) before any site is patched.
class Arena {
public:
    Arena(u64 lo, u64 hi, bool near_only) : lo_(lo), hi_(hi), near_(near_only) {}
    ~Arena() { for (auto& c : chunks_) if (c.base) VirtualFree(c.base, 0, MEM_RELEASE); }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // Memory for `n` bytes at a 16-byte aligned address; nullptr if unavailable.
    u8* Reserve(std::size_t n) {
        if (!chunks_.empty()) {
            Chunk& c = chunks_.back();
            std::size_t off = detail::RoundUp(c.used, 16);
            if (off + n <= c.size) return c.base + off;
        }
        std::size_t sz = detail::RoundUp(std::max<std::size_t>(n, kChunk), kChunk);
        u8* p = near_ ? AllocNear(lo_, hi_, sz)
                      : static_cast<u8*>(VirtualAlloc(nullptr, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!p) return nullptr;
        chunks_.push_back({p, sz, 0});
        return p;
    }
    void Commit(u8* at, std::size_t n) {
        Chunk& c = chunks_.back();
        c.used = static_cast<std::size_t>(at - c.base) + n;
    }
    bool Empty() const {
        for (auto& c : chunks_) if (c.used) return false;
        return true;
    }
    // RW -> RX for every chunk. After success the arena is read-only to us.
    bool Seal() {
        for (auto& c : chunks_) {
            DWORD old;
            if (!VirtualProtect(c.base, c.size, PAGE_EXECUTE_READ, &old)) return false;
            FlushInstructionCache(GetCurrentProcess(), c.base, c.size);
        }
        return true;
    }
    // Transfer chunk ownership to the global registry.
    void Publish(u64 owner_lo, u64 owner_hi) {
        LockEx lk;
        for (auto& c : chunks_) {
            Arenas().push_back({reinterpret_cast<u64>(c.base), c.size, owner_lo, owner_hi});
            c.base = nullptr;
        }
        chunks_.clear();
    }
private:
    struct Chunk { u8* base; std::size_t size, used; };
    std::vector<Chunk> chunks_;
    u64 lo_, hi_;
    bool near_;
};

// ---------------------------------------------------------------------------
// Instruction relocation (displaced instructions)
// ---------------------------------------------------------------------------
struct Disp {
    Decoded     d;
    const u8*   addr;
};

bool FitsI32(std::int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

// Re-emit `d` so that it behaves identically when executed at new_addr.
bool Relocate(const Disp& s, u64 new_addr, Emit& e) {
    const auto& ins = s.d.ins;
    const u64 old_addr = reinterpret_cast<u64>(s.addr);
    const std::size_t len = ins.length;
    if (const ZydisDecodedOperand* rel = FindRelImm(s.d)) {
        if (!IsJcc(s.d)) return false;
        u64 target = 0;
        if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins, rel, old_addr, &target))) return false;
        if (rel->imm.size == 8) {                       // 7x rel8 -> 0F 8x rel32
            if (ins.raw.prefix_count != 0 || s.addr[0] < 0x70 || s.addr[0] > 0x7F) return false;
            std::int64_t d = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(new_addr + 6);
            if (!FitsI32(d)) return false;
            e.Bytes(0x0F, 0x80 | (s.addr[0] & 0x0F));
            e.D(static_cast<u32>(static_cast<std::int32_t>(d)));
            return true;
        }
        if (rel->imm.size == 32) {
            std::int64_t d = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(new_addr + len);
            if (!FitsI32(d)) return false;
            std::size_t at = e.pos();
            e.Raw(s.addr, len);
            e.PatchD(at + rel->imm.offset, static_cast<u32>(static_cast<std::int32_t>(d)));
            return true;
        }
        return false;
    }
    if (const ZydisDecodedOperand* m = FindRipMem(s.d)) {
        u64 target = 0;
        if (m->mem.disp.size != 32 ||
            !ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins, m, old_addr, &target)))
            return false;
        std::int64_t d = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(new_addr + len);
        if (!FitsI32(d)) return false;
        std::size_t at = e.pos();
        e.Raw(s.addr, len);
        e.PatchD(at + m->mem.disp.offset, static_cast<u32>(static_cast<std::int32_t>(d)));
        return true;
    }
    e.Raw(s.addr, len);
    return true;
}

// ---------------------------------------------------------------------------
// Thunk generation
// ---------------------------------------------------------------------------
struct SiteInfo {
    u8*         addr;
    u8          len;
    SiteKind    kind;
};

// Builds the inline thunk for `s` to be placed at `at`. `region_len` is the
// number of original bytes that the 5-byte jump (plus filler) replaces.
bool BuildInlineThunk(u64 at, const SiteInfo& s, const std::vector<Disp>& disp, std::size_t region_len,
                      const ScanOptions& o, Emit& e) {
    const u64 site = reinterpret_cast<u64>(s.addr);
    // --- save state -------------------------------------------------------
    e.B(0x9C);                                          // pushfq
    e.B(0x55);                                          // push rbp
    e.Bytes(0x48, 0x89, 0xE5);                          // mov rbp, rsp
    e.Bytes(0x48, 0x81, 0xEC); e.D(kFrame);             // sub rsp, kFrame
    e.Bytes(0x48, 0x83, 0xE4, 0xF0);                    // and rsp, -16
    for (int hw = 0; hw < 16; ++hw) {
        if (hw == 4 || hw == 5) continue;               // rsp, rbp handled below
        MovStore(e, SlotOff(kSlot[hw]), hw);
    }
    e.Bytes(0x48, 0x8B, 0x45, 0x00);                    // mov rax, [rbp]      (caller's rbp)
    MovStore(e, SlotOff(kSlot[5]), 0);
    e.Bytes(0x48, 0x8D, 0x45, 0x10);                    // lea rax, [rbp+16]   (caller's rsp)
    MovStore(e, SlotOff(kSlot[4]), 0);
    e.Bytes(0x48, 0x8B, 0x45, 0x08);                    // mov rax, [rbp+8]    (caller's rflags)
    MovStore(e, SlotOff(kSlotRflags), 0);
    e.Bytes(0x48, 0xB8); e.Q(site);                     // mov rax, site
    MovStore(e, SlotOff(kSlotRip), 0);
    for (int x = 0; x < 16; ++x) Movups(e, true, x, kXmmOff + static_cast<u32>(x) * 16);
    // --- call handler(kind, &regs, ctx) ------------------------------------
    e.B(0xFC);                                          // cld
    e.B(0xB9); e.D(static_cast<u32>(s.kind));           // mov ecx, kind
    e.Bytes(0x48, 0x8D, 0x54, 0x24, kRegsOff);          // lea rdx, [rsp+kRegsOff]
    e.Bytes(0x49, 0xB8); e.Q(reinterpret_cast<u64>(o.handler_ctx));  // mov r8, ctx
    e.Bytes(0x48, 0xB8); e.Q(reinterpret_cast<u64>(o.handler));      // mov rax, handler
    e.Bytes(0xFF, 0xD0);                                // call rax
    // --- choose continuation: handled -> L_skip, else L_original -------------
    e.Bytes(0x84, 0xC0);                                // test al, al
    e.Bytes(0x48, 0x8D, 0x05); std::size_t fix_orig = e.pos(); e.D(0);   // lea rax, [rip+L_original]
    e.Bytes(0x48, 0x8D, 0x0D); std::size_t fix_skip = e.pos(); e.D(0);   // lea rcx, [rip+L_skip]
    e.Bytes(0x48, 0x0F, 0x45, 0xC1);                    // cmovnz rax, rcx
    e.Bytes(0x48, 0x89, 0x45, 0x08);                    // mov [rbp+8], rax    (becomes the `ret` target)
    MovLoad(e, 0, SlotOff(kSlot[5]));                   // handler may have changed rbp
    e.Bytes(0x48, 0x89, 0x45, 0x00);                    // mov [rbp], rax
    // --- restore state ----------------------------------------------------
    for (int x = 0; x < 16; ++x) Movups(e, false, x, kXmmOff + static_cast<u32>(x) * 16);
    for (int hw = 1; hw < 16; ++hw) {
        if (hw == 4 || hw == 5) continue;
        MovLoad(e, hw, SlotOff(kSlot[hw]));
    }
    MovLoad(e, 0, SlotOff(kSlot[0]));                   // rax last
    e.Bytes(0xFF, 0xB4, 0x24); e.D(SlotOff(kSlotRflags));   // push qword [rsp+rflags]
    e.B(0x9D);                                          // popfq
    e.Bytes(0x48, 0x89, 0xEC);                          // mov rsp, rbp
    e.B(0x5D);                                          // pop rbp
    e.B(0xC3);                                          // ret -> target, rsp = caller's rsp
    // --- continuation code --------------------------------------------------
    std::size_t l_orig = e.pos();
    e.Raw(s.addr, s.len);                               // the original instruction
    std::size_t l_skip = e.pos();
    for (const Disp& d : disp) {
        if (!Relocate(d, at + e.pos(), e)) return false;
    }
    JmpAbs(e, site + region_len);
    e.PatchD(fix_orig, static_cast<u32>(l_orig - (fix_orig + 4)));
    e.PatchD(fix_skip, static_cast<u32>(l_skip - (fix_skip + 4)));
    return true;
}

void BuildTrapStub(const SiteInfo& s, Emit& e) {
    e.Raw(s.addr, s.len);
    JmpAbs(e, reinterpret_cast<u64>(s.addr) + s.len);
}

// ---------------------------------------------------------------------------
// The scan job
// ---------------------------------------------------------------------------
struct Range { u8* start; std::size_t size; };

struct Job {
    std::vector<Range> exec;
    std::vector<u64>   funcs;          // decode resync points (function starts), sorted
    std::vector<u64>   hint_targets;   // known/suspected indirect branch targets
    u64                reach_lo = 0, reach_hi = 0;   // extent of the patched image/range
    u64                owner_lo = 0, owner_hi = 0;
};

bool WantKind(const ScanOptions& o, SiteKind k) {
    switch (k) {
    case SiteKind::Cpuid: return o.patch_cpuid;
    case SiteKind::Rdtsc: case SiteKind::Rdtscp: return o.patch_rdtsc;
    case SiteKind::Syscall: case SiteKind::Sysenter: case SiteKind::Int2E: return o.patch_syscall;
    case SiteKind::KuserRead: return o.patch_kuser_reads;
    }
    return false;
}

bool Classify(const Decoded& d, SiteKind* k) {
    switch (d.ins.mnemonic) {
    case ZYDIS_MNEMONIC_CPUID: *k = SiteKind::Cpuid; return true;
    case ZYDIS_MNEMONIC_RDTSC: *k = SiteKind::Rdtsc; return true;
    case ZYDIS_MNEMONIC_RDTSCP: *k = SiteKind::Rdtscp; return true;
    case ZYDIS_MNEMONIC_SYSCALL: *k = SiteKind::Syscall; return true;
    case ZYDIS_MNEMONIC_SYSENTER: *k = SiteKind::Sysenter; return true;
    case ZYDIS_MNEMONIC_INT:
        if (d.ins.operand_count_visible >= 1 && d.ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
            d.ops[0].imm.value.u == 0x2E) {
            *k = SiteKind::Int2E;
            return true;
        }
        return false;
    default:
        return false;
    }
}

// Linear sweep of one range. `on` is called for every decoded instruction.
// Protected ranges (rewritten thunks, earlier patches) and syscall thunks are
// stepped over, and decoding resynchronises at function starts.
template <class F>
void Sweep(const Range& r, const std::vector<u64>& funcs, F&& on) {
    const u8* p = r.start;
    const u8* const end = r.start + r.size;
    std::size_t fi = static_cast<std::size_t>(
        std::lower_bound(funcs.begin(), funcs.end(), reinterpret_cast<u64>(p)) - funcs.begin());
    Decoded d;
    while (p < end) {
        const void* pe = nullptr;
        if (detail::IsProtectedRange(p, &pe)) {
            p = std::max(p + 1, static_cast<const u8*>(pe));
            continue;
        }
        if (p[0] == 0x4C && static_cast<std::size_t>(end - p) >= 8 && p[1] == 0x8B && p[2] == 0xD1 && p[3] == 0xB8) {
            std::size_t tl = 0;
            if (detail::MatchSyscallThunk(p, static_cast<std::size_t>(end - p), nullptr, &tl) != detail::ThunkForm::None) {
                p += tl;
                continue;
            }
        }
        if (!Decode(p, static_cast<std::size_t>(end - p), &d)) {
            ++p;
            continue;
        }
        const u64 pa = reinterpret_cast<u64>(p);
        while (fi < funcs.size() && funcs[fi] <= pa) ++fi;
        if (fi < funcs.size() && funcs[fi] < pa + d.ins.length && funcs[fi] < reinterpret_cast<u64>(end)) {
            p = reinterpret_cast<const u8*>(funcs[fi]);     // a function starts inside this "instruction"
            continue;
        }
        on(p, d);
        p += d.ins.length;
    }
}

bool InRange(const std::vector<Range>& rs, u64 a) {
    for (auto& r : rs)
        if (a >= reinterpret_cast<u64>(r.start) && a < reinterpret_cast<u64>(r.start) + r.size) return true;
    return false;
}

NTSTATUS ScanCore(const Job& job, const ScanOptions& opts, ScanResult* result) {
    if (result) *result = ScanResult{};
    if (!opts.handler) return kStatusInvalidParameter;

    // ---- pass 1: sites and branch targets -----------------------------------
    std::vector<SiteInfo> sites;
    std::vector<u64> targets = job.hint_targets;
    for (const Range& r : job.exec) {
        Sweep(r, job.funcs, [&](const u8* p, const Decoded& d) {
            if (const ZydisDecodedOperand* rel = FindRelImm(d)) {
                u64 t = 0;
                if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&d.ins, rel, reinterpret_cast<u64>(p), &t))) targets.push_back(t);
            }
            SiteKind k;
            if (Classify(d, &k)) {
                if (WantKind(opts, k)) sites.push_back({const_cast<u8*>(p), static_cast<u8>(d.ins.length), k});
            } else if (opts.patch_kuser_reads && IsKuserRead(d)) {
                if (IsControlTransfer(d)) {
                    TRACE(scan, "kuser access in a control-transfer instruction at %p ignored", p);
                    if (result) ++result->skipped;
                } else {
                    sites.push_back({const_cast<u8*>(p), static_cast<u8>(d.ins.length), SiteKind::KuserRead});
                }
            }
        });
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    for (u64 f : job.funcs) targets.push_back(f);   // function starts can be entered indirectly
    std::sort(targets.begin(), targets.end());
    TRACE(scan, "%zu candidate sites, %zu branch targets", sites.size(), targets.size());
    if (sites.empty()) return kStatusSuccess;

    auto any_in = [](const std::vector<u64>& v, u64 lo_excl, u64 hi_excl) {
        auto it = std::upper_bound(v.begin(), v.end(), lo_excl);
        return it != v.end() && *it < hi_excl;
    };
    std::vector<u64> site_starts;
    for (auto& s : sites) site_starts.push_back(reinterpret_cast<u64>(s.addr));

    // ---- pass 2: plan each site -------------------------------------------------
    struct Plan {
        enum Mode { Skip, Inline, Trap } mode = Skip;
        std::size_t region_len = 0;
        u64 stub = 0;
    };
    std::vector<Plan> plans(sites.size());
    Arena near_arena(job.reach_lo, job.reach_hi, true), far_arena(0, 0, false);
    const bool can_reach = job.reach_hi - job.reach_lo < kReach / 2;

    for (std::size_t i = 0; i < sites.size(); ++i) {
        const SiteInfo& s = sites[i];
        const u64 sa = reinterpret_cast<u64>(s.addr);
        Plan& pl = plans[i];
        const Range* range = nullptr;
        for (auto& r : job.exec)
            if (sa >= reinterpret_cast<u64>(r.start) && sa < reinterpret_cast<u64>(r.start) + r.size) range = &r;
        const u8* range_end = range ? range->start + range->size : s.addr + s.len;

        // Inline attempt.
        bool inline_ok = can_reach;
        std::vector<Disp> disp;
        std::size_t total = s.len;
        while (inline_ok && total < kMinJmp) {
            const u8* p = s.addr + total;
            Disp dd;
            dd.addr = p;
            if (p >= range_end || detail::IsProtectedRange(p, nullptr) ||
                !Decode(p, static_cast<std::size_t>(range_end - p), &dd.d)) {
                inline_ok = false;
                break;
            }
            const auto& ins = dd.d.ins;
            bool rel = FindRelImm(dd.d) != nullptr;
            if (ins.meta.category == ZYDIS_CATEGORY_CALL || ins.meta.category == ZYDIS_CATEGORY_RET ||
                ins.meta.category == ZYDIS_CATEGORY_UNCOND_BR || ins.mnemonic == ZYDIS_MNEMONIC_IRET ||
                ins.mnemonic == ZYDIS_MNEMONIC_IRETD || ins.mnemonic == ZYDIS_MNEMONIC_IRETQ ||
                ins.mnemonic == ZYDIS_MNEMONIC_XBEGIN || (rel && !IsJcc(dd.d)) ||
                ins.meta.category == ZYDIS_CATEGORY_INTERRUPT) {
                inline_ok = false;
                break;
            }
            total += ins.length;
            disp.push_back(dd);
        }
        if (inline_ok && s.addr + total > range_end) inline_ok = false;
        if (inline_ok) {
            // No branch may enter the middle of the replaced bytes and no other
            // site may live inside them.
            if (any_in(targets, sa, sa + total) || any_in(site_starts, sa, sa + total)) {
                TRACE(scan, "site %p: branch target or site inside the displaced bytes", s.addr);
                inline_ok = false;
            }
        }
        if (inline_ok) {
            // Reserve() hands out room for the worst case so the address the
            // thunk is assembled for is final.
            const std::size_t room = 2048 + disp.size() * 32;
            u8* at = near_arena.Reserve(room);
            Emit e;
            std::int64_t dist = at ? static_cast<std::int64_t>(reinterpret_cast<u64>(at)) -
                                         static_cast<std::int64_t>(sa + kMinJmp)
                                   : 0;
            if (at && FitsI32(dist) && BuildInlineThunk(reinterpret_cast<u64>(at), s, disp, total, opts, e) &&
                e.pos() <= room) {
                std::memcpy(at, e.b.data(), e.b.size());
                near_arena.Commit(at, e.b.size());
                pl.mode = Plan::Inline;
                pl.region_len = total;
                pl.stub = reinterpret_cast<u64>(at);
            }
        }
        if (pl.mode == Plan::Inline) continue;

        // Trap fallback: needs an out-of-line copy of the original instruction.
        Emit e;
        BuildTrapStub(s, e);
        Arena* ar = &near_arena;
        u8* at = near_arena.Reserve(e.pos());
        if (!at) { ar = &far_arena; at = far_arena.Reserve(e.pos()); }
        if (!at) {
            WARN(scan, "no memory for the trap stub of site %p", s.addr);
            continue;
        }
        std::memcpy(at, e.b.data(), e.b.size());
        ar->Commit(at, e.b.size());
        pl.mode = Plan::Trap;
        pl.region_len = s.len;
        pl.stub = reinterpret_cast<u64>(at);
    }

    // ---- seal the arenas, then patch the code -------------------------------
    if (!near_arena.Seal() || !far_arena.Seal()) {
        ERR(scan, "cannot make the thunk arena executable: %lu", GetLastError());
        return kStatusUnsuccessful;
    }
    const bool need_veh = std::any_of(plans.begin(), plans.end(), [](const Plan& p) { return p.mode == Plan::Trap; });
    if (need_veh) EnsureTrapHandlerInstalled();

    bool used_arena = false;
    for (std::size_t i = 0; i < sites.size(); ++i) {
        const SiteInfo& s = sites[i];
        const Plan& pl = plans[i];
        if (pl.mode == Plan::Skip) {
            if (result) ++result->skipped;
            continue;
        }
        const u64 sa = reinterpret_cast<u64>(s.addr);
        if (pl.mode == Plan::Trap) {
            {   // register before the 0xCC becomes visible
                LockEx lk;
                auto& v = Traps();
                TrapSite ts{sa, s.len, s.kind, opts.handler, opts.handler_ctx, pl.stub};
                auto it = std::lower_bound(v.begin(), v.end(), sa, [](const TrapSite& t, u64 x) { return t.addr < x; });
                if (it != v.end() && it->addr == sa) *it = ts; else v.insert(it, ts);
            }
            detail::ScopedWritable w(s.addr, 1);
            if (!w.ok()) {
                LockEx lk;
                auto& v = Traps();
                v.erase(std::remove_if(v.begin(), v.end(), [&](const TrapSite& t) { return t.addr == sa; }), v.end());
                if (result) ++result->skipped;
                continue;
            }
            *s.addr = 0xCC;
            detail::AddProtectedRange(s.addr, s.len);
            used_arena = true;
            if (result) { ++result->trapped; result->sites.push_back({s.kind, s.addr, s.len, false}); }
        } else {
            detail::ScopedWritable w(s.addr, pl.region_len);
            if (!w.ok()) {
                if (result) ++result->skipped;
                continue;
            }
            u8 code[32];
            std::memset(code, 0xCC, sizeof(code));
            code[0] = 0xE9;
            std::int32_t rel = static_cast<std::int32_t>(static_cast<std::int64_t>(pl.stub) - static_cast<std::int64_t>(sa + 5));
            std::memcpy(code + 1, &rel, 4);
            // Fill the tail first so a concurrent reader never sees a half-written jump with garbage after it.
            std::memcpy(s.addr + 5, code + 5, pl.region_len - 5);
            std::memcpy(s.addr, code, 5);
            detail::AddProtectedRange(s.addr, pl.region_len);
            used_arena = true;
            if (result) { ++result->patched; result->sites.push_back({s.kind, s.addr, s.len, true}); }
        }
    }
    if (used_arena) near_arena.Publish(job.owner_lo, job.owner_hi), far_arena.Publish(job.owner_lo, job.owner_hi);
    TRACE(scan, "patched %zu inline, %zu trapped, %zu skipped", result ? result->patched : 0,
          result ? result->trapped : 0, result ? result->skipped : 0);
    return kStatusSuccess;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void EnsureTrapHandlerInstalled() {
    LockEx lk;
    if (g_veh) return;
    using AddVeh = PVOID(NTAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER);
    auto add = reinterpret_cast<AddVeh>(detail::HostNtdllProc("RtlAddVectoredExceptionHandler"));
    if (add) g_veh = add(1, TrapVeh);
    else g_veh = AddVectoredExceptionHandler(1, TrapVeh);
    if (!g_veh) ERR(scan, "cannot install the vectored exception handler");
}

NTSTATUS ScanAndPatchRange(void* start, std::size_t size, const ScanOptions& opts, ScanResult* result) {
    if (!start || !size) return kStatusInvalidParameter;
    Job job;
    job.exec.push_back({static_cast<u8*>(start), size});
    job.funcs.push_back(reinterpret_cast<u64>(start));
    job.reach_lo = reinterpret_cast<u64>(start);
    job.reach_hi = job.reach_lo + size;
    job.owner_lo = job.reach_lo;
    job.owner_hi = job.reach_hi;
    return ScanCore(job, opts, result);
}

NTSTATUS ScanAndPatch(const MappedImage& img, const ScanOptions& opts, ScanResult* result) {
    const IMAGE_NT_HEADERS64* nt = nullptr;
    const IMAGE_SECTION_HEADER* secs = nullptr;
    unsigned nsec = 0;
    if (!img.base || !detail::GetNtHeaders(img.base, &nt) || !detail::GetSections(img.base, &secs, &nsec))
        return kStatusInvalidParameter;
    auto* base = static_cast<u8*>(img.base);
    const u32 image_size = static_cast<u32>(img.size ? img.size : nt->OptionalHeader.SizeOfImage);

    Job job;
    job.reach_lo = reinterpret_cast<u64>(base);
    job.reach_hi = job.reach_lo + image_size;
    job.owner_lo = job.reach_lo;
    job.owner_hi = job.reach_hi;

    // Executable sections.
    for (unsigned i = 0; i < nsec; ++i) {
        u32 va = secs[i].VirtualAddress, sz = detail::SectionSize(secs[i]);
        if (!(secs[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !sz || va >= image_size) continue;
        sz = std::min<u32>(sz, image_size - va);
        job.exec.push_back({base + va, sz});
    }
    auto rva_in_exec = [&](u64 a) { return InRange(job.exec, a); };

    // Function starts: .pdata, entry point, exports. They resync the linear
    // sweep and double as potential indirect branch targets.
    auto add_func = [&](u32 rva) {
        if (rva < image_size && rva_in_exec(job.reach_lo + rva)) job.funcs.push_back(job.reach_lo + rva);
    };
    if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
        IMAGE_DATA_DIRECTORY ex = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (ex.VirtualAddress && u64(ex.VirtualAddress) + ex.Size <= image_size) {
            const auto* rf = reinterpret_cast<const RUNTIME_FUNCTION*>(base + ex.VirtualAddress);
            for (u32 i = 0; i < ex.Size / sizeof(RUNTIME_FUNCTION); ++i) {
                RUNTIME_FUNCTION f;
                std::memcpy(&f, rf + i, sizeof(f));
                add_func(f.BeginAddress);
                add_func(f.EndAddress);
            }
        }
    }
    add_func(nt->OptionalHeader.AddressOfEntryPoint);
    {
        std::vector<detail::NamedExport> ex;
        if (detail::EnumNamedExports(base, &ex))
            for (auto& e : ex) if (!e.forwarder && e.rva) add_func(e.rva);
    }
    std::sort(job.funcs.begin(), job.funcs.end());
    job.funcs.erase(std::unique(job.funcs.begin(), job.funcs.end()), job.funcs.end());

    // Jump-table style hints from initialised non-executable data: aligned
    // dwords that are RVAs (MSVC/LLVM tables) or offsets relative to their own
    // address (GCC tables), and aligned qwords that are absolute VAs
    // (relocated pointers). False positives only cost an inline patch.
    for (unsigned i = 0; i < nsec; ++i) {
        const auto& s = secs[i];
        if ((s.Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_DISCARDABLE)) || !s.SizeOfRawData) continue;
        if (!(s.Characteristics & IMAGE_SCN_MEM_READ)) continue;
        u32 va = s.VirtualAddress, sz = std::min<u32>(std::min<u32>(detail::SectionSize(s), s.SizeOfRawData),
                                                    va < image_size ? image_size - va : 0);
        const u8* d = base + va;
        for (u32 off = 0; off + 4 <= sz; off += 4) {
            u32 v;
            std::memcpy(&v, d + off, 4);
            if (v && v < image_size && rva_in_exec(job.reach_lo + v)) job.hint_targets.push_back(job.reach_lo + v);
            u64 rel = reinterpret_cast<u64>(d + off) + static_cast<u64>(static_cast<std::int64_t>(static_cast<std::int32_t>(v)));
            if (v && rva_in_exec(rel)) job.hint_targets.push_back(rel);
            if ((off & 7) == 0 && off + 8 <= sz) {
                u64 q;
                std::memcpy(&q, d + off, 8);
                if (rva_in_exec(q)) job.hint_targets.push_back(q);
            }
        }
    }
    return ScanCore(job, opts, result);
}

void ReleaseScanSites(const void* start, std::size_t size) {
    const u64 lo = reinterpret_cast<u64>(start), hi = lo + size;
    {
        LockEx lk;
        auto& t = Traps();
        t.erase(std::remove_if(t.begin(), t.end(), [&](const TrapSite& s) { return s.addr >= lo && s.addr < hi; }), t.end());
        auto& a = Arenas();
        for (auto it = a.begin(); it != a.end();) {
            if (it->owner_lo >= lo && it->owner_lo < hi) {
                VirtualFree(reinterpret_cast<void*>(it->base), 0, MEM_RELEASE);
                it = a.erase(it);
            } else {
                ++it;
            }
        }
    }
    detail::ForgetProtectedRanges(start, size);
}

}  // namespace win10xbox::loader
