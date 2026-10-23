/* guest_window_nc: the non-client area, end to end. A GUI guest creates a WS_OVERLAPPEDWINDOW at (100,100),
 * 400x300, and checks:
 *   1. frame geometry: the client rectangle is the window rectangle minus the frame AdjustWindowRectEx reports,
 *      and the client lies inside the window;
 *   2. WM_NCHITTEST on the caption, the three system buttons, the client area and the borders;
 *   3. a caption drag (the host injects WIN10XBOX_TEST_INJECT_INPUT: press on the caption at screen (300,115),
 *      move by +50,+40, release): the window must end up at (150,140) with its size unchanged;
 *   4. a border drag: press on the bottom-right corner (548,438), move by +30,+20, release: the window keeps its
 *      origin and grows to 430x320 (WM_SIZE reports the new client size);
 *   5. what the frame looks like after that (read back with GetWindowDC/GetPixel; the host screenshot checks the same
 *      pixels): active caption colour, border colour, caption text pixels, the three button colours, the client colour;
 *   6. a click on the close button (the window is then 150,140-580,460, its close button near screen (555,155)):
 *      WM_SYSCOMMAND/SC_CLOSE -> WM_CLOSE -> DefWindowProc -> DestroyWindow -> WM_DESTROY -> PostQuitMessage.
 * Exits 42 when all of it happened, 4 when a check failed, 3 when the 30 s watchdog fired first.
 *
 * Output lines (the integration test matches them):
 *   window_nc: created window=100,100,500,400 client=<w>x<h>
 *   window_nc: frame ok
 *   window_nc: hit caption=2 close=20 max=9 min=8 client=1 top=12 topleft=13 left=10 bottomright=17
 *   window_nc: dragged to 150,140 size=400x300
 *   window_nc: resized to 150,140 size=430x320
 *   window_nc: pixels ok
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
#define WATCHDOG_MS    30000

static int g_failed;
static int g_close_seen;
static int g_destroy_seen;
static int g_drag_seen;
static int g_size_seen;
static int g_exits;
static int g_close_count;
static LONG g_client_w0, g_client_h0;
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
    int hc, hclose, hmax, hmin, hclient, htop, htl, hleft, hbr;

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
    g_client_w0 = cr.right;
    g_client_h0 = cr.bottom;
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
    hmax = hit(hwnd, 437, 115);
    hmin = hit(hwnd, 400, 115);
    hclient = hit(hwnd, 300, 250);
    htop = hit(hwnd, 300, 101);
    htl = hit(hwnd, 101, 101);
    hleft = hit(hwnd, 101, 250);
    hbr = hit(hwnd, 498, 398);
    printf("window_nc: hit caption=%d close=%d max=%d min=%d client=%d top=%d topleft=%d left=%d bottomright=%d\n", hc,
           hclose, hmax, hmin, hclient, htop, htl, hleft, hbr);
    fflush(stdout);
    check(hc == HTCAPTION, "caption hit test");
    check(hclose == HTCLOSE, "close button hit test");
    check(hmax == HTMAXBUTTON, "maximize button hit test");
    check(hmin == HTMINBUTTON, "minimize button hit test");
    check(hclient == HTCLIENT, "client hit test");
    check(htop == HTTOP, "top border hit test");
    check(htl == HTTOPLEFT, "top-left corner hit test");
    check(hleft == HTLEFT, "left border hit test");
    check(hbr == HTBOTTOMRIGHT, "bottom-right corner hit test");
    check(hit(hwnd, 50, 50) == HTNOWHERE, "outside hit test");
}

static void check_pixel(HDC dc, int x, int y, COLORREF want, const char* what) {
    COLORREF got = GetPixel(dc, x, y);
    if (got != want) {
        printf("window_nc: pixel(%d,%d)=%06lx, wanted %06lx\n", x, y, (unsigned long)got, (unsigned long)want);
        fflush(stdout);
    }
    check(got == want, what);
}

/* The frame after the resize, read back through the window DC (window coordinates): the window is at 150,140 and
 * 430x320, so the caption is rows 8..29, the close button columns 386..421 (minimize 314.., maximize 350..). */
static void check_pixels(HWND hwnd) {
    HDC wdc = GetWindowDC(hwnd);
    HDC cdc = GetDC(hwnd);
    int x, y, text = 0;

    check_pixel(wdc, 270, 12, GetSysColor(COLOR_ACTIVECAPTION), "caption pixel is the active caption colour");
    check_pixel(wdc, 3, 160, GetSysColor(COLOR_ACTIVEBORDER), "left border pixel is the border colour");
    check_pixel(wdc, 388, 10, RGB(199, 80, 80), "close button pixel");
    check_pixel(wdc, 352, 10, RGB(200, 216, 240), "maximize button pixel");
    check_pixel(wdc, 316, 10, RGB(225, 225, 225), "minimize button pixel");
    check_pixel(cdc, 150, 150, RGB(64, 64, 64), "client pixel is the client colour");
    for (y = 11; y < 27; y++)
        for (x = 14; x < 112; x++)
            if (GetPixel(wdc, x, y) == RGB(0, 0, 0)) text++;
    check(text > 20, "the caption shows its text");
    ReleaseDC(hwnd, cdc);
    ReleaseDC(hwnd, wdc);
    if (!g_failed) {
        printf("window_nc: pixels ok\n");
        fflush(stdout);
    }
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
        RECT wr, cr;
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);
        g_exits++;
        if (g_exits == 1) {
            printf("window_nc: dragged to %ld,%ld size=%ldx%ld\n", (long)wr.left, (long)wr.top,
                   (long)(wr.right - wr.left), (long)(wr.bottom - wr.top));
            fflush(stdout);
            check(wr.left == 150 && wr.top == 140, "drag moved the window by exactly +50,+40");
            check(wr.right - wr.left == 400 && wr.bottom - wr.top == 300, "drag kept the window size");
            g_drag_seen = 1;
        } else if (g_exits == 2) {
            printf("window_nc: resized to %ld,%ld size=%ldx%ld\n", (long)wr.left, (long)wr.top,
                   (long)(wr.right - wr.left), (long)(wr.bottom - wr.top));
            fflush(stdout);
            check(wr.left == 150 && wr.top == 140, "the corner drag kept the window origin");
            check(wr.right - wr.left == 430 && wr.bottom - wr.top == 320, "the corner drag grew the window by +30,+20");
            check(cr.right == g_client_w0 + 30 && cr.bottom == g_client_h0 + 20, "the client grew by +30,+20");
            g_size_seen = 1;
            /* repaint the whole frame at its final size, then read it back */
            RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
            check_pixels(hwnd);
        }
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) {
            printf("window_nc: WM_SYSCOMMAND SC_CLOSE\n");
            fflush(stdout);
            check(g_drag_seen && g_size_seen, "close came after the drag and the resize");
        }
        break;
    case WM_CLOSE:
        printf("window_nc: WM_CLOSE\n");
        fflush(stdout);
        g_close_seen = 1;
        g_close_count++;
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
    return (g_drag_seen && g_size_seen && g_close_count == 1 && g_destroy_seen) ? 42 : 5;
}
