/* guest_window_menu: menus (core/user/win_menu.cpp). A WS_OVERLAPPEDWINDOW at (100,100) 400x300 gets a menu bar with
 * one popup "File" (Open 101 checked, a separator, Exit 102, Disabled 103 grayed). The guest checks the menu API
 * (CreateMenu, CreatePopupMenu, AppendMenuW, CheckMenuItem, EnableMenuItem, GetMenuState, GetMenuItemInfoW,
 * GetMenuItemID, GetSubMenu, IsMenu), that SetMenu shrinks the client area by SM_CYMENU, the geometry
 * (GetMenuBarInfo / GetMenuItemRect, printed as screen coordinates), the bar pixels (GetWindowDC + GetPixel), and then
 * the host-injected clicks (WIN10XBOX_TEST_INJECT_INPUT in tests/integration/CMakeLists.txt):
 *   1. click the bar item "File" -> the popup opens (WM_INITMENUPOPUP), click "Exit" -> WM_COMMAND id=102,
 *   2. the handler of that command calls TrackPopupMenu(TPM_RETURNCMD) at (300,250); a click on its first item
 *      makes it return 101.
 * Every printed line starts with "menu: "; failures print "menu: FAIL <what>". Exit 42 only when everything held.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define WM_APP_TRACK (WM_APP + 1)
#define ID_OPEN 101
#define ID_EXIT 102
#define ID_DISABLED 103

static HMENU g_bar, g_sub;
static int g_fail;
static int g_cmd_exit, g_init_popup, g_enter_loop, g_exit_loop, g_select_seen, g_tpm = -1, g_tpm_done, g_timeout;
static int g_select_exit_seen;

static void fail(const char *what) {
    g_fail++;
    printf("menu: FAIL %s\n", what);
    fflush(stdout);
}

static void check(const char *what, int ok) {
    if (!ok) fail(what);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        printf("menu: WM_COMMAND id=%u\n", (unsigned)LOWORD(wp));
        fflush(stdout);
        if (LOWORD(wp) == ID_EXIT) {
            g_cmd_exit++;
            PostMessageW(hwnd, WM_APP_TRACK, 0, 0);
        }
        return 0;
    case WM_INITMENUPOPUP:
        g_init_popup++;
        printf("menu: WM_INITMENUPOPUP popup=%d\n", (HMENU)wp == g_sub);
        fflush(stdout);
        return 0;
    case WM_ENTERMENULOOP:
        g_enter_loop++;
        return 0;
    case WM_EXITMENULOOP:
        g_exit_loop++;
        return 0;
    case WM_MENUSELECT:
        /* the popup window exists once an item of it is selected: print where its items are on the screen */
        if ((HMENU)lp == g_sub && !g_select_seen) {
            RECT r0, r2;
            g_select_seen = 1;
            if (GetMenuItemRect(NULL, g_sub, 0, &r0) && GetMenuItemRect(NULL, g_sub, 2, &r2)) {
                printf("menu: popup items open=%ld,%ld,%ld,%ld exit=%ld,%ld,%ld,%ld\n", (long)r0.left, (long)r0.top,
                       (long)r0.right, (long)r0.bottom, (long)r2.left, (long)r2.top, (long)r2.right, (long)r2.bottom);
            } else {
                printf("menu: popup items: GetMenuItemRect failed (%lu)\n", (unsigned long)GetLastError());
            }
            fflush(stdout);
        }
        if ((HMENU)lp == g_sub && LOWORD(wp) == ID_EXIT) g_select_exit_seen = 1;
        return 0;
    case WM_APP_TRACK: {
        int cmd = TrackPopupMenu(g_sub, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, 300, 250, 0, hwnd, NULL);
        g_tpm = cmd;
        g_tpm_done = 1;
        printf("menu: TrackPopupMenu returned %d\n", cmd);
        fflush(stdout);
        PostQuitMessage(0);
        return 0;
    }
    case WM_TIMER:
        g_timeout = 1;
        printf("menu: FAIL timeout waiting for the injected clicks (command=%d tpm=%d)\n", g_cmd_exit, g_tpm);
        fflush(stdout);
        PostQuitMessage(0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FillRect(dc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(void) {
    WNDCLASSW wc;
    HWND hwnd;
    RECT cr0, cr1, wr, ar0, ar1, ir;
    MENUBARINFO mbi;
    MENUITEMINFOW mii;
    WCHAR text[32];
    HDC dc;
    COLORREF bg, glyph, scan;
    int x, y, found_x = -1, found_y = -1;
    UINT st;
    MSG msg;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"W10XMenu";
    if (!RegisterClassW(&wc)) {
        printf("menu: FAIL RegisterClassW (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    hwnd = CreateWindowExW(0, L"W10XMenu", L"win10xbox menu", WS_OVERLAPPEDWINDOW, 100, 100, 400, 300, NULL, NULL,
                           wc.hInstance, NULL);
    if (!hwnd) {
        printf("menu: FAIL CreateWindowExW (%lu)\n", (unsigned long)GetLastError());
        return 2;
    }
    GetClientRect(hwnd, &cr0);
    check("no menu yet", GetMenu(hwnd) == NULL);

    /* the menu API */
    g_bar = CreateMenu();
    g_sub = CreatePopupMenu();
    check("CreateMenu / CreatePopupMenu", g_bar && g_sub && g_bar != g_sub);
    check("IsMenu", IsMenu(g_bar) && IsMenu(g_sub) && !IsMenu((HMENU)hwnd) && !IsMenu(NULL));
    check("AppendMenu Open", AppendMenuW(g_sub, MF_STRING, ID_OPEN, L"Open"));
    check("AppendMenu separator", AppendMenuW(g_sub, MF_SEPARATOR, 0, NULL));
    check("AppendMenu Exit", AppendMenuW(g_sub, MF_STRING, ID_EXIT, L"Exit"));
    check("AppendMenu Disabled", AppendMenuW(g_sub, MF_STRING, ID_DISABLED, L"Disabled"));
    check("CheckMenuItem returns the old state", CheckMenuItem(g_sub, ID_OPEN, MF_BYCOMMAND | MF_CHECKED) == MF_UNCHECKED);
    check("EnableMenuItem returns the old state", EnableMenuItem(g_sub, ID_DISABLED, MF_BYCOMMAND | MF_GRAYED) == MF_ENABLED);
    check("AppendMenu popup", AppendMenuW(g_bar, MF_POPUP | MF_STRING, (UINT_PTR)g_sub, L"File"));
    st = GetMenuState(g_sub, ID_OPEN, MF_BYCOMMAND);
    check("GetMenuState checked", st != (UINT)-1 && (st & MF_CHECKED));
    st = GetMenuState(g_sub, ID_DISABLED, MF_BYCOMMAND);
    check("GetMenuState grayed", st != (UINT)-1 && (st & MF_GRAYED));
    check("GetMenuState of a missing item", GetMenuState(g_sub, 999, MF_BYCOMMAND) == (UINT)-1);
    check("GetMenuItemCount", GetMenuItemCount(g_bar) == 1 && GetMenuItemCount(g_sub) == 4);
    check("GetMenuItemID", GetMenuItemID(g_sub, 0) == ID_OPEN && GetMenuItemID(g_sub, 2) == ID_EXIT &&
                               GetMenuItemID(g_bar, 0) == (UINT)-1);
    check("GetSubMenu", GetSubMenu(g_bar, 0) == g_sub && GetSubMenu(g_sub, 0) == NULL);
    memset(&mii, 0, sizeof mii);
    mii.cbSize = sizeof mii;
    mii.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE | MIIM_FTYPE;
    mii.dwTypeData = text;
    mii.cch = 32;
    check("GetMenuItemInfoW", GetMenuItemInfoW(g_sub, 2, TRUE, &mii) && mii.wID == ID_EXIT &&
                                  !lstrcmpW(text, L"Exit") && mii.cch == 4 && !(mii.fState & MFS_GRAYED));
    memset(&mii, 0, sizeof mii);
    mii.cbSize = sizeof mii;
    mii.fMask = MIIM_FTYPE | MIIM_STATE;
    check("GetMenuItemInfoW separator", GetMenuItemInfoW(g_sub, 1, TRUE, &mii) && (mii.fType & MFT_SEPARATOR));
    memset(&mii, 0, sizeof mii);
    mii.cbSize = sizeof mii;
    mii.fMask = MIIM_STATE;
    check("GetMenuItemInfoW state", GetMenuItemInfoW(g_sub, ID_OPEN, FALSE, &mii) && (mii.fState & MFS_CHECKED));
    printf("menu: api items=%d id0=%u checked=%d grayed=%d\n", GetMenuItemCount(g_sub), GetMenuItemID(g_sub, 0),
           (GetMenuState(g_sub, ID_OPEN, MF_BYCOMMAND) & MF_CHECKED) != 0,
           (GetMenuState(g_sub, ID_DISABLED, MF_BYCOMMAND) & MF_GRAYED) != 0);
    fflush(stdout);

    /* SetMenu: the client area shrinks by the menu bar */
    check("SetMenu", SetMenu(hwnd, g_bar));
    check("GetMenu", GetMenu(hwnd) == g_bar);
    GetClientRect(hwnd, &cr1);
    GetWindowRect(hwnd, &wr);
    printf("menu: client height before=%ld after=%ld delta=%ld SM_CYMENU=%d\n", (long)cr0.bottom, (long)cr1.bottom,
           (long)(cr0.bottom - cr1.bottom), GetSystemMetrics(SM_CYMENU));
    check("client height shrank by SM_CYMENU", cr0.bottom - cr1.bottom == GetSystemMetrics(SM_CYMENU));
    check("client width unchanged", cr0.right == cr1.right);
    check("window rect unchanged", wr.left == 100 && wr.top == 100 && wr.right == 500 && wr.bottom == 400);
    ar0.left = ar0.top = 0; ar0.right = 100; ar0.bottom = 100;
    ar1 = ar0;
    AdjustWindowRect(&ar0, WS_OVERLAPPEDWINDOW, FALSE);
    AdjustWindowRect(&ar1, WS_OVERLAPPEDWINDOW, TRUE);
    check("AdjustWindowRect agrees", (ar0.top - ar1.top) == GetSystemMetrics(SM_CYMENU));

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    RedrawWindow(hwnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);

    /* geometry: screen coordinates */
    memset(&mbi, 0, sizeof mbi);
    mbi.cbSize = sizeof mbi;
    check("GetMenuBarInfo", GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi) && mbi.hMenu == g_bar);
    printf("menu: bar rect=%ld,%ld,%ld,%ld\n", (long)mbi.rcBar.left, (long)mbi.rcBar.top, (long)mbi.rcBar.right,
           (long)mbi.rcBar.bottom);
    memset(&ir, 0, sizeof ir);
    check("GetMenuItemRect", GetMenuItemRect(hwnd, g_bar, 0, &ir));
    printf("menu: item File rect=%ld,%ld,%ld,%ld\n", (long)ir.left, (long)ir.top, (long)ir.right, (long)ir.bottom);
    check("item inside the bar", ir.left >= mbi.rcBar.left && ir.right <= mbi.rcBar.right && ir.top >= mbi.rcBar.top &&
                                     ir.bottom <= mbi.rcBar.bottom && ir.right > ir.left);
    {
        POINT co = {0, 0};
        ClientToScreen(hwnd, &co);
        check("bar ends where the client area starts", mbi.rcBar.bottom == co.y && mbi.rcBar.left == co.x);
    }

    /* pixels: the bar background and a glyph pixel of "File" in the window DC */
    dc = GetWindowDC(hwnd);
    bg = GetPixel(dc, mbi.rcBar.right - 20 - wr.left, mbi.rcBar.top + 5 - wr.top);
    glyph = GetPixel(dc, ir.left + 8 - wr.left, ir.top + 1 + 2 - wr.top);   /* 'F' row 2 is the top bar of the F */
    for (y = ir.top; y < ir.bottom && found_x < 0; y++)
        for (x = ir.left; x < ir.right; x++) {
            scan = GetPixel(dc, x - wr.left, y - wr.top);
            if (scan == GetSysColor(COLOR_MENUTEXT)) { found_x = x; found_y = y; break; }
        }
    ReleaseDC(hwnd, dc);
    printf("menu: pixels bg=%08lx glyph=%08lx first_text_pixel=%d,%d\n", (unsigned long)bg, (unsigned long)glyph, found_x,
           found_y);
    fflush(stdout);
    check("bar background is COLOR_MENU", bg == GetSysColor(COLOR_MENU));
    check("glyph pixel is COLOR_MENUTEXT", glyph == GetSysColor(COLOR_MENUTEXT));

    /* the host clicks File, Exit, then the TrackPopupMenu item */
    SetTimer(hwnd, 1, 40000, NULL);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    printf("menu: loops enter=%d exit=%d init=%d\n", g_enter_loop, g_exit_loop, g_init_popup);
    check("WM_COMMAND id=102 arrived once", g_cmd_exit == 1);
    check("TrackPopupMenu returned the first item", g_tpm_done && g_tpm == ID_OPEN);
    check("menu loop notifications", g_enter_loop == 2 && g_exit_loop == 2 && g_init_popup == 2);
    check("WM_MENUSELECT for the popup items", g_select_seen && g_select_exit_seen);
    check("no timeout", !g_timeout);
    printf("menu: done\n");
    fflush(stdout);
    return g_fail ? 5 : 42;
}
