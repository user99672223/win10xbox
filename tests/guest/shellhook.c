/* Shell hook messages (core/user/win_shell.cpp): a program registers a window with RegisterShellHookWindow and gets
 * WM_SHELLHOOK (the registered message "SHELLHOOK") for the top-level windows other threads create, activate and
 * destroy: HSHELL_WINDOWCREATED, HSHELL_WINDOWACTIVATED, HSHELL_WINDOWDESTROYED, with the window in lParam.
 * A console program that makes windows; prints "shellhook: created ok", "shellhook: activated ok",
 * "shellhook: destroyed ok" and exits 42. */
#include <windows.h>
#include <stdio.h>

#ifndef HSHELL_WINDOWCREATED
#define HSHELL_WINDOWCREATED 1
#define HSHELL_WINDOWDESTROYED 2
#define HSHELL_WINDOWACTIVATED 4
#endif

static UINT g_shellhook_msg;
static HWND g_hook_window;
static volatile HWND g_other;               /* the window the second thread created */
static volatile LONG g_other_ready, g_other_destroy;
static HWND g_created, g_activated, g_destroyed;
static int g_unexpected;

static LRESULT CALLBACK hook_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_shellhook_msg && g_shellhook_msg) {
        HWND w = (HWND)lp;
        printf("shellhook: message code=%u window=%p\n", (unsigned)wp, (void *)w);
        if (w == g_hook_window) return 0;   /* the hook window itself is hidden: not announced, but be tolerant */
        switch (wp) {
        case HSHELL_WINDOWCREATED: g_created = w; break;
        case HSHELL_WINDOWACTIVATED: g_activated = w; break;
        case HSHELL_WINDOWDESTROYED: g_destroyed = w; break;
        default: break;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK other_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI other_thread(void *arg) {
    WNDCLASSW wc = {0};
    HWND w;
    MSG msg;
    (void)arg;
    wc.lpfnWndProc = other_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ShellHookOther";
    RegisterClassW(&wc);
    w = CreateWindowExW(0, L"ShellHookOther", L"other window", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 150, 320, 200,
                        NULL, NULL, wc.hInstance, NULL);
    g_other = w;
    InterlockedExchange(&g_other_ready, 1);
    while (!g_other_destroy) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(10);
    }
    DestroyWindow(w);
    return 0;
}

static void pump(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    MSG msg;
    while ((LONG)(end - GetTickCount()) > 0) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 20, QS_ALLINPUT);
    }
}

int main(void) {
    WNDCLASSW wc = {0};
    HANDLE thread;
    int tries;

    g_shellhook_msg = RegisterWindowMessageW(L"SHELLHOOK");
    if (!g_shellhook_msg) { printf("FAIL RegisterWindowMessage(SHELLHOOK) err=%lu\n", GetLastError()); return 1; }
    wc.lpfnWndProc = hook_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ShellHookWindow";
    RegisterClassW(&wc);
    g_hook_window = CreateWindowExW(WS_EX_TOOLWINDOW, L"ShellHookWindow", L"hook", WS_POPUP, 0, 0, 10, 10, NULL, NULL,
                                    wc.hInstance, NULL);
    if (!g_hook_window) { printf("FAIL CreateWindow hook window err=%lu\n", GetLastError()); return 1; }
    if (!RegisterShellHookWindow(g_hook_window)) { printf("FAIL RegisterShellHookWindow err=%lu\n", GetLastError()); return 1; }
    printf("shellhook: registered, message=%u\n", g_shellhook_msg);

    thread = CreateThread(NULL, 0, other_thread, NULL, 0, NULL);
    if (!thread) { printf("FAIL CreateThread\n"); return 1; }
    for (tries = 0; tries < 500 && !g_other_ready; ++tries) pump(10);
    if (!g_other) { printf("FAIL the second window was not created\n"); return 1; }
    for (tries = 0; tries < 300 && !(g_created && g_activated); ++tries) pump(10);

    if (g_created == g_other) printf("shellhook: created ok\n");
    else { printf("FAIL HSHELL_WINDOWCREATED window=%p expected %p\n", (void *)g_created, (void *)g_other); ++g_unexpected; }
    if (g_activated == g_other) printf("shellhook: activated ok\n");
    else { printf("FAIL HSHELL_WINDOWACTIVATED window=%p expected %p\n", (void *)g_activated, (void *)g_other); ++g_unexpected; }

    InterlockedExchange(&g_other_destroy, 1);
    for (tries = 0; tries < 300 && !g_destroyed; ++tries) pump(10);
    if (g_destroyed == g_other) printf("shellhook: destroyed ok\n");
    else { printf("FAIL HSHELL_WINDOWDESTROYED window=%p expected %p\n", (void *)g_destroyed, (void *)g_other); ++g_unexpected; }

    if (!DeregisterShellHookWindow(g_hook_window)) { printf("FAIL DeregisterShellHookWindow err=%lu\n", GetLastError()); ++g_unexpected; }
    WaitForSingleObject(thread, 5000);
    return g_unexpected ? 1 : 42;
}
