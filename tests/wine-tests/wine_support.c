/* Support code for the Wine conformance test guests (win10xbox, not Wine code).
 *
 * Wine's tests (dlls/<dll>/tests/*.c, LGPL, compiled from wine-pe/wine-src and
 * never copied into this repository) call __wine_dbg_strdup() through
 * wine/debug.h (wine_dbgstr_w() and friends). Wine's own build gets it from its
 * ntdll import library; the llvm-mingw import library for ntdll does not have
 * it, so the guests link this equivalent. The strings are intentionally leaked,
 * like Wine's debug string ring buffer in spirit; a test process is short-lived.
 */
#include <stdlib.h>
#include <string.h>

const char* __cdecl __wine_dbg_strdup(const char* str) {
    size_t n = strlen(str) + 1;
    char* copy = (char*)malloc(n);
    if (copy) memcpy(copy, str, n);
    return copy ? copy : "(out of memory)";
}
