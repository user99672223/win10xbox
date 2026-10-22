/*
 * ac_gui_probe.c - can a process in THIS security context use the host's own user32/gdi32/win32u?
 *
 *   ac_gui_probe
 *
 * The question behind it ("route B"): the dispatcher forwards ntdll syscalls by name to the host; it could forward
 * NtUser* / NtGdi* to the host's own win32u.dll, so that the real win32k manages windows, controls, menus and fonts. That
 * only helps if a process inside an AppContainer may create, show and paint windows with the native user32/gdi32, and if
 * the pixels can be read back so that a presenter can put them on the TV. host-win32-appcontainer.yml runs this program
 * plain, in a genuine AppContainer and in a less-privileged one (launched by ac_launch, detached from any console like a
 * UWP app), and once directly from the runner's shell; the difference between the outputs IS the measurement.
 *
 * Output: one line per probe, "PROBE <id> | <OK|FAIL|INFO> | <detail>", ids "GUI.<area>.<what>". FAIL is data, not an
 * error: the exit status is always 0 (an unhandled exception or a call that hangs is itself reported as a PROBE line and
 * the program exits 0).
 *
 * user32.dll and gdi32.dll are DELAY-loaded (the CI build links them with /DELAYLOAD) and loaded explicitly with
 * LoadLibraryW first (GUI.load.*): a process whose static import of user32 cannot connect to win32k dies in the loader
 * (STATUS_DLL_INIT_FAILED) before it can print anything, and a failing LoadLibraryW reports the same condition as a
 * Win32 error code instead. It is also the way the layer would bind them: at run time, from the host.
 *
 * Plain C, static CRT, imports kernel32, user32 and gdi32 only.
 * Build (CI):    cl /nologo /O2 /MT /std:c11 /W3 /utf-8 ac_gui_probe.c /link user32.lib gdi32.lib delayimp.lib
 *                   /DELAYLOAD:user32.dll /DELAYLOAD:gdi32.dll
 * Build (check): x86_64-w64-mingw32-clang -std=c11 -municode -static ac_gui_probe.c -o ac_gui_probe.exe -luser32 -lgdi32
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#pragma warning(disable : 4996 4191)
#endif

#ifndef PW_CLIENTONLY
#define PW_CLIENTONLY 0x00000001
#endif
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif
#ifndef UOI_IO
#define UOI_IO 6
#endif

/* ------------------------------------------------------------------------------------------------------------------ */
/* Output                                                                                                              */
/* ------------------------------------------------------------------------------------------------------------------ */

static HANDLE g_out;
static volatile LONG g_ok, g_fail;
static const char *volatile g_stage = "start";
static volatile ULONGLONG g_stage_t0;

static void Probe(const char *id, const char *status, const char *fmt, ...)
{
    char line[2600];
    DWORD written;
    int n, m;
    va_list ap;

    n = snprintf(line, sizeof line - 2, "PROBE %s | %s | ", id, status);
    if (n < 0)
        return;
    va_start(ap, fmt);
    m = vsnprintf(line + n, sizeof line - 2 - (size_t)n, fmt, ap);
    va_end(ap);
    if (m < 0)
        m = 0;
    if (n + m > (int)sizeof line - 3)
        m = (int)sizeof line - 3 - n;
    line[n + m] = '\n';
    if (g_out != NULL && g_out != INVALID_HANDLE_VALUE)
        WriteFile(g_out, line, (DWORD)(n + m + 1), &written, NULL);
    if (strcmp(status, "OK") == 0)
        InterlockedIncrement(&g_ok);
    else if (strcmp(status, "FAIL") == 0)
        InterlockedIncrement(&g_fail);
}

/* "error 5 (0x00000005) Access is denied" */
static const char *ErrText(DWORD e, char *out, size_t cap)
{
    char msg[220];
    DWORD n;

    msg[0] = '\0';
    n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg, sizeof msg, NULL);
    while (n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n' || msg[n - 1] == ' ' || msg[n - 1] == '.'))
        msg[--n] = '\0';
    snprintf(out, cap, "error %lu (0x%08lX)%s%s", (unsigned long)e, (unsigned long)e, n ? " " : "", msg);
    return out;
}

static const char *U8(const wchar_t *w, char *out, int cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    if (n <= 0)
        out[0] = '\0';
    return out;
}

static void Stage(const char *s)
{
    g_stage = s;
    g_stage_t0 = GetTickCount64();
}

static void Summary(void)
{
    Probe("GUI.summary", "INFO", "%ld ok, %ld fail (FAIL is a measurement, not an error)", (long)g_ok, (long)g_fail);
}

/* An exception (a delay-load failure raises 0xC06D007E / 0xC06D007F) is a result, not a reason to lose the log. */
static LONG WINAPI Unhandled(EXCEPTION_POINTERS *ep)
{
    Probe("GUI.crash", "FAIL", "unhandled exception 0x%08lX at %p during %s (0xC06D007E/0xC06D007F: a delay-loaded DLL/function could not be bound)",
          (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress, g_stage);
    Summary();
    ExitProcess(0);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* A call that never returns (a window message nobody answers, a desktop that blocks) is a result too. */
static DWORD WINAPI Watchdog(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(500);
        if (GetTickCount64() - g_stage_t0 > 20000) {
            Probe("GUI.watchdog", "FAIL", "still in %s after 20 s; giving up on the remaining probes", g_stage);
            Summary();
            ExitProcess(0);
        }
    }
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Context: OS, session, token (read through kernelbase so that the import list stays kernel32/user32/gdi32)          */
/* ------------------------------------------------------------------------------------------------------------------ */

static void ProbeContext(void)
{
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOEXW *);
    typedef BOOL(WINAPI * GetTokenInformationFn)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD);
    OSVERSIONINFOEXW vi;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    RtlGetVersionFn ver = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    GetTokenInformationFn gti = kb ? (GetTokenInformationFn)(void *)GetProcAddress(kb, "GetTokenInformation") : NULL;
    HANDLE tok = (HANDLE)(LONG_PTR)-4; /* GetCurrentProcessToken() */
    DWORD session = 0, ac = 0xFFFFFFFF, lpac = 0xFFFFFFFF, rid = 0, n = 0;
    union {
        TOKEN_MANDATORY_LABEL tml;
        BYTE raw[128];
    } il;

    memset(&vi, 0, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    if (ver != NULL && ver(&vi) == 0)
        Probe("GUI.os", "INFO", "Windows %lu.%lu build %lu, product type %u", (unsigned long)vi.dwMajorVersion,
              (unsigned long)vi.dwMinorVersion, (unsigned long)vi.dwBuildNumber, (unsigned)vi.wProductType);
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    if (gti != NULL) {
        gti(tok, (TOKEN_INFORMATION_CLASS)29 /* TokenIsAppContainer */, &ac, sizeof ac, &n);
        gti(tok, (TOKEN_INFORMATION_CLASS)46 /* TokenIsLessPrivilegedAppContainer */, &lpac, sizeof lpac, &n);
        if (gti(tok, TokenIntegrityLevel, &il, sizeof il, &n)) {
            PSID sid = il.tml.Label.Sid;
            /* the RID is the last sub-authority of S-1-16-<rid> */
            rid = ((SID *)sid)->SubAuthority[((SID *)sid)->SubAuthorityCount - 1];
        }
    }
    Probe("GUI.context", "INFO", "pid %lu, session %lu, token: AppContainer=%ld lpac=%ld integrity=0x%04lX",
          (unsigned long)GetCurrentProcessId(), (unsigned long)session, (long)ac, (long)lpac, (unsigned long)rid);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Which DLLs, and what the host's win32u exports                                                                      */
/* ------------------------------------------------------------------------------------------------------------------ */

static const wchar_t *const kDlls[] = {L"user32.dll", L"gdi32.dll", L"gdi32full.dll", L"win32u.dll",
                                       L"uxtheme.dll", L"comctl32.dll", L"imm32.dll"};

static void ProbeDllsLoaded(const char *id)
{
    char line[1400], name[64], path[600];
    wchar_t where[MAX_PATH];
    size_t used = 0, i;

    line[0] = '\0';
    for (i = 0; i < sizeof kDlls / sizeof kDlls[0]; i++) {
        HMODULE h = GetModuleHandleW(kDlls[i]);
        U8(kDlls[i], name, sizeof name);
        if (h != NULL && i < 4) {
            where[0] = L'\0';
            GetModuleFileNameW(h, where, MAX_PATH);
            used += (size_t)snprintf(line + used, sizeof line - used, "%s%s=loaded(%s)", used ? " " : "", name, U8(where, path, sizeof path));
        } else {
            used += (size_t)snprintf(line + used, sizeof line - used, "%s%s=%s", used ? " " : "", name, h ? "loaded" : "no");
        }
        if (used >= sizeof line - 200)
            break;
    }
    Probe(id, "INFO", "%s", line);
}

static const char *const kWin32uExports[] = {"NtUserCreateWindowEx", "NtGdiPatBlt", "NtUserGetMessage", "NtGdiCreateSolidBrush"};

static void ProbeWin32uExports(void)
{
    char line[900], err[260];
    size_t used = 0, i;
    int missing = 0;
    HMODULE w = GetModuleHandleW(L"win32u.dll");
    const char *how = "already loaded";

    if (w == NULL) {
        how = "LoadLibraryW";
        SetLastError(0);
        w = LoadLibraryW(L"win32u.dll");
    }
    if (w == NULL) {
        Probe("GUI.dlls.win32u_exports", "FAIL", "win32u.dll not loaded and LoadLibraryW failed: %s", ErrText(GetLastError(), err, sizeof err));
        return;
    }
    line[0] = '\0';
    for (i = 0; i < sizeof kWin32uExports / sizeof kWin32uExports[0]; i++) {
        FARPROC p;
        SetLastError(0);
        p = GetProcAddress(w, kWin32uExports[i]);
        if (p == NULL)
            missing++;
        used += (size_t)snprintf(line + used, sizeof line - used, "%s%s=%s", used ? " " : "", kWin32uExports[i], p ? "exported" : "MISSING");
        if (p == NULL)
            used += (size_t)snprintf(line + used, sizeof line - used, "(%lu)", (unsigned long)GetLastError());
    }
    Probe("GUI.dlls.win32u_exports", missing ? "FAIL" : "OK", "win32u.dll (%s): %s", how, line);
}

/* The first 24 bytes of a win32u thunk. The loader rewrites "4C 8B D1 B8 <id32> ... 0F 05". */
static void ProbeSyscallStub(const char *name)
{
    char id[96], hex[24 * 3 + 1], err[260];
    HMODULE w = GetModuleHandleW(L"win32u.dll");
    const unsigned char *b;
    int i, sc = -1;

    snprintf(id, sizeof id, "GUI.syscallstub.%s", name);
    if (w == NULL)
        w = LoadLibraryW(L"win32u.dll");
    b = w ? (const unsigned char *)(void *)GetProcAddress(w, name) : NULL;
    if (b == NULL) {
        Probe(id, "FAIL", "no export: %s", ErrText(GetLastError(), err, sizeof err));
        return;
    }
    for (i = 0; i < 24; i++)
        snprintf(hex + i * 3, sizeof hex - (size_t)i * 3, "%02X%s", b[i], i < 23 ? " " : "");
    for (i = 0; i + 1 < 24; i++)
        if (b[i] == 0x0F && b[i + 1] == 0x05) {
            sc = i;
            break;
        }
    if (b[0] == 0x4C && b[1] == 0x8B && b[2] == 0xD1 && b[3] == 0xB8) {
        DWORD num;
        memcpy(&num, b + 4, sizeof num);
        if (sc >= 0)
            Probe(id, "OK", "%s; shape mov r10,rcx; mov eax,imm32; ... syscall at +%d; syscall id 0x%04lX", hex, sc, (unsigned long)num);
        else
            Probe(id, "INFO", "%s; starts mov r10,rcx; mov eax,0x%04lX but no 0F 05 within 24 bytes", hex, (unsigned long)num);
    } else {
        Probe(id, "INFO", "%s; NOT the 4C 8B D1 B8 shape", hex);
    }
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* The window                                                                                                          */
/* ------------------------------------------------------------------------------------------------------------------ */

#define COLOR_GETDC RGB(0x11, 0x22, 0x33) /* painted through GetDC */
#define COLOR_PAINT RGB(0x30, 0xA0, 0x50) /* painted by WM_PAINT and WM_PRINTCLIENT */
#define COLOR_MEM RGB(0x20, 0x40, 0x80)   /* painted into the memory DC */
#define COLOR_SENTINEL RGB(0xFF, 0x00, 0xFF)

static HWND g_main;
static struct {
    LONG nccreate, create, paint, paint_nodc, printclient, timer, posted, sent, erase;
    DWORD paint_err;
    RECT paint_rc;
} g_wm;

static int FillSolid(HDC dc, const RECT *rc, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    int r;
    if (br == NULL)
        return 0;
    r = FillRect(dc, rc, br);
    DeleteObject(br);
    return r;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    RECT rc;
    switch (m) {
    case WM_NCCREATE:
        g_wm.nccreate++;
        break;
    case WM_CREATE:
        g_wm.create++;
        break;
    case WM_ERASEBKGND:
        g_wm.erase++;
        return 1;
    case WM_PAINT:
        if (h == g_main) {
            PAINTSTRUCT ps;
            HDC dc;
            g_wm.paint++;
            SetLastError(0);
            dc = BeginPaint(h, &ps);
            if (dc == NULL) {
                g_wm.paint_nodc++;
                g_wm.paint_err = GetLastError();
            } else {
                g_wm.paint_rc = ps.rcPaint;
                GetClientRect(h, &rc);
                FillSolid(dc, &rc, COLOR_PAINT);
            }
            EndPaint(h, &ps);
            return 0;
        }
        break;
    case WM_PRINTCLIENT:
        g_wm.printclient++;
        GetClientRect(h, &rc);
        FillSolid((HDC)w, &rc, COLOR_PAINT);
        return 0;
    case WM_TIMER:
        g_wm.timer++;
        return 0;
    case WM_APP + 1:
        g_wm.posted++;
        return 0;
    case WM_APP + 2:
        g_wm.sent++;
        return 0x5A5A;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Memory DC + DIB helpers                                                                                             */
/* ------------------------------------------------------------------------------------------------------------------ */

#define DIB_W 64
#define DIB_H 64

typedef struct {
    HDC dc;
    HBITMAP bmp, old;
    DWORD *bits;
} Dib;

/* COLORREF 0x00BBGGRR -> 32 bpp DIB pixel 0x00RRGGBB */
static DWORD ToDib(COLORREF c) { return ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | (DWORD)GetBValue(c); }

static int MakeDib(Dib *d, char *why, size_t cap)
{
    BITMAPINFO bi;
    char err[260];
    void *bits = NULL;

    memset(d, 0, sizeof *d);
    SetLastError(0);
    d->dc = CreateCompatibleDC(NULL);
    if (d->dc == NULL) {
        snprintf(why, cap, "CreateCompatibleDC(NULL): %s", ErrText(GetLastError(), err, sizeof err));
        return 0;
    }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = DIB_W;
    bi.bmiHeader.biHeight = -DIB_H; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetLastError(0);
    d->bmp = CreateDIBSection(d->dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (d->bmp == NULL || bits == NULL) {
        snprintf(why, cap, "CreateDIBSection 64x64 32bpp: %s", ErrText(GetLastError(), err, sizeof err));
        DeleteDC(d->dc);
        d->dc = NULL;
        return 0;
    }
    d->bits = (DWORD *)bits;
    d->old = (HBITMAP)SelectObject(d->dc, d->bmp);
    return 1;
}

static void FreeDib(Dib *d)
{
    if (d->dc != NULL) {
        SelectObject(d->dc, d->old);
        DeleteObject(d->bmp);
        DeleteDC(d->dc);
    }
    memset(d, 0, sizeof *d);
}

static void FillDib(Dib *d, COLORREF c)
{
    RECT rc = {0, 0, DIB_W, DIB_H};
    FillSolid(d->dc, &rc, c);
    GdiFlush();
}

static int CountEq(const Dib *d, DWORD want)
{
    int i, n = 0;
    for (i = 0; i < DIB_W * DIB_H; i++)
        if ((d->bits[i] & 0x00FFFFFF) == want)
            n++;
    return n;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* The probes                                                                                                          */
/* ------------------------------------------------------------------------------------------------------------------ */

static void ObjName(HANDLE h, char *out, int cap)
{
    wchar_t name[256];
    DWORD need = 0;
    char err[200];
    if (GetUserObjectInformationW(h, UOI_NAME, name, sizeof name, &need))
        U8(name, out, cap);
    else
        snprintf(out, (size_t)cap, "(UOI_NAME: %s)", ErrText(GetLastError(), err, sizeof err));
}

static void ProbeWinsta(void)
{
    char name[600], err[260];
    HWINSTA ws;
    HDESK dt, in;
    USEROBJECTFLAGS f;
    DWORD need = 0;
    BOOL io = FALSE;

    Stage("GUI.winsta");
    SetLastError(0);
    ws = GetProcessWindowStation();
    if (ws == NULL) {
        Probe("GUI.winsta.station", "FAIL", "GetProcessWindowStation: %s", ErrText(GetLastError(), err, sizeof err));
    } else {
        ObjName(ws, name, sizeof name);
        memset(&f, 0, sizeof f);
        if (GetUserObjectInformationW(ws, UOI_FLAGS, &f, sizeof f, &need))
            Probe("GUI.winsta.station", "OK", "window station \"%s\", UOI_FLAGS dwFlags=0x%lX -> %s", name, (unsigned long)f.dwFlags,
                  (f.dwFlags & WSF_VISIBLE) ? "interactive (WSF_VISIBLE)" : "NOT interactive (no WSF_VISIBLE)");
        else
            Probe("GUI.winsta.station", "OK", "window station \"%s\", UOI_FLAGS: %s", name, ErrText(GetLastError(), err, sizeof err));
    }
    SetLastError(0);
    dt = GetThreadDesktop(GetCurrentThreadId());
    if (dt == NULL) {
        Probe("GUI.winsta.desktop", "FAIL", "GetThreadDesktop: %s", ErrText(GetLastError(), err, sizeof err));
    } else {
        ObjName(dt, name, sizeof name);
        if (GetUserObjectInformationW(dt, UOI_IO, &io, sizeof io, &need))
            Probe("GUI.winsta.desktop", "OK", "thread desktop \"%s\", UOI_IO=%d (%s)", name, (int)io,
                  io ? "this desktop receives user input" : "this desktop does NOT receive user input");
        else
            Probe("GUI.winsta.desktop", "OK", "thread desktop \"%s\", UOI_IO: %s", name, ErrText(GetLastError(), err, sizeof err));
    }
    SetLastError(0);
    in = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (in == NULL) {
        Probe("GUI.winsta.input_desktop", "FAIL", "OpenInputDesktop(DESKTOP_READOBJECTS): %s", ErrText(GetLastError(), err, sizeof err));
    } else {
        ObjName(in, name, sizeof name);
        Probe("GUI.winsta.input_desktop", "OK", "OpenInputDesktop: \"%s\"", name);
        CloseDesktop(in);
    }
}

static HWND CreateOne(const char *id, const wchar_t *cls, DWORD style, int x, int y, int w, int h, HWND parent)
{
    char err[260];
    HWND hw;
    LONG nc0 = g_wm.nccreate, c0 = g_wm.create;

    SetLastError(0);
    hw = CreateWindowExW(0, cls, L"win10xbox ac_gui_probe", style, x, y, w, h, parent, NULL, GetModuleHandleW(NULL), NULL);
    if (hw == NULL)
        Probe(id, "FAIL", "CreateWindowExW(style 0x%08lX): %s (WM_NCCREATE seen %ld, WM_CREATE seen %ld)", (unsigned long)style,
              ErrText(GetLastError(), err, sizeof err), (long)(g_wm.nccreate - nc0), (long)(g_wm.create - c0));
    else
        Probe(id, "OK", "hwnd %p (style 0x%08lX; WM_NCCREATE %ld, WM_CREATE %ld)", (void *)hw, (unsigned long)style,
              (long)(g_wm.nccreate - nc0), (long)(g_wm.create - c0));
    return hw;
}

static void ProbePaint(void)
{
    char err[260];
    HDC dc;
    RECT rc;
    COLORREF px;
    MSG msg;
    UINT_PTR timer;
    BOOL posted;
    DWORD e_timer, e_post;
    ULONGLONG deadline, t0;
    LONG peeked = 0, peeked_post = 0;
    LRESULT sr;

    /* GetDC + FillRect + GetPixel */
    Stage("GUI.paint.getdc");
    SetLastError(0);
    dc = GetDC(g_main);
    if (dc == NULL) {
        Probe("GUI.paint.getdc_fill_getpixel", "FAIL", "GetDC(hwnd): %s", ErrText(GetLastError(), err, sizeof err));
    } else {
        int filled;
        GetClientRect(g_main, &rc);
        SetLastError(0);
        filled = FillSolid(dc, &rc, COLOR_GETDC);
        GdiFlush();
        SetLastError(0);
        px = GetPixel(dc, 10, 10);
        if (px == CLR_INVALID)
            Probe("GUI.paint.getdc_fill_getpixel", "FAIL", "GetDC %p, FillRect=%d, GetPixel(10,10)=CLR_INVALID: %s", (void *)dc, filled,
                  ErrText(GetLastError(), err, sizeof err));
        else
            Probe("GUI.paint.getdc_fill_getpixel", px == COLOR_GETDC ? "OK" : "FAIL", "GetDC %p, FillRect=%d, GetPixel(10,10)=0x%06lX, painted 0x%06lX -> %s",
                  (void *)dc, filled, (unsigned long)px, (unsigned long)COLOR_GETDC, px == COLOR_GETDC ? "match" : "MISMATCH");
        ReleaseDC(g_main, dc);
    }

    /* WM_PAINT / WM_TIMER / a posted message, through a PeekMessage loop of at most 5 s */
    Stage("GUI.paint.loop");
    SetLastError(0);
    timer = SetTimer(g_main, 1, 2000, NULL);
    e_timer = GetLastError();
    InvalidateRect(g_main, NULL, FALSE);
    SetLastError(0);
    posted = PostMessageW(g_main, WM_APP + 1, 0x1234, 0x5678);
    e_post = GetLastError();
    t0 = GetTickCount64();
    deadline = t0 + 5000;
    for (;;) {
        ULONGLONG now;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            peeked++;
            if (msg.message == WM_APP + 1 && msg.hwnd == g_main && msg.wParam == 0x1234)
                peeked_post++;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_wm.paint > 0 && g_wm.timer > 0 && g_wm.posted > 0)
            break;
        now = GetTickCount64();
        if (now >= deadline)
            break;
        MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)(deadline - now), QS_ALLINPUT);
    }
    if (timer)
        KillTimer(g_main, timer);
    {
        char tbuf[300], pbuf[300], pe[300];
        if (timer)
            snprintf(tbuf, sizeof tbuf, "SetTimer(2000 ms) ok");
        else
            snprintf(tbuf, sizeof tbuf, "SetTimer FAILED: %s", ErrText(e_timer, err, sizeof err));
        if (posted)
            snprintf(pbuf, sizeof pbuf, "PostMessage ok");
        else
            snprintf(pbuf, sizeof pbuf, "PostMessage FAILED: %s", ErrText(e_post, err, sizeof err));
        if (g_wm.paint_nodc)
            snprintf(pe, sizeof pe, ", BeginPaint returned NULL %ld time(s): %s", (long)g_wm.paint_nodc, ErrText(g_wm.paint_err, err, sizeof err));
        else
            pe[0] = '\0';
        Probe("GUI.paint.messages", (g_wm.paint > 0 && g_wm.paint_nodc == 0 && g_wm.timer > 0 && g_wm.posted > 0) ? "OK" : "FAIL",
              "after %llu ms: WM_PAINT %s (%ld, rcPaint %ld,%ld-%ld,%ld), WM_TIMER %s (%ld), posted WM_APP+1 %s (%ld, via PeekMessage %ld); "
              "%s, %s, %ld message(s) peeked, WM_ERASEBKGND %ld%s",
              (unsigned long long)(GetTickCount64() - t0), g_wm.paint ? "arrived" : "DID NOT ARRIVE", (long)g_wm.paint,
              g_wm.paint_rc.left, g_wm.paint_rc.top, g_wm.paint_rc.right, g_wm.paint_rc.bottom, g_wm.timer ? "arrived" : "DID NOT ARRIVE",
              (long)g_wm.timer, g_wm.posted ? "arrived" : "DID NOT ARRIVE", (long)g_wm.posted, (long)peeked_post, tbuf, pbuf, (long)peeked,
              (long)g_wm.erase, pe);
    }

    /* PostMessage -> GetMessage round trip; GetMessage is called only once PeekMessage(PM_NOREMOVE) sees the message */
    Stage("GUI.paint.post_getmessage");
    SetLastError(0);
    posted = PostMessageW(g_main, WM_APP + 1, 0xBEEF, 0xCAFE);
    e_post = GetLastError();
    if (!posted) {
        Probe("GUI.paint.post_getmessage", "FAIL", "PostMessageW: %s", ErrText(e_post, err, sizeof err));
    } else if (!PeekMessageW(&msg, g_main, WM_APP + 1, WM_APP + 1, PM_NOREMOVE)) {
        Probe("GUI.paint.post_getmessage", "FAIL", "PostMessageW returned TRUE but PeekMessage(PM_NOREMOVE) does not see the message");
    } else {
        BOOL r;
        memset(&msg, 0, sizeof msg);
        SetLastError(0);
        r = GetMessageW(&msg, g_main, WM_APP + 1, WM_APP + 1);
        if (r > 0 && msg.wParam == 0xBEEF && msg.lParam == 0xCAFE)
            Probe("GUI.paint.post_getmessage", "OK", "PostMessageW -> GetMessageW returned it (wParam 0x%llX, lParam 0x%llX)",
                  (unsigned long long)msg.wParam, (unsigned long long)msg.lParam);
        else
            Probe("GUI.paint.post_getmessage", "FAIL", "GetMessageW returned %d, message 0x%X wParam 0x%llX: %s", (int)r, msg.message,
                  (unsigned long long)msg.wParam, ErrText(GetLastError(), err, sizeof err));
    }
    Stage("GUI.paint.sendmessage");
    SetLastError(0);
    sr = SendMessageW(g_main, WM_APP + 2, 0, 0);
    Probe("GUI.paint.sendmessage", (sr == 0x5A5A && g_wm.sent > 0) ? "OK" : "FAIL", "SendMessageW(WM_APP+2) returned 0x%llX (wndproc saw it %ld time(s)) %s",
          (unsigned long long)sr, (long)g_wm.sent, sr == 0x5A5A ? "" : ErrText(GetLastError(), err, sizeof err));

    /* What the window DC holds after WM_PAINT */
    Stage("GUI.paint.after_wm_paint");
    dc = GetDC(g_main);
    if (dc != NULL) {
        px = GetPixel(dc, 10, 10);
        Probe("GUI.paint.after_wm_paint", px == COLOR_PAINT ? "OK" : (g_wm.paint ? "FAIL" : "INFO"),
              "GetPixel(10,10) after the loop = 0x%06lX (WM_PAINT color 0x%06lX, GetDC color 0x%06lX)%s", (unsigned long)px,
              (unsigned long)COLOR_PAINT, (unsigned long)COLOR_GETDC, px == CLR_INVALID ? " CLR_INVALID" : "");
        ReleaseDC(g_main, dc);
    } else {
        Probe("GUI.paint.after_wm_paint", "FAIL", "GetDC(hwnd): %s", ErrText(GetLastError(), err, sizeof err));
    }
}

static int CALLBACK FontFamCb(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    (void)lf;
    (void)tm;
    (void)type;
    (*(int *)lp)++;
    return 1;
}

static void ProbeMemDc(void)
{
    char why[400], err[260], face[200];
    Dib d;
    int n, r;
    wchar_t wface[LF_FACESIZE];
    TEXTMETRICW tm;
    HGDIOBJ font, oldfont;
    RECT rc = {0, 0, DIB_W, DIB_H};
    DWORD want = ToDib(COLOR_MEM);
    LOGFONTW lf;
    int families = 0;

    Stage("GUI.memdc");
    if (!MakeDib(&d, why, sizeof why)) {
        Probe("GUI.memdc.dib_fill", "FAIL", "%s", why);
        return;
    }
    SetLastError(0);
    r = FillSolid(d.dc, &rc, COLOR_MEM);
    GdiFlush();
    n = CountEq(&d, want);
    Probe("GUI.memdc.dib_fill", n == DIB_W * DIB_H ? "OK" : "FAIL",
          "CreateCompatibleDC(NULL) + CreateDIBSection 64x64x32 + FillRect=%d: %d/%d pixels = 0x%06lX, pixel[0]=0x%08lX", r, n,
          DIB_W * DIB_H, (unsigned long)want, (unsigned long)d.bits[0]);

    font = GetStockObject(DEFAULT_GUI_FONT);
    oldfont = font ? SelectObject(d.dc, font) : NULL;
    wface[0] = L'\0';
    GetTextFaceW(d.dc, LF_FACESIZE, wface);
    memset(&tm, 0, sizeof tm);
    r = GetTextMetricsW(d.dc, &tm);
    Probe("GUI.memdc.font", (font != NULL && r) ? "OK" : "FAIL", "DEFAULT_GUI_FONT %p, face \"%s\", GetTextMetricsW=%d height %ld ave width %ld",
          (void *)font, U8(wface, face, sizeof face), r, (long)tm.tmHeight, (long)tm.tmAveCharWidth);

    SetTextColor(d.dc, RGB(0xFF, 0xFF, 0xFF));
    SetBkMode(d.dc, TRANSPARENT);
    SetLastError(0);
    r = TextOutW(d.dc, 2, 2, L"Wg8#", 4);
    GdiFlush();
    n = DIB_W * DIB_H - CountEq(&d, want);
    Probe("GUI.memdc.textout", (r && n > 0) ? "OK" : "FAIL", "TextOutW=%d, %d pixel(s) changed%s%s", r, n, r ? "" : ": ",
          r ? "" : ErrText(GetLastError(), err, sizeof err));

    FillDib(&d, COLOR_MEM);
    SetLastError(0);
    r = DrawTextW(d.dc, L"Wg8#", -1, &rc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    n = DIB_W * DIB_H - CountEq(&d, want);
    Probe("GUI.memdc.drawtext", (r > 0 && n > 0) ? "OK" : "FAIL", "DrawTextW returned height %d, %d pixel(s) changed%s%s", r, n, r ? "" : ": ",
          r ? "" : ErrText(GetLastError(), err, sizeof err));

    memset(&lf, 0, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(d.dc, &lf, FontFamCb, (LPARAM)&families, 0);
    Probe("GUI.memdc.font_families", families > 0 ? "OK" : "FAIL", "EnumFontFamiliesExW(DEFAULT_CHARSET): %d face(s)", families);

    if (oldfont)
        SelectObject(d.dc, oldfont);
    FreeDib(&d);
}

static void ReadbackResult(const char *id, const char *what, BOOL r, DWORD e, const Dib *d, LONG printclient)
{
    char err[260];
    int paint = CountEq(d, ToDib(COLOR_PAINT)), getdc = CountEq(d, ToDib(COLOR_GETDC)), sent = CountEq(d, ToDib(COLOR_SENTINEL));
    int total = DIB_W * DIB_H;
    Probe(id, (r && paint == total) ? "OK" : "FAIL",
          "%s returned %d%s%s; of %d pixels: %d = WM_PAINT color, %d = GetDC color, %d untouched (sentinel), %d other; pixel[10,10]=0x%06lX; "
          "WM_PRINTCLIENT +%ld",
          what, (int)r, r ? "" : ": ", r ? "" : ErrText(e, err, sizeof err), total, paint, getdc, sent, total - paint - getdc - sent,
          (unsigned long)(d->bits[10 * DIB_W + 10] & 0x00FFFFFF), (long)printclient);
}

static void ProbeReadback(void)
{
    char why[400], err[260];
    Dib d;
    BOOL r;
    DWORD e;
    LONG pc0;
    HDC wdc, sdc;
    POINT org = {0, 0};

    Stage("GUI.readback");
    if (!MakeDib(&d, why, sizeof why)) {
        Probe("GUI.readback.dib", "FAIL", "%s", why);
        return;
    }

    FillDib(&d, COLOR_SENTINEL);
    pc0 = g_wm.printclient;
    Stage("GUI.readback.printwindow_client");
    SetLastError(0);
    r = PrintWindow(g_main, d.dc, PW_CLIENTONLY);
    e = GetLastError();
    GdiFlush();
    ReadbackResult("GUI.readback.printwindow_client", "PrintWindow(PW_CLIENTONLY)", r, e, &d, g_wm.printclient - pc0);

    FillDib(&d, COLOR_SENTINEL);
    pc0 = g_wm.printclient;
    Stage("GUI.readback.printwindow_full");
    SetLastError(0);
    r = PrintWindow(g_main, d.dc, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
    e = GetLastError();
    GdiFlush();
    ReadbackResult("GUI.readback.printwindow_full", "PrintWindow(PW_CLIENTONLY|PW_RENDERFULLCONTENT)", r, e, &d, g_wm.printclient - pc0);

    FillDib(&d, COLOR_SENTINEL);
    Stage("GUI.readback.bitblt_window");
    SetLastError(0);
    wdc = GetDC(g_main);
    if (wdc == NULL) {
        Probe("GUI.readback.bitblt_window", "FAIL", "GetDC(hwnd): %s", ErrText(GetLastError(), err, sizeof err));
    } else {
        SetLastError(0);
        r = BitBlt(d.dc, 0, 0, DIB_W, DIB_H, wdc, 0, 0, SRCCOPY);
        e = GetLastError();
        GdiFlush();
        ReadbackResult("GUI.readback.bitblt_window", "BitBlt(window DC -> DIB)", r, e, &d, 0);
        ReleaseDC(g_main, wdc);
    }

    FillDib(&d, COLOR_SENTINEL);
    Stage("GUI.readback.bitblt_screen");
    ClientToScreen(g_main, &org);
    SetLastError(0);
    sdc = GetDC(NULL);
    if (sdc == NULL) {
        Probe("GUI.readback.bitblt_screen", "FAIL", "GetDC(NULL): %s", ErrText(GetLastError(), err, sizeof err));
    } else {
        char what[120];
        SetLastError(0);
        r = BitBlt(d.dc, 0, 0, DIB_W, DIB_H, sdc, org.x, org.y, SRCCOPY);
        e = GetLastError();
        GdiFlush();
        snprintf(what, sizeof what, "BitBlt(screen DC at client origin %ld,%ld -> DIB)", org.x, org.y);
        ReadbackResult("GUI.readback.bitblt_screen", what, r, e, &d, 0);
        ReleaseDC(NULL, sdc);
    }
    FreeDib(&d);
}

static BOOL CALLBACK MonCb(HMONITOR m, HDC dc, LPRECT rc, LPARAM lp)
{
    (void)m;
    (void)dc;
    (void)rc;
    (*(int *)lp)++;
    return TRUE;
}

static void ProbeSysmetrics(HMODULE user32)
{
    typedef UINT(WINAPI * GetDpiForSystemFn)(void);
    char err[260];
    HDC dc;
    int mons = 0;
    BOOL r;
    GetDpiForSystemFn dpi = user32 ? (GetDpiForSystemFn)(void *)GetProcAddress(user32, "GetDpiForSystem") : NULL;
    int cx, cy;

    Stage("GUI.sysmetrics");
    cx = GetSystemMetrics(SM_CXSCREEN);
    cy = GetSystemMetrics(SM_CYSCREEN);
    Probe("GUI.sysmetrics.screen", (cx > 0 && cy > 0) ? "OK" : "FAIL", "SM_CXSCREEN=%d SM_CYSCREEN=%d SM_CMONITORS=%d SM_REMOTESESSION=%d", cx, cy,
          GetSystemMetrics(SM_CMONITORS), GetSystemMetrics(SM_REMOTESESSION));
    SetLastError(0);
    dc = CreateDCW(L"DISPLAY", NULL, NULL, NULL);
    if (dc == NULL) {
        Probe("GUI.sysmetrics.display_dc", "FAIL", "CreateDCW(DISPLAY): %s", ErrText(GetLastError(), err, sizeof err));
    } else {
        Probe("GUI.sysmetrics.display_dc", "OK", "CreateDCW(DISPLAY): HORZRES=%d VERTRES=%d BITSPIXEL=%d PLANES=%d LOGPIXELSX=%d VREFRESH=%d TECHNOLOGY=%d",
              GetDeviceCaps(dc, HORZRES), GetDeviceCaps(dc, VERTRES), GetDeviceCaps(dc, BITSPIXEL), GetDeviceCaps(dc, PLANES),
              GetDeviceCaps(dc, LOGPIXELSX), GetDeviceCaps(dc, VREFRESH), GetDeviceCaps(dc, TECHNOLOGY));
        DeleteDC(dc);
    }
    SetLastError(0);
    r = EnumDisplayMonitors(NULL, NULL, MonCb, (LPARAM)&mons);
    Probe("GUI.sysmetrics.monitors", (r && mons > 0) ? "OK" : "FAIL", "EnumDisplayMonitors returned %d, %d monitor(s)%s%s", (int)r, mons, r ? "" : ": ",
          r ? "" : ErrText(GetLastError(), err, sizeof err));
    if (dpi != NULL)
        Probe("GUI.sysmetrics.dpi", "OK", "GetDpiForSystem=%u", dpi());
    else
        Probe("GUI.sysmetrics.dpi", "FAIL", "GetDpiForSystem not exported");
}

static void ProbeInput(void)
{
    char err[260], e1[260];
    POINT a = {0, 0}, b = {0, 0};
    BOOL set, got, got2;
    DWORD eset, eget;
    INPUT in;
    UINT sent;
    DWORD esend;

    Stage("GUI.input");
    SetLastError(0);
    set = SetCursorPos(200, 200);
    eset = GetLastError();
    SetLastError(0);
    got = GetCursorPos(&a);
    eget = GetLastError();
    Probe("GUI.input.cursor", (set && got && a.x == 200 && a.y == 200) ? "OK" : "FAIL",
          "SetCursorPos(200,200)=%d%s%s; GetCursorPos=%d -> (%ld,%ld)%s%s", (int)set, set ? "" : " ", set ? "" : ErrText(eset, e1, sizeof e1),
          (int)got, a.x, a.y, got ? "" : " ", got ? "" : ErrText(eget, err, sizeof err));
    Probe("GUI.input.keystate", "INFO", "GetKeyState: VK_SHIFT=0x%04X VK_CAPITAL=0x%04X VK_LBUTTON=0x%04X; GetAsyncKeyState(VK_LBUTTON)=0x%04X",
          (unsigned)(unsigned short)GetKeyState(VK_SHIFT), (unsigned)(unsigned short)GetKeyState(VK_CAPITAL),
          (unsigned)(unsigned short)GetKeyState(VK_LBUTTON), (unsigned)(unsigned short)GetAsyncKeyState(VK_LBUTTON));

    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dx = 7;
    in.mi.dy = 7;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SetLastError(0);
    sent = SendInput(1, &in, sizeof in);
    esend = GetLastError();
    Sleep(200);
    got2 = GetCursorPos(&b);
    Probe("GUI.input.sendinput", sent == 1 ? "OK" : "FAIL", "SendInput(mouse move +7,+7) inserted %u event(s)%s%s; GetCursorPos before (%ld,%ld) after=%d (%ld,%ld)",
          sent, sent == 1 ? "" : ": ", sent == 1 ? "" : ErrText(esend, err, sizeof err), a.x, a.y, (int)got2, b.x, b.y);
}

/* ------------------------------------------------------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    static const wchar_t kClass[] = L"win10xbox.ac_gui_probe";
    char err[260];
    HMODULE user32, gdi32;
    WNDCLASSEXW wc;
    ATOM atom;
    HWND popup = NULL, msgonly = NULL;
    DWORD e;

    (void)argc;
    (void)argv;
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(Unhandled);
    Stage("start");
    CloseHandle(CreateThread(NULL, 0, Watchdog, NULL, 0, NULL));

    Probe("GUI.probe", "INFO", "ac_gui_probe started");
    ProbeContext();
    ProbeDllsLoaded("GUI.dlls.at_start");

    /* Bind user32 and gdi32 explicitly: the failure of a container to connect to win32k is a LoadLibrary error here */
    Stage("GUI.load");
    SetLastError(0);
    user32 = LoadLibraryW(L"user32.dll");
    e = GetLastError();
    if (user32 == NULL)
        Probe("GUI.load.user32", "FAIL", "LoadLibraryW(user32.dll): %s (1114 = ERROR_DLL_INIT_FAILED)", ErrText(e, err, sizeof err));
    else
        Probe("GUI.load.user32", "OK", "user32.dll at %p", (void *)user32);
    SetLastError(0);
    gdi32 = LoadLibraryW(L"gdi32.dll");
    e = GetLastError();
    if (gdi32 == NULL)
        Probe("GUI.load.gdi32", "FAIL", "LoadLibraryW(gdi32.dll): %s", ErrText(e, err, sizeof err));
    else
        Probe("GUI.load.gdi32", "OK", "gdi32.dll at %p", (void *)gdi32);
    if (user32 == NULL || gdi32 == NULL) {
        Probe("GUI.skipped", "INFO", "user32/gdi32 could not be loaded: no further GUI probe can run");
        ProbeDllsLoaded("GUI.dlls.loaded");
        ProbeWin32uExports();
        ProbeSyscallStub("NtUserCreateWindowEx");
        ProbeSyscallStub("NtGdiCreateSolidBrush");
        Summary();
        return 0;
    }

    ProbeWinsta();

    Stage("GUI.register");
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = kClass;
    SetLastError(0);
    atom = RegisterClassExW(&wc);
    if (atom == 0)
        Probe("GUI.register", "FAIL", "RegisterClassExW: %s", ErrText(GetLastError(), err, sizeof err));
    else
        Probe("GUI.register", "OK", "RegisterClassExW atom 0x%04X", (unsigned)atom);

    if (atom != 0) {
        Stage("GUI.create");
        g_main = CreateOne("GUI.create.overlapped", kClass, WS_OVERLAPPEDWINDOW, 50, 50, 400, 300, NULL);
        popup = CreateOne("GUI.create.popup", kClass, WS_POPUP, 500, 50, 200, 150, NULL);
        msgonly = CreateOne("GUI.create.message_only", kClass, 0, 0, 0, 0, 0, HWND_MESSAGE);
    }

    if (g_main != NULL) {
        BOOL was, upd, vis;
        RECT wr, cr;
        DWORD eupd;
        HWND fg;

        Stage("GUI.show");
        SetLastError(0);
        was = ShowWindow(g_main, SW_SHOW);
        SetLastError(0);
        upd = UpdateWindow(g_main);
        eupd = GetLastError();
        vis = IsWindowVisible(g_main);
        fg = GetForegroundWindow();
        Probe("GUI.show.showwindow", (upd && vis) ? "OK" : "FAIL",
              "ShowWindow(SW_SHOW) previously-visible=%d, UpdateWindow=%d%s%s, IsWindowVisible=%d, IsIconic=%d, GetForegroundWindow=%p (%s), WM_PAINT so far %ld",
              (int)was, (int)upd, upd ? "" : " ", upd ? "" : ErrText(eupd, err, sizeof err), (int)vis, (int)IsIconic(g_main), (void *)fg,
              fg == g_main ? "this window" : "another window", (long)g_wm.paint);
        memset(&wr, 0, sizeof wr);
        memset(&cr, 0, sizeof cr);
        {
            BOOL a = GetWindowRect(g_main, &wr), b = GetClientRect(g_main, &cr);
            Probe("GUI.show.rects", (a && b) ? "OK" : "FAIL", "GetWindowRect=%d (%ld,%ld)-(%ld,%ld) GetClientRect=%d (%ld,%ld)-(%ld,%ld)", (int)a, wr.left,
                  wr.top, wr.right, wr.bottom, (int)b, cr.left, cr.top, cr.right, cr.bottom);
        }
        ProbePaint();
    } else {
        Probe("GUI.show.showwindow", "INFO", "not run: no window");
        Probe("GUI.paint.messages", "INFO", "not run: no window");
    }

    ProbeMemDc();
    if (g_main != NULL)
        ProbeReadback();
    else
        Probe("GUI.readback.printwindow_client", "INFO", "not run: no window");
    ProbeSysmetrics(user32);
    ProbeInput();

    Stage("GUI.dlls");
    ProbeDllsLoaded("GUI.dlls.loaded");
    ProbeWin32uExports();
    ProbeSyscallStub("NtUserCreateWindowEx");
    ProbeSyscallStub("NtGdiCreateSolidBrush");

    Stage("GUI.destroy");
    {
        char line[600];
        size_t used = 0;
        HWND all[3];
        const char *names[3] = {"overlapped", "popup", "message_only"};
        int i, bad = 0;
        all[0] = g_main;
        all[1] = popup;
        all[2] = msgonly;
        line[0] = '\0';
        for (i = 0; i < 3; i++) {
            BOOL r;
            if (all[i] == NULL)
                continue;
            SetLastError(0);
            r = DestroyWindow(all[i]);
            if (!r)
                bad++;
            used += (size_t)snprintf(line + used, sizeof line - used, "%s%s=%d%s%s", used ? ", " : "", names[i], (int)r, r ? "" : " ",
                                     r ? "" : ErrText(GetLastError(), err, sizeof err));
        }
        if (used == 0)
            Probe("GUI.destroy.windows", "INFO", "no window to destroy");
        else
            Probe("GUI.destroy.windows", bad ? "FAIL" : "OK", "DestroyWindow: %s", line);
        g_main = NULL;
    }
    if (atom != 0) {
        BOOL r;
        SetLastError(0);
        r = UnregisterClassW(kClass, GetModuleHandleW(NULL));
        Probe("GUI.destroy.class", r ? "OK" : "FAIL", "UnregisterClassW=%d%s%s", (int)r, r ? "" : " ", r ? "" : ErrText(GetLastError(), err, sizeof err));
    }
    Summary();
    return 0;
}
