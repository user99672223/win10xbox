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
 *   7. clipboard format listeners
 * Exit 42 when everything holds; otherwise the number of the first failing step.
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
    if (msg == WM_DESTROYCLIPBOARD) {
        g_destroy_calls++;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
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
    HWND hwnd;
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
    printf("clipboard: window created\n");
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
    printf("clipboard: set unicode text seq %lu -> %lu\n", (unsigned long)seq0, (unsigned long)seq1);
    fflush(stdout);
    CHECK(2, seq1 > seq0);
    CHECK(2, GetOpenClipboardWindow() == NULL);

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
    printf("clipboard: CF_UNICODETEXT ok\n");

    /* CF_TEXT, synthesized */
    h = GetClipboardData(CF_TEXT);
    CHECK(4, h != NULL);
    {
        const char* p = (const char*)GlobalLock(h);
        CHECK(4, p && strcmp(p, kTextA) == 0);
        printf("clipboard: CF_TEXT=%s\n", p);
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
        printf("clipboard: enum formats=%d\n", n);
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
        printf("clipboard: custom format round trip ok seq %lu\n", (unsigned long)seq2);
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
    printf("clipboard: delayed render calls=%d\n", g_render_calls);

    /* listeners */
    CHECK(7, AddClipboardFormatListener(hwnd));
    CHECK(7, !AddClipboardFormatListener(hwnd));
    CHECK(7, RemoveClipboardFormatListener(hwnd));
    CHECK(7, !RemoveClipboardFormatListener(hwnd));

    printf("clipboard: done\n");
    fflush(stdout);
    return 42;
}
