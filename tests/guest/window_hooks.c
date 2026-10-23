/* guest_window_hooks: SetWindowsHookEx (WH_MOUSE, WH_KEYBOARD, WH_GETMESSAGE, thread-local) and SetTimer precision.
 *
 * A WS_POPUP window at (100,100), 400x300 (client origin = screen (100,100)). Two WH_MOUSE hooks, a WH_KEYBOARD hook
 * and a WH_GETMESSAGE hook are installed on this thread. The host injects the input (WIN10XBOX_TEST_INJECT_INPUT,
 * host-win32/main.cpp):
 *   sleep 2000;move 150,150;ldown;lup;key 0x41;rdown;rup;key 0x42
 * so the queue holds, in order: WM_LBUTTONDOWN, WM_LBUTTONUP, WM_KEYDOWN/UP 0x41, WM_RBUTTONDOWN, WM_RBUTTONUP,
 * WM_KEYDOWN/UP 0x42. The window procedure unhooks the keyboard hook when WM_RBUTTONUP arrives (before the 0x42
 * keys are taken off the queue). Then the timers are measured.
 *
 * Output: "hooks: PASS <name>" / "hooks: FAIL <name> ..." per check, then
 *   hooks: <n> passed, <m> failed
 *   hooks: done
 * and exit 42 only when nothing failed (3 = the injected input never arrived).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
static HWND g_hwnd;
static unsigned g_seq;   /* one counter for hooks and window procedure: the order things happened in */

static void pass(const char *name) {
    g_pass++;
    printf("hooks: PASS %s\n", name);
    fflush(stdout);
}

static void fail(const char *name, const char *why) {
    g_fail++;
    printf("hooks: FAIL %s (%s)\n", name, why);
    fflush(stdout);
}

static void check(const char *name, int ok, const char *why) {
    if (ok) pass(name);
    else fail(name, why);
}

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* ---- hook state ---------------------------------------------------------------------------------------- */
static HHOOK g_hk_mouse_a, g_hk_mouse_b, g_hk_key, g_hk_get;

static unsigned ld_b_seq, ld_a_seq, ld_wp_seq;       /* WM_LBUTTONDOWN: hook B, hook A, window procedure */
static int ld_pt_ok, ld_hwnd_ok, ld_hit_ok, ld_next_ok;
static int rbd_hook_seen, rbd_wp, rbu_wp;
static int key_down41, key_up41, key_seen42, key_code_bad, key_lparam_bad;
static int wp_kd41, wp_ku41, wp_kd42, wp_ku42;
static int key_unhooked, unhook_ok, unhook_again_fails;

/* WH_GETMESSAGE: the messages we expect to be handed out, and what the hook saw them with */
static const struct { UINT msg; WPARAM wp; const char *name; } k_expect[] = {
    {WM_LBUTTONDOWN, 0, "LBUTTONDOWN"}, {WM_LBUTTONUP, 0, "LBUTTONUP"},  {WM_KEYDOWN, 0x41, "KEYDOWN 41"},
    {WM_KEYUP, 0x41, "KEYUP 41"},       {WM_RBUTTONUP, 0, "RBUTTONUP"},   {WM_KEYDOWN, 0x42, "KEYDOWN 42"},
    {WM_KEYUP, 0x42, "KEYUP 42"},
};
#define N_EXPECT ((int)(sizeof k_expect / sizeof k_expect[0]))
static int g_get_seen[N_EXPECT];   /* 1 = with PM_REMOVE, -1 = with something else */

static LRESULT CALLBACK mouse_hook_a(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_LBUTTONDOWN) ld_a_seq = ++g_seq;
    return CallNextHookEx(g_hk_mouse_a, code, wp, lp);
}

static LRESULT CALLBACK mouse_hook_b(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_LBUTTONDOWN) {
        const MOUSEHOOKSTRUCT *m = (const MOUSEHOOKSTRUCT *)lp;
        LRESULT r;
        ld_b_seq = ++g_seq;
        ld_pt_ok = m->pt.x == 150 && m->pt.y == 150;
        ld_hwnd_ok = m->hwnd == g_hwnd;
        ld_hit_ok = m->wHitTestCode == HTCLIENT;
        r = CallNextHookEx(g_hk_mouse_b, code, wp, lp);
        ld_next_ok = r == 0 && ld_a_seq > ld_b_seq;   /* the older hook ran inside the call, and its result came back */
        return r;
    }
    if (code == HC_ACTION && wp == WM_RBUTTONDOWN) {
        rbd_hook_seen++;
        return 1;   /* eat it */
    }
    return CallNextHookEx(g_hk_mouse_b, code, wp, lp);
}

static LRESULT CALLBACK key_hook(int code, WPARAM wp, LPARAM lp) {
    if (code < 0) return CallNextHookEx(g_hk_key, code, wp, lp);
    if (code != HC_ACTION) key_code_bad++;
    if (wp == 0x41) {
        int up = (lp & 0x80000000) != 0;
        if (up != (key_down41 + key_up41 == 1)) key_lparam_bad++;   /* first the down (bit 31 clear), then the up */
        if (up) key_up41++;
        else key_down41++;
    } else if (wp == 0x42) {
        key_seen42++;
    }
    return CallNextHookEx(g_hk_key, code, wp, lp);
}

static LRESULT CALLBACK get_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const MSG *m = (const MSG *)lp;
        int i;
        for (i = 0; i < N_EXPECT; i++)
            if (m->message == k_expect[i].msg && (!k_expect[i].wp || m->wParam == k_expect[i].wp))
                g_get_seen[i] = wp == PM_REMOVE ? 1 : -1;
    }
    return CallNextHookEx(g_hk_get, code, wp, lp);
}

/* ---- timers -------------------------------------------------------------------------------------------- */
#define T1_TICKS 20
static int t1_n;
static double t1_at[T1_TICKS + 1];
static int t3_wp_ticks;           /* WM_TIMER id 3 that reached the window procedure after KillTimer */
static int proc_calls_a, proc_hwnd_null, proc_id_ok, proc_msg_ok;
static UINT_PTR proc_a_id;
static int proc_calls_b;
static double proc_b_at[11];

static void CALLBACK timer_proc_a(HWND h, UINT m, UINT_PTR id, DWORD t) {
    (void)t;
    proc_calls_a++;
    proc_hwnd_null = h == NULL;
    proc_msg_ok = m == WM_TIMER;
    proc_id_ok = id == proc_a_id;
}

static void CALLBACK timer_proc_b(HWND h, UINT m, UINT_PTR id, DWORD t) {
    (void)h; (void)m; (void)id; (void)t;
    if (proc_calls_b < 11) proc_b_at[proc_calls_b] = now_ms();
    proc_calls_b++;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        HBRUSH b = CreateSolidBrush(RGB(40, 90, 40));
        GetClientRect(h, &rc);
        FillRect(dc, &rc, b);
        DeleteObject(b);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        ld_wp_seq = ++g_seq;
        return 0;
    case WM_RBUTTONDOWN:
        rbd_wp++;
        return 0;
    case WM_RBUTTONUP:
        rbu_wp++;
        /* the keyboard hook goes away before the keys 0x42 are taken off the queue */
        unhook_ok = UnhookWindowsHookEx(g_hk_key) != 0;
        SetLastError(0);
        unhook_again_fails = !UnhookWindowsHookEx(g_hk_key) && GetLastError() == ERROR_INVALID_HOOK_HANDLE;
        key_unhooked = 1;
        return 0;
    case WM_KEYDOWN:
        if (w == 0x41) wp_kd41++;
        if (w == 0x42) wp_kd42++;
        return 0;
    case WM_KEYUP:
        if (w == 0x41) wp_ku41++;
        if (w == 0x42) wp_ku42++;
        return 0;
    case WM_TIMER:
        if (w == 1) {
            if (t1_n <= T1_TICKS) t1_at[t1_n] = now_ms();
            t1_n++;
        } else if (w == 3) {
            t3_wp_ticks++;
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* Pumps messages for `ms` (PeekMessage + a bounded wait) until *stop becomes non-zero (stop may be NULL). */
static void pump(int ms, const int *stop) {
    MSG m;
    DWORD end = GetTickCount() + (DWORD)ms;
    for (;;) {
        int left;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (stop && *stop) return;
        left = (int)(end - GetTickCount());
        if (left <= 0) return;
        MsgWaitForMultipleObjectsEx(0, NULL, (DWORD)(left > 20 ? 20 : left), QS_ALLINPUT, 0);
    }
}

int main(void) {
    WNDCLASSW wc;
    MSG msg;
    int i, input_ok;
    DWORD tid = GetCurrentThreadId();

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"W10XHooks";
    if (!RegisterClassW(&wc)) {
        printf("hooks: RegisterClassW failed (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    g_hwnd = CreateWindowExW(0, L"W10XHooks", L"win10xbox hooks", WS_POPUP | WS_VISIBLE, 100, 100, 400, 300, NULL, NULL,
                             wc.hInstance, NULL);
    if (!g_hwnd) {
        printf("hooks: CreateWindowExW failed (%lu)\n", (unsigned long)GetLastError());
        return 2;
    }
    UpdateWindow(g_hwnd);

    g_hk_mouse_a = SetWindowsHookExW(WH_MOUSE, mouse_hook_a, NULL, tid);
    g_hk_mouse_b = SetWindowsHookExW(WH_MOUSE, mouse_hook_b, NULL, tid);
    g_hk_key = SetWindowsHookExW(WH_KEYBOARD, key_hook, NULL, tid);
    g_hk_get = SetWindowsHookExW(WH_GETMESSAGE, get_hook, NULL, tid);
    printf("hooks: installed %p %p %p %p\n", (void *)g_hk_mouse_a, (void *)g_hk_mouse_b, (void *)g_hk_key, (void *)g_hk_get);
    fflush(stdout);
    if (!g_hk_mouse_a || !g_hk_mouse_b || !g_hk_key || !g_hk_get) {
        printf("hooks: SetWindowsHookExW failed (%lu)\n", (unsigned long)GetLastError());
        return 2;
    }

    /* input phase: until the last injected key has been dispatched */
    pump(10000, &wp_ku42);
    pump(100, NULL);   /* anything extra shows up in the counters */
    input_ok = wp_ku42 == 1;
    check("injected input arrived", input_ok, "the script's last key never reached the window procedure");

    check("WH_MOUSE sees LBUTTONDOWN before the window procedure",
          ld_b_seq && ld_wp_seq > ld_b_seq && ld_pt_ok && ld_hwnd_ok && ld_hit_ok,
          ld_b_seq ? (!ld_pt_ok ? "pt is not 150,150" : !ld_hwnd_ok ? "hwnd is not our window" : !ld_hit_ok ? "hit test is not HTCLIENT" : "window procedure ran first")
                   : "hook never called");
    check("WH_MOUSE chain runs newest first and CallNextHookEx reaches the older hook",
          ld_b_seq && ld_a_seq > ld_b_seq && ld_next_ok, "older hook did not run inside CallNextHookEx or lost its result");
    check("WH_KEYBOARD sees vk 41 down and up", key_down41 == 1 && key_up41 == 1 && !key_code_bad && !key_lparam_bad && wp_kd41 == 1 && wp_ku41 == 1,
          "down/up counts or HC_ACTION / transition bit wrong");
    check("hook returning 1 eats WM_RBUTTONDOWN", rbd_hook_seen == 1 && rbd_wp == 0 && rbu_wp == 1,
          rbd_hook_seen != 1 ? "hook did not see WM_RBUTTONDOWN" : rbd_wp ? "window procedure got WM_RBUTTONDOWN" : "WM_RBUTTONUP did not arrive");
    check("UnhookWindowsHookEx stops WH_KEYBOARD", key_unhooked && unhook_ok && unhook_again_fails && key_seen42 == 0 && wp_kd42 == 1 && wp_ku42 == 1,
          !unhook_ok ? "unhook failed" : !unhook_again_fails ? "second unhook did not fail with ERROR_INVALID_HOOK_HANDLE" : key_seen42 ? "hook saw vk 42" : "vk 42 did not reach the window procedure");
    {
        int all = 1;
        char why[96] = "";
        for (i = 0; i < N_EXPECT; i++)
            if (g_get_seen[i] != 1) {
                all = 0;
                snprintf(why, sizeof why, "%s %s", k_expect[i].name, g_get_seen[i] ? "seen without PM_REMOVE" : "not seen");
                break;
            }
        check("WH_GETMESSAGE sees every message with PM_REMOVE", all, why);
    }

    /* timers: SetTimer(hwnd, 1, 50) for 20 ticks while the thread sits in GetMessage */
    {
        ULONGLONG start = GetTickCount64();
        double min_gap = 1e9, gap;
        ULONGLONG total;
        char why[96];
        SetTimer(g_hwnd, 1, 50, NULL);
        while (t1_n < T1_TICKS && GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        KillTimer(g_hwnd, 1);
        total = GetTickCount64() - start;
        for (i = 1; i < T1_TICKS; i++) {
            gap = t1_at[i] - t1_at[i - 1];
            if (gap < min_gap) min_gap = gap;
        }
        printf("hooks: timer 50 ms x%d: total %llu ms, shortest interval %.1f ms\n", T1_TICKS, (unsigned long long)total, min_gap);
        snprintf(why, sizeof why, "total %llu ms (want 950..1400), shortest interval %.1f ms (want >= 40)", (unsigned long long)total, min_gap);
        check("SetTimer 50 ms cadence", t1_n == T1_TICKS && total >= 950 && total <= 1400 && min_gap >= 40.0, why);
    }

    /* SetTimer(NULL, 0, 1, proc): clamped to 10 ms */
    {
        double t0 = now_ms(), elapsed;
        UINT_PTR id = SetTimer(NULL, 0, 1, timer_proc_b);
        char why[96];
        while (proc_calls_b < 10 && GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        KillTimer(NULL, id);
        elapsed = now_ms() - t0;
        printf("hooks: timer 1 ms (clamped) x10: %.1f ms\n", elapsed);
        snprintf(why, sizeof why, "10 ticks took %.1f ms (want >= 95), id %llu", elapsed, (unsigned long long)id);
        check("SetTimer minimum is 10 ms", id != 0 && proc_calls_b >= 10 && elapsed >= 95.0, why);
    }

    /* KillTimer removes the WM_TIMER that was already due */
    {
        SetTimer(g_hwnd, 3, 30, NULL);
        Sleep(80);   /* due, not yet taken off the queue */
        KillTimer(g_hwnd, 3);
        pump(200, NULL);
        check("KillTimer removes the pending WM_TIMER", t3_wp_ticks == 0, "WM_TIMER arrived after KillTimer");
    }

    /* a timer with a TIMERPROC: the message carries it, DispatchMessage calls it */
    {
        MSG t;
        int got = 0, before;
        DWORD tick;
        proc_a_id = SetTimer(NULL, 0, 20, timer_proc_a);
        Sleep(60);
        tick = GetTickCount();
        for (i = 0; i < 50 && !got; i++) {
            got = PeekMessageW(&t, NULL, WM_TIMER, WM_TIMER, PM_REMOVE);
            if (!got) Sleep(10);
        }
        before = proc_calls_a;
        if (got) DispatchMessageW(&t);
        KillTimer(NULL, proc_a_id);
        check("TIMERPROC is called through DispatchMessage",
              got && t.lParam == (LPARAM)timer_proc_a && before == 0 && proc_calls_a == 1 && proc_hwnd_null && proc_msg_ok && proc_id_ok,
              !got ? "no WM_TIMER" : t.lParam != (LPARAM)timer_proc_a ? "lParam is not the TIMERPROC" : before ? "called before DispatchMessage" : "not called, or called with wrong arguments");
        check("message time is the tick count", got && (int)(tick - t.time) >= -150 && (int)(tick - t.time) < 150, "MSG.time is not close to GetTickCount");
    }

    printf("hooks: %d passed, %d failed\n", g_pass, g_fail);
    printf("hooks: done\n");
    fflush(stdout);
    if (!input_ok) return 3;
    return g_fail ? 1 : 42;
}
