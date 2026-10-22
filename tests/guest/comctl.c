/* guest_comctl: Wine's builtin comctl32 through the layer. InitCommonControlsEx, then a ListView (report view,
 * 3 items), a TreeView (2 nodes), a progress bar, a trackbar and a status bar inside one popup window. The state is
 * read back with messages (LVM_GETITEMCOUNT, TVM_GETCOUNT, PBM_GETPOS, TBM_GETPOS, SB_GETPARTS), the controls are
 * painted (UpdateWindow) and each control's client area is sampled with GetPixel: the pixels must not be uniform.
 * Exit 42 only if everything held (4 = a check failed, 2 = setup failed, 3 = watchdog).
 *
 * Output lines (the integration test matches them):
 *   comctl: init ok
 *   comctl: created lv=1 tv=1 pb=1 tb=1 sb=1
 *   comctl: lv count=3 tv count=2 pb pos=60 tb pos=7 sb parts=2
 *   comctl: distinct colours lv=N tv=N pb=N tb=N sb=N
 *   comctl: done
 */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* number of distinct colours in a 16x8 grid over the control's client area */
static int distinct(HWND h) {
    COLORREF seen[128];
    int n = 0, x, y, i;
    RECT rc;
    HDC dc;
    GetClientRect(h, &rc);
    dc = GetDC(h);
    for (y = 0; y < 8; y++)
        for (x = 0; x < 16; x++) {
            COLORREF c = GetPixel(dc, rc.right * x / 16, rc.bottom * y / 8);
            for (i = 0; i < n; i++) if (seen[i] == c) break;
            if (i == n) seen[n++] = c;
        }
    ReleaseDC(h, dc);
    return n;
}

int main(void) {
    INITCOMMONCONTROLSEX icc;
    WNDCLASSW wc;
    HWND wnd, lv, tv, pb, tb, sb;
    HINSTANCE inst = GetModuleHandleW(NULL);
    int i, ok = 1, lvc, tvc, pbp, tbp, sbp, d[5];
    MSG msg;
    LVCOLUMNW col;
    LVITEMW item;
    TVINSERTSTRUCTW tvi;
    HTREEITEM root;
    int parts[2] = {200, -1};
    static const WCHAR *names[3] = {L"alpha", L"beta", L"gamma"};

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS;
    if (!InitCommonControlsEx(&icc)) { printf("comctl: InitCommonControlsEx failed (%lu)\n", GetLastError()); return 2; }
    printf("comctl: init ok\n");

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"W10XComctl";
    if (!RegisterClassW(&wc)) { printf("comctl: RegisterClass failed (%lu)\n", GetLastError()); return 2; }
    wnd = CreateWindowExW(0, L"W10XComctl", L"comctl", WS_POPUP | WS_VISIBLE, 100, 100, 400, 300, NULL, NULL, inst, NULL);
    if (!wnd) { printf("comctl: CreateWindow failed (%lu)\n", GetLastError()); return 2; }

    lv = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 5, 5, 190, 120, wnd, (HMENU)1, inst, NULL);
    tv = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_HASBUTTONS, 205, 5, 190, 120, wnd, (HMENU)2, inst, NULL);
    pb = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 5, 135, 190, 20, wnd, (HMENU)3, inst, NULL);
    tb = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 205, 135, 190, 40, wnd, (HMENU)4, inst, NULL);
    sb = CreateWindowExW(0, STATUSCLASSNAMEW, L"ready", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, wnd, (HMENU)5, inst, NULL);
    printf("comctl: created lv=%d tv=%d pb=%d tb=%d sb=%d\n", lv != 0, tv != 0, pb != 0, tb != 0, sb != 0);
    if (!lv || !tv || !pb || !tb || !sb) { printf("comctl: control creation failed (%lu)\n", GetLastError()); return 2; }

    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 90; col.pszText = (LPWSTR)L"Name";
    SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 80; col.pszText = (LPWSTR)L"Size";
    SendMessageW(lv, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    for (i = 0; i < 3; i++) {
        memset(&item, 0, sizeof item);
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = (LPWSTR)names[i];
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&item);
    }
    memset(&tvi, 0, sizeof tvi);
    tvi.hParent = TVI_ROOT; tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask = TVIF_TEXT; tvi.item.pszText = (LPWSTR)L"root";
    root = (HTREEITEM)SendMessageW(tv, TVM_INSERTITEMW, 0, (LPARAM)&tvi);
    tvi.hParent = root; tvi.item.pszText = (LPWSTR)L"child";
    SendMessageW(tv, TVM_INSERTITEMW, 0, (LPARAM)&tvi);
    SendMessageW(tv, TVM_EXPAND, TVE_EXPAND, (LPARAM)root);

    SendMessageW(pb, PBM_SETRANGE32, 0, 100);
    SendMessageW(pb, PBM_SETPOS, 60, 0);
    SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELONG(0, 10));
    SendMessageW(tb, TBM_SETPOS, TRUE, 7);
    SendMessageW(sb, SB_SETPARTS, 2, (LPARAM)parts);
    SendMessageW(sb, SB_SETTEXTW, 0, (LPARAM)L"ready");
    SendMessageW(sb, SB_SETTEXTW, 1, (LPARAM)L"part two");

    lvc = (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0);
    tvc = (int)SendMessageW(tv, TVM_GETCOUNT, 0, 0);
    pbp = (int)SendMessageW(pb, PBM_GETPOS, 0, 0);
    tbp = (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
    sbp = (int)SendMessageW(sb, SB_GETPARTS, 0, 0);
    printf("comctl: lv count=%d tv count=%d pb pos=%d tb pos=%d sb parts=%d\n", lvc, tvc, pbp, tbp, sbp);
    if (lvc != 3 || tvc != 2 || pbp != 60 || tbp != 7 || sbp != 2) ok = 0;

    ShowWindow(wnd, SW_SHOW);
    UpdateWindow(wnd);
    for (i = 0; i < 5; i++) UpdateWindow(i == 0 ? lv : i == 1 ? tv : i == 2 ? pb : i == 3 ? tb : sb);
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    d[0] = distinct(lv); d[1] = distinct(tv); d[2] = distinct(pb); d[3] = distinct(tb); d[4] = distinct(sb);
    printf("comctl: distinct colours lv=%d tv=%d pb=%d tb=%d sb=%d\n", d[0], d[1], d[2], d[3], d[4]);
    for (i = 0; i < 5; i++) if (d[i] < 2) ok = 0;
    printf("comctl: done\n");
    fflush(stdout);
    return ok ? 42 : 4;
}
