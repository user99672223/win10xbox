/* guest_dll: a DLL exercising what the loader has to get right.
 *  - add():         plain code + export
 *  - read_tsc():    an rdtsc site (scanner target)
 *  - cpuid_ecx1():  a cpuid site (scanner target)
 *  - TLS directory with a callback (its pointer lives in relocated data) and a
 *    __declspec(thread) variable
 * The test maps this file without running DllMain/TLS (that is Wine's loader),
 * and may call the TLS callback by hand to check the relocated callback array. */
#include <windows.h>
#include <intrin.h>

#define GUEST_EXPORT __declspec(dllexport)
#ifdef _MSC_VER
#define GUEST_TLS __declspec(thread)
#else
#define GUEST_TLS __thread
#endif

GUEST_EXPORT int add(int a, int b) { return a + b; }

GUEST_EXPORT unsigned long long read_tsc(void) { return __rdtsc(); }

GUEST_EXPORT unsigned int cpuid_ecx1(void) {
    int r[4];
    __cpuidex(r, 1, 0);
    return (unsigned int)r[2];
}

/* ---- TLS --------------------------------------------------------------- */
GUEST_TLS int g_tls_value = 0x1234;
volatile int g_tls_callback_hits = 0;
volatile DWORD g_tls_callback_reason = 0xFFFFFFFFu;

static void NTAPI tls_callback(PVOID module, DWORD reason, PVOID reserved) {
    (void)module;
    (void)reserved;
    g_tls_callback_reason = reason;
    g_tls_callback_hits++;
}

#ifdef _MSC_VER
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:tls_callback_ptr")
#pragma const_seg(".CRT$XLB")
const PIMAGE_TLS_CALLBACK tls_callback_ptr = tls_callback;
#pragma const_seg()
#else
__attribute__((section(".CRT$XLB"), used)) PIMAGE_TLS_CALLBACK tls_callback_ptr = tls_callback;
#endif

GUEST_EXPORT int tls_callback_hits(void) { return g_tls_callback_hits; }
GUEST_EXPORT unsigned int tls_callback_reason(void) { return g_tls_callback_reason; }
GUEST_EXPORT int tls_value(void) { return g_tls_value; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst;
    (void)reason;
    (void)reserved;
    return TRUE;
}
