/* shell_launch: a program started by the shell (explorer.exe as the desktop, core/dispatch/vprocess.h) that starts a
 * second program. Both are virtual processes of one host: this one has its own window (WS_POPUP 300x200 at 600,100,
 * solid green) and starts C:\guest\guest_window.exe (400x300 at 100,100, blue) with CreateProcess. It waits up to 10 s
 * for that process's window (FindWindow by class), checks that the window belongs to the process CreateProcess returned
 * (GetWindowThreadProcessId), that the process is still running (GetExitCodeProcess) and that its pid differs from
 * ours, prints
 *   shell_launch: child pid <n> window ok
 * and then stays up (the host script takes the screenshot of the desktop with both windows and the taskbar and quits).
 * Exits 42 after a minute at the latest. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH green = CreateSolidBrush(RGB(0, 160, 0));
        FillRect(dc, &rc, green);
        DeleteObject(green);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(void) {
    WNDCLASSW wc;
    HWND hwnd, child;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t cmd[] = L"C:\\guest\\guest_window.exe 60";
    DWORD tid, pid = 0, code = 0;
    int i;
    MSG msg;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"W10XShellLaunch";
    if (!RegisterClassW(&wc)) { printf("shell_launch: RegisterClassW failed (%lu)\n", GetLastError()); return 1; }
    hwnd = CreateWindowExW(0, L"W10XShellLaunch", L"shell_launch", WS_POPUP | WS_VISIBLE, 600, 100, 300, 200, NULL, NULL,
                           wc.hInstance, NULL);
    if (!hwnd) { printf("shell_launch: CreateWindowExW failed (%lu)\n", GetLastError()); return 2; }
    printf("shell_launch: own pid %lu window %p\n", GetCurrentProcessId(), (void *)hwnd);
    fflush(stdout);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("shell_launch: CreateProcessW failed (%lu)\n", GetLastError());
        return 3;
    }
    if (pi.dwProcessId == GetCurrentProcessId() || !pi.dwProcessId) {
        printf("shell_launch: child pid %lu is not a process of its own\n", pi.dwProcessId);
        return 4;
    }
    child = NULL;
    for (i = 0; i < 200 && !child; ++i) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        child = FindWindowW(L"W10XWindow", NULL);
        if (!child) Sleep(50);
    }
    if (!child) { printf("shell_launch: the child's window never appeared\n"); return 5; }
    tid = GetWindowThreadProcessId(child, &pid);
    if (!tid || pid != pi.dwProcessId) {
        printf("shell_launch: window %p belongs to pid %lu, CreateProcess said %lu\n", (void *)child, pid, pi.dwProcessId);
        return 6;
    }
    if (!GetExitCodeProcess(pi.hProcess, &code) || code != STILL_ACTIVE) {
        printf("shell_launch: child state %lu (error %lu)\n", code, GetLastError());
        return 7;
    }
    printf("shell_launch: child pid %lu window ok\n", pi.dwProcessId);
    fflush(stdout);

    for (i = 0; i < 1200; ++i) {   /* stay up for the screenshot */
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(50);
    }
    return 42;
}
