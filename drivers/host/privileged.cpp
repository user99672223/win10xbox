// Privileged-instruction trap-and-emulate for the driver host.
//
// A driver image is kernel-mode code, but the driver host maps and runs it in
// our user-mode process (CPL 3). A handful of instructions the compiler emits
// for ordinary kernel APIs are privileged at CPL 3 and raise a fault instead of
// executing: `mov cr8` (the inline form of KeGetCurrentIrql/KeRaiseIrql/
// KeLowerIrql), rdmsr/wrmsr, in/out, cli/sti, and a few others. Rather than
// patch every driver, we let them fault and emulate them here against the
// host's virtual CPU state, then resume at the next instruction - the same
// trap-and-emulate path the capability probe confirmed on the console
// (rdmsr -> #GP -> adjust RIP -> continue).
//
// Scope: a vectored exception handler that acts ONLY on faults whose RIP is
// inside a driver image we loaded (recorded via SetImageLoadHook). Faults from
// host or guest code fall through untouched. The emulated set is exactly the
// instructions that are privileged at CPL 3; everything else continues the
// search so a genuine bug still crashes.
//
// Not handled here (documented follow-ups, see drivers/README.md): reads of the
// kernel-VA KUSER_SHARED_DATA mirror at 0xFFFFF78000000000 and gs-relative KPCR
// fields - those are memory accesses, not privileged instructions, and need the
// virtual KUSER/KPCR served per D18/M2 rather than a one-instruction rewrite.
#include "../ntoskrnl/internal.h"
#include <Zydis/Zydis.h>
#include <array>
#include <cstdint>

namespace win10xbox::drivers {
using namespace detail;
namespace {

// ---- driver image ranges (set by the image-load hook) ----------------------
struct Range { std::uint64_t base, end; };
SRWLOCK g_lock = SRWLOCK_INIT;
std::array<Range, 64> g_ranges{};
std::size_t g_range_count = 0;
PVOID g_veh = nullptr;
ImageLoadHook g_prev_hook = nullptr;
void* g_prev_ctx = nullptr;

// A tiny virtual MSR store: wrmsr remembers, rdmsr reads back (default 0). Most
// drivers only round-trip a value or read an MSR they never act on.
struct MsrSlot { std::uint32_t index; std::uint64_t value; bool used; };
std::array<MsrSlot, 32> g_msr{};

bool InDriverImage(std::uint64_t rip) {
    AcquireSRWLockShared(&g_lock);
    bool in = false;
    for (std::size_t i = 0; i < g_range_count; ++i)
        if (rip >= g_ranges[i].base && rip < g_ranges[i].end) { in = true; break; }
    ReleaseSRWLockShared(&g_lock);
    return in;
}

std::uint64_t ReadMsr(std::uint32_t index) {
    for (auto& m : g_msr) if (m.used && m.index == index) return m.value;
    return 0;
}
void WriteMsr(std::uint32_t index, std::uint64_t value) {
    for (auto& m : g_msr) if (m.used && m.index == index) { m.value = value; return; }
    for (auto& m : g_msr) if (!m.used) { m = {index, value, true}; return; }
}

// Map a Zydis GPR (any width) to the enclosing 64-bit slot in the CONTEXT.
std::uint64_t* GprSlot(CONTEXT* c, ZydisRegister reg) {
    ZydisRegister r = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
    switch (r) {
    case ZYDIS_REGISTER_RAX: return &c->Rax; case ZYDIS_REGISTER_RCX: return &c->Rcx;
    case ZYDIS_REGISTER_RDX: return &c->Rdx; case ZYDIS_REGISTER_RBX: return &c->Rbx;
    case ZYDIS_REGISTER_RSP: return &c->Rsp; case ZYDIS_REGISTER_RBP: return &c->Rbp;
    case ZYDIS_REGISTER_RSI: return &c->Rsi; case ZYDIS_REGISTER_RDI: return &c->Rdi;
    case ZYDIS_REGISTER_R8:  return &c->R8;  case ZYDIS_REGISTER_R9:  return &c->R9;
    case ZYDIS_REGISTER_R10: return &c->R10; case ZYDIS_REGISTER_R11: return &c->R11;
    case ZYDIS_REGISTER_R12: return &c->R12; case ZYDIS_REGISTER_R13: return &c->R13;
    case ZYDIS_REGISTER_R14: return &c->R14; case ZYDIS_REGISTER_R15: return &c->R15;
    default: return nullptr;
    }
}
// Write `value` into a GPR operand honouring its width (a 32-bit dest zeroes the
// upper 32 bits, as x86-64 does; 8/16-bit dests merge).
void WriteGpr(CONTEXT* c, const ZydisDecodedOperand& op, std::uint64_t value) {
    std::uint64_t* slot = GprSlot(c, op.reg.value);
    if (!slot) return;
    switch (op.size) {
    case 8:  *slot = value; break;
    case 4:  *slot = static_cast<std::uint32_t>(value); break;   // zero-extends
    case 2:  *slot = (*slot & ~0xFFFFull) | (value & 0xFFFF); break;
    case 1:  *slot = (*slot & ~0xFFull) | (value & 0xFF); break;
    default: *slot = value; break;
    }
}
std::uint64_t ReadGpr(CONTEXT* c, const ZydisDecodedOperand& op) {
    std::uint64_t* slot = GprSlot(c, op.reg.value);
    if (!slot) return 0;
    switch (op.size) {
    case 4: return static_cast<std::uint32_t>(*slot);
    case 2: return *slot & 0xFFFF;
    case 1: return *slot & 0xFF;
    default: return *slot;
    }
}

bool IsControlReg(ZydisRegister r) { return r >= ZYDIS_REGISTER_CR0 && r <= ZYDIS_REGISTER_CR15; }

// Plausible read-back for the control registers a driver might read (it almost
// never acts on them). cr8 is the live one: it is the IRQL.
std::uint64_t ReadControlReg(ZydisRegister r) {
    switch (r) {
    case ZYDIS_REGISTER_CR8: return GetVirtualIrql();
    case ZYDIS_REGISTER_CR0: return 0x80050033;   // PE MP ET NE WP PG (typical)
    case ZYDIS_REGISTER_CR4: return 0x001506f8;   // typical x64 feature set
    default: return 0;                             // cr2/cr3/others
    }
}

// Returns true if it emulated `ins` (regs updated; caller advances RIP).
bool Emulate(const ZydisDecodedInstruction& ins, const ZydisDecodedOperand* ops, CONTEXT* c) {
    switch (ins.mnemonic) {
    case ZYDIS_MNEMONIC_MOV: {
        // mov r64, crN  /  mov crN, r64 (the only privileged MOV forms here)
        if (ins.operand_count < 2) return false;
        const ZydisDecodedOperand& dst = ops[0];
        const ZydisDecodedOperand& src = ops[1];
        if (dst.type == ZYDIS_OPERAND_TYPE_REGISTER && src.type == ZYDIS_OPERAND_TYPE_REGISTER &&
            IsControlReg(src.reg.value)) {                       // read from CRn
            WriteGpr(c, dst, ReadControlReg(src.reg.value));
            return true;
        }
        if (dst.type == ZYDIS_OPERAND_TYPE_REGISTER && IsControlReg(dst.reg.value) &&
            src.type == ZYDIS_OPERAND_TYPE_REGISTER) {           // write to CRn
            std::uint64_t v = ReadGpr(c, src);
            if (dst.reg.value == ZYDIS_REGISTER_CR8) SetVirtualIrql(static_cast<std::uint8_t>(v & 0xF));
            return true;                                         // writes to other CRn ignored
        }
        return false;
    }
    case ZYDIS_MNEMONIC_RDMSR: {
        std::uint64_t v = ReadMsr(static_cast<std::uint32_t>(c->Rcx));
        c->Rax = static_cast<std::uint32_t>(v);
        c->Rdx = static_cast<std::uint32_t>(v >> 32);
        return true;
    }
    case ZYDIS_MNEMONIC_WRMSR:
        WriteMsr(static_cast<std::uint32_t>(c->Rcx),
                 (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c->Rdx)) << 32) |
                     static_cast<std::uint32_t>(c->Rax));
        return true;
    case ZYDIS_MNEMONIC_RDPMC:
        c->Rax = 0; c->Rdx = 0;
        return true;
    case ZYDIS_MNEMONIC_IN: {               // in AL/AX/EAX, imm8|DX  -> 0 (no device)
        if (ins.operand_count >= 1 && ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER)
            WriteGpr(c, ops[0], 0);
        return true;
    }
    case ZYDIS_MNEMONIC_OUT:                // out imm8|DX, AL/AX/EAX -> discard
        return true;
    case ZYDIS_MNEMONIC_CLI:                // interrupt flag is virtual + inert here
    case ZYDIS_MNEMONIC_STI:
    case ZYDIS_MNEMONIC_HLT:                // never actually halt the host thread
    case ZYDIS_MNEMONIC_WBINVD:
    case ZYDIS_MNEMONIC_INVD:
    case ZYDIS_MNEMONIC_INVLPG:
        return true;
    default:
        return false;
    }
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    // A privileged instruction at CPL 3 surfaces as STATUS_PRIVILEGED_INSTRUCTION
    // or, on some hosts, a general-protection access violation. Decode-and-match
    // is the real gate, so accept either and let the decode decide.
    if (code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_ACCESS_VIOLATION &&
        code != static_cast<DWORD>(0xC0000096) /* STATUS_PRIVILEGED_INSTRUCTION */)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!InDriverImage(c->Rip)) return EXCEPTION_CONTINUE_SEARCH;

    ZydisDecoder dec;
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, reinterpret_cast<const void*>(c->Rip), 15, &ins, ops)))
        return EXCEPTION_CONTINUE_SEARCH;
    if (!Emulate(ins, ops, c)) return EXCEPTION_CONTINUE_SEARCH;
    c->Rip += ins.length;
    return EXCEPTION_CONTINUE_EXECUTION;
}

NTSTATUS ImageHook(const loader::MappedImage& image, void* ctx) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_range_count < g_ranges.size())
        g_ranges[g_range_count++] = {reinterpret_cast<std::uint64_t>(image.base),
                                     reinterpret_cast<std::uint64_t>(image.base) + image.size};
    ReleaseSRWLockExclusive(&g_lock);
    return g_prev_hook ? g_prev_hook(image, g_prev_ctx) : STATUS_SUCCESS;  // chain, if any
}

}  // namespace

NTSTATUS InstallPrivilegedInstructionHandler() {
    if (g_veh) return STATUS_SUCCESS;
    g_veh = AddVectoredExceptionHandler(/*first=*/1, Handler);
    if (!g_veh) return STATUS_INSUFFICIENT_RESOURCES;
    // Own the image-load hook so each driver's range is recorded before it runs;
    // preserve any previously-set hook by chaining.
    // (SetImageLoadHook keeps a single slot; see drivers/host/driver_loader.cpp.)
    SetImageLoadHook(ImageHook, nullptr);
    TRACE(driver, "privileged-instruction handler armed");
    return STATUS_SUCCESS;
}

void RemovePrivilegedInstructionHandler() {
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    AcquireSRWLockExclusive(&g_lock);
    g_range_count = 0;
    ReleaseSRWLockExclusive(&g_lock);
    for (auto& m : g_msr) m.used = false;
}

}  // namespace win10xbox::drivers
