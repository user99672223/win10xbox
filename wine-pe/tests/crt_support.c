/* Support code for the CRT conformance suites (wine-pe/tests/{msvcrt,ucrtbase}.sh).
 * win10xbox code, not Wine code.
 *
 * __wine_dbg_strdup: Wine's tests call it through wine/debug.h (wine_dbgstr_w()
 * and friends); Wine's own build gets it from its ntdll import library, the
 * llvm-mingw one does not have it. Same as tests/wine-tests/wine_support.c.
 *
 * WIN10XBOX_CRT_MSVCRT: the msvcrt suite is linked -nostdlib against msvcrt.dll
 * only (as Wine links the tests of a CRT DLL against that DLL), so it gets this
 * minimal entry point instead of mingw-w64's UCRT startup code.
 */
#include <stdlib.h>
#include <string.h>

const char* __cdecl __wine_dbg_strdup(const char* str) {
    size_t n = strlen(str) + 1;
    char* copy = (char*)malloc(n);
    if (copy) memcpy(copy, str, n);
    return copy ? copy : "(out of memory)";
}

#ifdef WIN10XBOX_CRT_MSVCRT
typedef struct { int newmode; } w10x_startupinfo;
extern void __cdecl __getmainargs(int*, char***, char***, int, w10x_startupinfo*);
extern int main(int, char**);

/* mingw's main() calls __main to run constructors; these C tests have none. */
void __main(void) {}

void __cdecl mainCRTStartup(void) {
    int argc;
    char **argv, **env;
    w10x_startupinfo si = {0};
    __getmainargs(&argc, &argv, &env, 0, &si);
    exit(main(argc, argv));
}
#endif
