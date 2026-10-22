/* guest_window_nc: the non-client area, end to end. A GUI guest creates a WS_OVERLAPPEDWINDOW at (100,100),
 * 400x300, and checks:
 *   1. frame geometry: the client rectangle is the window rectangle minus the frame AdjustWindowRectEx reports,
 *      and the client lies inside the window;
 *   2. WM_NCHITTEST on the caption, the close button, the client area and the borders;
 *   3. a caption drag (the host injects WIN10XBOX_TEST_INJECT_INPUT: press on the caption at screen (300,115),
 *      move by +50,+40, release): the window must end up at (150,140) with its size unchanged;
 *   4. a click on the close button (the window is then at 150,140, so its close button is near screen (535,155)):
 *      WM_SYSCOMMAND/SC_CLOSE -> WM_CLOSE -> DefWindowProc -> DestroyWindow -> WM_DESTROY -> PostQuitMessage.
 * Exits 42 when all of it happened, 4 when a check failed, 3 when the 10 s watchdog fired first.
 *
 * Output lines (the integration test matches them):
 *   window_nc: created window=100,100,500,400 client=<w>x<h>
 *   window_nc: frame ok
 *   window_nc: hit caption=2 close=20 client=1 top=12 topleft=13 left=10
 *   window_nc: dragged to 150,140 size=400x300
 *   window_nc: WM_SYSCOMMAND SC_CLOSE
 *   window_nc: WM_CLOSE
 *   window_nc: WM_DESTROY
 *   window_nc: done
 */
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <string.h>

#define TIMER_WATCHDOG 1
#define WATCHDOG_MS    10000

static int g_failed;
static int g_close_seen;
static int g_destroy_seen;
static int g_drag_seen;
static int g_timeout;

static void check(int ok, const char* what) {
    if (!ok) {
        printf("window_nc: CHECK FAILED: %s\n", what);
        fflush(stdout);
        g_failed = 1;
    }
}

static int hit(HWND hwnd, int x, int y) {
    return (int)SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(x, y));
}

static void check_frame(HWND hwnd) {
    RECT wr, cr, adj;
    POINT origin = {0, 0};
    int hc, hclose, hclient, htop, htl, hleft;

    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    printf("window_nc: created window=%ld,%ld,%ld,%ld client=%ldx%ld\n", (long)wr.left, (long)wr.top, (long)wr.right,
           (long)wr.bottom, (long)(cr.right - cr.left), (long)(cr.bottom - cr.top));
    fflush(stdout);
    check(wr.left == 100 && wr.top == 100 && wr.right == 500 && wr.bottom == 400, "window rect is 100,100,500,400");

    ClientToScreen(hwnd, &origin);
    adj = cr;
    AdjustWindowRectEx(&adj, WS_OVERLAPPEDWINDOW, FALSE, 0);
    check(cr.left == 0 && cr.top == 0, "client rect starts at 0,0");
    check(origin.x > wr.left && origin.y > wr.top, "client origin is inside the frame");
    check(origin.y - wr.top > origin.x - wr.left, "caption makes the top frame thicker than the side frame");
    check(cr.right > 0 && cr.bottom > 0 && origin.x + cr.right < wr.right && origin.y + cr.bottom < wr.bottom,
          "client is inside the window rect");
    check((adj.right - adj.left) == (wr.right - wr.left) && (adj.bottom - adj.top) == (wr.bottom - wr.top),
          "window size == client size + AdjustWindowRectEx frame");
    check(origin.x - wr.left == -adj.left && origin.y - wr.top == -adj.top, "client origin == window origin + frame");
    if (!g_failed) {
        printf("window_nc: frame ok\n");
        fflush(stdout);
    }

    hc = hit(hwnd, 300, 115);
    hclose = hit(hwnd, 485, 115);
    hclient = hit(hwnd, 300, 250);
    htop = hit(hwnd, 300, 101);
    htl = hit(hwnd, 101, 101);
    hleft = hit(hwnd, 101, 250);
    printf("window_nc: hit caption=%d close=%d client=%d top=%d topleft=%d left=%d\n", hc, hclose, hclient, htop, htl,
           hleft);
    fflush(stdout);
    check(hc == HTCAPTION, "caption hit test");
    check(hclose == HTCLOSE, "close button hit test");
    check(hclient == HTCLIENT, "client hit test");
    check(htop == HTTOP, "top border hit test");
    check(htl == HTTOPLEFT, "top-left corner hit test");
    check(hleft == HTLEFT, "left border hit test");
    check(hit(hwnd, 50, 50) == HTNOWHERE, "outside hit test");
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
    case WM_EXITSIZEMOVE: {
        RECT wr;
        GetWindowRect(hwnd, &wr);
        printf("window_nc: dragged to %ld,%ld size=%ldx%ld\n", (long)wr.left, (long)wr.top, (long)(wr.right - wr.left),
               (long)(wr.bottom - wr.top));
        fflush(stdout);
        check(wr.left == 150 && wr.top == 140, "drag moved the window by exactly +50,+40");
        check(wr.right - wr.left == 400 && wr.bottom - wr.top == 300, "drag kept the window size");
        g_drag_seen = 1;
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) {
            printf("window_nc: WM_SYSCOMMAND SC_CLOSE\n");
            fflush(stdout);
            check(g_drag_seen, "close came after the drag");
        }
        break;
    case WM_CLOSE:
        printf("window_nc: WM_CLOSE\n");
        fflush(stdout);
        g_close_seen = 1;
        break;   /* DefWindowProc destroys the window */
    case WM_DESTROY:
        printf("window_nc: WM_DESTROY\n");
        fflush(stdout);
        g_destroy_seen = 1;
        KillTimer(hwnd, TIMER_WATCHDOG);
        PostQuitMessage(0);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_WATCHDOG) {
            printf("window_nc: timeout drag=%d close=%d destroy=%d\n", g_drag_seen, g_close_seen, g_destroy_seen);
            fflush(stdout);
            g_timeout = 1;
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(void) {
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"W10XWindowNc";
    wc.hbrBackground = NULL;
    if (!RegisterClassW(&wc)) {
        printf("window_nc: RegisterClassW failed (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    hwnd = CreateWindowExW(0, L"W10XWindowNc", L"win10xbox nc", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 400, 300,
                           NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        printf("window_nc: CreateWindowExW failed (%lu)\n", (unsigned long)GetLastError());
        return 2;
    }
    check_frame(hwnd);
    SetTimer(hwnd, TIMER_WATCHDOG, WATCHDOG_MS, NULL);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    printf("window_nc: done\n");
    fflush(stdout);
    if (g_timeout) return 3;
    if (g_failed) return 4;
    return (g_drag_seen && g_close_seen && g_destroy_seen) ? 42 : 5;
}
