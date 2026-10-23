// Privileged syscalls: answered the way Windows answers a caller that holds
// no privilege. The guest is an unprivileged, sandboxed process, so loading a
// driver, shutting the machine down, changing the power state, creating a
// paging file or forging a token must not reach the host, and nothing here
// calls it. Every entry traces once per call on the dispatch channel.
//
// Signatures follow Wine's winternl.h / ntdll.spec (argument counts match the
// table in syscall_table.gen.inc). Types the headers this module builds against
// do not declare (POWER_ACTION, SYSTEM_POWER_STATE, SHUTDOWN_ACTION,
// SYSDBG_COMMAND) are ULONG here, which is what the registers carry.
#include "internal.h"

namespace win10xbox::dispatch::detail {
namespace {

NTSTATUS Answer(const char* name, NTSTATUS status, const char* why) {
    TRACE(dispatch, "%s -> %#lx (%s)", name, static_cast<unsigned long>(status), why);
    return status;
}

constexpr const char* kNoPrivilege = "caller holds no privilege, not forwarded to the host";

NTSTATUS NTAPI Priv_NtLoadDriver(const UNICODE_STRING* /*DriverServiceName*/) {
    return Answer("NtLoadDriver", st::PrivilegeNotHeld, kNoPrivilege);
}

NTSTATUS NTAPI Priv_NtUnloadDriver(const UNICODE_STRING* /*DriverServiceName*/) {
    return Answer("NtUnloadDriver", st::PrivilegeNotHeld, kNoPrivilege);
}

NTSTATUS NTAPI Priv_NtShutdownSystem(ULONG /*SHUTDOWN_ACTION*/) {
    return Answer("NtShutdownSystem", st::PrivilegeNotHeld, kNoPrivilege);
}

NTSTATUS NTAPI Priv_NtInitiatePowerAction(ULONG /*POWER_ACTION*/, ULONG /*SYSTEM_POWER_STATE*/, ULONG /*Flags*/,
                                          BOOLEAN /*Asynchronous*/) {
    return Answer("NtInitiatePowerAction", st::PrivilegeNotHeld, kNoPrivilege);
}

NTSTATUS NTAPI Priv_NtCreatePagingFile(PUNICODE_STRING /*PageFileName*/, PLARGE_INTEGER /*MinimumSize*/,
                                       PLARGE_INTEGER /*MaximumSize*/, PLARGE_INTEGER /*Reserved*/) {
    return Answer("NtCreatePagingFile", st::PrivilegeNotHeld, kNoPrivilege);
}

// x64 has no LDT, and Windows answers STATUS_NOT_IMPLEMENTED for the call.
// The value is the real one; it is returned here, not by the host.
NTSTATUS NTAPI Priv_NtSetLdtEntries(ULONG, ULONG, ULONG, ULONG, ULONG, ULONG) {
    return Answer("NtSetLdtEntries", st::NotImplemented, "x64 has no LDT");
}

// Debugging the system needs SeDebugPrivilege, which the caller lacks, so the
// answer is always ACCESS_DENIED. The host kernel is deliberately not asked.
NTSTATUS NTAPI Priv_NtSystemDebugControl(ULONG /*SYSDBG_COMMAND*/, PVOID, ULONG, PVOID, ULONG, PULONG) {
    return Answer("NtSystemDebugControl", st::AccessDenied, "needs SeDebugPrivilege, not forwarded to the host");
}

NTSTATUS NTAPI Priv_NtCreateToken(PHANDLE /*TokenHandle*/, ACCESS_MASK /*DesiredAccess*/,
                                  POBJECT_ATTRIBUTES /*ObjectAttributes*/, TOKEN_TYPE /*TokenType*/,
                                  PLUID /*AuthenticationId*/, PLARGE_INTEGER /*ExpirationTime*/,
                                  PTOKEN_USER /*TokenUser*/, PTOKEN_GROUPS /*TokenGroups*/,
                                  PTOKEN_PRIVILEGES /*TokenPrivileges*/, PTOKEN_OWNER /*TokenOwner*/,
                                  PTOKEN_PRIMARY_GROUP /*TokenPrimaryGroup*/,
                                  PTOKEN_DEFAULT_DACL /*TokenDefaultDacl*/, PTOKEN_SOURCE /*TokenSource*/) {
    return Answer("NtCreateToken", st::PrivilegeNotHeld, kNoPrivilege);
}

}  // namespace

const ImplDef* PrivilegedImpls() {
    static const ImplDef k[] = {
        {"NtLoadDriver", reinterpret_cast<void*>(&Priv_NtLoadDriver)},
        {"NtUnloadDriver", reinterpret_cast<void*>(&Priv_NtUnloadDriver)},
        {"NtShutdownSystem", reinterpret_cast<void*>(&Priv_NtShutdownSystem)},
        {"NtInitiatePowerAction", reinterpret_cast<void*>(&Priv_NtInitiatePowerAction)},
        {"NtCreatePagingFile", reinterpret_cast<void*>(&Priv_NtCreatePagingFile)},
        {"NtSetLdtEntries", reinterpret_cast<void*>(&Priv_NtSetLdtEntries)},
        {"NtSystemDebugControl", reinterpret_cast<void*>(&Priv_NtSystemDebugControl)},
        {"NtCreateToken", reinterpret_cast<void*>(&Priv_NtCreateToken)},
        {nullptr, nullptr},
    };
    return k;
}

}  // namespace win10xbox::dispatch::detail
