// win10xbox token/identity emulation: the guest sees a normal interactive
// user on a normal desktop (not an AppContainer): a user SID from
// machine.json "identity", the usual groups (Users, INTERACTIVE, Authenticated
// Users, LOCAL, CONSOLE LOGON, medium integrity), a non-elevated limited
// token, session 1. Owned by core/token/.
//
// Host tokens are never exposed to the guest. The dispatcher routes the
// token syscalls below here; handles are opaque ids wrapped like registry
// keys.
#pragma once
#include <windows.h>
#include <winternl.h>
#include <cstdint>
#include <string>
#include <vector>

namespace win10xbox::token {

struct Identity {
    std::wstring user_name = L"User";
    std::wstring domain = L"DESKTOP-XBOX";
    std::wstring user_sid = L"S-1-5-21-1004336348-1177238915-682003330-1001";
    std::wstring computer_name = L"DESKTOP-XBOX";
    std::uint32_t session_id = 1;
    bool elevated = false;          // TokenElevation / TokenElevationType (Limited unless true)
    std::uint32_t integrity_rid = 0x2000;   // SECURITY_MANDATORY_MEDIUM_RID
};

NTSTATUS Initialize(const Identity& id);
const Identity& Current();

using TokenId = std::uint64_t;
bool   IsTokenHandle(HANDLE h);
HANDLE ToHandle(TokenId id);
TokenId FromHandle(HANDLE h);
TokenId PrimaryToken();                 // the process token

// NT-signature entry points.
NTSTATUS Tok_NtOpenProcessToken(HANDLE ProcessHandle, ACCESS_MASK DesiredAccess, PHANDLE TokenHandle);
NTSTATUS Tok_NtOpenProcessTokenEx(HANDLE ProcessHandle, ACCESS_MASK DesiredAccess, ULONG HandleAttributes, PHANDLE TokenHandle);
NTSTATUS Tok_NtOpenThreadToken(HANDLE ThreadHandle, ACCESS_MASK DesiredAccess, BOOLEAN OpenAsSelf, PHANDLE TokenHandle);
NTSTATUS Tok_NtOpenThreadTokenEx(HANDLE ThreadHandle, ACCESS_MASK DesiredAccess, BOOLEAN OpenAsSelf, ULONG HandleAttributes, PHANDLE TokenHandle);
NTSTATUS Tok_NtQueryInformationToken(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass,
                                     PVOID TokenInformation, ULONG TokenInformationLength, PULONG ReturnLength);
NTSTATUS Tok_NtSetInformationToken(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass, PVOID TokenInformation, ULONG Length);
NTSTATUS Tok_NtAdjustPrivilegesToken(HANDLE TokenHandle, BOOLEAN DisableAllPrivileges, PTOKEN_PRIVILEGES NewState,
                                     ULONG BufferLength, PTOKEN_PRIVILEGES PreviousState, PULONG ReturnLength);
NTSTATUS Tok_NtAdjustGroupsToken(HANDLE, BOOLEAN, PTOKEN_GROUPS, ULONG, PTOKEN_GROUPS, PULONG);
NTSTATUS Tok_NtDuplicateToken(HANDLE ExistingTokenHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                              BOOLEAN EffectiveOnly, TOKEN_TYPE TokenType, PHANDLE NewTokenHandle);
NTSTATUS Tok_NtFilterToken(HANDLE, ULONG, PTOKEN_GROUPS, PTOKEN_PRIVILEGES, PTOKEN_GROUPS, PHANDLE);
NTSTATUS Tok_NtImpersonateAnonymousToken(HANDLE ThreadHandle);
NTSTATUS Tok_NtPrivilegeCheck(HANDLE, PPRIVILEGE_SET, PBOOLEAN);
NTSTATUS Tok_NtAccessCheck(PSECURITY_DESCRIPTOR, HANDLE, ACCESS_MASK, PGENERIC_MAPPING, PPRIVILEGE_SET, PULONG, PACCESS_MASK, PNTSTATUS);
NTSTATUS Tok_NtCompareTokens(HANDLE, HANDLE, PBOOLEAN);
NTSTATUS Tok_NtQuerySecurityObject(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, ULONG, PULONG);  // for token handles only
NTSTATUS Close(HANDLE TokenHandle);

// Helpers used by registry (HKCU path), VFS (user profile) and the PEB spoof.
std::wstring UserSidString();
std::wstring UserName();
std::wstring ComputerName();

}  // namespace win10xbox::token
