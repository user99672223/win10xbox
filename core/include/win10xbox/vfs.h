// win10xbox VFS: translates guest NT/Win32 paths (the virtual PC's C:\, the
// machine folder) to host paths and back. Owned by core/vfs/.
//
// Guest view                                 Host (example)
//   C:\ (drive root)                           <machine_root>\ (drive root)
//   C:\Windows\System32\foo.dll (native)       <machine_root>\Windows\System32\foo.dll
//   C:\Windows\System32\ntdll.dll (builtin)    <machine_root>\Windows\builtin\ntdll.dll
//   \??\C:\x, \DosDevices\C:\x, \SystemRoot\x  same as above
//   \Device\HarddiskVolume1\x                  C:\x
//   D:\ (extra drive from machine.json)        whatever machine.json maps it to
//   \??\PIPE\, \Device\NamedPipe\, \\.\pipe\  pass through unchanged (host namespace)
//   \Device\<driver objects we emulate>        not touched here (object manager)
//
// DIRECTORY CONVENTION (final; the dispatcher and ci.yml must match):
//   native  DLLs: <C: host root>\Windows\System32\<name>.dll
//   builtin DLLs: <C: host root>\Windows\builtin\<name>.dll   (Wine PE builds)
//   <C: host root> is <machine_root> unless machine.json remaps "C:".
//   The guest only ever sees System32: a guest path C:\Windows\System32\<name>
//   (extensions dll drv ocx acm ax cpl exe) is routed to the native or builtin
//   copy according to OverrideFor(<name>); Untranslate maps the builtin dir
//   (and everything below it) back onto C:\Windows\System32. Directory
//   enumeration of System32 is NOT merged: it lists the native dir only.
//
// Rules shared by all Translate/Untranslate functions
//   * Matching is case-insensitive with a simplified NTFS upcase table
//     (ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic); output keeps the
//     caller's spelling for components, drive letters are upper-cased.
//   * Both '\' and '/' separate components; runs of separators collapse;
//     "." is dropped; ".." removes the previous component and is clamped at
//     the drive root (C:\.. == C:\). A trailing separator is preserved
//     (it marks "must be a directory"); the drive root always ends in one.
//   * NUL characters in a path -> STATUS_OBJECT_NAME_INVALID.
//   * Result paths of TranslateWin32Path of 248+ characters (or produced
//     from a "\\?\" input) get the "\\?\" prefix so Win32 file APIs accept them.
//   * Status codes come from <ntstatus.h>; include it with the usual
//     WIN32_NO_STATUS dance after <windows.h>.
//
// Thread safety: Initialize may be called again at any time (it swaps in a
// fresh immutable snapshot); every other function is safe to call
// concurrently with each other and with Initialize.
#pragma once
#include <windows.h>
#include <winternl.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace win10xbox::vfs {

struct DriveMapping { wchar_t letter; std::wstring host_root; };   // host_root absolute, no trailing '\'

// Machine description parsed from machine.json (schema: machine/machine.schema.json).
// Missing sections/fields keep the defaults below (Windows 10 22H2).
struct MachineInfo {
    struct {
        std::uint32_t major = 10, minor = 0, build = 19045, ubr = 3803;
        std::wstring product_name = L"Windows 10 Pro", display_version = L"22H2", csd;
    } windows;
    struct {
        std::wstring computer_name = L"DESKTOP-XBOX", user_name = L"User";
        std::wstring user_sid = L"S-1-5-21-1004336348-1177238915-682003330-1001";
        std::wstring domain = L"DESKTOP-XBOX";
    } identity;
    struct { double scale = 1.0; std::int64_t offset_ms = 0; bool frozen = false; } clock;
    struct { bool hide_hypervisor = true; std::wstring vendor = L"AuthenticAMD"; } cpu;
    std::vector<std::pair<std::wstring, std::wstring>> syscall_overrides;   // name -> policy, source order
};

// Initialise from the machine folder: reads <machine_root>\machine.json
// ("drives", "dll_overrides", "identity", "windows", ...). A missing file
// gives all defaults (C: = machine_root, "*" = native,builtin) and returns
// STATUS_SUCCESS. A malformed file installs the defaults too but returns
// STATUS_INVALID_PARAMETER so the host can refuse to start. An empty root is
// STATUS_INVALID_PARAMETER and changes nothing. `machine_root` may be
// relative (made absolute against the host cwd). Idempotent; calling again
// re-reads machine.json.
NTSTATUS Initialize(const std::wstring& machine_root);
// Snapshot of the parsed machine.json (defaults before Initialize).
MachineInfo GetMachineInfo();
std::wstring MachineRoot();   // by value: Initialize may swap the snapshot
std::vector<DriveMapping> Drives();   // sorted by letter; C: is always present

enum class DllOverride { Native, Builtin, NativeThenBuiltin, BuiltinThenNative, Disabled };
// Override for a module base name (case-insensitive, with or without ".dll").
// ntdll, kernelbase, kernel32 always report Builtin (see DECISIONS.md E1).
DllOverride OverrideFor(std::wstring_view module_name);
// Host Win32 path of the builtin (Wine) copy of a system DLL, or empty if
// absent. `module_name` is a bare file name; ".dll" is appended if it has no
// extension. Names containing separators or ':' give empty.
std::wstring BuiltinDllPath(std::wstring_view module_name);
std::wstring NativeDllPath(std::wstring_view module_name);   // same for <System32>
// Applies OverrideFor + file existence to pick the host Win32 path the loader
// should open for a system module. Native/Builtin: that copy (even if absent,
// so the failure names it). *ThenBuiltin/*ThenNative: first existing copy,
// else the first-named one. Disabled -> STATUS_OBJECT_NAME_NOT_FOUND.
NTSTATUS ResolveSystemModule(std::wstring_view module_name, std::wstring* host_win32_path);

// Translate a guest NT-style path (as found in OBJECT_ATTRIBUTES, e.g.
// "\??\C:\Windows\notepad.exe") to a host NT path
// ("\??\D:\machine\Windows\notepad.exe"). Returns STATUS_SUCCESS, STATUS_OBJECT_PATH_NOT_FOUND for an
// unmapped drive (or \Device\HarddiskVolumeN with N out of range),
// STATUS_OBJECT_NAME_NOT_FOUND for a DLL whose override is Disabled,
// STATUS_OBJECT_NAME_INVALID for NULs, STATUS_UNSUCCESSFUL before
// Initialize, or STATUS_NOT_SUPPORTED if the path belongs to a namespace the
// VFS does not own (then the dispatcher passes it through untouched):
// \??\PIPE, \??\MAILSLOT, \??\UNC, other \??\ device names (NUL, COM1,
// GLOBALROOT, Volume{..}), \Device\<anything but HarddiskVolumeN>,
// \\.\<anything>, \Registry, \BaseNamedObjects, and every other rooted
// object-manager path.
// Accepted prefixes (case-insensitive): \??\X:, \DosDevices\X:,
// \GLOBAL??\X:, \\?\X: (Win32-style alias), \SystemRoot,
// \Device\HarddiskVolumeN (N=1 is C:, N>=2 the other drives in letter order).
// Applies DLL overrides when the path is <system32>\<name>.dll.
NTSTATUS TranslateNtPath(std::wstring_view guest_nt_path, std::wstring* host_nt_path);
// Same for Win32 paths ("C:\x", "C:/x", "\\?\C:\x", "\\.\C:\x"). Relative and
// drive-relative paths ("foo", "C:foo", "\foo") are STATUS_OBJECT_NAME_INVALID
// (resolve them with GuestFullPath first). UNC and other \\.\ / \\?\ forms
// are STATUS_NOT_SUPPORTED. Output is a host Win32 path ("D:\machine\x").
NTSTATUS TranslateWin32Path(std::wstring_view guest_win32_path, std::wstring* host_win32_path);
// Reverse: host NT or Win32 path back to the guest view (for
// FileNameInformation, GetFinalPathNameByHandle, module paths). Paths
// outside every mapping are returned unchanged with STATUS_NOT_SUPPORTED.
// Accepted inputs (either function): "\??\D:\..", "\??\UNC\..", "D:\..",
// "\\?\D:\..", "\\server\share\..". Output style follows the function:
// UntranslateNtPath -> "\??\C:\.."; UntranslateWin32Path -> "C:\.." (or
// "\\?\C:\.." when the input used the "\\?\" form). The most specific host
// root wins; the builtin dir maps to C:\Windows\System32.
NTSTATUS UntranslateNtPath(std::wstring_view host_nt_path, std::wstring* guest_nt_path);
NTSTATUS UntranslateWin32Path(std::wstring_view host_win32_path, std::wstring* guest_win32_path);

// Helper for the dispatcher: rewrite OBJECT_ATTRIBUTES in place for a syscall.
// `storage` keeps the new UNICODE_STRING alive for the call; RootDirectory
// handles are left alone (they are already host handles).
// `attr` is always usable by the caller afterwards: it is a copy of *in whose
// ObjectName points at `name` (translated == true) or at the original string
// (translated == false: no name, RootDirectory set, or a namespace we do not
// own). Not copyable/movable: `attr.ObjectName` points inside this object.
struct TranslatedAttributes {
    TranslatedAttributes() = default;
    TranslatedAttributes(const TranslatedAttributes&) = delete;
    TranslatedAttributes& operator=(const TranslatedAttributes&) = delete;

    OBJECT_ATTRIBUTES attr{};
    UNICODE_STRING name{};
    std::wstring storage;
    bool translated = false;
};
// Returns STATUS_SUCCESS (translated or deliberately untouched, see above),
// or the error from TranslateNtPath (PATH_NOT_FOUND, NAME_NOT_FOUND for a
// disabled DLL, NAME_INVALID), STATUS_NAME_TOO_LONG if the result does not fit
// a UNICODE_STRING, STATUS_INVALID_PARAMETER if in->Length is too small.
// A null `in` yields a zeroed attr and STATUS_SUCCESS.
NTSTATUS TranslateObjectAttributes(const OBJECT_ATTRIBUTES* in, TranslatedAttributes* out);

// Resolve a Win32 path against a guest current directory into an absolute
// guest Win32 path (what GetFullPathNameW does, without touching the host):
//   "C:\a\..\b"  -> "C:\b"       "C:foo" -> cwd\foo if cwd is on C:, else "C:\foo"
//   "\foo"        -> <cwd drive>:\foo     "foo" -> <cwd>\foo
//   "\\?\C:\x" and "\\.\..", UNC -> returned with only separators normalised.
// `guest_cwd` must be an absolute drive path ("C:\Users\User") when needed,
// else STATUS_OBJECT_NAME_INVALID.
NTSTATUS GuestFullPath(std::wstring_view path, std::wstring_view guest_cwd, std::wstring* absolute);

// Guest directories derived from machine.json "identity" (user_name):
std::wstring GuestSystemRoot();        // "C:\Windows"
std::wstring GuestSystemDirectory();   // "C:\Windows\System32"
std::wstring GuestUserProfile();       // "C:\Users\<user>"
std::wstring GuestTempDirectory();     // "C:\Users\<user>\AppData\Local\Temp"

}  // namespace win10xbox::vfs
