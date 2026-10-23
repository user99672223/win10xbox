// Handlers for instruction sites patched by the loader's scanner (D9/D10):
// cpuid (configured vendor, hypervisor hidden), rdtsc/rdtscp (virtual clock)
// and raw syscall / int 2Eh in guest code (routed into the dispatcher).
#include <atomic>
#include "internal.h"
#include "win10xbox/config.h"
#if WIN10XBOX_HAVE_CLOCK
#include "win10xbox/clock.h"
#endif
#include <Zydis/Zydis.h>
#include <cstring>

namespace win10xbox::dispatch {
namespace {

SRWLOCK g_cpu_lock = SRWLOCK_INIT;
CpuPolicy& Policy_() {
    detail::NoteHostStateUnderGuestView("dispatch::CpuPolicy");
    static CpuPolicy p;
    return p;
}

// CPUID.1:EAX from family/model/stepping: stepping 3:0, model 7:4, family 11:8, ext model 19:16, ext family 27:20.
std::uint32_t Signature(const CpuPolicy& p) {
    const std::uint32_t base_family = p.family > 0xF ? 0xF : p.family;
    const std::uint32_t ext_family = p.family > 0xF ? p.family - 0xF : 0;
    return (p.stepping & 0xF) | ((p.model & 0xF) << 4) | (base_family << 8) | (((p.model >> 4) & 0xF) << 16) |
           (ext_family << 20);
}

bool AmdStyleVendor(const std::string& v) { return v == "AuthenticAMD" || v == "HygonGenuine"; }

void Cpuid(loader::ScanRegs* r) {
    const std::uint32_t leaf = static_cast<std::uint32_t>(r->rax), sub = static_cast<std::uint32_t>(r->rcx);
    AcquireSRWLockShared(&g_cpu_lock);
    const CpuPolicy p = Policy_();
    ReleaseSRWLockShared(&g_cpu_lock);
    int v[4] = {0, 0, 0, 0};
    const bool hv_leaf = leaf >= 0x40000000u && leaf <= 0x400000FFu;
    if (!(p.hide_hypervisor && hv_leaf)) __cpuidex(v, static_cast<int>(leaf), static_cast<int>(sub));
    if (p.vendor.size() == 12 && (leaf == 0 || leaf == 0x80000000u)) {
        if (leaf == 0 || AmdStyleVendor(p.vendor)) {
            std::memcpy(&v[1], p.vendor.data(), 4);       // ebx
            std::memcpy(&v[3], p.vendor.data() + 4, 4);   // edx
            std::memcpy(&v[2], p.vendor.data() + 8, 4);   // ecx
        } else {
            v[1] = v[2] = v[3] = 0;                       // Intel: ext leaf 0 has no vendor
        }
    }
    if (leaf == 1 && p.hide_hypervisor) v[2] &= 0x7fffffff;   // hypervisor present
    if (leaf == 1 && p.advertise_rdrand) v[2] |= 0x40000000;   // RDRAND
    if ((leaf == 1 || leaf == 0x80000001u) && p.family) v[0] = static_cast<int>(Signature(p));
    if (leaf >= 0x80000002u && leaf <= 0x80000004u && p.brand[0]) {
        std::memcpy(v, p.brand + 16 * (leaf - 0x80000002u), 16);
    }
    r->rax = static_cast<std::uint32_t>(v[0]);
    r->rbx = static_cast<std::uint32_t>(v[1]);
    r->rcx = static_cast<std::uint32_t>(v[2]);
    r->rdx = static_cast<std::uint32_t>(v[3]);
}

std::uint64_t Tsc(std::uint32_t* aux) {
#if WIN10XBOX_HAVE_CLOCK
    if (aux) *aux = clock::RdtscpAux();
    return clock::Rdtsc();
#else
    unsigned int a = 0;
    std::uint64_t t = aux ? __rdtscp(&a) : __rdtsc();
    if (aux) *aux = a;
    return t;
#endif
}

// ---- Absolute reads of KUSER_SHARED_DATA's version block (D18) -------------------------------------------------
// The host's page says what the host is (a runner's build 26100, the console's own build); the guest is told 10.0.19045
// by the virtual page the clock module keeps (clock::VirtualSharedData(): NtBuildNumber, NtProductType, NtMajorVersion,
// NtMinorVersion). The scanner redirects the instructions that read those fields (mov eax,[7FFE026Ch]) or form their
// address (mov rax,7FFE026Ch / lea rax,[7FFE026Ch]); this decodes the original instruction and serves it from the
// virtual page. A form it does not emulate runs on the host page, with a trace line.
std::uint64_t* GprSlot(loader::ScanRegs* r, unsigned id) {
    std::uint64_t* const slots[16] = {&r->rax, &r->rcx, &r->rdx, &r->rbx, &r->rsp, &r->rbp, &r->rsi, &r->rdi,
                                      &r->r8,  &r->r9,  &r->r10, &r->r11, &r->r12, &r->r13, &r->r14, &r->r15};
    return id < 16 ? slots[id] : nullptr;
}

bool WriteGpr(loader::ScanRegs* r, ZydisRegister reg, std::uint64_t value, bool pointer) {
    const ZydisRegisterClass cls = ZydisRegisterGetClass(reg);
    unsigned id = static_cast<unsigned>(ZydisRegisterGetId(reg));
    std::uint64_t* slot = nullptr;
    if (reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH || reg == ZYDIS_REGISTER_BH) {
        if (pointer) return false;
        slot = GprSlot(r, id - 4);
        if (!slot) return false;
        *slot = (*slot & ~0xFF00ull) | ((value & 0xFF) << 8);
        return true;
    }
    slot = GprSlot(r, id);
    if (!slot) return false;
    if (pointer) {   // an address is a 64-bit quantity whatever the register's name in the instruction
        if (cls != ZYDIS_REGCLASS_GPR64 && cls != ZYDIS_REGCLASS_GPR32) return false;
        *slot = value;
        return true;
    }
    switch (cls) {
    case ZYDIS_REGCLASS_GPR64: *slot = value; return true;
    case ZYDIS_REGCLASS_GPR32: *slot = value & 0xFFFFFFFFull; return true;
    case ZYDIS_REGCLASS_GPR16: *slot = (*slot & ~0xFFFFull) | (value & 0xFFFF); return true;
    case ZYDIS_REGCLASS_GPR8: *slot = (*slot & ~0xFFull) | (value & 0xFF); return true;
    default: return false;
    }
}

bool KuserRead(loader::ScanRegs* r) {
#if WIN10XBOX_HAVE_CLOCK
    const auto* page = static_cast<const std::uint8_t*>(clock::VirtualSharedData());
    std::uint8_t bytes[16] = {};
    std::uint8_t len = 0;
    if (!page || !loader::KuserSiteBytes(r->rip, bytes, &len)) return false;
    ZydisDecoder dec;
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, bytes, len, &ins, ops)) || ins.operand_count_visible < 2 ||
        ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER)
        return false;
    constexpr std::uint64_t kBase = 0x7FFE0000ull;
    const auto& src = ops[1];
    if (src.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ins.mnemonic == ZYDIS_MNEMONIC_MOV) {
        if (src.imm.value.u < kBase || src.imm.value.u >= kBase + 0x1000) return false;
        return WriteGpr(r, ops[0].reg.value, reinterpret_cast<std::uint64_t>(page) + (src.imm.value.u - kBase), true);
    }
    if (src.type != ZYDIS_OPERAND_TYPE_MEMORY) return false;
    const std::uint64_t off = static_cast<std::uint64_t>(src.mem.disp.value) - kBase;
    if (src.mem.type == ZYDIS_MEMOP_TYPE_AGEN && ins.mnemonic == ZYDIS_MNEMONIC_LEA)
        return off < 0x1000 && WriteGpr(r, ops[0].reg.value, reinterpret_cast<std::uint64_t>(page) + off, true);
    if (src.mem.type != ZYDIS_MEMOP_TYPE_MEM || src.size == 0 || src.size > 64 || off + src.size / 8 > 0x1000)
        return false;
    bool sign = false;
    switch (ins.mnemonic) {
    case ZYDIS_MNEMONIC_MOV: case ZYDIS_MNEMONIC_MOVZX: break;
    case ZYDIS_MNEMONIC_MOVSX: case ZYDIS_MNEMONIC_MOVSXD: sign = true; break;
    default: return false;
    }
    std::uint64_t v = 0;
    std::memcpy(&v, page + off, src.size / 8);
    if (sign && src.size < 64 && (v >> (src.size - 1) & 1)) v |= ~0ull << src.size;
    return WriteGpr(r, ops[0].reg.value, v, false);
#else
    (void)r;
    return false;
#endif
}

void Syscall(loader::ScanRegs* r) {
    std::uint64_t args[kMaxArgs] = {};
    Frame f{};
    f.id = static_cast<std::uint32_t>(r->rax);
    f.nargs = win10xbox_syscall_nargs(f.id);
    args[0] = r->r10;
    args[1] = r->rdx;
    args[2] = r->r8;
    args[3] = r->r9;
    auto* stack = reinterpret_cast<const std::uint64_t*>(r->rsp);
    for (std::uint32_t i = 4; i < f.nargs; ++i) args[i] = stack[5 + (i - 4)];   // rsp+0x28 = arg 4
    f.args = args;
    f.return_address = stack[0];
    f.guest_rsp = r->rsp;
    std::int64_t status = win10xbox_dispatch(&f);
    r->rax = static_cast<std::uint32_t>(status);
    r->rcx = r->rip + 2;   // what `syscall` leaves behind
    r->r11 = r->rflags;
}

}  // namespace

// ---- RDRAND / RDSEED where the host has none (D18) ------------------------------------------------------------
// The console's CPU lacks both, and the CPUID patch advertises RDRAND (the identity is a 4700S, which has it): the
// instruction faults with #UD and is emulated here, as BCryptGenRandom bytes and CF=1 (success). A host that has the
// instructions never gets here.
namespace {
using BCryptGenRandomFn = LONG(WINAPI*)(void*, unsigned char*, ULONG, ULONG);
BCryptGenRandomFn g_bcrypt_gen_random = nullptr;
PVOID g_ud_veh = nullptr;
}  // namespace

namespace {
std::atomic<bool> g_force_random_trap{false};   // WIN10XBOX_TEST_FORCE_RDRAND_TRAP, latched in Launch (namespace scope: read by the scanner options)

// Fill `bytes` random bytes: BCryptGenRandom (system-preferred RNG), primed at install so that no loader work happens
// inside the exception handler (the first call loads and initialises the RNG provider).
bool GenerateRandom(void* out, ULONG bytes) {
    return g_bcrypt_gen_random &&
           g_bcrypt_gen_random(nullptr, static_cast<unsigned char*>(out), bytes, 2 /*BCRYPT_USE_SYSTEM_PREFERRED_RNG*/) >= 0;
}

// Decode `code` (the instruction at c->Rip, or its saved original bytes when the site was rewritten to ud2) and emulate it.
bool EmulateRandomAt(CONTEXT* c, const std::uint8_t* code, std::size_t avail) {
    ZydisDecoder dec;
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, code, avail, &ins, ops))) return false;
    if ((ins.mnemonic != ZYDIS_MNEMONIC_RDRAND && ins.mnemonic != ZYDIS_MNEMONIC_RDSEED) || ins.operand_count_visible < 1 ||
        ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER)
        return false;
    const ZydisRegister reg = ops[0].reg.value;
    const ZydisRegisterClass cls = ZydisRegisterGetClass(reg);
    const unsigned bytes = cls == ZYDIS_REGCLASS_GPR64 ? 8 : cls == ZYDIS_REGCLASS_GPR32 ? 4 : cls == ZYDIS_REGCLASS_GPR16 ? 2 : 0;
    const unsigned id = static_cast<unsigned>(ZydisRegisterGetId(reg));
    if (!bytes || id > 15) return false;
    std::uint64_t v = 0;
    if (!GenerateRandom(&v, bytes)) return false;
    DWORD64* slot = &c->Rax + id;   // CONTEXT keeps Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8..R15 in hardware order
    if (bytes == 8) *slot = v;
    else if (bytes == 4) *slot = v & 0xFFFFFFFFull;
    else *slot = (*slot & ~0xFFFFull) | (v & 0xFFFF);
    c->EFlags = (c->EFlags & ~0x8D5u) | 1u;   // CF=1; OF, SF, ZF, AF, PF cleared
    c->Rip += ins.length;
    return true;
}
}  // namespace

bool EmulateRandomInstruction(CONTEXT* c) {
    if (!g_bcrypt_gen_random) return false;
    std::uint8_t saved[16];
    std::uint8_t len = 0;
    if (loader::RandomSiteBytes(c->Rip, saved, &len)) return EmulateRandomAt(c, saved, len);   // a ud2 rewrite (test mode)
    return EmulateRandomAt(c, reinterpret_cast<const std::uint8_t*>(c->Rip), 15);
}

void SetForceRandomTrap(bool on) { g_force_random_trap.store(on); }
bool ForceRandomTrap() { return g_force_random_trap.load(); }

namespace {
LONG CALLBACK UdVeh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    HostModeScope host;
    return EmulateRandomInstruction(ep->ContextRecord) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void InstallRandomInstructionEmulation() {
    if (g_ud_veh) return;
    if (HMODULE bc = LoadLibraryW(L"bcrypt.dll"))
        g_bcrypt_gen_random = reinterpret_cast<BCryptGenRandomFn>(GetProcAddress(bc, "BCryptGenRandom"));
    if (!g_bcrypt_gen_random) {
        WARN(process, "bcrypt!BCryptGenRandom unavailable: RDRAND is advertised but not emulated");
        return;
    }
    // Prime: the first BCryptGenRandom loads the RNG provider, which must not happen inside the handler (the guest's
    // loader state is in place by then). A host where it fails has no emulation, and says so here, not at the fault.
    std::uint64_t probe = 0;
    if (!GenerateRandom(&probe, sizeof probe)) {
        ERR(process, "BCryptGenRandom failed while priming: RDRAND is advertised but not emulated");
        g_bcrypt_gen_random = nullptr;
        return;
    }
    g_ud_veh = AddVectoredExceptionHandler(1, UdVeh);   // first-call: ahead of every crash-capture handler (order 0)
    TRACE(process, "RDRAND/RDSEED #UD emulation %s", g_ud_veh ? "installed" : "NOT installed");
}

void SetCpuPolicy(const CpuPolicy& p) {
    AcquireSRWLockExclusive(&g_cpu_lock);
    Policy_() = p;
    ReleaseSRWLockExclusive(&g_cpu_lock);
}

CpuPolicy GetCpuPolicy() {
    AcquireSRWLockShared(&g_cpu_lock);
    CpuPolicy p = Policy_();
    ReleaseSRWLockShared(&g_cpu_lock);
    return p;
}

bool CpuSiteHandler(loader::SiteKind kind, loader::ScanRegs* regs, void*) {
    HostModeScope host;
    switch (kind) {
    case loader::SiteKind::Cpuid:
        Cpuid(regs);
        return true;
    case loader::SiteKind::Rdtsc: {
        std::uint64_t t = Tsc(nullptr);
        regs->rax = static_cast<std::uint32_t>(t);
        regs->rdx = t >> 32;
        return true;
    }
    case loader::SiteKind::Rdtscp: {
        std::uint32_t aux = 0;
        std::uint64_t t = Tsc(&aux);
        regs->rax = static_cast<std::uint32_t>(t);
        regs->rdx = t >> 32;
        regs->rcx = aux;
        return true;
    }
    case loader::SiteKind::Syscall:
    case loader::SiteKind::Int2E:
        Syscall(regs);
        return true;
    case loader::SiteKind::KuserRead:
        return KuserRead(regs);
    case loader::SiteKind::Random:   // never reaches a handler: the site is a ud2, served by UdVeh
        return false;
    default:
        return false;
    }
}

}  // namespace win10xbox::dispatch
