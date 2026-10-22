/*
 * ac_probe.c - what does the platform let a process in THIS security context do?
 *
 *   ac_probe <workdir> <machine-dir> <negative-control-file>
 *
 * Run it three times with the same arguments (host-win32-appcontainer.yml does): as an ordinary process (the control), in
 * a genuine AppContainer, and in a less-privileged one. The difference between the outputs IS the measurement. It exists
 * because the project has so far SUBSTITUTED GUESSES FOR MEASUREMENTS about the console's AppContainer:
 *
 *   - GetFinalPathNameByHandleW(VOLUME_NAME_DOS) is refused on the Xbox. core/dispatch/nt_file.cpp makes that call
 *     (HostPathOfHandle) and simulates its failure with WIN10XBOX_TEST_NO_HOST_PATHS=1. Is the refusal what any
 *     AppContainer does, or something the console adds? The matrix below asks the layer's exact call on the kind of
 *     file the layer asks about, plus every variant and every alternative that could replace it if it is refused:
 *     the volume-relative names (no mount manager involved), the NT device name, QueryDosDevice.
 *   - dbghelp.dll could not be bound on the console, so there was no crash.dmp. Here: LoadLibrary with the error code,
 *     the export, and a real MiniDumpWriteDump of this very process.
 *   - Dynamic code: the layer maps and patches code, so VirtualAlloc(PAGE_EXECUTE_READWRITE) must work.
 *
 * It also proves the sandbox is enforced rather than assumed: the token (TokenIsAppContainer, the container SID, the
 * integrity level, the capabilities), and NEGATIVE CONTROLS, things the same user may do as an ordinary process that the
 * container must not (read a file nobody granted it, write to C:\Windows\Temp).
 *
 * Output: one line per probe, "PROBE <id> | <OK|FAIL|INFO> | <detail>". FAIL is data, not an error: the exit status is
 * always 0. The ids the workflow's report reads are documented there; keep them stable.
 *
 * Plain C, static CRT, imports only kernel32 and advapi32, so that the loader of a container never has a reason to fail
 * before the first probe. Build (CI):  cl /nologo /O2 /MT /std:c11 /W3 /utf-8 ac_probe.c /link advapi32.lib
 * Build (check): x86_64-w64-mingw32-clang -std=c11 -municode -static ac_probe.c -o ac_probe.exe -ladvapi32
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <sddl.h>
#include <dbghelp.h> /* only for the MiniDump types: dbghelp.dll itself is loaded at run time, which is the point */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#pragma warning(disable : 4996 4191)
#endif

/* ------------------------------------------------------------------------------------------------------------------ */
/* Output                                                                                                              */
/* ------------------------------------------------------------------------------------------------------------------ */

static HANDLE g_out;
static int g_ok, g_fail;

static void Probe(const char *id, const char *status, const char *fmt, ...)
{
    char line[2600];
    DWORD written;
    int n, m;
    va_list ap;

    n = snprintf(line, sizeof line - 2, "PROBE %s | %s | ", id, status);
    if (n < 0)
        return;
    va_start(ap, fmt);
    m = vsnprintf(line + n, sizeof line - 2 - (size_t)n, fmt, ap);
    va_end(ap);
    if (m < 0)
        m = 0;
    if (n + m > (int)sizeof line - 3)
        m = (int)sizeof line - 3 - n;
    line[n + m] = '\n';
    if (g_out != NULL && g_out != INVALID_HANDLE_VALUE)
        WriteFile(g_out, line, (DWORD)(n + m + 1), &written, NULL);
    if (strcmp(status, "OK") == 0)
        g_ok++;
    else if (strcmp(status, "FAIL") == 0)
        g_fail++;
}

/* "error 5 (0x00000005) Access is denied" */
static const char *ErrText(DWORD e, char *out, size_t cap)
{
    char msg[220];
    DWORD n;

    msg[0] = '\0';
    n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg, sizeof msg, NULL);
    while (n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n' || msg[n - 1] == ' ' || msg[n - 1] == '.'))
        msg[--n] = '\0';
    snprintf(out, cap, "error %lu (0x%08lX)%s%s", (unsigned long)e, (unsigned long)e, n ? " " : "", msg);
    return out;
}

static const char *U8(const wchar_t *w, char *out, int cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    if (n <= 0)
        out[0] = '\0';
    return out;
}

/* A counted (not NUL-terminated) wide string, as the NT and FILE_NAME_INFO structures hold them. */
static const char *U8N(const wchar_t *w, size_t chars, char *out, int cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)chars, out, cap - 1, NULL, NULL);
    out[n > 0 ? n : 0] = '\0';
    return out;
}

static HANDLE OpenExisting(const wchar_t *path, DWORD access, int dir, DWORD *err)
{
    HANDLE h = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           dir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE && err != NULL)
        *err = GetLastError();
    return h;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Who am I                                                                                                            */
/* ------------------------------------------------------------------------------------------------------------------ */

static void ProbeOs(void)
{
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOEXW *);
    OSVERSIONINFOEXW vi;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn fn = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    DWORD session = 0;

    memset(&vi, 0, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    if (fn != NULL && fn(&vi) == 0)
        Probe("os.version", "INFO", "Windows %lu.%lu build %lu, product type %u (1 workstation, 2 domain controller, 3 server), suite 0x%04x",
              (unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion, (unsigned long)vi.dwBuildNumber,
              (unsigned)vi.wProductType, (unsigned)vi.wSuiteMask);
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    Probe("os.process", "INFO", "pid %lu, session %lu", (unsigned long)GetCurrentProcessId(), (unsigned long)session);
}

static const char *IntegrityName(DWORD rid)
{
    if (rid >= 0x5000)
        return "protected";
    if (rid >= 0x4000)
        return "System";
    if (rid >= 0x3000)
        return "High";
    if (rid >= 0x2000)
        return "Medium";
    if (rid >= 0x1000)
        return "Low";
    return "Untrusted";
}

/* GetTokenInformation into a LocalAlloc'd buffer of the right size. NULL on failure. */
static void *TokenInfo(HANDLE tok, TOKEN_INFORMATION_CLASS cls)
{
    DWORD need = 0;
    void *buf;

    GetTokenInformation(tok, cls, NULL, 0, &need);
    if (need == 0)
        return NULL;
    buf = LocalAlloc(LPTR, need + 8);
    if (buf != NULL && !GetTokenInformation(tok, cls, buf, need, &need)) {
        LocalFree(buf);
        return NULL;
    }
    return buf;
}

static void ProbeToken(void)
{
    HANDLE tok = NULL;
    DWORD v = 0, n = 0, i;
    char err[260];
    void *p;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        Probe("token.open", "FAIL", "OpenProcessToken: %s", ErrText(GetLastError(), err, sizeof err));
        return;
    }
    if (GetTokenInformation(tok, (TOKEN_INFORMATION_CLASS)TokenIsAppContainer, &v, sizeof v, &n))
        Probe("token.is_appcontainer", "INFO", "%lu", (unsigned long)v);
    else
        Probe("token.is_appcontainer", "INFO", "unknown: %s", ErrText(GetLastError(), err, sizeof err));
    v = 0;
    /* TokenIsLessPrivilegedAppContainer is 46: spelled as a number so that an older SDK still compiles it */
    if (GetTokenInformation(tok, (TOKEN_INFORMATION_CLASS)46, &v, sizeof v, &n))
        Probe("token.is_lpac", "INFO", "%lu", (unsigned long)v);
    else
        Probe("token.is_lpac", "INFO", "unknown: %s", ErrText(GetLastError(), err, sizeof err));

    p = TokenInfo(tok, TokenAppContainerSid);
    if (p != NULL) {
        PSID sid = ((TOKEN_APPCONTAINER_INFORMATION *)p)->TokenAppContainer;
        LPSTR s = NULL;
        if (sid != NULL && ConvertSidToStringSidA(sid, &s)) {
            Probe("token.appcontainer_sid", "INFO", "%s", s);
            LocalFree(s);
        } else {
            Probe("token.appcontainer_sid", "INFO", "(none: not an AppContainer token)");
        }
        LocalFree(p);
    }
    p = TokenInfo(tok, TokenIntegrityLevel);
    if (p != NULL) {
        PSID sid = ((TOKEN_MANDATORY_LABEL *)p)->Label.Sid;
        DWORD rid = *GetSidSubAuthority(sid, (DWORD)*GetSidSubAuthorityCount(sid) - 1);
        Probe("token.integrity", "INFO", "0x%04lX (%s)", (unsigned long)rid, IntegrityName(rid));
        LocalFree(p);
    }
    p = TokenInfo(tok, TokenCapabilities);
    if (p != NULL) {
        TOKEN_GROUPS *g = (TOKEN_GROUPS *)p;
        char list[1200];
        size_t used = 0;
        list[0] = '\0';
        for (i = 0; i < g->GroupCount; i++) {
            LPSTR s = NULL;
            if (ConvertSidToStringSidA(g->Groups[i].Sid, &s)) {
                used += (size_t)snprintf(list + used, sizeof list - used, "%s%s", used ? " " : "", s);
                LocalFree(s);
                if (used >= sizeof list - 64)
                    break;
            }
        }
        Probe("token.capabilities", "INFO", "%lu: %s", (unsigned long)g->GroupCount, list[0] ? list : "(none)");
        LocalFree(p);
    } else {
        Probe("token.capabilities", "INFO", "unknown: %s", ErrText(GetLastError(), err, sizeof err));
    }
    p = TokenInfo(tok, TokenPrivileges);
    if (p != NULL) {
        TOKEN_PRIVILEGES *tp = (TOKEN_PRIVILEGES *)p;
        char list[1200];
        size_t used = 0;
        list[0] = '\0';
        for (i = 0; i < tp->PrivilegeCount; i++) {
            char name[128];
            DWORD cch = sizeof name;
            if (!LookupPrivilegeNameA(NULL, &tp->Privileges[i].Luid, name, &cch))
                snprintf(name, sizeof name, "luid-%lu", (unsigned long)tp->Privileges[i].Luid.LowPart);
            used += (size_t)snprintf(list + used, sizeof list - used, "%s%s(%s)", used ? " " : "", name,
                                     (tp->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) ? "on" : "off");
            if (used >= sizeof list - 160)
                break;
        }
        Probe("token.privileges", "INFO", "%lu: %s", (unsigned long)tp->PrivilegeCount, list[0] ? list : "(none)");
        LocalFree(p);
    }
    CloseHandle(tok);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* The file system: the boundary, and what the layer needs                                                             */
/* ------------------------------------------------------------------------------------------------------------------ */

/* Create <dir>\<name>, write five bytes, close, delete. */
static void ProbeCreateIn(const char *id, const wchar_t *dir, int keep)
{
    wchar_t path[MAX_PATH * 2];
    char shown[600], err[260];
    HANDLE h;
    DWORD w = 0;

    _snwprintf(path, MAX_PATH * 2, L"%s\\ac-probe-%lu.tmp", dir, (unsigned long)GetCurrentProcessId());
    path[MAX_PATH * 2 - 1] = L'\0';
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Probe(id, "FAIL", "create %s: %s", U8(path, shown, sizeof shown), ErrText(GetLastError(), err, sizeof err));
        return;
    }
    if (!WriteFile(h, "probe", 5, &w, NULL))
        Probe(id, "FAIL", "created %s but WriteFile failed: %s", U8(path, shown, sizeof shown), ErrText(GetLastError(), err, sizeof err));
    else
        Probe(id, "OK", "created and wrote %s", U8(path, shown, sizeof shown));
    CloseHandle(h);
    if (!keep)
        DeleteFileW(path);
}

static void ProbeFs(const wchar_t *work, const wchar_t *machine, const wchar_t *negative)
{
    char shown[600], err[260];
    wchar_t sysdir[MAX_PATH], tmp[MAX_PATH * 2];
    HANDLE h;
    DWORD e = 0;

    /* The write the layer needs most: its registry overlay is saved into the machine folder at shutdown, and the guest's
       C:\ is the machine folder (on the console it lives in LocalState, which the package may write). */
    ProbeCreateIn("fs.write_workdir", work, 0);
    if (machine != NULL) {
        _snwprintf(tmp, MAX_PATH * 2, L"%s\\machine.json", machine);
        tmp[MAX_PATH * 2 - 1] = L'\0';
        h = OpenExisting(tmp, GENERIC_READ, 0, &e);
        if (h != INVALID_HANDLE_VALUE) {
            Probe("fs.read_machine", "OK", "read %s", U8(tmp, shown, sizeof shown));
            CloseHandle(h);
        } else {
            Probe("fs.read_machine", "FAIL", "open %s: %s", U8(tmp, shown, sizeof shown), ErrText(e, err, sizeof err));
        }
        ProbeCreateIn("fs.write_machine", machine, 0);
    }

    /* NEGATIVE CONTROLS. An ordinary process of this user may do both; a container that was not granted them must not.
       "OK" here, inside a container, would mean the boundary is not what we think it is. */
    if (negative != NULL) {
        h = OpenExisting(negative, GENERIC_READ, 0, &e);
        if (h != INVALID_HANDLE_VALUE) {
            Probe("ctl.read_ungranted", "OK", "read %s (an ordinary process may; a container with no ACE for it must not)",
                  U8(negative, shown, sizeof shown));
            CloseHandle(h);
        } else {
            Probe("ctl.read_ungranted", "FAIL", "open %s: %s (the expected answer inside a container)", U8(negative, shown, sizeof shown),
                  ErrText(e, err, sizeof err));
        }
    }
    if (GetWindowsDirectoryW(sysdir, MAX_PATH) != 0) {
        _snwprintf(tmp, MAX_PATH * 2, L"%s\\Temp", sysdir);
        tmp[MAX_PATH * 2 - 1] = L'\0';
        ProbeCreateIn("ctl.write_windows_temp", tmp, 0);
    }
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* GetFinalPathNameByHandleW and everything that could replace it                                                      */
/* ------------------------------------------------------------------------------------------------------------------ */

typedef LONG(NTAPI *PFN_NtQueryObject)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef LONG(NTAPI *PFN_NtQueryInformationFile)(HANDLE, PVOID, PVOID, ULONG, ULONG);

typedef struct {
    const char *id;
    DWORD flags;
} FlagCase;

static const FlagCase kCases[] = {
    {"dos_normalized", FILE_NAME_NORMALIZED | VOLUME_NAME_DOS}, /* THE LAYER'S CALL: HostPathOfHandle, core/dispatch/nt_file.cpp */
    {"dos_opened", FILE_NAME_OPENED | VOLUME_NAME_DOS},
    {"nt_normalized", FILE_NAME_NORMALIZED | VOLUME_NAME_NT},
    {"guid_normalized", FILE_NAME_NORMALIZED | VOLUME_NAME_GUID},
    {"none_normalized", FILE_NAME_NORMALIZED | VOLUME_NAME_NONE},
};

static void Alternatives(const char *tag, HANDLE h)
{
    char id[160], shown[1300], err[260];
    union {
        BYTE raw[2300];
        FILE_NAME_INFO fni;
        struct {
            ULONG len;
            WCHAR name[1];
        } nti;
        struct {
            USHORT length, maximum;
            PWSTR buffer;
        } ust;
    } u;
    struct {
        PVOID status;
        ULONG_PTR information;
    } iosb;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    PFN_NtQueryObject query_object = nt ? (PFN_NtQueryObject)(void *)GetProcAddress(nt, "NtQueryObject") : NULL;
    PFN_NtQueryInformationFile query_file = nt ? (PFN_NtQueryInformationFile)(void *)GetProcAddress(nt, "NtQueryInformationFile") : NULL;

    /* FileNormalizedNameInfo: the volume-relative name. No mount manager is involved. */
    memset(&u, 0, sizeof u);
    snprintf(id, sizeof id, "alt.%s.fileinfo_normalized", tag);
    if (GetFileInformationByHandleEx(h, FileNormalizedNameInfo, &u, sizeof u))
        Probe(id, "OK", "%s", U8N(u.fni.FileName, u.fni.FileNameLength / sizeof(WCHAR), shown, sizeof shown));
    else
        Probe(id, "FAIL", "%s", ErrText(GetLastError(), err, sizeof err));

    /* NtQueryInformationFile(FileNameInformation): what Xl_NtQueryInformationFile relays. */
    memset(&u, 0, sizeof u);
    snprintf(id, sizeof id, "alt.%s.ntqueryinfo_filename", tag);
    if (query_file != NULL) {
        LONG st = query_file(h, &iosb, &u, sizeof u, 9 /* FileNameInformation */);
        if (st >= 0)
            Probe(id, "OK", "%s", U8N(u.nti.name, u.nti.len / sizeof(WCHAR), shown, sizeof shown));
        else
            Probe(id, "FAIL", "NTSTATUS 0x%08lX", (unsigned long)st);
    }

    /* NtQueryObject(ObjectNameInformation): the NT device path, \Device\HarddiskVolumeN\... */
    memset(&u, 0, sizeof u);
    snprintf(id, sizeof id, "alt.%s.ntqueryobject_name", tag);
    if (query_object != NULL) {
        ULONG ret = 0;
        LONG st = query_object(h, 1 /* ObjectNameInformation */, &u, sizeof u, &ret);
        if (st >= 0 && u.ust.buffer != NULL)
            Probe(id, "OK", "%s", U8N(u.ust.buffer, u.ust.length / sizeof(WCHAR), shown, sizeof shown));
        else
            Probe(id, "FAIL", "NTSTATUS 0x%08lX", (unsigned long)st);
    }
}

static void GfpnbhMatrix(const char *tag, const wchar_t *path, int dir, DWORD access)
{
    char id[160], shown[1300], err[260], pshown[600];
    wchar_t buf[1024];
    HANDLE h;
    DWORD e = 0, n;
    size_t i;

    h = OpenExisting(path, access, dir, &e);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(id, sizeof id, "gfpnbh.%s.open", tag);
        Probe(id, "FAIL", "CreateFileW(%s, access 0x%lX): %s", U8(path, pshown, sizeof pshown), (unsigned long)access,
              ErrText(e, err, sizeof err));
        return;
    }
    for (i = 0; i < sizeof kCases / sizeof kCases[0]; i++) {
        snprintf(id, sizeof id, "gfpnbh.%s.%s", tag, kCases[i].id);
        SetLastError(0);
        n = GetFinalPathNameByHandleW(h, buf, 1024, kCases[i].flags);
        if (n == 0)
            Probe(id, "FAIL", "%s", ErrText(GetLastError(), err, sizeof err));
        else if (n >= 1024)
            Probe(id, "FAIL", "buffer too small, %lu chars needed", (unsigned long)n);
        else
            Probe(id, "OK", "%s", U8N(buf, n, shown, sizeof shown));
    }
    Alternatives(tag, h);
    CloseHandle(h);
}

static void ProbeVolumes(const wchar_t *sample_path)
{
    static wchar_t done[32];
    wchar_t name[3], target[512];
    char shown[700], err[260], id[64];
    DWORD n;
    size_t i;

    if (sample_path == NULL || sample_path[0] == L'\0' || sample_path[1] != L':')
        return;
    for (i = 0; done[i] != L'\0'; i++)
        if (done[i] == sample_path[0])
            return; /* one answer per drive letter */
    if (i < 30)
        done[i] = sample_path[0];
    name[0] = sample_path[0];
    name[1] = L':';
    name[2] = L'\0';
    snprintf(id, sizeof id, "alt.querydosdevice.%c", (char)sample_path[0]);
    n = QueryDosDeviceW(name, target, 512);
    if (n != 0)
        Probe(id, "OK", "%c: -> %s", (char)sample_path[0], U8(target, shown, sizeof shown));
    else
        Probe(id, "FAIL", "%s", ErrText(GetLastError(), err, sizeof err));
}

/* The mechanism behind VOLUME_NAME_DOS is the mount manager. If this device cannot be opened, that is the reason. */
static void ProbeMountManager(void)
{
    char err[260];
    HANDLE h;

    h = CreateFileW(L"\\\\.\\MountPointManager", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        Probe("mountmgr.open_noaccess", "OK", "opened \\\\.\\MountPointManager with no access rights");
        CloseHandle(h);
    } else {
        Probe("mountmgr.open_noaccess", "FAIL", "\\\\.\\MountPointManager: %s", ErrText(GetLastError(), err, sizeof err));
    }
    h = CreateFileW(L"\\\\.\\MountPointManager", FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        Probe("mountmgr.open_readattr", "OK", "opened \\\\.\\MountPointManager with FILE_READ_ATTRIBUTES|SYNCHRONIZE");
        CloseHandle(h);
    } else {
        Probe("mountmgr.open_readattr", "FAIL", "\\\\.\\MountPointManager: %s", ErrText(GetLastError(), err, sizeof err));
    }
}

static void ProbeGfpnbh(const wchar_t *work, const wchar_t *machine)
{
    wchar_t exe[MAX_PATH * 2], sysdir[MAX_PATH], path[MAX_PATH * 2];
    HANDLE h;
    DWORD w = 0;

    /* a file in the working directory, created here: the one file every context is expected to be able to open */
    _snwprintf(path, MAX_PATH * 2, L"%s\\probe-target.txt", work);
    path[MAX_PATH * 2 - 1] = L'\0';
    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, "probe", 5, &w, NULL);
        CloseHandle(h);
    }
    ProbeMountManager();
    GfpnbhMatrix("work_file", path, 0, GENERIC_READ);
    GfpnbhMatrix("work_dir", work, 1, GENERIC_READ);

    if (GetModuleFileNameW(NULL, exe, MAX_PATH * 2) != 0)
        GfpnbhMatrix("exe", exe, 0, GENERIC_READ);
    if (GetSystemDirectoryW(sysdir, MAX_PATH) != 0) {
        _snwprintf(path, MAX_PATH * 2, L"%s\\ntdll.dll", sysdir);
        path[MAX_PATH * 2 - 1] = L'\0';
        GfpnbhMatrix("system_ntdll", path, 0, GENERIC_READ);
    }
    if (machine != NULL) {
        /* THE kind of file the layer asks about: a Wine builtin in the machine folder, opened for reading. */
        _snwprintf(path, MAX_PATH * 2, L"%s\\Windows\\builtin\\ntdll.dll", machine);
        path[MAX_PATH * 2 - 1] = L'\0';
        GfpnbhMatrix("machine_ntdll", path, 0, GENERIC_READ);
        /* the same file, opened with attribute access only: separates "the handle lacks a right" from "the context is refused" */
        GfpnbhMatrix("machine_ntdll_attr", path, 0, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
        GfpnbhMatrix("machine_dir", machine, 1, GENERIC_READ);
        ProbeVolumes(machine);
    }
    ProbeVolumes(work);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* dbghelp                                                                                                             */
/* ------------------------------------------------------------------------------------------------------------------ */

typedef BOOL(WINAPI *PFN_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                            PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

typedef struct {
    PFN_MiniDumpWriteDump fn;
    HANDLE file;
    BOOL ok;
    DWORD err;
} DumpJob;

/* On a thread of its own: dumping the calling thread's process from the calling thread is the case the documentation
   warns about, and it is also how host-win32's crash capture does it (a dedicated crash thread). */
static DWORD WINAPI DumpThread(LPVOID p)
{
    DumpJob *j = (DumpJob *)p;
    SetLastError(0);
    j->ok = j->fn(GetCurrentProcess(), GetCurrentProcessId(), j->file, MiniDumpNormal, NULL, NULL, NULL);
    j->err = j->ok ? 0 : GetLastError();
    return 0;
}

static void ProbeDbghelp(const wchar_t *work)
{
    char shown[700], err[260];
    wchar_t path[MAX_PATH * 2], where[MAX_PATH * 2];
    HMODULE dbg, core;
    PFN_MiniDumpWriteDump fn;
    HANDLE f, t;
    DumpJob job;
    DWORD tid, size_lo;

    SetLastError(0);
    dbg = LoadLibraryW(L"dbghelp.dll");
    if (dbg == NULL) {
        /* this is the failure the console reported: crash.cpp's BindDbghelp() -> HRESULT 0x800736B2 */
        Probe("dbghelp.loadlibrary", "FAIL", "LoadLibraryW(dbghelp.dll): %s", ErrText(GetLastError(), err, sizeof err));
    } else {
        where[0] = L'\0';
        GetModuleFileNameW(dbg, where, MAX_PATH * 2);
        Probe("dbghelp.loadlibrary", "OK", "%s", U8(where, shown, sizeof shown));
    }
    SetLastError(0);
    core = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (core == NULL)
        Probe("dbghelp.loadlibrary_system32", "FAIL", "LoadLibraryExW(dbghelp.dll, SEARCH_SYSTEM32): %s", ErrText(GetLastError(), err, sizeof err));
    else
        Probe("dbghelp.loadlibrary_system32", "OK", "loaded from the system directory");
    SetLastError(0);
    core = LoadLibraryW(L"dbgcore.dll");
    if (core == NULL)
        Probe("dbghelp.dbgcore", "FAIL", "LoadLibraryW(dbgcore.dll): %s", ErrText(GetLastError(), err, sizeof err));
    else
        Probe("dbghelp.dbgcore", "OK", "dbgcore.dll (the real MiniDumpWriteDump since Windows 10) loaded");
    if (dbg == NULL)
        return;

    fn = (PFN_MiniDumpWriteDump)(void *)GetProcAddress(dbg, "MiniDumpWriteDump");
    if (fn == NULL) {
        Probe("dbghelp.export", "FAIL", "no MiniDumpWriteDump export: %s", ErrText(GetLastError(), err, sizeof err));
        return;
    }
    Probe("dbghelp.export", "OK", "MiniDumpWriteDump resolved");

    _snwprintf(path, MAX_PATH * 2, L"%s\\ac-probe-self.dmp", work);
    path[MAX_PATH * 2 - 1] = L'\0';
    f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        Probe("dbghelp.minidump_self", "FAIL", "cannot create %s: %s", U8(path, shown, sizeof shown), ErrText(GetLastError(), err, sizeof err));
        return;
    }
    memset(&job, 0, sizeof job);
    job.fn = fn;
    job.file = f;
    t = CreateThread(NULL, 0, DumpThread, &job, 0, &tid);
    if (t == NULL) {
        Probe("dbghelp.minidump_self", "FAIL", "CreateThread: %s", ErrText(GetLastError(), err, sizeof err));
    } else if (WaitForSingleObject(t, 60000) != WAIT_OBJECT_0) {
        Probe("dbghelp.minidump_self", "FAIL", "MiniDumpWriteDump did not return within 60 s");
    } else {
        size_lo = GetFileSize(f, NULL);
        if (job.ok && size_lo != INVALID_FILE_SIZE && size_lo > 0)
            Probe("dbghelp.minidump_self", "OK", "MiniDumpWriteDump(this process) wrote %lu bytes to %s", (unsigned long)size_lo,
                  U8(path, shown, sizeof shown));
        else
            Probe("dbghelp.minidump_self", "FAIL", "MiniDumpWriteDump returned %d, %lu bytes: %s", (int)job.ok, (unsigned long)(size_lo == INVALID_FILE_SIZE ? 0 : size_lo),
                  ErrText(job.err, err, sizeof err));
        CloseHandle(t);
    }
    CloseHandle(f);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Dynamic code                                                                                                        */
/* ------------------------------------------------------------------------------------------------------------------ */

static void ProbeCodegen(void)
{
    typedef void (*Fn)(void);
    char err[260];
    unsigned char *p;
    DWORD old = 0;

    p = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (p == NULL) {
        Probe("codegen.rwx", "FAIL", "VirtualAlloc(PAGE_EXECUTE_READWRITE): %s (ERROR_DYNAMIC_CODE_BLOCKED is 1655)", ErrText(GetLastError(), err, sizeof err));
    } else {
        p[0] = 0xC3; /* ret */
        FlushInstructionCache(GetCurrentProcess(), p, 1);
        ((Fn)p)();
        Probe("codegen.rwx", "OK", "allocated RWX memory and executed a ret in it");
        VirtualFree(p, 0, MEM_RELEASE);
    }
    p = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p == NULL) {
        Probe("codegen.protect", "FAIL", "VirtualAlloc(PAGE_READWRITE): %s", ErrText(GetLastError(), err, sizeof err));
        return;
    }
    p[0] = 0xC3;
    if (!VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old)) {
        Probe("codegen.protect", "FAIL", "VirtualProtect(PAGE_EXECUTE_READ): %s", ErrText(GetLastError(), err, sizeof err));
    } else {
        FlushInstructionCache(GetCurrentProcess(), p, 1);
        ((Fn)p)();
        Probe("codegen.protect", "OK", "RW page switched to RX and executed");
    }
    VirtualFree(p, 0, MEM_RELEASE);
}

/* ------------------------------------------------------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *work = argc > 1 ? argv[1] : L".";
    const wchar_t *machine = argc > 2 ? argv[2] : NULL;
    const wchar_t *negative = argc > 3 ? argv[3] : NULL;
    char a[600], b[600], c[600];

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    Probe("probe", "INFO", "ac_probe started: workdir=%s machine=%s negative-control=%s", U8(work, a, sizeof a),
          machine ? U8(machine, b, sizeof b) : "(none)", negative ? U8(negative, c, sizeof c) : "(none)");
    ProbeOs();
    ProbeToken();
    ProbeFs(work, machine, negative);
    ProbeGfpnbh(work, machine);
    ProbeDbghelp(work);
    ProbeCodegen();
    Probe("summary", "INFO", "%d ok, %d fail (FAIL is a measurement, not an error; the negative controls ctl.* are expected to FAIL inside a container)",
          g_ok, g_fail);
    return 0;
}
