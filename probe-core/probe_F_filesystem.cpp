// probe_F_filesystem.cpp - category F: filesystem access matrix.
//
// For each target path two independent records are emitted:
//     read:<label>   can we open + list (or read bytes from) it?
//     write:<label>  can we create a uniquely-named temp file, write 16 bytes,
//                    read them back and delete it?
//
// Targets: localstate(localappdata), package-install, package-localstate,
// windows-dir, system32, system32-read-ntdll (read only), drive-c-root, temp
// (positive control), programdata, userprofile, known-documents.
//
// Special reads: system32 opens System32\kernel32.dll and checks for "MZ";
// system32-read-ntdll opens ntdll.dll and reads the first 64 bytes (DOS header).
//
// Safety: every write test uses a name __probe_tmp_<pid>_<tick>_<seq>.tmp. The
// file is opened FILE_FLAG_DELETE_ON_CLOSE (so a crash cannot strand it) and an
// RAII guard additionally deletes it explicitly. Nothing else is ever modified.
//
// Link deps: none beyond kernel32. Files are opened with CreateFile2 (CreateFileW
// is not in the UWP API surface). SHGetKnownFolderPath, CoTaskMemFree,
// GetWindowsDirectoryW and the package-identity queries are resolved at runtime.
// Sonnet
#include "probe.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef FIND_FIRST_EX_LARGE_FETCH
#define FIND_FIRST_EX_LARGE_FETCH 0x00000002
#endif

namespace probe {
namespace {

// ---- error classification ---------------------------------------------------

Status classify(DWORD e, Status other = Status::INFO) {
    switch (e) {
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
            return Status::DENIED;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
            return Status::MISSING;
        case ERROR_NOT_SUPPORTED:
        case ERROR_CALL_NOT_IMPLEMENTED:
            return Status::NOTIMPL;
        default:
            return other;
    }
}

void fail(Record& rec, DWORD e, const char* what, const std::wstring& path) {
    rec.status = classify(e);
    rec.errorcode = e;
    rec.detail = std::string(what) + " failed on " + narrow(path) + ": " + error_text(e);
}

// ---- path helpers -------------------------------------------------------------

std::wstring with_slash(std::wstring p) {
    if (p.empty() || (p.back() != L'\\' && p.back() != L'/')) p += L'\\';
    return p;
}

bool ends_with_nocase(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + (s.size() - n), suffix) == 0;
}

bool get_env(const wchar_t* name, std::wstring* out) {
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);  // incl. NUL
    if (n == 0) return false;
    std::wstring s(n, L'\0');
    const DWORD m = GetEnvironmentVariableW(name, &s[0], n);
    if (m == 0 || m >= n) return false;
    s.resize(m);
    *out = s;
    return true;
}

// Grow-and-retry for the GetXxxDirectory / GetTempPath style APIs: they return
// the length (excl. NUL) on success, or the required size (incl. NUL) when the
// buffer is too small, or 0 on failure.
template <class F>
std::wstring grow_call(F f) {
    std::wstring s(MAX_PATH, L'\0');
    for (int i = 0; i < 3; ++i) {
        const DWORD n = f(&s[0], static_cast<DWORD>(s.size()));
        if (n == 0) return std::wstring();
        if (n < s.size()) {
            s.resize(n);
            return s;
        }
        s.assign(static_cast<size_t>(n) + 1, L'\0');
    }
    return std::wstring();
}

// ---- target resolution ----------------------------------------------------------

struct Resolved {
    bool ok = false;
    std::wstring path;
    Status why = Status::MISSING;  // meaningful only when !ok
    uint32_t err = 0;
    std::string note;              // human-readable origin / reason
};

Resolved resolved_ok(std::wstring path, std::string note) {
    Resolved r;
    r.ok = true;
    r.path = std::move(path);
    r.note = std::move(note);
    return r;
}

Resolved unresolved(Status why, std::string note, uint32_t err = 0) {
    Resolved r;
    r.why = why;
    r.note = std::move(note);
    r.err = err;
    return r;
}

Resolved from_env(const wchar_t* var) {
    std::wstring v;
    if (!get_env(var, &v))
        return unresolved(Status::MISSING,
                          "environment variable %" + narrow(var) + "% is not set");
    return resolved_ok(v, "from %" + narrow(var) + "%");
}

Resolved res_localappdata() { return from_env(L"LOCALAPPDATA"); }
Resolved res_programdata() { return from_env(L"ProgramData"); }
Resolved res_userprofile() { return from_env(L"USERPROFILE"); }

Resolved res_temp() {
    std::wstring p = grow_call([](LPWSTR b, DWORD n) { return GetTempPathW(n, b); });
    if (p.empty()) return unresolved(Status::INFO, "GetTempPathW returned 0", GetLastError());
    return resolved_ok(p, "GetTempPathW");
}

Resolved res_system32() {
    std::wstring p = grow_call([](LPWSTR b, DWORD n) { return GetSystemDirectoryW(b, n); });
    if (p.empty())
        return unresolved(Status::INFO, "GetSystemDirectoryW returned 0", GetLastError());
    return resolved_ok(p, "GetSystemDirectoryW");
}

Resolved res_windows() {
    typedef UINT(WINAPI * Fn)(LPWSTR, UINT);
    Fn fn = reinterpret_cast<Fn>(resolve("kernel32.dll", "GetWindowsDirectoryW"));
    if (!fn)
        return unresolved(Status::SKIPPED, "GetWindowsDirectoryW not exported by kernel32");
    std::wstring p = grow_call([fn](LPWSTR b, DWORD n) { return static_cast<DWORD>(fn(b, n)); });
    if (p.empty())
        return unresolved(Status::INFO, "GetWindowsDirectoryW returned 0", GetLastError());
    return resolved_ok(p, "GetWindowsDirectoryW");
}

Resolved res_drive_c() { return resolved_ok(L"C:\\", "literal C:\\"); }

Resolved res_known_documents() {
    typedef HRESULT(WINAPI * GetFolderFn)(const GUID*, DWORD, HANDLE, PWSTR*);
    typedef void(WINAPI * FreeFn)(LPVOID);
    // FOLDERID_Documents {FDD39AD0-238F-46AF-ADB4-6C85480369C7}, defined locally
    // so we need neither shlobj.h nor uuid.lib.
    static const GUID kDocuments = {
        0xFDD39AD0, 0x238F, 0x46AF, {0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7}};

    GetFolderFn fn = reinterpret_cast<GetFolderFn>(resolve("shell32.dll", "SHGetKnownFolderPath"));
    if (!fn)
        fn = reinterpret_cast<GetFolderFn>(resolve("shcore.dll", "SHGetKnownFolderPath"));
    if (!fn)
        return unresolved(Status::SKIPPED, "SHGetKnownFolderPath not available (shell32/shcore)");

    PWSTR out = nullptr;
    const HRESULT hr = fn(&kDocuments, 0, nullptr, &out);
    std::wstring path;
    if (SUCCEEDED(hr) && out) path = out;
    if (out) {
        FreeFn freer = reinterpret_cast<FreeFn>(resolve("ole32.dll", "CoTaskMemFree"));
        if (freer) freer(out);  // else: leak a few bytes, harmless
    }
    if (FAILED(hr)) {
        Status st = Status::INFO;
        // HRESULT_FROM_WIN32 range: map the embedded Win32 code.
        if ((static_cast<uint32_t>(hr) & 0xFFFF0000u) == 0x80070000u)
            st = classify(static_cast<uint32_t>(hr) & 0xFFFFu);
        return unresolved(st, "SHGetKnownFolderPath(FOLDERID_Documents) failed: " +
                                  error_text(static_cast<uint32_t>(hr)),
                          static_cast<uint32_t>(hr));
    }
    if (path.empty())
        return unresolved(Status::INFO, "SHGetKnownFolderPath returned an empty path");
    return resolved_ok(path, "SHGetKnownFolderPath(FOLDERID_Documents)");
}

// Package identity (UWP / MSIX). Resolved at runtime; absent or "no package" on
// the plain desktop baseline, which is itself a result.
// rc: 0 ok, -1 API missing, otherwise the APPMODEL_ERROR_* / Win32 code.
bool query_package_string(const char* symbol, std::wstring* out, LONG* rc) {
    typedef LONG(WINAPI * Fn)(UINT32*, PWSTR);
    Fn fn = reinterpret_cast<Fn>(resolve("kernel32.dll", symbol));
    if (!fn) {
        *rc = -1;
        return false;
    }
    UINT32 n = 0;
    LONG r = fn(&n, nullptr);
    if (r != ERROR_INSUFFICIENT_BUFFER) {
        *rc = r;  // typically 15700 = APPMODEL_ERROR_NO_PACKAGE
        return false;
    }
    std::wstring s(n, L'\0');
    r = fn(&n, &s[0]);
    if (r != ERROR_SUCCESS) {
        *rc = r;
        return false;
    }
    s.resize(n ? n - 1 : 0);  // n includes the terminating NUL
    *out = s;
    *rc = 0;
    return true;
}

Resolved package_unavailable(const char* what, LONG rc) {
    if (rc == -1) return unresolved(Status::SKIPPED, std::string(what) + " API not exported");
    if (rc == 15700)  // APPMODEL_ERROR_NO_PACKAGE
        return unresolved(Status::SKIPPED, "process has no package identity (desktop process)",
                          static_cast<uint32_t>(rc));
    return unresolved(Status::INFO, std::string(what) + " failed: " +
                                        error_text(static_cast<uint32_t>(rc)),
                      static_cast<uint32_t>(rc));
}

Resolved res_package_install() {
    std::wstring p;
    LONG rc = 0;
    if (!query_package_string("GetCurrentPackagePath", &p, &rc))
        return package_unavailable("GetCurrentPackagePath", rc);
    return resolved_ok(p, "GetCurrentPackagePath");
}

// LocalState lives next to the AC folder: %LOCALAPPDATA% inside an AppContainer is
// ...\Packages\<pfn>\AC, so LocalState is its sibling. Fall back to
// %LOCALAPPDATA%\Packages\<pfn>\LocalState if LOCALAPPDATA is not the AC folder.
Resolved res_package_localstate() {
    std::wstring pfn;
    LONG rc = 0;
    if (!query_package_string("GetCurrentPackageFamilyName", &pfn, &rc))
        return package_unavailable("GetCurrentPackageFamilyName", rc);
    std::wstring lad;
    if (!get_env(L"LOCALAPPDATA", &lad))
        return unresolved(Status::MISSING, "package family " + narrow(pfn) +
                                               " known but %LOCALAPPDATA% is not set");
    if (ends_with_nocase(lad, L"\\AC") || ends_with_nocase(lad, L"\\AC\\")) {
        while (!lad.empty() && lad.back() == L'\\') lad.pop_back();
        lad.resize(lad.size() - 3);  // strip "\AC"
        return resolved_ok(lad + L"\\LocalState",
                           "derived: sibling of AppContainer %LOCALAPPDATA% (pfn " +
                               narrow(pfn) + ")");
    }
    return resolved_ok(with_slash(lad) + L"Packages\\" + pfn + L"\\LocalState",
                       "derived: %LOCALAPPDATA%\\Packages\\<pfn>\\LocalState (pfn " +
                           narrow(pfn) + ")");
}

// ---- READ operations --------------------------------------------------------------

// Open the directory for listing and count its entries.
void read_list(Record& rec, const Resolved& res) {
    const std::wstring pattern = with_slash(res.path) + L"*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        fail(rec, GetLastError(), "FindFirstFileExW", pattern);
        return;
    }
    const unsigned kCap = 100000;
    unsigned entries = 0;
    std::wstring first;
    DWORD stop_err = ERROR_NO_MORE_FILES;
    for (;;) {
        if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
            if (entries == 0) first = fd.cFileName;
            ++entries;
            if (entries >= kCap) break;
        }
        if (!FindNextFileW(h, &fd)) {
            stop_err = GetLastError();
            break;
        }
    }
    FindClose(h);
    rec.value = "first=" + narrow(first) + ";entries=" + std::to_string(entries);
    rec.detail = "listed " + narrow(pattern) + " [" + res.note + "]";
    if (entries >= kCap) {
        rec.value += ";capped=1";
    } else if (stop_err != ERROR_NO_MORE_FILES) {
        rec.status = Status::WARN;
        rec.errorcode = stop_err;
        rec.detail += "; enumeration aborted: " + error_text(stop_err);
        return;
    }
    rec.status = Status::OK;
}

// CreateFile2 (not CreateFileW) so the identical call compiles and runs on both
// the desktop host and the UWP host, where CreateFileW is not in the API surface.
HANDLE create_file(const std::wstring& path, DWORD access, DWORD share, DWORD disposition,
                   DWORD file_flags) {
    CREATEFILE2_EXTENDED_PARAMETERS p;
    memset(&p, 0, sizeof p);
    p.dwSize = sizeof p;
    p.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    p.dwFileFlags = file_flags;
    return CreateFile2(path.c_str(), access, share, disposition, &p);
}

HANDLE open_for_read(const std::wstring& file) {
    return create_file(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       OPEN_EXISTING, 0);
}

std::string hex_bytes(const unsigned char* p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 0xF];
    }
    return s;
}

// System32\kernel32.dll: open GENERIC_READ, read 2 bytes, expect "MZ".
void read_system32_kernel32(Record& rec, const Resolved& res) {
    const std::wstring file = with_slash(res.path) + L"kernel32.dll";
    HANDLE h = open_for_read(file);
    if (h == INVALID_HANDLE_VALUE) {
        fail(rec, GetLastError(), "CreateFile2(GENERIC_READ)", file);
        return;
    }
    unsigned char buf[2] = {0, 0};
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof buf, &got, nullptr);
    const DWORD err = ok ? 0 : GetLastError();
    LARGE_INTEGER size;
    size.QuadPart = 0;
    GetFileSizeEx(h, &size);
    CloseHandle(h);
    if (!ok) {
        fail(rec, err, "ReadFile", file);
        return;
    }
    const bool mz = got == 2 && buf[0] == 'M' && buf[1] == 'Z';
    rec.value = std::string("MZ=") + (mz ? "1" : "0") + ";bytes=" + std::to_string(got);
    rec.detail = "opened " + narrow(file) + " (size " + std::to_string(size.QuadPart) + ")";
    rec.status = mz ? Status::OK : Status::WARN;
    if (!mz) rec.detail += "; first bytes are not 'MZ'";
}

// System32\ntdll.dll: open GENERIC_READ, read 64 bytes (the DOS header), report
// the first two bytes and a sanity check of e_lfanew (offset 0x3C).
void read_system32_ntdll(Record& rec, const Resolved& res) {
    const std::wstring file = with_slash(res.path) + L"ntdll.dll";
    HANDLE h = open_for_read(file);
    if (h == INVALID_HANDLE_VALUE) {
        fail(rec, GetLastError(), "CreateFile2(GENERIC_READ)", file);
        return;
    }
    unsigned char buf[64];
    memset(buf, 0, sizeof buf);
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof buf, &got, nullptr);
    const DWORD err = ok ? 0 : GetLastError();
    LARGE_INTEGER size;
    size.QuadPart = 0;
    GetFileSizeEx(h, &size);
    CloseHandle(h);
    if (!ok) {
        fail(rec, err, "ReadFile", file);
        return;
    }
    uint32_t e_lfanew = 0;
    if (got >= 64) memcpy(&e_lfanew, buf + 0x3C, sizeof e_lfanew);
    const bool mz = got >= 2 && buf[0] == 'M' && buf[1] == 'Z';
    const bool lfanew_ok = got >= 64 && e_lfanew >= 0x40 &&
                           static_cast<int64_t>(e_lfanew) < size.QuadPart;
    char lf[24];
    std::snprintf(lf, sizeof lf, "0x%X", e_lfanew);
    rec.value = "opened=1;first2=" + hex_bytes(buf, got >= 2 ? 2 : got) +
                ";bytes=" + std::to_string(got) + ";e_lfanew=" + lf +
                ";e_lfanew_plausible=" + (lfanew_ok ? "1" : "0");
    rec.detail = "opened " + narrow(file) + " (size " + std::to_string(size.QuadPart) + ")";
    if (mz && lfanew_ok) {
        rec.status = Status::OK;
    } else {
        rec.status = Status::WARN;
        rec.detail += "; DOS header did not look like a PE image";
    }
}

// ---- WRITE operation ----------------------------------------------------------------

std::wstring make_temp_name() {
    static std::atomic<unsigned> seq{0};
    return L"__probe_tmp_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
           std::to_wstring(static_cast<unsigned long long>(GetTickCount64())) + L"_" +
           std::to_wstring(seq.fetch_add(1)) + L".tmp";
}

// RAII owner of one temp file: whatever happens, the handle is closed and the file
// is deleted (explicitly, on top of FILE_FLAG_DELETE_ON_CLOSE) when this goes away.
struct TempFile {
    std::wstring path;
    HANDLE h = INVALID_HANDLE_VALUE;
    bool created = false;  // set only after a successful create

    ~TempFile() {
        DWORD ignored = 0;
        ensure_deleted(&ignored);
    }

    void close() {
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
    }

    enum Presence { Present, Gone, Unknown };
    Presence presence(DWORD* err) const {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return Present;
        *err = GetLastError();
        if (*err == ERROR_FILE_NOT_FOUND || *err == ERROR_PATH_NOT_FOUND) return Gone;
        return Unknown;
    }

    // True when the file no longer exists. On failure *err holds the last error.
    bool ensure_deleted(DWORD* err) {
        close();
        if (!created) return true;
        DWORD last = 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (attempt) Sleep(20);  // let a scanner / delete-pending state settle
            if (presence(&last) == Gone) {
                created = false;
                return true;
            }
            if (DeleteFileW(path.c_str())) {
                created = false;
                return true;
            }
            last = GetLastError();
            if (last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND) {
                created = false;
                return true;
            }
        }
        *err = last;
        return false;
    }
};

// create + write 16 bytes + read back + delete, inside res.path.
void write_cycle(Record& rec, const Resolved& res) {
    static const char kPattern[17] = "XBOXPROBE-F-0001";  // exactly 16 bytes + NUL

    TempFile tf;
    tf.path = with_slash(res.path) + make_temp_name();
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    // First try with DELETE access + delete-on-close (crash-proof cleanup).
    bool delete_on_close = true;
    tf.h = create_file(tf.path, GENERIC_READ | GENERIC_WRITE | DELETE, share, CREATE_NEW,
                       FILE_FLAG_DELETE_ON_CLOSE);
    if (tf.h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
        // Maybe only the DELETE access was refused; retry as a plain create.
        delete_on_close = false;
        tf.h = create_file(tf.path, GENERIC_READ | GENERIC_WRITE, share, CREATE_NEW, 0);
    }
    if (tf.h == INVALID_HANDLE_VALUE) {
        fail(rec, GetLastError(), "CreateFile2(CREATE_NEW)", tf.path);
        return;
    }
    tf.created = true;

    // Stage: write 16 bytes, rewind, read back, compare.
    Status stage_status = Status::OK;
    DWORD stage_err = 0;
    std::string stage_what;
    DWORD wrote = 0;
    char back[16] = {0};
    DWORD got = 0;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    bool readback_match = false;

    if (!WriteFile(tf.h, kPattern, 16, &wrote, nullptr) || wrote != 16) {
        stage_err = GetLastError();
        stage_what = "WriteFile";
    } else if (!SetFilePointerEx(tf.h, zero, nullptr, FILE_BEGIN)) {
        stage_err = GetLastError();
        stage_what = "SetFilePointerEx";
    } else if (!ReadFile(tf.h, back, 16, &got, nullptr) || got != 16) {
        stage_err = GetLastError();
        stage_what = "ReadFile(readback)";
    } else {
        readback_match = memcmp(back, kPattern, 16) == 0;
    }

    // Stage: delete (close the handle; delete-on-close removes it, else explicit).
    DWORD del_err = 0;
    const bool deleted = tf.ensure_deleted(&del_err);

    if (!stage_what.empty()) {
        stage_status = classify(stage_err);
        rec.status = stage_status;
        rec.errorcode = stage_err;
        rec.detail = "created " + narrow(tf.path) + " but " + stage_what +
                     " failed: " + error_text(stage_err);
        rec.value = "created=1;wrote=" + std::to_string(wrote) + ";deleted=" + (deleted ? "1" : "0");
    } else if (!readback_match) {
        rec.status = Status::WARN;
        rec.detail = "created and wrote " + narrow(tf.path) + " but readback bytes differ";
        rec.value = "created=1;wrote=16;readback=0;deleted=" + std::string(deleted ? "1" : "0");
    } else {
        rec.status = Status::OK;
        rec.detail = "create+write+readback+delete in " + narrow(res.path) + " [" + res.note +
                     "]" + (delete_on_close ? "" : " (delete-on-close not permitted; deleted explicitly)");
        rec.value = "created=1;wrote=16;readback=1;deleted=" + std::string(deleted ? "1" : "0");
    }
    if (!deleted) {
        // Never silently report a clean pass if cleanup failed.
        rec.status = Status::WARN;
        rec.errorcode = del_err;
        rec.detail += "; CLEANUP FAILED, temp file left behind: " + narrow(tf.path) + " (" +
                      error_text(del_err) + ")";
    }
}

// ---- target table -----------------------------------------------------------------------

typedef Resolved (*ResolveFn)();
typedef void (*OpFn)(Record&, const Resolved&);

struct Target {
    const char* label;
    ResolveFn resolve_path;
    OpFn read;   // may not be null
    OpFn write;  // null = no write record for this label
};

const Target kTargets[] = {
    {"localstate(localappdata)", res_localappdata, read_list, write_cycle},
    {"package-install", res_package_install, read_list, write_cycle},
    {"package-localstate", res_package_localstate, read_list, write_cycle},
    {"windows-dir", res_windows, read_list, write_cycle},
    {"system32", res_system32, read_system32_kernel32, write_cycle},
    {"system32-read-ntdll", res_system32, read_system32_ntdll, nullptr},
    {"drive-c-root", res_drive_c, read_list, write_cycle},
    {"temp", res_temp, read_list, write_cycle},
    {"programdata", res_programdata, read_list, write_cycle},
    {"userprofile", res_userprofile, read_list, write_cycle},
    {"known-documents", res_known_documents, read_list, write_cycle},
};

void run_op(Reporter& r, const char* prefix, const Target& t, OpFn op) {
    r.run("F", std::string(prefix) + t.label, [&t, op](Record& rec) {
        const Resolved res = t.resolve_path();
        if (!res.ok) {
            rec.status = res.why;
            rec.errorcode = res.err;
            rec.detail = "path not resolvable: " + res.note;
            return;
        }
        op(rec, res);
    });
}

}  // namespace

void register_F_filesystem(Reporter& r) {
    for (const Target& t : kTargets) {
        run_op(r, "read:", t, t.read);
        if (t.write) run_op(r, "write:", t, t.write);
    }
}

}  // namespace probe
