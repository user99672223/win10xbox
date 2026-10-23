// guest_sidecar.h - the per-guest sidecar files the launcher reads next to a guest program (portable: no Windows,
// no layer; also compiled into host_uwp_selftest). Format: docs/HANDOFF.md, "Launcher".
//
//   <name>.args    one argument per line, used verbatim (blank lines and lines starting with '#' are skipped)
//   <name>.env     KEY=VALUE per line (same skipping rules); applied to the one launch only
//   <name>.title   first non-blank line = the text the picker shows instead of "<name>.exe"
//   <name>.machine first non-blank line = the machine folder under LocalState this guest runs on (default "machine";
//                  "machine-native-d3d" = route B + native Direct3D 11, docs/slices/uwpd3d.md)
//   <name>.launch  an extra picker entry that runs another guest: lines  exe=, title=, args= (whitespace separated,
//                  "double quotes" group a word), env=KEY=VALUE (repeatable), machine=<folder>, cwd=<dir relative to the
//                  entry's directory>. exe= may be a relative path with subdirectories (Binaries\Win64\App.exe).
//   <dir>\<dir>.launch  (a subdirectory of guests\) one picker entry for a whole directory install: the directory is
//                  C:\guest\<dir>\, exe= and cwd= are relative to it.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace w10x_uwp
{
    struct GuestLaunch
    {
        std::string exe;     // guest program (from a .launch file; empty for a plain .exe entry): a leaf name, or a relative
                             // path with backslashes (normalized) for a directory entry
        std::string cwd;     // working directory relative to the entry's directory (backslashes); empty = launch.json / default
        std::string title;   // empty = show the file name
        std::vector<std::string> args;
        std::vector<std::pair<std::string, std::string>> env;
        std::string machine; // machine folder name under LocalState; empty = "machine"
    };

    void ParseArgsFile(std::string_view text, std::vector<std::string>* out);
    bool ParseEnvFile(std::string_view text, std::vector<std::pair<std::string, std::string>>* out, std::string* error);
    std::string ParseTitleFile(std::string_view text);
    // Whitespace-separated words; "double quotes" group (a quote pair may be empty and yields an empty word).
    std::vector<std::string> SplitArgLine(std::string_view line);
    // A machine folder name: a leaf name that does not start with '.'.
    bool IsMachineFolderName(std::string_view name);
    // First non-blank line of a .machine file, trimmed; sets *ok = false (and returns "") when it is not a valid name.
    std::string ParseMachineFile(std::string_view text, bool* ok);
    // The machine folder that carries the native Direct3D 11 runtime (route B + D3D, tests/integration/prepare_machine.cmake
    // D3D_MACHINE) and the System32 DLLs the host copies BY NAME into it from the console (the same set).
    constexpr const char* kD3dMachineName = "machine-native-d3d";
    const std::vector<std::string>& D3dMachineNativeDlls();
    // machine-emu-d3d (docs/slices/uwpswap.md): Wine's builtin user32/gdi32/win32u over core/user plus the native Direct3D 11
    // runtime (tests/integration/prepare_machine.cmake EMU_D3D_MACHINE); swap chains on its windows are virtual.
    constexpr const char* kEmuD3dMachineName = "machine-emu-d3d";
    const std::vector<std::string>& EmuD3dMachineNativeDlls();
    // A relative path of leaf names joined by '\\' or '/' ("Binaries\\Win64\\App.exe"); "." is allowed only when
    // allow_dot (cwd=.). No drive, no leading separator, no "..", no empty component. *out gets backslashes.
    bool NormalizeRelativePath(std::string_view in, bool allow_dot, std::string* out);
    bool ParseLaunchFile(std::string_view text, GuestLaunch* out, std::string* error);
}  // namespace w10x_uwp
