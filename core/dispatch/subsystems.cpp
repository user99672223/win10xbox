// Subsystem entry points wired as Emulate implementations. Each group is
// compiled in only when its module is built (WIN10XBOX_HAVE_<SUB>, set by
// core/dispatch/sources.cmake); without it the policy table falls back to
// pass-through for that group (see NeverPassThrough for the exceptions).
#include "internal.h"
#if WIN10XBOX_HAVE_REGISTRY
#include "win10xbox/registry.h"
#endif
#if WIN10XBOX_HAVE_TOKEN
#include "win10xbox/token.h"
#endif
#if WIN10XBOX_HAVE_CLOCK
#include "win10xbox/clock.h"
#endif
#if WIN10XBOX_HAVE_AFD
#include "win10xbox/afd.h"
#endif
#include <cstring>

namespace win10xbox::dispatch::detail {

#define W10X_IMPL(sys, fn) {#sys, reinterpret_cast<void*>(&fn)}

const ImplDef* SubsystemImpls() {
    static const ImplDef k[] = {
#if WIN10XBOX_HAVE_REGISTRY
        W10X_IMPL(NtOpenKey, registry::Reg_NtOpenKey),
        W10X_IMPL(NtOpenKeyEx, registry::Reg_NtOpenKeyEx),
        W10X_IMPL(NtOpenKeyTransacted, registry::Reg_NtOpenKeyTransacted),
        W10X_IMPL(NtOpenKeyTransactedEx, registry::Reg_NtOpenKeyTransactedEx),
        W10X_IMPL(NtCreateKey, registry::Reg_NtCreateKey),
        W10X_IMPL(NtCreateKeyTransacted, registry::Reg_NtCreateKeyTransacted),
        W10X_IMPL(NtDeleteKey, registry::Reg_NtDeleteKey),
        W10X_IMPL(NtDeleteValueKey, registry::Reg_NtDeleteValueKey),
        W10X_IMPL(NtEnumerateKey, registry::Reg_NtEnumerateKey),
        W10X_IMPL(NtEnumerateValueKey, registry::Reg_NtEnumerateValueKey),
        W10X_IMPL(NtFlushKey, registry::Reg_NtFlushKey),
        W10X_IMPL(NtNotifyChangeKey, registry::Reg_NtNotifyChangeKey),
        W10X_IMPL(NtNotifyChangeMultipleKeys, registry::Reg_NtNotifyChangeMultipleKeys),
        W10X_IMPL(NtQueryKey, registry::Reg_NtQueryKey),
        W10X_IMPL(NtQueryValueKey, registry::Reg_NtQueryValueKey),
        W10X_IMPL(NtQueryMultipleValueKey, registry::Reg_NtQueryMultipleValueKey),
        W10X_IMPL(NtRenameKey, registry::Reg_NtRenameKey),
        W10X_IMPL(NtSetValueKey, registry::Reg_NtSetValueKey),
        W10X_IMPL(NtSetInformationKey, registry::Reg_NtSetInformationKey),
        W10X_IMPL(NtLoadKey, registry::Reg_NtLoadKey),
        W10X_IMPL(NtLoadKeyEx, registry::Reg_NtLoadKeyEx),
        W10X_IMPL(NtLoadKey2, registry::Reg_NtLoadKey2),
        W10X_IMPL(NtUnloadKey, registry::Reg_NtUnloadKey),
        W10X_IMPL(NtSaveKey, registry::Reg_NtSaveKey),
        W10X_IMPL(NtRestoreKey, registry::Reg_NtRestoreKey),
        W10X_IMPL(NtReplaceKey, registry::Reg_NtReplaceKey),
#endif
#if WIN10XBOX_HAVE_TOKEN
        W10X_IMPL(NtOpenProcessToken, token::Tok_NtOpenProcessToken),
        W10X_IMPL(NtOpenProcessTokenEx, token::Tok_NtOpenProcessTokenEx),
        W10X_IMPL(NtOpenThreadToken, token::Tok_NtOpenThreadToken),
        W10X_IMPL(NtOpenThreadTokenEx, token::Tok_NtOpenThreadTokenEx),
        W10X_IMPL(NtQueryInformationToken, token::Tok_NtQueryInformationToken),
        W10X_IMPL(NtSetInformationToken, token::Tok_NtSetInformationToken),
        W10X_IMPL(NtAdjustPrivilegesToken, token::Tok_NtAdjustPrivilegesToken),
        W10X_IMPL(NtAdjustGroupsToken, token::Tok_NtAdjustGroupsToken),
        W10X_IMPL(NtDuplicateToken, token::Tok_NtDuplicateToken),
        W10X_IMPL(NtFilterToken, token::Tok_NtFilterToken),
        W10X_IMPL(NtImpersonateAnonymousToken, token::Tok_NtImpersonateAnonymousToken),
        W10X_IMPL(NtPrivilegeCheck, token::Tok_NtPrivilegeCheck),
        W10X_IMPL(NtAccessCheck, token::Tok_NtAccessCheck),
        W10X_IMPL(NtCompareTokens, token::Tok_NtCompareTokens),
        W10X_IMPL(NtCreateLowBoxToken, token::Tok_NtCreateLowBoxToken),
#endif
#if WIN10XBOX_HAVE_CLOCK
        W10X_IMPL(NtQueryPerformanceCounter, clock::Clk_NtQueryPerformanceCounter),
        W10X_IMPL(NtQuerySystemTime, clock::Clk_NtQuerySystemTime),
        W10X_IMPL(NtSetSystemTime, clock::Clk_NtSetSystemTime),
        W10X_IMPL(NtDelayExecution, clock::Clk_NtDelayExecution),
        W10X_IMPL(NtQueryTimerResolution, clock::Clk_NtQueryTimerResolution),
        W10X_IMPL(NtSetTimerResolution, clock::Clk_NtSetTimerResolution),
#endif
#if WIN10XBOX_HAVE_AFD
        W10X_IMPL(NtDeviceIoControlFile, afd::Afd_NtDeviceIoControlFile),
#endif
        {nullptr, nullptr},
    };
    return k;
}

bool NeverPassThrough(const char* name) {
    // Emulated because the host must never see them, not merely because a
    // subsystem owns them.
    static const char* const kNames[] = {"NtSetSystemTime"};
    for (const char* n : kNames)
        if (!std::strcmp(n, name)) return true;
    return false;
}

}  // namespace win10xbox::dispatch::detail
