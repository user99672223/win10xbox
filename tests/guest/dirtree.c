/* guest_dirtree: a program that is installed as a directory tree (docs/slices/dirlaunch.md).
 *
 * Layout (tests/guest/dirtree/, staged as C:\guest\dirtree\):
 *   bin\guest_dirtree.exe     this program, NOT at the top of C:\guest
 *   data\file.pak             the file it reads, "PAKDIR dirtree payload"
 *   dirtree.launch            exe=bin\guest_dirtree.exe  cwd=data
 * The working directory is C:\guest\dirtree\data (the launcher's cwd=, or --cwd of the console host). It checks what a
 * game that loads its data relative to itself needs:
 *   1. GetModuleFileNameW names the program as C:\guest\dirtree\bin\guest_dirtree.exe
 *   2. GetCurrentDirectoryW is C:\guest\dirtree\data
 *   3. a relative open ("file.pak", "..\\dirtree.launch"), an open relative to the program's own directory
 *      ("..\\data\\file.pak") and an absolute open all find the files
 *   4. the same names in other letter case find them too (NTFS semantics: the files are case-preserving, not -sensitive)
 *   5. a path longer than MAX_PATH (a \\?\ path, 300+ characters) can be created, written, read, listed and removed
 * Exit 42 when everything held, 1 otherwise; every line starts with "dirtree:". */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int failures;

static void check(int ok, const char* what, const wchar_t* detail) {
    printf("dirtree: %s: %s", ok ? "ok" : "FAIL", what);
    if (detail) printf(" [%ls]", detail);
    printf("\n");
    if (!ok) failures++;
}

static void trim_slash(wchar_t* s) {
    size_t n = wcslen(s);
    while (n > 3 && s[n - 1] == L'\\') s[--n] = 0;
}

/* Opens `path`, reads up to 64 bytes and returns whether it starts with prefix. */
static int file_starts_with(const wchar_t* path, const char* prefix) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("dirtree: open %ls failed, error %lu\n", path, GetLastError());
        return 0;
    }
    char buf[65];
    DWORD got = 0;
    BOOL r = ReadFile(h, buf, 64, &got, NULL);
    CloseHandle(h);
    if (!r) return 0;
    buf[got] = 0;
    return strncmp(buf, prefix, strlen(prefix)) == 0;
}

static int exists(const wchar_t* path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static void opens(const wchar_t* path, const char* prefix, const char* what) {
    int ok = file_starts_with(path, prefix);
    check(ok, what, path);
}

int main(void) {
    wchar_t mod[MAX_PATH], cwd[MAX_PATH], bin[MAX_PATH], root[MAX_PATH], p[1024];
    DWORD n;

    n = GetModuleFileNameW(NULL, mod, MAX_PATH);
    check(n > 0 && n < MAX_PATH && _wcsicmp(mod, L"C:\\guest\\dirtree\\bin\\guest_dirtree.exe") == 0,
          "GetModuleFileNameW is C:\\guest\\dirtree\\bin\\guest_dirtree.exe", mod);

    n = GetCurrentDirectoryW(MAX_PATH, cwd);
    trim_slash(cwd);
    check(n > 0 && n < MAX_PATH && _wcsicmp(cwd, L"C:\\guest\\dirtree\\data") == 0,
          "GetCurrentDirectoryW is C:\\guest\\dirtree\\data", cwd);

    /* The program's own directory and the install root, derived from the module path as a game does. */
    wcscpy(bin, mod);
    wchar_t* sl = wcsrchr(bin, L'\\');
    if (sl) *sl = 0;
    wcscpy(root, bin);
    sl = wcsrchr(root, L'\\');
    if (sl) *sl = 0;

    opens(L"file.pak", "PAKDIR", "relative open file.pak (cwd)");
    opens(L"..\\dirtree.launch", "# A directory install", "relative open ..\\dirtree.launch (parent of cwd)");
    opens(L".\\file.pak", "PAKDIR", "relative open .\\file.pak");
    swprintf(p, 1024, L"%ls\\..\\data\\file.pak", bin);
    opens(p, "PAKDIR", "open <exe dir>\\..\\data\\file.pak");
    swprintf(p, 1024, L"%ls\\data\\file.pak", root);
    opens(p, "PAKDIR", "open <install root>\\data\\file.pak");
    opens(L"C:\\guest\\dirtree\\data\\file.pak", "PAKDIR", "absolute open");

    /* Letter case. */
    opens(L"FILE.PAK", "PAKDIR", "case: FILE.PAK");
    opens(L"File.Pak", "PAKDIR", "case: File.Pak");
    opens(L"..\\DATA\\file.pak", "PAKDIR", "case: ..\\DATA\\file.pak");
    opens(L"C:\\GUEST\\DIRTREE\\DATA\\FILE.PAK", "PAKDIR", "case: absolute upper case");
    opens(L"c:\\Guest\\DirTree\\Data\\File.pak", "PAKDIR", "case: absolute mixed case, lower drive");
    check(exists(L"..\\BIN\\GUEST_DIRTREE.EXE"), "case: GetFileAttributesW ..\\BIN\\GUEST_DIRTREE.EXE", NULL);
    check(!exists(L"nothere.pak"), "a missing file is missing", NULL);

    /* FindFirstFileW with a case-folded wildcard finds the file under its stored name. */
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(L"..\\DATA\\*.PAK", &fd);
        int ok = h != INVALID_HANDLE_VALUE && _wcsicmp(fd.cFileName, L"file.pak") == 0;
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
        check(ok, "case: FindFirstFileW ..\\DATA\\*.PAK lists file.pak", ok ? fd.cFileName : NULL);
    }

    /* A path over MAX_PATH. Components of 70 characters, 5 deep below C:\guest\dirtree\: about 380 characters. */
    {
        wchar_t dir[1024], file[1100], seg[80];
        int i, created = 0, ok;
        wchar_t* made[8];
        int nmade = 0;
        for (i = 0; i < 70; i++) seg[i] = (wchar_t)(L'a' + (i % 26));
        seg[70] = 0;
        swprintf(dir, 1024, L"\\\\?\\%ls\\longpath", root);
        if (CreateDirectoryW(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) created++;
        for (i = 0; i < 5; i++) {
            size_t len = wcslen(dir);
            made[nmade] = (wchar_t*)malloc((len + 1) * sizeof(wchar_t));
            wcscpy(made[nmade++], dir);
            swprintf(dir + len, 1024 - len, L"\\%ls%d", seg, i);
            if (CreateDirectoryW(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) created++;
            else printf("dirtree: CreateDirectoryW depth %d failed, error %lu\n", i, GetLastError());
        }
        swprintf(file, 1100, L"%ls\\deep.txt", dir);
        printf("dirtree: long path is %u characters\n", (unsigned)wcslen(file));
        check(created == 6 && wcslen(file) > 300, "long path: directories created", NULL);
        {
            HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            DWORD put = 0;
            ok = h != INVALID_HANDLE_VALUE && WriteFile(h, "DEEP\n", 5, &put, NULL) && put == 5;
            if (h == INVALID_HANDLE_VALUE) printf("dirtree: create long file failed, error %lu\n", GetLastError());
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            check(ok, "long path: file written", NULL);
        }
        check(file_starts_with(file, "DEEP"), "long path: file read back", NULL);
        {
            WIN32_FIND_DATAW fd;
            wchar_t pat[1100];
            swprintf(pat, 1100, L"%ls\\*", dir);
            HANDLE h = FindFirstFileW(pat, &fd);
            ok = 0;
            if (h != INVALID_HANDLE_VALUE) {
                do { if (_wcsicmp(fd.cFileName, L"deep.txt") == 0) ok = 1; } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            check(ok, "long path: directory listing holds deep.txt", NULL);
        }
        DeleteFileW(file);
        /* Remove deepest first. */
        RemoveDirectoryW(dir);
        for (i = nmade - 1; i >= 0; i--) {
            RemoveDirectoryW(made[i]);
            free(made[i]);
        }
        check(!exists(file), "long path: file removed", NULL);
    }

    printf("dirtree: %s\n", failures ? "FAILED" : "all checks passed");
    fflush(stdout);
    return failures ? 1 : 42;
}
