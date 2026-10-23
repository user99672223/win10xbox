// core/user menus: menu objects (HMENU handles), the NtUserThunkedMenuItemInfo family, the menu bar of a top-level
// window (reserved in WM_NCCALCSIZE, painted by WM_NCPAINT, opened by WM_NCLBUTTONDOWN on HTMENU), popup menus
// (NtUserTrackPopupMenuEx) and the `#32768` popup window. docs/GUI.md "Menus" lists what is and is not done.
//
// The contract is Wine's (include/ntuser.h, dlls/user32/menu.c, dlls/win32u/menu.c); the implementation is our own:
// a simple table of menu objects, a fixed layout (one row bar, one column popups, drawn with the built-in 8x16 face)
// and one modal loop that pumps this thread's queue with the mouse captured.
#include "user_internal.h"
#include "user_syscalls.h"
#include "font8x16.h"

#include <algorithm>
#include <cstring>
#include <string>

#ifndef MN_GETHMENU
#define MN_GETHMENU 0x01E1
#endif

namespace win10xbox::user {
namespace detail {
namespace {

// ---- Menu objects ---------------------------------------------------------------------------------
constexpr UINT kTypeMask = MFT_STRING | MFT_BITMAP | MFT_OWNERDRAW | MFT_SEPARATOR | MFT_MENUBARBREAK | MFT_MENUBREAK |
                           MFT_RADIOCHECK | MFT_RIGHTORDER | MFT_RIGHTJUSTIFY;
constexpr UINT kStateMask = MFS_GRAYED | MFS_CHECKED | MFS_HILITE | MFS_DEFAULT;

// ThunkedMenuItemInfo `method` (ntuser.h, NtUserThunkedMenuItemInfo codes)
enum : UINT {
    kSetMenuItemInfo, kInsertMenuItem, kCheckMenuRadioItem, kGetMenuDefaultItem, kGetMenuItemID, kGetMenuItemInfoA,
    kGetMenuItemInfoW, kGetMenuState, kGetSubMenu,
};

// Fixed Windows 10 style metrics (96 DPI).
constexpr int kBarPadding = 8;         // each side of a bar item's text
constexpr int kPopupCheckColumn = 24;  // left of the popup text
constexpr int kPopupRightPad = 24;     // right of the popup text (submenu arrow)
constexpr int kPopupItemHeight = 22;
constexpr int kPopupSepHeight = 9;
constexpr int kPopupTop = 3;           // border + padding above the first item and below the last
constexpr int kPopupAccelGap = 24;

struct MenuItem {
    UINT type = 0;     // fType (MFT_*) | MF_POPUP
    UINT state = 0;    // fState (MFS_*)
    UINT id = 0;
    HMENU sub = nullptr;
    std::wstring text;
    bool has_text = false;   // a null string (separator) differs from an empty one
    HBITMAP bmp = nullptr, checked = nullptr, unchecked = nullptr;
    ULONG_PTR data = 0;
    RECT rect{};       // menu coordinates, set by Layout*
};

struct MenuObj {
    HMENU handle = nullptr;
    bool popup = false;          // MF_POPUP: created by CreatePopupMenu or used as a submenu
    std::vector<MenuItem> items;
    DWORD style = 0, help_id = 0;
    UINT cy_max = 0;
    HBRUSH back = nullptr;
    ULONG_PTR menu_data = 0;
    HWND owner = nullptr;        // the window this menu is the bar of
    HWND wnd = nullptr;          // the `#32768` window showing it
    int hilite = -1;             // item shown highlighted while tracking
    int width = 0, height = 0;   // laid out size (popup: client size; bar: items extent)
};

struct MenuTable {
    std::vector<MenuObj*> slots;
    std::vector<WORD> gens;
    std::vector<std::uint32_t> free_slots;
};
MenuTable* g_menus = nullptr;   // allocated under the lock on first use (no function-local statics)

// HMENU = (0x8000 + slot) | (generation << 16): never a window handle (their low word is 0x20 + slot), fits 32 bits.
constexpr std::uint32_t kMenuBase = 0x8000;
constexpr std::uint32_t kMaxMenus = 0x7000;

MenuTable& Table() {
    if (!g_menus) g_menus = new MenuTable;
    return *g_menus;
}

HMENU EncodeMenu(std::uint32_t slot, WORD gen) {
    return reinterpret_cast<HMENU>(static_cast<ULONG_PTR>((kMenuBase + slot) | (static_cast<DWORD>(gen) << 16)));
}

MenuObj* MenuOf(ULONG_PTR handle) {
    if (!g_menus || (handle >> 32) != 0) return nullptr;
    const DWORD h = static_cast<DWORD>(handle);
    const DWORD low = LOWORD(h);
    if (low < kMenuBase) return nullptr;
    const std::uint32_t slot = low - kMenuBase;
    MenuTable& t = *g_menus;
    if (slot >= t.slots.size() || !t.slots[slot] || t.gens[slot] != HIWORD(h)) return nullptr;
    return t.slots[slot];
}
MenuObj* MenuOf(HMENU handle) { return MenuOf(reinterpret_cast<ULONG_PTR>(handle)); }

HMENU CreateMenuObj(bool popup) {
    MenuTable& t = Table();
    std::uint32_t slot;
    if (!t.free_slots.empty()) {
        slot = t.free_slots.back();
        t.free_slots.pop_back();
    } else {
        if (t.slots.size() >= kMaxMenus) {
            SetLastErr(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        slot = static_cast<std::uint32_t>(t.slots.size());
        t.slots.push_back(nullptr);
        t.gens.push_back(0);
    }
    WORD gen = static_cast<WORD>(t.gens[slot] + 1);
    if (gen == 0 || gen > 0x7fff) gen = 1;
    t.gens[slot] = gen;
    auto* m = new MenuObj;
    m->handle = EncodeMenu(slot, gen);
    m->popup = popup;
    t.slots[slot] = m;
    return m->handle;
}

void DestroyMenuObj(HMENU handle) {
    MenuObj* m = MenuOf(handle);
    if (!m) return;
    MenuTable& t = *g_menus;
    const std::uint32_t slot = LOWORD(reinterpret_cast<ULONG_PTR>(handle)) - kMenuBase;
    t.slots[slot] = nullptr;
    t.free_slots.push_back(slot);
    std::vector<MenuItem> items = std::move(m->items);
    const HWND wnd = m->wnd;
    delete m;
    if (wnd && Win(wnd)) DestroyWindowInternal(wnd);
    for (const MenuItem& it : items)
        if (it.type & MF_POPUP) DestroyMenuObj(it.sub);   // recursively, like Windows
}

// find_menu_item: by position, or by command id (searching the submenus of popup items first).
bool FindItem(HMENU handle, UINT id, UINT flags, MenuObj** out, UINT* pos, bool set_error = true) {
    MenuObj* m = MenuOf(handle);
    if (!m) {
        if (set_error) SetLastErr(ERROR_INVALID_MENU_HANDLE);
        return false;
    }
    if (flags & MF_BYPOSITION) {
        if (id >= m->items.size()) {
            if (set_error) SetLastErr(ERROR_MENU_ITEM_NOT_FOUND);
            return false;
        }
        *out = m;
        *pos = id;
        return true;
    }
    UINT fallback = ~0u;
    for (UINT i = 0; i < m->items.size(); ++i) {
        const MenuItem& it = m->items[i];
        if (it.type & MF_POPUP) {
            MenuObj* sm = nullptr;
            UINT sp = 0;
            if (MenuOf(it.sub) && FindItem(it.sub, id, flags, &sm, &sp, false)) {
                *out = sm;
                *pos = sp;
                return true;
            }
            if (it.id == id) fallback = i;
        } else if (it.id == id) {
            *out = m;
            *pos = i;
            return true;
        }
    }
    if (fallback != ~0u) {
        *out = m;
        *pos = fallback;
        return true;
    }
    if (set_error) SetLastErr(ERROR_MENU_ITEM_NOT_FOUND);
    return false;
}

bool Disabled(const MenuItem& it) { return (it.state & (MFS_GRAYED)) != 0; }
bool Separator(const MenuItem& it) { return (it.type & MFT_SEPARATOR) != 0; }
bool Selectable(const MenuItem& it) { return !Separator(it) && !Disabled(it); }

// get_menu_default_item: `bypos` selects position or id; GMDI_* in `flags`.
UINT DefaultItem(HMENU handle, UINT bypos, UINT flags) {
    MenuObj* m = MenuOf(handle);
    if (!m) return static_cast<UINT>(-1);
    int found = -1;
    for (UINT k = 0; k < m->items.size(); ++k)
        if (m->items[k].state & MFS_DEFAULT) { found = static_cast<int>(k); break; }
    if (found >= 0 && !(flags & GMDI_USEDISABLED) && Disabled(m->items[found])) found = -1;
    if (found >= 0 && (m->items[found].type & MF_POPUP) && (flags & GMDI_GOINTOPOPUPS)) {
        const UINT r = DefaultItem(m->items[found].sub, bypos, flags);
        if (r != static_cast<UINT>(-1)) return r;   // otherwise the popup item itself
    }
    if (found < 0) return static_cast<UINT>(-1);
    return bypos ? static_cast<UINT>(found) : m->items[found].id;
}

// ---- Text ---------------------------------------------------------------------------------------------
// The label of an item without the '&' mnemonic markers; `accel` gets the text after a tab.
std::wstring Label(const MenuItem& it, std::wstring* accel) {
    std::wstring out, tail;
    bool after_tab = false;
    for (std::size_t i = 0; i < it.text.size(); ++i) {
        const WCHAR c = it.text[i];
        if (c == L'\t') { after_tab = true; continue; }
        if (c == L'&') {
            if (i + 1 < it.text.size() && it.text[i + 1] == L'&') { ++i; }
            else continue;
        }
        (after_tab ? tail : out) += c;
    }
    if (accel) *accel = tail;
    return out;
}
int TextWidth(const std::wstring& s) { return static_cast<int>(s.size()) * font8x16::kCellWidth; }

void DrawString(const DrawTarget& t, int x, int y, const std::wstring& s, std::uint32_t px) {
    for (WCHAR ch : s) {
        const std::uint8_t* rows = font8x16::GlyphRows(ch);
        for (int r = 0; r < font8x16::kCellHeight; ++r) {
            int c = 0;
            while (c < font8x16::kCellWidth) {
                if (!font8x16::GlyphPixel(rows, c, r)) { ++c; continue; }
                int e = c;
                while (e < font8x16::kCellWidth && font8x16::GlyphPixel(rows, e, r)) ++e;
                FillTargetRect(t, RECT{x + c, y + r, x + e, y + r + 1}, px);
                c = e;
            }
        }
        x += font8x16::kCellWidth;
    }
}

std::uint32_t Px(int color_index) { return ColorToPixel(SysColor(color_index)); }

// ---- Layout -------------------------------------------------------------------------------------------
int BarItemHeight() { return SystemMetric(SM_CYMENU) - 1; }

void LayoutBar(MenuObj& m) {
    int x = 0;
    for (MenuItem& it : m.items) {
        const int w = Separator(it) ? kBarPadding : TextWidth(Label(it, nullptr)) + 2 * kBarPadding;
        it.rect = RECT{x, 0, x + w, BarItemHeight()};
        x += w;
    }
    m.width = x;
    m.height = SystemMetric(SM_CYMENU);
}

void LayoutPopup(MenuObj& m) {
    int label = 0, accel = 0;
    for (const MenuItem& it : m.items) {
        std::wstring a;
        const std::wstring l = Label(it, &a);
        label = std::max(label, TextWidth(l));
        accel = std::max(accel, TextWidth(a));
    }
    const int w = kPopupCheckColumn + label + (accel ? kPopupAccelGap + accel : 0) + kPopupRightPad;
    int y = kPopupTop;
    for (MenuItem& it : m.items) {
        const int h = Separator(it) ? kPopupSepHeight : kPopupItemHeight;
        it.rect = RECT{1, y, w - 1, y + h};
        y += h;
    }
    m.width = w;
    m.height = y + kPopupTop;
}

// ---- The bar of a window -----------------------------------------------------------------------------
bool WindowCanHaveBar(const WindowObj* w) {
    if (!w || (w->flags & (kWinIsDesktop | kWinIsMsgParent))) return false;
    return (w->style & (WS_CHILD | WS_POPUP)) != WS_CHILD;
}

MenuObj* BarMenu(const WindowObj* w) {
    if (!WindowCanHaveBar(w)) return nullptr;
    MenuObj* m = MenuOf(static_cast<ULONG_PTR>(w->id));
    if (m) m->owner = w->handle;
    return m;
}

// The bar rectangle in window coordinates (the area between the caption and the client area).
RECT BarRectInWindow(const WindowObj* w) {
    const int h = SystemMetric(SM_CYMENU);
    const int left = w->client.left - w->window.left, top = w->client.top - w->window.top;
    return RECT{left, top - h, w->client.right - w->window.left, top};
}

POINT BarOriginScreen(const WindowObj* w) {
    const RECT r = BarRectInWindow(w);
    return POINT{w->window.left + r.left, w->window.top + r.top};   // top-level windows: parent origin is the screen
}

// ---- Drawing ---------------------------------------------------------------------------------------------
void FillRectPx(const DrawTarget& t, RECT r, std::uint32_t px) { FillTargetRect(t, r, px); }

void DrawItemText(const DrawTarget& t, const MenuItem& it, RECT r, int text_x, bool hot) {
    std::uint32_t fg = Px(COLOR_MENUTEXT);
    if (hot) fg = Px(COLOR_HIGHLIGHTTEXT);
    else if (Disabled(it)) fg = Px(COLOR_GRAYTEXT);
    std::wstring accel;
    const std::wstring label = Label(it, &accel);
    const int y = r.top + (r.bottom - r.top - font8x16::kCellHeight) / 2;
    DrawString(t, text_x, y, label, fg);
    if (!accel.empty()) DrawString(t, r.right - kPopupRightPad - TextWidth(accel) + 8, y, accel, fg);
}

void PaintBarInto(const WindowObj* w, MenuObj& m, const DrawTarget& t) {
    LayoutBar(m);
    const RECT bar = BarRectInWindow(w);
    FillRectPx(t, bar, Px(COLOR_MENU));
    for (std::size_t i = 0; i < m.items.size(); ++i) {
        const MenuItem& it = m.items[i];
        if (Separator(it)) continue;
        const RECT r = RectOffset(it.rect, bar.left, bar.top);
        const bool hot = static_cast<int>(i) == m.hilite;
        if (hot) FillRectPx(t, r, Px(COLOR_HIGHLIGHT));
        DrawItemText(t, it, r, r.left + kBarPadding, hot);
    }
}

// A 7x6 check mark and a 4x4 bullet, centred in the check column.
void DrawCheck(const DrawTarget& t, int x, int y, bool radio, std::uint32_t px) {
    if (radio) {
        FillRectPx(t, RECT{x + 1, y + 1, x + 5, y + 5}, px);
        return;
    }
    const char* rows[6] = {"......X", ".....XX", "X...XX.", "XX.XX..", ".XXX...", "..X...."};
    for (int r = 0; r < 6; ++r)
        for (int c = 0; c < 7; ++c)
            if (rows[r][c] == 'X') FillRectPx(t, RECT{x + c, y + r, x + c + 1, y + r + 1}, px);
}

void PaintPopupInto(MenuObj& m, const DrawTarget& t) {
    LayoutPopup(m);
    const RECT all{0, 0, m.width, m.height};
    FillRectPx(t, all, Px(COLOR_MENU));
    const std::uint32_t border = Px(COLOR_3DSHADOW);
    FillRectPx(t, RECT{0, 0, m.width, 1}, border);
    FillRectPx(t, RECT{0, m.height - 1, m.width, m.height}, border);
    FillRectPx(t, RECT{0, 0, 1, m.height}, border);
    FillRectPx(t, RECT{m.width - 1, 0, m.width, m.height}, border);
    for (std::size_t i = 0; i < m.items.size(); ++i) {
        const MenuItem& it = m.items[i];
        const RECT r = it.rect;
        if (Separator(it)) {
            FillRectPx(t, RECT{r.left + 24, r.top + 4, r.right - 4, r.top + 5}, Px(COLOR_BTNSHADOW));
            continue;
        }
        const bool hot = static_cast<int>(i) == m.hilite && Selectable(it);
        if (hot) FillRectPx(t, r, Px(COLOR_HIGHLIGHT));
        DrawItemText(t, it, r, r.left + kPopupCheckColumn, hot);
        const std::uint32_t fg = hot ? Px(COLOR_HIGHLIGHTTEXT) : Disabled(it) ? Px(COLOR_GRAYTEXT) : Px(COLOR_MENUTEXT);
        if (it.state & MFS_CHECKED) DrawCheck(t, r.left + 8, r.top + 8, (it.type & MFT_RADIOCHECK) != 0, fg);
        if (it.type & MF_POPUP) {   // submenu arrow
            const int ax = r.right - 12, ay = r.top + 7;
            for (int k = 0; k < 4; ++k) FillRectPx(t, RECT{ax + k, ay + k, ax + k + 1, ay + 8 - k}, fg);
        }
    }
}

// ---- Tracking state ------------------------------------------------------------------------------------
struct Track {
    HWND owner = nullptr;
    HMENU root = nullptr;
    bool bar = false;
    UINT flags = 0;
    int bar_item = -1;           // bar: the item whose popup is open
    HMENU popup = nullptr;       // the menu shown in popup_wnd
    HWND popup_wnd = nullptr;
    POINT popup_org{};           // screen position of the popup's client origin
    bool end = false;
    bool executed = false;
    UINT cmd = 0;
    bool entered = false;
};
Track* g_track = nullptr;        // the innermost active menu loop of the process

bool Notify(const Track& tk) { return !(tk.flags & TPM_NONOTIFY); }

void RepaintBar(HWND hwnd) {
    WindowObj* w = Win(hwnd);
    if (!w || !BarMenu(w) || !IsWindowReallyVisible(w)) return;
    HDC hdc = GetDCInternal(hwnd, nullptr, DCX_WINDOW | DCX_CACHE);
    DcObj* dc = DcGet(hdc);
    DrawTarget t;
    if (dc && ResolveDrawTarget(dc, &t)) {
        PaintBarInto(w, *BarMenu(w), t);
        NoteDrawn(dc, t, BarRectInWindow(w));
    }
    ReleaseDCInternal(hwnd, hdc);
}

void RepaintPopup(const Track& tk) {
    MenuObj* m = MenuOf(tk.popup);
    if (!m || !Win(tk.popup_wnd)) return;
    HDC hdc = GetDCInternal(tk.popup_wnd, nullptr, DCX_CACHE);
    DcObj* dc = DcGet(hdc);
    DrawTarget t;
    if (dc && ResolveDrawTarget(dc, &t)) {
        PaintPopupInto(*m, t);
        NoteDrawn(dc, t, RECT{0, 0, m->width, m->height});
    }
    ReleaseDCInternal(tk.popup_wnd, hdc);
}

int BarItemAt(const Track& tk, POINT pt) {
    WindowObj* w = Win(tk.owner);
    MenuObj* m = tk.bar && w ? BarMenu(w) : nullptr;
    if (!m) return -1;
    LayoutBar(*m);
    const POINT o = BarOriginScreen(w);
    const int lx = pt.x - o.x, ly = pt.y - o.y;
    if (ly < 0 || ly >= SystemMetric(SM_CYMENU)) return -1;
    for (std::size_t i = 0; i < m->items.size(); ++i)
        if (!Separator(m->items[i]) && PtInRectangle(m->items[i].rect, lx, std::min(ly, BarItemHeight() - 1))) return static_cast<int>(i);
    return -1;
}

int PopupItemAt(const Track& tk, POINT pt) {
    MenuObj* m = MenuOf(tk.popup);
    if (!m || !Win(tk.popup_wnd)) return -1;
    const int lx = pt.x - tk.popup_org.x, ly = pt.y - tk.popup_org.y;
    for (std::size_t i = 0; i < m->items.size(); ++i)
        if (PtInRectangle(m->items[i].rect, lx, ly)) return static_cast<int>(i);
    return -1;
}

bool InsidePopup(const Track& tk, POINT pt) {
    MenuObj* m = MenuOf(tk.popup);
    if (!m || !Win(tk.popup_wnd)) return false;
    return PtInRectangle(RECT{0, 0, m->width, m->height}, pt.x - tk.popup_org.x, pt.y - tk.popup_org.y);
}

// WM_MENUSELECT for the item `pos` of `menu` (lock held).
void SendMenuSelect(const Track& tk, MenuObj& menu, int pos) {
    if (!Notify(tk) || pos < 0 || pos >= static_cast<int>(menu.items.size())) return;
    const MenuItem& it = menu.items[pos];
    const HMENU h = menu.handle;
    UINT flags = (it.state & kStateMask) | (it.type & (MF_POPUP | MF_SEPARATOR | MFT_OWNERDRAW)) | MF_MOUSESELECT | MF_HILITE;
    const UINT low = (it.type & MF_POPUP) ? static_cast<UINT>(pos) : it.id;
    SendMessageInternal(tk.owner, WM_MENUSELECT, MAKEWPARAM(low, flags), reinterpret_cast<LPARAM>(h));
}

POINT PlacePopup(const MenuObj& m, int x, int y, UINT flags) {
    RECT screen;
    ScreenRect(&screen);
    if (flags & TPM_CENTERALIGN) x -= m.width / 2;
    else if (flags & TPM_RIGHTALIGN) x -= m.width;
    if (flags & TPM_VCENTERALIGN) y -= m.height / 2;
    else if (flags & TPM_BOTTOMALIGN) y -= m.height;
    if (x + m.width > screen.right) x = screen.right - m.width;
    if (y + m.height > screen.bottom) y = screen.bottom - m.height;
    if (x < screen.left) x = screen.left;
    if (y < screen.top) y = screen.top;
    return POINT{x, y};
}

// ---- Popup windows ---------------------------------------------------------------------------------------
// Closes the popup window of `tk` (lock NOT held).
void ClosePopup(Track& tk) {
    HWND wnd;
    {
        LockGuard g;
        wnd = tk.popup_wnd;
        if (MenuObj* m = MenuOf(tk.popup)) {
            m->wnd = nullptr;
            m->hilite = -1;
        }
        tk.popup = nullptr;
        tk.popup_wnd = nullptr;
    }
    if (wnd) sys::NtUserDestroyWindow(wnd);
}

// Shows `hsub` as the popup of `tk` at screen (x, y) (lock NOT held). parent_pos: the item of the bar / parent menu.
bool OpenPopup(Track& tk, HMENU hsub, int x, int y, int parent_pos, bool place) {
    int w = 0, h = 0;
    {
        LockGuard g;
        MenuObj* m = MenuOf(hsub);
        if (!m) return false;
        if (Notify(tk)) {
            SendMessageInternal(tk.owner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(hsub), MAKELPARAM(parent_pos < 0 ? 0 : parent_pos, 0));
            m = MenuOf(hsub);
            if (!m) return false;
        }
        LayoutPopup(*m);
        if (place) {
            const POINT p = PlacePopup(*m, x, y, tk.flags);
            x = p.x;
            y = p.y;
        }
        m->hilite = -1;
        w = m->width;
        h = m->height;
    }
    WCHAR cls[] = L"#32768";
    UNICODE_STRING name{static_cast<USHORT>(6 * sizeof(WCHAR)), static_cast<USHORT>(7 * sizeof(WCHAR)), cls};
    HWND wnd = sys::NtUserCreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, &name, nullptr, nullptr, WS_POPUP, x, y, w, h,
                                         tk.owner, nullptr, nullptr, nullptr, 0, nullptr, nullptr, FALSE);
    if (!wnd) return false;
    {
        LockGuard g;
        WindowObj* pw = Win(wnd);
        MenuObj* m = MenuOf(hsub);
        if (!pw || !m) return false;
        if (pw->extra.size() >= sizeof(HMENU)) std::memcpy(pw->extra.data(), &hsub, sizeof(HMENU));
        m->wnd = wnd;
        tk.popup = hsub;
        tk.popup_wnd = wnd;
        tk.popup_org = POINT{x, y};
    }
    sys::NtUserShowWindow(wnd, SW_SHOWNOACTIVATE);
    sys::NtUserSetCapture(wnd);
    {
        LockGuard g;
        RepaintPopup(tk);
    }
    return true;
}

void OpenBarItem(Track& tk, int idx) {
    if (idx == tk.bar_item && tk.popup_wnd) return;
    HMENU sub = nullptr;
    POINT at{};
    UINT pos = static_cast<UINT>(idx);
    {
        LockGuard g;
        WindowObj* w = Win(tk.owner);
        MenuObj* m = w ? BarMenu(w) : nullptr;
        if (!m || idx < 0 || idx >= static_cast<int>(m->items.size())) return;
        LayoutBar(*m);
        m->hilite = idx;
        tk.bar_item = idx;
        const MenuItem& it = m->items[idx];
        if (it.type & MF_POPUP) sub = it.sub;
        const POINT o = BarOriginScreen(w);
        at = POINT{o.x + it.rect.left, o.y + SystemMetric(SM_CYMENU)};
        SendMenuSelect(tk, *m, idx);
        RepaintBar(tk.owner);
    }
    if (tk.popup_wnd) ClosePopup(tk);
    if (sub) OpenPopup(tk, sub, at.x, at.y, static_cast<int>(pos), false);
}

void Hilite(Track& tk, int idx) {
    LockGuard g;
    MenuObj* m = MenuOf(tk.popup);
    if (!m || m->hilite == idx) return;
    m->hilite = idx;
    if (idx >= 0) SendMenuSelect(tk, *m, idx);
    RepaintPopup(tk);
}

void Execute(Track& tk, int idx) {
    LockGuard g;
    MenuObj* m = MenuOf(tk.popup);
    if (!m || idx < 0 || idx >= static_cast<int>(m->items.size())) return;
    const MenuItem& it = m->items[idx];
    if (!Selectable(it) || (it.type & MF_POPUP)) return;
    tk.executed = true;
    tk.cmd = it.id;
    tk.end = true;
}

void OnMouse(Track& tk, UINT msg, POINT pt) {
    const bool down = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK;
    const bool up = msg == WM_LBUTTONUP || (msg == WM_RBUTTONUP && (tk.flags & TPM_RIGHTBUTTON));
    bool in_popup, in_bar_item = false;
    int bar_idx, pop_idx;
    {
        LockGuard g;
        in_popup = InsidePopup(tk, pt);
        bar_idx = tk.bar ? BarItemAt(tk, pt) : -1;
        in_bar_item = bar_idx >= 0;
        pop_idx = in_popup ? PopupItemAt(tk, pt) : -1;
    }
    if (msg == WM_MOUSEMOVE) {
        if (in_bar_item && bar_idx != tk.bar_item) OpenBarItem(tk, bar_idx);
        else if (in_popup) {
            bool ok;
            { LockGuard g; MenuObj* m = MenuOf(tk.popup); ok = m && pop_idx >= 0 && Selectable(m->items[pop_idx]); }
            Hilite(tk, ok ? pop_idx : -1);
        } else if (tk.popup_wnd) Hilite(tk, -1);
        return;
    }
    if (down) {
        if (in_bar_item) {
            if (bar_idx == tk.bar_item) tk.end = true;   // a second click on the open item closes the menu
            else OpenBarItem(tk, bar_idx);
        } else if (in_popup) {
            bool ok;
            { LockGuard g; MenuObj* m = MenuOf(tk.popup); ok = m && pop_idx >= 0 && Selectable(m->items[pop_idx]); }
            Hilite(tk, ok ? pop_idx : -1);
        } else {
            tk.end = true;   // a click outside cancels
        }
        return;
    }
    if (up) {
        if (in_popup) {
            if (pop_idx >= 0) Execute(tk, pop_idx);
        } else if (in_bar_item) {
            // the release of the click that opened the menu, or a command item on the bar
            bool command = false;
            UINT id = 0;
            {
                LockGuard g;
                WindowObj* w = Win(tk.owner);
                MenuObj* m = w ? BarMenu(w) : nullptr;
                if (m && bar_idx < static_cast<int>(m->items.size())) {
                    const MenuItem& it = m->items[bar_idx];
                    command = !(it.type & MF_POPUP) && Selectable(it);
                    id = it.id;
                }
            }
            if (command) { tk.executed = true; tk.cmd = id; tk.end = true; }
        } else if (msg == WM_LBUTTONUP && tk.entered) {
            tk.end = true;
        }
    }
}

void OnKey(Track& tk, UINT vk) {
    if (vk == VK_ESCAPE) { tk.end = true; return; }
    int count = 0, cur = -1;
    {
        LockGuard g;
        if (MenuObj* m = MenuOf(tk.popup)) { count = static_cast<int>(m->items.size()); cur = m->hilite; }
    }
    if (vk == VK_DOWN || vk == VK_UP) {
        if (!count) return;
        int idx = cur;
        for (int n = 0; n < count; ++n) {
            idx = vk == VK_DOWN ? (idx + 1) % count : (idx < 0 ? count - 1 : (idx + count - 1) % count);
            bool ok;
            { LockGuard g; MenuObj* m = MenuOf(tk.popup); ok = m && Selectable(m->items[idx]); }
            if (ok) { Hilite(tk, idx); break; }
        }
    } else if (vk == VK_RETURN) {
        if (cur >= 0) Execute(tk, cur);
    } else if ((vk == VK_LEFT || vk == VK_RIGHT) && tk.bar) {
        int n, cb = tk.bar_item;
        {
            LockGuard g;
            WindowObj* w = Win(tk.owner);
            MenuObj* m = w ? BarMenu(w) : nullptr;
            n = m ? static_cast<int>(m->items.size()) : 0;
        }
        if (n > 0) OpenBarItem(tk, vk == VK_RIGHT ? (cb + 1) % n : (cb + n - 1) % n);
    }
}

struct TrackResult { bool executed = false; UINT id = 0; };

// The modal loop (lock NOT held). For a popup root: opens `root` at (x, y); for a bar: opens item `open_item`.
TrackResult RunTrack(HWND owner, HMENU root, bool bar, int open_item, int x, int y, UINT flags) {
    Track tk;
    tk.owner = owner;
    tk.root = root;
    tk.bar = bar;
    tk.flags = flags;
    Track* prev;
    {
        LockGuard g;
        prev = g_track;
        g_track = &tk;
        if (Notify(tk)) {
            SendMessageInternal(owner, WM_ENTERMENULOOP, bar ? FALSE : TRUE, 0);
            SendMessageInternal(owner, WM_INITMENU, reinterpret_cast<WPARAM>(root), 0);
        }
    }
    if (bar) OpenBarItem(tk, open_item);
    else OpenPopup(tk, root, x, y, -1, true);
    // without a popup window (a bar item that is a command) the owner still captures the mouse
    if (!tk.popup_wnd) sys::NtUserSetCapture(owner);
    for (;;) {
        {
            LockGuard g;
            if (tk.end || !Win(owner)) break;
        }
        MSG msg{};
        const BOOL r = sys::NtUserGetMessage(&msg, nullptr, 0, 0);
        if (r == 0) {
            sys::NtUserPostQuitMessage(static_cast<INT>(msg.wParam));
            break;
        }
        if (r < 0) break;
        switch (msg.message) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP:
            if (msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN || msg.message == WM_LBUTTONDBLCLK) tk.entered = true;
            OnMouse(tk, msg.message, msg.pt);
            break;
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            OnKey(tk, static_cast<UINT>(msg.wParam));
            break;
        case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
            break;   // swallowed while the menu is up
        default:
            sys::NtUserDispatchMessage(&msg);
            break;
        }
    }
    sys::NtUserReleaseCapture();
    ClosePopup(tk);
    TrackResult res;
    {
        LockGuard g;
        res.executed = tk.executed;
        res.id = tk.cmd;
        if (WindowObj* w = Win(owner)) {
            if (Notify(tk)) {
                SendMessageInternal(owner, WM_MENUSELECT, MAKEWPARAM(0, 0xFFFF), 0);
                SendMessageInternal(owner, WM_EXITMENULOOP, bar ? FALSE : TRUE, 0);
            }
            if (bar) {
                if (MenuObj* m = BarMenu(w)) m->hilite = -1;
                RepaintBar(owner);
            }
            if (tk.executed && !(flags & TPM_RETURNCMD))
                PostMessageInternal(owner, WM_COMMAND, MAKEWPARAM(tk.cmd, 0), 0);
        }
        g_track = prev;
    }
    return res;
}

}  // namespace

// ---- Call-outs used by the window manager ------------------------------------------------------------
int MenuBarHeight(const WindowObj* w) {
    return BarMenu(w) ? SystemMetric(SM_CYMENU) : 0;
}

void MenuPaintBar(WindowObj* w, DcObj* dc, const DrawTarget& t) {
    MenuObj* m = BarMenu(w);
    if (!m) return;
    PaintBarInto(w, *m, t);
    NoteDrawn(dc, t, BarRectInWindow(w));
}

bool MenuBarHitTest(const WindowObj* w, POINT screen_pt) {
    if (!BarMenu(w)) return false;
    const POINT o = BarOriginScreen(w);
    const RECT r = BarRectInWindow(w);
    return PtInRectangle(RECT{o.x, o.y, o.x + (r.right - r.left), o.y + SystemMetric(SM_CYMENU)}, screen_pt.x, screen_pt.y);
}

void MenuBarClick(HWND hwnd, POINT screen_pt) {
    WindowObj* w = Win(hwnd);
    MenuObj* m = w ? BarMenu(w) : nullptr;
    if (!m) return;
    Track probe;
    probe.owner = w->handle;
    probe.bar = true;
    LayoutBar(*m);
    const int idx = BarItemAt(probe, screen_pt);
    if (idx < 0) return;
    const HMENU root = m->handle;
    UnlockScope unlock;
    RunTrack(hwnd, root, true, idx, 0, 0, 0);
}

void MenuDestroyWindowMenu(WindowObj* w) {
    if (!WindowCanHaveBar(w)) return;
    if (MenuOf(static_cast<ULONG_PTR>(w->id))) {
        DestroyMenuObj(reinterpret_cast<HMENU>(w->id));
        w->id = 0;
    }
}

BOOL MenuAttachToWindow(WindowObj* w, HMENU menu) {
    if (menu && !MenuOf(menu)) {
        SetLastErr(ERROR_INVALID_MENU_HANDLE);
        return FALSE;
    }
    if (!WindowCanHaveBar(w)) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (menu) {
        MenuObj* m = MenuOf(menu);
        m->owner = w->handle;
    }
    w->id = reinterpret_cast<ULONG_PTR>(menu);
    return TRUE;
}

ULONG_PTR MenuItemCount(ULONG_PTR handle) {
    MenuObj* m = MenuOf(handle);
    if (!m) {
        SetLastErr(ERROR_INVALID_MENU_HANDLE);
        return static_cast<ULONG_PTR>(-1);
    }
    return m->items.size();
}

BOOL MenuGetInfo(ULONG_PTR handle, MENUINFO* info) {
    MenuObj* m = MenuOf(handle);
    if (!info || info->cbSize != sizeof(MENUINFO) || !m) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (info->fMask & MIM_BACKGROUND) info->hbrBack = m->back;
    if (info->fMask & MIM_HELPID) info->dwContextHelpID = m->help_id;
    if (info->fMask & MIM_MAXHEIGHT) info->cyMax = m->cy_max;
    if (info->fMask & MIM_MENUDATA) info->dwMenuData = m->menu_data;
    if (info->fMask & MIM_STYLE) info->dwStyle = m->style;
    return TRUE;
}

LRESULT MenuPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool ansi) {
    WindowObj* w = Win(hwnd);
    if (!w) return 0;
    HMENU hmenu = nullptr;
    if (w->extra.size() >= sizeof(HMENU)) std::memcpy(&hmenu, w->extra.data(), sizeof(HMENU));
    switch (msg) {
    case WM_CREATE:
    case WM_DESTROY:
    case WM_SHOWWINDOW:
    case WM_PRINTCLIENT:
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return TRUE;
    case MN_GETHMENU:
        return reinterpret_cast<LRESULT>(hmenu);
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaintInternal(hwnd, &ps);
        DcObj* dc = hdc ? DcGet(hdc) : nullptr;
        MenuObj* m = MenuOf(hmenu);
        DrawTarget t;
        if (dc && m && ResolveDrawTarget(dc, &t)) {
            PaintPopupInto(*m, t);
            NoteDrawn(dc, t, RECT{0, 0, m->width, m->height});
        }
        EndPaintInternal(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcInternal(hwnd, msg, wp, lp, ansi);
}

}  // namespace detail

namespace sys {
using namespace detail;

HMENU WINAPI NtUserCreateMenu() {
    LockGuard lock;
    return CreateMenuObj(false);
}

HMENU WINAPI NtUserCreatePopupMenu() {
    LockGuard lock;
    return CreateMenuObj(true);
}

BOOL WINAPI NtUserDestroyMenu(HMENU handle) {
    LockGuard lock;
    if (!MenuOf(handle)) {
        SetLastErr(ERROR_INVALID_MENU_HANDLE);
        return FALSE;
    }
    DestroyMenuObj(handle);
    return TRUE;
}

BOOL WINAPI NtUserRemoveMenu(HMENU handle, UINT id, UINT flags) {
    LockGuard lock;
    MenuObj* m;
    UINT pos;
    if (!FindItem(handle, id, flags, &m, &pos)) return FALSE;
    m->items.erase(m->items.begin() + pos);
    return TRUE;
}

BOOL WINAPI NtUserDeleteMenu(HMENU handle, UINT id, UINT flags) {
    LockGuard lock;
    MenuObj* m;
    UINT pos;
    if (!FindItem(handle, id, flags, &m, &pos)) return FALSE;
    const MenuItem it = m->items[pos];
    m->items.erase(m->items.begin() + pos);
    if (it.type & MF_POPUP) DestroyMenuObj(it.sub);
    return TRUE;
}

DWORD WINAPI NtUserCheckMenuItem(HMENU handle, UINT id, UINT flags) {
    LockGuard lock;
    MenuObj* m;
    UINT pos;
    if (!FindItem(handle, id, flags, &m, &pos)) return static_cast<DWORD>(-1);
    MenuItem& it = m->items[pos];
    const DWORD old = it.state & MF_CHECKED;
    if (flags & MF_CHECKED) it.state |= MF_CHECKED;
    else it.state &= ~static_cast<UINT>(MF_CHECKED);
    return old;
}

BOOL WINAPI NtUserEnableMenuItem(HMENU handle, UINT id, UINT flags) {
    LockGuard lock;
    MenuObj* m;
    UINT pos;
    if (!FindItem(handle, id, flags, &m, &pos)) return static_cast<BOOL>(~0u);
    MenuItem& it = m->items[pos];
    const UINT old = it.state & (MF_GRAYED | MF_DISABLED);
    it.state ^= (old ^ flags) & (MF_GRAYED | MF_DISABLED);
    return static_cast<BOOL>(old);
}

BOOL WINAPI NtUserSetMenuDefaultItem(HMENU handle, UINT item, UINT bypos) {
    LockGuard lock;
    MenuObj* m = MenuOf(handle);
    if (!m) {
        SetLastErr(ERROR_INVALID_MENU_HANDLE);
        return FALSE;
    }
    for (MenuItem& it : m->items) it.state &= ~static_cast<UINT>(MFS_DEFAULT);
    if (item == static_cast<UINT>(-1)) return TRUE;
    BOOL ret = FALSE;
    if (bypos) {
        if (item < m->items.size()) { m->items[item].state |= MFS_DEFAULT; ret = TRUE; }
    } else {
        for (MenuItem& it : m->items)
            if (it.id == item) { it.state |= MFS_DEFAULT; ret = TRUE; }
    }
    return ret;
}

BOOL WINAPI NtUserHiliteMenuItem(HWND hwnd, HMENU handle, UINT item, UINT hilite) {
    LockGuard lock;
    MenuObj* m;
    UINT pos;
    if (!FindItem(handle, item, hilite, &m, &pos)) return FALSE;
    if (hilite & MF_HILITE) m->items[pos].state |= MFS_HILITE;
    else m->items[pos].state &= ~static_cast<UINT>(MFS_HILITE);
    if (m->owner) RepaintBar(m->owner);
    (void)hwnd;
    return TRUE;
}

// A bar item or popup item rectangle in screen coordinates (win32u NtUserGetMenuItemRect).
static bool ItemScreenRect(HWND hwnd, MenuObj& m, UINT pos, RECT* out) {
    if (pos >= m.items.size()) return false;
    if (m.popup || Win(m.wnd)) {
        LayoutPopup(m);
        POINT o;
        if (WindowObj* pw = Win(m.wnd)) o = ClientOriginScreen(pw);
        else if (WindowObj* hw = Win(hwnd)) o = ClientOriginScreen(hw);
        else return false;
        *out = RectOffset(m.items[pos].rect, o.x, o.y);
        return true;
    }
    WindowObj* w = Win(hwnd ? hwnd : m.owner);
    if (!w) return false;
    LayoutBar(m);
    const POINT o = BarOriginScreen(w);
    *out = RectOffset(m.items[pos].rect, o.x, o.y);
    return true;
}

BOOL WINAPI NtUserGetMenuItemRect(HWND hwnd, HMENU handle, UINT item, RECT* rect) {
    LockGuard lock;
    if (!rect) return FALSE;
    MenuObj* m;
    UINT pos;
    if (!FindItem(handle, item, MF_BYPOSITION, &m, &pos)) return FALSE;
    return ItemScreenRect(hwnd, *m, pos, rect);
}

INT WINAPI NtUserMenuItemFromPoint(HWND hwnd, HMENU handle, int x, int y) {
    LockGuard lock;
    MenuObj* m = MenuOf(handle);
    if (!m) return -1;
    for (UINT i = 0; i < m->items.size(); ++i) {
        RECT r;
        if (ItemScreenRect(hwnd, *m, i, &r) && PtInRectangle(r, x, y)) return static_cast<INT>(i);
    }
    return -1;
}

BOOL WINAPI NtUserGetMenuBarInfo(HWND hwnd, LONG id, LONG item, MENUBARINFO* info) {
    LockGuard lock;
    if (id != OBJID_MENU) return FALSE;   // popup windows (OBJID_CLIENT) and the system menu are not covered
    WindowObj* w = WinChecked(hwnd);
    MenuObj* m = w ? BarMenu(w) : nullptr;
    if (!m) return FALSE;
    if (!info || info->cbSize != sizeof(MENUBARINFO)) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (item < 0 || item > static_cast<LONG>(m->items.size())) return FALSE;
    LayoutBar(*m);
    if (item == 0) {
        const POINT o = BarOriginScreen(w);
        const RECT r = BarRectInWindow(w);
        info->rcBar = RECT{o.x, o.y, o.x + (r.right - r.left), o.y + SystemMetric(SM_CYMENU)};
    } else if (!ItemScreenRect(w->handle, *m, static_cast<UINT>(item - 1), &info->rcBar)) {
        return FALSE;
    }
    info->hMenu = m->handle;
    info->hwndMenu = nullptr;
    info->fBarFocused = g_track && g_track->bar && g_track->owner == w->handle;
    info->fFocused = info->fBarFocused;
    if (item) {
        info->fFocused = m->hilite == item - 1;
        if (info->fFocused && (m->items[item - 1].type & MF_POPUP))
            if (MenuObj* sm = MenuOf(m->items[item - 1].sub)) info->hwndMenu = sm->wnd;
    }
    return TRUE;
}

static BOOL SetItemInfo(MenuItem& it, const MENUITEMINFOW* info) {
    if (info->fMask & MIIM_FTYPE) {
        it.type &= ~kTypeMask;
        it.type |= info->fType & kTypeMask;
    }
    if (info->fMask & MIIM_STRING) {
        it.has_text = info->dwTypeData != nullptr;
        if (info->dwTypeData) it.text = info->dwTypeData;
        else it.text.clear();
    }
    if (info->fMask & MIIM_STATE) it.state = info->fState & kStateMask;
    if (info->fMask & MIIM_ID) it.id = info->wID;
    if (info->fMask & MIIM_SUBMENU) {
        it.sub = info->hSubMenu;
        if (it.sub) {
            MenuObj* sm = MenuOf(it.sub);
            if (!sm) {
                SetLastErr(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            sm->popup = true;
            it.type |= MF_POPUP;
        } else {
            it.type &= ~static_cast<UINT>(MF_POPUP);
        }
    }
    if (info->fMask & MIIM_CHECKMARKS) {
        it.checked = info->hbmpChecked;
        it.unchecked = info->hbmpUnchecked;
    }
    if (info->fMask & MIIM_DATA) it.data = info->dwItemData;
    if (info->fMask & MIIM_BITMAP) it.bmp = info->hbmpItem;
    if (!it.has_text && !(it.type & MFT_OWNERDRAW) && !it.bmp) it.type |= MFT_SEPARATOR;
    return TRUE;
}

static BOOL GetItemInfo(HMENU handle, UINT id, UINT flags, MENUITEMINFOW* info, bool ansi) {
    if (!info || info->cbSize != sizeof(*info)) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    MenuObj* m;
    UINT pos;
    if (!FindItem(handle, id, flags, &m, &pos)) {
        SetLastErr(ERROR_MENU_ITEM_NOT_FOUND);
        return FALSE;
    }
    const MenuItem& it = m->items[pos];
    if (info->fMask & MIIM_TYPE) {
        if (info->fMask & (MIIM_STRING | MIIM_FTYPE | MIIM_BITMAP)) {
            SetLastErr(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        info->fType = it.type & kTypeMask;
        info->hbmpItem = it.bmp;
        if (info->fType & (MFT_OWNERDRAW | MFT_SEPARATOR)) {
            info->dwTypeData = nullptr;
            info->cch = 0;
        }
    }
    if (info->fMask & (MIIM_TYPE | MIIM_STRING)) {
        if (!it.has_text) {
            if (info->dwTypeData && info->cch) {
                if (ansi) *reinterpret_cast<char*>(info->dwTypeData) = 0;
                else info->dwTypeData[0] = 0;
            }
            info->cch = 0;
        } else if (ansi) {
            const int len = WideCharToMultiByte(CP_ACP, 0, it.text.c_str(), static_cast<int>(it.text.size()), nullptr, 0, nullptr, nullptr);
            if (info->dwTypeData && info->cch) {
                char* dst = reinterpret_cast<char*>(info->dwTypeData);
                std::string tmp(static_cast<std::size_t>(len), '\0');
                WideCharToMultiByte(CP_ACP, 0, it.text.c_str(), static_cast<int>(it.text.size()), tmp.data(), len, nullptr, nullptr);
                const std::size_t n = std::min<std::size_t>(info->cch - 1, tmp.size());
                std::memcpy(dst, tmp.data(), n);
                dst[n] = 0;
                info->cch = static_cast<UINT>(n);
            } else {
                info->cch = static_cast<UINT>(len);
            }
        } else if (info->dwTypeData && info->cch) {
            const std::size_t n = std::min<std::size_t>(info->cch - 1, it.text.size());
            std::memcpy(info->dwTypeData, it.text.data(), n * sizeof(WCHAR));
            info->dwTypeData[n] = 0;
            info->cch = static_cast<UINT>(n);
        } else {
            info->cch = static_cast<UINT>(it.text.size());
        }
    }
    if (info->fMask & MIIM_FTYPE) info->fType = it.type & kTypeMask;
    if (info->fMask & MIIM_BITMAP) info->hbmpItem = it.bmp;
    if (info->fMask & MIIM_STATE) info->fState = it.state & kStateMask;
    if (info->fMask & MIIM_ID) info->wID = it.id;
    if (info->fMask & MIIM_DATA) info->dwItemData = it.data;
    info->hSubMenu = (info->fMask & MIIM_SUBMENU) ? it.sub : nullptr;
    if (info->fMask & MIIM_CHECKMARKS) {
        info->hbmpChecked = it.checked;
        info->hbmpUnchecked = it.unchecked;
    }
    return TRUE;
}

UINT WINAPI NtUserThunkedMenuItemInfo(HMENU handle, UINT pos, UINT flags, UINT method, MENUITEMINFOW* info,
                                      UNICODE_STRING* str) {
    LockGuard lock;
    (void)str;
    MenuObj* m;
    UINT i;
    switch (method) {
    case kCheckMenuRadioItem: {
        // user32 abuses the structure: cch = last, fMask = the item to check; `pos` is the first
        const UINT first = pos, last = info ? info->cch : 0, check = info ? info->fMask : 0;
        MenuObj* first_menu = nullptr;
        BOOL done = FALSE;
        for (UINT k = first; k <= last && k >= first; ++k) {
            MenuObj* cm;
            UINT cp;
            if (!FindItem(handle, k, flags, &cm, &cp)) continue;
            if (!first_menu) first_menu = cm;
            if (cm != first_menu) continue;
            MenuItem& it = cm->items[cp];
            if (it.type != MFT_SEPARATOR) {
                if (k == check) {
                    it.type |= MFT_RADIOCHECK;
                    it.state |= MFS_CHECKED;
                    done = TRUE;
                } else {
                    it.state &= ~static_cast<UINT>(MFS_CHECKED);
                }
            }
        }
        return done;
    }
    case kGetMenuDefaultItem:
        return DefaultItem(handle, pos, flags);
    case kGetMenuItemID:
        if (!FindItem(handle, pos, flags, &m, &i)) return static_cast<UINT>(-1);
        return (m->items[i].type & MF_POPUP) ? static_cast<UINT>(-1) : m->items[i].id;
    case kGetMenuItemInfoA:
        return GetItemInfo(handle, pos, flags, info, true);
    case kGetMenuItemInfoW:
        return GetItemInfo(handle, pos, flags, info, false);
    case kGetSubMenu: {
        if (!FindItem(handle, pos, MF_BYPOSITION, &m, &i)) return 0;
        return (m->items[i].type & MF_POPUP) ? HandleToUlong(m->items[i].sub) : 0;
    }
    case kInsertMenuItem: {
        if (!info || info->cbSize != sizeof(*info)) {
            SetLastErr(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        UINT at = 0;
        MenuObj* target = nullptr;
        if (!MenuOf(handle)) {
            SetLastErr(ERROR_INVALID_MENU_HANDLE);
            return FALSE;
        }
        if (FindItem(handle, pos, flags, &target, &at, false)) {
            // inserted before the item found (possibly in a submenu)
        } else {
            target = MenuOf(handle);
            at = static_cast<UINT>(target->items.size());
        }
        target->items.insert(target->items.begin() + at, MenuItem{});
        if (!SetItemInfo(target->items[at], info)) {
            target->items.erase(target->items.begin() + at);
            return FALSE;
        }
        return TRUE;
    }
    case kSetMenuItemInfo: {
        if (!info || info->cbSize != sizeof(*info)) {
            SetLastErr(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        if (!FindItem(handle, pos, flags, &m, &i)) return FALSE;
        return SetItemInfo(m->items[i], info);
    }
    case kGetMenuState: {
        if (!FindItem(handle, pos, flags, &m, &i)) return static_cast<UINT>(-1);
        const MenuItem& it = m->items[i];
        if (it.type & MF_POPUP) {
            MenuObj* sm = MenuOf(it.sub);
            if (!sm) return static_cast<UINT>(-1);
            return static_cast<UINT>((sm->items.size() << 8) | ((it.state | it.type) & 0xff));
        }
        return it.type | it.state;
    }
    }
    FIXME(user, "NtUserThunkedMenuItemInfo method %u", method);
    return FALSE;
}

BOOL WINAPI NtUserThunkedMenuInfo(HMENU handle, const MENUINFO* info) {
    LockGuard lock;
    if (!info) {
        SetLastErr(ERROR_NOACCESS);
        return FALSE;
    }
    MenuObj* m = MenuOf(handle);
    if (!m) {
        SetLastErr(ERROR_INVALID_MENU_HANDLE);
        return FALSE;
    }
    std::vector<MenuObj*> todo{m};
    while (!todo.empty()) {
        MenuObj* cur = todo.back();
        todo.pop_back();
        if (info->fMask & MIM_BACKGROUND) cur->back = info->hbrBack;
        if (info->fMask & MIM_HELPID) cur->help_id = info->dwContextHelpID;
        if (info->fMask & MIM_MAXHEIGHT) cur->cy_max = info->cyMax;
        if (info->fMask & MIM_MENUDATA) cur->menu_data = info->dwMenuData;
        if (info->fMask & MIM_STYLE) cur->style = info->dwStyle;
        if (info->fMask & MIM_APPLYTOSUBMENUS)
            for (const MenuItem& it : cur->items)
                if ((it.type & MF_POPUP) && MenuOf(it.sub)) todo.push_back(MenuOf(it.sub));
    }
    return TRUE;
}

BOOL WINAPI NtUserTrackPopupMenuEx(HMENU handle, UINT flags, INT x, INT y, HWND hwnd, TPMPARAMS* params) {
    (void)params;
    {
        LockGuard lock;
        if (!MenuOf(handle)) {
            SetLastErr(ERROR_INVALID_MENU_HANDLE);
            return FALSE;
        }
        if (!WinChecked(hwnd)) return FALSE;
        hwnd = FullHandle(hwnd);
    }
    const TrackResult r = RunTrack(hwnd, handle, false, -1, x, y, flags);
    if (flags & TPM_RETURNCMD) return r.executed ? static_cast<BOOL>(r.id) : 0;
    return TRUE;
}

BOOL WINAPI NtUserEndMenu() {
    LockGuard lock;
    if (!g_track) return FALSE;
    g_track->end = true;
    PostMessageInternal(g_track->owner, WM_NULL, 0, 0);   // wakes the loop if it is waiting
    return TRUE;
}

BOOL WINAPI NtUserDrawMenuBar(HWND hwnd) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w) return FALSE;
    if (!WindowCanHaveBar(w)) return TRUE;
    return SetWindowPosInternal(w->handle, nullptr, 0, 0, 0, 0,
                                SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

}  // namespace sys
}  // namespace win10xbox::user
