// Privileged syscalls (core/dispatch/nt_privileged.cpp): each is an Emulate
// entry that answers the way Windows answers a caller holding no privilege,
// and the answer comes from the implementation, not from the host.
#include "check.h"
#include "win10xbox/dispatch.h"
#include <cstdint>

using namespace win10xbox;
using namespace win10xbox::dispatch;

namespace {

struct Expect {
    const char* name;
    std::uint32_t nargs;   // argument count in the generated table (Wine's ntdll.spec)
    NTSTATUS status;
};

constexpr NTSTATUS kPrivilegeNotHeld = static_cast<NTSTATUS>(0xC0000061);
constexpr NTSTATUS kAccessDenied = static_cast<NTSTATUS>(0xC0000022);
constexpr NTSTATUS kNotImplemented = static_cast<NTSTATUS>(0xC0000002);

const Expect kPrivileged[] = {
    {"NtLoadDriver", 1, kPrivilegeNotHeld},
    {"NtUnloadDriver", 1, kPrivilegeNotHeld},
    {"NtShutdownSystem", 1, kPrivilegeNotHeld},
    {"NtInitiatePowerAction", 4, kPrivilegeNotHeld},
    {"NtCreatePagingFile", 4, kPrivilegeNotHeld},
    {"NtSetLdtEntries", 6, kNotImplemented},
    {"NtSystemDebugControl", 6, kAccessDenied},
    {"NtCreateToken", 13, kPrivilegeNotHeld},
};

}  // namespace

TEST(privileged_syscalls_are_emulated) {
    CHECK_EQ(Initialize(), 0);
    for (const Expect& x : kPrivileged) {
        const Entry* e = LookupByName(x.name);
        CHECK_MSG(e != nullptr, "%s is not in the table", x.name);
        if (!e) continue;
        CHECK_MSG(e->policy == Policy::Emulate, "%s policy is %s", x.name, PolicyName(e->policy));
        CHECK_MSG(e->impl != nullptr, "%s has no implementation", x.name);
        CHECK_MSG(e->nargs == x.nargs, "%s has %u args", x.name, static_cast<unsigned>(e->nargs));
    }
}

TEST(privileged_syscalls_refused) {
    CHECK_EQ(Initialize(), 0);
    std::uint64_t zeros[20] = {};
    for (const Expect& x : kPrivileged) {
        const Entry* e = LookupByName(x.name);
        if (!e || !e->impl) continue;   // reported by privileged_syscalls_are_emulated
        NTSTATUS s = CallWithArgs(e->impl, zeros, e->nargs);
        CHECK_MSG(s == x.status, "%s returned %#lx", x.name, static_cast<unsigned long>(s));
    }
}
