/* guest_lockfile: LockFileEx / UnlockFileEx / LockFile / UnlockFile on two handles of one file. Found by running the
 * SQLite shell on real Windows: its VFS locks the database with LockFileEx, and every lock failed with
 * STATUS_ACCESS_VIOLATION (reported as "database is locked") because Wine's kernelbase calls NtLockFile with a NULL
 * IoStatusBlock, which Wine's ntdll takes and Windows' does not. Exit code 42 when everything holds, 1 otherwise. */
#include <stdio.h>
#include <windows.h>

static int g_bad;

static void expect(const char* what, BOOL got, BOOL want, DWORD want_error) {
    DWORD err = got ? 0 : GetLastError();
    int ok = got == want && (want || !want_error || err == want_error);   /* want_error 0: any failure */
    printf("lockfile: %s -> %d error=%lu%s\n", what, got ? 1 : 0, (unsigned long)err, ok ? "" : "  <-- WRONG");
    if (!ok) g_bad = 1;
}

static BOOL lock(HANDLE h, DWORD flags, DWORD off, DWORD len) {
    OVERLAPPED ov = {0};
    ov.Offset = off;
    return LockFileEx(h, flags, 0, len, 0, &ov);
}

static BOOL unlock(HANDLE h, DWORD off, DWORD len) {
    OVERLAPPED ov = {0};
    ov.Offset = off;
    return UnlockFileEx(h, 0, len, 0, &ov);
}

int main(void) {
    const wchar_t* path = L"C:\\Users\\User\\lockfile.tmp";
    const DWORD fast_x = LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, fast_s = LOCKFILE_FAIL_IMMEDIATELY;
    char block[4096] = {0};
    DWORD n;
    HANDLE a = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE b = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (a == INVALID_HANDLE_VALUE || b == INVALID_HANDLE_VALUE) {
        printf("lockfile: cannot open %ls (error %lu)\n", path, (unsigned long)GetLastError());
        return 1;
    }
    WriteFile(a, block, sizeof block, &n, NULL);

    expect("exclusive lock 10+100 on first handle", lock(a, fast_x, 10, 100), TRUE, 0);
    expect("same range on second handle", lock(b, fast_x, 50, 10), FALSE, ERROR_LOCK_VIOLATION);
    expect("disjoint range on second handle", lock(b, fast_x, 500, 10), TRUE, 0);
    expect("unlock 10+100 on first handle", unlock(a, 10, 100), TRUE, 0);
    expect("former range on second handle", lock(b, fast_x, 50, 10), TRUE, 0);
    expect("unlock 50+10 on second handle", unlock(b, 50, 10), TRUE, 0);
    expect("unlock 500+10 on second handle", unlock(b, 500, 10), TRUE, 0);
    expect("unlock of a range not locked", unlock(b, 500, 10), FALSE, 0);   /* ERROR_NOT_LOCKED on Windows, 33 on Wine */

    expect("shared lock on first handle", lock(a, fast_s, 1000, 20), TRUE, 0);
    expect("shared lock, same range, second handle", lock(b, fast_s, 1000, 20), TRUE, 0);
    expect("exclusive over the shared range", lock(b, fast_x, 1010, 5), FALSE, ERROR_LOCK_VIOLATION);
    expect("unlock shared, first", unlock(a, 1000, 20), TRUE, 0);
    expect("unlock shared, second", unlock(b, 1000, 20), TRUE, 0);

    expect("LockFile 2000+8", LockFile(a, 2000, 0, 8, 0), TRUE, 0);
    expect("LockFile same, second handle", LockFile(b, 2000, 0, 8, 0), FALSE, ERROR_LOCK_VIOLATION);
    expect("UnlockFile 2000+8", UnlockFile(a, 2000, 0, 8, 0), TRUE, 0);

    CloseHandle(a);
    CloseHandle(b);
    DeleteFileW(path);
    printf("lockfile: done\n");
    fflush(stdout);
    return g_bad ? 1 : 42;
}
