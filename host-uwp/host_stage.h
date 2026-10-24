// host_stage.h - sets up the machine folder (the guest's C:\) in LocalState.
//
//   <LocalState>\machine\machine.json                 machine/machine.default.json from the package; never overwritten
//   <C:>\Windows\builtin\*.dll                        Wine PE builtins, shipped INSIDE the package
//   <C:>\guest\*.exe                                  the bundled guests, shipped inside the package
//   <C:>\Windows\System32\*.nls                       genuine NLS data read BY NAME from the console's own System32
//   <C:>\Windows\Globalization\Sorting\sortdefault.nls   (from the console's C:\Windows\Globalization)
//   <C:>\Windows\System32\<launch.json stage_native>  genuine DLLs read BY NAME, only the ones the owner lists
//   <C:>\<profile>\AppData\..., Windows\Temp, ProgramData, Program Files...   empty directories the guest expects
//
// <C:> is the machine folder unless machine.json remaps "C:" (core/vfs). The
// console denies directory listing of System32 but opens individual files by
// name (docs/DECISIONS.md D17), so system files are only ever read by name.
//
// Overwrite rules (nothing the owner may have edited or supplied is clobbered):
//   * package-owned files (builtin DLLs, bundled guests) are refreshed when the
//     package version changes (the stamp file <C:>\.w10x-stage holds the package
//     full name) and otherwise only copied when missing;
//   * files read from the console's System32 are copied only when missing, so a
//     22H2 file the owner dropped into the machine folder is never replaced by
//     the console's newer one;
//   * machine.json is never overwritten.
// Every file is written through "<name>.part" and renamed, so an interrupted
// copy is never mistaken for a good file.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace w10x_uwp
{
    enum class Copy
    {
        Copied,    // written
        Kept,      // destination already there (see the overwrite rules)
        Missing,   // source not found
        Failed,    // any other error
    };

    struct GroupStats
    {
        int copied = 0;
        int kept = 0;
        int missing = 0;
        int failed = 0;
        unsigned long long bytes = 0;
        std::vector<std::string> bad;   // "name(missing)" / "name(error N)"

        std::string Summary() const;    // "copied=1 kept=2 missing=0 failed=0 bytes=123"
        std::string BadList() const;    // "a(missing),b(error 5)"
        bool AnyFailed() const { return failed != 0; }
    };

    // Copies `src` to `dst`. always == false: an existing non-empty `dst` is kept.
    Copy CopyFileTo(const std::wstring& src, const std::wstring& dst, bool always, unsigned long long* bytes, DWORD* err);

    // Phase A, before the core is initialised: machine.json from the package if there is none yet.
    // Creates `machine_root` as needed. Returns true if machine.json exists afterwards.
    // `json_rel`: the package file relative to payload_dir (a machine folder with its own derived machine.json uses
    // <folder>\machine.json).
    bool StageMachineJson(const std::wstring& machine_root, const std::wstring& payload_dir,
                          const std::wstring& json_rel = L"machine.json");

    struct StagePlan
    {
        std::wstring c_root;           // host root of the guest's C:
        std::wstring payload_dir;      // <package>\payload
        std::wstring system32;         // the console's own System32 (read by name only)
        std::wstring windows;          // the console's own Windows directory
        std::wstring profile_rel;      // guest profile relative to C:, e.g. L"Users\\User"
        std::wstring stamp;            // package full name ("" = no package identity: only copy what is missing)
        std::vector<std::wstring> native_names;   // launch.json stage_native (validated leaf names)
        std::wstring profile_dir;      // payload\<profile_dir>\*.dll are copied into System32 (the msctf.dll stand-in); "" = none
    };

    struct StageResult
    {
        GroupStats builtin, guest, nls, native, stubs;
        bool have_ntdll = false;                   // <C:>\Windows\builtin\ntdll.dll present
        bool refreshed = false;                    // package files were re-copied (new package version)
        std::vector<std::string> required_missing; // required NLS data that could not be staged
    };

    // Phase B, after the VFS knows where C: is.
    StageResult StageMachineFiles(const StagePlan& plan);
}  // namespace w10x_uwp
