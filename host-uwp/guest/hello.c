/*
 * hello.c - the sample guest bundled with host-uwp.
 *
 * A plain x64 Win32 program (static CRT, NOT AppContainer) that the win10xbox layer loads
 * and runs on top of Wine's PE ntdll/kernelbase/kernel32. It is what the host runs when
 * LocalState\launch.json does not name another program ("guest": "hello.exe", the default
 * of launch_config.h), and the packaged payload\launch.json expects it to exit with 42, so
 * a first run on the console ends in a PASS / FAIL verdict in status.txt.
 *
 * It writes a few facts to stdout (the host redirects stdout/stderr to LocalState\stdout.txt):
 *   - the greeting the repo's other hello guest prints (tests/guest/hello.c);
 *   - its command line and current directory, which shows that the "args" and "cwd" keys
 *     of launch.json reach the guest;
 *   - the OS version RtlGetVersion reports, which on the layer is the 22H2 spoof
 *     (10.0.19045) on a console that really runs build 26100.
 * and returns 42.
 *
 * Built twice from this one file by CMake (the same arrangement as host-uwp-lite's guest):
 *   hello.exe      /SUBSYSTEM:WINDOWS (WinMain, -DHELLO_GUI)  the default guest: the AppContainer
 *                  host has no console, and the repo records trouble with Wine's kernelbase
 *                  console path on real Windows (docs/STATUS.md)
 *   hello_cui.exe  /SUBSYSTEM:CONSOLE (main)                  same code, for A/B comparison
 *                  ("guest": "hello_cui.exe" in launch.json)
 *
 * Output goes straight to GetStdHandle(STD_OUTPUT_HANDLE) with WriteFile, not through CRT
 * stdio, so nothing depends on console detection. The only static import is KERNEL32.dll.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

#define HELLO_EXIT_CODE 42

static HANDLE g_out;

#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_LIKE(fmt_idx, first_arg) __attribute__((format(printf, fmt_idx, first_arg)))
#else
#define PRINTF_LIKE(fmt_idx, first_arg)
#endif

static void out(const char *fmt, ...) PRINTF_LIKE(1, 2);

static void out(const char *fmt, ...)
{
    char buf[1280];
    va_list ap;
    int n;
    DWORD written;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof buf - 2)
        n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    if (g_out != NULL && g_out != INVALID_HANDLE_VALUE)
        WriteFile(g_out, buf, (DWORD)n, &written, NULL);
}

/* UTF-16 to UTF-8 in a caller buffer; "?" when it does not fit. */
static void narrow(const wchar_t *w, char *dst, int cap)
{
    dst[0] = 0;
    if (w == NULL || WideCharToMultiByte(CP_UTF8, 0, w, -1, dst, cap, NULL, NULL) <= 0)
    {
        dst[0] = '?';
        dst[1] = 0;
    }
}

typedef LONG(WINAPI *rtl_get_version_fn)(OSVERSIONINFOW *);

static int hello_main(void)
{
    char text[1024];
    wchar_t cwd[MAX_PATH];
    DWORD n;
    HMODULE ntdll;
    rtl_get_version_fn get_version = NULL;
    OSVERSIONINFOW ver;

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    out("hello from guest");

    narrow(GetCommandLineW(), text, (int)sizeof text);
    out("guest: command line: %s", text);

    n = GetCurrentDirectoryW(MAX_PATH, cwd);
    if (n != 0 && n < MAX_PATH)
    {
        narrow(cwd, text, (int)sizeof text);
        out("guest: current directory: %s", text);
    }
    else
    {
        out("guest: current directory: unavailable (error %lu)", (unsigned long)GetLastError());
    }

    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != NULL)
        get_version = (rtl_get_version_fn)(void *)GetProcAddress(ntdll, "RtlGetVersion");
    ver.dwOSVersionInfoSize = sizeof ver;
    if (get_version != NULL && get_version(&ver) == 0)
        out("guest: OS version %lu.%lu build %lu (RtlGetVersion)", (unsigned long)ver.dwMajorVersion,
            (unsigned long)ver.dwMinorVersion, (unsigned long)ver.dwBuildNumber);
    else
        out("guest: OS version unavailable (RtlGetVersion)");

    out("guest: done, returning %d", HELLO_EXIT_CODE);
    return HELLO_EXIT_CODE;
}

#if defined(HELLO_GUI)
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)inst;
    (void)prev;
    (void)cmd;
    (void)show;
    return hello_main();
}
#else
int main(void)
{
    return hello_main();
}
#endif
