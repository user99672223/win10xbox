// win10xbox registry: real REGF hives (SYSTEM, SOFTWARE, NTUSER.DAT, ...)
// read through hivex, with an in-memory overlay that absorbs every write
// (SetValue/CreateKey/Delete*) so supplied hives are never modified.
// Owned by core/registry/ (+ machine/hivex/ for building hivex).
//
// The dispatcher routes NtOpenKey/NtCreateKey/NtQueryValueKey/... to the
// Reg_Nt* functions below, which have the exact NT signatures and do the
// NT-level marshalling (KEY_INFORMATION_CLASS, KEY_VALUE_INFORMATION_CLASS,
// buffer-too-small semantics: STATUS_BUFFER_TOO_SMALL vs
// STATUS_BUFFER_OVERFLOW exactly as Windows does).
#pragma once
#include <windows.h>
#include <winternl.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace win10xbox::registry {

// Hive files live in <machine_root>\Windows\System32\config\{SYSTEM,SOFTWARE,
// SAM,SECURITY,DEFAULT} and <machine_root>\Users\<user>\NTUSER.DAT. Missing
// hives are created empty in the overlay (with the 22H2 fidelity keys from
// machine.json "windows" applied: CurrentVersion, ProductName, CurrentBuild...).
NTSTATUS Initialize(const std::wstring& machine_root);
void Shutdown();

// Root names as NT paths: \Registry\Machine, \Registry\User, \Registry\User\<sid>,
// \Registry\Machine\SYSTEM\CurrentControlSet (link to ControlSet001).

// Key handles are opaque 64-bit ids owned by this module; the dispatcher
// wraps them in guest HANDLE values via its emulated-object table and calls
// Close() on NtClose. IsKeyHandle() lets the dispatcher recognise them.
using KeyId = std::uint64_t;
bool IsKeyHandle(HANDLE h);
HANDLE ToHandle(KeyId id);
KeyId FromHandle(HANDLE h);

// NT-signature entry points (names mirror the syscalls they implement).
NTSTATUS Reg_NtOpenKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS Reg_NtOpenKeyEx(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, ULONG OpenOptions);
NTSTATUS Reg_NtCreateKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                         ULONG TitleIndex, PUNICODE_STRING Class, ULONG CreateOptions, PULONG Disposition);
NTSTATUS Reg_NtCreateKeyTransacted(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, HANDLE, PULONG);
NTSTATUS Reg_NtDeleteKey(HANDLE KeyHandle);
NTSTATUS Reg_NtDeleteValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName);
NTSTATUS Reg_NtEnumerateKey(HANDLE KeyHandle, ULONG Index, KEY_INFORMATION_CLASS KeyInformationClass,
                            PVOID KeyInformation, ULONG Length, PULONG ResultLength);
NTSTATUS Reg_NtEnumerateValueKey(HANDLE KeyHandle, ULONG Index, KEY_VALUE_INFORMATION_CLASS KeyValueInformationClass,
                                 PVOID KeyValueInformation, ULONG Length, PULONG ResultLength);
NTSTATUS Reg_NtFlushKey(HANDLE KeyHandle);
NTSTATUS Reg_NtNotifyChangeKey(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, ULONG, BOOLEAN, PVOID, ULONG, BOOLEAN);
NTSTATUS Reg_NtNotifyChangeMultipleKeys(HANDLE, ULONG, POBJECT_ATTRIBUTES, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, ULONG, BOOLEAN, PVOID, ULONG, BOOLEAN);
NTSTATUS Reg_NtQueryKey(HANDLE KeyHandle, KEY_INFORMATION_CLASS KeyInformationClass,
                        PVOID KeyInformation, ULONG Length, PULONG ResultLength);
NTSTATUS Reg_NtQueryValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, KEY_VALUE_INFORMATION_CLASS KeyValueInformationClass,
                             PVOID KeyValueInformation, ULONG Length, PULONG ResultLength);
NTSTATUS Reg_NtQueryMultipleValueKey(HANDLE, PKEY_VALUE_ENTRY, ULONG, PVOID, PULONG, PULONG);
NTSTATUS Reg_NtRenameKey(HANDLE KeyHandle, PUNICODE_STRING NewName);
NTSTATUS Reg_NtSetValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG TitleIndex, ULONG Type, PVOID Data, ULONG DataSize);
NTSTATUS Reg_NtSetInformationKey(HANDLE, int KeySetInformationClass, PVOID, ULONG);
NTSTATUS Reg_NtLoadKey(POBJECT_ATTRIBUTES TargetKey, POBJECT_ATTRIBUTES SourceFile);
NTSTATUS Reg_NtLoadKeyEx(POBJECT_ATTRIBUTES, POBJECT_ATTRIBUTES, ULONG, HANDLE, HANDLE, ACCESS_MASK, PHANDLE, PVOID);
NTSTATUS Reg_NtUnloadKey(POBJECT_ATTRIBUTES TargetKey);
NTSTATUS Reg_NtSaveKey(HANDLE KeyHandle, HANDLE FileHandle);
NTSTATUS Reg_NtRestoreKey(HANDLE KeyHandle, HANDLE FileHandle, ULONG Flags);
NTSTATUS Reg_NtReplaceKey(POBJECT_ATTRIBUTES, HANDLE, POBJECT_ATTRIBUTES);
NTSTATUS Reg_NtQueryOpenSubKeys(POBJECT_ATTRIBUTES, PULONG);
NTSTATUS Close(HANDLE KeyHandle);

// Convenience for other core modules and tests (no NT marshalling):
struct Value { std::wstring name; ULONG type = 0; std::vector<std::uint8_t> data; };
NTSTATUS OpenKey(std::wstring_view nt_path, KeyId* out);
NTSTATUS QueryValue(KeyId key, std::wstring_view name, Value* out);
NTSTATUS QueryString(std::wstring_view nt_path, std::wstring_view name, std::wstring* out);
NTSTATUS QueryDword(std::wstring_view nt_path, std::wstring_view name, std::uint32_t* out);
NTSTATUS SetValue(KeyId key, const Value& v);
NTSTATUS EnumerateSubkeys(KeyId key, std::vector<std::wstring>* out);
NTSTATUS EnumerateValues(KeyId key, std::vector<Value>* out);
void CloseKey(KeyId key);

// Overlay persistence (machine folder\registry-overlay.bin): optional.
NTSTATUS SaveOverlay();
NTSTATUS LoadOverlay();

}  // namespace win10xbox::registry
