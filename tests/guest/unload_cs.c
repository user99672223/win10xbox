/* unload_cs: LoadLibrary/FreeLibrary of a DLL that leaks critical sections (unload_cs_dll.c) 50 times, then 1000
 * critical sections of the program's own. The host ntdll must not fault on a dangling debug record
 * (docs/slices/unload-slist.md). Prints `unload_cs: ok`, exit 42. */
#include <windows.h>
#include <stdio.h>

#ifndef CRITICAL_SECTION_NO_DEBUG_INFO
#define CRITICAL_SECTION_NO_DEBUG_INFO 0x01000000
#endif

int main(void) {
    char path[MAX_PATH];
    static CRITICAL_SECTION cs[1000];
    int i;
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    lstrcpyA(path + n, "guest_unload_cs_dll.dll");
    for (i = 0; i < 50; i++) {
        HMODULE m = LoadLibraryA(path);
        if (!m) { printf("unload_cs: FAIL LoadLibrary %d err=%lu\n", i, GetLastError()); return 1; }
        if (!GetProcAddress(m, "unload_cs_touch")) { printf("unload_cs: FAIL export %d\n", i); return 2; }
        if (!FreeLibrary(m)) { printf("unload_cs: FAIL FreeLibrary %d\n", i); return 3; }
    }
    printf("unload_cs: unloaded 50\n");
    for (i = 0; i < 1000; i++) {
        InitializeCriticalSectionEx(&cs[i], i % 7 ? 0 : 100, i % 3 ? 0 : CRITICAL_SECTION_NO_DEBUG_INFO);
        EnterCriticalSection(&cs[i]);
        LeaveCriticalSection(&cs[i]);
    }
    for (i = 0; i < 1000; i++) DeleteCriticalSection(&cs[i]);
    printf("unload_cs: ok\n");
    return 42;
}
