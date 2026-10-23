// guest_sidecar.h - the per-guest sidecar files the launcher reads next to a guest program (portable: no Windows,
// no layer; also compiled into host_uwp_selftest). Format: docs/HANDOFF.md, "Launcher".
//
//   <name>.args    one argument per line, used verbatim (blank lines and lines starting with '#' are skipped)
//   <name>.env     KEY=VALUE per line (same skipping rules); applied to the one launch only
//   <name>.title   first non-blank line = the text the picker shows instead of "<name>.exe"
//   <name>.launch  an extra picker entry that runs another guest: lines  exe=, title=, args= (whitespace separated,
//                  "double quotes" group a word), env=KEY=VALUE (repeatable)
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace w10x_uwp
{
    struct GuestLaunch
    {
        std::string exe;     // leaf name of the guest program (from a .launch file; empty for a plain .exe entry)
        std::string title;   // empty = show the file name
        std::vector<std::string> args;
        std::vector<std::pair<std::string, std::string>> env;
    };

    void ParseArgsFile(std::string_view text, std::vector<std::string>* out);
    bool ParseEnvFile(std::string_view text, std::vector<std::pair<std::string, std::string>>* out, std::string* error);
    std::string ParseTitleFile(std::string_view text);
    // Whitespace-separated words; "double quotes" group (a quote pair may be empty and yields an empty word).
    std::vector<std::string> SplitArgLine(std::string_view line);
    bool ParseLaunchFile(std::string_view text, GuestLaunch* out, std::string* error);
}  // namespace w10x_uwp
