// Handlers for instruction sites patched by the loader's scanner (D9/D10):
// cpuid (configured vendor, hypervisor hidden), rdtsc/rdtscp (virtual clock)
// and raw syscall / int 2Eh in guest code (routed into the dispatcher).
#include "internal.h"
#include "win10xbox/config.h"
#if WIN10XBOX_HAVE_CLOCK
#include "win10xbox/clock.h"
#endif
#include <cstring>

namespace win10xbox::dispatch {
namespace {

SRWLOCK g_cpu_lock = SRWLOCK_INIT;
CpuPolicy& Policy_() { static CpuPolicy p; return p; }

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
    default:
        return false;
    }
}

}  // namespace win10xbox::dispatch
