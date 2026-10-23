// launch_config.h - the launch configuration of host-uwp (LocalState\launch.json).
//
// Portable C++20 on purpose (no Windows headers): the parser is unit-tested
// off-device by selftest/selftest.cpp (the CI workflow runs it) and the Windows
// side only converts the UTF-8 strings it returns.
//
// The file is JSON with two conveniences for editing by hand through the
// Device Portal: // and /* */ comments, and trailing commas. UNKNOWN KEYS ARE
// AN ERROR, not a warning: a typo such as "gest" must not silently run the
// default guest on a device with no console to show a message (the same rule
// machine/machine.schema.json applies to machine.json). Keys starting with '$'
// or '_' are free-form annotations and ignored.
//
// Keys (all optional):
//   guest             string   program to run: a bare file name (looked up in C:\guest)
//                              or an absolute guest path such as "C:\\Program Files\\App\\app.exe"
//   args              array of strings (each quoted as needed) or one string (appended verbatim)
//   cwd               string   guest current directory (absolute, e.g. "C:\\guest"); default C:\Users\<user>
//   trace             string   WIN10XBOX_DEBUG spec ("+loader,+process", "+all", ...); default: err + fixme only
//   winedebug         string   WIN10XBOX_WINEDEBUG spec for Wine's own channels ("+loaddll,warn+module")
//   trace_log_max_mb  integer  size cap of trace.log in MiB, 0 = unlimited (default 64)
//   timeout_sec       integer  watchdog: end the run after this many seconds, 0 = none (default 0)
//   exit_when_done    boolean  close the app when the guest has exited (default true)
//   expect_exit_code  integer  status.txt reports PASS/FAIL against this guest exit code
//   stage_native      array of file names to copy BY NAME from the console's System32 into the machine folder
//   picker            boolean  show the on-console launcher (launcher.h) instead of running "guest" directly;
//                              the default is true only when there is no launch.json at all
//   autoselect        string or array of strings: file name(s) of launcher entries to launch without waiting
//                              for input (test hook; implies "picker"). With a list the app runs the first,
//                              restarts itself (RequestRestartAsync), runs the second, and so on; one block per
//                              run is appended to LocalState\launcher_result.txt
//   input_pump        boolean  forward CoreWindow keyboard/pointer input to the winexbox backend (default true)
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace w10x_uwp
{
    struct LaunchConfig
    {
        // What to run. After ParseLaunchConfig() `guest` is always an absolute
        // guest path ("C:\guest\hello.exe"), `cwd` is empty or absolute.
        std::string guest = "C:\\guest\\hello.exe";
        std::vector<std::string> args;   // each argument is quoted as needed
        std::string args_raw;            // verbatim command-line tail ("args" given as one string)
        std::string cwd;                 // empty = the layer's default (C:\Users\<user>)

        // Diagnostics.
        std::string trace;               // WIN10XBOX_DEBUG; empty = quiet (err + fixme)
        std::string winedebug;           // WIN10XBOX_WINEDEBUG
        std::uint32_t trace_log_max_mb = 64;

        // Lifetime.
        std::uint32_t timeout_sec = 0;   // 0 = run until the guest exits
        bool exit_when_done = true;
        bool has_expect_exit_code = false;
        std::int32_t expect_exit_code = 0;

        // Machine folder / shell.
        std::vector<std::string> stage_native;   // System32 file names (leaf names only)
        bool input_pump = true;

        // Launcher (launcher.h).
        bool has_picker = false;         // "picker" was written
        bool picker = false;
        std::vector<std::string> autoselect;   // leaf names, run k = autoselect[k]; empty = wait for the user
    };

    // Parses the text of a launch.json. On failure returns false and sets
    // *error to "line N: message" (the caller prefixes the file name). `out` is
    // only written on success. Never throws, never recurses deeper than a few
    // levels, rejects inputs larger than 256 KiB.
    bool ParseLaunchConfig(std::string_view text, LaunchConfig* out, std::string* error);

    // Resolves a guest path the way the "guest" and "cwd" keys do. A bare file
    // name (no separator, no ':') becomes C:\guest\<name> when allow_bare_name,
    // an absolute drive path (C:\... or C:/...) is returned with backslashes and
    // an upper-case drive letter. Everything else is rejected with a reason.
    bool NormalizeGuestPath(std::string_view in, bool allow_bare_name, std::string* out, std::string* why);

    // A bare file name: no separators, no drive, not "." / "..", at most 128 characters.
    bool IsLeafNameUtf8(std::string_view name);

    // Human-readable dump of an effective configuration (status.txt / trace.log).
    std::string DescribeLaunchConfig(const LaunchConfig& c);

    // CommandLineToArgvW-compatible quoting of one argument (the rules
    // host-win32/main.cpp uses): an argument without space, tab or quote is
    // returned unchanged, anything else is wrapped in quotes with the
    // backslash-before-quote rules applied.
    template <class CharT>
    std::basic_string<CharT> QuoteArgT(std::basic_string_view<CharT> a)
    {
        using View = std::basic_string_view<CharT>;
        using Str = std::basic_string<CharT>;
        const bool plain = !a.empty() && a.find(CharT(' ')) == View::npos && a.find(CharT('\t')) == View::npos &&
                           a.find(CharT('"')) == View::npos;
        if (plain) return Str(a);
        Str out(1, CharT('"'));
        std::size_t backslashes = 0;
        for (const CharT c : a)
        {
            if (c == CharT('\\'))
            {
                ++backslashes;
                continue;
            }
            if (c == CharT('"'))
                out.append(backslashes * 2 + 1, CharT('\\'));
            else
                out.append(backslashes, CharT('\\'));
            backslashes = 0;
            out.push_back(c);
        }
        out.append(backslashes * 2, CharT('\\'));
        out.push_back(CharT('"'));
        return out;
    }

    inline std::string QuoteArg(const std::string& a) { return QuoteArgT<char>(a); }
    inline std::wstring QuoteArg(const std::wstring& a) { return QuoteArgT<wchar_t>(a); }

    // ---- the restart argument (docs/HANDOFF.md "Launcher") -------------------------------------------
    // After a guest has exited the app restarts itself with CoreApplication::RequestRestartAsync(arg) so the
    // next guest gets a fresh process (core/dispatch/process.cpp allows one Launch per process). The argument
    // comes back in the launch activation as "picker run=<N> select=<leaf name>": N is the number of guests
    // this chain has already run (it indexes launch.json "autoselect" and numbers the launcher_result.txt
    // blocks), select is the program that just ran (the cursor starts on it).
    struct RestartArgs
    {
        bool valid = false;      // the argument was a restart argument
        int run = 0;             // guests already run in this chain, 0..kMaxRestartRun
        std::string select;      // leaf name, may be empty
    };
    constexpr int kMaxRestartRun = 10000;
    std::string FormatRestartArgs(int run, const std::string& select);
    // Anything that is not "picker [run=N] [select=NAME]" (a normal launch, an empty string, a leaf name that
    // is not a leaf name, a number out of range) gives valid == false.
    RestartArgs ParseRestartArgs(const std::string& args);

}  // namespace w10x_uwp
