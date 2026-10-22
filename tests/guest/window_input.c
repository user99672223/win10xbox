/* guest_window_input: the input path, end to end. A GUI guest that creates a
 * WS_POPUP window at (100,100), 400x300, paints it once and logs every input
 * message its window procedure receives. The host injects the input (the
 * WIN10XBOX_TEST_INJECT_INPUT script in host-win32/main.cpp), so this program
 * only has to see it: no clicks, no keys, nothing else.
 *
 * The expected sequence, in order (messages in between are logged but ignored):
 *   WM_MOUSEMOVE (50,50)              client coordinates: the script moves to screen (150,150)
 *   WM_LBUTTONDOWN (50,50), MK_LBUTTON
 *   WM_LBUTTONUP (50,50)
 *   WM_KEYDOWN vk=0x41
 *   WM_CHAR 'a'
 *   WM_KEYUP vk=0x41
 *   WM_MOUSEWHEEL delta=120
 * When the whole sequence has arrived it posts WM_QUIT and exits 42. An 8 s
 * watchdog exits 3 with "input: timeout after N steps" if it has not.
 *
 * Output lines (the integration test matches them):
 *   window_input: class registered
 *   window_input: created hwnd=... client=400x300
 *   window_input: painted
 *   input: WM_MOUSEMOVE x=50 y=50
 *   input: WM_LBUTTONDOWN x=50 y=50 mk=0x1
 *   input: WM_LBUTTONUP x=50 y=50
 *   input: WM_KEYDOWN vk=0x41
 *   input: WM_CHAR ch=a
 *   input: WM_KEYUP vk=0x41
 *   input: WM_MOUSEWHEEL delta=120
 *   window_input: done
 */
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <string.h>

#define TIMER_WATCHDOG 1
#define WATCHDOG_MS    8000

enum {
    STEP_MOUSEMOVE,
    STEP_LBUTTONDOWN,
    STEP_LBUTTONUP,
    STEP_KEYDOWN,
    STEP_CHAR,
    STEP_KEYUP,
    STEP_WHEEL,
    STEP_COUNT
};

static int g_painted;
static int g_step;                 /* next expected step; STEP_COUNT = all arrived */
static int g_result = 3;           /* 3 = watchdog fired first */

/* Does `msg` complete step `step`? */
static int matches(int step, UINT msg, WPARAM wp, LPARAM lp) {
    switch (step) {
    case STEP_MOUSEMOVE:
        return msg == WM_MOUSEMOVE && GET_X_LPARAM(lp) == 50 && GET_Y_LPARAM(lp) == 50;
    case STEP_LBUTTONDOWN:
        return msg == WM_LBUTTONDOWN && GET_X_LPARAM(lp) == 50 && GET_Y_LPARAM(lp) == 50 && (wp & MK_LBUTTON);
    case STEP_LBUTTONUP:
        return msg == WM_LBUTTONUP && GET_X_LPARAM(lp) == 50 && GET_Y_LPARAM(lp) == 50;
    case STEP_KEYDOWN:
        return msg == WM_KEYDOWN && wp == 0x41;
    case STEP_CHAR:
        return msg == WM_CHAR && wp == 'a';
    case STEP_KEYUP:
        return msg == WM_KEYUP && wp == 0x41;
    case STEP_WHEEL:
        return msg == WM_MOUSEWHEEL && GET_WHEEL_DELTA_WPARAM(wp) == 120;
    }
    return 0;
}

static void log_input(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE:
        printf("input: WM_MOUSEMOVE x=%d y=%d\n", GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        break;
    case WM_LBUTTONDOWN:
        printf("input: WM_LBUTTONDOWN x=%d y=%d mk=0x%x\n", GET_X_LPARAM(lp), GET_Y_LPARAM(lp), (unsigned)wp);
        break;
    case WM_LBUTTONUP:
        printf("input: WM_LBUTTONUP x=%d y=%d\n", GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        break;
    case WM_KEYDOWN:
        printf("input: WM_KEYDOWN vk=0x%02x\n", (unsigned)wp);
        break;
    case WM_CHAR:
        if (wp >= 0x20 && wp < 0x7f) printf("input: WM_CHAR ch=%c\n", (int)wp);
        else printf("input: WM_CHAR ch=0x%02x\n", (unsigned)wp);
        break;
    case WM_KEYUP:
        printf("input: WM_KEYUP vk=0x%02x\n", (unsigned)wp);
        break;
    case WM_MOUSEWHEEL:
        printf("input: WM_MOUSEWHEEL delta=%d\n", (int)GET_WHEEL_DELTA_WPARAM(wp));
        break;
    }
    fflush(stdout);
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
        if (!g_painted) {
            g_painted = 1;
            printf("window_input: painted\n");
            fflush(stdout);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_KEYDOWN:
    case WM_CHAR:
    case WM_KEYUP:
    case WM_MOUSEWHEEL:
        log_input(msg, wp, lp);
        if (g_step < STEP_COUNT && matches(g_step, msg, wp, lp) && ++g_step == STEP_COUNT) {
            KillTimer(hwnd, TIMER_WATCHDOG);
            g_result = 42;
            PostQuitMessage(0);
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_WATCHDOG && g_step < STEP_COUNT) {
            printf("input: timeout after %d steps\n", g_step);
            fflush(stdout);
            g_result = 3;
            PostQuitMessage(0);
        }
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(void) {
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;
    RECT rc;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"W10XWindowInput";
    wc.hbrBackground = NULL;
    if (!RegisterClassW(&wc)) {
        printf("window_input: RegisterClassW failed (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    printf("window_input: class registered\n");
    fflush(stdout);

    hwnd = CreateWindowExW(0, L"W10XWindowInput", L"win10xbox input", WS_POPUP | WS_VISIBLE, 100, 100, 400, 300, NULL,
                           NULL, wc.hInstance, NULL);
    if (!hwnd) {
        printf("window_input: CreateWindowExW failed (%lu)\n", (unsigned long)GetLastError());
        return 2;
    }
    GetClientRect(hwnd, &rc);
    printf("window_input: created hwnd=%p client=%ldx%ld\n", (void*)hwnd, (long)(rc.right - rc.left),
           (long)(rc.bottom - rc.top));
    fflush(stdout);
    SetTimer(hwnd, TIMER_WATCHDOG, WATCHDOG_MS, NULL);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    printf("window_input: done\n");
    fflush(stdout);
    return g_result;
}
