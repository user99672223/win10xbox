/* guest_window2: what an ordinary small GUI program does, after guest_window's
 * frameless first pixels: an overlapped (captioned, sizable) window sized with
 * AdjustWindowRectEx, its title set and read back, ShowWindow + UpdateWindow,
 * double-buffered painting (memory DC + CreateCompatibleBitmap + BitBlt), a
 * WM_TIMER that invalidates and later destroys the window, a PeekMessage loop
 * driven by MsgWaitForMultipleObjectsEx, and the WM_DESTROY / WM_NCDESTROY
 * pair. It checks everything itself and exits 42 only when all of it held.
 *
 * Output lines (the integration test matches them):
 *   window2: client=320x200
 *   window2: text=win10xbox two
 *   window2: pixel(5,5)=0000ff00 pixel(20,20)=0000ffff   (COLORREF, 0x00BBGGRR)
 *   window2: timer=2 destroy=1 ncdestroy=1 iswindow=0
 *   window2: done
 */
#include <windows.h>
#include <stdio.h>

#define GREEN  RGB(0, 255, 0)
#define YELLOW RGB(255, 255, 0)

static int g_paints, g_timer_ticks, g_destroy, g_ncdestroy, g_created, g_sized;

static void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc, sq = {10, 10, 60, 60};
    HDC mem;
    HBITMAP bmp, old;
    HBRUSH green, yellow;

    GetClientRect(hwnd, &rc);
    /* the classic flicker-free paint: draw into a bitmap, then one BitBlt */
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    old = (HBITMAP)SelectObject(mem, bmp);
    green = CreateSolidBrush(GREEN);
    yellow = CreateSolidBrush(YELLOW);
    FillRect(mem, &rc, green);
    FillRect(mem, &sq, yellow);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteObject(green);
    DeleteObject(yellow);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
    g_paints++;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_created++;
        return 0;
    case WM_SIZE:
        g_sized++;
        return 0;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_TIMER:
        if (++g_timer_ticks == 1) {
            InvalidateRect(hwnd, NULL, FALSE);   /* a second paint */
        } else {
            KillTimer(hwnd, 7);
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        g_destroy++;
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        g_ncdestroy++;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(void) {
    WNDCLASSEXW wc;
    HWND hwnd;
    RECT r = {0, 0, 320, 200}, cr, wr;
    WCHAR text[64];
    HDC dc;
    COLORREF a, b;
    int ok = 1, quit = 0;
    MSG msg;

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"W10XWindow2";
    if (!RegisterClassExW(&wc)) {
        printf("window2: RegisterClassExW failed (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    hwnd = CreateWindowExW(0, L"W10XWindow2", L"first title", WS_OVERLAPPEDWINDOW, 50, 60, r.right - r.left,
                           r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        printf("window2: CreateWindowExW failed (%lu)\n", (unsigned long)GetLastError());
        return 2;
    }
    GetClientRect(hwnd, &cr);
    GetWindowRect(hwnd, &wr);
    printf("window2: client=%ldx%ld\n", (long)cr.right, (long)cr.bottom);
    printf("window2: window=(%ld,%ld)-(%ld,%ld) created=%d\n", (long)wr.left, (long)wr.top, (long)wr.right,
           (long)wr.bottom, g_created);
    if (cr.right != 320 || cr.bottom != 200 || wr.left != 50 || wr.top != 60 || g_created != 1) ok = 0;

    SetWindowTextW(hwnd, L"win10xbox two");
    GetWindowTextW(hwnd, text, 64);
    printf("window2: text=%ls\n", text);
    if (lstrcmpW(text, L"win10xbox two")) ok = 0;

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    if (g_paints < 1 || !g_sized) {
        printf("window2: no paint after UpdateWindow (paints=%d sized=%d)\n", g_paints, g_sized);
        ok = 0;
    }
    dc = GetDC(hwnd);
    a = GetPixel(dc, 5, 5);
    b = GetPixel(dc, 20, 20);
    ReleaseDC(hwnd, dc);
    printf("window2: pixel(5,5)=%08lx pixel(20,20)=%08lx\n", (unsigned long)a, (unsigned long)b);
    if (a != GREEN || b != YELLOW) ok = 0;
    printf("window2: screen=%dx%d window colour=%06lx\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
           (unsigned long)GetSysColor(COLOR_WINDOW));
    if (GetSystemMetrics(SM_CXSCREEN) <= 0 || GetSystemMetrics(SM_CYCAPTION) <= 0) ok = 0;
    fflush(stdout);

    SetTimer(hwnd, 7, 50, NULL);
    while (!quit) {
        DWORD w = MsgWaitForMultipleObjectsEx(0, NULL, 5000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (w == WAIT_TIMEOUT) {
            printf("window2: MsgWaitForMultipleObjectsEx timed out (timer=%d)\n", g_timer_ticks);
            ok = 0;
            break;
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = 1;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    printf("window2: timer=%d destroy=%d ncdestroy=%d iswindow=%d paints=%d\n", g_timer_ticks, g_destroy, g_ncdestroy,
           IsWindow(hwnd) ? 1 : 0, g_paints);
    if (g_timer_ticks != 2 || g_destroy != 1 || g_ncdestroy != 1 || IsWindow(hwnd) || g_paints < 2) ok = 0;
    printf("window2: done\n");
    fflush(stdout);
    return ok ? 42 : 5;
}
