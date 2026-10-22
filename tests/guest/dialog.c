/* guest_dialog: DialogBoxParamW through Wine's user32 and core/user. The dialog template is a DIALOG resource
 * (dialog.rc). In WM_INITDIALOG the program places the dialog at screen (100,100), moves the OK button to client
 * (10,10) size 100x30 (so the click position does not depend on dialog units), sets the edit text and reads it back.
 * The host injects a click on the OK button's centre, screen (160,125), with WIN10XBOX_TEST_INJECT_INPUT; the button
 * sends WM_COMMAND(IDOK) and the dialog procedure calls EndDialog(hDlg, 7). An 8 s timer ends the dialog with -2.
 * The program exits 42 when DialogBoxParamW returned 7 and every check passed.
 *
 *   dialog: init edit=100 button=1
 *   dialog: text=hello dialog
 *   dialog: class=#32770 edit=Edit button=Button static=Static
 *   dialog: focus=edit
 *   dialog: ok clicked
 *   dialog: result=7
 *   dialog: done
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define IDC_EDIT       100
#define IDC_STATIC     101
#define TIMER_WATCHDOG 1

static int g_fail;
static int g_clicked;

static void check(int ok, const char* what) {
    if (!ok) {
        printf("dialog: CHECK FAILED: %s (last error %lu)\n", what, (unsigned long)GetLastError());
        g_fail++;
    }
    fflush(stdout);
}

static INT_PTR CALLBACK dlgproc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        WCHAR buf[64];
        WCHAR cls[32];
        HWND edit = GetDlgItem(dlg, IDC_EDIT);
        HWND ok = GetDlgItem(dlg, IDOK);
        HWND stat = GetDlgItem(dlg, IDC_STATIC);
        check(lp == 0x1234, "lParam passed through DialogBoxParam");
        check(edit && ok && stat && GetDlgItem(dlg, IDCANCEL), "GetDlgItem finds every control");
        printf("dialog: init edit=%d button=%d\n", (int)GetDlgCtrlID(edit), (int)GetDlgCtrlID(ok));
        SetWindowPos(dlg, NULL, 100, 100, 160, 80, SWP_NOZORDER | SWP_NOACTIVATE);
        MoveWindow(ok, 10, 10, 100, 30, FALSE);
        check(SetDlgItemTextW(dlg, IDC_EDIT, L"hello dialog"), "SetDlgItemText");
        memset(buf, 0, sizeof buf);
        check(GetDlgItemTextW(dlg, IDC_EDIT, buf, 64) == 12 && !wcscmp(buf, L"hello dialog"), "GetDlgItemText");
        printf("dialog: text=%ls\n", buf);
        GetClassNameW(dlg, cls, 32);
        printf("dialog: class=%ls", cls);
        GetClassNameW(edit, cls, 32);
        printf(" edit=%ls", cls);
        GetClassNameW(ok, cls, 32);
        printf(" button=%ls", cls);
        GetClassNameW(stat, cls, 32);
        printf(" static=%ls\n", cls);
        check(GetNextDlgTabItem(dlg, edit, FALSE) == ok, "tab order edit -> OK");
        check(GetNextDlgTabItem(dlg, ok, TRUE) == edit, "tab order OK <- edit");
        check(GetParent(edit) == dlg && GetDlgItem(dlg, 9999) == NULL, "GetParent / missing item");
        SetTimer(dlg, TIMER_WATCHDOG, 8000, NULL);
        SetFocus(edit);
        printf("dialog: focus=%s\n", GetFocus() == edit ? "edit" : "other");
        fflush(stdout);
        return FALSE;   /* we set the focus ourselves */
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            WCHAR buf[64];
            g_clicked = 1;
            GetDlgItemTextW(dlg, IDC_EDIT, buf, 64);
            check(!wcscmp(buf, L"hello dialog"), "edit text survives until OK");
            printf("dialog: ok clicked\n");
            fflush(stdout);
            KillTimer(dlg, TIMER_WATCHDOG);
            EndDialog(dlg, 7);
            return TRUE;
        }
        return FALSE;
    case WM_TIMER:
        if (wp == TIMER_WATCHDOG) {
            printf("dialog: timeout, no click arrived\n");
            fflush(stdout);
            EndDialog(dlg, -2);
        }
        return TRUE;
    }
    return FALSE;
}

int main(void) {
    INT_PTR r;
    HINSTANCE inst = GetModuleHandleW(NULL);
    r = DialogBoxParamW(inst, MAKEINTRESOURCEW(101), NULL, dlgproc, 0x1234);
    printf("dialog: result=%d\n", (int)r);
    check(r == 7, "EndDialog return value");
    check(g_clicked, "OK click arrived");
    printf("dialog: done\n");
    fflush(stdout);
    return g_fail ? 1 : 42;
}
