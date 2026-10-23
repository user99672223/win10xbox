/* guest_comctl: Wine's builtin comctl32 through the layer. InitCommonControlsEx, then a report-view ListView (2
 * columns, 3 items) and a progress bar (range 0..100, position 50, explicit bar and background colours) as children
 * of a popup window. The state is read back with messages (LVM_GETITEMCOUNT, LVM_GETHEADER + HDM_GETITEMCOUNT,
 * PBM_GETPOS), the controls are painted (UpdateWindow) and sampled with GetPixel: the ListView's white background
 * and its header (button face), the progress bar's filled part and the part outside the fill. A TreeView is created
 * and filled too (TVM_GETCOUNT), without a pixel check.
 * Exit 42 only if everything held (4 = a check failed, 2 = setup failed).
 *
 * The window is a 400x300 WS_POPUP at (100,100), so client (x,y) of the ListView (placed at 5,5) is screen
 * (105+x, 105+y) and of the progress bar (placed at 5,135) screen (105+x, 235+y); the integration test checks the
 * same pixels in the screenshot.
 *
 * Output lines (the integration test matches them):
 *   comctl: init ok
 *   comctl: created lv=1 tv=1 pb=1 hdr=1
 *   comctl: lv count=3 cols=2 tv count=2 pb pos=50
 *   comctl: pixels lv=00ffffff hdr=00f0f0f0 pb_in=0000a000 pb_out=00e6e6e6
 *   comctl: label pixels drawn=N
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

static COLORREF pixel(HWND h, int x, int y) {
    HDC dc = GetDC(h);
    COLORREF c = GetPixel(dc, x, y);
    ReleaseDC(h, dc);
    return c;
}

int main(void) {
    INITCOMMONCONTROLSEX icc;
    WNDCLASSW wc;
    HWND wnd, lv, tv, pb, hdr;
    HINSTANCE inst = GetModuleHandleW(NULL);
    int i, ok = 1, lvc, cols, tvc, pbp, drawn = 0, x, y;
    COLORREF c_lv, c_hdr, c_in, c_out;
    const COLORREF bar = RGB(0, 160, 0), back = RGB(230, 230, 230);
    MSG msg;
    LVCOLUMNW col;
    LVITEMW item;
    TVINSERTSTRUCTW tvi;
    HTREEITEM root;
    RECT rc;
    HDC dc;
    static const WCHAR *names[3] = {L"alpha", L"beta", L"gamma"};

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_PROGRESS_CLASS;
    if (!InitCommonControlsEx(&icc)) { printf("comctl: InitCommonControlsEx failed (%lu)\n", GetLastError()); return 2; }
    printf("comctl: init ok\n");
    fflush(stdout);

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
    pb = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 5, 135, 190, 20, wnd, (HMENU)3, inst, NULL);
    hdr = lv ? (HWND)SendMessageW(lv, LVM_GETHEADER, 0, 0) : NULL;
    printf("comctl: created lv=%d tv=%d pb=%d hdr=%d\n", lv != 0, tv != 0, pb != 0, hdr != 0);
    fflush(stdout);
    if (!lv || !tv || !pb || !hdr) { printf("comctl: control creation failed (%lu)\n", GetLastError()); return 2; }

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

    SendMessageW(pb, PBM_SETBARCOLOR, 0, (LPARAM)bar);
    SendMessageW(pb, PBM_SETBKCOLOR, 0, (LPARAM)back);
    SendMessageW(pb, PBM_SETRANGE32, 0, 100);
    SendMessageW(pb, PBM_SETPOS, 50, 0);

    lvc = (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0);
    cols = (int)SendMessageW(hdr, HDM_GETITEMCOUNT, 0, 0);
    tvc = (int)SendMessageW(tv, TVM_GETCOUNT, 0, 0);
    pbp = (int)SendMessageW(pb, PBM_GETPOS, 0, 0);
    printf("comctl: lv count=%d cols=%d tv count=%d pb pos=%d\n", lvc, cols, tvc, pbp);
    fflush(stdout);
    if (lvc != 3 || cols != 2 || tvc != 2 || pbp != 50) ok = 0;

    ShowWindow(wnd, SW_SHOW);
    UpdateWindow(wnd);
    UpdateWindow(lv);
    UpdateWindow(hdr);
    UpdateWindow(tv);
    UpdateWindow(pb);
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    /* ListView: right of the columns and below the three rows = background; header: right of the two columns (x=170..) */
    c_lv = pixel(lv, 150, 100);
    GetClientRect(hdr, &rc);
    c_hdr = pixel(hdr, 180, rc.bottom / 2);
    /* Progress bar (190x20, smooth): half filled; sample well inside each part */
    c_in = pixel(pb, 20, 10);
    c_out = pixel(pb, 170, 10);
    printf("comctl: pixels lv=%08lx hdr=%08lx pb_in=%08lx pb_out=%08lx\n", (unsigned long)c_lv, (unsigned long)c_hdr,
           (unsigned long)c_in, (unsigned long)c_out);
    fflush(stdout);
    if (c_lv != RGB(255, 255, 255)) ok = 0;
    if (c_hdr != GetSysColor(COLOR_BTNFACE)) ok = 0;
    if (c_in != bar || c_out != back) ok = 0;

    /* the first item's label rectangle holds glyph pixels (not the white background) */
    rc.left = LVIR_LABEL;
    if (SendMessageW(lv, LVM_GETITEMRECT, 0, (LPARAM)&rc)) {
        dc = GetDC(lv);
        for (y = rc.top; y < rc.bottom; y++)
            for (x = rc.left; x < rc.right; x++)
                if (GetPixel(dc, x, y) != RGB(255, 255, 255)) drawn++;
        ReleaseDC(lv, dc);
    }
    printf("comctl: label pixels drawn=%d\n", drawn);
    if (drawn == 0) ok = 0;
    printf("comctl: done\n");
    fflush(stdout);
    return ok ? 42 : 4;
}
