/*
 * smoke_guest.c - the guest program of the host-uwp-lite on-console smoke.
 *
 * A plain x64 Win32 program (static CRT, NOT AppContainer) that the win10xbox
 * layer loads and runs on top of Wine's PE ntdll/kernelbase/kernel32. It writes
 * a few facts to stdout (the host redirects stdout/stderr to LocalState\stdout.txt)
 * and returns 42; the exit code is the success signal the host checks.
 *
 * The facts make the layer's virtualisation observable:
 *   - OS version as the guest sees it (PEB fields, GetVersionExW, RtlGetVersion):
 *     the 22H2 spoof (10.0.19045) on a console that is really build 26100;
 *   - GetSystemInfo, CPUID vendor / family / hypervisor bit / leaf 0x40000000 /
 *     brand string (the D18 identity: AuthenticAMD, hypervisor hidden);
 *   - rdtsc/QPC progress (patched to the virtual clock);
 *   - which file a genuine Microsoft DLL is read from when asked for by name.
 *
 * Command-line modes (set from LocalState\guest.txt, e.g. "smoke.exe load"):
 *   (none)  facts, then a LOAD_LIBRARY_AS_DATAFILE look at ucrtbase.dll / msvcrt.dll
 *           (SEH guarded, last, so the facts are already on disk if it goes wrong)
 *   min     facts only
 *   load    facts, the data-file look, then a REAL LoadLibraryW of the same DLLs
 *           (runs the genuine DllMain on the layer; SEH guarded)
 *
 * Built twice from this one file by CMake:
 *   smoke.exe      /SUBSYSTEM:WINDOWS (WinMain)  default, as a precaution: the
 *                  AppContainer host has no console (ConsoleHandle is NULL) and the
 *                  repo records trouble with Wine's kernelbase console path on real
 *                  Windows (docs/STATUS.md, BuildProcessParameters in
 *                  core/dispatch/process.cpp). A hypothesis, not a measurement.
 *   smoke_cui.exe  /SUBSYSTEM:CONSOLE (main)     same code, for A/B comparison
 *
 * Output goes straight to GetStdHandle(STD_OUTPUT_HANDLE) with WriteFile, not
 * through CRT stdio, so nothing depends on console detection.
 */
#include <windows.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#pragma warning(disable : 4996) /* GetVersionExW is deprecated: reading it is the point */
#endif

#define EXPECTED_EXIT_CODE 42

static HANDLE g_out;

#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_LIKE(fmt_idx, first_arg) __attribute__((format(printf, fmt_idx, first_arg)))
#else
#define PRINTF_LIKE(fmt_idx, first_arg)
#endif

static void out(const char *fmt, ...) PRINTF_LIKE(1, 2);

static void out(const char *fmt, ...)
{
    char buf[640];
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

/* ---- command line ------------------------------------------------------- */

/* True if `word` is one of the arguments after argv[0]. */
static int has_arg(const char *cmd, const char *word)
{
    const char *p = cmd;
    size_t wl = strlen(word);

    if (p == NULL)
        return 0;
    if (*p == '"')
    {
        ++p;
        while (*p != 0 && *p != '"')
            ++p;
        if (*p != 0)
            ++p;
    }
    else
    {
        while (*p != 0 && *p != ' ' && *p != '\t')
            ++p;
    }
    for (;;)
    {
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == 0)
            return 0;
        if (strncmp(p, word, wl) == 0 && (p[wl] == 0 || p[wl] == ' ' || p[wl] == '\t'))
            return 1;
        while (*p != 0 && *p != ' ' && *p != '\t')
            ++p;
    }
}

/* ---- OS version --------------------------------------------------------- */

typedef LONG(NTAPI *RtlGetVersionFn)(RTL_OSVERSIONINFOW *);

static void report_version(void)
{
    const unsigned char *peb = (const unsigned char *)__readgsqword(0x60);
    const volatile ULONG *kusd = (const volatile ULONG *)0x7FFE0000u;
    OSVERSIONINFOW vi;
    RTL_OSVERSIONINFOW rv;
    HMODULE ntdll;
    RtlGetVersionFn rtl_get_version = NULL;

    /* PEB.OSMajorVersion 0x118, OSMinorVersion 0x11C, OSBuildNumber 0x120, OSPlatformId 0x124 */
    out("peb: os %lu.%lu build %u platform %lu", (unsigned long)*(const DWORD *)(peb + 0x118),
        (unsigned long)*(const DWORD *)(peb + 0x11C), (unsigned)*(const WORD *)(peb + 0x120),
        (unsigned long)*(const DWORD *)(peb + 0x124));

    memset(&vi, 0, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    if (GetVersionExW(&vi))
        out("GetVersionExW: %lu.%lu build %lu platform %lu csd='%ls'", (unsigned long)vi.dwMajorVersion,
            (unsigned long)vi.dwMinorVersion, (unsigned long)vi.dwBuildNumber, (unsigned long)vi.dwPlatformId,
            vi.szCSDVersion);
    else
        out("GetVersionExW: failed, error %lu", (unsigned long)GetLastError());

    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != NULL)
        rtl_get_version = (RtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion");
    memset(&rv, 0, sizeof rv);
    rv.dwOSVersionInfoSize = sizeof rv;
    if (rtl_get_version != NULL && rtl_get_version(&rv) == 0)
        out("RtlGetVersion: %lu.%lu build %lu platform %lu", (unsigned long)rv.dwMajorVersion,
            (unsigned long)rv.dwMinorVersion, (unsigned long)rv.dwBuildNumber, (unsigned long)rv.dwPlatformId);
    else
        out("RtlGetVersion: not available");

    /* KUSER_SHARED_DATA: NtBuildNumber 0x260, NtMajorVersion 0x26C, NtMinorVersion 0x270.
     * Informational: this is the host kernel's page, not necessarily the spoofed values. */
    out("KUSER_SHARED_DATA: %lu.%lu build %lu", (unsigned long)kusd[0x26C / 4], (unsigned long)kusd[0x270 / 4],
        (unsigned long)kusd[0x260 / 4]);
}

/* ---- system / CPU ------------------------------------------------------- */

static void report_system(void)
{
    SYSTEM_INFO si, nsi;

    memset(&si, 0, sizeof si);
    memset(&nsi, 0, sizeof nsi);
    GetSystemInfo(&si);
    GetNativeSystemInfo(&nsi);
    out("GetSystemInfo: arch %u procs %lu type %lu level %u revision 0x%04x pagesize %lu app_addr %p..%p",
        (unsigned)si.wProcessorArchitecture, (unsigned long)si.dwNumberOfProcessors,
        (unsigned long)si.dwProcessorType, (unsigned)si.wProcessorLevel, (unsigned)si.wProcessorRevision,
        (unsigned long)si.dwPageSize, si.lpMinimumApplicationAddress, si.lpMaximumApplicationAddress);
    out("GetNativeSystemInfo: arch %u procs %lu", (unsigned)nsi.wProcessorArchitecture,
        (unsigned long)nsi.dwNumberOfProcessors);
}

static void report_cpu(void)
{
    int r[4];
    char vendor[13];
    char brand[49];
    unsigned eax, ecx, base_family, family, model;
    unsigned max_ext;
    int i;

    __cpuid(r, 0);
    memcpy(vendor + 0, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    vendor[12] = 0;
    out("cpuid.0: max_leaf %u vendor '%s'", (unsigned)r[0], vendor);

    __cpuid(r, 1);
    eax = (unsigned)r[0];
    ecx = (unsigned)r[2];
    base_family = (eax >> 8) & 0xF;
    family = base_family == 0xF ? base_family + ((eax >> 20) & 0xFF) : base_family;
    model = (eax >> 4) & 0xF;
    if (base_family == 0xF || base_family == 0x6)
        model |= (eax >> 12) & 0xF0;
    out("cpuid.1: eax=%08x ebx=%08x ecx=%08x edx=%08x", eax, (unsigned)r[1], ecx, (unsigned)r[3]);
    out("cpuid.1: family %u model %u stepping %u hypervisor_bit %u rdrand_bit %u", family, model, eax & 0xF,
        (ecx >> 31) & 1, (ecx >> 30) & 1);

    __cpuid(r, 0x40000000);
    out("cpuid.40000000: eax=%08x ebx=%08x ecx=%08x edx=%08x (%s)", (unsigned)r[0], (unsigned)r[1], (unsigned)r[2],
        (unsigned)r[3], (r[0] | r[1] | r[2] | r[3]) == 0 ? "hidden" : "VISIBLE");

    __cpuid(r, 0x80000000);
    max_ext = (unsigned)r[0];
    brand[0] = 0;
    if (max_ext >= 0x80000004u)
    {
        for (i = 0; i < 3; ++i)
        {
            __cpuid(r, 0x80000002 + i);
            memcpy(brand + 16 * i, r, 16);
        }
        brand[48] = 0;
    }
    out("cpuid.brand: '%s'", brand);
}

static void report_clock(void)
{
    LARGE_INTEGER freq, c0, c1;
    unsigned long long t0, t1;
    SYSTEMTIME st;

    freq.QuadPart = 0;
    c0.QuadPart = 0;
    c1.QuadPart = 0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c0);
    t0 = __rdtsc();
    Sleep(20);
    t1 = __rdtsc();
    QueryPerformanceCounter(&c1);
    GetSystemTime(&st);
    out("clock: qpc freq %lld, qpc delta over Sleep(20) %lld, rdtsc delta %llu (%s)", (long long)freq.QuadPart,
        (long long)(c1.QuadPart - c0.QuadPart), t1 - t0, t1 > t0 ? "monotonic" : "NOT monotonic");
    out("clock: system time %04u-%02u-%02u %02u:%02u:%02u UTC", (unsigned)st.wYear, (unsigned)st.wMonth,
        (unsigned)st.wDay, (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
}

/* ---- genuine DLLs by name ---------------------------------------------- */

/* Wine's PE builtins carry the signature "Wine builtin DLL" in the DOS stub (offset 0x40). */
static int is_wine_builtin(const unsigned char *base)
{
    int i;

    for (i = 0; i + 16 <= 0x200; ++i)
        if (memcmp(base + i, "Wine builtin DLL", 16) == 0)
            return 1;
    return 0;
}

/* Maps the DLL as a data file (no DllMain, no imports) and reports which file the
 * VFS handed out (staged genuine copy vs. Wine builtin) and its version resource. */
static void probe_datafile(const wchar_t *name)
{
    HMODULE h;
    wchar_t path[MAX_PATH];
    DWORD n, code = 0;
    const unsigned char *base;

    __try
    {
        h = LoadLibraryExW(name, NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (h == NULL)
        {
            out("datafile %ls: LoadLibraryExW failed, error %lu", name, (unsigned long)GetLastError());
            return;
        }
        path[0] = 0;
        n = GetModuleFileNameW(h, path, MAX_PATH);
        base = (const unsigned char *)((ULONG_PTR)h & ~(ULONG_PTR)3);
        out("datafile %ls: mapped at %p, path '%ls', flavor %s", name, (const void *)base, n != 0 ? path : L"?",
            is_wine_builtin(base) ? "WINE-BUILTIN" : "native (genuine)");
        {
            HRSRC res = FindResourceW(h, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16) /* RT_VERSION */);
            HGLOBAL g = res != NULL ? LoadResource(h, res) : NULL;
            const unsigned char *p = g != NULL ? (const unsigned char *)LockResource(g) : NULL;
            DWORD size = res != NULL ? SizeofResource(h, res) : 0;
            DWORD off, sig, ms = 0, ls = 0;
            int found = 0;

            for (off = 0; p != NULL && off + 16 <= size && off < 96; off += 4)
            {
                memcpy(&sig, p + off, 4);
                if (sig == 0xFEEF04BDu)
                {
                    memcpy(&ms, p + off + 8, 4);
                    memcpy(&ls, p + off + 12, 4);
                    found = 1;
                    break;
                }
            }
            if (found)
                out("datafile %ls: file version %u.%u.%u.%u", name, (unsigned)HIWORD(ms), (unsigned)LOWORD(ms),
                    (unsigned)HIWORD(ls), (unsigned)LOWORD(ls));
            else
                out("datafile %ls: no version resource found", name);
        }
        FreeLibrary(h);
    }
    __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
        out("datafile %ls: EXCEPTION 0x%08lx while probing", name, (unsigned long)code);
    }
}

/* A real load: runs the DLL's DllMain on the layer, then unloads it again inside
 * the SEH guard so that nothing genuine is left to detach at process exit. */
static void probe_load(const wchar_t *name)
{
    HMODULE h = NULL;
    wchar_t path[MAX_PATH];
    DWORD n, code = 0;

    out("load %ls: LoadLibraryW (real load, runs DllMain) ...", name);
    __try
    {
        h = LoadLibraryW(name);
        if (h == NULL)
        {
            out("load %ls: failed, error %lu", name, (unsigned long)GetLastError());
        }
        else
        {
            typedef size_t(__cdecl * StrlenFn)(const char *);
            StrlenFn fn;

            path[0] = 0;
            n = GetModuleFileNameW(h, path, MAX_PATH);
            out("load %ls: loaded at %p, path '%ls', flavor %s", name, (const void *)h, n != 0 ? path : L"?",
                is_wine_builtin((const unsigned char *)h) ? "WINE-BUILTIN" : "native (genuine)");
            fn = (StrlenFn)(void *)GetProcAddress(h, "strlen");
            if (fn != NULL)
                out("load %ls: strlen(\"win10xbox\") through the loaded DLL = %u", name, (unsigned)fn("win10xbox"));
            FreeLibrary(h);
            out("load %ls: unloaded", name);
        }
    }
    __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
        out("load %ls: EXCEPTION 0x%08lx", name, (unsigned long)code);
    }
}

/* ---- main --------------------------------------------------------------- */

static int smoke_main(void)
{
    const char *cmd = GetCommandLineA();

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    out("hello from guest");
    out("guest: command line '%s'", cmd != NULL ? cmd : "?");

    report_version();
    report_system();
    report_cpu();
    report_clock();

    if (!has_arg(cmd, "min"))
    {
        probe_datafile(L"ucrtbase.dll");
        probe_datafile(L"msvcrt.dll");
    }
    if (has_arg(cmd, "load"))
    {
        probe_load(L"ucrtbase.dll");
        probe_load(L"msvcrt.dll");
    }

    out("guest: done, returning %d", EXPECTED_EXIT_CODE);
    return EXPECTED_EXIT_CODE;
}

#if defined(SMOKE_GUI)
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)inst;
    (void)prev;
    (void)cmd;
    (void)show;
    return smoke_main();
}
#else
int main(void)
{
    return smoke_main();
}
#endif
