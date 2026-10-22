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

// A TokenId names one guest HANDLE to a token (several ids may share one token
// object, as in NT). Handle values are 31-bit: 0x54000000 | id << 2. The NT
// pseudo handles -4 (process token), -5 (thread token), -6 (effective token)
// are recognised by IsTokenHandle() and accepted by every Tok_Nt* call, but have
// no TokenId (FromHandle returns 0 for them).
using TokenId = std::uint64_t;
bool   IsTokenHandle(HANDLE h);
HANDLE ToHandle(TokenId id);
TokenId FromHandle(HANDLE h);           // 0 if not a token handle
TokenId PrimaryToken();                 // id of the module's own permanent, full-access handle on the process token (Close() on it is a no-op)

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
NTSTATUS Tok_NtAccessCheck(PSECURITY_DESCRIPTOR, HANDLE, ACCESS_MASK, PGENERIC_MAPPING, PPRIVILEGE_SET, PULONG, PACCESS_MASK, NTSTATUS*);  // PNTSTATUS is not declared by MSVC winternl.h
NTSTATUS Tok_NtCompareTokens(HANDLE, HANDLE, PBOOLEAN);
// AppContainer (LowBox) tokens are not emulated: the guest is a medium-integrity desktop user, never an AppContainer
// (docs/DECISIONS.md token note). Validates its arguments like NT, then answers STATUS_NOT_SUPPORTED with a FIXME.
NTSTATUS Tok_NtCreateLowBoxToken(PHANDLE NewTokenHandle, HANDLE ExistingTokenHandle, ACCESS_MASK DesiredAccess,
                                 POBJECT_ATTRIBUTES ObjectAttributes, PSID PackageSid, ULONG CapabilityCount,
                                 PSID_AND_ATTRIBUTES Capabilities, ULONG HandleCount, HANDLE* Handles);
NTSTATUS Tok_NtQuerySecurityObject(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, ULONG, PULONG);  // for token handles only
NTSTATUS Close(HANDLE TokenHandle);

// Extensions -------------------------------------------------------------
// NtSetInformationThread(ThreadImpersonationToken) lands here: make `TokenHandle`
// the impersonation token of `ThreadHandle` (nullptr/0 reverts). A primary token
// is converted to an impersonation copy like NT does. Tok_NtOpenThreadToken and
// pseudo handle -5/-6 observe it.
NTSTATUS SetThreadImpersonationToken(HANDLE ThreadHandle, HANDLE TokenHandle);

// SID string <-> binary SID (documented layout: revision 1, count, 6-byte
// big-endian authority, little-endian DWORD sub-authorities).
bool SidFromString(const std::wstring& s, std::vector<std::uint8_t>* out);
std::wstring SidToString(const void* sid);

// Helpers used by registry (HKCU path), VFS (user profile) and the PEB spoof.
std::wstring UserSidString();
std::wstring UserName();
std::wstring ComputerName();

}  // namespace win10xbox::token
