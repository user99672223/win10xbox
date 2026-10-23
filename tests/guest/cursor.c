/* guest_cursor: cursors and icons, end to end (core/user/win_cursor.cpp). A GUI guest that loads Wine's own
 * IDC_ARROW and IDC_IBEAM (user32 reads the 32 bpp images from its resources and hands the bitmaps to
 * NtUserSetCursorIconData), builds a monochrome AND/XOR cursor with CreateCursor, checks what GetIconInfo /
 * GetCursorInfo / GetCursor / SetCursor / ShowCursor report, draws the icons into its window with DrawIconEx and reads
 * the pixels back with GetPixel. The composed cursor itself is not painted by the guest, so the host looks at it: the
 * integration test (tests/integration/CMakeLists.txt) injects moves and takes screenshots with the inject script's
 * `shot FILE`, and checks pixels of each file.
 *
 * The window is a WS_POPUP at (100,100), 400x300, client colour RGB(64,128,192) = ff4080c0, with the DrawIconEx results
 * near its top left. The script moves the pointer to four places in turn and the guest changes state on each move
 * (it recognises them by GetCursorPos):
 *   P0 (250,250)  IBEAM set before the window exists, still set        -> screenshot 1: the I-beam, hotspot 3,9
 *   P1 (320,250)  SetCursor(IDC_ARROW)                                  -> screenshot 2: the arrow, hotspot 0,0
 *   P2 (390,250)  SetCursor(monochrome cursor, hotspot 4,4)             -> screenshot 3: AND/XOR
 *   P3 (392,252)  ShowCursor(FALSE), then the process exits 42          -> the final screenshot: no cursor
 *
 * Output lines (the integration test matches them):
 *   cursor: loaded arrow=... ibeam=... shared=1
 *   cursor: arrow icon=0 hot=0,0 mask=32x32x1 color=32x32x32
 *   cursor: ibeam icon=0 hot=3,9 mask=32x32x1 color=32x32x32
 *   cursor: mono hot=4,4 mask=32x64x1 color=none
 *   cursor: DrawIconEx arrow=ok stretched=ok mono=ok
 *   cursor: P0 ibeam showing pos=250,250
 *   cursor: P1 SetCursor(arrow) returned ibeam
 *   cursor: P2 SetCursor(mono) returned arrow
 *   cursor: P3 ShowCursor(FALSE)=-1 flags=0
 *   cursor: done
 */
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <string.h>

#define TIMER_WATCHDOG 1
#define WATCHDOG_MS    40000

static const POINT P[4] = {{250, 250}, {320, 250}, {390, 250}, {392, 252}};

static HCURSOR g_arrow, g_ibeam, g_mono;
static int g_phase;        /* the next position the guest waits for */
static int g_failed;
static int g_result = 3;   /* 3 = the watchdog fired first */
static int g_draw_ok;

#define CHECK(cond, ...) \
    do { if (!(cond)) { g_failed++; printf("cursor: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

/* The pixels of the image the cursor has, as GetIconInfo and GetObject report them. */
static void describe(const char* name, HCURSOR c, int want_hx, int want_hy, int want_mask_h, int want_color) {
    ICONINFO ii;
    BITMAP mask, color;
    memset(&ii, 0, sizeof ii);
    memset(&mask, 0, sizeof mask);
    memset(&color, 0, sizeof color);
    if (!GetIconInfo(c, &ii)) {
        CHECK(0, "GetIconInfo(%s) failed (%lu)", name, (unsigned long)GetLastError());
        return;
    }
    CHECK(ii.fIcon == FALSE, "%s fIcon=%d", name, (int)ii.fIcon);
    CHECK((int)ii.xHotspot == want_hx && (int)ii.yHotspot == want_hy, "%s hotspot %lu,%lu", name,
          (unsigned long)ii.xHotspot, (unsigned long)ii.yHotspot);
    CHECK(ii.hbmMask && GetObject(ii.hbmMask, sizeof mask, &mask) == sizeof mask, "%s mask bitmap", name);
    CHECK(mask.bmWidth == 32 && mask.bmHeight == want_mask_h && mask.bmBitsPixel == 1, "%s mask %ldx%ldx%u", name,
          (long)mask.bmWidth, (long)mask.bmHeight, (unsigned)mask.bmBitsPixel);
    if (want_color) {
        CHECK(ii.hbmColor && GetObject(ii.hbmColor, sizeof color, &color) == sizeof color, "%s colour bitmap", name);
        CHECK(color.bmWidth == 32 && color.bmHeight == 32 && color.bmBitsPixel == 32, "%s colour %ldx%ldx%u", name,
              (long)color.bmWidth, (long)color.bmHeight, (unsigned)color.bmBitsPixel);
    } else {
        CHECK(ii.hbmColor == NULL, "%s has a colour bitmap", name);
    }
    printf("cursor: %s icon=%d hot=%lu,%lu mask=%ldx%ldx%u color=", name, (int)ii.fIcon, (unsigned long)ii.xHotspot,
           (unsigned long)ii.yHotspot, (long)mask.bmWidth, (long)mask.bmHeight, (unsigned)mask.bmBitsPixel);
    if (want_color) printf("%ldx%ldx%u\n", (long)color.bmWidth, (long)color.bmHeight, (unsigned)color.bmBitsPixel);
    else printf("none\n");
    fflush(stdout);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
}

static int same(HDC dc, int x, int y, COLORREF want) {
    COLORREF got = GetPixel(dc, x, y);
    if (got != want) {
        g_failed++;
        printf("cursor: FAIL DrawIconEx pixel (%d,%d) is %06lx, expected %06lx\n", x, y, (unsigned long)got,
               (unsigned long)want);
        fflush(stdout);
        return 0;
    }
    return 1;
}

/* WM_PAINT: the background, then the icons through DrawIconEx and their pixels read back through the window DC. */
static void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    HBRUSH bg = CreateSolidBrush(RGB(64, 128, 192));
    const COLORREF B = RGB(64, 128, 192);
    int ok_arrow = 1, ok_stretch = 1, ok_mono = 1;
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    /* Wine's arrow, 32x32 with alpha: image (1,1) is opaque black, (2,4) and (3,5) opaque white, (20,5) transparent. */
    CHECK(DrawIconEx(dc, 10, 10, g_arrow, 0, 0, 0, NULL, DI_NORMAL), "DrawIconEx(arrow) failed (%lu)",
          (unsigned long)GetLastError());
    ok_arrow &= same(dc, 11, 11, RGB(0, 0, 0));
    ok_arrow &= same(dc, 12, 14, RGB(255, 255, 255));
    ok_arrow &= same(dc, 13, 15, RGB(255, 255, 255));
    ok_arrow &= same(dc, 30, 15, B);
    /* stretched to 64x64: image (1,1) covers (2..3, 2..3), image (2,4) covers (4..5, 8..9) */
    CHECK(DrawIconEx(dc, 60, 10, g_arrow, 64, 64, 0, NULL, DI_NORMAL), "DrawIconEx(arrow 64) failed");
    ok_stretch &= same(dc, 62, 12, RGB(0, 0, 0));
    ok_stretch &= same(dc, 63, 13, RGB(0, 0, 0));
    ok_stretch &= same(dc, 64, 18, RGB(255, 255, 255));
    ok_stretch &= same(dc, 95, 15, B);
    /* the monochrome cursor, columns 0-7 AND 0 XOR 0 (black), 8-15 AND 0 XOR 1 (white), 16-23 AND 1 XOR 1 (invert),
     * 24-31 AND 1 XOR 0 (unchanged) */
    CHECK(DrawIconEx(dc, 120, 10, g_mono, 0, 0, 0, NULL, DI_NORMAL), "DrawIconEx(mono) failed");
    ok_mono &= same(dc, 122, 16, RGB(0, 0, 0));
    ok_mono &= same(dc, 130, 16, RGB(255, 255, 255));
    ok_mono &= same(dc, 138, 16, RGB(191, 127, 63));
    ok_mono &= same(dc, 146, 16, B);
    EndPaint(hwnd, &ps);
    if (!g_draw_ok) {
        g_draw_ok = 1;
        printf("cursor: DrawIconEx arrow=%s stretched=%s mono=%s\n", ok_arrow ? "ok" : "BAD", ok_stretch ? "ok" : "BAD",
               ok_mono ? "ok" : "BAD");
        fflush(stdout);
    }
}

static void at_position(HWND hwnd, int phase) {
    CURSORINFO ci;
    HCURSOR prev;
    memset(&ci, 0, sizeof ci);
    ci.cbSize = sizeof ci;
    CHECK(GetCursorInfo(&ci), "GetCursorInfo failed (%lu)", (unsigned long)GetLastError());
    switch (phase) {
    case 0:
        CHECK(ci.hCursor == g_ibeam, "GetCursorInfo.hCursor %p, expected the I-beam %p", (void*)ci.hCursor, (void*)g_ibeam);
        CHECK(ci.flags == CURSOR_SHOWING, "GetCursorInfo.flags %lu", (unsigned long)ci.flags);
        CHECK(ci.ptScreenPos.x == P[0].x && ci.ptScreenPos.y == P[0].y, "GetCursorInfo position %ld,%ld",
              (long)ci.ptScreenPos.x, (long)ci.ptScreenPos.y);
        CHECK(GetCursor() == g_ibeam, "GetCursor() %p", (void*)GetCursor());
        printf("cursor: P0 ibeam showing pos=%ld,%ld\n", (long)ci.ptScreenPos.x, (long)ci.ptScreenPos.y);
        break;
    case 1:
        prev = SetCursor(g_arrow);
        CHECK(prev == g_ibeam, "SetCursor(arrow) returned %p", (void*)prev);
        CHECK(GetCursor() == g_arrow, "GetCursor() %p after SetCursor(arrow)", (void*)GetCursor());
        printf("cursor: P1 SetCursor(arrow) returned %s\n", prev == g_ibeam ? "ibeam" : "something else");
        break;
    case 2:
        prev = SetCursor(g_mono);
        CHECK(prev == g_arrow, "SetCursor(mono) returned %p", (void*)prev);
        CHECK(GetCursor() == g_mono, "GetCursor() %p after SetCursor(mono)", (void*)GetCursor());
        printf("cursor: P2 SetCursor(mono) returned %s\n", prev == g_arrow ? "arrow" : "something else");
        break;
    case 3: {
        int n = ShowCursor(FALSE);
        memset(&ci, 0, sizeof ci);
        ci.cbSize = sizeof ci;
        CHECK(GetCursorInfo(&ci), "GetCursorInfo failed after ShowCursor(FALSE)");
        CHECK(n == -1, "ShowCursor(FALSE) returned %d", n);
        CHECK(ci.flags == 0, "GetCursorInfo.flags %lu while hidden", (unsigned long)ci.flags);
        CHECK(ci.hCursor == g_mono, "GetCursorInfo.hCursor %p while hidden", (void*)ci.hCursor);
        printf("cursor: P3 ShowCursor(FALSE)=%d flags=%lu\n", n, (unsigned long)ci.flags);
        fflush(stdout);
        KillTimer(hwnd, TIMER_WATCHDOG);
        g_result = g_failed || !g_draw_ok ? 1 : 42;
        PostQuitMessage(0);
        break;
    }
    }
    fflush(stdout);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    POINT pt;
    switch (msg) {
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        GetCursorPos(&pt);
        if (g_phase < 4 && pt.x == P[g_phase].x && pt.y == P[g_phase].y) {
            int phase = g_phase++;
            at_position(hwnd, phase);
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_WATCHDOG) {
            printf("cursor: timeout waiting for position %d\n", g_phase);
            fflush(stdout);
            g_result = 3;
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
    HCURSOR arrow2;
    BYTE and_bits[32 * 4], xor_bits[32 * 4];
    int i;

    g_arrow = LoadCursorW(NULL, MAKEINTRESOURCEW(32512) /* IDC_ARROW */);
    g_ibeam = LoadCursorW(NULL, MAKEINTRESOURCEW(32513) /* IDC_IBEAM */);
    arrow2 = LoadCursorW(NULL, MAKEINTRESOURCEW(32512) /* IDC_ARROW */);
    if (!g_arrow || !g_ibeam) {
        printf("cursor: LoadCursorW failed arrow=%p ibeam=%p (%lu)\n", (void*)g_arrow, (void*)g_ibeam,
               (unsigned long)GetLastError());
        return 1;
    }
    printf("cursor: loaded arrow=%p ibeam=%p shared=%d\n", (void*)g_arrow, (void*)g_ibeam, arrow2 == g_arrow);
    CHECK(arrow2 == g_arrow, "a second LoadCursorW(IDC_ARROW) gave %p, not %p", (void*)arrow2, (void*)g_arrow);
    CHECK(g_arrow != g_ibeam, "the arrow and the I-beam are one handle");
    describe("arrow", g_arrow, 0, 0, 32, 1);
    describe("ibeam", g_ibeam, 3, 9, 32, 1);

    for (i = 0; i < 32; i++) {
        and_bits[i * 4 + 0] = 0x00; and_bits[i * 4 + 1] = 0x00; and_bits[i * 4 + 2] = 0xFF; and_bits[i * 4 + 3] = 0xFF;
        xor_bits[i * 4 + 0] = 0x00; xor_bits[i * 4 + 1] = 0xFF; xor_bits[i * 4 + 2] = 0xFF; xor_bits[i * 4 + 3] = 0x00;
    }
    g_mono = CreateCursor(GetModuleHandleW(NULL), 4, 4, 32, 32, and_bits, xor_bits);
    if (!g_mono) {
        printf("cursor: CreateCursor failed (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    describe("mono", g_mono, 4, 4, 64, 0);

    SetCursor(g_ibeam);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"W10XCursor";
    if (!RegisterClassW(&wc)) {
        printf("cursor: RegisterClassW failed (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    hwnd = CreateWindowExW(0, L"W10XCursor", L"win10xbox cursor", WS_POPUP | WS_VISIBLE, 100, 100, 400, 300, NULL, NULL,
                           wc.hInstance, NULL);
    if (!hwnd) {
        printf("cursor: CreateWindowExW failed (%lu)\n", (unsigned long)GetLastError());
        return 2;
    }
    fflush(stdout);
    SetTimer(hwnd, TIMER_WATCHDOG, WATCHDOG_MS, NULL);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    printf("cursor: done\n");
    fflush(stdout);
    return g_result;
}
