/* guest_clipboard: the clipboard through user32 -> win32u syscalls -> core/user (win_clipboard.cpp).
 * A console-subsystem program (stdout is the evidence) that owns a hidden popup window and checks,
 * printing a line per step so a failure is attributable from stdout alone:
 *   2. OpenClipboard/EmptyClipboard/SetClipboardData(CF_UNICODETEXT)/CloseClipboard; the sequence
 *      number increased and GetClipboardOwner / GetOpenClipboardWindow are right
 *   3. IsClipboardFormatAvailable / CountClipboardFormats (CF_TEXT, CF_OEMTEXT, CF_LOCALE are
 *      synthesized when the clipboard is closed), read back as CF_UNICODETEXT
 *   4. read back as CF_TEXT (synthesized from the Unicode text), EnumClipboardFormats, error codes
 *   5. a registered format (RegisterClipboardFormatW, GetClipboardFormatNameW) round-trips its
 *      bytes; EmptyClipboard sends WM_DESTROYCLIPBOARD to the previous owner
 *   6. delayed rendering: SetClipboardData(CF_UNICODETEXT, NULL) and the WM_RENDERFORMAT answer
 *   7. clipboard format listeners: a second window registered with AddClipboardFormatListener receives
 *      WM_CLIPBOARDUPDATE when another window changes the clipboard, and stops receiving it after
 *      RemoveClipboardFormatListener
 * Every passed check prints "clipboard: <name> ok". Exit 42 when everything holds; otherwise the number
 * of the first failing step.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static const WCHAR kText[] = L"Hello Xbox clipboard";
static const char kTextA[] = "Hello Xbox clipboard";
static const WCHAR kRendered[] = L"rendered on demand";
static int g_render_calls;
static int g_destroy_calls;
static int g_update_calls;   /* WM_CLIPBOARDUPDATE seen by the listener window */

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_RENDERFORMAT) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(kRendered));
        void* p = GlobalLock(h);
        memcpy(p, kRendered, sizeof(kRendered));
        GlobalUnlock(h);
        g_render_calls++;
        if (wp == CF_UNICODETEXT) SetClipboardData(CF_UNICODETEXT, h);
        return 0;
    }
    if (msg == WM_CLIPBOARDUPDATE) {
        g_update_calls++;
        return 0;
    }
    if (msg == WM_DESTROYCLIPBOARD) {
        g_destroy_calls++;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Dispatches the thread's posted messages until `*counter` differs from `until_ne` or `ms` ran out. */
static void pump(const int* counter, int until_ne, DWORD ms) {
    DWORD start = GetTickCount();
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (*counter != until_ne || GetTickCount() - start >= ms) return;
        MsgWaitForMultipleObjectsEx(0, NULL, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

static HGLOBAL make_text(const WCHAR* s) {
    size_t n = (wcslen(s) + 1) * sizeof(WCHAR);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    void* p = GlobalLock(h);
    memcpy(p, s, n);
    GlobalUnlock(h);
    return h;
}

#define CHECK(step, cond)                                                                                      \
    do {                                                                                                       \
        if (!(cond)) {                                                                                         \
            printf("clipboard: FAIL step %d: %s (err %lu)\n", step, #cond, (unsigned long)GetLastError());     \
            fflush(stdout);                                                                                    \
            return step;                                                                                       \
        }                                                                                                      \
    } while (0)

int main(void) {
    WNDCLASSW wc;
    HWND hwnd, hwnd2;
    DWORD seq0, seq1, seq2;
    HANDLE h;
    UINT fmt;
    UINT custom;
    WCHAR name[64];

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"W10XClipboard";
    CHECK(1, RegisterClassW(&wc));
    hwnd = CreateWindowExW(0, L"W10XClipboard", L"clip", WS_POPUP, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    CHECK(1, hwnd != NULL);
    hwnd2 = CreateWindowExW(0, L"W10XClipboard", L"clip2", WS_POPUP, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    CHECK(1, hwnd2 != NULL && hwnd2 != hwnd);
    printf("clipboard: window ok\n");
    fflush(stdout);

    /* set CF_UNICODETEXT */
    seq0 = GetClipboardSequenceNumber();
    CHECK(2, OpenClipboard(hwnd));
    CHECK(2, GetOpenClipboardWindow() == hwnd);
    CHECK(2, EmptyClipboard());
    CHECK(2, GetClipboardOwner() == hwnd);
    h = make_text(kText);
    CHECK(2, SetClipboardData(CF_UNICODETEXT, h) == h);
    CHECK(2, CloseClipboard());
    seq1 = GetClipboardSequenceNumber();
    CHECK(2, GetOpenClipboardWindow() == NULL);
    printf("clipboard: set-unicode ok\n");
    CHECK(2, seq1 > seq0);
    printf("clipboard: sequence-number ok seq %lu -> %lu\n", (unsigned long)seq0, (unsigned long)seq1);
    fflush(stdout);

    /* formats and the Unicode read-back */
    CHECK(3, IsClipboardFormatAvailable(CF_UNICODETEXT));
    CHECK(3, IsClipboardFormatAvailable(CF_TEXT));
    CHECK(3, IsClipboardFormatAvailable(CF_OEMTEXT));
    CHECK(3, !IsClipboardFormatAvailable(CF_BITMAP));
    CHECK(3, CountClipboardFormats() == 4); /* UNICODETEXT + LOCALE + TEXT + OEMTEXT */
    CHECK(3, OpenClipboard(hwnd));
    h = GetClipboardData(CF_UNICODETEXT);
    CHECK(3, h != NULL);
    {
        const WCHAR* p = (const WCHAR*)GlobalLock(h);
        CHECK(3, p && wcscmp(p, kText) == 0);
        GlobalUnlock(h);
    }
    printf("clipboard: get-unicode ok\n");

    /* CF_TEXT, synthesized */
    h = GetClipboardData(CF_TEXT);
    CHECK(4, h != NULL);
    {
        const char* p = (const char*)GlobalLock(h);
        CHECK(4, p && strcmp(p, kTextA) == 0);
        printf("clipboard: synthesized-text ok CF_TEXT=%s\n", p);
        GlobalUnlock(h);
    }
    fmt = 0;
    {
        int n = 0, saw_unicode = 0, saw_text = 0;
        while ((fmt = EnumClipboardFormats(fmt)) != 0) {
            n++;
            if (fmt == CF_UNICODETEXT) saw_unicode = 1;
            if (fmt == CF_TEXT) saw_text = 1;
        }
        CHECK(4, n == 4 && saw_unicode && saw_text);
        CHECK(4, GetLastError() == ERROR_SUCCESS);
        printf("clipboard: enum-formats ok count=%d\n", n);
    }
    CHECK(4, GetClipboardData(CF_BITMAP) == NULL);
    CHECK(4, CloseClipboard());
    CHECK(4, !CloseClipboard());
    CHECK(4, GetLastError() == ERROR_CLIPBOARD_NOT_OPEN);
    fflush(stdout);

    /* registered format */
    custom = RegisterClipboardFormatW(L"W10X.CustomFormat");
    CHECK(5, custom >= 0xC000);
    CHECK(5, RegisterClipboardFormatW(L"W10X.CustomFormat") == custom);
    CHECK(5, GetClipboardFormatNameW(custom, name, 64) == 17 && wcscmp(name, L"W10X.CustomFormat") == 0);
    {
        static const unsigned char blob[] = {1, 2, 3, 4, 5, 0xfe, 0xff, 0};
        HGLOBAL hb = GlobalAlloc(GMEM_MOVEABLE, sizeof blob);
        unsigned char* p = (unsigned char*)GlobalLock(hb);
        memcpy(p, blob, sizeof blob);
        GlobalUnlock(hb);
        CHECK(5, OpenClipboard(hwnd));
        CHECK(5, EmptyClipboard());
        CHECK(5, g_destroy_calls == 1);
        CHECK(5, SetClipboardData(custom, hb) == hb);
        CHECK(5, CloseClipboard());
        seq2 = GetClipboardSequenceNumber();
        CHECK(5, seq2 > seq1);
        CHECK(5, IsClipboardFormatAvailable(custom));
        CHECK(5, !IsClipboardFormatAvailable(CF_UNICODETEXT));
        CHECK(5, OpenClipboard(hwnd));
        h = GetClipboardData(custom);
        CHECK(5, h != NULL);
        CHECK(5, GlobalSize(h) >= sizeof blob);
        p = (unsigned char*)GlobalLock(h);
        CHECK(5, p && memcmp(p, blob, sizeof blob) == 0);
        GlobalUnlock(h);
        CHECK(5, CloseClipboard());
        printf("clipboard: registered-format ok seq %lu\n", (unsigned long)seq2);
        fflush(stdout);
    }

    /* delayed rendering */
    CHECK(6, OpenClipboard(hwnd));
    CHECK(6, EmptyClipboard());
    CHECK(6, SetClipboardData(CF_UNICODETEXT, NULL) == NULL);
    CHECK(6, CloseClipboard());
    CHECK(6, IsClipboardFormatAvailable(CF_UNICODETEXT));
    CHECK(6, OpenClipboard(hwnd));
    h = GetClipboardData(CF_UNICODETEXT);
    CHECK(6, h != NULL);
    {
        const WCHAR* p = (const WCHAR*)GlobalLock(h);
        CHECK(6, p && wcscmp(p, kRendered) == 0);
        GlobalUnlock(h);
    }
    CHECK(6, g_render_calls == 1);
    CHECK(6, CloseClipboard());
    printf("clipboard: delayed-render ok calls=%d\n", g_render_calls);

    /* listeners: hwnd2 is registered, hwnd changes the clipboard */
    CHECK(7, AddClipboardFormatListener(hwnd2));
    g_update_calls = 0;
    CHECK(7, OpenClipboard(hwnd));
    CHECK(7, EmptyClipboard());
    h = make_text(kText);
    CHECK(7, SetClipboardData(CF_UNICODETEXT, h) == h);
    CHECK(7, g_update_calls == 0); /* only sent when the clipboard is closed */
    CHECK(7, CloseClipboard());
    pump(&g_update_calls, 0, 5000);
    CHECK(7, g_update_calls == 1);
    printf("clipboard: listener-update ok count=%d\n", g_update_calls);
    CHECK(7, RemoveClipboardFormatListener(hwnd2));
    CHECK(7, OpenClipboard(hwnd));
    CHECK(7, EmptyClipboard());
    CHECK(7, CloseClipboard());
    pump(&g_update_calls, 1, 300);
    CHECK(7, g_update_calls == 1);
    printf("clipboard: listener-removed ok\n");

    printf("clipboard: done\n");
    fflush(stdout);
    return 42;
}
