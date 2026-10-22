/*
 * window.c - placeholder GUI guest bundled with host-uwp (window.exe), the packaged default
 * guest: the first thing the owner sees on the TV after installing the package.
 *
 * Used only while tests/guest/window.c (the GUI guest of the core/user work) does not exist in
 * the tree; host-uwp/CMakeLists.txt builds that one instead as soon as it does.
 *
 * A plain x64 Win32 program (static CRT, no console): RegisterClassW, CreateWindowExW 640x400
 * visible, WM_PAINT fills the client area with solid blue, a 3 s SetTimer posts WM_QUIT, and the
 * program returns 42 (payload\launch.json expects it). The window is deliberately NOT destroyed
 * before the exit, so the last composed frame the presenter keeps showing still has it.
 * A line per step goes to stdout (LocalState\stdout.txt) through WriteFile, not CRT stdio.
 */
#include <windows.h>

#define BLUE RGB(0, 80, 220)
#define QUIT_TIMER 1

static void say(const char* text) {
    DWORD put = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != NULL && out != INVALID_HANDLE_VALUE) WriteFile(out, text, lstrlenA(text), &put, NULL);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(hwnd, &ps);
        HBRUSH blue = CreateSolidBrush(BLUE);
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, blue);
        DeleteObject(blue);
        EndPaint(hwnd, &ps);
        say("window: painted\r\n");
        return 0;
    }
    case WM_TIMER:
        if (wp == QUIT_TIMER) {
            KillTimer(hwnd, QUIT_TIMER);
            say("window: timer fired, quitting\r\n");
            PostQuitMessage(42);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show) {
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;
    char line[128];
    (void)prev;
    (void)cmdline;
    (void)show;

    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"win10xbox_window";
    if (!RegisterClassW(&wc)) {
        wsprintfA(line, "window: RegisterClassW failed (error %lu)\r\n", GetLastError());
        say(line);
        return 1;
    }
    say("window: class registered\r\n");

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"win10xbox", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 400,
                           NULL, NULL, inst, NULL);
    if (!hwnd) {
        wsprintfA(line, "window: CreateWindowExW failed (error %lu)\r\n", GetLastError());
        say(line);
        return 2;
    }
    wsprintfA(line, "window: created hwnd=%p\r\n", (void*)hwnd);
    say(line);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (!SetTimer(hwnd, QUIT_TIMER, 3000, NULL)) say("window: SetTimer failed\r\n");

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    say("window: done\r\n");
    return 42;
}
