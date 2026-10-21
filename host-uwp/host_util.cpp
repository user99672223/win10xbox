// host_util.cpp - see host_util.h. Most of this is lifted from
// host-uwp-lite/smoke.cpp, where it ran as part of the proven (CI-built) smoke.
#include "host_util.h"

#include <cstdarg>
#include <cstdio>

namespace w10x_uwp
{
std::string Fmt(const char* fmt, ...)
{
    char buf[1536];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return std::string();
    if (static_cast<std::size_t>(n) >= sizeof buf) n = static_cast<int>(sizeof buf) - 1;
    return std::string(buf, static_cast<std::size_t>(n));
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

void WriteAll(HANDLE h, const char* data, std::size_t len)
{
    while (h != INVALID_HANDLE_VALUE && h != nullptr && len != 0)
    {
        const DWORD chunk = len > 0x1000000u ? 0x1000000u : static_cast<DWORD>(len);
        DWORD written = 0;
        if (!WriteFile(h, data, chunk, &written, nullptr) || written == 0) return;
        data += written;
        len -= written;
    }
}

bool FileSize(const std::wstring& path, unsigned long long* size)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    *size = (static_cast<unsigned long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return true;
}

bool DirExists(const std::wstring& path)
{
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

namespace
{
    bool EnsureDirDepth(const std::wstring& dir, int depth)
    {
        if (dir.empty() || depth > 32) return false;
        const DWORD a = GetFileAttributesW(dir.c_str());
        if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const std::size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash > 2) EnsureDirDepth(dir.substr(0, slash), depth + 1);
        if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
        return GetLastError() == ERROR_ALREADY_EXISTS;
    }
}  // namespace

bool EnsureDir(const std::wstring& dir)
{
    return EnsureDirDepth(dir, 0);
}

bool ReadSmallFile(const std::wstring& path, std::size_t max_bytes, std::string* out, DWORD* err)
{
    out->clear();
    if (err) *err = 0;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        if (err) *err = GetLastError();
        return false;
    }
    char buf[4096];
    DWORD got = 0;
    bool ok = true;
    for (;;)
    {
        if (!ReadFile(h, buf, sizeof buf, &got, nullptr))
        {
            if (err) *err = GetLastError();
            ok = false;
            break;
        }
        if (got == 0) break;
        if (out->size() + got > max_bytes)
        {
            if (err) *err = ERROR_FILE_TOO_LARGE;
            ok = false;
            break;
        }
        out->append(buf, got);
    }
    CloseHandle(h);
    if (!ok) out->clear();
    return ok;
}

std::vector<std::wstring> ListFiles(const std::wstring& dir)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.emplace_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

bool IsLeafName(const std::wstring& name)
{
    if (name.empty() || name.size() > 128) return false;
    if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
    for (const wchar_t c : name)
        if (c < 0x20) return false;
    return name != L"." && name != L"..";
}

std::string Hex32(long value)
{
    return Fmt("0x%08lx", static_cast<unsigned long>(value));
}

std::string TimestampUtc()
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    return Fmt("%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
               static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
               static_cast<unsigned>(st.wSecond), static_cast<unsigned>(st.wMilliseconds));
}
}  // namespace w10x_uwp
