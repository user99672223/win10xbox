// host_util.h - small Win32 helpers shared by the host sources. Plain Win32 +
// STL, no WinRT (main.cpp is the only translation unit that sees WinRT).
#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace w10x_uwp
{
    // printf-style formatting into a std::string (truncated at about 1.5 KiB).
    std::string Fmt(const char* fmt, ...);

    std::string Narrow(const std::wstring& w);   // UTF-16 -> UTF-8
    std::wstring Widen(const std::string& s);    // UTF-8 -> UTF-16

    // WriteFile in a loop; ignores invalid handles. Safe on any thread and,
    // because it only touches the handle, safe while a guest owns the PEB.
    void WriteAll(HANDLE h, const char* data, std::size_t len);

    bool FileSize(const std::wstring& path, unsigned long long* size);   // false for directories / missing
    bool DirExists(const std::wstring& path);
    bool EnsureDir(const std::wstring& dir);                              // creates parents, true if it exists afterwards

    // Reads a whole (small) file. *err is the Win32 error on failure. Files over
    // `max_bytes` are an error (ERROR_FILE_TOO_LARGE).
    bool ReadSmallFile(const std::wstring& path, std::size_t max_bytes, std::string* out, DWORD* err);

    // Leaf names of the regular files directly inside `dir` (empty if it does not exist).
    std::vector<std::wstring> ListFiles(const std::wstring& dir);

    // A bare file name: no separators, no drive, not "." / "..", at most 128 characters.
    bool IsLeafName(const std::wstring& name);

    std::string Hex32(long value);   // "0x0000002a"
    std::string TimestampUtc();      // "2026-10-21T18:04:05.123Z"
}  // namespace w10x_uwp
