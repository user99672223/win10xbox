/* guest_regpersist: registry writes outlive the process that made them (docs/DISPATCHER.md "Registry persistence").
 *
 * No argument ("write"): writes HKCU\Software\win10xbox\persist with a value of each common type (REG_SZ,
 * REG_EXPAND_SZ, REG_MULTI_SZ, REG_DWORD, REG_QWORD, REG_BINARY, the default value, a subkey), RegFlushKey, then one
 * more value ("prespawn") that is NOT flushed, and runs itself as `child`: the child reads "sz" (flushed) and
 * "prespawn" (saved by the layer before the child started) and exits 7. Prints `regpersist: write ok`, and
 * `regpersist: child ok` when the child saw both. A host that cannot start processes (the UWP host answers
 * ERROR_NOT_SUPPORTED) prints nothing for the child. Exits 42 when everything it ran behaved.
 *
 * `read` (a second run, a new host process after the first one exited): reads every value back from the registry and
 * prints `regpersist: <name> ok` for each that has the type and bytes the write run stored. Exits 42 when all match.
 *
 * `child`: the process the write run starts; exits 7 (see above). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define KEY_PATH L"Software\\win10xbox\\persist"

static const wchar_t k_sz[] = L"persisted text";
static const wchar_t k_expand[] = L"%SystemRoot%\\persist";
static const wchar_t k_multi[] = L"one\0two\0three\0";   /* + the terminating NUL of the literal */
static const DWORD k_dword = 0xC0FFEE42u;
static const ULONGLONG k_qword = 0x0123456789ABCDEFull;
static const unsigned char k_binary[] = {0x00, 0x01, 0xFE, 0xFF, 0x10, 0x20, 0x00, 0x7F};
static const wchar_t k_default[] = L"the default value";
static const wchar_t k_sub[] = L"subkey value";
static const wchar_t k_prespawn[] = L"written after the flush";

static int set_value(HKEY k, const wchar_t* name, DWORD type, const void* data, DWORD size) {
    LONG e = RegSetValueExW(k, name, 0, type, (const BYTE*)data, size);
    if (e != ERROR_SUCCESS) printf("regpersist: RegSetValueExW(%ls) error %ld\n", name ? name : L"(default)", e);
    return e == ERROR_SUCCESS;
}

/* The value `name` of `k` has exactly this type and these bytes. */
static int check_value(HKEY k, const wchar_t* label, const wchar_t* name, DWORD type, const void* data, DWORD size) {
    BYTE buf[256];
    DWORD got_type = 0, got_size = sizeof buf;
    LONG e = RegQueryValueExW(k, name, NULL, &got_type, buf, &got_size);
    int good = e == ERROR_SUCCESS && got_type == type && got_size == size && memcmp(buf, data, size) == 0;
    if (good) printf("regpersist: %ls ok\n", label);
    else printf("regpersist: %ls FAILED (error %ld, type %lu size %lu)\n", label, e, got_type, got_size);
    return good;
}

static int read_main(int quiet_child) {
    HKEY k = NULL, sub = NULL;
    int good = 1;
    LONG e = RegOpenKeyExW(HKEY_CURRENT_USER, KEY_PATH, 0, KEY_READ, &k);
    if (e != ERROR_SUCCESS) {
        printf("regpersist: %s: RegOpenKeyExW error %ld\n", quiet_child ? "child" : "read", e);
        return 0;
    }
    if (quiet_child) {
        good = check_value(k, L"child sz", L"sz", REG_SZ, k_sz, sizeof k_sz);
        good = check_value(k, L"child prespawn", L"prespawn", REG_SZ, k_prespawn, sizeof k_prespawn) && good;
        RegCloseKey(k);
        return good;
    }
    good = check_value(k, L"sz", L"sz", REG_SZ, k_sz, sizeof k_sz) && good;
    good = check_value(k, L"expand", L"expand", REG_EXPAND_SZ, k_expand, sizeof k_expand) && good;
    good = check_value(k, L"multisz", L"multisz", REG_MULTI_SZ, k_multi, sizeof k_multi) && good;
    good = check_value(k, L"dword", L"dword", REG_DWORD, &k_dword, sizeof k_dword) && good;
    good = check_value(k, L"qword", L"qword", REG_QWORD, &k_qword, sizeof k_qword) && good;
    good = check_value(k, L"binary", L"binary", REG_BINARY, k_binary, sizeof k_binary) && good;
    good = check_value(k, L"default", NULL, REG_SZ, k_default, sizeof k_default) && good;
    good = check_value(k, L"prespawn", L"prespawn", REG_SZ, k_prespawn, sizeof k_prespawn) && good;
    e = RegOpenKeyExW(k, L"sub", 0, KEY_READ, &sub);
    if (e == ERROR_SUCCESS) {
        good = check_value(sub, L"subkey", L"leaf", REG_SZ, k_sub, sizeof k_sub) && good;
        RegCloseKey(sub);
    } else {
        printf("regpersist: subkey FAILED (error %ld)\n", e);
        good = 0;
    }
    RegCloseKey(k);
    return good;
}

/* Runs this program as `child`; 1 = ran and exited 7, 0 = failed, -1 = this host cannot start processes. */
static int run_child(void) {
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 16];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return 0;
    wcscpy(cmd, L"\"");
    wcscat(cmd, exe);
    wcscat(cmd, L"\" child");
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    fflush(stdout);   /* the child writes to the same pipe: keep the order */
    if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        DWORD err = GetLastError();
        if (err == ERROR_NOT_SUPPORTED) return -1;
        printf("regpersist: CreateProcessW failed, error %lu\n", err);
        return 0;
    }
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0 || !GetExitCodeProcess(pi.hProcess, &code)) {
        printf("regpersist: waiting for the child failed\n");
        TerminateProcess(pi.hProcess, 1);
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 7;
}

static int write_main(void) {
    HKEY k = NULL, sub = NULL;
    DWORD disp = 0;
    int good = 1, child;
    LONG e;
    e = RegCreateKeyExW(HKEY_CURRENT_USER, KEY_PATH, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, &disp);
    if (e != ERROR_SUCCESS) {
        printf("regpersist: RegCreateKeyExW error %ld\n", e);
        return 0;
    }
    good = set_value(k, L"sz", REG_SZ, k_sz, sizeof k_sz) && good;
    good = set_value(k, L"expand", REG_EXPAND_SZ, k_expand, sizeof k_expand) && good;
    good = set_value(k, L"multisz", REG_MULTI_SZ, k_multi, sizeof k_multi) && good;
    good = set_value(k, L"dword", REG_DWORD, &k_dword, sizeof k_dword) && good;
    good = set_value(k, L"qword", REG_QWORD, &k_qword, sizeof k_qword) && good;
    good = set_value(k, L"binary", REG_BINARY, k_binary, sizeof k_binary) && good;
    good = set_value(k, NULL, REG_SZ, k_default, sizeof k_default) && good;
    e = RegCreateKeyExW(k, L"sub", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &sub, &disp);
    if (e == ERROR_SUCCESS) {
        good = set_value(sub, L"leaf", REG_SZ, k_sub, sizeof k_sub) && good;
        RegCloseKey(sub);
    } else {
        printf("regpersist: RegCreateKeyExW(sub) error %ld\n", e);
        good = 0;
    }
    e = RegFlushKey(k);
    if (e != ERROR_SUCCESS) {
        printf("regpersist: RegFlushKey error %ld\n", e);
        good = 0;
    }
    good = set_value(k, L"prespawn", REG_SZ, k_prespawn, sizeof k_prespawn) && good;   /* not flushed */
    RegCloseKey(k);
    if (good) printf("regpersist: write ok\n");
    else printf("regpersist: write FAILED\n");
    child = run_child();
    if (child == 1) printf("regpersist: child ok\n");
    else if (child == 0) {
        printf("regpersist: child FAILED\n");
        good = 0;
    }
    return good;
}

int main(int argc, char** argv) {
    int good;
    if (argc > 1 && strcmp(argv[1], "child") == 0) {
        good = read_main(1);
        fflush(stdout);
        return good ? 7 : 1;
    }
    good = argc > 1 && strcmp(argv[1], "read") == 0 ? read_main(0) : write_main();
    printf("regpersist: %s\n", good ? "done" : "FAILED");
    fflush(stdout);
    return good ? 42 : 1;
}
