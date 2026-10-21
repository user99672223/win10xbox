// win10xbox VFS: translates guest NT/Win32 paths (the virtual PC's C:\, the
// machine folder) to host paths and back. Owned by core/vfs/.
//
// Guest view                                 Host (example)
//   C:\                                        <machine_root>\
//   C:\Windows\System32\foo.dll (native)       <machine_root>\Windows\System32\foo.dll
//   C:\Windows\System32\ntdll.dll (builtin)    <machine_root>\Windows\builtin\ntdll.dll
//   \??\C:\x, \DosDevices\C:\x, \SystemRoot\x  same as above
//   \Device\HarddiskVolume1\x                  C:\x
//   D:\ (extra drive from machine.json)        whatever machine.json maps it to
//   \??\PIPE\, \Device\NamedPipe\, \\.\pipe\  pass through unchanged (host namespace)
//   \Device\<driver objects we emulate>        not touched here (object manager)
#pragma once
#include <windows.h>
#include <winternl.h>
#include <string>
#include <string_view>
#include <vector>

namespace win10xbox::vfs {

struct DriveMapping { wchar_t letter; std::wstring host_root; };   // host_root absolute, no trailing '\'

// Initialise from the machine folder: reads <machine_root>\machine.json
// ("drives", "dll_overrides") and the builtin DLL dir. Idempotent.
NTSTATUS Initialize(const std::wstring& machine_root);
const std::wstring& MachineRoot();
std::vector<DriveMapping> Drives();

enum class DllOverride { Native, Builtin, NativeThenBuiltin, BuiltinThenNative, Disabled };
// Override for a module base name (case-insensitive, with or without ".dll").
// ntdll, kernelbase, kernel32 always report Builtin (see DECISIONS.md E1).
DllOverride OverrideFor(std::wstring_view module_name);
// Host path of the builtin (Wine) copy of a system DLL, or empty if absent.
std::wstring BuiltinDllPath(std::wstring_view module_name);
std::wstring NativeDllPath(std::wstring_view module_name);

// Translate a guest NT-style path (as found in OBJECT_ATTRIBUTES, e.g.
// "\??\C:\Windows\notepad.exe") to a host NT path ("\??\D:\machine\Windows\
// notepad.exe"). Returns STATUS_SUCCESS, STATUS_OBJECT_PATH_NOT_FOUND for an
// unmapped drive, or STATUS_NOT_SUPPORTED if the path belongs to a namespace
// the VFS does not own (then the dispatcher passes it through untouched).
// Applies DLL overrides when the path is <system32>\<name>.dll.
NTSTATUS TranslateNtPath(std::wstring_view guest_nt_path, std::wstring* host_nt_path);
// Same for Win32 paths ("C:\x", "\\?\C:\x", relative not allowed).
NTSTATUS TranslateWin32Path(std::wstring_view guest_win32_path, std::wstring* host_win32_path);
// Reverse: host NT or Win32 path back to the guest view (for
// FileNameInformation, GetFinalPathNameByHandle, module paths). Paths
// outside every mapping are returned unchanged with STATUS_NOT_SUPPORTED.
NTSTATUS UntranslateNtPath(std::wstring_view host_nt_path, std::wstring* guest_nt_path);
NTSTATUS UntranslateWin32Path(std::wstring_view host_win32_path, std::wstring* guest_win32_path);

// Helper for the dispatcher: rewrite OBJECT_ATTRIBUTES in place for a syscall.
// `storage` keeps the new UNICODE_STRING alive for the call; RootDirectory
// handles are left alone (they are already host handles).
struct TranslatedAttributes {
    OBJECT_ATTRIBUTES attr{};
    UNICODE_STRING name{};
    std::wstring storage;
    bool translated = false;
};
NTSTATUS TranslateObjectAttributes(const OBJECT_ATTRIBUTES* in, TranslatedAttributes* out);

// Guest current directory and environment-derived defaults (from machine.json
// "identity"): "C:\Users\User", "C:\Windows", "C:\Windows\System32", "C:\Windows\Temp".
std::wstring GuestSystemRoot();        // "C:\Windows"
std::wstring GuestSystemDirectory();   // "C:\Windows\System32"
std::wstring GuestUserProfile();       // "C:\Users\<user>"
std::wstring GuestTempDirectory();     // "C:\Users\<user>\AppData\Local\Temp"

}  // namespace win10xbox::vfs
