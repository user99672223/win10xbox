// core/user shell support: what Wine's explorer.exe needs from win32u to be the desktop of one process (docs/GUI.md,
// "Shell"). There is exactly one window station ("WinSta0") and one desktop; explorer's CreateDesktopW("shell")
// answers a handle to that desktop, whose name is reported as "Default" (so explorer sets the shell window), and the
// desktop window explorer creates is the existing desktop window object, adopted for the creating thread (messages
// for it then reach explorer's window procedure instead of DefWindowProc). The taskbar learns about top-level
// windows from WM_PARENTNOTIFY posted to the desktop window (as win32u does); a program that called
// RegisterShellHookWindow gets WM_SHELLHOOK (HSHELL_WINDOWCREATED / DESTROYED / ACTIVATED / REDRAW).
#include "user_internal.h"
#include "user_syscalls.h"

#include <algorithm>
#include <cstring>

namespace win10xbox::user {
namespace detail {
namespace {

constexpr ULONG_PTR kWinstaHandle = 0x50, kDesktopHandle = 0x54;
constexpr const WCHAR kShellHookName[] = L"SHELLHOOK";

bool IsShellHandle(HANDLE h) {
    const auto v = reinterpret_cast<ULONG_PTR>(h);
    return v == kWinstaHandle || v == kDesktopHandle;
}

bool TaskbarWindow(const WindowObj* w) {
    if (!w || (w->flags & (kWinIsDesktop | kWinIsMsgParent)) || (w->style & WS_CHILD) || w->owner) return false;
    if (!IsTopLevel(w)) return false;
    if ((w->ex_style & WS_EX_TOOLWINDOW) && !(w->ex_style & WS_EX_APPWINDOW)) return false;
    return true;
}

}  // namespace

void ShellNotify(UINT code, HWND hwnd) {
    State* s = StateIfAny();
    if (!s || s->shell_hooks.empty()) return;
    if (!TaskbarWindow(Win(hwnd))) return;
    const UINT msg = AddAtom(kShellHookName, wcslen(kShellHookName));
    for (HWND hook : std::vector<HWND>(s->shell_hooks))
        if (Win(hook)) PostMessageInternal(hook, msg, code, reinterpret_cast<LPARAM>(hwnd));
}

void ShellWindowCreated(WindowObj* w) {
    State& s = S();
    if (!w || !IsTopLevel(w)) return;
    if (WindowObj* desk = Win(s.desktop); desk && desk->tid)
        PostMessageInternal(desk->handle, WM_PARENTNOTIFY, WM_CREATE, reinterpret_cast<LPARAM>(w->handle));
}

void ShellWindowDestroyed(WindowObj* w) {
    State& s = S();
    if (!w) return;
    const HWND hwnd = w->handle;
    if (IsTopLevel(w)) {
        if (WindowObj* desk = Win(s.desktop); desk && desk->tid)
            PostMessageInternal(desk->handle, WM_PARENTNOTIFY, WM_DESTROY, reinterpret_cast<LPARAM>(hwnd));
        ShellNotify(2 /* HSHELL_WINDOWDESTROYED */, hwnd);
    }
    if (s.shell_window == hwnd) s.shell_window = nullptr;
    if (s.shell_listview == hwnd) s.shell_listview = nullptr;
    if (s.progman_window == hwnd) s.progman_window = nullptr;
    if (s.taskman_window == hwnd) s.taskman_window = nullptr;
    s.shell_hooks.erase(std::remove(s.shell_hooks.begin(), s.shell_hooks.end(), hwnd), s.shell_hooks.end());
}

WindowObj* AdoptSpecialWindow(const ClassObj* cls, UserThread& t) {
    if (!cls || !cls->builtin) return nullptr;
    HWND h = nullptr;
    if (cls->name == L"#32769") h = DesktopWindow();
    else if (cls->name == L"Message") h = MessageParentWindow();
    WindowObj* w = Win(h);
    if (!w || w->tid) return nullptr;
    w->tid = t.tid;
    w->thread = &t;
    w->unicode = true;
    TRACE(user, "%ls window %p adopted by thread %lu", cls->name.c_str(), w->handle, static_cast<unsigned long>(t.tid));
    return w;
}

ULONG_PTR ShellCallOneParam(ULONG code, ULONG_PTR arg, bool* handled) {
    State& s = S();
    *handled = true;
    HWND hwnd = reinterpret_cast<HWND>(arg);
    switch (code) {
    case kOneParamRegisterShellHook: {
        WindowObj* w = WinChecked(hwnd);
        if (!w) return FALSE;
        if (std::find(s.shell_hooks.begin(), s.shell_hooks.end(), w->handle) == s.shell_hooks.end())
            s.shell_hooks.push_back(w->handle);
        return TRUE;
    }
    case kOneParamDeregisterShellHook: {
        WindowObj* w = Win(hwnd);
        const HWND h = w ? w->handle : hwnd;
        auto it = std::find(s.shell_hooks.begin(), s.shell_hooks.end(), h);
        if (it == s.shell_hooks.end()) return FALSE;
        s.shell_hooks.erase(it);
        return TRUE;
    }
    }
    *handled = false;
    return 0;
}

}  // namespace detail

namespace sys {
using namespace detail;

HDESK WINAPI NtUserCreateDesktopEx(OBJECT_ATTRIBUTES*, UNICODE_STRING*, DEVMODEW*, DWORD, ACCESS_MASK, ULONG) {
    return reinterpret_cast<HDESK>(kDesktopHandle);
}
HDESK WINAPI NtUserOpenDesktop(OBJECT_ATTRIBUTES*, DWORD, ACCESS_MASK) { return reinterpret_cast<HDESK>(kDesktopHandle); }
HDESK WINAPI NtUserOpenInputDesktop(DWORD, BOOL, ACCESS_MASK) { return reinterpret_cast<HDESK>(kDesktopHandle); }
HWINSTA WINAPI NtUserCreateWindowStation(OBJECT_ATTRIBUTES*, ACCESS_MASK, ULONG, ULONG, ULONG, ULONG, ULONG) {
    return reinterpret_cast<HWINSTA>(kWinstaHandle);
}
HWINSTA WINAPI NtUserOpenWindowStation(OBJECT_ATTRIBUTES*, ACCESS_MASK) { return reinterpret_cast<HWINSTA>(kWinstaHandle); }
BOOL WINAPI NtUserCloseDesktop(HDESK) { return TRUE; }
BOOL WINAPI NtUserCloseWindowStation(HWINSTA) { return TRUE; }
HDESK WINAPI NtUserGetThreadDesktop(DWORD) { return reinterpret_cast<HDESK>(kDesktopHandle); }
BOOL WINAPI NtUserSetThreadDesktop(HDESK desktop) {
    if (desktop != reinterpret_cast<HDESK>(kDesktopHandle)) {
        SetLastErr(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return TRUE;
}
HWINSTA WINAPI NtUserGetProcessWindowStation() { return reinterpret_cast<HWINSTA>(kWinstaHandle); }
BOOL WINAPI NtUserSetProcessWindowStation(HWINSTA winsta) {
    if (winsta != reinterpret_cast<HWINSTA>(kWinstaHandle)) {
        SetLastErr(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return TRUE;
}
BOOL WINAPI NtUserSwitchDesktop(HDESK) { return TRUE; }

BOOL WINAPI NtUserGetObjectInformation(HANDLE handle, INT index, void* info, DWORD len, DWORD* needed) {
    if (!IsShellHandle(handle)) {
        SetLastErr(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    const bool winsta = reinterpret_cast<ULONG_PTR>(handle) == kWinstaHandle;
    const WCHAR* text = nullptr;
    switch (index) {
    case UOI_FLAGS: {
        if (needed) *needed = sizeof(USEROBJECTFLAGS);
        if (len < sizeof(USEROBJECTFLAGS)) {
            SetLastErr(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        USEROBJECTFLAGS f{};
        f.dwFlags = winsta ? WSF_VISIBLE : 0;
        std::memcpy(info, &f, sizeof f);
        return TRUE;
    }
    case UOI_NAME: text = winsta ? L"WinSta0" : L"Default"; break;
    case UOI_TYPE: text = winsta ? L"WindowStation" : L"Desktop"; break;
    default:
        SetLastErr(ERROR_CALL_NOT_IMPLEMENTED);
        return FALSE;
    }
    const DWORD bytes = static_cast<DWORD>((wcslen(text) + 1) * sizeof(WCHAR));
    if (needed) *needed = bytes;
    if (len < bytes) {
        SetLastErr(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::memcpy(info, text, bytes);
    return TRUE;
}

BOOL WINAPI NtUserSetObjectInformation(HANDLE handle, INT, void*, DWORD) {
    if (!IsShellHandle(handle)) {
        SetLastErr(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return TRUE;
}

// DrawCaptionTemp / DrawCaption / DrawCaptionTempW: a caption bar or a taskbar button face (DC_INBUTTON) with an icon and
// a title. Made of the public GDI syscalls (each takes the lock itself), like win32u's defwnd.c does.
BOOL WINAPI NtUserDrawCaptionTemp(HWND hwnd, HDC hdc, const RECT* rect, HFONT font, HICON icon, const WCHAR* str,
                                  UINT flags) {
    if (!rect || !hdc) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    RECT rc = *rect;
    COLORREF text_color;
    HBRUSH face;
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    std::wstring title;
    HICON window_icon = nullptr;
    {
        LockGuard lock;
        const bool active = (flags & 0x0001 /* DC_ACTIVE */) != 0;
        if (flags & 0x0010 /* DC_INBUTTON */) {
            face = SysColorBrush(COLOR_3DFACE);
            text_color = SysColor(COLOR_BTNTEXT);
        } else {
            face = SysColorBrush(active ? COLOR_ACTIVECAPTION : COLOR_INACTIVECAPTION);
            text_color = SysColor(active ? COLOR_CAPTIONTEXT : COLOR_INACTIVECAPTIONTEXT);
        }
        SysParamsInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        if (WindowObj* w = Win(hwnd)) title = w->text;
    }
    if (hwnd && !icon && (flags & 0x0004 /* DC_ICON */)) window_icon = NtUserInternalGetWindowIcon(hwnd, ICON_SMALL);
    HGDIOBJ old_brush = NtGdiSelectBrush(hdc, face);
    NtGdiPatBlt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, PATCOPY);
    NtGdiSelectBrush(hdc, old_brush);
    if ((flags & 0x0004 /* DC_ICON */) && !(flags & 0x0002 /* DC_SMALLCAP */)) {
        const int cx = NtUserCallOneParam(SM_CXSMICON, abi::OneParamGetSystemMetrics);
        const int cy = NtUserCallOneParam(SM_CYSMICON, abi::OneParamGetSystemMetrics);
        const int x = rc.left + 2, y = (rc.bottom + rc.top - cy) / 2;
        if (!icon) icon = window_icon;
        if (icon) NtUserDrawIconEx(hdc, x, y, icon, cx, cy, 0, nullptr, DI_NORMAL);
        rc.left = x + cx;
    }
    if (flags & 0x0008 /* DC_TEXT */) {
        const WCHAR* text = str ? str : title.c_str();
        const INT count = static_cast<INT>(wcslen(text));
        DWORD prev = 0;
        NtGdiGetAndSetDCDword(hdc, abi::DcSetTextColor, text_color, &prev);
        NtGdiGetAndSetDCDword(hdc, abi::DcSetBkMode, TRANSPARENT, &prev);
        HFONT own = font ? nullptr : NtGdiHfontCreate(&((flags & 0x0002) ? ncm.lfSmCaptionFont : ncm.lfCaptionFont),
                                                      sizeof(LOGFONTW), 0, 0, nullptr);
        HGDIOBJ old_font = NtGdiSelectFont(hdc, font ? font : own);
        SIZE sz{};
        NtGdiGetTextExtentExW(hdc, text, count, 0, nullptr, nullptr, &sz, 0);
        rc.left += 2;
        int x = rc.left;
        if (flags & 0x4000) x = rc.left + std::max<LONG>(0, (rc.right - rc.left - sz.cx) / 2);
        const int y = (rc.top + rc.bottom - sz.cy) / 2;
        NtGdiExtTextOutW(hdc, x, y, ETO_CLIPPED, &rc, text, count, nullptr, 0);
        NtGdiSelectFont(hdc, old_font);
        if (own) NtGdiDeleteObjectApp(own);
    }
    return FALSE;   // sic: DrawCaptionTemp returns what win32u returns, FALSE
}

// The layer shows one display of the host's size: explorer.exe stores the current mode and asks for the size of its
// virtual desktop; both succeed when they match the screen, a different size is a bad mode.
LONG WINAPI NtUserChangeDisplaySettings(UNICODE_STRING*, DEVMODEW* devmode, HWND, DWORD, void*) {
    LockGuard lock;
    if (!devmode) return DISP_CHANGE_SUCCESSFUL;
    RECT screen;
    ScreenRect(&screen);
    if ((devmode->dmFields & DM_PELSWIDTH) && static_cast<LONG>(devmode->dmPelsWidth) != screen.right) return DISP_CHANGE_BADMODE;
    if ((devmode->dmFields & DM_PELSHEIGHT) && static_cast<LONG>(devmode->dmPelsHeight) != screen.bottom) return DISP_CHANGE_BADMODE;
    return DISP_CHANGE_SUCCESSFUL;
}

BOOL WINAPI NtUserSetShellWindowEx(HWND shell, HWND list_view) {
    LockGuard lock;
    State& s = S();
    WindowObj* w = WinChecked(shell);
    if (!w) return FALSE;
    if (list_view && !WinChecked(list_view)) return FALSE;
    if (Win(s.shell_window) && s.shell_window != w->handle) {
        SetLastErr(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    s.shell_window = w->handle;
    s.shell_listview = list_view ? Win(list_view)->handle : w->handle;
    return TRUE;
}

HWND WINAPI NtUserSetProgmanWindow(HWND hwnd) {
    LockGuard lock;
    WindowObj* w = hwnd ? WinChecked(hwnd) : nullptr;
    if (hwnd && !w) return nullptr;
    S().progman_window = w ? w->handle : nullptr;
    return S().progman_window;
}

HWND WINAPI NtUserSetTaskmanWindow(HWND hwnd) {
    LockGuard lock;
    WindowObj* w = hwnd ? WinChecked(hwnd) : nullptr;
    if (hwnd && !w) return nullptr;
    S().taskman_window = w ? w->handle : nullptr;
    return S().taskman_window;
}

}  // namespace sys
}  // namespace win10xbox::user
