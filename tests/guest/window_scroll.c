/* guest_window_scroll: scroll bars end to end. A 300x200 WS_POPUP|WS_VSCROLL|WS_HSCROLL window at (100,100)
 * (client 283x183, vertical bar at screen x 383..400, horizontal bar at screen y 283..300).
 *   1. SetScrollInfo / SetScrollRange / SetScrollPos, read back with GetScrollInfo / GetScrollRange /
 *      GetScrollPos / GetScrollBarInfo.
 *   2. WM_NCHITTEST on the vertical bar's down arrow gives HTVSCROLL.
 *   3. ScrollWindowEx and ScrollDC move client pixels (verified with GetPixel).
 *   4. The host clicks the down arrow (WIN10XBOX_TEST_INJECT_INPUT): the window gets WM_VSCROLL SB_LINEDOWN, then
 *      SB_ENDSCROLL; the handler moves the position by one.
 * Exits 42 when everything held; an 8 s watchdog exits 3.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define TIMER_WATCHDOG 1

static int g_linedown;
static int g_endscroll;
static int g_result = 3;

static void finish(HWND hwnd, int code) {
    g_result = code;
    KillTimer(hwnd, TIMER_WATCHDOG);
    PostQuitMessage(0);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        HBRUSH grey = CreateSolidBrush(RGB(64, 64, 64));
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, grey);
        DeleteObject(grey);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_VSCROLL: {
        int code = LOWORD(wp);
        printf("scroll: WM_VSCROLL code=%d\n", code);
        fflush(stdout);
        if (code == SB_LINEDOWN) {
            g_linedown++;
            SetScrollPos(hwnd, SB_VERT, GetScrollPos(hwnd, SB_VERT) + 1, TRUE);
        } else if (code == SB_ENDSCROLL) {
            int pos = GetScrollPos(hwnd, SB_VERT);
            g_endscroll++;
            printf("scroll: after click linedown=%d endscroll=%d pos=%d\n", g_linedown, g_endscroll, pos);
            fflush(stdout);
            finish(hwnd, (g_linedown == 1 && pos == 21) ? 42 : 4);
        }
        return 0;
    }
    case WM_TIMER:
        printf("scroll: timeout (linedown=%d endscroll=%d)\n", g_linedown, g_endscroll);
        fflush(stdout);
        finish(hwnd, 3);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(void) {
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;
    RECT rc, upd;
    SCROLLINFO si;
    SCROLLBARINFO sbi;
    HDC dc;
    HBRUSH red;
    COLORREF before, after, after2;
    int min, max;
    LRESULT ht;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"W10XWindowScroll";
    if (!RegisterClassW(&wc)) {
        printf("scroll: RegisterClassW failed (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    hwnd = CreateWindowExW(0, L"W10XWindowScroll", L"win10xbox scroll", WS_POPUP | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL,
                           100, 100, 300, 200, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        printf("scroll: CreateWindowExW failed (%lu)\n", (unsigned long)GetLastError());
        return 2;
    }
    GetClientRect(hwnd, &rc);
    printf("scroll: client=%ldx%ld\n", (long)(rc.right - rc.left), (long)(rc.bottom - rc.top));
    UpdateWindow(hwnd);

    /* 1. state */
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = 100;
    si.nPage = 10;
    si.nPos = 20;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_ALL;
    GetScrollInfo(hwnd, SB_VERT, &si);
    printf("scroll: v min=%d max=%d page=%u pos=%d\n", si.nMin, si.nMax, si.nPage, si.nPos);
    SetScrollRange(hwnd, SB_HORZ, 0, 50, TRUE);
    SetScrollPos(hwnd, SB_HORZ, 5, TRUE);
    min = max = -1;
    GetScrollRange(hwnd, SB_HORZ, &min, &max);
    printf("scroll: h min=%d max=%d pos=%d\n", min, max, GetScrollPos(hwnd, SB_HORZ));
    memset(&sbi, 0, sizeof sbi);
    sbi.cbSize = sizeof sbi;
    GetScrollBarInfo(hwnd, OBJID_VSCROLL, &sbi);
    printf("scroll: vbar rect=%ld,%ld,%ld,%ld state=%lu\n", (long)sbi.rcScrollBar.left, (long)sbi.rcScrollBar.top,
           (long)sbi.rcScrollBar.right, (long)sbi.rcScrollBar.bottom, (unsigned long)sbi.rgstate[0]);

    /* 2. hit test */
    ht = SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(391, 275));
    printf("scroll: hittest down-arrow=%ld hbar=%ld client=%ld\n", (long)ht,
           (long)SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(200, 291)),
           (long)SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(200, 200)));

    /* 3. scrolling pixels */
    dc = GetDC(hwnd);
    red = CreateSolidBrush(RGB(255, 0, 0));
    rc.left = 0; rc.top = 0; rc.right = 100; rc.bottom = 20;
    FillRect(dc, &rc, red);
    before = GetPixel(dc, 10, 40);
    memset(&upd, 0, sizeof upd);
    ReleaseDC(hwnd, dc);
    ScrollWindowEx(hwnd, 0, 30, NULL, NULL, NULL, &upd, SW_INVALIDATE);
    dc = GetDC(hwnd);
    after = GetPixel(dc, 10, 40);
    printf("scroll: ScrollWindowEx before=%06lx after=%06lx upd=%ld,%ld,%ld,%ld\n", (unsigned long)before,
           (unsigned long)after, (long)upd.left, (long)upd.top, (long)upd.right, (long)upd.bottom);
    rc.left = 0; rc.top = 0; rc.right = 283; rc.bottom = 183;
    ScrollDC(dc, 0, 10, &rc, &rc, NULL, &upd);
    after2 = GetPixel(dc, 10, 50);
    printf("scroll: ScrollDC pixel=%06lx upd=%ld,%ld,%ld,%ld\n", (unsigned long)after2, (long)upd.left, (long)upd.top,
           (long)upd.right, (long)upd.bottom);
    ReleaseDC(hwnd, dc);
    DeleteObject(red);
    UpdateWindow(hwnd);   /* repaint what ScrollWindowEx invalidated */
    fflush(stdout);

    /* 4. the host clicks the down arrow */
    SetTimer(hwnd, TIMER_WATCHDOG, 8000, NULL);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    printf("scroll: done\n");
    fflush(stdout);
    return g_result;
}
