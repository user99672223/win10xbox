// core/user windows: the handle table, the tree (parent / owner / z-ordered
// children), rectangles, NtUserCreateWindowEx with the message sequence of
// Wine's window.c, ShowWindow / SetWindowPos (winpos.c), DestroyWindow,
// activation and focus, window longs, properties, and the NtUserCall* code
// multiplexers that user32 uses for the small queries.
//
// Handles: HWND = (0x20 + slot) | (generation << 16), generation 1..0x7fff,
// so a handle fits 32 bits, is never 0, never has bit 31 set and never looks
// like HWND_BROADCAST / HWND_MESSAGE / HWND_TOPMOST (user32's
// WIN_GetFullHandle passes anything with a high word through unchanged).
#include "user_internal.h"
#include "user_syscalls.h"

#include <algorithm>
#include <cstring>
#include <cwctype>

namespace win10xbox::user {
namespace detail {

ULONG_PTR GetClassLongSize(HWND hwnd, INT offset, UINT size, bool ansi);   // win_class.cpp
void SendNcCalcSize(HWND hwnd, RECT* window_parent, RECT* client_out);     // below

namespace {

constexpr std::uint32_t kFirstUserHandle = 0x20;
constexpr std::uint32_t kMaxUserHandles = 0xffef - kFirstUserHandle;

HWND MakeHwnd(std::uint32_t slot, WORD gen) { return EncodeHwnd(slot, gen); }

WindowObj* AllocWindow() {
    State& s = S();
    std::uint32_t slot;
    if (!s.free_slots.empty()) {
        slot = s.free_slots.back();
        s.free_slots.pop_back();
    } else {
        if (s.windows.size() >= kMaxUserHandles) return nullptr;
        slot = static_cast<std::uint32_t>(s.windows.size());
        s.windows.push_back(nullptr);
        s.generations.push_back(0);
    }
    WORD& gen = s.generations[slot];
    gen = static_cast<WORD>(gen + 1);
    if (gen > 0x7fff) gen = 1;
    auto* w = new WindowObj;
    w->handle = MakeHwnd(slot, gen);
    s.windows[slot] = w;
    return w;
}

void FreeWindowSlot(WindowObj* w) {
    State& s = S();
    const std::uint32_t slot = (static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(w->handle)) & 0xffff) - kFirstUserHandle;
    if (slot < s.windows.size() && s.windows[slot] == w) {
        s.windows[slot] = nullptr;
        s.free_slots.push_back(slot);
    }
    delete w;
}

void Unlink(WindowObj* w) {
    if (WindowObj* p = Win(w->parent)) {
        auto& v = p->children;
        v.erase(std::remove(v.begin(), v.end(), w->handle), v.end());
    }
}

// Inserts `w` in its parent's z-order: HWND_TOP / HWND_BOTTOM / after a sibling.
void LinkAfter(WindowObj* w, HWND after) {
    WindowObj* p = Win(w->parent);
    if (!p) return;
    auto& v = p->children;
    v.erase(std::remove(v.begin(), v.end(), w->handle), v.end());
    if (after == HWND_BOTTOM) { v.push_back(w->handle); return; }
    if (after == HWND_TOP || after == HWND_TOPMOST || after == HWND_NOTOPMOST || !after) {
        // topmost windows stay above the rest
        auto it = v.begin();
        if (!(w->ex_style & WS_EX_TOPMOST))
            while (it != v.end()) {
                WindowObj* c = Win(*it);
                if (!c || !(c->ex_style & WS_EX_TOPMOST)) break;
                ++it;
            }
        v.insert(it, w->handle);
        return;
    }
    auto it = std::find(v.begin(), v.end(), FullHandle(after));
    if (it == v.end()) v.insert(v.begin(), w->handle);
    else v.insert(it + 1, w->handle);
}

WindowObj* CreateSpecialWindow(const wchar_t* class_name, UINT flag) {
    State& s = S();
    if (!s.builtins_registered) RegisterBuiltinClasses();
    ClassObj* cls = FindClass(nullptr, class_name, wcslen(class_name));
    WindowObj* w = AllocWindow();
    if (!w || !cls) return w;
    w->cls = cls;
    ++cls->windows;
    w->proc = cls->proc;
    w->style = WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    if (flag == kWinIsDesktop) w->style |= WS_VISIBLE;
    w->flags = flag;
    RECT screen;
    ScreenRect(&screen);
    if (flag == kWinIsDesktop) w->window = w->client = screen;
    else w->window = w->client = RECT{0, 0, 100, 100};
    w->extra.assign(static_cast<std::size_t>(cls->win_extra), 0);
    return w;
}

bool IsDescendant(HWND ancestor, HWND hwnd) {
    for (WindowObj* w = Win(hwnd); w; w = Win(w->parent))
        if (w->parent == ancestor) return true;
    return false;
}

MINMAXINFO DefaultMinMax(const WindowObj* w) {
    MINMAXINFO mm{};
    RECT work;
    WorkAreaRect(&work);
    RECT frame{0, 0, 0, 0};
    AdjustRectForStyle(&frame, w->style & ~WS_DLGFRAME, false, w->ex_style);   // maximized windows hide the border
    mm.ptMaxSize = POINT{work.right - work.left + (frame.right - frame.left), work.bottom - work.top + (frame.bottom - frame.top)};
    mm.ptMaxPosition = POINT{work.left + frame.left, work.top + frame.top};
    mm.ptMinTrackSize = POINT{SystemMetric(SM_CXMINTRACK), SystemMetric(SM_CYMINTRACK)};
    mm.ptMaxTrackSize = POINT{SystemMetric(SM_CXMAXTRACK), SystemMetric(SM_CYMAXTRACK)};
    return mm;
}

MINMAXINFO GetMinMaxInfo(HWND hwnd) {
    WindowObj* w = Win(hwnd);
    MINMAXINFO mm = w ? DefaultMinMax(w) : MINMAXINFO{};
    if (w) SendMessageInternal(hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&mm));
    return mm;
}

HWND ActivateOtherWindow(HWND hwnd) {
    // the next visible, enabled top-level window of the same thread (or nothing)
    WindowObj* w = Win(hwnd);
    WindowObj* desk = Win(DesktopWindow());
    if (!w || !desk) return nullptr;
    for (HWND h : desk->children) {
        if (h == hwnd) continue;
        WindowObj* c = Win(h);
        if (c && c->tid == w->tid && (c->style & WS_VISIBLE) && !(c->style & WS_DISABLED) &&
            !(c->ex_style & WS_EX_NOACTIVATE))
            return h;
    }
    return nullptr;
}

}  // namespace

// ---- Lookup and geometry -------------------------------------------------------------

HWND EncodeHwnd(std::uint32_t slot, WORD generation) {
    return reinterpret_cast<HWND>(static_cast<ULONG_PTR>((kFirstUserHandle + slot) | (static_cast<DWORD>(generation) << 16)));
}

bool DecodeHwnd(HWND hwnd, std::uint32_t* slot, WORD* generation) {
    const auto v = static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(hwnd));
    const std::uint32_t low = v & 0xffff;
    if (!hwnd || low < kFirstUserHandle || low > kFirstUserHandle + kMaxUserHandles) return false;
    *slot = low - kFirstUserHandle;
    *generation = static_cast<WORD>(v >> 16);   // 0 / 0xffff: a truncated (16-bit) handle, any generation
    return true;
}

WindowObj* Win(HWND hwnd) {
    State* s = StateIfAny();
    std::uint32_t slot;
    WORD gen;
    if (!s || !DecodeHwnd(hwnd, &slot, &gen)) return nullptr;
    if (slot >= s->windows.size() || !s->windows[slot]) return nullptr;
    if (gen && gen != 0xffff && gen != s->generations[slot]) return nullptr;
    return s->windows[slot];
}

WindowObj* WinChecked(HWND hwnd) {
    WindowObj* w = Win(hwnd);
    if (!w) SetLastErr(ERROR_INVALID_WINDOW_HANDLE);
    return w;
}

HWND FullHandle(HWND hwnd) {
    WindowObj* w = Win(hwnd);
    return w ? w->handle : hwnd;
}

HWND DesktopWindow() {
    State& s = S();
    if (!Win(s.desktop)) {
        WindowObj* w = CreateSpecialWindow(L"#32769", kWinIsDesktop);
        s.desktop = w ? w->handle : nullptr;
        if (w) {
            TRACE(user, "desktop window %p", w->handle);
            if (winexbox::IWinexboxDriver* d = Driver()) {
                d->WindowCreate(reinterpret_cast<winexbox::WindowId>(w->handle));
                d->DesktopSetWindow(reinterpret_cast<winexbox::WindowId>(w->handle));
                w->driver_created = true;
            }
        }
    }
    return s.desktop;
}

HWND MessageParentWindow() {
    State& s = S();
    if (!Win(s.msg_parent)) {
        WindowObj* w = CreateSpecialWindow(L"Message", kWinIsMsgParent);
        s.msg_parent = w ? w->handle : nullptr;
    }
    return s.msg_parent;
}

bool IsTopLevel(const WindowObj* w) {
    State& s = S();
    return w && w->parent && w->parent == s.desktop;
}

HWND TopLevelOf(HWND hwnd) {
    WindowObj* w = Win(hwnd);
    while (w && !IsTopLevel(w)) {
        WindowObj* p = Win(w->parent);
        if (!p) return w->handle;
        w = p;
    }
    return w ? w->handle : nullptr;
}

POINT ClientOriginScreen(const WindowObj* w) {
    POINT p{0, 0};
    for (const WindowObj* c = w; c; c = Win(c->parent)) {
        p.x += c->client.left;
        p.y += c->client.top;
        if (c->flags & (kWinIsDesktop | kWinIsMsgParent)) break;
    }
    return p;
}

RECT WindowRectScreen(const WindowObj* w) {
    if (w->flags & (kWinIsDesktop | kWinIsMsgParent)) return w->window;
    WindowObj* p = Win(w->parent);
    const POINT o = p ? ClientOriginScreen(p) : POINT{0, 0};
    return RectOffset(w->window, o.x, o.y);
}

RECT ClientRectScreen(const WindowObj* w) {
    if (w->flags & (kWinIsDesktop | kWinIsMsgParent)) return w->client;
    WindowObj* p = Win(w->parent);
    const POINT o = p ? ClientOriginScreen(p) : POINT{0, 0};
    return RectOffset(w->client, o.x, o.y);
}

bool IsWindowReallyVisible(const WindowObj* w) {
    for (const WindowObj* c = w; c; c = Win(c->parent)) {
        if (!(c->style & WS_VISIBLE)) return false;
        if (c->flags & kWinIsDesktop) return true;
    }
    return false;
}

void ForEachWindowPreorder(HWND root, const std::function<bool(WindowObj*)>& fn) {
    WindowObj* w = Win(root);
    if (!w) return;
    if (!fn(w)) return;
    const std::vector<HWND> kids = w->children;
    for (HWND c : kids) ForEachWindowPreorder(c, fn);
}

HWND WindowFromScreenPoint(POINT pt) {
    WindowObj* desk = Win(DesktopWindow());
    if (!desk) return nullptr;
    HWND found = nullptr;
    for (HWND h : desk->children) {
        WindowObj* w = Win(h);
        if (!w || !(w->style & WS_VISIBLE) || !PtInRectangle(WindowRectScreen(w), pt.x, pt.y)) continue;
        found = h;
        break;
    }
    // descend into children
    for (WindowObj* w = Win(found); w;) {
        if (!PtInRectangle(ClientRectScreen(w), pt.x, pt.y)) break;
        HWND next = nullptr;
        for (HWND h : w->children) {
            WindowObj* c = Win(h);
            if (!c || !(c->style & WS_VISIBLE) || (c->style & WS_DISABLED) || (c->ex_style & WS_EX_TRANSPARENT)) continue;
            if (PtInRectangle(WindowRectScreen(c), pt.x, pt.y)) { next = h; break; }
        }
        if (!next) break;
        found = next;
        w = Win(next);
    }
    return found;
}

// server/window.c is_point_in_window: visible, not a disabled child, not layered+transparent, inside the window rectangle.
static bool PointInWindow(const WindowObj* w, POINT pt) {
    if (!(w->style & WS_VISIBLE)) return false;
    if ((w->style & (WS_POPUP | WS_CHILD | WS_DISABLED)) == (WS_CHILD | WS_DISABLED)) return false;
    if ((w->ex_style & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) == (WS_EX_LAYERED | WS_EX_TRANSPARENT)) return false;
    return PtInRectangle(WindowRectScreen(w), pt.x, pt.y);
}

static void AppendChildrenFromPoint(const WindowObj* parent, POINT pt, std::vector<HWND>* out) {
    const std::vector<HWND> kids = parent->children;
    for (HWND h : kids) {
        const WindowObj* c = Win(h);
        if (!c || !PointInWindow(c, pt)) continue;
        if (!(c->style & (WS_MINIMIZE | WS_DISABLED)) && PtInRectangle(ClientRectScreen(c), pt.x, pt.y))
            AppendChildrenFromPoint(c, pt, out);
        out->push_back(h);
    }
}

void WindowsFromPoint(HWND scope, POINT pt, std::vector<HWND>* out) {
    const WindowObj* top = Win(scope ? scope : DesktopWindow());
    if (!top) return;
    const bool desktop = (top->flags & kWinIsDesktop) != 0;
    if (!desktop && !PointInWindow(top, pt)) return;
    if (!(top->style & (WS_MINIMIZE | WS_DISABLED)) && PtInRectangle(ClientRectScreen(top), pt.x, pt.y))
        AppendChildrenFromPoint(top, pt, out);
    out->push_back(top->handle);
}

HWND ShallowWindowFromPoint(POINT pt) {
    const WindowObj* desk = Win(DesktopWindow());
    if (!desk) return nullptr;
    for (HWND h : desk->children)
        if (const WindowObj* w = Win(h); w && PointInWindow(w, pt)) return h;
    return desk->handle;
}

void AdjustRectForStyle(RECT* rect, DWORD style, bool menu, DWORD ex_style) {
    int adjust = 0;
    if ((ex_style & (WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME)) == WS_EX_STATICEDGE) adjust = 1;
    else if ((ex_style & WS_EX_DLGMODALFRAME) || (style & (WS_THICKFRAME | WS_DLGFRAME))) adjust = 2;
    if (style & WS_THICKFRAME) adjust += SystemMetric(SM_CXBORDER) + SystemMetric(SM_CXPADDEDBORDER);
    if ((style & (WS_BORDER | WS_DLGFRAME)) || (ex_style & WS_EX_DLGMODALFRAME)) adjust++;
    rect->left -= adjust; rect->top -= adjust; rect->right += adjust; rect->bottom += adjust;
    if ((style & WS_CAPTION) == WS_CAPTION)
        rect->top -= (ex_style & WS_EX_TOOLWINDOW) ? SystemMetric(SM_CYSMCAPTION) : SystemMetric(SM_CYCAPTION);
    if (menu) rect->top -= SystemMetric(SM_CYMENU);
    if (ex_style & WS_EX_CLIENTEDGE) {
        rect->left -= SystemMetric(SM_CXEDGE); rect->right += SystemMetric(SM_CXEDGE);
        rect->top -= SystemMetric(SM_CYEDGE); rect->bottom += SystemMetric(SM_CYEDGE);
    }
}

void SendNcCalcSize(HWND hwnd, RECT* window_parent, RECT* client_out) {
    WindowObj* w = Win(hwnd);
    if (!w) return;
    WindowObj* p = Win(w->parent);
    const POINT o = p ? ClientOriginScreen(p) : POINT{0, 0};
    // the rectangle is in screen coordinates for WM_NCCALCSIZE when wparam is FALSE
    RECT r = RectOffset(*window_parent, o.x, o.y);
    SendMessageInternal(hwnd, WM_NCCALCSIZE, FALSE, reinterpret_cast<LPARAM>(&r));
    *client_out = RectOffset(r, -o.x, -o.y);
    if (client_out->right < client_out->left) client_out->right = client_out->left;
    if (client_out->bottom < client_out->top) client_out->bottom = client_out->top;
}

void SetWindowTextInternal(WindowObj* w, const WCHAR* text) {
    w->text = text ? text : L"";
    if (w->driver_created)
        if (winexbox::IWinexboxDriver* d = Driver())
            d->WindowSetText(reinterpret_cast<winexbox::WindowId>(w->handle),
                             std::u16string(reinterpret_cast<const char16_t*>(w->text.c_str()), w->text.size()));
    if ((w->style & WS_CAPTION) == WS_CAPTION) w->nc_pending = true;
}

// ---- Activation and focus ------------------------------------------------------------------

HWND SetFocusInternal(HWND hwnd) {
    UserThread& t = CurrentUserThread();
    HWND prev = t.focus;
    if (hwnd) {
        WindowObj* w = WinChecked(hwnd);
        if (!w) return nullptr;
        hwnd = w->handle;
        if (w->tid != t.tid) {
            FIXME(user, "SetFocus(%p) of another thread's window", hwnd);
            return nullptr;
        }
        if (w->style & (WS_MINIMIZE | WS_DISABLED)) return nullptr;
        // activate the top-level window first
        HWND top = TopLevelOf(hwnd);
        if (top != t.active) {
            SetActiveWindowInternal(top, false);
            if (!Win(hwnd)) return nullptr;
            if (t.focus == hwnd) return prev;
        }
    }
    if (prev == hwnd) return prev;
    t.focus = hwnd;
    TRACE(user, "focus %p -> %p", prev, hwnd);
    if (prev && Win(prev)) SendMessageInternal(prev, WM_KILLFOCUS, reinterpret_cast<WPARAM>(hwnd), 0);
    if (hwnd && t.focus == hwnd && Win(hwnd)) SendMessageInternal(hwnd, WM_SETFOCUS, reinterpret_cast<WPARAM>(prev), 0);
    return prev;
}

bool SetActiveWindowInternal(HWND hwnd, bool mouse) {
    UserThread& t = CurrentUserThread();
    State& s = S();
    HWND prev = t.active;
    if (hwnd) {
        WindowObj* w = Win(hwnd);
        if (!w || !IsTopLevel(w)) return false;
        hwnd = w->handle;
    }
    if (prev == hwnd) return true;
    t.active = hwnd;
    const bool first = !prev && s.foreground != hwnd;
    if (hwnd) s.foreground = hwnd;
    TRACE(user, "active %p -> %p", prev, hwnd);
    if (prev && Win(prev)) {
        SendMessageInternal(prev, WM_NCACTIVATE, FALSE, reinterpret_cast<LPARAM>(hwnd));
        if (Win(prev))
            SendMessageInternal(prev, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, (Win(prev)->style & WS_MINIMIZE) != 0),
                                reinterpret_cast<LPARAM>(hwnd));
    }
    if (!hwnd) {
        if (t.focus) {
            HWND f = t.focus;
            t.focus = nullptr;
            if (Win(f)) SendMessageInternal(f, WM_KILLFOCUS, 0, 0);
        }
        return true;
    }
    if (first) {
        // the thread becomes the foreground one: every top-level window of it hears about it
        std::vector<HWND> tops;
        if (WindowObj* desk = Win(s.desktop))
            for (HWND h : desk->children)
                if (WindowObj* c = Win(h); c && c->tid == t.tid) tops.push_back(h);
        for (HWND h : tops)
            if (Win(h)) SendMessageInternal(h, WM_ACTIVATEAPP, TRUE, 0);
    }
    if (!Win(hwnd) || t.active != hwnd) return true;
    if (winexbox::IWinexboxDriver* d = Driver())
        d->WindowActivate(reinterpret_cast<winexbox::WindowId>(hwnd), reinterpret_cast<winexbox::WindowId>(prev));
    SendMessageInternal(hwnd, WM_NCACTIVATE, TRUE, reinterpret_cast<LPARAM>(prev));
    if (WindowObj* w = Win(hwnd))
        SendMessageInternal(hwnd, WM_ACTIVATE, MAKEWPARAM(mouse ? WA_CLICKACTIVE : WA_ACTIVE, (w->style & WS_MINIMIZE) != 0),
                            reinterpret_cast<LPARAM>(prev));
    // focus that belongs to another top-level window goes away (DefWindowProc's WM_ACTIVATE sets it again)
    if (t.focus && TopLevelOf(t.focus) != hwnd && t.active == hwnd) SetFocusInternal(nullptr);
    return true;
}

// ---- SetWindowPos ----------------------------------------------------------------------------

BOOL SetWindowPosInternal(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    WindowObj* w = WinChecked(hwnd);
    if (!w || (w->flags & kWinIsDesktop)) return FALSE;
    hwnd = w->handle;
    WINDOWPOS wp{hwnd, after, x, y, cx, cy, flags};
    if (flags & SWP_NOSIZE) { wp.cx = w->window.right - w->window.left; wp.cy = w->window.bottom - w->window.top; }
    if (flags & SWP_NOMOVE) { wp.x = w->window.left; wp.y = w->window.top; }
    if (wp.cx < 0) wp.cx = 0;
    if (wp.cy < 0) wp.cy = 0;
    if (w->style & WS_VISIBLE) wp.flags &= ~SWP_SHOWWINDOW;
    else {
        if (!(wp.flags & SWP_SHOWWINDOW)) wp.flags |= SWP_NOREDRAW;
        wp.flags &= ~SWP_HIDEWINDOW;
    }
    if ((w->style & WS_CHILD) || !IsTopLevel(w)) wp.flags |= SWP_NOACTIVATE;
    if (wp.hwndInsertAfter == hwnd) wp.flags |= SWP_NOZORDER;
    if (!(wp.flags & SWP_NOSENDCHANGING)) {
        SendMessageInternal(hwnd, WM_WINDOWPOSCHANGING, 0, reinterpret_cast<LPARAM>(&wp));
        w = Win(hwnd);
        if (!w) return FALSE;
    }
    const RECT old_window = w->window, old_client = w->client;
    RECT new_window{wp.x, wp.y, wp.x + wp.cx, wp.y + wp.cy};
    if (wp.flags & SWP_NOMOVE) new_window = RECT{old_window.left, old_window.top, old_window.left + wp.cx, old_window.top + wp.cy};
    if (wp.flags & SWP_NOSIZE) new_window.right = new_window.left + (old_window.right - old_window.left),
                               new_window.bottom = new_window.top + (old_window.bottom - old_window.top);
    RECT new_client = RectOffset(old_client, new_window.left - old_window.left, new_window.top - old_window.top);
    const bool size_changed = (new_window.right - new_window.left) != (old_window.right - old_window.left) ||
                              (new_window.bottom - new_window.top) != (old_window.bottom - old_window.top);
    if (size_changed || (wp.flags & SWP_FRAMECHANGED)) {
        NCCALCSIZE_PARAMS nc{};
        WINDOWPOS wp_copy = wp;
        nc.rgrc[0] = new_window;
        nc.rgrc[1] = old_window;
        nc.rgrc[2] = old_client;
        nc.lppos = &wp_copy;
        SendMessageInternal(hwnd, WM_NCCALCSIZE, TRUE, reinterpret_cast<LPARAM>(&nc));
        w = Win(hwnd);
        if (!w) return FALSE;
        new_client = nc.rgrc[0];
        if (new_client.right < new_client.left) new_client.right = new_client.left;
        if (new_client.bottom < new_client.top) new_client.bottom = new_client.top;
    }
    const bool client_moved = new_client.left != old_client.left || new_client.top != old_client.top;
    const bool client_sized = (new_client.right - new_client.left) != (old_client.right - old_client.left) ||
                              (new_client.bottom - new_client.top) != (old_client.bottom - old_client.top);
    const bool was_visible = IsWindowReallyVisible(w);
    w->window = new_window;
    w->client = new_client;
    if (!(wp.flags & SWP_NOZORDER) && !(w->flags & kWinIsMsgParent)) {
        if (wp.hwndInsertAfter == HWND_TOPMOST) w->ex_style |= WS_EX_TOPMOST;
        else if (wp.hwndInsertAfter == HWND_NOTOPMOST) w->ex_style &= ~WS_EX_TOPMOST;
        LinkAfter(w, wp.hwndInsertAfter);
    }
    if (wp.flags & SWP_SHOWWINDOW) w->style |= WS_VISIBLE;
    if (wp.flags & SWP_HIDEWINDOW) w->style &= ~WS_VISIBLE;
    const bool now_visible = IsWindowReallyVisible(w);
    if (IsTopLevel(w)) EnsureSurface(w);
    if (w->driver_created)
        if (winexbox::IWinexboxDriver* d = Driver()) {
            winexbox::WindowRects rects;
            const RECT ws = WindowRectScreen(w), cs = ClientRectScreen(w);
            rects.window = winexbox::Rect{ws.left, ws.top, ws.right, ws.bottom};
            rects.client = winexbox::Rect{cs.left, cs.top, cs.right, cs.bottom};
            rects.visible = now_visible ? rects.window : winexbox::Rect{};
            if (wp.flags & SWP_SHOWWINDOW) d->WindowShow(reinterpret_cast<winexbox::WindowId>(hwnd), winexbox::ShowCmd::Show, nullptr, wp.flags);
            if (wp.flags & SWP_HIDEWINDOW) d->WindowShow(reinterpret_cast<winexbox::WindowId>(hwnd), winexbox::ShowCmd::Hide, nullptr, wp.flags);
            d->WindowPosChanged(reinterpret_cast<winexbox::WindowId>(hwnd), 0, 0, wp.flags, rects, w->surface.get());
        }
    // invalidation
    if (now_visible && !(wp.flags & SWP_NOREDRAW)) {
        const bool redraw_all = (wp.flags & SWP_SHOWWINDOW) || (wp.flags & SWP_FRAMECHANGED) ||
                                (client_sized && (w->cls->style & (CS_HREDRAW | CS_VREDRAW))) || (IsTopLevel(w) && size_changed);
        if (redraw_all) InvalidateWindow(hwnd, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        else if (client_sized) {
            // newly exposed parts of the client area
            const int ow = old_client.right - old_client.left, oh = old_client.bottom - old_client.top;
            const int nw = new_client.right - new_client.left, nh = new_client.bottom - new_client.top;
            if (nw > ow) { RECT r{ow, 0, nw, nh}; InvalidateWindow(hwnd, &r, RDW_INVALIDATE | RDW_ERASE); }
            if (nh > oh) { RECT r{0, oh, nw, nh}; InvalidateWindow(hwnd, &r, RDW_INVALIDATE | RDW_ERASE); }
            if (size_changed) w->nc_pending = true;
        } else if (size_changed) {
            w->nc_pending = true;
        }
    }
    if (!IsTopLevel(w) && was_visible && (!now_visible || size_changed || client_moved ||
                                          old_window.left != new_window.left || old_window.top != new_window.top)) {
        // what the child uncovered in its parent
        if (WindowObj* p = Win(w->parent)) {
            RECT r = old_window;
            InvalidateWindow(p->handle, &r, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            if (now_visible && !(wp.flags & SWP_NOREDRAW))
                InvalidateWindow(hwnd, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        }
    }
    if (IsTopLevel(w) && (was_visible || now_visible)) NoteScreenChanged();
    // activation
    if (!(wp.flags & SWP_NOACTIVATE) && now_visible && !(w->ex_style & WS_EX_NOACTIVATE)) {
        SetActiveWindowInternal(hwnd, false);
        w = Win(hwnd);
        if (!w) return TRUE;
    }
    if ((wp.flags & SWP_HIDEWINDOW) && FindUserThread() && FindUserThread()->active == hwnd)
        SetActiveWindowInternal(ActivateOtherWindow(hwnd), false);
    // WM_WINDOWPOSCHANGED with the final geometry
    WINDOWPOS done = wp;
    if (WindowObj* cur = Win(hwnd)) {
        done.x = cur->window.left;
        done.y = cur->window.top;
        done.cx = cur->window.right - cur->window.left;
        done.cy = cur->window.bottom - cur->window.top;
    }
    if (!client_moved) done.flags |= abi::kSwpNoClientMove;
    if (!client_sized) done.flags |= abi::kSwpNoClientSize;
    if (!(wp.flags & SWP_NOSENDCHANGING) || true)
        SendMessageInternal(hwnd, WM_WINDOWPOSCHANGED, 0, reinterpret_cast<LPARAM>(&done));
    return TRUE;
}

// ---- ShowWindow -------------------------------------------------------------------------------

BOOL ShowWindowInternal(HWND hwnd, int cmd) {
    WindowObj* w = WinChecked(hwnd);
    if (!w || (w->flags & kWinIsDesktop)) return FALSE;
    hwnd = w->handle;
    const bool was_visible = (w->style & WS_VISIBLE) != 0;
    const bool child = (w->style & WS_CHILD) != 0;
    UINT swp = 0;
    RECT r = w->window;
    bool show = true;
    if (cmd == SW_SHOWDEFAULT) cmd = SW_SHOWNORMAL;
    switch (cmd) {
    case SW_HIDE:
        if (!was_visible) return FALSE;
        show = false;
        swp = SWP_HIDEWINDOW | SWP_NOSIZE | SWP_NOMOVE;
        if (FindUserThread() && FindUserThread()->active != hwnd) swp |= SWP_NOACTIVATE | SWP_NOZORDER;
        break;
    case SW_SHOWMINNOACTIVE: case SW_SHOWMINIMIZED: case SW_FORCEMINIMIZE: case SW_MINIMIZE:
        swp = SWP_SHOWWINDOW | SWP_FRAMECHANGED | ((cmd == SW_SHOWMINIMIZED) ? 0 : SWP_NOACTIVATE);
        if (!(w->style & WS_MINIMIZE)) {
            if (!(w->style & WS_MAXIMIZE)) w->normal = w->window;
            w->style = (w->style | WS_MINIMIZE) & ~WS_MAXIMIZE;
            r = RECT{-32000, -32000, -32000 + SystemMetric(SM_CXMINIMIZED), -32000 + SystemMetric(SM_CYMINIMIZED)};
        } else swp |= SWP_NOSIZE | SWP_NOMOVE;
        break;
    case SW_SHOWMAXIMIZED: {
        swp = SWP_SHOWWINDOW | SWP_FRAMECHANGED;
        if (!(w->style & WS_MAXIMIZE)) {
            if (!(w->style & WS_MINIMIZE)) w->normal = w->window;
            const MINMAXINFO mm = GetMinMaxInfo(hwnd);
            w = Win(hwnd);
            if (!w) return was_visible;
            w->style = (w->style | WS_MAXIMIZE) & ~WS_MINIMIZE;
            r = RECT{mm.ptMaxPosition.x, mm.ptMaxPosition.y, mm.ptMaxPosition.x + mm.ptMaxSize.x, mm.ptMaxPosition.y + mm.ptMaxSize.y};
        } else swp |= SWP_NOSIZE | SWP_NOMOVE;
        break;
    }
    case SW_SHOWNA:
        swp = SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE;
        if (child) swp |= SWP_NOZORDER;
        break;
    case SW_SHOW:
        if (was_visible) return TRUE;
        swp = SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE;
        if (child) swp |= SWP_NOACTIVATE | SWP_NOZORDER;
        break;
    case SW_SHOWNOACTIVATE:
        swp = SWP_NOACTIVATE;
        [[fallthrough]];
    case SW_RESTORE:
    case SW_SHOWNORMAL:
        swp |= SWP_SHOWWINDOW;
        if (w->style & (WS_MINIMIZE | WS_MAXIMIZE)) {
            w->style &= ~(WS_MINIMIZE | WS_MAXIMIZE);
            r = w->normal;
            swp |= SWP_FRAMECHANGED;
        } else {
            if (was_visible && cmd != SW_RESTORE && cmd != SW_SHOWNORMAL) return TRUE;
            swp |= SWP_NOSIZE | SWP_NOMOVE;
        }
        if (child && !(swp & SWP_NOACTIVATE)) swp |= SWP_NOACTIVATE | SWP_NOZORDER;
        break;
    default:
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if ((show != was_visible || cmd == SW_SHOWNA) && cmd != SW_SHOWMAXIMIZED && !(w->style & WS_MINIMIZE)) {
        SendMessageInternal(hwnd, WM_SHOWWINDOW, show, 0);
        w = Win(hwnd);
        if (!w) return was_visible;
    }
    WindowObj* parent = Win(w->parent);
    if (child && parent && !IsWindowReallyVisible(parent)) {
        // a child of a hidden parent only changes its style
        if (show) w->style |= WS_VISIBLE;
        else w->style &= ~WS_VISIBLE;
    } else {
        SetWindowPosInternal(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, swp);
    }
    w = Win(hwnd);
    if (!w) return was_visible;
    if (cmd == SW_HIDE) {
        UserThread& t = CurrentUserThread();
        if (t.focus == hwnd || (t.focus && IsDescendant(hwnd, t.focus))) SetFocusInternal(child ? w->parent : nullptr);
    }
    if (w->flags & kWinNeedSize) {
        w->flags &= ~kWinNeedSize;
        const UINT how = (w->style & WS_MAXIMIZE) ? SIZE_MAXIMIZED : (w->style & WS_MINIMIZE) ? SIZE_MINIMIZED : SIZE_RESTORED;
        const RECT c = w->client;
        SendMessageInternal(hwnd, WM_SIZE, how, MAKELONG(c.right - c.left, c.bottom - c.top));
        SendMessageInternal(hwnd, WM_MOVE, 0, MAKELONG(c.left, c.top));
    }
    return was_visible;
}

// ---- DestroyWindow ---------------------------------------------------------------------------

namespace {

void SendDestroyMessage(HWND hwnd) {
    UserThread& t = CurrentUserThread();
    if (t.focus == hwnd) t.focus = nullptr;
    if (t.capture == hwnd) t.capture = nullptr;
    SendMessageInternal(hwnd, WM_DESTROY, 0, 0);
    WindowObj* w = Win(hwnd);
    if (!w) return;
    const std::vector<HWND> kids = w->children;
    for (HWND c : kids)
        if (Win(c)) SendDestroyMessage(c);
}

void FreeWindow(HWND hwnd, bool send) {
    WindowObj* w = Win(hwnd);
    if (!w) return;
    const std::vector<HWND> kids = w->children;
    for (HWND c : kids) FreeWindow(c, send);
    if (send) {
        SendMessageInternal(hwnd, WM_NCDESTROY, 0, 0);
        w = Win(hwnd);
        if (!w) return;
    }
    State& s = S();
    KillWindowTimers(hwnd);
    for (UserThread* t : s.threads) {
        if (t->focus == hwnd) t->focus = nullptr;
        if (t->active == hwnd) t->active = nullptr;
        if (t->capture == hwnd) t->capture = nullptr;
        // posted messages for a dead window are dropped
        t->posted.erase(std::remove_if(t->posted.begin(), t->posted.end(), [&](const QueuedMsg& m) { return m.msg.hwnd == hwnd; }),
                        t->posted.end());
    }
    if (s.foreground == hwnd) s.foreground = nullptr;
    if (w->own_dc) {
        if (DcObj* dc = DcGet(w->own_dc)) dc->own_dc = false;
        FreeDcObj(w->own_dc);
        w->own_dc = nullptr;
    }
    // owned windows lose their owner
    for (WindowObj* o : s.windows)
        if (o && o->owner == hwnd) o->owner = nullptr;
    ReleaseSurface(w);
    if (w->driver_created)
        if (winexbox::IWinexboxDriver* d = Driver()) d->WindowDestroy(reinterpret_cast<winexbox::WindowId>(hwnd));
    Unlink(w);
    if (w->cls && w->cls->windows) --w->cls->windows;
    BumpStat(&Stats::windows_destroyed);
    TRACE(user, "window %p freed", hwnd);
    FreeWindowSlot(w);
}

}  // namespace

BOOL DestroyWindowInternal(HWND hwnd) {
    WindowObj* w = WinChecked(hwnd);
    if (!w) return FALSE;
    hwnd = w->handle;
    if (w->flags & (kWinIsDesktop | kWinIsMsgParent)) {
        SetLastErr(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (w->tid != GetCurrentThreadId()) {
        FIXME(user, "DestroyWindow(%p) of another thread's window", hwnd);
        SetLastErr(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (w->flags & kWinDestroying) return TRUE;
    w->flags |= kWinDestroying;
    TRACE(user, "%p", hwnd);
    const bool child = (w->style & WS_CHILD) != 0;
    if (child && !(w->ex_style & WS_EX_NOPARENTNOTIFY) && Win(w->parent))
        SendMessageInternal(w->parent, WM_PARENTNOTIFY, MAKEWPARAM(WM_DESTROY, static_cast<WORD>(w->id)),
                            reinterpret_cast<LPARAM>(hwnd));
    if ((w = Win(hwnd)) && (w->style & WS_VISIBLE)) {
        if (child) ShowWindowInternal(hwnd, SW_HIDE);
        else SetWindowPosInternal(hwnd, nullptr, 0, 0, 0, 0,
                                  SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE);
    }
    if (!Win(hwnd)) return TRUE;
    if (!child) {
        // owned windows go first
        for (;;) {
            HWND owned = nullptr;
            for (WindowObj* o : S().windows)
                if (o && o->owner == hwnd && !(o->flags & kWinDestroying) && o->tid == GetCurrentThreadId()) { owned = o->handle; break; }
            if (!owned) break;
            DestroyWindowInternal(owned);
        }
        UserThread& t = CurrentUserThread();
        if (t.active == hwnd) SetActiveWindowInternal(ActivateOtherWindow(hwnd), false);
    }
    if (!Win(hwnd)) return TRUE;
    SendDestroyMessage(hwnd);
    if (!Win(hwnd)) return TRUE;
    FreeWindow(hwnd, true);
    NoteScreenChanged();
    return TRUE;
}

void DestroyThreadWindows(UserThread* t) {
    // A thread that exits takes its windows with it, without running any of its code.
    for (;;) {
        HWND victim = nullptr;
        for (WindowObj* o : S().windows)
            if (o && o->tid == t->tid && !(o->flags & (kWinIsDesktop | kWinIsMsgParent))) { victim = o->handle; break; }
        if (!victim) break;
        // free from the top-level down
        HWND top = TopLevelOf(victim);
        FreeWindow(top ? top : victim, false);
    }
    NoteScreenChanged();
}

}  // namespace detail

// ---- Syscalls -----------------------------------------------------------------------------------
namespace sys {
using namespace detail;

namespace {

void FixCsCoordinates(CREATESTRUCTW* cs, int* sw) {
    // CW_USEDEFAULT for overlapped windows (win32u fix_cs_coordinates)
    if (cs->style & (WS_CHILD | WS_POPUP)) {
        if (cs->dwExStyle & WS_EX_MDICHILD) return;
        if (cs->x == CW_USEDEFAULT || cs->x == static_cast<int>(0x8000)) cs->x = cs->y = 0;
        if (cs->cx == CW_USEDEFAULT || cs->cx == static_cast<int>(0x8000)) cs->cx = cs->cy = 0;
        return;
    }
    RECT work;
    WorkAreaRect(&work);
    if (cs->x == CW_USEDEFAULT) {
        if (cs->y != CW_USEDEFAULT && cs->y >= 0 && cs->y <= SW_MAX) *sw = cs->y;   // y is the show command
        State& s = S();
        int n = 0;
        if (WindowObj* desk = Win(s.desktop)) n = static_cast<int>(desk->children.size() % 8);
        cs->x = work.left + 26 * n;
        cs->y = work.top + 26 * n;
    }
    if (cs->cx == CW_USEDEFAULT || cs->cx == static_cast<int>(0x80000000)) {
        cs->cx = (work.right - work.left) * 3 / 4 - cs->x;
        cs->cy = (work.bottom - work.top) * 3 / 4 - cs->y;
        if (cs->cx < 200) cs->cx = 200;
        if (cs->cy < 150) cs->cy = 150;
    }
}

}  // namespace

HWND WINAPI NtUserCreateWindowEx(DWORD ex_style, UNICODE_STRING* class_name, UNICODE_STRING* version,
                                 UNICODE_STRING* window_name, DWORD style, INT x, INT y, INT cx, INT cy, HWND parent,
                                 HMENU menu, HINSTANCE class_instance, void* params, DWORD flags, HINSTANCE instance,
                                 const WCHAR* class_ptr, BOOL ansi) {
    LockGuard lock;
    (void)version; (void)flags;
    State& s = S();
    UserThread& t = CurrentUserThread();
    if (!Driver()) {
        ERR(user, "CreateWindowEx: no display driver");
        SetLastErr(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    HWND desktop = DesktopWindow();
    if (t.info) {
        t.info->top_window = static_cast<UINT>(reinterpret_cast<ULONG_PTR>(desktop));
        if (!t.info->dpi_context) t.info->dpi_context = 0;
    }
    if (!class_name || !class_name->Buffer) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    ClassObj* cls = FindClass(class_instance, class_name->Buffer, class_name->Length / sizeof(WCHAR));
    if (!cls) {
        SetLastErr(ERROR_CANNOT_FIND_WND_CLASS);
        return nullptr;
    }
    CREATESTRUCTW cs{};
    cs.lpCreateParams = params;
    cs.hInstance = instance ? instance : class_instance;
    cs.hMenu = menu;
    cs.hwndParent = parent;
    cs.style = static_cast<LONG>(style);
    cs.dwExStyle = ex_style;
    cs.lpszName = window_name ? window_name->Buffer : nullptr;
    cs.x = x; cs.y = y; cs.cx = cx; cs.cy = cy;
    HWND owner = nullptr;
    if (parent == HWND_MESSAGE) {
        cs.hwndParent = parent = MessageParentWindow();
    } else if (parent) {
        WindowObj* p = WinChecked(parent);
        if (!p) return nullptr;
        parent = p->handle;
        if ((style & (WS_CHILD | WS_POPUP)) != WS_CHILD) {
            owner = TopLevelOf(parent);
            parent = desktop;
        }
    } else {
        if ((style & (WS_CHILD | WS_POPUP)) == WS_CHILD) {
            SetLastErr(ERROR_TLW_WITH_WSCHILD);
            return nullptr;
        }
        parent = desktop;
    }
    int sw = SW_SHOW;
    FixCsCoordinates(&cs, &sw);
    // fix_exstyle
    if ((cs.style & WS_DLGFRAME) || (cs.dwExStyle & WS_EX_DLGMODALFRAME) ||
        (!(cs.dwExStyle & WS_EX_STATICEDGE) && (cs.style & (WS_DLGFRAME | WS_THICKFRAME))))
        cs.dwExStyle |= WS_EX_WINDOWEDGE;
    else
        cs.dwExStyle &= ~WS_EX_WINDOWEDGE;

    WindowObj* w = AllocWindow();
    if (!w) {
        SetLastErr(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    const HWND hwnd = w->handle;
    w->cls = cls;
    ++cls->windows;
    w->parent = parent;
    w->owner = owner;
    w->style = static_cast<DWORD>(cs.style) & ~WS_VISIBLE;
    w->ex_style = cs.dwExStyle & ~WS_EX_LAYERED;
    w->instance = cs.hInstance;
    w->proc = cls->proc;
    w->unicode = cls->proc.w && cls->proc.a ? !ansi : cls->proc.w != nullptr;
    w->extra.assign(static_cast<std::size_t>(cls->win_extra), 0);
    w->tid = t.tid;
    w->thread = &t;
    w->dpi_context = t.info && t.info->dpi_context ? t.info->dpi_context : s.process_dpi_context;
    if ((w->style & (WS_CHILD | WS_POPUP)) != WS_CHILD) {
        w->style |= WS_CLIPSIBLINGS;
        if (!(w->style & WS_POPUP)) w->style |= WS_CAPTION;
    }
    if ((w->style & (WS_DLGFRAME | WS_THICKFRAME)) && !(w->style & (WS_CHILD | WS_POPUP))) w->ex_style |= WS_EX_WINDOWEDGE;
    if (!(w->style & (WS_CHILD | WS_POPUP))) w->flags |= kWinNeedSize;
    w->id = reinterpret_cast<ULONG_PTR>(cs.hMenu);
    w->normal = RECT{cs.x, cs.y, cs.x + cs.cx, cs.y + cs.cy};
    w->window = w->client = w->normal;
    LinkAfter(w, (w->style & WS_CHILD) ? HWND_BOTTOM : HWND_TOP);
    BumpStat(&Stats::windows_created);
    TRACE(user, "%p class %ls style %08lx ex %08lx (%d,%d %dx%d) parent %p owner %p", hwnd, cls->name.c_str(),
          w->style, w->ex_style, cs.x, cs.y, cs.cx, cs.cy, parent, owner);

    // the class name WM_[NC]CREATE sees
    WCHAR class_buf[256];
    if (class_ptr && IS_INTRESOURCE(class_ptr)) cs.lpszClass = class_ptr;
    else {
        const std::size_t n = std::min<std::size_t>(cls->name.size() - cls->name_offset, 255);
        std::memcpy(class_buf, cls->name.data() + cls->name_offset, n * sizeof(WCHAR));
        class_buf[n] = 0;
        cs.lpszClass = class_buf;
    }

    auto fail = [&]() -> HWND {
        if (Win(hwnd)) {
            WindowObj* f = Win(hwnd);
            f->flags |= kWinDestroying;
            DestroyWindowInternal(hwnd);
            if (Win(hwnd)) FreeWindow(hwnd, false);
        }
        return nullptr;
    };

    // WM_GETMINMAXINFO
    int ccx = cs.cx, ccy = cs.cy;
    if ((cs.style & WS_THICKFRAME) || !(cs.style & (WS_POPUP | WS_CHILD))) {
        const MINMAXINFO mm = GetMinMaxInfo(hwnd);
        ccx = std::max<int>(std::min<int>(ccx, mm.ptMaxTrackSize.x), mm.ptMinTrackSize.x);
        ccy = std::max<int>(std::min<int>(ccy, mm.ptMaxTrackSize.y), mm.ptMinTrackSize.y);
    }
    if (ccx < 0) ccx = 0;
    if (ccy < 0) ccy = 0;
    if (!(w = Win(hwnd))) return nullptr;
    w->window = w->client = RECT{cs.x, cs.y, cs.x + ccx, cs.y + ccy};

    // WM_NCCREATE
    if (!SendMessageInternal(hwnd, WM_NCCREATE, 0, reinterpret_cast<LPARAM>(&cs))) {
        WARN(user, "%p: aborted by WM_NCCREATE", hwnd);
        return fail();
    }
    if (!(w = Win(hwnd))) return nullptr;
    // WM_NCCALCSIZE
    {
        RECT wr = w->window, client;
        SendNcCalcSize(hwnd, &wr, &client);
        if (!(w = Win(hwnd))) return nullptr;
        w->client = client;
        LinkAfter(w, (w->style & WS_CHILD) ? HWND_BOTTOM : HWND_TOP);
    }
    // WM_CREATE
    if (SendMessageInternal(hwnd, WM_CREATE, 0, reinterpret_cast<LPARAM>(&cs)) == -1) {
        WARN(user, "%p: aborted by WM_CREATE", hwnd);
        return fail();
    }
    if (!(w = Win(hwnd))) return nullptr;
    if (winexbox::IWinexboxDriver* d = Driver()) {
        if (!d->WindowCreate(reinterpret_cast<winexbox::WindowId>(hwnd))) return fail();
        w->driver_created = true;
        d->WindowSetParent(reinterpret_cast<winexbox::WindowId>(hwnd), reinterpret_cast<winexbox::WindowId>(w->parent), 0);
        d->WindowSetStyle(reinterpret_cast<winexbox::WindowId>(hwnd), GWL_STYLE, w->style, 0);
        d->WindowSetStyle(reinterpret_cast<winexbox::WindowId>(hwnd), GWL_EXSTYLE, w->ex_style, 0);
        if (!w->text.empty()) SetWindowTextInternal(w, w->text.c_str());
    }
    if (!(w->flags & kWinNeedSize)) {
        const RECT c = w->client;
        SendMessageInternal(hwnd, WM_SIZE, SIZE_RESTORED, MAKELONG(c.right - c.left, c.bottom - c.top));
        if (!Win(hwnd)) return nullptr;
        SendMessageInternal(hwnd, WM_MOVE, 0, MAKELONG(c.left, c.top));
        if (!(w = Win(hwnd))) return nullptr;
    }
    if (w->style & (WS_MINIMIZE | WS_MAXIMIZE)) {
        const bool minimize = (w->style & WS_MINIMIZE) != 0;
        w->style &= ~(WS_MINIMIZE | WS_MAXIMIZE);
        if (cs.style & WS_VISIBLE) sw = minimize ? SW_SHOWMINIMIZED : SW_SHOWMAXIMIZED;
        else FIXME(user, "%p: hidden minimized/maximized window created restored", hwnd);
    }
    // WM_PARENTNOTIFY
    if ((w->style & WS_CHILD) && !(w->ex_style & WS_EX_NOPARENTNOTIFY) && Win(w->parent))
        SendMessageInternal(w->parent, WM_PARENTNOTIFY, MAKEWPARAM(WM_CREATE, static_cast<WORD>(w->id)),
                            reinterpret_cast<LPARAM>(hwnd));
    if (!Win(hwnd)) return nullptr;
    if (cs.style & WS_VISIBLE) ShowWindowInternal(hwnd, sw);
    if (!Win(hwnd)) return nullptr;
    TRACE(user, "created %p", hwnd);
    return hwnd;
}

BOOL WINAPI NtUserDestroyWindow(HWND hwnd) {
    LockGuard lock;
    return DestroyWindowInternal(hwnd);
}

BOOL WINAPI NtUserShowWindow(HWND hwnd, INT cmd) {
    LockGuard lock;
    return ShowWindowInternal(hwnd, cmd);
}

BOOL WINAPI NtUserShowWindowAsync(HWND hwnd, INT cmd) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w) return FALSE;
    if (w->tid != GetCurrentThreadId()) FIXME(user, "ShowWindowAsync on another thread's window runs synchronously");
    return ShowWindowInternal(hwnd, cmd);
}

BOOL WINAPI NtUserSetWindowPos(HWND hwnd, HWND after, INT x, INT y, INT cx, INT cy, UINT flags) {
    LockGuard lock;
    return SetWindowPosInternal(hwnd, after, x, y, cx, cy, flags);
}

BOOL WINAPI NtUserMoveWindow(HWND hwnd, INT x, INT y, INT cx, INT cy, BOOL repaint) {
    LockGuard lock;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (!repaint) flags |= SWP_NOREDRAW;
    return SetWindowPosInternal(hwnd, nullptr, x, y, cx, cy, flags);
}

namespace {

LONG_PTR GetWindowLongSize(HWND hwnd, INT offset, UINT size, bool ansi) {
    WindowObj* w = WinChecked(hwnd);
    if (!w) return 0;
    if (offset >= 0) {
        if (static_cast<std::size_t>(offset) + size > w->extra.size()) {
            SetLastErr(ERROR_INVALID_INDEX);
            return 0;
        }
        LONG_PTR v = 0;
        std::memcpy(&v, w->extra.data() + offset, size);
        if (size == sizeof(LONG)) v = static_cast<LONG>(v);
        return v;
    }
    switch (offset) {
    case GWL_STYLE: return static_cast<LONG>(w->style);
    case GWL_EXSTYLE: return static_cast<LONG>(w->ex_style);
    case GWLP_USERDATA: return size == sizeof(LONG) ? static_cast<LONG>(w->userdata) : w->userdata;
    case GWLP_ID: return static_cast<LONG_PTR>(w->id);
    case GWLP_HINSTANCE: return reinterpret_cast<LONG_PTR>(w->instance);
    case GWLP_HWNDPARENT:
        if (w->style & WS_CHILD) return reinterpret_cast<LONG_PTR>(w->parent);
        return reinterpret_cast<LONG_PTR>(w->owner);
    case GWLP_WNDPROC: {
        if (size != sizeof(LONG_PTR)) {
            SetLastErr(ERROR_ACCESS_DENIED);
            return 0;
        }
        WNDPROC p = ansi ? (w->proc.a ? w->proc.a : w->proc.w) : (w->proc.w ? w->proc.w : w->proc.a);
        return reinterpret_cast<LONG_PTR>(p);
    }
    }
    SetLastErr(ERROR_INVALID_INDEX);
    return 0;
}

LONG_PTR SetWindowLongSize(HWND hwnd, INT offset, UINT size, LONG_PTR newval, bool ansi) {
    WindowObj* w = WinChecked(hwnd);
    if (!w) return 0;
    hwnd = w->handle;
    if (w->flags & kWinIsDesktop) {
        SetLastErr(ERROR_ACCESS_DENIED);
        return 0;
    }
    LONG_PTR old = 0;
    if (offset >= 0) {
        if (static_cast<std::size_t>(offset) + size > w->extra.size()) {
            SetLastErr(ERROR_INVALID_INDEX);
            return 0;
        }
        std::memcpy(&old, w->extra.data() + offset, size);
        if (size == sizeof(LONG)) old = static_cast<LONG>(old);
        std::memcpy(w->extra.data() + offset, &newval, size);
        return old;
    }
    switch (offset) {
    case GWL_STYLE:
    case GWL_EXSTYLE: {
        STYLESTRUCT ss{offset == GWL_STYLE ? w->style : w->ex_style, static_cast<DWORD>(newval)};
        SendMessageInternal(hwnd, WM_STYLECHANGING, static_cast<WPARAM>(offset), reinterpret_cast<LPARAM>(&ss));
        if (!(w = Win(hwnd))) return 0;
        old = static_cast<LONG>(ss.styleOld);
        if (offset == GWL_STYLE) {
            // WS_VISIBLE through SetWindowLong does not repaint, but does change visibility
            w->style = ss.styleNew;
            if ((ss.styleOld ^ ss.styleNew) & WS_VISIBLE) {
                if (IsTopLevel(w)) EnsureSurface(w);
                NoteScreenChanged();
                if (ss.styleNew & WS_VISIBLE) InvalidateWindow(hwnd, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
            }
        } else {
            w->ex_style = ss.styleNew;
        }
        if (w->driver_created)
            if (winexbox::IWinexboxDriver* d = Driver())
                d->WindowSetStyle(reinterpret_cast<winexbox::WindowId>(hwnd), offset, ss.styleNew, ss.styleOld);
        SendMessageInternal(hwnd, WM_STYLECHANGED, static_cast<WPARAM>(offset), reinterpret_cast<LPARAM>(&ss));
        return old;
    }
    case GWLP_USERDATA: old = w->userdata; w->userdata = size == sizeof(LONG) ? static_cast<LONG>(newval) : newval; return old;
    case GWLP_ID: old = static_cast<LONG_PTR>(w->id); w->id = static_cast<ULONG_PTR>(newval); return old;
    case GWLP_HINSTANCE: old = reinterpret_cast<LONG_PTR>(w->instance); w->instance = reinterpret_cast<HINSTANCE>(newval); return old;
    case GWLP_HWNDPARENT:
        if (w->style & WS_CHILD) {
            FIXME(user, "SetWindowLong(GWLP_HWNDPARENT) on a child: use SetParent");
            return reinterpret_cast<LONG_PTR>(w->parent);
        }
        old = reinterpret_cast<LONG_PTR>(w->owner);
        w->owner = FullHandle(reinterpret_cast<HWND>(newval));
        return old;
    case GWLP_WNDPROC: {
        if (size != sizeof(LONG_PTR)) {
            SetLastErr(ERROR_ACCESS_DENIED);
            return 0;
        }
        WNDPROC p = ansi ? (w->proc.a ? w->proc.a : w->proc.w) : (w->proc.w ? w->proc.w : w->proc.a);
        old = reinterpret_cast<LONG_PTR>(p);
        w->proc = ResolveWinProc(reinterpret_cast<WNDPROC>(newval), ansi);
        w->unicode = w->proc.w && w->proc.a ? w->unicode : w->proc.w != nullptr;
        return old;
    }
    }
    SetLastErr(ERROR_INVALID_INDEX);
    return 0;
}

}  // namespace

LONG WINAPI NtUserSetWindowLong(HWND hwnd, INT offset, LONG newval, BOOL ansi) {
    LockGuard lock;
    return static_cast<LONG>(SetWindowLongSize(hwnd, offset, sizeof(LONG), newval, ansi));
}

LONG_PTR WINAPI NtUserSetWindowLongPtr(HWND hwnd, INT offset, LONG_PTR newval, BOOL ansi) {
    LockGuard lock;
    return SetWindowLongSize(hwnd, offset, sizeof(LONG_PTR), newval, ansi);
}

WORD WINAPI NtUserSetWindowWord(HWND hwnd, INT offset, WORD newval) {
    LockGuard lock;
    if (offset < 0 && offset != GWLP_ID && offset != GWLP_HINSTANCE && offset != GWLP_HWNDPARENT) {
        SetLastErr(ERROR_INVALID_INDEX);
        return 0;
    }
    return static_cast<WORD>(SetWindowLongSize(hwnd, offset, sizeof(WORD), newval, TRUE));
}

ULONG_PTR WINAPI NtUserCallHwnd(HWND hwnd, DWORD code) {
    LockGuard lock;
    switch (code) {
    case abi::HwndGetFullWindowHandle: {
        WindowObj* w = Win(hwnd);
        return w ? HandleToUlong(w->handle) : HandleToUlong(hwnd);
    }
    case abi::HwndIsCurrentProcessWindow: {
        WindowObj* w = Win(hwnd);
        return w ? HandleToUlong(w->handle) : 0;
    }
    case abi::HwndIsCurrentThreadWindow: {
        WindowObj* w = Win(hwnd);
        return w && w->tid == GetCurrentThreadId() ? HandleToUlong(w->handle) : 0;
    }
    case abi::HwndIsWindow: return Win(hwnd) != nullptr;
    }
    WindowObj* w = WinChecked(hwnd);
    if (!w) return 0;
    switch (code) {
    case abi::HwndActivateOtherWindow: SetActiveWindowInternal(ActivateOtherWindow(w->handle), false); return 0;
    case abi::HwndGetDialogInfo: return static_cast<ULONG_PTR>(w->dlg_info);
    case abi::HwndGetDpiForWindow: return 96;
    case abi::HwndGetLastActivePopup: return HandleToUlong(w->last_active_popup ? w->last_active_popup : w->handle);
    case abi::HwndGetMDIClientInfo: return static_cast<ULONG_PTR>(w->mdi_info);
    case abi::HwndGetParent:
        if (w->style & WS_CHILD) return HandleToUlong(w->parent);
        if (w->style & WS_POPUP) return HandleToUlong(w->owner);
        return 0;
    case abi::HwndGetWindowDpiAwarenessContext: return w->dpi_context;
    case abi::HwndGetWindowInputContext: return 0;
    case abi::HwndGetWindowSysSubMenu: return 0;
    case abi::HwndGetWindowTextLength: return w->text.size();
    case abi::HwndIsWindowEnabled: return !(w->style & WS_DISABLED);
    case abi::HwndIsWindowUnicode: return w->unicode;
    case abi::HwndIsWindowVisible: return IsWindowReallyVisible(w);
    case abi::HwndSetForegroundWindowInternal:
        if (!IsTopLevel(w)) return FALSE;
        return SetActiveWindowInternal(w->handle, false);
    }
    FIXME(user, "NtUserCallHwnd code %lu", code);
    return 0;
}

ULONG_PTR WINAPI NtUserCallHwndParam(HWND hwnd, DWORD_PTR param, DWORD code) {
    LockGuard lock;
    switch (code) {
    case abi::HwndParamGetClassLongA: return static_cast<DWORD>(GetClassLongSize(hwnd, static_cast<INT>(param), sizeof(DWORD), true));
    case abi::HwndParamGetClassLongW: return static_cast<DWORD>(GetClassLongSize(hwnd, static_cast<INT>(param), sizeof(DWORD), false));
    case abi::HwndParamGetClassLongPtrA: return GetClassLongSize(hwnd, static_cast<INT>(param), sizeof(ULONG_PTR), true);
    case abi::HwndParamGetClassLongPtrW: return GetClassLongSize(hwnd, static_cast<INT>(param), sizeof(ULONG_PTR), false);
    case abi::HwndParamGetClassWord: return static_cast<WORD>(GetClassLongSize(hwnd, static_cast<INT>(param), sizeof(WORD), true));
    case abi::HwndParamGetWindowLongA: return static_cast<ULONG_PTR>(static_cast<LONG>(GetWindowLongSize(hwnd, static_cast<INT>(param), sizeof(LONG), true)));
    case abi::HwndParamGetWindowLongW: return static_cast<ULONG_PTR>(static_cast<LONG>(GetWindowLongSize(hwnd, static_cast<INT>(param), sizeof(LONG), false)));
    case abi::HwndParamGetWindowLongPtrA: return static_cast<ULONG_PTR>(GetWindowLongSize(hwnd, static_cast<INT>(param), sizeof(LONG_PTR), true));
    case abi::HwndParamGetWindowLongPtrW: return static_cast<ULONG_PTR>(GetWindowLongSize(hwnd, static_cast<INT>(param), sizeof(LONG_PTR), false));
    case abi::HwndParamGetWindowWord: return static_cast<WORD>(GetWindowLongSize(hwnd, static_cast<INT>(param), sizeof(WORD), true));
    case abi::HwndParamMapWindowPoints: {
        auto* p = reinterpret_cast<abi::MapWindowPointsParams*>(param);
        WindowObj* from = hwnd ? WinChecked(hwnd) : nullptr;
        WindowObj* to = p->hwnd_to ? WinChecked(p->hwnd_to) : nullptr;
        if ((hwnd && !from) || (p->hwnd_to && !to)) return 0;
        const POINT a = from ? ClientOriginScreen(from) : POINT{0, 0};
        const POINT b = to ? ClientOriginScreen(to) : POINT{0, 0};
        const LONG dx = a.x - b.x, dy = a.y - b.y;
        for (UINT i = 0; i < p->count; ++i) { p->points[i].x += dx; p->points[i].y += dy; }
        return static_cast<ULONG_PTR>(static_cast<DWORD>(MAKELONG(static_cast<WORD>(dx), static_cast<WORD>(dy))));
    }
    }
    WindowObj* w = WinChecked(hwnd);
    if (!w) return 0;
    switch (code) {
    case abi::HwndParamClientToScreen: {
        auto* pt = reinterpret_cast<POINT*>(param);
        if (!pt) return FALSE;
        const POINT o = ClientOriginScreen(w);
        pt->x += o.x; pt->y += o.y;
        return TRUE;
    }
    case abi::HwndParamScreenToClient: {
        auto* pt = reinterpret_cast<POINT*>(param);
        if (!pt) return FALSE;
        const POINT o = ClientOriginScreen(w);
        pt->x -= o.x; pt->y -= o.y;
        return TRUE;
    }
    case abi::HwndParamGetChildRect: {
        auto* r = reinterpret_cast<RECT*>(param);
        if (!r) return FALSE;
        *r = w->window;
        return TRUE;
    }
    case abi::HwndParamGetWindowRect: {
        auto* p = reinterpret_cast<abi::GetWindowRectsParams*>(param);
        if (!p || !p->rect) return FALSE;
        *p->rect = WindowRectScreen(w);
        return TRUE;
    }
    case abi::HwndParamGetClientRect: {
        auto* p = reinterpret_cast<abi::GetWindowRectsParams*>(param);
        if (!p || !p->rect) return FALSE;
        *p->rect = RECT{0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top};
        return TRUE;
    }
    case abi::HwndParamGetPresentRect: return FALSE;
    case abi::HwndParamGetWindowInfo: {
        auto* info = reinterpret_cast<WINDOWINFO*>(param);
        if (!info) return FALSE;
        info->rcWindow = WindowRectScreen(w);
        info->rcClient = ClientRectScreen(w);
        info->dwStyle = w->style;
        info->dwExStyle = w->ex_style;
        UserThread* t = FindUserThreadById(w->tid);
        info->dwWindowStatus = t && t->active == w->handle ? WS_ACTIVECAPTION : 0;
        info->cxWindowBorders = static_cast<UINT>(info->rcClient.left - info->rcWindow.left);
        info->cyWindowBorders = static_cast<UINT>(info->rcWindow.bottom - info->rcClient.bottom);
        info->atomWindowType = w->cls->atom;
        info->wCreatorVersion = 0x0601;
        return TRUE;
    }
    case abi::HwndParamGetWindowRelative: {
        const UINT rel = static_cast<UINT>(param);
        WindowObj* p = Win(w->parent);
        auto& sibs = p ? p->children : w->children;
        switch (rel) {
        case GW_HWNDFIRST: return p && !sibs.empty() ? HandleToUlong(sibs.front()) : 0;
        case GW_HWNDLAST: return p && !sibs.empty() ? HandleToUlong(sibs.back()) : 0;
        case GW_HWNDNEXT:
        case GW_HWNDPREV: {
            if (!p) return 0;
            auto it = std::find(sibs.begin(), sibs.end(), w->handle);
            if (it == sibs.end()) return 0;
            if (rel == GW_HWNDNEXT) return (it + 1) != sibs.end() ? HandleToUlong(*(it + 1)) : 0;
            return it != sibs.begin() ? HandleToUlong(*(it - 1)) : 0;
        }
        case GW_OWNER: return HandleToUlong(w->owner);
        case GW_CHILD: return w->children.empty() ? 0 : HandleToUlong(w->children.front());
        case GW_ENABLEDPOPUP: return 0;
        }
        SetLastErr(ERROR_INVALID_PARAMETER);
        return 0;
    }
    case abi::HwndParamGetWindowThread: {
        if (param) *reinterpret_cast<DWORD*>(param) = GetCurrentProcessId();
        return w->tid;
    }
    case abi::HwndParamIsChild: {
        const HWND child = static_cast<HWND>(UlongToHandle(static_cast<ULONG>(param)));
        for (WindowObj* c = Win(child); c && (c->style & WS_CHILD); c = Win(c->parent))
            if (c->parent == w->handle) return TRUE;
        return FALSE;
    }
    case abi::HwndParamMonitorFromWindow: return HandleToUlong(abi::PrimaryMonitor());
    case abi::HwndParamSetDialogInfo: w->dlg_info = static_cast<LONG_PTR>(param); return 0;
    case abi::HwndParamSetMDIClientInfo: w->mdi_info = static_cast<LONG_PTR>(param); return 0;
    case abi::HwndParamGetWinMonitorDpi: return 96;
    case abi::HwndParamGetScrollInfo: return FALSE;
    case abi::HwndParamExposeWindowSurface: return FALSE;
    case abi::HwndParamGetPrivateData: {
        auto* p = reinterpret_cast<abi::GetPrivateDataParams*>(param);
        if (!p || static_cast<std::size_t>(p->offset) + p->size > w->extra.size() || p->size > 8) return 0;
        LONG_PTR v = 0;
        std::memcpy(&v, w->extra.data() + p->offset, p->size);
        return static_cast<ULONG_PTR>(v);
    }
    case abi::HwndParamSetPrivateData: {
        auto* p = reinterpret_cast<abi::SetPrivateDataParams*>(param);
        if (!p || static_cast<std::size_t>(p->offset) + p->size > w->extra.size() || p->size > 8) return 0;
        LONG_PTR old = 0;
        std::memcpy(&old, w->extra.data() + p->offset, p->size);
        std::memcpy(w->extra.data() + p->offset, &p->value, p->size);
        return static_cast<ULONG_PTR>(old);
    }
    }
    FIXME(user, "NtUserCallHwndParam code %lu", code);
    return 0;
}

HWND WINAPI NtUserGetAncestor(HWND hwnd, UINT type) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w || (w->flags & kWinIsDesktop)) return nullptr;
    switch (type) {
    case GA_PARENT: return w->parent;
    case GA_ROOT: return TopLevelOf(w->handle);
    case GA_ROOTOWNER: {
        HWND h = TopLevelOf(w->handle);
        for (WindowObj* o = Win(h); o && o->owner; o = Win(o->owner)) h = o->owner;
        return h;
    }
    }
    return nullptr;
}

HWND WINAPI NtUserSetParent(HWND hwnd, HWND parent) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w || (w->flags & kWinIsDesktop)) return nullptr;
    hwnd = w->handle;
    if (!parent) parent = DesktopWindow();
    else if (parent == HWND_MESSAGE) parent = MessageParentWindow();
    WindowObj* p = WinChecked(parent);
    if (!p) return nullptr;
    parent = p->handle;
    if (parent == hwnd || IsDescendant(hwnd, parent)) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const HWND old = w->parent;
    const bool was_visible = (w->style & WS_VISIBLE) != 0;
    if (was_visible) ShowWindowInternal(hwnd, SW_HIDE);
    if (!(w = Win(hwnd))) return nullptr;
    Unlink(w);
    // keep the position relative to the old parent's client area, now relative to the new one
    w->parent = parent;
    ReleaseSurface(w);
    LinkAfter(w, HWND_TOP);
    if (w->driver_created)
        if (winexbox::IWinexboxDriver* d = Driver())
            d->WindowSetParent(reinterpret_cast<winexbox::WindowId>(hwnd), reinterpret_cast<winexbox::WindowId>(parent),
                               reinterpret_cast<winexbox::WindowId>(old));
    SetWindowPosInternal(hwnd, HWND_TOP, w->window.left, w->window.top, 0, 0,
                         SWP_NOSIZE | (was_visible ? SWP_SHOWWINDOW : 0));
    return old;
}

HWND WINAPI NtUserFindWindowEx(HWND parent, HWND child, UNICODE_STRING* cls, UNICODE_STRING* title, ULONG unk) {
    LockGuard lock;
    (void)unk;
    if (cls && !cls->Length && !IS_INTRESOURCE(cls->Buffer)) return nullptr;
    WindowObj* p = parent ? WinChecked(parent) : Win(DesktopWindow());
    if (!p) return nullptr;
    ATOM atom = 0;
    if (cls && IS_INTRESOURCE(cls->Buffer)) atom = static_cast<ATOM>(reinterpret_cast<ULONG_PTR>(cls->Buffer));
    bool started = !child;
    for (HWND h : std::vector<HWND>(p->children)) {
        if (!started) {
            if (h == FullHandle(child)) started = true;
            continue;
        }
        WindowObj* w = Win(h);
        if (!w) continue;
        if (cls) {
            if (atom) { if (w->cls->atom != atom) continue; }
            else {
                const std::size_t len = cls->Length / sizeof(WCHAR);
                if (w->cls->name.size() != len || _wcsnicmp(w->cls->name.c_str(), cls->Buffer, len)) continue;
            }
        }
        if (title) {
            const std::size_t len = title->Length / sizeof(WCHAR);
            if (w->text.size() != len || (len && std::wmemcmp(w->text.c_str(), title->Buffer, len))) continue;
        }
        return h;
    }
    return nullptr;
}

NTSTATUS WINAPI NtUserBuildHwndList(HDESK desktop, HWND hwnd, BOOL children, BOOL non_immersive, ULONG thread_id,
                                    ULONG count, HWND* buffer, ULONG* size) {
    LockGuard lock;
    (void)desktop; (void)non_immersive;
    WindowObj* root = hwnd ? Win(hwnd) : Win(DesktopWindow());
    if (!root) return static_cast<NTSTATUS>(0xC0000008);   // STATUS_INVALID_HANDLE
    std::vector<HWND> list;
    std::function<void(WindowObj*)> walk = [&](WindowObj* w) {
        for (HWND h : w->children) {
            WindowObj* c = Win(h);
            if (!c) continue;
            if (!thread_id || c->tid == thread_id) list.push_back(h);
            if (children) walk(c);
        }
    };
    walk(root);
    *size = static_cast<ULONG>(list.size() + 1);
    if (count < *size) return static_cast<NTSTATUS>(0xC0000023);   // STATUS_BUFFER_TOO_SMALL
    for (std::size_t i = 0; i < list.size(); ++i) buffer[i] = list[i];
    buffer[list.size()] = HWND_BOTTOM;
    return dispatch::detail::st::Success;
}

HANDLE WINAPI NtUserQueryWindow(HWND hwnd, int cls) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w) return nullptr;
    UserThread* t = FindUserThreadById(w->tid);
    switch (cls) {
    case abi::QwWindowProcess:
    case abi::QwWindowProcess2: return UlongToHandle(GetCurrentProcessId());
    case abi::QwWindowThread: return UlongToHandle(w->tid);
    case abi::QwWindowActiveWindow: return t ? t->active : nullptr;
    case abi::QwWindowFocusWindow: return t ? t->focus : nullptr;
    case abi::QwWindowIsHung: return nullptr;
    case abi::QwWindowClientBase: return nullptr;
    case abi::QwWindowIsForegroundThread: return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(t && t->active == S().foreground && t->active));
    case abi::QwWindowDefaultImeWindow: return nullptr;
    case abi::QwWindowDefaultInputContext: return nullptr;
    }
    return nullptr;
}

HWND WINAPI NtUserWindowFromPoint(LONG x, LONG y) {
    LockGuard lock;
    return WindowFromScreenPoint(POINT{x, y});
}

HWND WINAPI NtUserChildWindowFromPointEx(HWND parent, LONG x, LONG y, UINT flags) {
    LockGuard lock;
    WindowObj* p = WinChecked(parent);
    if (!p) return nullptr;
    const RECT client{0, 0, p->client.right - p->client.left, p->client.bottom - p->client.top};
    if (!PtInRectangle(client, x, y)) return nullptr;
    for (HWND h : p->children) {
        WindowObj* c = Win(h);
        if (!c) continue;
        if ((flags & CWP_SKIPINVISIBLE) && !(c->style & WS_VISIBLE)) continue;
        if ((flags & CWP_SKIPDISABLED) && (c->style & WS_DISABLED)) continue;
        if ((flags & CWP_SKIPTRANSPARENT) && (c->ex_style & WS_EX_TRANSPARENT)) continue;
        if (PtInRectangle(c->window, x, y)) return h;
    }
    return p->handle;
}

HWND WINAPI NtUserRealChildWindowFromPoint(HWND parent, LONG x, LONG y) {
    return NtUserChildWindowFromPointEx(parent, x, y, CWP_SKIPTRANSPARENT | CWP_SKIPINVISIBLE);
}

HWND WINAPI NtUserSetFocus(HWND hwnd) {
    LockGuard lock;
    return SetFocusInternal(hwnd);
}

HWND WINAPI NtUserSetActiveWindow(HWND hwnd) {
    LockGuard lock;
    UserThread& t = CurrentUserThread();
    HWND prev = t.active;
    if (hwnd) {
        WindowObj* w = WinChecked(hwnd);
        if (!w) return nullptr;
        if (w->tid != t.tid || (w->style & (WS_POPUP | WS_CHILD)) == WS_CHILD) return nullptr;
    }
    if (!SetActiveWindowInternal(hwnd, false)) return nullptr;
    return prev;
}

HWND WINAPI NtUserGetForegroundWindow() {
    LockGuard lock;
    State& s = S();
    return Win(s.foreground) ? s.foreground : nullptr;
}

BOOL WINAPI NtUserSetForegroundWindow(HWND hwnd) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w || !IsTopLevel(w)) return FALSE;
    if (w->tid != GetCurrentThreadId()) {
        S().foreground = w->handle;
        return TRUE;
    }
    return SetActiveWindowInternal(w->handle, false);
}

HWND WINAPI NtUserSetCapture(HWND hwnd) {
    LockGuard lock;
    UserThread& t = CurrentUserThread();
    HWND prev = t.capture;
    if (hwnd && !WinChecked(hwnd)) return nullptr;
    t.capture = FullHandle(hwnd);
    if (winexbox::IWinexboxDriver* d = Driver()) d->WindowSetCapture(reinterpret_cast<winexbox::WindowId>(t.capture), 0);
    if (prev && prev != t.capture && Win(prev))
        SendMessageInternal(prev, WM_CAPTURECHANGED, 0, reinterpret_cast<LPARAM>(t.capture));
    return prev;
}

BOOL WINAPI NtUserReleaseCapture() {
    LockGuard lock;
    UserThread& t = CurrentUserThread();
    HWND prev = t.capture;
    t.capture = nullptr;
    if (winexbox::IWinexboxDriver* d = Driver()) d->WindowSetCapture(0, 0);
    if (prev && Win(prev)) SendMessageInternal(prev, WM_CAPTURECHANGED, 0, 0);
    return TRUE;
}

BOOL WINAPI NtUserEnableWindow(HWND hwnd, BOOL enable) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w) return FALSE;
    hwnd = w->handle;
    const BOOL was_disabled = (w->style & WS_DISABLED) != 0;
    if (enable && was_disabled) {
        w->style &= ~WS_DISABLED;
        SendMessageInternal(hwnd, WM_ENABLE, TRUE, 0);
    } else if (!enable && !was_disabled) {
        SendMessageInternal(hwnd, WM_CANCELMODE, 0, 0);
        if ((w = Win(hwnd))) {
            w->style |= WS_DISABLED;
            UserThread& t = CurrentUserThread();
            if (t.focus == hwnd || (t.focus && IsDescendant(hwnd, t.focus))) SetFocusInternal(nullptr);
            SendMessageInternal(hwnd, WM_ENABLE, FALSE, 0);
        }
    }
    return was_disabled;
}

INT WINAPI NtUserInternalGetWindowText(HWND hwnd, WCHAR* text, INT count) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w || !text || count <= 0) return 0;
    const INT n = std::min<INT>(count - 1, static_cast<INT>(w->text.size()));
    std::memcpy(text, w->text.data(), static_cast<std::size_t>(n) * sizeof(WCHAR));
    text[n] = 0;
    return n;
}

namespace {
std::wstring PropKey(const WCHAR* str) {
    if (IS_INTRESOURCE(str)) return L"#" + std::to_wstring(reinterpret_cast<ULONG_PTR>(str));
    std::wstring k(str);
    for (auto& c : k) c = static_cast<wchar_t>(std::towlower(c));
    return k;
}
}  // namespace

BOOL WINAPI NtUserSetProp(HWND hwnd, const WCHAR* str, HANDLE handle) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w || !str) return FALSE;
    const std::wstring key = PropKey(str);
    for (auto& p : w->props)
        if (p.first == key) { p.second = handle; return TRUE; }
    w->props.emplace_back(key, handle);
    return TRUE;
}

HANDLE WINAPI NtUserGetProp(HWND hwnd, const WCHAR* str) {
    LockGuard lock;
    WindowObj* w = Win(hwnd);
    if (!w || !str) return nullptr;
    const std::wstring key = PropKey(str);
    for (auto& p : w->props)
        if (p.first == key) return p.second;
    return nullptr;
}

HANDLE WINAPI NtUserRemoveProp(HWND hwnd, const WCHAR* str) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w || !str) return nullptr;
    const std::wstring key = PropKey(str);
    for (auto it = w->props.begin(); it != w->props.end(); ++it)
        if (it->first == key) {
            HANDLE h = it->second;
            w->props.erase(it);
            return h;
        }
    return nullptr;
}

BOOL WINAPI NtUserGetWindowPlacement(HWND hwnd, WINDOWPLACEMENT* placement) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w || !placement) return FALSE;
    placement->length = sizeof(WINDOWPLACEMENT);
    placement->flags = 0;
    placement->showCmd = (w->style & WS_MINIMIZE) ? SW_SHOWMINIMIZED : (w->style & WS_MAXIMIZE) ? SW_SHOWMAXIMIZED
                                                                                                  : SW_SHOWNORMAL;
    placement->ptMinPosition = POINT{-1, -1};
    placement->ptMaxPosition = POINT{-1, -1};
    placement->rcNormalPosition = (w->style & (WS_MINIMIZE | WS_MAXIMIZE)) ? w->normal : w->window;
    return TRUE;
}

BOOL WINAPI NtUserSetWindowPlacement(HWND hwnd, const WINDOWPLACEMENT* wpl) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w || !wpl) return FALSE;
    hwnd = w->handle;
    w->normal = wpl->rcNormalPosition;
    if (!(w->style & (WS_MINIMIZE | WS_MAXIMIZE))) {
        const RECT& r = wpl->rcNormalPosition;
        SetWindowPosInternal(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindowInternal(hwnd, static_cast<int>(wpl->showCmd));
    return TRUE;
}

BOOL WINAPI NtUserSetWindowFNID(HWND hwnd, WORD fnid) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w) return FALSE;
    w->fnid = fnid;
    return TRUE;
}

BOOL WINAPI NtUserGetGUIThreadInfo(DWORD id, GUITHREADINFO* info) {
    LockGuard lock;
    if (!info || info->cbSize != sizeof(GUITHREADINFO)) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    UserThread* t = id ? FindUserThreadById(id) : nullptr;
    if (!id) {
        State& s = S();
        if (WindowObj* fg = Win(s.foreground)) t = FindUserThreadById(fg->tid);
    }
    std::memset(reinterpret_cast<std::uint8_t*>(info) + sizeof(DWORD), 0, sizeof(*info) - sizeof(DWORD));
    if (!t) return TRUE;
    info->hwndActive = t->active;
    info->hwndFocus = t->focus;
    info->hwndCapture = t->capture;
    return TRUE;
}

BOOL WINAPI NtUserFlashWindowEx(FLASHWINFO* info) {
    LockGuard lock;
    if (!info || !WinChecked(info->hwnd)) return FALSE;
    FIXME(user, "FlashWindowEx(%p): nothing to flash on the headless display", info->hwnd);
    return FALSE;
}

BOOL WINAPI NtUserIsChildWindowDpiMessageEnabled(HWND hwnd) {
    (void)hwnd;
    return FALSE;
}

BOOL WINAPI NtUserGetTitleBarInfo(HWND hwnd, TITLEBARINFO* info) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w || !info || info->cbSize != sizeof(TITLEBARINFO)) return FALSE;
    const RECT wr = WindowRectScreen(w);
    const RECT cr = ClientRectScreen(w);
    info->rcTitleBar = RECT{cr.left, wr.top + (cr.left - wr.left), cr.right, cr.top};
    for (DWORD& s : info->rgstate) s = 0;
    if ((w->style & WS_CAPTION) != WS_CAPTION) info->rgstate[0] = STATE_SYSTEM_INVISIBLE;
    return TRUE;
}

BOOL WINAPI NtUserSetMenu(HWND hwnd, HMENU menu) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w || (w->style & (WS_CHILD | WS_POPUP)) == WS_CHILD) return FALSE;
    if (!MenuAttachToWindow(w, menu)) return FALSE;
    // the client area shrinks by the menu bar (win32u NtUserSetMenu)
    SetWindowPosInternal(w->handle, nullptr, 0, 0, 0, 0,
                         SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
    return TRUE;
}

HMENU WINAPI NtUserGetSystemMenu(HWND hwnd, BOOL revert) {
    (void)hwnd; (void)revert;
    NoteUnimplemented("NtUserGetSystemMenu (no menu support yet)");
    return nullptr;
}

DWORD WINAPI NtUserGetWindowContextHelpId(HWND hwnd) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w) return 0;
    for (auto& p : w->props)
        if (p.first == L"#context-help-id") return static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(p.second));
    return 0;
}

BOOL WINAPI NtUserSetWindowContextHelpId(HWND hwnd, DWORD id) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w) return FALSE;
    for (auto& p : w->props)
        if (p.first == L"#context-help-id") { p.second = UlongToHandle(id); return TRUE; }
    w->props.emplace_back(L"#context-help-id", UlongToHandle(id));
    return TRUE;
}

void WINAPI NtUserNotifyWinEvent(DWORD event, HWND hwnd, LONG object_id, LONG child_id) {
    // No WinEvent hooks can be installed (NtUserSetWinEventHook is Todo), so there is nobody to notify.
    (void)event; (void)hwnd; (void)object_id; (void)child_id;
}

BOOL WINAPI NtUserShowOwnedPopups(HWND owner, BOOL show) {
    LockGuard lock;
    std::vector<HWND> popups;
    for (WindowObj* w : S().windows)
        if (w && w->owner == FullHandle(owner) && (w->style & WS_POPUP)) popups.push_back(w->handle);
    for (HWND h : popups) {
        WindowObj* w = Win(h);
        if (!w) continue;
        if (show && !(w->style & WS_VISIBLE)) ShowWindowInternal(h, SW_SHOWNOACTIVATE);
        else if (!show && (w->style & WS_VISIBLE)) ShowWindowInternal(h, SW_HIDE);
    }
    return TRUE;
}

HICON WINAPI NtUserInternalGetWindowIcon(HWND hwnd, UINT type) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w) return nullptr;
    HICON i = type == ICON_SMALL ? (w->icon_sm ? w->icon_sm : w->cls->icon_sm) : (w->icon ? w->icon : w->cls->icon);
    return i ? i : (w->icon ? w->icon : w->cls->icon);
}

// ---- NtUserCall{No,One,Two}Param --------------------------------------------------------------

ULONG_PTR WINAPI NtUserCallNoParam(ULONG code) {
    LockGuard lock;
    switch (code) {
    case abi::NoParamGetDesktopWindow: {
        HWND d = DesktopWindow();
        UserThread& t = CurrentUserThread();
        if (t.info) t.info->top_window = HandleToUlong(d);
        return HandleToUlong(d);
    }
    case abi::NoParamGetDialogBaseUnits: return MAKELONG(8, 16);
    case abi::NoParamGetLastInputTime: return S().last_input_time;
    case abi::NoParamGetProgmanWindow:
    case abi::NoParamGetShellWindow:
    case abi::NoParamGetTaskmanWindow: return 0;
    case abi::NoParamDisplayModeChanged: return FALSE;
    case abi::NoParamExitingThread: {
        if (UserThread* t = FindUserThread()) {
            t->exiting = true;
            DestroyThreadWindows(t);
        }
        return 0;
    }
    case abi::NoParamThreadDetach: {
        if (UserThread* t = FindUserThread()) {
            DestroyThreadWindows(t);
            KillWindowTimers(nullptr);
            t->timers.clear();
            t->posted.clear();
        }
        return 0;
    }
    }
    FIXME(user, "NtUserCallNoParam code %lu", code);
    return 0;
}

ULONG_PTR WINAPI NtUserCallOneParam(ULONG_PTR arg, ULONG code) {
    LockGuard lock;
    switch (code) {
    case abi::OneParamGetSysColor: return SysColor(static_cast<int>(arg));
    case abi::OneParamGetSysColorBrush:
        if (static_cast<int>(arg) == 0x100 /* COLOR_55AA_BRUSH */) return HandleToUlong(S().stock[GRAY_BRUSH]);
        return HandleToUlong(SysColorBrush(static_cast<int>(arg)));
    case abi::OneParamGetSysColorPen: return HandleToUlong(SysColorPen(static_cast<int>(arg)));
    case abi::OneParamGetSystemMetrics: return static_cast<ULONG_PTR>(static_cast<LONG_PTR>(SystemMetric(static_cast<int>(arg))));
    case abi::OneParamSetThreadDpiAwarenessContext: {
        UserThread& t = CurrentUserThread();
        if (!t.info) return 0;
        UINT prev = t.info->dpi_context;
        if (!prev) prev = S().process_dpi_context | abi::kDpiFlagProcess;
        const UINT ctx = static_cast<UINT>(arg);
        t.info->dpi_context = (ctx & abi::kDpiFlagProcess) ? 0 : ctx;
        return prev;
    }
    case abi::OneParamGetPrimaryMonitorRect: {
        auto* r = reinterpret_cast<RECT*>(arg);
        if (r) ScreenRect(r);
        return TRUE;
    }
    case abi::OneParamGetVirtualScreenRect: {
        auto* r = reinterpret_cast<RECT*>(arg);
        if (r) ScreenRect(r);
        return TRUE;
    }
    case abi::OneParamGetAsyncKeyboardState: {
        auto* st = reinterpret_cast<BYTE*>(arg);
        if (!st) return FALSE;
        std::memcpy(st, S().key_state, 256);
        return TRUE;
    }
    case abi::OneParamGetMenuItemCount: return MenuItemCount(arg);
    case abi::OneParamEnableThunkLock:
        return 0;   // the thunk lock exists for 16-bit code, which never runs here
    case abi::OneParamSetKeyboardAutoRepeat:
        return 0;
    }
    FIXME(user, "NtUserCallOneParam code %lu (arg %#llx)", code, static_cast<unsigned long long>(arg));
    SetLastErr(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

ULONG_PTR WINAPI NtUserCallTwoParam(ULONG_PTR arg1, ULONG_PTR arg2, ULONG code) {
    LockGuard lock;
    switch (code) {
    case abi::TwoParamGetMonitorInfo: {
        auto* info = reinterpret_cast<MONITORINFO*>(arg2);
        if (UlongToHandle(static_cast<ULONG>(arg1)) != abi::PrimaryMonitor() || !info ||
            (info->cbSize != sizeof(MONITORINFO) && info->cbSize != sizeof(MONITORINFOEXW))) {
            SetLastErr(ERROR_INVALID_MONITOR_HANDLE);
            return FALSE;
        }
        ScreenRect(&info->rcMonitor);
        WorkAreaRect(&info->rcWork);
        info->dwFlags = MONITORINFOF_PRIMARY;
        if (info->cbSize == sizeof(MONITORINFOEXW)) wcscpy(reinterpret_cast<MONITORINFOEXW*>(info)->szDevice, L"\\\\.\\DISPLAY1");
        return TRUE;
    }
    case abi::TwoParamGetSystemMetricsForDpi:
        return static_cast<ULONG_PTR>(static_cast<LONG_PTR>(SystemMetric(static_cast<int>(arg1))));
    case abi::TwoParamMonitorFromRect: {
        auto* r = reinterpret_cast<const RECT*>(arg1);
        RECT screen;
        ScreenRect(&screen);
        if (r && RectEmpty(RectIntersect(*r, screen)) && (arg2 & 3) == MONITOR_DEFAULTTONULL) return 0;
        return HandleToUlong(abi::PrimaryMonitor());
    }
    case abi::TwoParamAdjustWindowRect: {
        auto* r = reinterpret_cast<RECT*>(arg1);
        auto* p = reinterpret_cast<const abi::AdjustWindowRectParams*>(arg2);
        if (!r || !p) return FALSE;
        AdjustRectForStyle(r, p->style, p->menu != 0, p->ex_style);
        return TRUE;
    }
    case abi::TwoParamGetVirtualScreenRect: {
        auto* r = reinterpret_cast<RECT*>(arg1);
        if (r) ScreenRect(r);
        return TRUE;
    }
    case abi::TwoParamGetDialogProc:
    case abi::TwoParamAllocWinProc:
        return arg1;   // raw procedure pointers throughout (no WINPROC handles)
    case abi::TwoParamGetMenuInfo:
        return MenuGetInfo(arg1, reinterpret_cast<MENUINFO*>(arg2));
    }
    FIXME(user, "NtUserCallTwoParam code %lu", code);
    SetLastErr(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

}  // namespace sys
}  // namespace win10xbox::user
