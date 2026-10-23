// host_stage.cpp - see host_stage.h. The copy loop, the per-group statistics and
// the lists of NLS files come from host-uwp-lite/smoke.cpp; the overwrite rules,
// the package stamp and the launch.json-driven native list are new.
#include "host_stage.h"

#include "host_diag.h"
#include "host_util.h"

#include <cwchar>

namespace w10x_uwp
{
std::string GroupStats::Summary() const
{
    return Fmt("copied=%d kept=%d missing=%d failed=%d bytes=%llu", copied, kept, missing, failed, bytes);
}

std::string GroupStats::BadList() const
{
    std::string s;
    for (const std::string& b : bad)
    {
        if (!s.empty()) s += ",";
        s += b;
    }
    return s;
}

namespace
{
    std::vector<char>& CopyBuffer()
    {
        static std::vector<char> buf(1u << 20);
        return buf;
    }

    // Genuine NLS data Wine's ntdll needs at locale init (the emulated
    // NtGetNlsSectionPtr, core/dispatch/nt_nls.cpp, serves it from the machine
    // folder). Required = the en-US ANSI/OEM code pages plus the data files; the
    // code pages after that are a common-case set. machine/nls_files.txt lists
    // c_*.nls, which cannot be expanded without a directory listing of System32.
    const wchar_t* const kNlsRequired[] = {
        L"c_1252.nls",  L"c_437.nls",   L"l_intl.nls",   L"locale.nls",   L"normidna.nls",
        L"normnfc.nls", L"normnfd.nls", L"normnfkc.nls", L"normnfkd.nls",
    };
    const unsigned kNlsCodePages[] = {
        850, 874, 932, 936, 949, 950, 1250, 1251, 1253, 1254, 1255, 1256, 1257, 1258, 10000, 20127, 28591,
    };

    // Empty directories the guest's environment points at (relative to C:).
    const wchar_t* const kCommonDirs[] = {
        L"Windows\\builtin",
        L"Windows\\System32",
        L"Windows\\System32\\config",
        L"Windows\\Globalization\\Sorting",
        L"Windows\\Temp",
        L"Users\\Public",
        L"ProgramData",
        L"Program Files",
        L"Program Files (x86)",
        L"guest",
    };

    Copy StageOne(GroupStats& g, const char* group, const std::wstring& src, const std::wstring& dst,
                  const std::wstring& label, bool always)
    {
        unsigned long long bytes = 0;
        DWORD err = 0;
        const Copy c = CopyFileTo(src, dst, always, &bytes, &err);
        const std::string name = Narrow(label);
        switch (c)
        {
        case Copy::Copied:
            ++g.copied;
            g.bytes += bytes;
            diag::Line("stage[%s] %s: copied %llu bytes", group, name.c_str(), bytes);
            break;
        case Copy::Kept:
            ++g.kept;
            diag::Line("stage[%s] %s: kept (%llu bytes)", group, name.c_str(), bytes);
            break;
        case Copy::Missing:
            ++g.missing;
            g.bad.push_back(name + "(missing)");
            diag::Line("stage[%s] %s: MISSING at source (error %lu) <- %s", group, name.c_str(),
                       static_cast<unsigned long>(err), Narrow(src).c_str());
            break;
        case Copy::Failed:
            ++g.failed;
            g.bad.push_back(name + Fmt("(error %lu)", static_cast<unsigned long>(err)));
            diag::Line("stage[%s] %s: FAILED (error %lu) <- %s", group, name.c_str(), static_cast<unsigned long>(err),
                       Narrow(src).c_str());
            break;
        }
        return c;
    }

    std::wstring StampPath(const StagePlan& p) { return p.c_root + L"\\.w10x-stage"; }

    // True when the package changed since the last complete staging pass.
    bool PackageChanged(const StagePlan& p)
    {
        if (p.stamp.empty()) return false;   // no package identity: only copy what is missing
        std::string old;
        DWORD err = 0;
        if (!ReadSmallFile(StampPath(p), 4096, &old, &err)) return true;
        while (!old.empty() && (old.back() == '\n' || old.back() == '\r' || old.back() == ' ')) old.pop_back();
        return old != Narrow(p.stamp);
    }

    void WriteStamp(const StagePlan& p)
    {
        if (p.stamp.empty()) return;
        HANDLE h = CreateFileW(StampPath(p).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        const std::string text = Narrow(p.stamp) + "\n";
        WriteAll(h, text.data(), text.size());
        CloseHandle(h);
    }
}  // namespace

Copy CopyFileTo(const std::wstring& src, const std::wstring& dst, bool always, unsigned long long* bytes, DWORD* err)
{
    *err = 0;
    *bytes = 0;
    unsigned long long have = 0;
    if (!always && FileSize(dst, &have) && have != 0)
    {
        *bytes = have;
        return Copy::Kept;
    }
    HANDLE in = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE)
    {
        *err = GetLastError();
        return (*err == ERROR_FILE_NOT_FOUND || *err == ERROR_PATH_NOT_FOUND) ? Copy::Missing : Copy::Failed;
    }
    LARGE_INTEGER size;
    size.QuadPart = 0;
    if (!GetFileSizeEx(in, &size))
    {
        *err = GetLastError();
        CloseHandle(in);
        return Copy::Failed;
    }
    *bytes = static_cast<unsigned long long>(size.QuadPart);

    const std::wstring tmp = dst + L".part";
    HANDLE out = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE)
    {
        *err = GetLastError();
        CloseHandle(in);
        return Copy::Failed;
    }
    std::vector<char>& buf = CopyBuffer();
    bool ok = true;
    for (;;)
    {
        DWORD got = 0;
        if (!ReadFile(in, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr))
        {
            *err = GetLastError();
            ok = false;
            break;
        }
        if (got == 0) break;
        DWORD put = 0;
        if (!WriteFile(out, buf.data(), got, &put, nullptr) || put != got)
        {
            *err = GetLastError();
            if (*err == 0) *err = ERROR_WRITE_FAULT;
            ok = false;
            break;
        }
    }
    CloseHandle(in);
    CloseHandle(out);
    if (ok && !MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        *err = GetLastError();
        ok = false;
    }
    if (!ok)
    {
        DeleteFileW(tmp.c_str());
        return Copy::Failed;
    }
    return Copy::Copied;
}

bool StageMachineJson(const std::wstring& machine_root, const std::wstring& payload_dir)
{
    EnsureDir(machine_root);
    const std::wstring dst = machine_root + L"\\machine.json";
    unsigned long long sz = 0;
    if (FileSize(dst, &sz)) return true;   // the owner's file (or an earlier run's): never overwritten
    GroupStats g;
    StageOne(g, "config", payload_dir + L"\\machine.json", dst, L"machine.json", true);
    return FileSize(dst, &sz);
}

StageResult StageMachineFiles(const StagePlan& plan)
{
    StageResult r;
    diag::Line("stage: C: = %s, payload = %s", Narrow(plan.c_root).c_str(), Narrow(plan.payload_dir).c_str());
    diag::Line("stage: reading genuine console files BY NAME from %s and %s", Narrow(plan.system32).c_str(),
               Narrow(plan.windows).c_str());

    EnsureDir(plan.c_root);
    for (const wchar_t* d : kCommonDirs) EnsureDir(plan.c_root + L"\\" + d);
    if (!plan.profile_rel.empty())
    {
        EnsureDir(plan.c_root + L"\\" + plan.profile_rel + L"\\AppData\\Local\\Temp");
        EnsureDir(plan.c_root + L"\\" + plan.profile_rel + L"\\AppData\\Roaming");
    }

    // Package-owned files: refreshed when the package changed, else only when missing.
    r.refreshed = PackageChanged(plan);
    diag::Line("stage: package stamp '%s' -> %s", Narrow(plan.stamp).c_str(),
               r.refreshed ? "changed, package files are refreshed" : "unchanged");
    for (const std::wstring& f : ListFiles(plan.payload_dir + L"\\builtin"))
        StageOne(r.builtin, "builtin", plan.payload_dir + L"\\builtin\\" + f, plan.c_root + L"\\Windows\\builtin\\" + f, f,
                 r.refreshed);
    for (const std::wstring& f : ListFiles(plan.payload_dir + L"\\guest"))
        StageOne(r.guest, "guest", plan.payload_dir + L"\\guest\\" + f, plan.c_root + L"\\guest\\" + f, f, r.refreshed);
    if (!r.builtin.AnyFailed() && !r.guest.AnyFailed()) WriteStamp(plan);

    {
        unsigned long long sz = 0;
        r.have_ntdll = FileSize(plan.c_root + L"\\Windows\\builtin\\ntdll.dll", &sz) && sz != 0;
    }

    const std::wstring dst_sys32 = plan.c_root + L"\\Windows\\System32\\";
    // NLS data bundled in the package (payload\\nls, Wine's own files): package-owned, so the package is self-contained
    // and never depends on the console's System32. sortdefault.nls goes to Globalization\\Sorting, the rest to System32.
    for (const std::wstring& f : ListFiles(plan.payload_dir + L"\\nls"))
    {
        const bool sort = _wcsicmp(f.c_str(), L"sortdefault.nls") == 0;
        const std::wstring dst = sort ? plan.c_root + L"\\Windows\\Globalization\\Sorting\\" + f : dst_sys32 + f;
        StageOne(r.nls, "nls-pkg", plan.payload_dir + L"\\nls\\" + f, dst, f, r.refreshed);
    }

    // Genuine NLS data from the host's own System32, by name; fallback only, copied when still missing.
    for (const wchar_t* name : kNlsRequired)
    {
        const Copy c = StageOne(r.nls, "nls", plan.system32 + L"\\" + name, dst_sys32 + name, name, false);
        if (c == Copy::Missing || c == Copy::Failed) r.required_missing.push_back(Narrow(name));
    }
    for (const unsigned cp : kNlsCodePages)
    {
        wchar_t name[32];
        std::swprintf(name, 32, L"c_%03u.nls", cp);
        StageOne(r.nls, "nls", plan.system32 + L"\\" + name, dst_sys32 + name, name, false);
    }
    {
        // sortdefault.nls lives in C:\Windows\Globalization\Sorting (System32 as the fallback, like the emulation).
        const std::wstring dst = plan.c_root + L"\\Windows\\Globalization\\Sorting\\sortdefault.nls";
        Copy c = StageOne(r.nls, "nls", plan.windows + L"\\Globalization\\Sorting\\sortdefault.nls", dst,
                          L"sortdefault.nls", false);
        if (c == Copy::Missing || c == Copy::Failed)
        {
            c = StageOne(r.nls, "nls", plan.system32 + L"\\sortdefault.nls", dst_sys32 + L"sortdefault.nls",
                         L"sortdefault.nls(System32)", false);
            if (c == Copy::Missing || c == Copy::Failed) r.required_missing.push_back("sortdefault.nls");
        }
    }

    // Genuine DLLs the owner asked for in launch.json ("stage_native"), by name.
    for (const std::wstring& name : plan.native_names)
    {
        if (!IsLeafName(name))
        {
            diag::Line("stage_native: ignoring '%s' (not a bare file name)", Narrow(name).c_str());
            continue;
        }
        StageOne(r.native, "native", plan.system32 + L"\\" + name, dst_sys32 + name, name, false);
    }
    return r;
}
}  // namespace w10x_uwp
