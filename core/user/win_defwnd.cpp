// core/user default window procedure (NtUserMessageCall(NtUserDefWindowProc),
// what user32's DefWindowProcA/W forward for everything they do not handle
// themselves): window text, the non-client size and frame, background erase
// with the class brush, close, size/move notifications, activation -> focus,
// control colours. Mirrors the parts of Wine's defwnd.c that real programs
// depend on; frameless windows first, captioned frames drawn in solid colours.
#include "user_internal.h"
#include "user_syscalls.h"

#include <algorithm>
#include <cstring>

namespace win10xbox::user {
namespace detail {

HDC BeginPaintInternal(HWND hwnd, PAINTSTRUCT* ps);   // win_paint.cpp
BOOL EndPaintInternal(HWND hwnd, const PAINTSTRUCT* ps);

namespace {

std::wstring FromAnsi(const char* s) {
    if (!s) return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? static_cast<std::size_t>(n - 1) : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    return w;
}

// The non-client insets of `w`: what WM_NCCALCSIZE takes off the window rectangle.
RECT NcInsets(const WindowObj* w) {
    RECT r{0, 0, 0, 0};
    AdjustRectForStyle(&r, w->style, false, w->ex_style);
    RECT insets{-r.left, -r.top, r.right, r.bottom};
    insets.top += MenuBarHeight(w);   // win_menu.cpp: SM_CYMENU below the caption when the window has a menu
    if (w->style & WS_VSCROLL) insets.right += SystemMetric(SM_CXVSCROLL);
    if (w->style & WS_HSCROLL) insets.bottom += SystemMetric(SM_CYHSCROLL);
    return insets;
}

void ShrinkByInsets(RECT* r, const RECT& in) {
    r->left += in.left;
    r->top += in.top;
    r->right -= in.right;
    r->bottom -= in.bottom;
    if (r->right < r->left) r->right = r->left;
    if (r->bottom < r->top) r->bottom = r->top;
}

LRESULT EraseBackground(WindowObj* w, HDC hdc) {
    HBRUSH brush = w->cls->brush;
    if (!brush) return 0;
    if (reinterpret_cast<ULONG_PTR>(brush) <= COLOR_MENUBAR + 1) brush = SysColorBrush(static_cast<int>(reinterpret_cast<ULONG_PTR>(brush)) - 1);
    auto* br = GdiGetT<BrushObj>(brush, abi::kObjBrush);
    DcObj* dc = DcGet(hdc);
    if (!br || !dc) return 0;
    if (br->style == BS_NULL) return 1;
    if (br->style != BS_SOLID) FIXME(gdi, "WM_ERASEBKGND with a non-solid class brush: filled with its colour");
    DrawTarget t;
    if (!ResolveDrawTarget(dc, &t)) return 1;
    const RECT client{0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top};
    // device coordinates of a client DC are client coordinates
    FillTargetRect(t, client, ColorToPixel(br->color));
    NoteDrawn(dc, t, client);
    return 1;
}

LRESULT CtlColor(UINT msg, HDC hdc) {
    DcObj* dc = DcGet(hdc);
    int bk = COLOR_WINDOW;
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORDLG || msg == WM_CTLCOLORBTN || msg == WM_CTLCOLORMSGBOX) bk = COLOR_3DFACE;
    if (msg == WM_CTLCOLORSCROLLBAR) bk = COLOR_SCROLLBAR;
    if (dc) {
        dc->attr->text_color = SysColor(COLOR_WINDOWTEXT);
        dc->attr->background_color = SysColor(bk);
    }
    return reinterpret_cast<LRESULT>(SysColorBrush(bk));
}

LRESULT HitTest(WindowObj* w, POINT pt) {
    const RECT wr = WindowRectScreen(w), cr = ClientRectScreen(w);
    if (!PtInRectangle(wr, pt.x, pt.y)) return HTNOWHERE;
    if (PtInRectangle(cr, pt.x, pt.y)) return HTCLIENT;
    if (MenuBarHitTest(w, pt)) return HTMENU;
    if ((w->style & WS_CAPTION) == WS_CAPTION && pt.y < cr.top && pt.x >= cr.left && pt.x < cr.right) {
        if ((w->style & WS_SYSMENU) && pt.x >= cr.right - SystemMetric(SM_CXSIZE)) return HTCLOSE;
        return HTCAPTION;
    }
    if (w->style & WS_THICKFRAME) {
        const bool l = pt.x < cr.left, r = pt.x >= cr.right, t = pt.y < cr.top, b = pt.y >= cr.bottom;
        if (t && l) return HTTOPLEFT;
        if (t && r) return HTTOPRIGHT;
        if (b && l) return HTBOTTOMLEFT;
        if (b && r) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (t) return HTTOP;
        if (b) return HTBOTTOM;
    }
    return HTBORDER;
}

}  // namespace

LRESULT DefWindowProcInternal(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool ansi) {
    WindowObj* w = Win(hwnd);
    if (!w) return 0;
    hwnd = w->handle;
    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        if (cs && cs->lpszName && !IS_INTRESOURCE(cs->lpszName)) {
            if (ansi) SetWindowTextInternal(w, FromAnsi(reinterpret_cast<const char*>(cs->lpszName)).c_str());
            else SetWindowTextInternal(w, cs->lpszName);
        }
        return TRUE;
    }
    case WM_NCCALCSIZE: {
        const RECT in = NcInsets(w);
        if (wp) ShrinkByInsets(&reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0], in);
        else if (lp) ShrinkByInsets(reinterpret_cast<RECT*>(lp), in);
        return 0;
    }
    case WM_NCHITTEST:
        return HitTest(w, POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
    case WM_NCPAINT:
        PaintNonClient(hwnd);
        return 0;
    case WM_NCACTIVATE:
        if (WindowObj* cur = Win(hwnd); cur && (cur->style & WS_CAPTION) == WS_CAPTION && IsWindowReallyVisible(cur))
            PaintNonClient(hwnd);
        return TRUE;
    case WM_ERASEBKGND:
    case WM_ICONERASEBKGND:
        return EraseBackground(w, reinterpret_cast<HDC>(wp));
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLORSCROLLBAR:
        return CtlColor(msg, reinterpret_cast<HDC>(wp));
    case WM_PAINT: {
        PAINTSTRUCT ps;
        if (BeginPaintInternal(hwnd, &ps)) EndPaintInternal(hwnd, &ps);
        return 0;
    }
    case WM_SETTEXT: {
        if (!lp) SetWindowTextInternal(w, L"");
        else if (ansi) SetWindowTextInternal(w, FromAnsi(reinterpret_cast<const char*>(lp)).c_str());
        else SetWindowTextInternal(w, reinterpret_cast<const WCHAR*>(lp));
        if ((w = Win(hwnd)) && (w->style & WS_CAPTION) == WS_CAPTION && IsWindowReallyVisible(w)) PaintNonClient(hwnd);
        return TRUE;
    }
    case WM_GETTEXT: {
        if (!wp || !lp) return 0;
        if (ansi) {
            const int n = WideCharToMultiByte(CP_ACP, 0, w->text.c_str(), static_cast<int>(w->text.size()),
                                              reinterpret_cast<char*>(lp), static_cast<int>(wp) - 1, nullptr, nullptr);
            reinterpret_cast<char*>(lp)[n] = 0;
            return n;
        }
        const std::size_t n = std::min<std::size_t>(wp - 1, w->text.size());
        std::memcpy(reinterpret_cast<WCHAR*>(lp), w->text.data(), n * sizeof(WCHAR));
        reinterpret_cast<WCHAR*>(lp)[n] = 0;
        return static_cast<LRESULT>(n);
    }
    case WM_GETTEXTLENGTH:
        if (ansi) return WideCharToMultiByte(CP_ACP, 0, w->text.c_str(), static_cast<int>(w->text.size()), nullptr, 0, nullptr, nullptr);
        return static_cast<LRESULT>(w->text.size());
    case WM_CLOSE:
        DestroyWindowInternal(hwnd);
        return 0;
    case WM_WINDOWPOSCHANGED: {
        auto* pos = reinterpret_cast<const WINDOWPOS*>(lp);
        if (!pos) return 0;
        const RECT c = w->client;
        const DWORD style = w->style;
        if (!(pos->flags & abi::kSwpNoClientMove)) SendMessageInternal(hwnd, WM_MOVE, 0, MAKELONG(c.left, c.top));
        if (!(pos->flags & abi::kSwpNoClientSize) && Win(hwnd)) {
            const WPARAM how = (style & WS_MAXIMIZE) ? SIZE_MAXIMIZED : (style & WS_MINIMIZE) ? SIZE_MINIMIZED : SIZE_RESTORED;
            SendMessageInternal(hwnd, WM_SIZE, how, MAKELONG(c.right - c.left, c.bottom - c.top));
        }
        return 0;
    }
    case WM_WINDOWPOSCHANGING: {
        auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        if (pos && !(pos->flags & SWP_NOSIZE) && ((w->style & WS_THICKFRAME) || !(w->style & (WS_POPUP | WS_CHILD))) &&
            !(w->style & (WS_MINIMIZE | WS_MAXIMIZE))) {
            MINMAXINFO mm{};
            mm.ptMinTrackSize = POINT{SystemMetric(SM_CXMINTRACK), SystemMetric(SM_CYMINTRACK)};
            mm.ptMaxTrackSize = POINT{SystemMetric(SM_CXMAXTRACK), SystemMetric(SM_CYMAXTRACK)};
            SendMessageInternal(hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&mm));
            pos->cx = std::max<int>(std::min<int>(pos->cx, mm.ptMaxTrackSize.x), mm.ptMinTrackSize.x);
            pos->cy = std::max<int>(std::min<int>(pos->cy, mm.ptMaxTrackSize.y), mm.ptMinTrackSize.y);
        }
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && !(w->style & WS_MINIMIZE) && IsTopLevel(w)) SetFocusInternal(hwnd);
        return 0;
    case WM_MOUSEACTIVATE:
        if ((w->style & WS_CHILD) && Win(w->parent)) {
            const LRESULT r = SendMessageInternal(w->parent, WM_MOUSEACTIVATE, wp, lp);
            if (r) return r;
        }
        return (LOWORD(lp) == HTCAPTION) ? MA_NOACTIVATE : MA_ACTIVATE;
    case WM_SETCURSOR:
        if ((w->style & WS_CHILD) && Win(w->parent)) {
            if (SendMessageInternal(w->parent, WM_SETCURSOR, wp, lp)) return TRUE;
            if (!(w = Win(hwnd))) return FALSE;
        }
        if (LOWORD(lp) == HTCLIENT && w->cls->cursor)
            if (winexbox::IWinexboxDriver* d = Driver()) d->CursorSet(reinterpret_cast<winexbox::WindowId>(hwnd),
                                                                       reinterpret_cast<winexbox::CursorId>(w->cls->cursor));
        return FALSE;
    case WM_SYSKEYDOWN:
        if (wp == VK_F4 && (HIWORD(lp) & KF_ALTDOWN)) PostMessageInternal(TopLevelOf(hwnd), WM_SYSCOMMAND, SC_CLOSE, 0);
        return 0;
    case WM_SHOWWINDOW:
        return 0;
    case WM_QUERYOPEN:
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_GETICON:
        if (wp == ICON_BIG) return reinterpret_cast<LRESULT>(w->icon);
        return reinterpret_cast<LRESULT>(w->icon_sm);
    case WM_SETICON: {
        HICON old;
        if (wp == ICON_BIG) { old = w->icon; w->icon = reinterpret_cast<HICON>(lp); }
        else { old = w->icon_sm; w->icon_sm = reinterpret_cast<HICON>(lp); }
        return reinterpret_cast<LRESULT>(old);
    }
    case WM_SETREDRAW:
        if (wp) {
            if (!(w->style & WS_VISIBLE)) {
                w->style |= WS_VISIBLE;
                if (IsTopLevel(w)) EnsureSurface(w);
                NoteScreenChanged();
            }
        } else if (w->style & WS_VISIBLE) {
            w->style &= ~WS_VISIBLE;   // drawing stops; the window keeps its place
            NoteScreenChanged();
        }
        return 0;
    case WM_CONTEXTMENU:
        if ((w->style & WS_CHILD) && Win(w->parent)) SendMessageInternal(w->parent, msg, wp, lp);
        return 0;
    case WM_NCDESTROY:
        w->text.clear();
        MenuDestroyWindowMenu(w);
        return 0;
    case WM_NCLBUTTONDOWN:
        if (IsTopLevel(w) && FindUserThread() && FindUserThread()->active != hwnd) SetActiveWindowInternal(hwnd, true);
        if (wp == HTMENU) MenuBarClick(hwnd, POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        return 0;
    case WM_NCLBUTTONUP:
        if (wp == HTCLOSE) PostMessageInternal(hwnd, WM_SYSCOMMAND, SC_CLOSE, lp);
        return 0;
    case WM_PRINTCLIENT:
    case WM_GETMINMAXINFO:
    case WM_DESTROY:
    case WM_PARENTNOTIFY:
    case WM_STYLECHANGING:
    case WM_STYLECHANGED:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ACTIVATEAPP:
    case WM_ENABLE:
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
    case WM_MOVE:
    case WM_SIZE:
    case WM_CREATE:
    case WM_TIMER:
    case WM_NULL:
        return 0;
    }
    if (msg >= WM_USER) return 0;
    TRACE(user, "%p: message %04x not handled by the default procedure", hwnd, msg);
    return 0;
}

}  // namespace detail
}  // namespace win10xbox::user
