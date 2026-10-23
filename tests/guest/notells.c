/* guest_notells: an ordinary program that LOOKS for Wine does not find it (docs/slices/notells.md).
 *
 * The checks a Windows program (or an anti-tamper / "IsWine" routine) can make without any special knowledge of the
 * layer, each printing `notells: <check> ok` or `FAIL <what>`; exit 42 only when every check passed:
 *   getprocaddress   ntdll!wine_get_version / wine_get_build_id / wine_get_host_version and
 *                    kernel32!wine_get_unix_file_name / wine_get_dos_file_name are not exported
 *   exports          no export of ntdll, kernelbase, kernel32, win32u, user32 or gdi32 has "wine_" in its name,
 *                    except the three wine_server_* entry points programs/conhost.exe imports (printed, allowed);
 *                    the __wine_* names are counted and printed, not gated
 *   version-<dll>    GetFileVersionInfoW / VerQueryValueW on System32\ntdll.dll, kernel32.dll, user32.dll: the file
 *                    version is 10.0.19045, company "Microsoft Corporation", the Windows file description, product
 *                    "Microsoft(R) Windows(R) Operating System", and no string of the whole block says "wine"
 *   registry         HKCU\Software\Wine, HKLM\Software\Wine (and the 32-bit view) do not exist
 *   environment      no variable starts with WINE or WIN10XBOX
 *   drives           no Z: (GetLogicalDrives, GetDriveTypeW, GetFileAttributesW, \\.\Z:, QueryDosDeviceW), C: reads
 *                    as \Device\HarddiskVolume1, DefineDosDevice makes a drive and removes it again
 *   os-version       RtlGetVersion, GetVersionExW (the manifest asks for Windows 10) and GetVersion say 10.0.19045
 * `info` lines are measurements that are not gated (the DOS-stub marker of the builtin DLLs, the DOS device names).
 *
 * `--shared-folder` is for the shared machine folder, where "native,builtin" maps the runner's own build-26100
 * version.dll, user32's imports (advapi32, sechost) and so on, which cannot load under Wine's ntdll (docs/TESTING.md):
 * it skips the version-<dll> checks and scans only the modules every guest has (ntdll, kernelbase, kernel32); the
 * builtin-only folder, the console's DLL set, runs all of them.
 * Plain C for MSVC and mingw. Run through the layer; no function-local statics. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winver.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* version.dll is loaded when the version checks run, not linked: on the shared machine folder the runner's native one
 * would be bound at startup, and the whole program would die on a Wine import stub before printing a line. */
typedef DWORD (WINAPI *GetFileVersionInfoSizeWFn)(LPCWSTR, LPDWORD);
typedef BOOL (WINAPI *GetFileVersionInfoWFn)(LPCWSTR, DWORD, DWORD, LPVOID);
typedef BOOL (WINAPI *VerQueryValueWFn)(LPCVOID, LPCWSTR, LPVOID *, PUINT);
static GetFileVersionInfoSizeWFn p_GetFileVersionInfoSizeW;
static GetFileVersionInfoWFn p_GetFileVersionInfoW;
static VerQueryValueWFn p_VerQueryValueW;

#define EXPECT_MAJOR 10u
#define EXPECT_MINOR 0u
#define EXPECT_BUILD 19045u

static int g_fail;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("FAIL %s\n", what);
        g_fail++;
    }
}

static void narrow(const wchar_t *w, char *out, size_t cap) {
    size_t i = 0;
    for (; w[i] && i + 1 < cap; i++) out[i] = w[i] < 0x80 ? (char)w[i] : '?';
    out[i] = 0;
}

/* Case-insensitive search of a narrow needle (lower case) in a UTF-16 buffer of n wide characters. */
static int wide_contains_nocase(const wchar_t *hay, size_t n, const char *needle) {
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        for (; k < m; k++) {
            wchar_t c = hay[i + k];
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
            if (c != (wchar_t)(unsigned char)needle[k]) break;
        }
        if (k == m) return 1;
    }
    return 0;
}

static int narrow_starts_nocase(const wchar_t *s, const char *prefix) {
    for (; *prefix; s++, prefix++) {
        wchar_t c = *s;
        if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
        if (c != (wchar_t)(unsigned char)*prefix) return 0;
    }
    return 1;
}

/* ---- GetProcAddress ------------------------------------------------------------------------------------------- */

static const char *const k_ntdll_names[] = {"wine_get_version", "wine_get_build_id", "wine_get_host_version"};
static const char *const k_kernel32_names[] = {"wine_get_unix_file_name", "wine_get_dos_file_name"};

static void check_getprocaddress(void) {
    int before = g_fail;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll"), k32 = GetModuleHandleW(L"kernel32.dll");
    check(nt != NULL && k32 != NULL, "ntdll.dll / kernel32.dll module handles");
    for (size_t i = 0; nt && i < sizeof(k_ntdll_names) / sizeof(k_ntdll_names[0]); i++) {
        FARPROC p = GetProcAddress(nt, k_ntdll_names[i]);
        if (p) {
            printf("FAIL GetProcAddress(ntdll, \"%s\") = %p\n", k_ntdll_names[i], (void *)p);
            g_fail++;
        }
    }
    for (size_t i = 0; k32 && i < sizeof(k_kernel32_names) / sizeof(k_kernel32_names[0]); i++) {
        FARPROC p = GetProcAddress(k32, k_kernel32_names[i]);
        if (p) {
            printf("FAIL GetProcAddress(kernel32, \"%s\") = %p\n", k_kernel32_names[i], (void *)p);
            g_fail++;
        }
    }
    if (g_fail == before) printf("notells: getprocaddress ok\n");
}

/* ---- export tables -------------------------------------------------------------------------------------------- */

/* The only "wine_" exports left: imported by programs/conhost.exe (Wine's own console host), which the layer does not
 * start; they answer STATUS_NOT_IMPLEMENTED. */
static const char *const k_allowed_wine_exports[] = {"wine_server_call", "wine_server_fd_to_handle",
                                                     "wine_server_handle_to_fd"};

static int allowed_wine_export(const char *dll, const char *name) {
    if (_stricmp(dll, "ntdll.dll") != 0) return 0;
    for (size_t i = 0; i < sizeof(k_allowed_wine_exports) / sizeof(k_allowed_wine_exports[0]); i++)
        if (strcmp(name, k_allowed_wine_exports[i]) == 0) return 1;
    return 0;
}

static void scan_exports(const char *dll, int load, int *dunder_total) {
    HMODULE mod = GetModuleHandleA(dll);
    if (!mod && load) mod = LoadLibraryA(dll);   /* user32 (and with it win32u, gdi32) is not mapped by a console guest */
    if (!mod) {
        printf("FAIL exports: %s is not loadable (error %lu)\n", dll, GetLastError());
        g_fail++;
        return;
    }
    const BYTE *base = (const BYTE *)mod;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd->VirtualAddress) {
        printf("FAIL exports: %s has no export directory\n", dll);
        g_fail++;
        return;
    }
    const IMAGE_EXPORT_DIRECTORY *ed = (const IMAGE_EXPORT_DIRECTORY *)(base + dd->VirtualAddress);
    const DWORD *names = (const DWORD *)(base + ed->AddressOfNames);
    int wine_named = 0, dunder = 0, allowed = 0;
    for (DWORD i = 0; i < ed->NumberOfNames; i++) {
        const char *n = (const char *)(base + names[i]);
        if (strncmp(n, "__wine_", 7) == 0) {
            dunder++;
        } else if (strstr(n, "wine_") != NULL) {
            if (allowed_wine_export(dll, n)) {
                allowed++;
                printf("notells: info allowed export %s!%s\n", dll, n);
            } else {
                printf("FAIL exports: %s exports %s\n", dll, n);
                wine_named++;
                g_fail++;
            }
        }
    }
    printf("notells: info %s: %lu named exports, %d wine_ (not allowed) %d wine_ (allowed) %d __wine_\n", dll,
           (unsigned long)ed->NumberOfNames, wine_named, allowed, dunder);
    *dunder_total += dunder;
}

static const char *const k_scanned_dlls[] = {"ntdll.dll", "kernelbase.dll", "kernel32.dll",
                                             "win32u.dll", "user32.dll",    "gdi32.dll"};

static void check_exports(int shared_folder) {
    int before = g_fail, dunder_total = 0;
    for (size_t i = 0; i < sizeof(k_scanned_dlls) / sizeof(k_scanned_dlls[0]); i++) {
        if (shared_folder && i >= 3) break;   /* ntdll, kernelbase, kernel32 only */
        scan_exports(k_scanned_dlls[i], !shared_folder, &dunder_total);
    }
    printf("notells: info __wine_ exports in total: %d (not gated)\n", dunder_total);
    if (g_fail == before) printf("notells: exports ok\n");
}

/* ---- version resources ---------------------------------------------------------------------------------------- */

static const wchar_t *const k_string_names[] = {L"CompanyName",      L"FileDescription", L"FileVersion",
                                                L"InternalName",     L"LegalCopyright",  L"OriginalFilename",
                                                L"ProductName",      L"ProductVersion"};

static int query_string(const void *block, WORD lang, WORD cp, const wchar_t *name, const wchar_t **out) {
    wchar_t sub[96];
    UINT len = 0;
    void *p = NULL;
    _snwprintf(sub, sizeof(sub) / sizeof(sub[0]), L"\\StringFileInfo\\%04x%04x\\%ls", lang, cp, name);
    sub[sizeof(sub) / sizeof(sub[0]) - 1] = 0;
    if (!p_VerQueryValueW(block, sub, &p, &len) || !p) return 0;
    *out = (const wchar_t *)p;
    return 1;
}

static void check_version(const wchar_t *file, const wchar_t *want_description) {
    char nfile[32];
    int before = g_fail;
    narrow(file, nfile, sizeof(nfile));
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n + 1 + wcslen(file) >= MAX_PATH) {
        printf("FAIL version %s: GetSystemDirectoryW\n", nfile);
        g_fail++;
        return;
    }
    wcscat(path, L"\\");
    wcscat(path, file);
    DWORD handle = 0;
    DWORD size = p_GetFileVersionInfoSizeW(path, &handle);
    if (!size) {
        printf("FAIL version %s: GetFileVersionInfoSizeW error %lu\n", nfile, GetLastError());
        g_fail++;
        return;
    }
    void *block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    if (!block || !p_GetFileVersionInfoW(path, 0, size, block)) {
        printf("FAIL version %s: GetFileVersionInfoW error %lu\n", nfile, GetLastError());
        g_fail++;
        return;
    }
    /* The fixed part: file and product version 10.0.19045.x. */
    VS_FIXEDFILEINFO *ffi = NULL;
    UINT len = 0;
    if (!p_VerQueryValueW(block, L"\\", (void **)&ffi, &len) || !ffi || len < sizeof(*ffi)) {
        printf("FAIL version %s: no VS_FIXEDFILEINFO\n", nfile);
        g_fail++;
    } else {
        printf("notells: info %s file %u.%u.%u.%u product %u.%u.%u.%u\n", nfile, HIWORD(ffi->dwFileVersionMS),
               LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS),
               HIWORD(ffi->dwProductVersionMS), LOWORD(ffi->dwProductVersionMS), HIWORD(ffi->dwProductVersionLS),
               LOWORD(ffi->dwProductVersionLS));
        check(HIWORD(ffi->dwFileVersionMS) == EXPECT_MAJOR && LOWORD(ffi->dwFileVersionMS) == EXPECT_MINOR &&
                  HIWORD(ffi->dwFileVersionLS) == EXPECT_BUILD,
              "file version is not 10.0.19045");
        check(HIWORD(ffi->dwProductVersionMS) == EXPECT_MAJOR && LOWORD(ffi->dwProductVersionMS) == EXPECT_MINOR &&
                  HIWORD(ffi->dwProductVersionLS) == EXPECT_BUILD,
              "product version is not 10.0.19045");
    }
    /* No string of the whole block, in any language, says Wine. */
    check(!wide_contains_nocase((const wchar_t *)block, size / sizeof(wchar_t), "wine"),
          "the version block contains \"wine\"");
    /* Every translation the file offers carries the Windows strings. */
    struct { WORD lang, cp; } *tr = NULL;
    len = 0;
    if (!p_VerQueryValueW(block, L"\\VarFileInfo\\Translation", (void **)&tr, &len) || !tr || len < 4) {
        printf("FAIL version %s: no Translation\n", nfile);
        g_fail++;
    } else {
        for (UINT t = 0; t < len / 4; t++) {
            for (size_t s = 0; s < sizeof(k_string_names) / sizeof(k_string_names[0]); s++) {
                const wchar_t *val = NULL;
                char nn[32], nv[256];
                narrow(k_string_names[s], nn, sizeof(nn));
                if (!query_string(block, tr[t].lang, tr[t].cp, k_string_names[s], &val)) {
                    printf("FAIL version %s: %04x%04x %s missing\n", nfile, tr[t].lang, tr[t].cp, nn);
                    g_fail++;
                    continue;
                }
                narrow(val, nv, sizeof(nv));
                printf("notells: info %s %04x%04x %s=%s\n", nfile, tr[t].lang, tr[t].cp, nn, nv);
                if (wcscmp(k_string_names[s], L"CompanyName") == 0)
                    check(wcscmp(val, L"Microsoft Corporation") == 0, "CompanyName is not Microsoft Corporation");
                else if (wcscmp(k_string_names[s], L"FileDescription") == 0)
                    check(wcscmp(val, want_description) == 0, "FileDescription is not the Windows one");
                else if (wcscmp(k_string_names[s], L"ProductName") == 0)
                    check(wcscmp(val, L"Microsoft\u00ae Windows\u00ae Operating System") == 0,
                          "ProductName is not the Windows one");
                else if (wcscmp(k_string_names[s], L"InternalName") == 0 ||
                         wcscmp(k_string_names[s], L"OriginalFilename") == 0)
                    check(_wcsicmp(val, file) == 0, "InternalName / OriginalFilename is not the file name");
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, block);
    if (g_fail == before) {
        char *dot = strrchr(nfile, '.');
        if (dot) *dot = 0;
        printf("notells: version-%s ok\n", nfile);
    }
}

/* ---- registry, environment, drives ---------------------------------------------------------------------------- */

static void check_registry(void) {
    struct { HKEY root; const wchar_t *sub; REGSAM wow; const char *what; } keys[] = {
        {HKEY_CURRENT_USER, L"Software\\Wine", 0, "HKCU\\Software\\Wine"},
        {HKEY_LOCAL_MACHINE, L"Software\\Wine", 0, "HKLM\\Software\\Wine"},
        {HKEY_LOCAL_MACHINE, L"Software\\Wine", KEY_WOW64_32KEY, "HKLM\\Software\\Wine (32-bit view)"},
        {HKEY_LOCAL_MACHINE, L"Software\\Wine", KEY_WOW64_64KEY, "HKLM\\Software\\Wine (64-bit view)"},
        {HKEY_CURRENT_USER, L"Software\\Wine\\Drivers", 0, "HKCU\\Software\\Wine\\Drivers"},
    };
    int before = g_fail;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        HKEY k = NULL;
        LONG rc = RegOpenKeyExW(keys[i].root, keys[i].sub, 0, KEY_READ | keys[i].wow, &k);
        printf("notells: info RegOpenKeyExW(%s) = %ld\n", keys[i].what, (long)rc);
        if (rc == ERROR_SUCCESS) {
            RegCloseKey(k);
            printf("FAIL registry: %s exists\n", keys[i].what);
            g_fail++;
        } else if (rc != ERROR_FILE_NOT_FOUND) {
            printf("FAIL registry: %s answers %ld, not ERROR_FILE_NOT_FOUND\n", keys[i].what, (long)rc);
            g_fail++;
        }
    }
    if (g_fail == before) printf("notells: registry ok\n");
}

static void check_environment(void) {
    int before = g_fail, count = 0;
    wchar_t *env = GetEnvironmentStringsW();
    check(env != NULL, "GetEnvironmentStringsW");
    for (const wchar_t *p = env; p && *p; p += wcslen(p) + 1) {
        count++;
        if (narrow_starts_nocase(p, "WINE") || narrow_starts_nocase(p, "WIN10XBOX")) {
            char nv[128];
            narrow(p, nv, sizeof(nv));
            printf("FAIL environment: %s\n", nv);
            g_fail++;
        }
    }
    if (env) FreeEnvironmentStringsW(env);
    printf("notells: info environment has %d variables\n", count);
    if (g_fail == before) printf("notells: environment ok\n");
}

static void check_drives(void) {
    int before = g_fail;
    DWORD mask = GetLogicalDrives();
    char letters[27];
    int n = 0;
    for (int i = 0; i < 26; i++)
        if (mask & (1u << i)) letters[n++] = (char)('A' + i);
    letters[n] = 0;
    printf("notells: info logical drives: %s\n", letters);
    check(mask != 0, "GetLogicalDrives is empty");
    check((mask & (1u << ('Z' - 'A'))) == 0, "Z: is a logical drive");
    check(GetDriveTypeW(L"Z:\\") == DRIVE_NO_ROOT_DIR, "GetDriveTypeW(Z:\\) is not DRIVE_NO_ROOT_DIR");
    check(GetFileAttributesW(L"Z:\\") == INVALID_FILE_ATTRIBUTES, "GetFileAttributesW(Z:\\) succeeds");
    HANDLE h = CreateFileW(L"\\\\.\\Z:", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    check(h == INVALID_HANDLE_VALUE, "CreateFileW(\\\\.\\Z:) succeeds");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    wchar_t target[512];
    check(QueryDosDeviceW(L"Z:", target, 512) == 0, "QueryDosDeviceW(Z:) succeeds");
    /* C: reads like a volume of a Windows machine (the VFS numbers C: as \Device\HarddiskVolume1). */
    wchar_t ctarget[512];
    if (QueryDosDeviceW(L"C:", ctarget, 512) == 0) {
        printf("FAIL QueryDosDeviceW(C:) error %lu\n", GetLastError());
        g_fail++;
    } else {
        char nt[128];
        narrow(ctarget, nt, sizeof(nt));
        printf("notells: info QueryDosDeviceW(C:) = %s\n", nt);
        check(wcsncmp(ctarget, L"\\Device\\HarddiskVolume", 22) == 0, "QueryDosDeviceW(C:) is not a \\Device\\HarddiskVolumeN");
    }
    /* DefineDosDevice: a drive the program makes itself shows up and goes away again (the guest's own namespace). */
    wchar_t qtarget[512];
    check(DefineDosDeviceW(0, L"Q:", L"C:\\Windows"), "DefineDosDeviceW(Q:) fails");
    qtarget[0] = 0;
    check(QueryDosDeviceW(L"Q:", qtarget, 512) != 0 && wcsncmp(qtarget, L"\\??\\C:", 6) == 0,
          "QueryDosDeviceW(Q:) does not return the definition");
    {
        char nq[128];
        narrow(qtarget, nq, sizeof(nq));
        printf("notells: info QueryDosDeviceW(Q:) = %s\n", nq);
    }
    check(GetLogicalDrives() & (1u << ('Q' - 'A')), "Q: is not in GetLogicalDrives after DefineDosDeviceW");
    check(DefineDosDeviceW(DDD_REMOVE_DEFINITION, L"Q:", NULL), "DefineDosDeviceW(Q:, remove) fails");
    SetLastError(0);
    check(QueryDosDeviceW(L"Q:", qtarget, 512) == 0 && GetLastError() == ERROR_FILE_NOT_FOUND,
          "QueryDosDeviceW(Q:) still answers after the definition was removed");
    check((GetLogicalDrives() & (1u << ('Q' - 'A'))) == 0, "Q: is still in GetLogicalDrives after the removal");
    /* Informational: every DOS device name the guest can list. */
    wchar_t *all = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 65536 * sizeof(wchar_t));
    DWORD got = all ? QueryDosDeviceW(NULL, all, 65536) : 0;
    if (!got) {
        printf("notells: info QueryDosDeviceW(NULL) failed, error %lu\n", GetLastError());
    } else {
        char line[1024];
        size_t used = 0;
        int names = 0, wine_named = 0;
        line[0] = 0;
        for (const wchar_t *p = all; *p; p += wcslen(p) + 1) {
            char nn[64];
            narrow(p, nn, sizeof(nn));
            names++;
            if (wide_contains_nocase(p, wcslen(p), "wine")) wine_named++;
            if (used + strlen(nn) + 2 < sizeof(line)) used += (size_t)snprintf(line + used, sizeof(line) - used, "%s ", nn);
        }
        printf("notells: info %d DOS device names: %s\n", names, line);
        check(wine_named == 0, "a DOS device name contains \"wine\"");
    }
    if (all) HeapFree(GetProcessHeap(), 0, all);
    if (g_fail == before) printf("notells: drives ok\n");
}

/* ---- the OS version ------------------------------------------------------------------------------------------- */

typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOEXW *);
typedef BOOL (WINAPI *GetVersionExWFn)(OSVERSIONINFOW *);

static void check_os_version(void) {
    int before = g_fail;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll"), k32 = GetModuleHandleW(L"kernel32.dll");
    RtlGetVersionFn rtl = nt ? (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion") : NULL;
    GetVersionExWFn gve = k32 ? (GetVersionExWFn)GetProcAddress(k32, "GetVersionExW") : NULL;
    check(rtl != NULL && gve != NULL, "RtlGetVersion / GetVersionExW exports");
    if (rtl) {
        OSVERSIONINFOEXW v;
        memset(&v, 0, sizeof(v));
        v.dwOSVersionInfoSize = sizeof(v);
        check(rtl(&v) == 0, "RtlGetVersion status");
        printf("notells: info RtlGetVersion %lu.%lu.%lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
        check(v.dwMajorVersion == EXPECT_MAJOR && v.dwMinorVersion == EXPECT_MINOR && v.dwBuildNumber == EXPECT_BUILD,
              "RtlGetVersion is not 10.0.19045");
    }
    if (gve) {
        OSVERSIONINFOW v;
        memset(&v, 0, sizeof(v));
        v.dwOSVersionInfoSize = sizeof(v);
        check(gve(&v), "GetVersionExW");
        printf("notells: info GetVersionExW %lu.%lu.%lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
        check(v.dwMajorVersion == EXPECT_MAJOR && v.dwMinorVersion == EXPECT_MINOR && v.dwBuildNumber == EXPECT_BUILD,
              "GetVersionExW is not 10.0.19045");
    }
    DWORD gv = GetVersion();
    check((gv & 0xff) == EXPECT_MAJOR && ((gv >> 8) & 0xff) == EXPECT_MINOR && (gv >> 16) == EXPECT_BUILD,
          "GetVersion is not 10.0.19045");
    if (g_fail == before) printf("notells: os-version ok\n");
}

/* ---- not gated ------------------------------------------------------------------------------------------------ */

/* Wine writes "Wine builtin DLL" into the DOS stub of its PE builtins (winebuild); what the file says about itself. */
static void info_dos_stub(const char *dll) {
    HMODULE mod = GetModuleHandleA(dll);
    if (!mod) return;
    const char *p = (const char *)mod;
    int found = 0;
    for (size_t i = 0; i + 16 <= 0x400; i++)
        if (memcmp(p + i, "Wine builtin DLL", 16) == 0) found = 1;
    printf("notells: info %s DOS stub marker \"Wine builtin DLL\": %s\n", dll, found ? "present" : "absent");
}

int main(int argc, char **argv) {
    int shared_folder = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--shared-folder") == 0) shared_folder = 1;

    check_getprocaddress();
    check_exports(shared_folder);
    if (!shared_folder) {
        HMODULE ver = LoadLibraryW(L"version.dll");
        p_GetFileVersionInfoSizeW = ver ? (GetFileVersionInfoSizeWFn)GetProcAddress(ver, "GetFileVersionInfoSizeW") : NULL;
        p_GetFileVersionInfoW = ver ? (GetFileVersionInfoWFn)GetProcAddress(ver, "GetFileVersionInfoW") : NULL;
        p_VerQueryValueW = ver ? (VerQueryValueWFn)GetProcAddress(ver, "VerQueryValueW") : NULL;
        check(p_GetFileVersionInfoSizeW && p_GetFileVersionInfoW && p_VerQueryValueW, "version.dll and its exports");
    }
    if (!shared_folder && p_VerQueryValueW) {
        check_version(L"ntdll.dll", L"NT Layer DLL");
        check_version(L"kernel32.dll", L"Windows NT BASE API Client DLL");
        check_version(L"user32.dll", L"Multi-User Windows USER API Client DLL");
    } else {
        printf("notells: info version-<dll> checks and the user32 / win32u / gdi32 export scan are not run on the shared folder\n");
    }
    check_registry();
    check_environment();
    check_drives();
    check_os_version();
    info_dos_stub("ntdll.dll");
    info_dos_stub("kernel32.dll");

    if (g_fail) {
        printf("notells: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("notells: done\n");
    return 42;
}
