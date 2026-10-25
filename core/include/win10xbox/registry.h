// win10xbox registry: real REGF hives (SYSTEM, SOFTWARE, NTUSER.DAT, ...)
// read through core/registry/regf.cpp (own REGF reader/writer; hivex was not
// used, see machine/hivex/README.md), with an in-memory overlay that absorbs every write
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
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace win10xbox::registry {

// NTSTATUS values returned by this module, spelled out here so callers and
// tests need not juggle <ntstatus.h> / WIN32_NO_STATUS.
namespace nt {
constexpr NTSTATUS kSuccess             = 0;
constexpr NTSTATUS kPending             = 0x00000103;
constexpr NTSTATUS kBufferOverflow      = static_cast<NTSTATUS>(0x80000005);
constexpr NTSTATUS kNoMoreEntries       = static_cast<NTSTATUS>(0x8000001A);
constexpr NTSTATUS kUnsuccessful        = static_cast<NTSTATUS>(0xC0000001);
constexpr NTSTATUS kNotImplemented      = static_cast<NTSTATUS>(0xC0000002);
constexpr NTSTATUS kInvalidInfoClass    = static_cast<NTSTATUS>(0xC0000003);
constexpr NTSTATUS kInfoLengthMismatch  = static_cast<NTSTATUS>(0xC0000004);
constexpr NTSTATUS kInvalidHandle       = static_cast<NTSTATUS>(0xC0000008);
constexpr NTSTATUS kInvalidParameter    = static_cast<NTSTATUS>(0xC000000D);
constexpr NTSTATUS kAccessDenied        = static_cast<NTSTATUS>(0xC0000022);
constexpr NTSTATUS kBufferTooSmall      = static_cast<NTSTATUS>(0xC0000023);
constexpr NTSTATUS kObjectTypeMismatch  = static_cast<NTSTATUS>(0xC0000024);
constexpr NTSTATUS kObjectNameInvalid   = static_cast<NTSTATUS>(0xC0000033);
constexpr NTSTATUS kObjectNameNotFound  = static_cast<NTSTATUS>(0xC0000034);
constexpr NTSTATUS kObjectNameCollision = static_cast<NTSTATUS>(0xC0000035);
constexpr NTSTATUS kObjectPathSyntaxBad = static_cast<NTSTATUS>(0xC000003B);
constexpr NTSTATUS kKeyDeleted          = static_cast<NTSTATUS>(0xC000017C);
constexpr NTSTATUS kCannotDelete        = static_cast<NTSTATUS>(0xC0000121);
}  // namespace nt

// KEY_INFORMATION_CLASS / KEY_VALUE_INFORMATION_CLASS numeric values. The
// Reg_Nt* entry points take the raw ULONG the syscall receives (mingw's
// winternl.h does not declare the enums and phnt's would clash), so invalid
// classes are rejected with STATUS_INVALID_INFO_CLASS like the kernel does.
enum KeyClass : ULONG { kKeyBasic = 0, kKeyNode = 1, kKeyFull = 2, kKeyName = 3, kKeyCached = 4,
                        kKeyFlags = 5, kKeyVirtualization = 6, kKeyHandleTags = 7 };
enum KeyValueClass : ULONG { kValueBasic = 0, kValueFull = 1, kValuePartial = 2, kValueFullAlign64 = 3,
                             kValuePartialAlign64 = 4, kValueLayer = 5 };

// Same layout as KEY_VALUE_ENTRY (NtQueryMultipleValueKey); redeclared because
// MSVC's winternl.h lacks it.
struct ValueEntry { PUNICODE_STRING ValueName; ULONG DataLength; ULONG DataOffset; ULONG Type; };

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
// No transaction manager: the Transaction handle is ignored (traced) and the open is the plain one.
NTSTATUS Reg_NtOpenKeyTransacted(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, HANDLE Transaction);
NTSTATUS Reg_NtOpenKeyTransactedEx(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, ULONG OpenOptions, HANDLE Transaction);
NTSTATUS Reg_NtCreateKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                         ULONG TitleIndex, PUNICODE_STRING Class, ULONG CreateOptions, PULONG Disposition);
NTSTATUS Reg_NtCreateKeyTransacted(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, HANDLE, PULONG);
NTSTATUS Reg_NtDeleteKey(HANDLE KeyHandle);
NTSTATUS Reg_NtDeleteValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName);
NTSTATUS Reg_NtEnumerateKey(HANDLE KeyHandle, ULONG Index, ULONG KeyInformationClass,
                            PVOID KeyInformation, ULONG Length, PULONG ResultLength);
NTSTATUS Reg_NtEnumerateValueKey(HANDLE KeyHandle, ULONG Index, ULONG KeyValueInformationClass,
                                 PVOID KeyValueInformation, ULONG Length, PULONG ResultLength);
NTSTATUS Reg_NtFlushKey(HANDLE KeyHandle);
NTSTATUS Reg_NtNotifyChangeKey(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, ULONG, BOOLEAN, PVOID, ULONG, BOOLEAN);
NTSTATUS Reg_NtNotifyChangeMultipleKeys(HANDLE, ULONG, POBJECT_ATTRIBUTES, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, ULONG, BOOLEAN, PVOID, ULONG, BOOLEAN);
NTSTATUS Reg_NtQueryKey(HANDLE KeyHandle, ULONG KeyInformationClass,
                        PVOID KeyInformation, ULONG Length, PULONG ResultLength);
NTSTATUS Reg_NtQueryValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG KeyValueInformationClass,
                             PVOID KeyValueInformation, ULONG Length, PULONG ResultLength);
// ValueEntries points at EntryCount ValueEntry structs (== KEY_VALUE_ENTRY).
NTSTATUS Reg_NtQueryMultipleValueKey(HANDLE KeyHandle, ValueEntry* ValueEntries, ULONG EntryCount,
                                     PVOID ValueBuffer, PULONG BufferLength, PULONG RequiredBufferLength);
NTSTATUS Reg_NtRenameKey(HANDLE KeyHandle, PUNICODE_STRING NewName);
NTSTATUS Reg_NtSetValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG TitleIndex, ULONG Type, PVOID Data, ULONG DataSize);
NTSTATUS Reg_NtSetInformationKey(HANDLE, int KeySetInformationClass, PVOID, ULONG);
NTSTATUS Reg_NtLoadKey(POBJECT_ATTRIBUTES TargetKey, POBJECT_ATTRIBUTES SourceFile);
NTSTATUS Reg_NtLoadKeyEx(POBJECT_ATTRIBUTES, POBJECT_ATTRIBUTES, ULONG, HANDLE, HANDLE, ACCESS_MASK, PHANDLE, PVOID);
NTSTATUS Reg_NtLoadKey2(POBJECT_ATTRIBUTES TargetKey, POBJECT_ATTRIBUTES SourceFile, ULONG Flags);
NTSTATUS Reg_NtUnloadKey(POBJECT_ATTRIBUTES TargetKey);
NTSTATUS Reg_NtSaveKey(HANDLE KeyHandle, HANDLE FileHandle);
NTSTATUS Reg_NtRestoreKey(HANDLE KeyHandle, HANDLE FileHandle, ULONG Flags);
NTSTATUS Reg_NtReplaceKey(POBJECT_ATTRIBUTES, HANDLE, POBJECT_ATTRIBUTES);
NTSTATUS Reg_NtQueryOpenSubKeys(POBJECT_ATTRIBUTES, PULONG);
// NtQueryObject for a key handle (the dispatcher routes it here when IsKeyHandle()): ObjectBasicInformation and
// ObjectNameInformation (STATUS_KEY_DELETED for a deleted key); other classes are STATUS_NOT_IMPLEMENTED.
NTSTATUS Reg_NtQueryObject(HANDLE Handle, ULONG ObjectInformationClass, PVOID ObjectInformation, ULONG Length, PULONG ReturnLength);
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

// Used by NtLoadKey/NtLoadKeyEx to turn the NT path of a
// hive file (\??\C:\x\hive) into a host path. Default: the "\??\" and
// "\DosDevices\" prefixes are stripped and "C:\" maps to the machine root.
// The dispatcher should install the VFS translator here.
using HostPathResolver = std::function<bool(const std::wstring& nt_path, std::wstring* host_path)>;
void SetHostPathResolver(HostPathResolver resolver);

// The host's NtOpenDirectoryObject and NtClose. A name outside \Registry is answered the way Windows answers an
// NtOpenKey on it (OBJECT_NAME_NOT_FOUND, OBJECT_PATH_NOT_FOUND, OBJECT_TYPE_MISMATCH) by asking the host's object
// namespace, and once a guest runs the process's "ntdll.dll" is Wine's, so the dispatcher hands the host's in. Without
// them (the unit tests) the process's own ntdll is used.
using OpenDirectoryFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
using CloseObjectFn = NTSTATUS(NTAPI*)(HANDLE);
void SetHostNamespace(OpenDirectoryFn open_directory, CloseObjectFn close);

// Overlay persistence (machine folder\registry-overlay.bin): optional.
// SaveOverlay writes the whole overlay, LoadOverlay applies the file on top of the current state; both take the
// cross-process lock file (registry-overlay.lock) around the file access and write through a temp file + rename.
NTSTATUS SaveOverlay();
NTSTATUS LoadOverlay();

// Cross-process persistence for the dispatcher (docs/DISPATCHER.md "Registry persistence"). EnablePersistence is
// called once right after Initialize: it remembers the freshly seeded overlay as the baseline (only differences from
// it are ever written), loads the file when there is one and arms PersistOverlay. PersistOverlay (NtFlushKey, before a
// child process starts, at guest exit) saves when the overlay changed since the last load or save; a machine folder
// that cannot be written logs one FIXME and nothing persists. It never fails the caller.
// `overlay_file` (a host path; empty: <machine>\registry-overlay.bin) is the file to load and save, its lock
// `<overlay_file>.lock`: the dispatcher passes WIN10XBOX_REGISTRY_OVERLAY, so parallel test runs on one machine folder
// each keep their own registry.
NTSTATUS EnablePersistence(const std::wstring& overlay_file = {});
void PersistOverlay();

}  // namespace win10xbox::registry
