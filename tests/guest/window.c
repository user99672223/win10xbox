/* guest_window: the first GUI guest. A real Windows program that puts a solid
 * colour on the screen through user32/gdi32 (Wine's PE builtins) -> win32u
 * syscalls -> core/user, and proves it by reading its own pixels back.
 *
 * What it does, in order, printing a line for each step so a failure is
 * attributable from stdout alone:
 *   1. RegisterClassW
 *   2. CreateWindowExW (WS_POPUP: no frame, 400x300 at 100,100), WS_VISIBLE
 *   3. WM_PAINT: FillRect(client, solid RGB(0,80,220)) ; a red 50x50 square at (20,20)
 *   4. after the first paint: GetDC + GetPixel at (10,10) -> must be the blue, at (30,30) -> the red
 *   5. PostQuitMessage, message loop ends, exit 42
 * A 5 s SetTimer is the watchdog: if WM_PAINT never arrives the program still
 * exits (with 3) instead of hanging the test.
 *
 * Optional argument: the number of seconds to stay on screen after the paint
 * (default 0: quit right after the pixel check, the CI case). The console's
 * launch.json passes e.g. 30 so the window can be seen on the TV. The window is
 * never destroyed: the process exit takes it down, and a host that keeps
 * presenting after the guest has exited (host-uwp, exit_when_done false)
 * keeps showing it.
 *
 * Output lines (the integration test matches them):
 *   window: class registered
 *   window: created hwnd=... client=400x300
 *   window: painted
 *   window: pixel(10,10)=00dc5000 pixel(30,30)=000000ff   (COLORREF, 0x00BBGGRR)
 *   window: done
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define BLUE RGB(0, 80, 220)
#define RED  RGB(255, 0, 0)

static int g_painted;
static int g_result = 3;   /* 3 = watchdog fired before the paint */
static int g_stay_seconds;  /* argv[1] */

#define TIMER_WATCHDOG 1
#define TIMER_STAY     2

static void check_pixels(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    COLORREF a = GetPixel(dc, 10, 10);
    COLORREF b = GetPixel(dc, 30, 30);
    ReleaseDC(hwnd, dc);
    printf("window: pixel(10,10)=%08lx pixel(30,30)=%08lx\n", (unsigned long)a, (unsigned long)b);
    g_result = (a == BLUE && b == RED) ? 42 : 4;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH blue = CreateSolidBrush(BLUE);
        HBRUSH red = CreateSolidBrush(RED);
        FillRect(dc, &rc, blue);
        RECT sq = {20, 20, 70, 70};
        FillRect(dc, &sq, red);
        DeleteObject(blue);
        DeleteObject(red);
        EndPaint(hwnd, &ps);
        if (!g_painted) {
            g_painted = 1;
            printf("window: painted\n");
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
    wc.lpszClassName = L"W10XWindow";
    wc.hbrBackground = NULL;
    if (!RegisterClassW(&wc)) {
        printf("window: RegisterClassW failed (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    printf("window: class registered\n");
    fflush(stdout);

    hwnd = CreateWindowExW(0, L"W10XWindow", L"win10xbox", WS_POPUP | WS_VISIBLE, 100, 100, 400, 300, NULL, NULL,
                           wc.hInstance, NULL);
    if (!hwnd) {
        printf("window: CreateWindowExW failed (%lu)\n", (unsigned long)GetLastError());
        return 2;
    }
    GetClientRect(hwnd, &rc);
    printf("window: created hwnd=%p client=%ldx%ld\n", (void*)hwnd, (long)(rc.right - rc.left), (long)(rc.bottom - rc.top));
    fflush(stdout);
    SetTimer(hwnd, TIMER_WATCHDOG, 5000, NULL);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    printf("window: done\n");
    fflush(stdout);
    return g_result;
}
