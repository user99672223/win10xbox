/* guest_spawn: CreateProcess through the layer (NtCreateUserProcess starts a real child).
 *
 * Without arguments it is the parent and runs two children:
 *   1. C:\guest\guest_hello.exe, which prints "hello from guest" on the inherited stdout and exits 42; the parent
 *      waits for it, reads the exit code and prints "spawn: child exit 42".
 *   2. itself, as "child", with a quoted command line, a working directory and an extra environment variable; the
 *      child prints what it sees ("spawn-child: ...") and exits 7, which the parent reports as "spawn: child2 exit 7".
 * The parent exits 42 when both children behaved, 1 otherwise. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int run_child(const wchar_t* app, wchar_t* cmdline, void* env, const wchar_t* cwd, DWORD* code) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    fflush(stdout);   /* the child writes to the same pipe: keep the order */
    if (!CreateProcessW(app, cmdline, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, env, cwd, &si, &pi)) {
        printf("spawn: CreateProcessW failed, error %lu\n", GetLastError());
        return 0;
    }
    if (pi.dwProcessId == 0 || pi.dwThreadId == 0)
        printf("spawn: no process or thread id\n");
    DWORD w = WaitForSingleObject(pi.hProcess, 60000);
    if (w != WAIT_OBJECT_0) {
        printf("spawn: wait returned %lu\n", w);
        TerminateProcess(pi.hProcess, 1);
        return 0;
    }
    if (!GetExitCodeProcess(pi.hProcess, code)) {
        printf("spawn: GetExitCodeProcess failed, error %lu\n", GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* The environment block of this process plus SPAWN_MARK=mark42. */
static wchar_t* env_with_mark(void) {
    wchar_t* cur = GetEnvironmentStringsW();
    size_t len = 0;
    while (cur[len]) len += wcslen(cur + len) + 1;
    static const wchar_t mark[] = L"SPAWN_MARK=mark42";
    wchar_t* out = (wchar_t*)malloc((len + sizeof(mark) / sizeof(wchar_t) + 1) * sizeof(wchar_t));
    memcpy(out, cur, len * sizeof(wchar_t));
    memcpy(out + len, mark, sizeof(mark));
    out[len + sizeof(mark) / sizeof(wchar_t)] = 0;
    FreeEnvironmentStringsW(cur);
    return out;
}

static int child_main(int argc, char** argv) {
    wchar_t cwd[MAX_PATH], mark[64];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    if (!GetEnvironmentVariableW(L"SPAWN_MARK", mark, 64)) wcscpy(mark, L"<unset>");
    printf("spawn-child: argc=%d\n", argc);
    for (int i = 1; i < argc; i++) printf("spawn-child: argv[%d]=[%s]\n", i, argv[i]);
    printf("spawn-child: cwd=%ls\n", cwd);
    printf("spawn-child: mark=%ls\n", mark);
    fflush(stdout);
    return 7;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "child") == 0) return child_main(argc, argv);

    DWORD code = 0;
    int ok = run_child(L"C:\\guest\\guest_hello.exe", NULL, NULL, NULL, &code);
    if (ok) printf("spawn: child exit %lu\n", code);
    int good = ok && code == 42;

    /* Quotes and a backslash before a quote must reach the child exactly as written. */
    wchar_t cmdline[] = L"\"C:\\guest\\guest_spawn.exe\" child \"a b\" c\\\"d e";
    wchar_t* env = env_with_mark();
    DWORD code2 = 0;
    ok = run_child(L"C:\\guest\\guest_spawn.exe", cmdline, env, L"C:\\Windows", &code2);
    free(env);
    if (ok) printf("spawn: child2 exit %lu\n", code2);
    good = good && ok && code2 == 7;

    printf("spawn: %s\n", good ? "done" : "FAILED");
    fflush(stdout);
    return good ? 42 : 1;
}
