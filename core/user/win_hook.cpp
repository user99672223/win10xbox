// core/user hooks: SetWindowsHookEx and the hook chains, in process. The layer is one process, so a hook with
// tid 0 (every thread) is called on whatever thread takes the message, and a hook with a thread id on that thread
// only; there is no module to load (the procedure address is used as it is). The call into the guest's hook
// procedure is the NtUserCallWindowsHook callback into user32 (CbCallWindowsHook: user32 hook.c, User32CallWindowsHook),
// driven like CbCallWinProc: win_hook_params + the module name (always empty here) + at the next 16-byte boundary
// a copy of the data lparam points at; user32 points params->lparam at that copy, calls the procedure and returns the
// LRESULT, followed by the (possibly modified) data, through NtCallbackReturn.
//
// Call points (win32u/message.c, hook.c): WH_KEYBOARD and WH_MOUSE when a hardware message is handed out
// (win_message.cpp), WH_GETMESSAGE for every message PeekMessage / GetMessage return, WH_CALLWNDPROC /
// WH_CALLWNDPROCRET around a message sent to a window procedure of the calling thread, WH_MSGFILTER /
// WH_SYSMSGFILTER through CallMsgFilter. Not done: the low-level hooks (WH_KEYBOARD_LL, WH_MOUSE_LL), WH_CBT,
// WH_SHELL, WH_DEBUG, WH_JOURNAL*, WH_FOREGROUNDIDLE, WinEvent hooks.
#include "user_internal.h"
#include "user_syscalls.h"

#include <cstring>

namespace win10xbox::user {
namespace detail {
namespace {

constexpr std::uint32_t kMaxHookDepth = 25;   // win32u/hook.c: "Too many hooks called recursively"

int Slot(int id) { return id - WH_MINHOOK; }
bool ValidId(int id) { return id >= WH_MINHOOK && id <= WH_MAXHOOK; }

HookObj* FindHook(State& s, HHOOK handle) {
    for (HookObj* h : s.hooks)
        if (!h->removed && h->handle == handle) return h;
    return nullptr;
}

bool AppliesTo(const HookObj* h, DWORD tid) { return !h->removed && (h->tid == 0 || h->tid == tid); }

// The newest hook of `id` that applies to `tid`.
HookObj* FirstHook(State& s, int id, DWORD tid) {
    for (std::size_t i = s.hooks.size(); i-- > 0;)
        if (s.hooks[i]->id == id && AppliesTo(s.hooks[i], tid)) return s.hooks[i];
    return nullptr;
}

// The next older hook of the same id after `cur`.
HookObj* NextHook(State& s, const HookObj* cur, DWORD tid) {
    std::size_t i = 0;
    while (i < s.hooks.size() && s.hooks[i] != cur) ++i;
    while (i-- > 0)
        if (s.hooks[i]->id == cur->id && AppliesTo(s.hooks[i], tid)) return s.hooks[i];
    return nullptr;
}

void SweepRemoved(State& s) {
    for (std::size_t i = 0; i < s.hooks.size();) {
        if (s.hooks[i]->removed) {
            delete s.hooks[i];
            s.hooks.erase(s.hooks.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void RemoveHook(State& s, HookObj* h) {
    if (h->removed) return;
    h->removed = true;
    --s.hook_count[Slot(h->id)];
    if (!s.hook_calls_active) SweepRemoved(s);   // a running chain keeps its entries until it returns
}

// win32u/hook.c call_hook for an in-process hook. Lock held; dropped around the guest call.
LRESULT CallOneHook(State& s, HookObj* h, int code, WPARAM wp, LPARAM lp, std::size_t lparam_size, bool prev_unicode) {
    UserThread& t = CurrentUserThread();
    if (t.hook_depth >= kMaxHookDepth) {
        WARN(user, "too many hooks called recursively, skipping the call");
        return 0;
    }
    const std::size_t head = offsetof(abi::WinHookParams, module) + sizeof(WCHAR);   // the empty module name
    const std::size_t lparam_offset = (head + 15) & ~static_cast<std::size_t>(15);
    const std::size_t total = lparam_size ? lparam_offset + lparam_size : head;
    std::vector<std::uint64_t> buf((total + 7) / 8 + 1, 0);
    auto* p = reinterpret_cast<abi::WinHookParams*>(buf.data());
    p->proc = reinterpret_cast<void*>(h->proc);
    p->handle = h->handle;
    p->pid = GetCurrentProcessId();
    p->tid = h->tid;
    p->id = h->id;
    p->code = code;
    p->wparam = wp;
    p->lparam = lp;
    p->prev_unicode = prev_unicode;
    p->next_unicode = h->unicode;
    p->module[0] = 0;
    if (lparam_size) std::memcpy(reinterpret_cast<std::uint8_t*>(p) + lparam_offset, reinterpret_cast<const void*>(lp), lparam_size);

    const HHOOK saved_hook = t.cur_hook;
    const bool saved_unicode = t.cur_hook_unicode;
    t.cur_hook = h->handle;
    t.cur_hook_unicode = h->unicode;
    ++t.hook_depth;
    ++s.hook_calls_active;
    TRACE(user, "hook %d proc %p code %d wp %llx lp %llx", h->id, reinterpret_cast<void*>(h->proc), code,
          static_cast<unsigned long long>(wp), static_cast<unsigned long long>(lp));
    void* ret = nullptr;
    ULONG len = 0;
    NTSTATUS status;
    {
        UnlockScope unlock;
        status = UserModeCallback(abi::CbCallWindowsHook, p, static_cast<ULONG>(total), &ret, &len);
    }
    LRESULT result = 0;
    if (!status && ret && len >= sizeof(LRESULT)) {
        std::memcpy(&result, ret, sizeof result);
        if (lparam_size && len == sizeof(LRESULT) + lparam_size)
            std::memcpy(reinterpret_cast<void*>(lp), static_cast<const std::uint8_t*>(ret) + sizeof(LRESULT), lparam_size);
    }
    t.cur_hook = saved_hook;
    t.cur_hook_unicode = saved_unicode;
    --t.hook_depth;
    if (--s.hook_calls_active == 0) SweepRemoved(s);
    return result;
}

}  // namespace

bool IsHooked(int id) {
    State* s = StateIfAny();
    return s && ValidId(id) && s->hook_count[Slot(id)] > 0;
}

LRESULT CallHooks(int id, int code, WPARAM wparam, LPARAM lparam, std::size_t lparam_size) {
    if (!IsHooked(id)) return 0;
    State& s = S();
    HookObj* h = FirstHook(s, id, GetCurrentThreadId());
    if (!h) return 0;
    return CallOneHook(s, h, code, wparam, lparam, lparam_size, true);
}

LRESULT CallWindowProcHooked(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool ansi, bool sent_by_this_thread) {
    if (!IsHooked(WH_CALLWNDPROC) && !IsHooked(WH_CALLWNDPROCRET)) return CallWindowProcInternal(hwnd, msg, wp, lp, ansi);
    if (IsHooked(WH_CALLWNDPROC)) {
        CWPSTRUCT cwp{lp, wp, msg, hwnd};
        CallHooks(WH_CALLWNDPROC, HC_ACTION, sent_by_this_thread ? 1 : 0, reinterpret_cast<LPARAM>(&cwp), sizeof cwp);
    }
    const LRESULT result = CallWindowProcInternal(hwnd, msg, wp, lp, ansi);
    if (IsHooked(WH_CALLWNDPROCRET)) {
        CWPRETSTRUCT ret{result, lp, wp, msg, hwnd};
        CallHooks(WH_CALLWNDPROCRET, HC_ACTION, sent_by_this_thread ? 1 : 0, reinterpret_cast<LPARAM>(&ret), sizeof ret);
    }
    return result;
}

void RemoveThreadHooks(DWORD tid) {
    State* s = StateIfAny();
    if (!s) return;
    for (HookObj* h : std::vector<HookObj*>(s->hooks))
        if (h->owner == tid || h->tid == tid) RemoveHook(*s, h);
}

}  // namespace detail

namespace sys {
using namespace detail;

HHOOK WINAPI NtUserSetWindowsHookEx(HINSTANCE inst, UNICODE_STRING* module, DWORD tid, INT id, HOOKPROC proc,
                                    BOOL ansi) {
    LockGuard lock;
    (void)module;   // in process: the procedure address is used as given
    if (!proc) {
        SetLastErr(ERROR_INVALID_FILTER_PROC);
        return nullptr;
    }
    if (!ValidId(id)) {
        SetLastErr(ERROR_INVALID_HOOK_FILTER);
        return nullptr;
    }
    if (tid) {
        if (id == WH_JOURNALRECORD || id == WH_JOURNALPLAYBACK || id == WH_KEYBOARD_LL || id == WH_MOUSE_LL ||
            id == WH_SYSMSGFILTER) {
            SetLastErr(ERROR_GLOBAL_ONLY_HOOK);
            return nullptr;
        }
    } else {
        if (id == WH_JOURNALRECORD || id == WH_JOURNALPLAYBACK) {
            SetLastErr(ERROR_ACCESS_DENIED);
            return nullptr;
        }
        if (id != WH_KEYBOARD_LL && id != WH_MOUSE_LL && !inst) {
            SetLastErr(ERROR_HOOK_NEEDS_HMOD);
            return nullptr;
        }
    }
    if (id == WH_KEYBOARD_LL || id == WH_MOUSE_LL) {
        // Low-level hooks need the raw input stream in front of the queue; not implemented, so say so rather than
        // hand out a handle whose procedure is never called.
        NoteUnimplemented("SetWindowsHookEx(WH_KEYBOARD_LL / WH_MOUSE_LL)");
        return nullptr;
    }
    State& s = S();
    auto* h = new HookObj;
    h->handle = reinterpret_cast<HHOOK>(static_cast<ULONG_PTR>(0x10000 + ++s.next_hook));
    h->id = id;
    h->proc = proc;
    h->tid = tid;
    h->owner = GetCurrentThreadId();
    h->unicode = !ansi;
    s.hooks.push_back(h);
    ++s.hook_count[Slot(id)];
    TRACE(user, "hook %d proc %p tid %04lx -> %p", id, reinterpret_cast<void*>(proc), tid, static_cast<void*>(h->handle));
    return h->handle;
}

BOOL WINAPI NtUserUnhookWindowsHookEx(HHOOK handle) {
    LockGuard lock;
    State& s = S();
    HookObj* h = FindHook(s, handle);
    if (!h) {
        SetLastErr(ERROR_INVALID_HOOK_HANDLE);
        return FALSE;
    }
    RemoveHook(s, h);
    return TRUE;
}

BOOL WINAPI NtUserUnhookWindowsHook(INT id, HOOKPROC proc) {
    LockGuard lock;
    State& s = S();
    const DWORD self = GetCurrentThreadId();
    for (std::size_t i = s.hooks.size(); i-- > 0;) {
        HookObj* h = s.hooks[i];
        if (!h->removed && h->id == id && h->proc == proc && h->owner == self) {
            RemoveHook(s, h);
            return TRUE;
        }
    }
    SetLastErr(ERROR_INVALID_HOOK_HANDLE);
    return FALSE;
}

LRESULT WINAPI NtUserCallNextHookEx(HHOOK hhook, INT code, WPARAM wparam, LPARAM lparam) {
    LockGuard lock;
    (void)hhook;   // like win32u: the hook being called on this thread is the current one
    State& s = S();
    UserThread& t = CurrentUserThread();
    if (!t.cur_hook) return 0;
    HookObj* cur = nullptr;
    for (HookObj* h : s.hooks)
        if (h->handle == t.cur_hook) cur = h;
    if (!cur) return 0;
    HookObj* next = NextHook(s, cur, t.tid);
    if (!next) return 0;
    // lparam already points at user32's copy of the data (if any): handed on as it is
    return CallOneHook(s, next, code, wparam, lparam, 0, t.cur_hook_unicode);
}

BOOL WINAPI NtUserCallMsgFilter(MSG* msg, INT code) {
    LockGuard lock;
    if (!msg) return FALSE;
    if (CallHooks(WH_SYSMSGFILTER, code, 0, reinterpret_cast<LPARAM>(msg), sizeof(MSG))) return TRUE;
    return CallHooks(WH_MSGFILTER, code, 0, reinterpret_cast<LPARAM>(msg), sizeof(MSG)) != 0;
}

}  // namespace sys
}  // namespace win10xbox::user
