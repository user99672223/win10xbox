/* guest_window_text: text through the GUI stack. A real Windows program that draws
 * text with gdi32 (TextOutW, DrawTextW, the stock SYSTEM_FONT) into its window
 * through user32/gdi32 (Wine's PE builtins) -> win32u syscalls -> core/user, and
 * proves the glyph pixels by reading them back with GetPixel.
 *
 * The text contract it checks: an 8x16 glyph cell per character, the first
 * cell's top-left at the TextOut position. For 'H' (first cell) black pixels
 * sit at cell-relative (1,3) and (6,3) and white at (3,3); 'I' (second cell,
 * x offset 8) has black at (3,3).
 *
 * What it does, in order, printing a line for each step so a failure is
 * attributable from stdout alone:
 *   1. RegisterClassW
 *   2. CreateWindowExW (WS_POPUP: no frame, 400x300 at 100,100), WS_VISIBLE
 *   3. WM_PAINT: FillRect(client, white) ; SetBkMode TRANSPARENT ; SetTextColor black ;
 *      TextOutW(20,20,"HI") with SYSTEM_FONT ; DrawTextW("Hello\nXbox") into (20,60)-(380,120)
 *   4. after the first paint: GetDC + GetPixel at the client-relative glyph pixels
 *      (21,23) H(1,3) black, (26,23) H(6,3) black, (23,23) H(3,3) white,
 *      (31,23) I(3,3) black, and (200,200) background white
 *   5. PostQuitMessage, message loop ends, exit 42 only if all five hold (else 4)
 * A 5 s SetTimer is the watchdog: if WM_PAINT never arrives the program still
 * exits (with 3) instead of hanging the test.
 *
 * Optional argument: the number of seconds to stay on screen after the paint
 * (default 0: quit right after the pixel check, the CI case). The window is
 * never destroyed: the process exit takes it down.
 *
 * Output lines (the integration test matches them):
 *   text: class registered
 *   text: created hwnd=... client=400x300
 *   text: painted
 *   text: pixels H(1,3)=00000000 H(6,3)=00000000 H(3,3)=00ffffff I(3,3)=00000000 bg=00ffffff   (COLORREF, 0x00BBGGRR)
 *   text: done
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLACK RGB(0, 0, 0)
#define WHITE RGB(255, 255, 255)

static int g_painted;
static int g_result = 3;   /* 3 = watchdog fired before the paint */
static int g_stay_seconds;  /* argv[1] */

#define TIMER_WATCHDOG 1
#define TIMER_STAY     2

static void check_pixels(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    COLORREF h13 = GetPixel(dc, 21, 23);
    COLORREF h63 = GetPixel(dc, 26, 23);
    COLORREF h33 = GetPixel(dc, 23, 23);
    COLORREF i33 = GetPixel(dc, 31, 23);
    COLORREF bg = GetPixel(dc, 200, 200);
    ReleaseDC(hwnd, dc);
    printf("text: pixels H(1,3)=%08lx H(6,3)=%08lx H(3,3)=%08lx I(3,3)=%08lx bg=%08lx\n",
           (unsigned long)h13, (unsigned long)h63, (unsigned long)h33, (unsigned long)i33, (unsigned long)bg);
    g_result = (h13 == BLACK && h63 == BLACK && h33 == WHITE && i33 == BLACK && bg == WHITE) ? 42 : 4;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH white = CreateSolidBrush(WHITE);
        FillRect(dc, &rc, white);
        DeleteObject(white);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, BLACK);
        HGDIOBJ old_font = SelectObject(dc, GetStockObject(SYSTEM_FONT));
        TextOutW(dc, 20, 20, L"HI", 2);
        RECT rc2 = {20, 60, 380, 120};
        DrawTextW(dc, L"Hello\nXbox", -1, &rc2, DT_LEFT | DT_TOP);
        SelectObject(dc, old_font);
        EndPaint(hwnd, &ps);
        if (!g_painted) {
            g_painted = 1;
            printf("text: painted\n");
            fflush(stdout);
            check_pixels(hwnd);
            KillTimer(hwnd, TIMER_WATCHDOG);
            if (g_stay_seconds > 0) SetTimer(hwnd, TIMER_STAY, (UINT)g_stay_seconds * 1000, NULL);
            else PostMessageW(hwnd, WM_USER + 1, 0, 0);
        }
        return 0;
    }
    case WM_USER + 1:
    case WM_TIMER:
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(int argc, char** argv) {
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;
    RECT rc;

    if (argc > 1) g_stay_seconds = atoi(argv[1]);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"W10XWindowText";
    wc.hbrBackground = NULL;
    if (!RegisterClassW(&wc)) {
        printf("text: RegisterClassW failed (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    printf("text: class registered\n");
    fflush(stdout);

    hwnd = CreateWindowExW(0, L"W10XWindowText", L"win10xbox text", WS_POPUP | WS_VISIBLE, 100, 100, 400, 300, NULL,
                           NULL, wc.hInstance, NULL);
    if (!hwnd) {
        printf("text: CreateWindowExW failed (%lu)\n", (unsigned long)GetLastError());
        return 2;
    }
    GetClientRect(hwnd, &rc);
    printf("text: created hwnd=%p client=%ldx%ld\n", (void*)hwnd, (long)(rc.right - rc.left), (long)(rc.bottom - rc.top));
    fflush(stdout);
    SetTimer(hwnd, TIMER_WATCHDOG, 5000, NULL);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    printf("text: done\n");
    fflush(stdout);
    return g_result;
}
