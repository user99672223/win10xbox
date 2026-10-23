// core/user scroll bars: the per-window scroll state (SetScrollInfo / GetScrollInfo / ranges / ShowScrollBar /
// EnableScrollBar / GetScrollBarInfo), the non-client scroll bars of WS_HSCROLL / WS_VSCROLL windows (drawn in solid
// colours, hit-tested as HTHSCROLL / HTVSCROLL, a click sends WM_HSCROLL / WM_VSCROLL SB_LINEUP / SB_LINEDOWN /
// SB_PAGEUP / SB_PAGEDOWN then SB_ENDSCROLL), the ScrollBar control's window procedure (SBM_*), and ScrollDC.
// Geometry and the SetScrollInfo rules follow Wine's dlls/win32u/scroll.c.
// Not implemented (FIXME in docs/GUI.md): thumb dragging and button auto-repeat (the click is a single event),
// hover/pressed visuals, WS_EX_LEFTSCROLLBAR, size grips, accessibility events.
#include "win_scroll.h"
#include "user_syscalls.h"

#include <algorithm>
#include <cwchar>

namespace win10xbox::user {
namespace detail {
namespace {

constexpr int kMinRect = 4;     // SCROLL_MIN_RECT
constexpr int kMinThumb = 17;   // SCROLL_MIN_THUMB

enum Hit { kNowhere = 0, kTopArrow, kTopRect, kThumb, kBottomRect, kBottomArrow };

struct Info {
    int min = 0, max = 100;
    int page = 0, pos = 0;
    UINT flags = ESB_ENABLE_BOTH;
};
struct Rec {
    HWND hwnd = nullptr;
    bool bars = false;        // horz / vert allocated (Wine's win->pScroll)
    Info horz, vert;
    bool has_ctl = false;
    Info ctl;
};
// Namespace scope, trivially initialised; allocated under the lock on first use (docs/GUI.md rule 3).
std::vector<Rec>* g_recs = nullptr;

int Mul(int a, int b, int c) {   // MulDiv, rounded
    if (!c) return -1;
    const long long v = static_cast<long long>(a) * b;
    return static_cast<int>((v + (v >= 0 ? c / 2 : -(c / 2))) / c);
}

bool IsScrollBarClass(const WindowObj* w) { return w->cls && _wcsicmp(w->cls->name.c_str(), L"ScrollBar") == 0; }

Rec* FindRec(HWND full, bool create) {
    if (!g_recs) {
        if (!create) return nullptr;
        g_recs = new std::vector<Rec>();
    }
    for (Rec& r : *g_recs)
        if (r.hwnd == full) return &r;
    if (!create) return nullptr;
    // drop the records of destroyed windows now and then
    g_recs->erase(std::remove_if(g_recs->begin(), g_recs->end(), [](const Rec& r) { return !Win(r.hwnd); }), g_recs->end());
    g_recs->push_back(Rec{});
    g_recs->back().hwnd = full;
    return &g_recs->back();
}

// Wine's get_scroll_info_ptr. The pointer is valid until the next FindRec(create).
Info* GetInfo(WindowObj* w, int bar, bool alloc) {
    switch (bar) {
    case SB_HORZ:
    case SB_VERT: {
        Rec* r = FindRec(w->handle, alloc);
        if (!r) return nullptr;
        if (!r->bars) {
            if (!alloc) return nullptr;
            r->bars = true;
            r->horz = Info{};
            r->vert = Info{};
        }
        return bar == SB_HORZ ? &r->horz : &r->vert;
    }
    case SB_CTL: {
        if (!IsScrollBarClass(w)) return nullptr;
        Rec* r = FindRec(w->handle, true);
        if (!r->has_ctl) {
            r->has_ctl = true;
            r->ctl = Info{0, 0, 0, 0, (w->style & WS_DISABLED) ? static_cast<UINT>(ESB_DISABLE_BOTH) : static_cast<UINT>(ESB_ENABLE_BOTH)};
        }
        return &r->ctl;
    }
    }
    return nullptr;
}

RECT ClientInWindow(const WindowObj* w) { return RectOffset(w->client, -w->window.left, -w->window.top); }

// Wine's get_scroll_bar_rect: the bar rectangle in drawing coordinates (client for SB_CTL, window for the
// non-client bars). Returns true for a vertical bar.
bool BarRect(WindowObj* w, int bar, RECT* rect, int* arrow, int* thumb_size, int* thumb_pos) {
    bool vertical;
    const int cx = SystemMetric(SM_CXVSCROLL);
    switch (bar) {
    case SB_HORZ:
        *rect = ClientInWindow(w);
        rect->top = rect->bottom;
        rect->bottom += SystemMetric(SM_CYHSCROLL);
        vertical = false;
        break;
    case SB_VERT:
        *rect = ClientInWindow(w);
        rect->left = rect->right;
        rect->right += cx;
        vertical = true;
        break;
    case SB_CTL:
        *rect = RECT{0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top};
        vertical = (w->style & SBS_VERT) != 0;
        break;
    default:
        return false;
    }
    int pixels = vertical ? rect->bottom - rect->top : rect->right - rect->left;
    if (pixels <= 2 * cx + kMinRect) {
        *arrow = pixels > kMinRect ? (pixels - kMinRect) / 2 : 0;
        *thumb_pos = *thumb_size = 0;
        return vertical;
    }
    Info* info = GetInfo(w, bar, true);
    if (!info) {
        *arrow = *thumb_pos = *thumb_size = 0;
        return vertical;
    }
    *arrow = cx;
    pixels -= 2 * cx;
    if (info->page) {
        *thumb_size = Mul(pixels, info->page, info->max - info->min + 1);
        if (*thumb_size < kMinThumb) *thumb_size = kMinThumb;
    } else {
        *thumb_size = cx;
    }
    if ((pixels -= *thumb_size) < 0 || (info->flags & ESB_DISABLE_BOTH) == ESB_DISABLE_BOTH) {
        *thumb_pos = *thumb_size = 0;
    } else {
        const int max = info->max - std::max(info->page - 1, 0);
        if (info->min >= max) *thumb_pos = *arrow;
        else *thumb_pos = *arrow + Mul(pixels, info->pos - info->min, max - info->min);
    }
    return vertical;
}

int HitTestBar(WindowObj* w, int bar, POINT pt) {   // pt in the bar's drawing coordinates
    RECT rect;
    int arrow, tsize, tpos;
    const bool vertical = BarRect(w, bar, &rect, &arrow, &tsize, &tpos);
    if (!PtInRectangle(rect, pt.x, pt.y)) return kNowhere;
    const int p = vertical ? pt.y : pt.x;
    const int lo = vertical ? rect.top : rect.left, hi = vertical ? rect.bottom : rect.right;
    if (p < lo + arrow) return kTopArrow;
    if (p >= hi - arrow) return kBottomArrow;
    if (!tpos) return kTopRect;
    if (p - lo < tpos) return kTopRect;
    if (p - lo >= tpos + tsize) return kBottomRect;
    return kThumb;
}

// ---- Drawing (solid colours) ------------------------------------------------------------------------------------
std::uint32_t Px(int index) { return ColorToPixel(SysColor(index)); }

void DrawArrowButton(const DrawTarget& t, const RECT& r, int dir /*0 up 1 down 2 left 3 right*/, bool disabled) {
    FillTargetRect(t, r, Px(COLOR_BTNFACE));
    FillTargetRect(t, RECT{r.left, r.top, r.right, r.top + 1}, Px(COLOR_BTNHIGHLIGHT));
    FillTargetRect(t, RECT{r.left, r.top, r.left + 1, r.bottom}, Px(COLOR_BTNHIGHLIGHT));
    FillTargetRect(t, RECT{r.left, r.bottom - 1, r.right, r.bottom}, Px(COLOR_BTNSHADOW));
    FillTargetRect(t, RECT{r.right - 1, r.top, r.right, r.bottom}, Px(COLOR_BTNSHADOW));
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const std::uint32_t c = Px(disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT);
    for (int i = 0; i < 4; ++i) {
        const int half = (dir == 0 || dir == 2) ? i : 3 - i;
        if (dir < 2) FillTargetRect(t, RECT{cx - half, cy - 2 + i, cx + half + 1, cy - 1 + i}, c);
        else FillTargetRect(t, RECT{cx - 2 + i, cy - half, cx - 1 + i, cy + half + 1}, c);
    }
}

void DrawBar(HWND hwnd, int bar) {
    WindowObj* w = Win(hwnd);
    if (!w || !IsWindowReallyVisible(w)) return;
    if ((bar == SB_VERT && !(w->style & WS_VSCROLL)) || (bar == SB_HORZ && !(w->style & WS_HSCROLL))) return;
    RECT r;
    int arrow, tsize, tpos;
    const bool vertical = BarRect(w, bar, &r, &arrow, &tsize, &tpos);
    const Info* in = GetInfo(w, bar, true);
    if (!in || RectEmpty(r)) return;
    const UINT flags = in->flags;
    HDC hdc = GetDCInternal(hwnd, nullptr, DCX_CACHE | (bar == SB_CTL ? 0 : DCX_WINDOW));
    DcObj* dc = DcGet(hdc);
    DrawTarget t;
    if (dc && ResolveDrawTarget(dc, &t)) {
        const int total = vertical ? r.bottom - r.top : r.right - r.left;
        auto Span = [&](int p0, int p1) {
            return vertical ? RECT{r.left, r.top + p0, r.right, r.top + p1} : RECT{r.left + p0, r.top, r.left + p1, r.bottom};
        };
        FillTargetRect(t, r, Px(COLOR_SCROLLBAR));
        if (arrow > 0) {
            DrawArrowButton(t, Span(0, arrow), vertical ? 0 : 2, (flags & ESB_DISABLE_LTUP) != 0);
            DrawArrowButton(t, Span(total - arrow, total), vertical ? 1 : 3, (flags & ESB_DISABLE_RTDN) != 0);
        }
        if (tsize > 0) {
            const RECT th = Span(tpos, tpos + tsize);
            FillTargetRect(t, th, Px(COLOR_BTNFACE));
            FillTargetRect(t, RECT{th.left, th.top, th.right, th.top + 1}, Px(COLOR_BTNHIGHLIGHT));
            FillTargetRect(t, RECT{th.left, th.top, th.left + 1, th.bottom}, Px(COLOR_BTNHIGHLIGHT));
            FillTargetRect(t, RECT{th.left, th.bottom - 1, th.right, th.bottom}, Px(COLOR_BTNSHADOW));
            FillTargetRect(t, RECT{th.right - 1, th.top, th.right, th.bottom}, Px(COLOR_BTNSHADOW));
        }
        NoteDrawn(dc, t, r);
    }
    ReleaseDCInternal(hwnd, hdc);
}

// ---- Style / redraw ---------------------------------------------------------------------------------------------
bool ShowBars(HWND hwnd, int bar, bool show_horz, bool show_vert) {
    WindowObj* w = Win(hwnd);
    if (!w) return false;
    DWORD set = 0, clear = 0;
    switch (bar) {
    case SB_CTL:
        ShowWindowInternal(hwnd, show_horz ? SW_SHOW : SW_HIDE);
        return true;
    case SB_BOTH:
    case SB_HORZ:
        (show_horz ? set : clear) |= WS_HSCROLL;
        if (bar == SB_HORZ) break;
        [[fallthrough]];
    case SB_VERT:
        (show_vert ? set : clear) |= WS_VSCROLL;
        break;
    default:
        return false;
    }
    const DWORD old = w->style;
    w->style = (old & ~clear) | set;
    if ((old & clear) != 0 || (old & set) != set) {
        SetWindowPosInternal(hwnd, nullptr, 0, 0, 0, 0,
                             SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
        return true;
    }
    return false;
}

bool Valid(const SCROLLINFO* i) {
    return i && !(i->fMask & ~(SIF_ALL | SIF_DISABLENOSCROLL | 0x1000 /*SIF_RETURNPREV*/)) &&
           (i->cbSize == sizeof(SCROLLINFO) || i->cbSize == sizeof(SCROLLINFO) - sizeof(int));
}

BOOL GetScrollInfoImpl(HWND hwnd, int bar, SCROLLINFO* info) {
    WindowObj* w = Win(hwnd);
    if (!w || !Valid(info)) return FALSE;
    const Info* s = GetInfo(w, bar, false);
    if (!s) return FALSE;
    if (info->fMask & SIF_PAGE) info->nPage = s->page;
    if (info->fMask & SIF_POS) info->nPos = s->pos;
    if ((info->fMask & SIF_TRACKPOS) && info->cbSize == sizeof(*info)) info->nTrackPos = s->pos;
    if (info->fMask & SIF_RANGE) {
        info->nMin = s->min;
        info->nMax = s->max;
    }
    return (info->fMask & SIF_ALL) != 0;
}

enum { kSsiHide = 1, kSsiShow = 2, kSsiRefresh = 4, kSsiRepaintArrows = 8 };

int SetScrollInfoImpl(HWND hwnd, int bar, const SCROLLINFO* info, BOOL redraw) {
    WindowObj* w = Win(hwnd);
    if (!w || !Valid(info)) return 0;
    Info* s = GetInfo(w, bar, true);
    if (!s) return 0;
    hwnd = w->handle;
    int action = 0, ret = 0;
    if (info->fMask & 0x1000) ret = s->pos;
    if ((info->fMask & SIF_PAGE) && s->page != static_cast<int>(info->nPage)) {
        s->page = static_cast<int>(info->nPage);
        action |= kSsiRefresh;
    }
    if ((info->fMask & SIF_POS) && s->pos != info->nPos) {
        s->pos = info->nPos;
        action |= kSsiRefresh;
    }
    if (info->fMask & SIF_RANGE) {
        if (info->nMin > info->nMax || static_cast<UINT>(info->nMax - info->nMin) >= 0x80000000u) {
            action |= kSsiRefresh;
            s->min = s->max = 0;
        } else if (s->min != info->nMin || s->max != info->nMax) {
            action |= kSsiRefresh;
            s->min = info->nMin;
            s->max = info->nMax;
        }
    }
    if (s->page < 0) s->page = 0;
    else if (s->page > s->max - s->min + 1) s->page = s->max - s->min + 1;
    if (s->pos < s->min) s->pos = s->min;
    else if (s->pos > s->max - std::max(s->page - 1, 0)) s->pos = s->max - std::max(s->page - 1, 0);

    if (info->fMask & SIF_ALL) {
        if (info->fMask & (SIF_RANGE | SIF_PAGE | SIF_DISABLENOSCROLL)) {
            UINT new_flags = s->flags;
            if (s->min >= s->max - std::max(s->page - 1, 0)) {
                if (info->fMask & SIF_DISABLENOSCROLL) {
                    new_flags = ESB_DISABLE_BOTH;
                    action |= kSsiRefresh;
                } else if (bar != SB_CTL && (action & kSsiRefresh)) {
                    action = kSsiHide;
                }
            } else if (info->fMask != SIF_PAGE) {
                new_flags = ESB_ENABLE_BOTH;
                if (bar != SB_CTL && (action & kSsiRefresh)) action |= kSsiShow;
            }
            if (bar == SB_CTL && redraw && IsWindowReallyVisible(w) &&
                (new_flags == ESB_ENABLE_BOTH || new_flags == ESB_DISABLE_BOTH)) {
                if (new_flags == ESB_ENABLE_BOTH) w->style &= ~WS_DISABLED;
                else w->style |= WS_DISABLED;
            }
            if (s->flags != new_flags) {
                s->flags = new_flags;
                action |= kSsiRepaintArrows;
            }
        }
    }
    if (!(info->fMask & 0x1000)) ret = s->pos;
    if (action & kSsiHide) {
        ShowBars(hwnd, bar, false, false);
    } else {
        if ((action & kSsiShow) && ShowBars(hwnd, bar, true, true)) return ret;   // SetWindowPos repainted
        if (redraw || (action & kSsiRepaintArrows)) DrawBar(hwnd, bar);
    }
    return ret;
}

void SendScroll(HWND hwnd, int bar, WPARAM action) {
    WindowObj* w = Win(hwnd);
    if (!w) return;
    const bool vertical = bar == SB_CTL ? (w->style & SBS_VERT) != 0 : bar == SB_VERT;
    const HWND owner = bar == SB_CTL ? w->parent : hwnd;
    const LPARAM ctl = bar == SB_CTL ? reinterpret_cast<LPARAM>(hwnd) : 0;
    if (owner && Win(owner)) SendMessageInternal(owner, vertical ? WM_VSCROLL : WM_HSCROLL, action, ctl);
}

// A click on `bar` at pt (drawing coordinates): one WM_xSCROLL for the part hit, then SB_ENDSCROLL.
bool ClickBar(HWND hwnd, int bar, POINT pt) {
    WindowObj* w = Win(hwnd);
    if (!w) return false;
    int part = HitTestBar(w, bar, pt);
    WPARAM action;
    switch (part) {
    case kTopArrow: action = SB_LINEUP; break;
    case kTopRect: action = SB_PAGEUP; break;
    case kBottomRect: action = SB_PAGEDOWN; break;
    case kBottomArrow: action = SB_LINEDOWN; break;
    case kThumb: {
        static volatile LONG reported = 0;   // plain flag
        if (!InterlockedExchange(&reported, 1)) FIXME(user, "scroll bar thumb dragging is not implemented");
        return true;
    }
    default: return false;
    }
    const Info* s = GetInfo(w, bar, false);
    // a disabled arrow does nothing
    if (s && ((part == kTopArrow && (s->flags & ESB_DISABLE_LTUP)) || (part == kBottomArrow && (s->flags & ESB_DISABLE_RTDN)))) return true;
    SendScroll(hwnd, bar, action);
    SendScroll(hwnd, bar, SB_ENDSCROLL);
    return true;
}

bool NcBarAt(WindowObj* w, POINT screen_pt, int* bar, POINT* local) {
    const RECT wr = WindowRectScreen(w);
    const POINT p{screen_pt.x - wr.left, screen_pt.y - wr.top};
    if ((w->style & WS_VSCROLL) && HitTestBar(w, SB_VERT, p) != kNowhere) { *bar = SB_VERT; *local = p; return true; }
    if ((w->style & WS_HSCROLL) && HitTestBar(w, SB_HORZ, p) != kNowhere) { *bar = SB_HORZ; *local = p; return true; }
    return false;
}

BOOL GetBarInfoImpl(HWND hwnd, LONG id, SCROLLBARINFO* info) {
    WindowObj* w = Win(hwnd);
    if (!w || !info || info->cbSize != sizeof(*info)) return FALSE;
    int bar;
    switch (id) {
    case OBJID_CLIENT: bar = SB_CTL; break;
    case OBJID_HSCROLL: bar = SB_HORZ; break;
    case OBJID_VSCROLL: bar = SB_VERT; break;
    default: return FALSE;
    }
    int dummy;
    BarRect(w, bar, &info->rcScrollBar, &dummy, &info->dxyLineButton, &info->xyThumbTop);
    const RECT wr = bar == SB_CTL ? ClientRectScreen(w) : WindowRectScreen(w);
    info->rcScrollBar = RectOffset(info->rcScrollBar, wr.left, wr.top);
    info->xyThumbBottom = info->xyThumbTop + info->dxyLineButton;
    const Info* s = GetInfo(w, bar, true);
    if (!s) return FALSE;
    info->rgstate[0] = 0;
    if ((bar == SB_HORZ && !(w->style & WS_HSCROLL)) || (bar == SB_VERT && !(w->style & WS_VSCROLL)))
        info->rgstate[0] |= STATE_SYSTEM_INVISIBLE;
    if (s->min >= s->max - std::max(s->page - 1, 0))
        info->rgstate[0] |= (info->rgstate[0] & STATE_SYSTEM_INVISIBLE) ? STATE_SYSTEM_OFFSCREEN : STATE_SYSTEM_UNAVAILABLE;
    if (bar == SB_CTL && (w->style & WS_DISABLED)) info->rgstate[0] |= STATE_SYSTEM_UNAVAILABLE;
    info->rgstate[1] = (s->flags & ESB_DISABLE_LTUP) ? STATE_SYSTEM_UNAVAILABLE : 0;
    info->rgstate[2] = s->pos == s->min ? STATE_SYSTEM_INVISIBLE : 0;
    info->rgstate[3] = 0;
    info->rgstate[4] = s->pos >= s->max - 1 ? STATE_SYSTEM_INVISIBLE : 0;
    info->rgstate[5] = (s->flags & ESB_DISABLE_RTDN) ? STATE_SYSTEM_UNAVAILABLE : 0;
    return TRUE;
}

BOOL EnableImpl(HWND hwnd, UINT bar, UINT flags) {
    WindowObj* w = WinChecked(hwnd);
    if (!w) return FALSE;
    hwnd = w->handle;
    flags &= ESB_DISABLE_BOTH;
    bool check_flags;
    if (bar == SB_BOTH) {
        Info* s = GetInfo(w, SB_VERT, true);
        if (!s) return FALSE;
        check_flags = s->flags == flags;
        s->flags = flags;
        if (!check_flags) DrawBar(hwnd, SB_VERT);
        bar = SB_HORZ;
    } else {
        check_flags = bar != SB_CTL;
    }
    w = Win(hwnd);
    Info* s = w ? GetInfo(w, bar, true) : nullptr;
    if (!s) return FALSE;
    if (check_flags) check_flags = s->flags == flags;
    s->flags = flags;
    if (check_flags) return FALSE;
    if (bar == SB_CTL && (flags == ESB_DISABLE_BOTH || flags == ESB_ENABLE_BOTH)) {
        if (flags == ESB_ENABLE_BOTH) w->style &= ~WS_DISABLED;
        else w->style |= WS_DISABLED;
    }
    DrawBar(hwnd, bar);
    return TRUE;
}

// Moves a rectangle of pixels of the DC's drawing target by (dx, dy); returns the uncovered part (device coords).
Rgn ScrollTarget(DcObj* dc, const RECT& src_in, const RECT* clip_in, int dx, int dy) {
    DrawTarget t;
    Rgn exposed;
    if (!ResolveDrawTarget(dc, &t)) return exposed;
    RECT src = src_in;
    if (clip_in) src = RectIntersect(src, *clip_in);
    if (RectEmpty(src)) return exposed;
    const int w_ = src.right - src.left, h_ = src.bottom - src.top;
    std::vector<std::uint32_t> copy(static_cast<std::size_t>(w_) * h_, 0xff000000u);
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_; ++x) {
            const int tx = src.left + x + t.origin.x, ty = src.top + y + t.origin.y;
            if (tx >= 0 && ty >= 0 && tx < t.px.width && ty < t.px.height) copy[static_cast<std::size_t>(y) * w_ + x] = t.px.Row(ty)[tx];
        }
    RECT dst = RectOffset(src, dx, dy);
    if (clip_in) dst = RectIntersect(dst, *clip_in);
    for (int y = dst.top; y < dst.bottom; ++y)
        for (int x = dst.left; x < dst.right; ++x) {
            if (!t.clip.Contains(x, y)) continue;
            const int tx = x + t.origin.x, ty = y + t.origin.y;
            if (tx < 0 || ty < 0 || tx >= t.px.width || ty >= t.px.height) continue;
            t.px.Row(ty)[tx] = copy[static_cast<std::size_t>(y - dy - src.top) * w_ + (x - dx - src.left)];
        }
    NoteDrawn(dc, t, dst);
    Rgn all, moved;
    all.Set(src);
    moved.Set(RectIntersect(RectOffset(src, dx, dy), src));
    return CombineRgns(all, moved, RGN_DIFF);
}

}  // namespace

// ---- Hooks -------------------------------------------------------------------------------------------------------
ULONG_PTR ScrollGetInfoCall(HWND hwnd, ULONG_PTR param) {
    auto* p = reinterpret_cast<GetScrollInfoParams*>(param);
    if (!p) return FALSE;
    return GetScrollInfoImpl(hwnd, p->bar, p->info);
}

LRESULT ScrollNcHitTest(WindowObj* w, POINT screen_pt) {
    int bar;
    POINT local;
    if (!NcBarAt(w, screen_pt, &bar, &local)) return 0;
    return bar == SB_VERT ? HTVSCROLL : HTHSCROLL;
}

void ScrollDrawNc(HWND hwnd) {
    WindowObj* w = Win(hwnd);
    if (!w || !(w->style & (WS_VSCROLL | WS_HSCROLL)) || !IsWindowReallyVisible(w)) return;
    const bool both = (w->style & WS_VSCROLL) && (w->style & WS_HSCROLL);
    if (w->style & WS_HSCROLL) DrawBar(hwnd, SB_HORZ);
    if (w->style & WS_VSCROLL) DrawBar(hwnd, SB_VERT);
    if (both && (w = Win(hwnd))) {
        const RECT c = ClientInWindow(w);
        HDC hdc = GetDCInternal(hwnd, nullptr, DCX_WINDOW | DCX_CACHE);
        DcObj* dc = DcGet(hdc);
        DrawTarget t;
        if (dc && ResolveDrawTarget(dc, &t)) {
            const RECT corner{c.right, c.bottom, c.right + SystemMetric(SM_CXVSCROLL), c.bottom + SystemMetric(SM_CYHSCROLL)};
            FillTargetRect(t, corner, Px(COLOR_BTNFACE));
            NoteDrawn(dc, t, corner);
        }
        ReleaseDCInternal(hwnd, hdc);
    }
}

bool ScrollNcLButtonDown(HWND hwnd, LPARAM screen_pt) {
    WindowObj* w = Win(hwnd);
    if (!w) return false;
    int bar;
    POINT local;
    const POINT pt{static_cast<short>(LOWORD(screen_pt)), static_cast<short>(HIWORD(screen_pt))};
    if (!NcBarAt(w, pt, &bar, &local)) return false;
    return ClickBar(w->handle, bar, local);
}

LRESULT ScrollBarWndProcInternal(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool ansi) {
    WindowObj* w = Win(hwnd);
    if (!w) return 0;
    hwnd = w->handle;
    switch (msg) {
    case WM_CREATE:
        GetInfo(w, SB_CTL, true);
        return 0;
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONDOWN:
        if (w->style & (SBS_SIZEGRIP | SBS_SIZEBOX)) return 0;   // FIXME: size grips do not resize
        ClickBar(hwnd, SB_CTL, POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        return 0;
    case WM_ENABLE:
        if (Info* s = GetInfo(w, SB_CTL, true)) s->flags = wp ? ESB_ENABLE_BOTH : ESB_DISABLE_BOTH;
        DrawBar(hwnd, SB_CTL);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        if (wp) {
            DrawBar(hwnd, SB_CTL);
        } else if (BeginPaintInternal(hwnd, &ps)) {
            DrawBar(hwnd, SB_CTL);
            EndPaintInternal(hwnd, &ps);
        }
        return 0;
    }
    case SBM_SETRANGEREDRAW:
    case SBM_SETRANGE: {
        Info* s = GetInfo(w, SB_CTL, true);
        if (!s) return 0;
        const int old = s->pos;
        s->min = static_cast<int>(wp);
        s->max = static_cast<int>(lp);
        if (msg == SBM_SETRANGEREDRAW) DrawBar(hwnd, SB_CTL);
        return old != s->pos ? old : 0;
    }
    case SBM_GETSCROLLINFO:
        return GetScrollInfoImpl(hwnd, SB_CTL, reinterpret_cast<SCROLLINFO*>(lp));
    case SBM_GETSCROLLBARINFO:
        return GetBarInfoImpl(hwnd, OBJID_CLIENT, reinterpret_cast<SCROLLBARINFO*>(lp));
    case SBM_SETSCROLLINFO:
        return SetScrollInfoImpl(hwnd, SB_CTL, reinterpret_cast<const SCROLLINFO*>(lp), static_cast<BOOL>(wp));
    case SBM_SETPOS: {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.nPos = static_cast<int>(wp);
        si.fMask = SIF_POS | 0x1000;
        return SetScrollInfoImpl(hwnd, SB_CTL, &si, static_cast<BOOL>(lp));
    }
    case SBM_GETPOS: {
        const Info* s = GetInfo(w, SB_CTL, false);
        return s ? s->pos : 0;
    }
    case SBM_GETRANGE: {
        const Info* s = GetInfo(w, SB_CTL, false);
        if (!s) return FALSE;
        if (wp) *reinterpret_cast<int*>(wp) = s->min;
        if (lp) *reinterpret_cast<int*>(lp) = s->max;
        return TRUE;
    }
    case SBM_ENABLE_ARROWS:
        return EnableImpl(hwnd, SB_CTL, static_cast<UINT>(wp));
    case WM_KEYDOWN: {
        WPARAM a;
        switch (wp) {
        case VK_PRIOR: a = SB_PAGEUP; break;
        case VK_NEXT: a = SB_PAGEDOWN; break;
        case VK_HOME: a = SB_TOP; break;
        case VK_END: a = SB_BOTTOM; break;
        case VK_UP: case VK_LEFT: a = SB_LINEUP; break;
        case VK_DOWN: case VK_RIGHT: a = SB_LINEDOWN; break;
        default: return 0;
        }
        SendScroll(hwnd, SB_CTL, a);
        return 0;
    }
    }
    return DefWindowProcInternal(hwnd, msg, wp, lp, ansi);
}

}  // namespace detail

// ---- Syscalls ----------------------------------------------------------------------------------------------------
namespace sys {
using namespace detail;

INT WINAPI NtUserSetScrollInfo(HWND hwnd, INT bar, const SCROLLINFO* info, BOOL redraw) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w) return 0;
    if (bar == SB_CTL) return static_cast<INT>(SendMessageInternal(w->handle, SBM_SETSCROLLINFO, redraw, reinterpret_cast<LPARAM>(info)));
    return SetScrollInfoImpl(w->handle, bar, info, redraw);
}

BOOL WINAPI NtUserShowScrollBar(HWND hwnd, INT bar, BOOL show) {
    LockGuard lock;
    WindowObj* w = hwnd ? Win(hwnd) : nullptr;
    if (!w) return FALSE;
    ShowBars(w->handle, bar, bar == SB_VERT ? false : show != 0, bar == SB_HORZ ? false : show != 0);
    return TRUE;
}

BOOL WINAPI NtUserEnableScrollBar(HWND hwnd, UINT bar, UINT flags) {
    LockGuard lock;
    return EnableImpl(hwnd, bar, flags);
}

BOOL WINAPI NtUserGetScrollBarInfo(HWND hwnd, LONG id, SCROLLBARINFO* info) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w) return FALSE;
    if (id == OBJID_CLIENT) return static_cast<BOOL>(SendMessageInternal(w->handle, SBM_GETSCROLLBARINFO, 0, reinterpret_cast<LPARAM>(info)));
    return GetBarInfoImpl(w->handle, id, info);
}

BOOL WINAPI NtUserScrollDC(HDC hdc, INT dx, INT dy, const RECT* scroll, const RECT* clip, HRGN update_rgn,
                           RECT* update_rect) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) {
        SetLastErr(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    DrawTarget t;
    if (!ResolveDrawTarget(dc, &t)) return FALSE;
    RECT src;
    if (scroll) src = *scroll;
    else if (dc->kind == DcKind::Window && Win(dc->hwnd))
        src = RECT{0, 0, Win(dc->hwnd)->client.right - Win(dc->hwnd)->client.left, Win(dc->hwnd)->client.bottom - Win(dc->hwnd)->client.top};
    else
        src = RECT{0, 0, t.px.width, t.px.height};
    Rgn exposed = ScrollTarget(dc, src, clip, dx, dy);
    if (update_rect) *update_rect = exposed.Box();
    if (auto* r = GdiGetT<RegionObj>(update_rgn, abi::kObjRegion)) r->rgn = exposed;
    return TRUE;
}

}  // namespace sys
}  // namespace win10xbox::user
