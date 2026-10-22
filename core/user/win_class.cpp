// core/user window classes and atoms: RegisterClassExW/GetClassInfoEx and the
// class longs, the builtin control classes win32u registers itself (with
// user32's client procedures from NtUserInitializeClientPfnArrays), and the
// user atom table shared by class names and RegisterWindowMessage.
//
// Window procedures are kept as raw pointers, an (A, W) pair: user32's
// dispatch_win_proc_params calls procA/procW directly and converts messages
// when the window and the procedure disagree (ansi vs ansi_dst), so Wine's
// WINPROC_HANDLE encoding is never handed out.
#include "user_internal.h"
#include "user_syscalls.h"

#include <cstring>
#include <cwctype>

namespace win10xbox::user {
namespace detail {
namespace {

bool NameEquals(const std::wstring& a, const WCHAR* b, std::size_t len) {
    if (a.size() != len) return false;
    for (std::size_t i = 0; i < len; ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return true;
}

// "#123" -> 123 for integer atoms spelled as strings.
bool IntegralAtom(const WCHAR* name, std::size_t len, ATOM* out) {
    if (!len || name[0] != L'#') return false;
    unsigned v = 0;
    for (std::size_t i = 1; i < len; ++i) {
        if (name[i] < L'0' || name[i] > L'9') return false;
        v = v * 10 + static_cast<unsigned>(name[i] - L'0');
        if (v >= MAXINTATOM) return false;
    }
    if (len < 2) return false;
    *out = static_cast<ATOM>(v);
    return true;
}

struct BuiltinClass {
    const wchar_t* name;
    UINT style;
    abi::ClientProc proc;
    int extra;
    int brush;   // COLOR_* + 1, or 0
};
// win32u's builtin classes (class.c builtin_classes + edit, desktop and message).
const BuiltinClass kBuiltins[] = {
    {L"Button", CS_DBLCLKS | CS_VREDRAW | CS_HREDRAW | CS_PARENTDC, abi::ProcButton, sizeof(UINT) + 2 * sizeof(HANDLE), 0},
    {L"ComboBox", CS_PARENTDC | CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW, abi::ProcCombo, sizeof(void*), 0},
    {L"ComboLBox", CS_DBLCLKS | CS_SAVEBITS, abi::ProcComboLBox, sizeof(void*), 0},
    {L"#32770", CS_SAVEBITS | CS_DBLCLKS, abi::ProcDialog, DLGWINDOWEXTRA, 0},
    {L"#32772", 0, abi::ProcIconTitle, 0, 0},
    {L"IME", 0, abi::ProcIme, 2 * sizeof(LONG_PTR), 0},
    {L"ListBox", CS_DBLCLKS, abi::ProcListBox, sizeof(void*), 0},
    {L"#32768", CS_DROPSHADOW | CS_SAVEBITS | CS_DBLCLKS, abi::ProcMenu, sizeof(HMENU), COLOR_MENU + 1},
    {L"MDIClient", 0, abi::ProcMDIClient, 2 * sizeof(void*), COLOR_APPWORKSPACE + 1},
    {L"ScrollBar", CS_DBLCLKS | CS_VREDRAW | CS_HREDRAW | CS_PARENTDC, abi::ProcScrollBar, 64, 0},
    {L"Static", CS_DBLCLKS | CS_PARENTDC, abi::ProcStatic, 2 * sizeof(HANDLE), 0},
    {L"Edit", CS_DBLCLKS | CS_PARENTDC, abi::ProcEdit, sizeof(UINT64), 0},
    {L"#32769", CS_DBLCLKS, abi::ProcDesktop, 0, COLOR_BACKGROUND + 1},
    {L"Message", 0, abi::ProcMessage, 0, 0},
};

ClassObj* AddClass(ClassObj* cls) {
    State& s = S();
    if (cls->local) s.classes.insert(s.classes.begin(), cls);
    else s.classes.push_back(cls);
    return cls;
}

}  // namespace

ATOM FindAtom(const WCHAR* name, std::size_t len) {
    ATOM a;
    if (IntegralAtom(name, len, &a)) return a;
    State& s = S();
    for (std::size_t i = 0; i < s.atoms.size(); ++i)
        if (NameEquals(s.atoms[i], name, len)) return static_cast<ATOM>(0xC000 + i);
    return 0;
}

ATOM AddAtom(const WCHAR* name, std::size_t len) {
    if (ATOM a = FindAtom(name, len)) return a;
    State& s = S();
    if (s.atoms.size() >= 0x3fff) return 0;
    s.atoms.emplace_back(name, len);
    return static_cast<ATOM>(0xC000 + s.atoms.size() - 1);
}

std::wstring AtomName(ATOM atom) {
    if (atom < MAXINTATOM) return L"#" + std::to_wstring(atom);
    State& s = S();
    const std::size_t i = static_cast<std::size_t>(atom - 0xC000);
    return i < s.atoms.size() ? s.atoms[i] : std::wstring();
}

ClassObj* FindClassByAtom(ATOM atom) {
    for (ClassObj* c : S().classes)
        if (c->atom == atom) return c;
    return nullptr;
}

ClassObj* FindClass(HINSTANCE instance, const WCHAR* name, std::size_t len) {
    State& s = S();
    if (!s.builtins_registered) RegisterBuiltinClasses();
    ATOM atom = 0;
    const bool by_atom = IntegralAtom(name, len, &atom);
    const auto inst = reinterpret_cast<ULONG_PTR>(instance);
    for (ClassObj* c : s.classes) {
        if (by_atom ? c->atom != atom : !NameEquals(c->name, name, len)) continue;
        const auto ci = reinterpret_cast<ULONG_PTR>(c->instance);
        // Same matching as win32u's find_class: any instance, a global class, the same
        // instance, or the same module (the high bits of a 32-bit-aligned HINSTANCE).
        if (!inst || !c->local || ci == inst || ((ci >> 16) && (ci & ~0xffffull) == (inst & ~0xffffull))) return c;
    }
    return nullptr;
}

WinProc ResolveWinProc(WNDPROC func, bool ansi) {
    WinProc p;
    if (!func) return p;
    State& s = S();
    // builtin procedures have both halves; "some apps confuse A and W" (win32u's find_winproc)
    for (const WinProc& b : s.client_procs)
        if (b.a == func || b.w == func) return b;
    for (const ClassObj* c : s.classes)
        if ((c->proc.a == func && c->proc.w) || (c->proc.w == func && c->proc.a)) return c->proc;
    if (ansi) p.a = func;
    else p.w = func;
    return p;
}

void RegisterBuiltinClasses() {
    State& s = S();
    if (s.builtins_registered) return;
    s.builtins_registered = true;
    for (const BuiltinClass& b : kBuiltins) {
        auto* c = new ClassObj;
        c->name = b.name;
        c->instance = s.user32;
        c->local = false;
        c->builtin = true;
        c->style = b.style;
        c->proc = s.client_procs[b.proc];
        c->win_extra = b.extra;
        c->brush = reinterpret_cast<HBRUSH>(static_cast<ULONG_PTR>(b.brush));
        c->cursor = nullptr;   // LoadImage(IDC_ARROW) needs the cursor code (Todo)
        ATOM a = 0;
        c->atom = IntegralAtom(c->name.data(), c->name.size(), &a) ? a : AddAtom(c->name.data(), c->name.size());
        AddClass(c);
    }
    TRACE(user, "%zu builtin classes registered (client procs %s)", std::size(kBuiltins),
          s.client_procs[abi::ProcDefWnd].w ? "set" : "NOT set yet");
}

}  // namespace detail

namespace sys {
using namespace detail;

NTSTATUS WINAPI NtUserInitializeClientPfnArrays(const WNDPROC* procsA, const WNDPROC* procsW, const WNDPROC* workers,
                                                HINSTANCE user_module) {
    LockGuard lock;
    (void)workers;
    State& s = S();
    for (int i = 0; i < abi::ProcCount; ++i) {
        s.client_procs[i].a = procsA ? procsA[i] : nullptr;
        s.client_procs[i].w = procsW ? procsW[i] : nullptr;
    }
    s.user32 = user_module;
    // classes registered before (none normally) get their procedures now
    for (ClassObj* c : s.classes)
        if (c->builtin)
            for (const auto& b : kBuiltins)
                if (c->name == b.name) c->proc = s.client_procs[b.proc];
    TRACE(user, "user32 %p: client procs installed (DefWindowProc A %p W %p)", user_module,
          reinterpret_cast<void*>(s.client_procs[abi::ProcDefWnd].a), reinterpret_cast<void*>(s.client_procs[abi::ProcDefWnd].w));
    return dispatch::detail::st::Success;
}

ATOM WINAPI NtUserRegisterClassExWOW(const WNDCLASSEXW* wc, UNICODE_STRING* name, UNICODE_STRING* version,
                                     abi::ClientMenuName* menu_name, DWORD fnid, DWORD flags, DWORD* wow) {
    LockGuard lock;
    (void)wow;
    State& s = S();
    if (!s.builtins_registered) RegisterBuiltinClasses();
    const bool builtin = fnid != 0, ansi = flags != 0;
    if (!wc || !name || wc->cbSize != sizeof(WNDCLASSEXW) || wc->cbClsExtra < 0 || wc->cbWndExtra < 0 ||
        (!builtin && s.user32 && wc->hInstance == s.user32)) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return 0;
    }
    HINSTANCE instance = wc->hInstance;
    if (!instance) instance = dispatch::detail::PebField<HINSTANCE>(dispatch::detail::peb::ImageBaseAddress);
    const std::size_t len = name->Length / sizeof(WCHAR);
    const bool local = !builtin && !(wc->style & CS_GLOBALCLASS);
    // a class of the same name and instance (or a global one of the same name) already exists
    for (ClassObj* c : s.classes) {
        if (!NameEquals(c->name, name->Buffer, len)) continue;
        if (c->instance == instance || (!local && !c->local)) {
            SetLastErr(ERROR_CLASS_ALREADY_EXISTS);
            return 0;
        }
    }
    auto* c = new ClassObj;
    c->name.assign(name->Buffer, len);
    c->name_offset = version ? version->Length / sizeof(WCHAR) : 0;
    c->instance = instance;
    c->local = local;
    c->builtin = builtin;
    c->style = wc->style;
    c->proc = ResolveWinProc(wc->lpfnWndProc, ansi);
    c->cls_extra = wc->cbClsExtra;
    c->win_extra = wc->cbWndExtra;
    c->extra.assign(static_cast<std::size_t>(wc->cbClsExtra), 0);
    c->icon = wc->hIcon;
    c->icon_sm = wc->hIconSm;
    c->cursor = wc->hCursor;
    c->brush = wc->hbrBackground;
    if (menu_name) c->menu = *menu_name;
    ATOM a = 0;
    c->atom = IntegralAtom(name->Buffer, len, &a) ? a : AddAtom(name->Buffer, len);
    if (!c->atom) {
        delete c;
        SetLastErr(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    AddClass(c);
    TRACE(user, "class %ls atom %04x proc A %p W %p instance %p style %08x brush %p extra %d/%d", c->name.c_str(),
          c->atom, reinterpret_cast<void*>(c->proc.a), reinterpret_cast<void*>(c->proc.w), instance, c->style, c->brush,
          c->cls_extra, c->win_extra);
    return c->atom;
}

BOOL WINAPI NtUserUnregisterClass(UNICODE_STRING* name, HINSTANCE instance, abi::ClientMenuName* menu_name) {
    LockGuard lock;
    State& s = S();
    if (!name) return FALSE;
    ClassObj* c = FindClass(instance, name->Buffer, name->Length / sizeof(WCHAR));
    if (!c || c->builtin) {
        SetLastErr(ERROR_CLASS_DOES_NOT_EXIST);
        return FALSE;
    }
    if (c->windows) {
        SetLastErr(ERROR_CLASS_HAS_WINDOWS);
        return FALSE;
    }
    if (menu_name) *menu_name = c->menu;
    if (reinterpret_cast<ULONG_PTR>(c->brush) > COLOR_GRADIENTINACTIVECAPTION + 1) GdiDelete(c->brush);
    s.classes.erase(std::find(s.classes.begin(), s.classes.end(), c));
    delete c;
    return TRUE;
}

ATOM WINAPI NtUserGetClassInfoEx(HINSTANCE instance, UNICODE_STRING* name, WNDCLASSEXW* wc,
                                 abi::ClientMenuName* menu_name, BOOL ansi) {
    LockGuard lock;
    State& s = S();
    if (!name) return 0;
    ClassObj* c = FindClass(instance, name->Buffer, name->Length / sizeof(WCHAR));
    if (!c) {
        TRACE(user, "%.*ls %p: not found", static_cast<int>(name->Length / 2), name->Buffer, instance);
        SetLastErr(ERROR_CLASS_DOES_NOT_EXIST);
        return 0;
    }
    if (wc) {
        wc->style = c->style;
        WNDPROC p = ansi ? c->proc.a : c->proc.w;
        wc->lpfnWndProc = p ? p : (c->proc.w ? c->proc.w : c->proc.a);
        wc->cbClsExtra = c->cls_extra;
        wc->cbWndExtra = c->win_extra;
        wc->hInstance = (instance == s.user32) ? nullptr : instance;
        wc->hIcon = c->icon;
        wc->hIconSm = c->icon_sm;
        wc->hCursor = c->cursor;
        wc->hbrBackground = c->brush;
        wc->lpszMenuName = ansi ? reinterpret_cast<const WCHAR*>(c->menu.nameA) : c->menu.nameW;
        wc->lpszClassName = name->Buffer;
    }
    if (menu_name) *menu_name = c->menu;
    return c->atom;
}

INT WINAPI NtUserGetClassName(HWND hwnd, BOOL real, UNICODE_STRING* name) {
    LockGuard lock;
    (void)real;
    WindowObj* w = WinChecked(hwnd);
    if (!w || !name || name->MaximumLength <= sizeof(WCHAR)) {
        if (w) SetLastErr(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const std::wstring& n = w->cls->name;
    const std::size_t off = w->cls->name_offset;
    const std::size_t len = n.size() > off ? n.size() - off : 0;
    const std::size_t max = name->MaximumLength / sizeof(WCHAR) - 1;
    const std::size_t copy = len < max ? len : max;
    std::memcpy(name->Buffer, n.data() + off, copy * sizeof(WCHAR));
    name->Buffer[copy] = 0;
    return static_cast<INT>(copy);
}

ULONG WINAPI NtUserGetAtomName(ATOM atom, UNICODE_STRING* name) {
    LockGuard lock;
    if (!atom || !name) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::wstring n = AtomName(atom);
    if (n.empty()) {
        SetLastErr(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (name->MaximumLength < sizeof(WCHAR)) {
        SetLastErr(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const std::size_t max = name->MaximumLength / sizeof(WCHAR) - 1;
    const std::size_t copy = n.size() < max ? n.size() : max;
    std::memcpy(name->Buffer, n.data(), copy * sizeof(WCHAR));
    name->Buffer[copy] = 0;
    return static_cast<ULONG>(copy);
}

ATOM WINAPI NtUserRegisterWindowMessage(UNICODE_STRING* name) {
    LockGuard lock;
    if (!name || !name->Length) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return AddAtom(name->Buffer, name->Length / sizeof(WCHAR));
}

namespace {

ULONG_PTR SetClassLongSize(HWND hwnd, INT offset, LONG_PTR newval, UINT size, BOOL ansi) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w) return 0;
    ClassObj* c = w->cls;
    if (offset >= 0) {
        if (offset + size > c->extra.size()) {
            SetLastErr(ERROR_INVALID_INDEX);
            return 0;
        }
        ULONG_PTR old = 0;
        std::memcpy(&old, c->extra.data() + offset, size);
        std::memcpy(c->extra.data() + offset, &newval, size);
        return old;
    }
    ULONG_PTR old = 0;
    switch (offset) {
    case GCLP_HBRBACKGROUND: old = reinterpret_cast<ULONG_PTR>(c->brush); c->brush = reinterpret_cast<HBRUSH>(newval); break;
    case GCLP_HCURSOR: old = reinterpret_cast<ULONG_PTR>(c->cursor); c->cursor = reinterpret_cast<HCURSOR>(newval); break;
    case GCLP_HICON: old = reinterpret_cast<ULONG_PTR>(c->icon); c->icon = reinterpret_cast<HICON>(newval); break;
    case GCLP_HICONSM: old = reinterpret_cast<ULONG_PTR>(c->icon_sm); c->icon_sm = reinterpret_cast<HICON>(newval); break;
    case GCLP_WNDPROC:
        old = reinterpret_cast<ULONG_PTR>(ansi ? (c->proc.a ? c->proc.a : c->proc.w) : (c->proc.w ? c->proc.w : c->proc.a));
        c->proc = ResolveWinProc(reinterpret_cast<WNDPROC>(newval), ansi);
        break;
    case GCLP_MENUNAME:
        FIXME(user, "SetClassLong(GCLP_MENUNAME)");
        break;
    case GCL_STYLE: old = c->style; c->style = static_cast<UINT>(newval); break;
    case GCL_CBWNDEXTRA: old = static_cast<ULONG_PTR>(c->win_extra); c->win_extra = static_cast<int>(newval); break;
    case GCL_CBCLSEXTRA: SetLastErr(ERROR_INVALID_PARAMETER); return 0;
    case GCLP_HMODULE: old = reinterpret_cast<ULONG_PTR>(c->instance); c->instance = reinterpret_cast<HINSTANCE>(newval); break;
    case GCW_ATOM: SetLastErr(ERROR_INVALID_PARAMETER); return 0;
    default: SetLastErr(ERROR_INVALID_INDEX); return 0;
    }
    return old;
}

}  // namespace

DWORD WINAPI NtUserSetClassLong(HWND hwnd, INT offset, LONG newval, BOOL ansi) {
    return static_cast<DWORD>(SetClassLongSize(hwnd, offset, newval, sizeof(LONG), ansi));
}

ULONG_PTR WINAPI NtUserSetClassLongPtr(HWND hwnd, INT offset, LONG_PTR newval, BOOL ansi) {
    return SetClassLongSize(hwnd, offset, newval, sizeof(LONG_PTR), ansi);
}

WORD WINAPI NtUserSetClassWord(HWND hwnd, INT offset, WORD newval) {
    if (offset < 0) {
        SetLastErr(ERROR_INVALID_INDEX);
        return 0;
    }
    return static_cast<WORD>(SetClassLongSize(hwnd, offset, newval, sizeof(WORD), TRUE));
}

}  // namespace sys

namespace detail {
// Class longs as GetClassLong[Ptr]{A,W} / GetClassWord read them (NtUserCallHwndParam codes).
ULONG_PTR GetClassLongSize(HWND hwnd, INT offset, UINT size, bool ansi) {
    WindowObj* w = WinChecked(hwnd);
    if (!w) return 0;
    ClassObj* c = w->cls;
    if (offset >= 0) {
        if (offset + size > c->extra.size()) {
            SetLastErr(ERROR_INVALID_INDEX);
            return 0;
        }
        ULONG_PTR v = 0;
        std::memcpy(&v, c->extra.data() + offset, size);
        return v;
    }
    switch (offset) {
    case GCLP_HBRBACKGROUND: return reinterpret_cast<ULONG_PTR>(c->brush);
    case GCLP_HCURSOR: return reinterpret_cast<ULONG_PTR>(c->cursor);
    case GCLP_HICON: return reinterpret_cast<ULONG_PTR>(c->icon);
    case GCLP_HICONSM: return reinterpret_cast<ULONG_PTR>(c->icon_sm);
    case GCLP_WNDPROC:
        return reinterpret_cast<ULONG_PTR>(ansi ? (c->proc.a ? c->proc.a : c->proc.w) : (c->proc.w ? c->proc.w : c->proc.a));
    case GCLP_MENUNAME: return reinterpret_cast<ULONG_PTR>(ansi ? static_cast<void*>(c->menu.nameA) : static_cast<void*>(c->menu.nameW));
    case GCL_STYLE: return c->style;
    case GCL_CBWNDEXTRA: return static_cast<ULONG_PTR>(c->win_extra);
    case GCL_CBCLSEXTRA: return static_cast<ULONG_PTR>(c->cls_extra);
    case GCLP_HMODULE: return reinterpret_cast<ULONG_PTR>(c->instance);
    case GCW_ATOM: return c->atom;
    }
    SetLastErr(ERROR_INVALID_INDEX);
    return 0;
}
}  // namespace detail

}  // namespace win10xbox::user
