/* guest_window_input2: input semantics beyond the plain sequence of window_input.c. Same machinery (the host
 * injects with WIN10XBOX_TEST_INJECT_INPUT, host-win32/main.cpp), plus SendInput from the guest for what the
 * script has no verb for (modifiers, the lock keys, extra windows). Every case logs one token per message a
 * window procedure receives into a string and compares the whole string with what Windows (and Wine's
 * win32u/message.c, input.c) deliver, so a missing, extra or reordered message fails the case.
 *
 * Token key (the window id is the first character): MM/LD/LU/RD/RU/MD/MU/XD/XU (client x,y; wParam), LDC =
 * WM_LBUTTONDBLCLK, WH = WM_MOUSEWHEEL (delta; MK_ flags; screen x,y), NCMM/NCLD/NCLU (hit test; screen x,y),
 * NCHT = WM_NCHITTEST, SC = WM_SETCURSOR (hit test, mouse message), MACT = WM_MOUSEACTIVATE (hit test, message),
 * ACT/NCACT/SF/KF = WM_ACTIVATE/WM_NCACTIVATE/WM_SETFOCUS/WM_KILLFOCUS, CAPCH = WM_CAPTURECHANGED,
 * PN = WM_PARENTNOTIFY, KD/KU = WM_KEYDOWN/UP (vk), SKD/SKU = the SYS variants (vk; context bit), CH/SCH =
 * WM_CHAR/WM_SYSCHAR.
 *
 * Output: one "PASS <case>" / "FAIL <case>" line per case (FAIL shows got/want), then
 *   window_input2: <n> passed, <m> failed
 * and exit 42 when nothing failed (3 = the host script never arrived, 1 = a case failed).
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static char g_log[4096];
static int g_fail, g_pass;
static int g_verbose;            /* log WM_NCHITTEST / WM_SETCURSOR too */
static DWORD g_ld_time, g_ld_pos;/* GetMessageTime / GetMessagePos inside the last WM_LBUTTONDOWN */
static int g_ld_keystate;        /* GetKeyState(VK_LBUTTON) inside it */

static void ev(const char *fmt, ...) {
    char b[160];
    size_t n = strlen(g_log);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n + strlen(b) + 2 >= sizeof g_log) return;
    if (n) g_log[n++] = ' ';
    strcpy(g_log + n, b);
}

#define X(l) ((int)(short)LOWORD(l))
#define Y(l) ((int)(short)HIWORD(l))

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    int id = (int)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!id) return DefWindowProcW(h, m, w, l);   /* still being created: not logged */
    switch (m) {
    case WM_NCHITTEST:
        if (id == 'T') return HTTRANSPARENT;
        if (g_verbose) ev("%c:NCHT(%d,%d)", id, X(l), Y(l));
        break;
    case WM_SETCURSOR:
        if (g_verbose) ev("%c:SC(%d,%x)", id, X(l), (unsigned)HIWORD(l));
        break;
    case WM_MOUSEACTIVATE: ev("%c:MACT(%d,%x)", id, X(l), (unsigned)HIWORD(l)); break;
    case WM_ACTIVATE: ev("%c:ACT(%d)", id, (int)LOWORD(w)); break;
    case WM_NCACTIVATE: ev("%c:NCACT(%d)", id, (int)w); break;
    case WM_SETFOCUS: ev("%c:SF", id); break;
    case WM_KILLFOCUS: ev("%c:KF", id); break;
    case WM_CAPTURECHANGED: ev("%c:CAPCH", id); break;
    case WM_PARENTNOTIFY: ev("%c:PN(%x;%d,%d)", id, (unsigned)LOWORD(w), X(l), Y(l)); break;
    case WM_MOUSEMOVE: ev("%c:MM(%d,%d;%x)", id, X(l), Y(l), (unsigned)w); break;
    case WM_LBUTTONDOWN:
        g_ld_time = (DWORD)GetMessageTime();
        g_ld_pos = (DWORD)GetMessagePos();
        g_ld_keystate = GetKeyState(VK_LBUTTON) < 0;
        ev("%c:LD(%d,%d;%x)", id, X(l), Y(l), (unsigned)w);
        break;
    case WM_LBUTTONUP: ev("%c:LU(%d,%d;%x)", id, X(l), Y(l), (unsigned)w); break;
    case WM_LBUTTONDBLCLK: ev("%c:LDC(%d,%d;%x)", id, X(l), Y(l), (unsigned)w); break;
    case WM_RBUTTONDOWN: ev("%c:RD(%d,%d;%x)", id, X(l), Y(l), (unsigned)w); break;
    case WM_RBUTTONUP: ev("%c:RU(%d,%d;%x)", id, X(l), Y(l), (unsigned)w); break;
    case WM_MBUTTONDOWN: ev("%c:MD(%d,%d;%x)", id, X(l), Y(l), (unsigned)w); break;
    case WM_MBUTTONUP: ev("%c:MU(%d,%d;%x)", id, X(l), Y(l), (unsigned)w); break;
    case WM_XBUTTONDOWN: ev("%c:XD(%d,%d;%x)", id, X(l), Y(l), (unsigned)w); break;
    case WM_XBUTTONUP: ev("%c:XU(%d,%d;%x)", id, X(l), Y(l), (unsigned)w); break;
    case WM_NCLBUTTONDOWN: ev("%c:NCLD(%d;%d,%d)", id, (int)w, X(l), Y(l)); break;
    case WM_NCLBUTTONUP: ev("%c:NCLU(%d;%d,%d)", id, (int)w, X(l), Y(l)); break;
    case WM_NCMOUSEMOVE: ev("%c:NCMM(%d;%d,%d)", id, (int)w, X(l), Y(l)); break;
    case WM_MOUSEWHEEL: ev("%c:WH(%d;%x;%d,%d)", id, (int)(short)HIWORD(w), (unsigned)LOWORD(w), X(l), Y(l)); break;
    case WM_KEYDOWN: ev("%c:KD(%02x)", id, (unsigned)w); break;
    case WM_KEYUP: ev("%c:KU(%02x)", id, (unsigned)w); break;
    case WM_SYSKEYDOWN: ev("%c:SKD(%02x;%d)", id, (unsigned)w, (int)((l >> 29) & 1)); break;
    case WM_SYSKEYUP: ev("%c:SKU(%02x)", id, (unsigned)w); break;
    case WM_CHAR: ev("%c:CH(%02x)", id, (unsigned)w); break;
    case WM_SYSCHAR: ev("%c:SCH(%02x)", id, (unsigned)w); break;
    case WM_CLOSE: return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void pump(int ms) {
    MSG m;
    DWORD end = GetTickCount() + (DWORD)ms;
    for (;;) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_TIMER) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        {
            int left = (int)(end - GetTickCount());
            if (left <= 0) break;
            MsgWaitForMultipleObjectsEx(0, NULL, (DWORD)(left > 40 ? 40 : left), QS_ALLINPUT, 0);
        }
    }
}

static void pass(const char *name) {
    g_pass++;
    printf("PASS %s\n", name);
    fflush(stdout);
}

static void fail(const char *name, const char *got, const char *want) {
    g_fail++;
    printf("FAIL %s\n   got:  %s\n   want: %s\n", name, got, want);
    fflush(stdout);
}

static void check(const char *name, int ok) {
    if (ok) pass(name);
    else fail(name, "false", "true");
}

/* Waits (up to 8 s) until the log has `want` as its prefix-complete value, then compares it. */
static void expect_wait(const char *name, const char *want) {
    DWORD end = GetTickCount() + 8000;
    while (strcmp(g_log, want) && (int)(end - GetTickCount()) > 0) pump(50);
    pump(60);   /* anything extra shows up in the comparison */
    if (!strcmp(g_log, want)) pass(name);
    else fail(name, g_log, want);
    g_log[0] = 0;
}

static void expect(const char *name, const char *want) {
    pump(60);
    if (!strcmp(g_log, want)) pass(name);
    else fail(name, g_log, want);
    g_log[0] = 0;
}

static int g_sw, g_sh;

static void mouse(DWORD flags, int x, int y, DWORD data) {
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    if (flags & MOUSEEVENTF_MOVE) {   /* exact: ceil(x * 65536 / width) maps back to x */
        in.mi.dwFlags |= MOUSEEVENTF_ABSOLUTE;
        in.mi.dx = (x * 65536 + g_sw - 1) / g_sw;
        in.mi.dy = (y * 65536 + g_sh - 1) / g_sh;
    }
    SendInput(1, &in, sizeof in);
}
static void moveto(int x, int y) { mouse(MOUSEEVENTF_MOVE, x, y, 0); }
static void click(void) {
    mouse(MOUSEEVENTF_LEFTDOWN, 0, 0, 0);
    mouse(MOUSEEVENTF_LEFTUP, 0, 0, 0);
}
static void key(int vk, int scan, int up) {
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)scan;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof in);
}
static void tap(int vk, int scan) {
    key(vk, scan, 0);
    key(vk, scan, 1);
}

static HWND mk(const wchar_t *cls, int id, DWORD style, int x, int y, int w, int h, HWND parent) {
    HWND r = CreateWindowExW(0, cls, L"p", style, x, y, w, h, parent, NULL, GetModuleHandleW(NULL), NULL);
    if (r) SetWindowLongPtrW(r, GWLP_USERDATA, id);
    return r;
}

/* A window on a second thread: input is queued for, and delivered on, the thread that owns the window. */
static char g_log3[512];
static volatile LONG g_t3_ready;
static DWORD g_t3_tid;
static LRESULT CALLBACK wndproc3(HWND h, UINT m, WPARAM w, LPARAM l) {
    char b[64];
    size_t n = strlen(g_log3);
    if (m == WM_MOUSEMOVE && n < 400) {
        sprintf(b, "3:MM(%d,%d;own-thread=%d) ", X(l), Y(l), GetCurrentThreadId() == g_t3_tid);
        strcat(g_log3, b);
    }
    if (m == WM_LBUTTONDOWN && n < 400) {
        sprintf(b, "3:LD(%d,%d) ", X(l), Y(l));
        strcat(g_log3, b);
    }
    return DefWindowProcW(h, m, w, l);
}
static DWORD WINAPI thread3(void *p) {
    WNDCLASSW wc;
    MSG m;
    (void)p;
    g_t3_tid = GetCurrentThreadId();
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc3;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"W10XInput3";
    RegisterClassW(&wc);
    CreateWindowExW(0, L"W10XInput3", L"t3", WS_POPUP | WS_VISIBLE, 900, 500, 300, 200, NULL, NULL, wc.hInstance, NULL);
    InterlockedExchange(&g_t3_ready, 1);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

/* The US layout: every printable ASCII character maps to a key and back, and agrees with ToUnicode. */
static void layout_cases(void) {
    int bad = 0, c;
    char first_bad[96] = "";
    for (c = 0x20; c < 0x7f; c++) {
        SHORT vks = VkKeyScanW((WCHAR)c);
        BYTE vk = LOBYTE(vks), mods = HIBYTE(vks);
        BYTE state[256];
        WCHAR out[4] = {0};
        UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        int n;
        memset(state, 0, sizeof state);
        if (mods & 1) state[VK_SHIFT] = 0x80;
        n = ToUnicode(vk, scan, state, out, 4, 0);
        if (vks == -1 || !scan || n != 1 || out[0] != (WCHAR)c || MapVirtualKeyW(scan, MAPVK_VSC_TO_VK) != vk) {
            if (!bad) snprintf(first_bad, sizeof first_bad, "'%c' vkscan=%04x scan=%02x n=%d out=%04x", c, (unsigned)(USHORT)vks, scan, n, out[0]);
            bad++;
        }
    }
    if (bad) fail("US layout: VkKeyScan / MapVirtualKey / ToUnicode agree for ASCII", first_bad, "0 mismatches");
    else pass("US layout: VkKeyScan / MapVirtualKey / ToUnicode agree for ASCII");
    check("MapVirtualKey 'A' -> scan 0x1e and back", MapVirtualKeyW('A', MAPVK_VK_TO_VSC) == 0x1e && MapVirtualKeyW(0x1e, MAPVK_VSC_TO_VK) == 'A');
    check("MapVirtualKey VK_TO_CHAR gives the upper case letter", MapVirtualKeyW('Q', MAPVK_VK_TO_CHAR) == 'Q');
    check("VkKeyScan('a') = 'A', ('A') = shift+'A'", VkKeyScanW(L'a') == 'A' && VkKeyScanW(L'A') == (0x100 | 'A'));
    {
        BYTE state[256];
        WCHAR out[4] = {0};
        memset(state, 0, sizeof state);
        state[VK_CAPITAL] = 1;   /* caps lock on: letters flip, digits do not */
        check("ToUnicode with caps lock: 'a' -> 'A', '1' -> '1'",
              ToUnicode('A', 0x1e, state, out, 4, 0) == 1 && out[0] == L'A' && ToUnicode('1', 0x02, state, out, 4, 0) == 1 && out[0] == L'1');
    }
}

int main(void) {
    WNDCLASSW wc;
    HWND w1, w2, ch;
    MSG msg;
    DWORD t_end;

    g_verbose = 1;   /* part A compares the hit test / cursor messages too; the host script may start at any time */
    g_sw = GetSystemMetrics(SM_CXSCREEN);
    g_sh = GetSystemMetrics(SM_CYSCREEN);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"W10XInput2Dbl";
    wc.style = CS_DBLCLKS;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc)) return 2;
    wc.lpszClassName = L"W10XInput2";
    wc.style = 0;
    if (!RegisterClassW(&wc)) return 2;
    printf("window_input2: classes registered\n");

    w1 = mk(L"W10XInput2Dbl", '1', WS_POPUP | WS_VISIBLE, 100, 100, 400, 300, NULL);
    if (!w1) return 2;
    pump(50);   /* not cleared: the host script may already be delivering */
    check("a new visible window is active and has the focus", GetActiveWindow() == w1 && GetFocus() == w1 && GetForegroundWindow() == w1);

    /* ---- Part A: the host injects (script in tests/integration/CMakeLists.txt). Hit test, cursor and activation
     * sequencing are part of the expectation: WM_NCHITTEST, then WM_SETCURSOR, then the mouse message. */
    {
        const char *a =
            "1:NCHT(150,150) 1:SC(1,200) 1:MM(50,50;0) "
            "1:NCHT(150,150) 1:SC(1,201) 1:LD(50,50;1) "
            "1:NCHT(150,150) 1:SC(1,202) 1:LU(50,50;0) "
            "1:NCHT(150,150) 1:SC(1,201) 1:LDC(50,50;1) "
            "1:NCHT(150,150) 1:SC(1,202) 1:LU(50,50;0) "
            "1:NCHT(150,150) 1:SC(1,204) 1:RD(50,50;2) "
            "1:NCHT(150,150) 1:SC(1,205) 1:RU(50,50;0) "
            "1:NCHT(150,150) 1:SC(1,20a) 1:WH(-120;0;150,150) "
            "1:KD(42) 1:CH(62) 1:KU(42)";
        expect_wait("host-injected move, double click (CS_DBLCLKS), right click, wheel, key", a);
        if (g_fail) {
            printf("window_input2: the host script did not deliver\n");
            return 3;
        }
    }
    g_verbose = 0;
    {
        int d = (int)(GetTickCount() - g_ld_time);
        check("GetMessageTime is on the clock of GetTickCount", d > -1000 && d < 20000);
    }
    check("GetMessagePos is the screen position of the message", (int)(short)LOWORD(g_ld_pos) == 150 && (int)(short)HIWORD(g_ld_pos) == 150);
    check("GetKeyState sees the button of the message being handled", g_ld_keystate == 1);
    {
        POINT p;
        GetCursorPos(&p);
        check("GetCursorPos is the injected position", p.x == 150 && p.y == 150);
    }
    check("GetAsyncKeyState: all keys and buttons are up again", GetAsyncKeyState(VK_LBUTTON) == 0 && GetAsyncKeyState('B') == 0);

    /* ---- Part B: SendInput from here. */
    moveto(150, 150);
    pump(60);
    g_log[0] = 0;
    click();
    expect("left click", "1:LD(50,50;1) 1:LU(50,50;0)");
    pump(600);   /* out of double click range in time */
    g_log[0] = 0;
    mouse(MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0);
    mouse(MOUSEEVENTF_MIDDLEUP, 0, 0, 0);
    mouse(MOUSEEVENTF_XDOWN, 0, 0, XBUTTON2);
    mouse(MOUSEEVENTF_XUP, 0, 0, XBUTTON2);
    expect("middle and X2 buttons", "1:MD(50,50;10) 1:MU(50,50;0) 1:XD(50,50;20040) 1:XU(50,50;20000)");

    tap('A', 0x1e);
    expect("a", "1:KD(41) 1:CH(61) 1:KU(41)");
    key(VK_LSHIFT, 0x2a, 0);
    tap('A', 0x1e);
    key(VK_LSHIFT, 0x2a, 1);
    expect("shift+a -> 'A' (the shift state is the one of the message)", "1:KD(10) 1:KD(41) 1:CH(41) 1:KU(41) 1:KU(10)");
    key(VK_LSHIFT, 0x2a, 0);
    tap('1', 0x02);
    tap(VK_OEM_2, 0x35);
    key(VK_LSHIFT, 0x2a, 1);
    expect("shift+1 -> '!', shift+/ -> '?'", "1:KD(10) 1:KD(31) 1:CH(21) 1:KU(31) 1:KD(bf) 1:CH(3f) 1:KU(bf) 1:KU(10)");
    tap(VK_OEM_PERIOD, 0x34);
    tap(VK_OEM_MINUS, 0x0c);
    tap(VK_SPACE, 0x39);
    tap(VK_RETURN, 0x1c);
    expect("punctuation, space, return", "1:KD(be) 1:CH(2e) 1:KU(be) 1:KD(bd) 1:CH(2d) 1:KU(bd) 1:KD(20) 1:CH(20) 1:KU(20) 1:KD(0d) 1:CH(0d) 1:KU(0d)");
    tap(VK_CAPITAL, 0x3a);
    pump(60);
    g_log[0] = 0;
    check("caps lock toggles the low bit of GetKeyState", (GetKeyState(VK_CAPITAL) & 1) == 1);
    tap('A', 0x1e);
    expect("caps lock on: 'a' key -> 'A'", "1:KD(41) 1:CH(41) 1:KU(41)");
    tap(VK_CAPITAL, 0x3a);
    pump(60);
    g_log[0] = 0;
    check("a second press toggles it back", (GetKeyState(VK_CAPITAL) & 1) == 0);
    key(VK_LCONTROL, 0x1d, 0);
    tap('A', 0x1e);
    key(VK_LCONTROL, 0x1d, 1);
    expect("ctrl+a -> 0x01", "1:KD(11) 1:KD(41) 1:CH(01) 1:KU(41) 1:KU(11)");
    key(VK_LMENU, 0x38, 0);
    tap('F', 0x21);
    key(VK_LMENU, 0x38, 1);
    expect("alt+f: SYS messages, context bit set", "1:SKD(12;1) 1:SKD(46;1) 1:SCH(66) 1:SKU(46) 1:SKU(12)");
    tap(VK_F10, 0x44);
    expect("F10 is a system key", "1:SKD(79;0) 1:SKU(79)");
    key(VK_LSHIFT, 0x2a, 0);
    pump(60);
    g_log[0] = 0;
    check("GetKeyboardState and GetKeyState agree on the held shift", (GetKeyState(VK_SHIFT) & 0x8000) != 0 && GetAsyncKeyState(VK_LSHIFT) < 0);
    key(VK_LSHIFT, 0x2a, 1);
    pump(60);
    g_log[0] = 0;

    /* PeekMessage filters and PM_NOREMOVE */
    moveto(160, 160);
    tap('B', 0x30);
    {
        BOOL a = PeekMessageW(&msg, NULL, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE);
        UINT m1 = msg.message;
        BOOL a2 = PeekMessageW(&msg, NULL, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE);
        BOOL b = PeekMessageW(&msg, NULL, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE);
        DWORD qs = GetQueueStatus(QS_KEY | QS_MOUSE);
        check("PeekMessage: a key filter skips the mouse message ahead of it, PM_NOREMOVE keeps it",
              a && a2 && m1 == WM_KEYDOWN && b && msg.message == WM_MOUSEMOVE && X(msg.lParam) == 60 && msg.pt.x == 160 && msg.pt.y == 160 &&
                  (HIWORD(qs) & QS_KEY));
    }
    pump(60);
    g_log[0] = 0;

    /* A second window: clicking it activates it, the focus follows, keys go there. */
    w2 = mk(L"W10XInput2", '2', WS_POPUP | WS_VISIBLE, 600, 100, 200, 200, NULL);
    pump(50);
    g_log[0] = 0;
    check("creating a second window activates it", GetActiveWindow() == w2);
    SetForegroundWindow(w1);
    pump(50);
    g_log[0] = 0;
    check("SetForegroundWindow(w1) activates it again", GetActiveWindow() == w1 && GetForegroundWindow() == w1);
    moveto(650, 150);
    click();
    expect("click on an inactive window: WM_MOUSEACTIVATE, deactivate, activate (WA_CLICKACTIVE), focus, then the click",
           "2:MM(50,50;0) 2:MACT(1,201) 1:NCACT(0) 1:ACT(0) 2:NCACT(1) 2:ACT(2) 1:KF 2:SF 2:LD(50,50;1) 2:LU(50,50;0)");
    tap('C', 0x2e);
    expect("keys follow the focus", "2:KD(43) 2:CH(63) 2:KU(43)");

    /* capture: moves and clicks outside any window go to the capture window, in its client coordinates */
    SetCapture(w1);
    moveto(50, 60);
    expect("capture: a move outside every window", "1:MM(-50,-40;0)");
    click();
    expect("capture: a click outside (no activation)", "1:LD(-50,-40;1) 1:LU(-50,-40;0)");
    check("GetCapture", GetCapture() == w1 && GetActiveWindow() == w2);
    ReleaseCapture();
    expect("ReleaseCapture sends WM_CAPTURECHANGED", "1:CAPCH");
    moveto(60, 70);
    expect("nothing under the cursor: nothing is delivered", "");

    /* children: the deepest child gets the message, its parents hear WM_PARENTNOTIFY, a transparent child
     * (HTTRANSPARENT from WM_NCHITTEST) lets the point fall through to its parent */
    ch = mk(L"W10XInput2", 'C', WS_CHILD | WS_VISIBLE, 10, 10, 100, 100, w1);
    mk(L"W10XInput2", 'T', WS_CHILD | WS_VISIBLE, 200, 10, 100, 100, w1);
    pump(50);
    g_log[0] = 0;
    moveto(125, 125);
    expect("child: move in child client coordinates", "C:MM(15,15;0)");
    click();
    expect("child: click -> parent notify, mouse activate (child first, then its parent), activation",
           "1:PN(201;25,25) C:MACT(1,201) 1:MACT(1,201) 2:NCACT(0) 2:ACT(0) 1:NCACT(1) 1:ACT(2) 2:KF 1:SF C:LD(15,15;1) C:LU(15,15;0)");
    SetFocus(ch);
    pump(50);
    g_log[0] = 0;
    tap('D', 0x20);
    expect("keys go to the focus child", "C:KD(44) C:CH(64) C:KU(44)");
    moveto(320, 130);
    expect("HTTRANSPARENT child: the parent gets the move", "1:MM(220,30;0)");

    /* non-client area */
    mk(L"W10XInput2", '4', WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 500, 300, 200, NULL);
    pump(50);
    g_log[0] = 0;
    SetForegroundWindow(w1);
    pump(50);
    g_log[0] = 0;
    moveto(200, 510);
    expect("caption: WM_NCMOUSEMOVE with HTCAPTION and screen coordinates", "4:NCMM(2;200,510)");
    click();
    expect("caption click: DefWindowProc answers MA_NOACTIVATE for HTCAPTION, WM_NCLBUTTONDOWN activates",
           "4:MACT(2,201) 4:NCLD(2;200,510) 1:NCACT(0) 1:ACT(0) 4:NCACT(1) 4:ACT(2) 1:KF 4:SF 4:NCLU(2;200,510)");

    /* a window of another thread gets its input on that thread */
    {
        HANDLE t = CreateThread(NULL, 0, thread3, NULL, 0, NULL);
        DWORD wait_end = GetTickCount() + 5000;
        while (!g_t3_ready && (int)(wait_end - GetTickCount()) > 0) Sleep(10);
        Sleep(100);
        moveto(950, 550);
        t_end = GetTickCount() + 3000;
        while (!strstr(g_log3, "3:MM(") && (int)(t_end - GetTickCount()) > 0) pump(30);
        printf("window_input2: thread 3 log [%s]\n", g_log3);
        check("another thread's window: delivered on that thread", strstr(g_log3, "3:MM(50,50;own-thread=1)") != NULL);
        click();
        t_end = GetTickCount() + 3000;
        while (!strstr(g_log3, "3:LD(") && (int)(t_end - GetTickCount()) > 0) pump(30);
        check("another thread's window: click", strstr(g_log3, "3:LD(50,50)") != NULL);
        (void)t;
    }
    pump(100);
    g_log[0] = 0;

    layout_cases();

    printf("window_input2: %d passed, %d failed\n", g_pass, g_fail);
    fflush(stdout);
    return g_fail ? 1 : 42;
}
