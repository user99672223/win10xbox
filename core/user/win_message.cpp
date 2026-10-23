// core/user message queues: per-thread posted / input / sent messages, the
// paint and timer pseudo-messages, SendMessage (same thread: the window
// procedure through the NtUserCallWinProc callback; another thread: queued
// and waited for), NtUserMessageCall's codes, GetMessage / PeekMessage /
// MsgWaitForMultipleObjectsEx, timers, input from the display driver
// (InputSink -> WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_KEYDOWN ... posted to the
// window under the cursor or with the focus), and the keyboard state and
// US-layout keyboard functions.
#include "user_internal.h"
#include "user_syscalls.h"
#include "win_scroll.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace win10xbox::user {
namespace detail {
namespace {

constexpr UINT kUserTimerMinimum = 10;
constexpr UINT kUserTimerMaximum = 0x7fffffff;
constexpr int kMaxRecursion = 64;

// Message times are in the guest's own tick count: what its GetTickCount() reads out of KUSER_SHARED_DATA (the
// formula of kernel32's GetTickCount64), so GetMessageTime() and GetTickCount() can be compared.
DWORD Now() {
    const auto* usd = reinterpret_cast<const volatile ULONG*>(0x7ffe0000);
    const ULONG multiplier = usd[1];                    // TickCountMultiplier
    ULONG high, low;
    do {
        high = usd[0x324 / 4];                            // TickCount.High1Time (KSYSTEM_TIME at 0x320)
        low = usd[0x320 / 4];                            // TickCount.LowPart
    } while (high != usd[0x328 / 4]);                    // High2Time
    return static_cast<DWORD>((((static_cast<ULONGLONG>(high) << 32) | low) * multiplier) >> 24);
}

void FillWinProcParams(abi::WinProcParams* p, WindowObj* w, UINT msg, WPARAM wp, LPARAM lp, bool ansi, int mapping) {
    WNDPROC func = w->proc.w ? w->proc.w : w->proc.a;
    if (!w->unicode && w->proc.a) func = w->proc.a;
    p->func = func;
    p->hwnd = w->handle;
    p->msg = msg;
    p->wparam = wp;
    p->lparam = lp;
    p->ansi = ansi;
    p->ansi_dst = !w->unicode;
    p->mapping = mapping;
    p->dpi_context = w->dpi_context;
    // get_winproc_params(fixup_ansi_dst): prefer the procedure of the message's own character set
    if (!w->dlg_info) {
        if (ansi) {
            if (w->proc.a) p->ansi_dst = TRUE;
            else if (w->proc.w) p->ansi_dst = FALSE;
        } else {
            if (w->proc.w) p->ansi_dst = FALSE;
            else if (w->proc.a) p->ansi_dst = TRUE;
        }
    }
    p->procA = w->proc.a ? w->proc.a : func;
    p->procW = w->proc.w ? w->proc.w : func;
}

bool MessageInRange(UINT msg, UINT first, UINT last) {
    if (!first && !last) return true;
    if (first <= last) return msg >= first && msg <= last;
    return msg >= first || msg <= last;
}

bool WindowMatches(HWND filter, HWND hwnd) {
    if (!filter) return true;
    if (filter == reinterpret_cast<HWND>(-1)) return hwnd == nullptr;
    if (!hwnd) return false;
    filter = FullHandle(filter);
    if (hwnd == filter) return true;
    for (WindowObj* w = Win(hwnd); w; w = Win(w->parent))
        if (w->parent == filter) return true;
    return false;
}

void ProcessSentMessages(UserThread& t) {
    while (!t.sent.empty()) {
        SentMsg* m = t.sent.front();
        t.sent.pop_front();
        if (m->abandoned) {
            CloseHandle(m->done_event);
            delete m;
            continue;
        }
        ++t.send_depth;
        const LRESULT r = Win(m->hwnd) ? CallWindowProcInternal(m->hwnd, m->msg, m->wparam, m->lparam, m->ansi) : 0;
        --t.send_depth;
        m->result = r;
        m->done = true;
        SetEvent(m->done_event);
    }
}

// The earliest timer deadline of `t`, in milliseconds from now (INFINITE when none).
DWORD NextTimerDelay(const UserThread& t) {
    if (t.timers.empty()) return INFINITE;
    const ULONGLONG now = GetTickCount64();
    ULONGLONG best = ~0ull;
    for (const Timer& tm : t.timers) best = std::min(best, tm.next);
    return best <= now ? 0 : static_cast<DWORD>(std::min<ULONGLONG>(best - now, 0x7ffffffe));
}

UINT QueueBits(UserThread& t) {
    UINT bits = 0;
    if (!t.posted.empty() || t.quit) bits |= QS_POSTMESSAGE;
    if (!t.sent.empty()) bits |= QS_SENDMESSAGE;
    for (const QueuedMsg& m : t.input) {
        if (m.msg.message >= WM_KEYFIRST && m.msg.message <= WM_KEYLAST) bits |= QS_KEY;
        else if (m.msg.message == WM_MOUSEMOVE || m.msg.message == WM_NCMOUSEMOVE) bits |= QS_MOUSEMOVE;
        else bits |= QS_MOUSEBUTTON;
    }
    if (FindPaintWindow(&t)) bits |= QS_PAINT;
    const ULONGLONG now = GetTickCount64();
    for (const Timer& tm : t.timers)
        if (tm.next <= now) { bits |= QS_TIMER; break; }
    return bits;
}

void SetMessageInfo(UserThread& t, const MSG& m, ULONG_PTR extra) {
    if (!t.info) return;
    t.info->message_time = m.time;
    t.info->message_pos = static_cast<DWORD>(MAKELONG(static_cast<WORD>(m.pt.x), static_cast<WORD>(m.pt.y)));
    t.info->message_extra = extra;
}

// Hardware input (defined with the sink below).
enum class HwStatus { Deliver, Consumed, Skip };
HwStatus ProcessMouseMessage(UserThread& t, const QueuedMsg& raw, MSG* out, HWND hwnd_filter, UINT first, UINT last,
                             bool remove);
void AcceptInput(UserThread& t, const QueuedMsg& q);

// One PeekMessage pass. Lock held; may drop it (sent messages, hit testing).
bool PeekOnce(UserThread& t, MSG* out, HWND hwnd, UINT first, UINT last, UINT flags) {
    ProcessSentMessages(t);
    const bool remove = (flags & PM_REMOVE) != 0;
    const UINT qs = flags >> 16;   // PM_QS_*: 0 means everything
    const bool want_post = !qs || (qs & (QS_POSTMESSAGE | QS_SENDMESSAGE)) != 0;
    const bool want_input = !qs || (qs & QS_INPUT) != 0;
    const bool want_paint = !qs || (qs & QS_PAINT) != 0;
    const bool want_timer = !qs || (qs & QS_TIMER) != 0;
    State& s = S();
    if (want_post) {
        for (auto it = t.posted.begin(); it != t.posted.end(); ++it) {
            if (!WindowMatches(hwnd, it->msg.hwnd) || !MessageInRange(it->msg.message, first, last)) continue;
            *out = it->msg;
            const ULONG_PTR extra = it->extra;
            if (remove) t.posted.erase(it);
            SetMessageInfo(t, *out, extra);
            return true;
        }
        if (t.quit && MessageInRange(WM_QUIT, first, last)) {
            *out = MSG{nullptr, WM_QUIT, static_cast<WPARAM>(t.quit_code), 0, Now(), s.cursor};
            if (remove) t.quit = false;
            SetMessageInfo(t, *out, 0);
            return true;
        }
    }
    if (want_input) {
        for (std::size_t i = 0; i < t.input.size();) {
            const QueuedMsg q = t.input[i];
            if (q.raw) {
                // a mouse message the hardware reported: find the window, ask it, activate (may run window procedures)
                MSG m{};
                const HwStatus st = ProcessMouseMessage(t, q, &m, hwnd, first, last, remove);
                std::size_t at = 0;
                while (at < t.input.size() && t.input[at].seq != q.seq) ++at;
                if (st == HwStatus::Deliver) {
                    *out = m;
                    SetMessageInfo(t, *out, q.extra);
                    return true;
                }
                if (st == HwStatus::Consumed) {
                    i = 0;   // gone from the queue; procedures that ran may have changed it, start over
                    continue;
                }
                i = at < t.input.size() ? at + 1 : 0;
                continue;
            }
            if (!WindowMatches(hwnd, q.msg.hwnd) || !MessageInRange(q.msg.message, first, last)) {
                ++i;
                continue;
            }
            *out = q.msg;
            if (remove) AcceptInput(t, q);
            SetMessageInfo(t, *out, q.extra);
            return true;
        }
    }
    if (want_paint && MessageInRange(WM_PAINT, first, last)) {
        HWND ph = nullptr;
        if (hwnd && hwnd != reinterpret_cast<HWND>(-1)) {
            ForEachWindowPreorder(FullHandle(hwnd), [&](WindowObj* w) {
                if (ph) return false;
                if (w->tid == t.tid && IsWindowReallyVisible(w) &&
                    (!RectEmpty(w->update) || w->internal_paint || w->nc_pending))
                    ph = w->handle;
                return true;
            });
        } else if (hwnd != reinterpret_cast<HWND>(-1)) {
            ph = FindPaintWindow(&t);
        }
        if (ph) {
            *out = MSG{ph, WM_PAINT, 0, 0, Now(), s.cursor};
            if (WindowObj* w = Win(ph)) w->internal_paint = false;
            SetMessageInfo(t, *out, 0);
            return true;
        }
    }
    if (want_timer) {
        const ULONGLONG now = GetTickCount64();
        for (Timer& tm : t.timers) {
            if (tm.next > now || !WindowMatches(hwnd, tm.hwnd) || !MessageInRange(tm.msg, first, last)) continue;
            *out = MSG{tm.hwnd, tm.msg, tm.id, reinterpret_cast<LPARAM>(tm.proc), Now(), s.cursor};
            if (remove) tm.next = now + tm.timeout;
            SetMessageInfo(t, *out, 0);
            return true;
        }
    }
    return false;
}

// Drains the display driver's input queue (lock NOT held: the sink takes it). One thread at a time, so
// two GUI threads cannot deliver two batches out of order.
SRWLOCK g_pump_lock = SRWLOCK_INIT;

void DriverProcessEvents(winexbox::IWinexboxDriver* d) {
    AcquireSRWLockExclusive(&g_pump_lock);
    d->ProcessEvents(QS_ALLINPUT);
    ReleaseSRWLockExclusive(&g_pump_lock);
}

void PumpDriver() {
    winexbox::IWinexboxDriver* d = Driver();
    if (!d) return;
    HANDLE wake = d->WakeHandle();
    if (wake && WaitForSingleObject(wake, 0) == WAIT_OBJECT_0) DriverProcessEvents(d);
}

// Blocks until something may have arrived for `t`, a handle is signalled, or `timeout` ms pass.
// Lock held on entry and exit, dropped while waiting. Returns the WaitForMultipleObjectsEx result
// relative to `handles` (count = the queue).
DWORD WaitForQueue(UserThread& t, DWORD count, const HANDLE* handles, DWORD timeout, bool alertable, bool wait_all) {
    std::vector<HANDLE> hs(handles, handles + count);
    hs.push_back(t.wake);
    winexbox::IWinexboxDriver* d = Driver();
    HANDLE driver_wake = d ? static_cast<HANDLE>(d->WakeHandle()) : nullptr;
    if (driver_wake) hs.push_back(driver_wake);
    const DWORD timer = NextTimerDelay(t);
    if (timer < timeout) timeout = timer;
    DWORD r;
    {
        UnlockScope unlock;
        if (wait_all && count) {
            // MWMO_WAITALL: the handles all together, or the queue
            r = WaitForMultipleObjectsEx(count, handles, TRUE, 0, alertable);
            if (r == WAIT_TIMEOUT)
                r = WaitForMultipleObjectsEx(static_cast<DWORD>(hs.size() - count), hs.data() + count, FALSE, timeout, alertable);
            if (r < WAIT_OBJECT_0 + hs.size() - count && r != WAIT_TIMEOUT && r != WAIT_IO_COMPLETION) r += count;
        } else {
            r = WaitForMultipleObjectsEx(static_cast<DWORD>(hs.size()), hs.data(), FALSE, timeout, alertable);
        }
        if (driver_wake && r == WAIT_OBJECT_0 + hs.size() - 1) {
            DriverProcessEvents(d);
            r = WAIT_OBJECT_0 + count;   // treat as "the queue"
        }
    }
    return r;
}

// ---- Input -------------------------------------------------------------------------------------
// The shape is Wine's: the sink (the driver's events, SendInput) updates the asynchronous key state and the
// cursor and queues a hardware message on the thread that owns the window the input is for; that thread
// finishes the job when it takes the message off its queue (ProcessMouseMessage = process_mouse_message), because
// the hit test, the activation and WM_SETCURSOR are messages to window procedures.

// The previous click, for double clicks (message.c: static MSG clk_msg).
struct ClickState { UINT message; HWND hwnd; WPARAM wparam; DWORD time; POINT pt; };
ClickState g_click{};

UserThread* ForegroundThread() {
    State& s = S();
    if (WindowObj* fg = Win(s.foreground)) return FindUserThreadById(fg->tid);
    return nullptr;
}

// server/queue.c update_key_state (0x80 = down; 0x01 = toggled, kept for CapsLock, NumLock and ScrollLock only)
void UpdateKeyState(BYTE* k, BYTE vk, bool down) {
    if (down) {
        // only the lock keys have a toggle state
        if (!(k[vk] & 0x80) && (vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL)) k[vk] ^= 0x01;
        k[vk] |= 0x80;
    } else {
        k[vk] &= static_cast<BYTE>(~0x80);
    }
    auto sync = [&](BYTE generic, BYTE l, BYTE r) {
        k[generic] = static_cast<BYTE>((k[generic] & 0x01) | ((k[l] | k[r]) & 0x80));
    };
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) sync(VK_SHIFT, VK_LSHIFT, VK_RSHIFT);
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) sync(VK_CONTROL, VK_LCONTROL, VK_RCONTROL);
    if (vk == VK_LMENU || vk == VK_RMENU) sync(VK_MENU, VK_LMENU, VK_RMENU);
}

bool IsButtonDownMessage(UINT m) { return m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN || m == WM_XBUTTONDOWN; }

// The virtual key and direction of a raw mouse button message.
bool ButtonOfMessage(UINT m, WPARAM wp, BYTE* vk, bool* down) {
    switch (m) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: *vk = VK_LBUTTON; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: *vk = VK_RBUTTON; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: *vk = VK_MBUTTON; break;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: *vk = HIWORD(wp) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1; break;
    default: return false;
    }
    *down = IsButtonDownMessage(m);
    return true;
}

// Wine's accept_hardware_message / update_input_key_state: the state GetKeyState sees moves when an
// application takes the message, not when the hardware reports it.
void UpdateInputKeyState(const QueuedMsg& q) {
    BYTE* k = S().input_key_state;
    if (q.raw) {
        BYTE vk;
        bool down;
        if (ButtonOfMessage(q.msg.message, q.msg.wParam, &vk, &down)) UpdateKeyState(k, vk, down);
    } else if (q.vk && q.msg.message >= WM_KEYFIRST && q.msg.message <= WM_KEYLAST) {
        UpdateKeyState(k, q.vk, q.msg.message == WM_KEYDOWN || q.msg.message == WM_SYSKEYDOWN);
    }
}

void QueueInput(UserThread* t, QueuedMsg q) {
    q.hardware = true;
    q.seq = ++S().input_seq;
    DWORD bits = q.msg.message >= WM_KEYFIRST && q.msg.message <= WM_KEYLAST ? QS_KEY
                 : q.msg.message == WM_MOUSEMOVE ? QS_MOUSEMOVE : QS_MOUSEBUTTON;
    // moves are coalesced with the previous one for the same window (server/queue.c)
    if (q.raw && q.msg.message == WM_MOUSEMOVE && !t->input.empty()) {
        QueuedMsg& back = t->input.back();
        if (back.raw && back.msg.message == WM_MOUSEMOVE && back.msg.hwnd == q.msg.hwnd) {
            back = q;
            WakeThread(t, bits);
            return;
        }
    }
    t->input.push_back(q);
    WakeThread(t, bits);
}

WPARAM MouseKeyState() {
    const BYTE* k = S().key_state;
    WPARAM w = 0;
    if (k[VK_LBUTTON] & 0x80) w |= MK_LBUTTON;
    if (k[VK_RBUTTON] & 0x80) w |= MK_RBUTTON;
    if (k[VK_MBUTTON] & 0x80) w |= MK_MBUTTON;
    if (k[VK_SHIFT] & 0x80) w |= MK_SHIFT;
    if (k[VK_CONTROL] & 0x80) w |= MK_CONTROL;
    if (k[VK_XBUTTON1] & 0x80) w |= MK_XBUTTON1;
    if (k[VK_XBUTTON2] & 0x80) w |= MK_XBUTTON2;
    return w;
}

// Set-1 scan codes of a US keyboard, by virtual key.
BYTE ScanOf(UINT vk);
UINT VkOfScan(UINT scan);

// A key event: updates the asynchronous state and queues WM_KEYDOWN / WM_KEYUP (or the SYS variants) for the
// focus window of the foreground thread, the active window when nothing has the focus.
void RouteKey(const winexbox::InputEvent& ev, DWORD time) {
    State& s = S();
    const bool down = ev.kind == winexbox::InputKind::KeyDown;
    BYTE vk = static_cast<BYTE>(ev.vk ? ev.vk : VkOfScan(ev.scan));
    if (vk == VK_SHIFT) vk = VK_LSHIFT;
    if (vk == VK_CONTROL) vk = (ev.flags & winexbox::kInputExtendedKey) ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) vk = (ev.flags & winexbox::kInputExtendedKey) ? VK_RMENU : VK_LMENU;
    const bool was_down = (s.key_state[vk] & 0x80) != 0;
    UpdateKeyState(s.key_state, vk, down);
    const BYTE generic = (vk == VK_LSHIFT || vk == VK_RSHIFT) ? VK_SHIFT
                         : (vk == VK_LCONTROL || vk == VK_RCONTROL) ? VK_CONTROL
                         : (vk == VK_LMENU || vk == VK_RMENU) ? VK_MENU : vk;
    const UINT scan = ev.scan ? ev.scan : ScanOf(generic);
    const bool alt = (s.key_state[VK_MENU] & 0x80) != 0;
    UINT msg = down ? WM_KEYDOWN : WM_KEYUP;
    if ((alt && !(s.key_state[VK_CONTROL] & 0x80)) || generic == VK_MENU || generic == VK_F10)
        msg = down ? WM_SYSKEYDOWN : WM_SYSKEYUP;
    LPARAM lp = 1 | (static_cast<LPARAM>(scan & 0xff) << 16);
    if (ev.flags & winexbox::kInputExtendedKey) lp |= 1 << 24;
    if (alt && msg >= WM_SYSKEYDOWN) lp |= 1 << 29;
    if (!down || was_down) lp |= 1 << 30;
    if (!down) lp |= static_cast<LPARAM>(1u << 31);
    UserThread* t = ForegroundThread();
    HWND target = nullptr;
    if (t) {
        if (Win(t->focus)) target = t->focus;
        else if (Win(t->active)) {
            target = t->active;
            if (msg < WM_SYSKEYDOWN) msg += WM_SYSKEYDOWN - WM_KEYDOWN;   // a key with nothing focused is a system key
        }
    }
    if (!target) {
        TRACE(user, "key %02x %s dropped: no focus or active window", vk, down ? "down" : "up");
        return;
    }
    QueuedMsg q;
    q.msg = MSG{target, msg, generic, lp, time, s.cursor};
    q.vk = vk;
    QueueInput(t, q);
}

// A pointer event: moves the cursor and queues the raw message for the thread that owns the window under it
// (or the capture window). `from_guest`: SendInput, which also has to tell the driver where the cursor is.
void RouteMouse(const winexbox::InputEvent& ev, DWORD time, bool from_guest) {
    State& s = S();
    const bool move = ev.kind == winexbox::InputKind::MouseMove;
    // Buttons and the wheel carry the position they happened at; (0,0) means "where the cursor is".
    if (move || ev.x || ev.y) {
        s.cursor = POINT{ev.x, ev.y};
        if (from_guest)
            if (winexbox::IWinexboxDriver* d = Driver()) {
                d->CursorSetPos(ev.x, ev.y);
                winexbox::Point p{};
                if (d->CursorGetPos(&p)) s.cursor = POINT{p.x, p.y};   // the driver clamps to the screen / clip rectangle
            }
    }
    const POINT pt = s.cursor;
    QueuedMsg q;
    q.raw = true;
    q.msg = MSG{nullptr, 0, 0, 0, time, pt};
    switch (ev.kind) {
    case winexbox::InputKind::MouseMove:
        q.msg.message = WM_MOUSEMOVE;
        q.msg.wParam = MouseKeyState();
        break;
    case winexbox::InputKind::MouseWheel:
        q.msg.message = (ev.flags & winexbox::kInputHorizontal) ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL;
        q.msg.wParam = MAKEWPARAM(static_cast<WORD>(MouseKeyState()), static_cast<WORD>(ev.wheel_delta));
        break;
    default: {
        const bool down = ev.kind == winexbox::InputKind::MouseButtonDown;
        BYTE vk = 0;
        WPARAM extra = 0;
        switch (ev.button) {
        case winexbox::MouseButton::Left: q.msg.message = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; vk = VK_LBUTTON; break;
        case winexbox::MouseButton::Right: q.msg.message = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; vk = VK_RBUTTON; break;
        case winexbox::MouseButton::Middle: q.msg.message = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; vk = VK_MBUTTON; break;
        case winexbox::MouseButton::X1: q.msg.message = down ? WM_XBUTTONDOWN : WM_XBUTTONUP; vk = VK_XBUTTON1; extra = XBUTTON1; break;
        case winexbox::MouseButton::X2: q.msg.message = down ? WM_XBUTTONDOWN : WM_XBUTTONUP; vk = VK_XBUTTON2; extra = XBUTTON2; break;
        }
        UpdateKeyState(s.key_state, vk, down);
        q.msg.wParam = MouseKeyState() | (extra << 16);
        break;
    }
    }
    // Who gets it: the capture window, else the top-level window at the point; the thread is the one of the
    // deepest child there (server/queue.c find_hardware_message_window).
    HWND scope = nullptr;
    UserThread* target = nullptr;
    for (UserThread* t : s.threads)
        if (Win(t->capture)) {
            scope = t->capture;
            target = t;
            break;
        }
    if (!scope) {
        scope = ShallowWindowFromPoint(pt);
        std::vector<HWND> list;
        WindowsFromPoint(scope, pt, &list);
        if (WindowObj* deepest = Win(list.empty() ? scope : list.front())) target = FindUserThreadById(deepest->tid);
    }
    WindowObj* sw = Win(scope);
    if (!sw || (sw->flags & kWinIsDesktop) || !target) {
        TRACE(user, "mouse message %04x at %ld,%ld dropped: no window of ours under the cursor", q.msg.message, pt.x, pt.y);
        return;
    }
    q.msg.hwnd = sw->handle;
    QueueInput(target, q);
}

void RouteInput(const winexbox::InputEvent& ev, DWORD time, bool from_guest) {
    State& s = S();
    s.last_input_time = time;
    switch (ev.kind) {
    case winexbox::InputKind::KeyDown:
    case winexbox::InputKind::KeyUp:
        RouteKey(ev, time);
        break;
    default:
        RouteMouse(ev, time, from_guest);
        break;
    }
}

// Removes `q` from the queue of `t` and moves the keyboard state it carries (Wine: accept_hardware_message).
void AcceptInput(UserThread& t, const QueuedMsg& q) {
    for (auto it = t.input.begin(); it != t.input.end(); ++it)
        if (it->seq == q.seq) {
            t.input.erase(it);
            break;
        }
    UpdateInputKeyState(q);
}

// message.c window_from_point with send_nchittest: the window at `pt` below `scope`, asking the windows of this
// thread (WM_NCHITTEST) from the front; HTTRANSPARENT passes the point on to the next window behind.
HWND WindowFromPointHit(UserThread& t, HWND scope, POINT pt, int* hittest) {
    *hittest = HTNOWHERE;
    std::vector<HWND> list;
    WindowsFromPoint(scope, pt, &list);
    for (HWND h : list) {
        WindowObj* w = Win(h);
        if (!w) continue;   // destroyed by an earlier window's procedure
        if (w->style & WS_DISABLED) {
            *hittest = HTERROR;
            return h;
        }
        if (w->tid != t.tid) {
            *hittest = HTCLIENT;
            return h;
        }
        const int res = static_cast<int>(SendMessageInternal(h, WM_NCHITTEST, 0, MAKELPARAM(static_cast<WORD>(pt.x), static_cast<WORD>(pt.y))));
        if (res != HTTRANSPARENT) {
            *hittest = res;
            return h;
        }
    }
    return nullptr;
}

// message.c send_parent_notify
void SendParentNotify(HWND hwnd, WORD event, POINT screen_pt) {
    for (;;) {
        WindowObj* w = Win(hwnd);
        if (!w || !(w->style & WS_CHILD) || (w->ex_style & WS_EX_NOPARENTNOTIFY)) break;
        WindowObj* parent = Win(w->parent);
        if (!parent || (parent->flags & kWinIsDesktop)) break;
        const POINT o = ClientOriginScreen(parent);
        hwnd = parent->handle;
        SendMessageInternal(hwnd, WM_PARENTNOTIFY, MAKEWPARAM(event, 0),
                            MAKELPARAM(static_cast<WORD>(screen_pt.x - o.x), static_cast<WORD>(screen_pt.y - o.y)));
    }
}

// message.c process_mouse_message, run by the thread taking `raw` off its queue (lock held, dropped while window
// procedures run). Deliver: *out is the message the application gets. Consumed: it is gone from the queue and
// nothing is delivered. Skip: the filter does not want it; it stays queued.
HwStatus ProcessMouseMessage(UserThread& t, const QueuedMsg& raw, MSG* out, HWND hwnd_filter, UINT first, UINT last,
                             bool remove) {
    MSG msg = raw.msg;
    const QueuedMsg copy = raw;
    bool accepted = false;
    auto accept = [&] {
        if (accepted) return;
        accepted = true;
        AcceptInput(t, copy);
    };

    // find the window to dispatch this mouse message to
    const bool captured = Win(t.capture) != nullptr;
    int hittest = HTCLIENT;
    HWND hwnd;
    if (captured) {
        hwnd = t.capture;
    } else {
        hwnd = WindowFromPointHit(t, Win(msg.hwnd) ? msg.hwnd : DesktopWindow(), msg.pt, &hittest);
    }
    WindowObj* w = Win(hwnd);
    if (!w || w->tid != t.tid) {
        TRACE(user, "mouse message %04x at %ld,%ld: no window of this thread there, dropped", msg.message, msg.pt.x, msg.pt.y);
        accept();
        return HwStatus::Consumed;
    }
    msg.hwnd = hwnd = w->handle;
    if (!WindowMatches(hwnd_filter, hwnd)) return HwStatus::Skip;

    const UINT base = msg.message;
    UINT message = base;
    WPARAM wparam = msg.wParam;
    POINT pt = msg.pt;
    // windows has no concept of a non-client wheel message
    if (base != WM_MOUSEWHEEL && base != WM_MOUSEHWHEEL) {
        if (hittest != HTCLIENT) {
            message += WM_NCMOUSEMOVE - WM_MOUSEMOVE;
            wparam = static_cast<WPARAM>(hittest);
        } else {
            const POINT o = ClientOriginScreen(w);
            pt.x -= o.x;
            pt.y -= o.y;
        }
    }
    const LPARAM lparam = MAKELPARAM(static_cast<WORD>(pt.x), static_cast<WORD>(pt.y));

    // translate double clicks (note that moves can slip in between the down and the double click)
    if (IsButtonDownMessage(base)) {
        bool update = remove;
        if (hittest != HTCLIENT || (w->cls && (w->cls->style & CS_DBLCLKS))) {
            if (base == g_click.message && hwnd == g_click.hwnd && msg.wParam == g_click.wparam &&
                msg.time - g_click.time < sys::NtUserGetDoubleClickTime() &&
                std::abs(msg.pt.x - g_click.pt.x) < SystemMetric(SM_CXDOUBLECLK) / 2 &&
                std::abs(msg.pt.y - g_click.pt.y) < SystemMetric(SM_CYDOUBLECLK) / 2) {
                message += WM_LBUTTONDBLCLK - WM_LBUTTONDOWN;
                if (update) {
                    g_click.message = 0;   // clear the double click conditions
                    update = false;
                }
            }
        }
        if (!MessageInRange(message, first, last)) return HwStatus::Skip;
        if (update) g_click = ClickState{base, hwnd, msg.wParam, msg.time, msg.pt};
    } else if (!MessageInRange(message, first, last)) {
        return HwStatus::Skip;
    }

    if (hittest == HTERROR || hittest == HTNOWHERE) {
        SendMessageInternal(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), MAKELONG(static_cast<WORD>(hittest), base));
        accept();
        return HwStatus::Consumed;
    }

    if (remove) accept();

    auto result = [&] {
        msg.message = message;
        msg.wParam = wparam;
        msg.lParam = lparam;
        *out = msg;
    };
    if (!remove || captured) {
        result();
        return HwStatus::Deliver;
    }

    bool eat = false;
    if (IsButtonDownMessage(base)) {
        SendParentNotify(hwnd, static_cast<WORD>(base), msg.pt);
        // activate the window if needed
        if (Win(hwnd) && hwnd != t.active) {
            const HWND top = TopLevelOf(hwnd);
            WindowObj* tw = Win(top);
            if (tw && (tw->style & (WS_POPUP | WS_CHILD)) != WS_CHILD) {
                const LRESULT ret = SendMessageInternal(hwnd, WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(top),
                                                        MAKELONG(static_cast<WORD>(hittest), base));
                switch (ret) {
                case MA_NOACTIVATEANDEAT:
                    eat = true;
                    break;
                case MA_NOACTIVATE:
                    break;
                case MA_ACTIVATEANDEAT:
                    eat = true;
                    [[fallthrough]];
                case MA_ACTIVATE:
                case 0:
                    if (!SetActiveWindowInternal(top, true)) eat = true;
                    break;
                default:
                    WARN(user, "unknown WM_MOUSEACTIVATE code %lld", static_cast<long long>(ret));
                    break;
                }
            }
        }
    }
    // Windows sends the normal mouse message as the message parameter of WM_SETCURSOR even for non-client messages
    if (Win(hwnd)) SendMessageInternal(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), MAKELONG(static_cast<WORD>(hittest), base));
    result();
    return eat ? HwStatus::Consumed : HwStatus::Deliver;
}

class Sink final : public winexbox::IEventSink {
public:
    // Driver events carry the host's clock; the message time is the layer's tick count (what GetTickCount shows).
    void OnInput(const winexbox::InputEvent& ev) override {
        LockGuard lock;
        if (!StateIfAny()) {
            TRACE(user, "input event %d dropped: no window was ever created", static_cast<int>(ev.kind));
            return;
        }
        RouteInput(ev, Now(), false);
    }
    void OnWindowEvent(winexbox::WindowId hwnd, WindowEvent ev, const winexbox::Rect&) override {
        LockGuard lock;
        if (!StateIfAny()) return;
        if (ev == WindowEvent::CloseRequested)
            if (WindowObj* w = Win(reinterpret_cast<HWND>(hwnd)))
                if (UserThread* t = FindUserThreadById(w->tid))
                    PostThreadMessageInternal(t, w->handle, WM_SYSCOMMAND, SC_CLOSE, 0);
    }
};
// Constant-initialised (no members, constexpr constructor): no init guard.
Sink g_sink;

// ---- US keyboard tables ---------------------------------------------------------------------

struct KeyMap { BYTE vk; BYTE scan; char normal; char shifted; };
const KeyMap kKeys[] = {
    {'1', 0x02, '1', '!'}, {'2', 0x03, '2', '@'}, {'3', 0x04, '3', '#'}, {'4', 0x05, '4', '$'}, {'5', 0x06, '5', '%'},
    {'6', 0x07, '6', '^'}, {'7', 0x08, '7', '&'}, {'8', 0x09, '8', '*'}, {'9', 0x0a, '9', '('}, {'0', 0x0b, '0', ')'},
    {VK_OEM_MINUS, 0x0c, '-', '_'}, {VK_OEM_PLUS, 0x0d, '=', '+'}, {VK_BACK, 0x0e, '\b', '\b'}, {VK_TAB, 0x0f, '\t', '\t'},
    {'Q', 0x10, 'q', 'Q'}, {'W', 0x11, 'w', 'W'}, {'E', 0x12, 'e', 'E'}, {'R', 0x13, 'r', 'R'}, {'T', 0x14, 't', 'T'},
    {'Y', 0x15, 'y', 'Y'}, {'U', 0x16, 'u', 'U'}, {'I', 0x17, 'i', 'I'}, {'O', 0x18, 'o', 'O'}, {'P', 0x19, 'p', 'P'},
    {VK_OEM_4, 0x1a, '[', '{'}, {VK_OEM_6, 0x1b, ']', '}'}, {VK_RETURN, 0x1c, '\r', '\r'}, {VK_CONTROL, 0x1d, 0, 0},
    {'A', 0x1e, 'a', 'A'}, {'S', 0x1f, 's', 'S'}, {'D', 0x20, 'd', 'D'}, {'F', 0x21, 'f', 'F'}, {'G', 0x22, 'g', 'G'},
    {'H', 0x23, 'h', 'H'}, {'J', 0x24, 'j', 'J'}, {'K', 0x25, 'k', 'K'}, {'L', 0x26, 'l', 'L'}, {VK_OEM_1, 0x27, ';', ':'},
    {VK_OEM_7, 0x28, '\'', '"'}, {VK_OEM_3, 0x29, '`', '~'}, {VK_SHIFT, 0x2a, 0, 0}, {VK_OEM_5, 0x2b, '\\', '|'},
    {'Z', 0x2c, 'z', 'Z'}, {'X', 0x2d, 'x', 'X'}, {'C', 0x2e, 'c', 'C'}, {'V', 0x2f, 'v', 'V'}, {'B', 0x30, 'b', 'B'},
    {'N', 0x31, 'n', 'N'}, {'M', 0x32, 'm', 'M'}, {VK_OEM_COMMA, 0x33, ',', '<'}, {VK_OEM_PERIOD, 0x34, '.', '>'},
    {VK_OEM_2, 0x35, '/', '?'}, {VK_RSHIFT, 0x36, 0, 0}, {VK_MULTIPLY, 0x37, '*', '*'}, {VK_MENU, 0x38, 0, 0},
    {VK_SPACE, 0x39, ' ', ' '}, {VK_CAPITAL, 0x3a, 0, 0}, {VK_F1, 0x3b, 0, 0}, {VK_F2, 0x3c, 0, 0}, {VK_F3, 0x3d, 0, 0},
    {VK_F4, 0x3e, 0, 0}, {VK_F5, 0x3f, 0, 0}, {VK_F6, 0x40, 0, 0}, {VK_F7, 0x41, 0, 0}, {VK_F8, 0x42, 0, 0},
    {VK_F9, 0x43, 0, 0}, {VK_F10, 0x44, 0, 0}, {VK_NUMLOCK, 0x45, 0, 0}, {VK_SCROLL, 0x46, 0, 0}, {VK_HOME, 0x47, 0, 0},
    {VK_UP, 0x48, 0, 0}, {VK_PRIOR, 0x49, 0, 0}, {VK_SUBTRACT, 0x4a, '-', '-'}, {VK_LEFT, 0x4b, 0, 0},
    {VK_CLEAR, 0x4c, 0, 0}, {VK_RIGHT, 0x4d, 0, 0}, {VK_ADD, 0x4e, '+', '+'}, {VK_END, 0x4f, 0, 0}, {VK_DOWN, 0x50, 0, 0},
    {VK_NEXT, 0x51, 0, 0}, {VK_INSERT, 0x52, 0, 0}, {VK_DELETE, 0x53, 0, 0}, {VK_F11, 0x57, 0, 0}, {VK_F12, 0x58, 0, 0},
    {VK_ESCAPE, 0x01, 0x1b, 0x1b}, {VK_NUMPAD0, 0x52, '0', '0'}, {VK_NUMPAD1, 0x4f, '1', '1'}, {VK_NUMPAD2, 0x50, '2', '2'},
    {VK_NUMPAD3, 0x51, '3', '3'}, {VK_NUMPAD4, 0x4b, '4', '4'}, {VK_NUMPAD5, 0x4c, '5', '5'}, {VK_NUMPAD6, 0x4d, '6', '6'},
    {VK_NUMPAD7, 0x47, '7', '7'}, {VK_NUMPAD8, 0x48, '8', '8'}, {VK_NUMPAD9, 0x49, '9', '9'}, {VK_DECIMAL, 0x53, '.', '.'},
    {VK_DIVIDE, 0x35, '/', '/'}, {VK_LWIN, 0x5b, 0, 0}, {VK_RWIN, 0x5c, 0, 0}, {VK_APPS, 0x5d, 0, 0},
    {VK_LSHIFT, 0x2a, 0, 0}, {VK_LCONTROL, 0x1d, 0, 0}, {VK_RCONTROL, 0x1d, 0, 0}, {VK_LMENU, 0x38, 0, 0}, {VK_RMENU, 0x38, 0, 0},
};

const KeyMap* KeyByVk(UINT vk) {
    for (const KeyMap& k : kKeys)
        if (k.vk == vk) return &k;
    return nullptr;
}

BYTE ScanOf(UINT vk) {
    const KeyMap* k = KeyByVk(vk);
    return k ? k->scan : 0;
}

UINT VkOfScan(UINT scan) {
    for (const KeyMap& k : kKeys)
        if (k.scan == (scan & 0xff)) return k.vk;
    return 0;
}

int CharOf(UINT vk, const BYTE* state) {
    const KeyMap* k = KeyByVk(vk);
    if (!k || !k->normal) return 0;
    const bool shift = (state[VK_SHIFT] & 0x80) != 0;
    const bool caps = (state[VK_CAPITAL] & 0x01) != 0;
    const bool ctrl = (state[VK_CONTROL] & 0x80) != 0;
    const bool alt = (state[VK_MENU] & 0x80) != 0;
    if (ctrl && !alt) {
        if (vk >= 'A' && vk <= 'Z') return static_cast<int>(vk - 'A' + 1);
        if (vk == VK_OEM_4) return 0x1b;
        if (vk == VK_OEM_5) return 0x1c;
        if (vk == VK_OEM_6) return 0x1d;
        if (vk == VK_BACK) return 0x7f;
        if (vk == VK_RETURN) return '\n';
        return vk == VK_SPACE ? ' ' : 0;
    }
    bool upper = shift;
    if (vk >= 'A' && vk <= 'Z' && caps) upper = !upper;
    return upper ? k->shifted : k->normal;
}

}  // namespace

winexbox::IEventSink* SinkObject() { return &g_sink; }

void WakeThread(UserThread* t, DWORD qs_bits) {
    if (!t) return;
    t->changed_bits |= qs_bits;
    if (t->wake) SetEvent(t->wake);
}

LRESULT CallWindowProcInternal(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool ansi) {
    WindowObj* w = Win(hwnd);
    if (!w) return 0;
    if (w->flags & (kWinIsDesktop | kWinIsMsgParent)) return DefWindowProcInternal(w->handle, msg, wp, lp, ansi);
    if (w->proc.Empty()) return DefWindowProcInternal(w->handle, msg, wp, lp, ansi);
    abi::WinProcParams p{};
    FillWinProcParams(&p, w, msg, wp, lp, ansi, abi::WmCharMapSendMessage);
    UserThread& t = CurrentUserThread();
    if (t.info && t.info->recursion_count > kMaxRecursion) {
        WARN(user, "%p msg %04x: window procedure recursion too deep", hwnd, msg);
        return 0;
    }
    if (t.info) ++t.info->recursion_count;
    void* ret = nullptr;
    ULONG len = 0;
    NTSTATUS status;
    {
        UnlockScope unlock;
        status = UserModeCallback(abi::CbCallWinProc, &p, sizeof p, &ret, &len);
    }
    if (t.info && t.info->recursion_count) --t.info->recursion_count;
    LRESULT result = 0;
    if (ret && len >= sizeof(LRESULT)) std::memcpy(&result, ret, sizeof result);
    if (status) TRACE(user, "%p msg %04x: callback status %08lx", hwnd, msg, static_cast<unsigned long>(status));
    return result;
}

LRESULT SendMessageInternal(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool ansi) {
    if (hwnd == HWND_BROADCAST || hwnd == HWND_TOPMOST) {
        std::vector<HWND> tops;
        if (WindowObj* desk = Win(DesktopWindow()))
            for (HWND h : desk->children)
                if (WindowObj* w = Win(h); w && (!(w->style & WS_CHILD)) && (w->style & WS_POPUP || (w->style & WS_CAPTION) == WS_CAPTION || true))
                    tops.push_back(h);
        for (HWND h : tops)
            if (Win(h)) SendMessageInternal(h, msg, wp, lp, ansi);
        return TRUE;
    }
    WindowObj* w = Win(hwnd);
    if (!w) {
        SetLastErr(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    hwnd = w->handle;
    if (w->tid == GetCurrentThreadId() || (w->flags & (kWinIsDesktop | kWinIsMsgParent)))
        return CallWindowProcInternal(hwnd, msg, wp, lp, ansi);
    UserThread* target = FindUserThreadById(w->tid);
    if (!target || target->exiting) return 0;
    auto* m = new SentMsg{hwnd, msg, wp, lp, ansi};
    m->done_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m->sender_tid = GetCurrentThreadId();
    target->sent.push_back(m);
    WakeThread(target, QS_SENDMESSAGE);
    UserThread& self = CurrentUserThread();
    while (!m->done) {
        ProcessSentMessages(self);   // a thread waiting for a reply still answers its own incoming sends
        if (m->done) break;
        HANDLE hs[2] = {m->done_event, self.wake};
        UnlockScope unlock;
        WaitForMultipleObjects(2, hs, FALSE, INFINITE);
    }
    const LRESULT r = m->result;
    CloseHandle(m->done_event);
    delete m;
    return r;
}

bool PostThreadMessageInternal(UserThread* t, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool hardware) {
    if (!t) return false;
    State& s = S();
    QueuedMsg q;
    q.msg = MSG{hwnd, msg, wp, lp, Now(), s.cursor};
    q.hardware = hardware;
    t->posted.push_back(q);
    BumpStat(&Stats::messages_posted);
    WakeThread(t, QS_POSTMESSAGE);
    return true;
}

bool PostMessageInternal(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!hwnd) return PostThreadMessageInternal(&CurrentUserThread(), nullptr, msg, wp, lp);
    if (hwnd == HWND_BROADCAST || hwnd == HWND_TOPMOST) {
        if (WindowObj* desk = Win(DesktopWindow()))
            for (HWND h : std::vector<HWND>(desk->children))
                if (WindowObj* w = Win(h)) PostThreadMessageInternal(FindUserThreadById(w->tid), h, msg, wp, lp);
        return true;
    }
    WindowObj* w = WinChecked(hwnd);
    if (!w) return false;
    if (w->flags & (kWinIsDesktop | kWinIsMsgParent)) return true;   // nobody pumps the desktop: dropped like a full queue
    return PostThreadMessageInternal(FindUserThreadById(w->tid), w->handle, msg, wp, lp);
}

void KillWindowTimers(HWND hwnd) {
    State* s = StateIfAny();
    if (!s) return;
    for (UserThread* t : s->threads)
        t->timers.erase(std::remove_if(t->timers.begin(), t->timers.end(),
                                       [&](const Timer& tm) { return hwnd ? tm.hwnd == hwnd : !Win(tm.hwnd) && tm.hwnd; }),
                        t->timers.end());
}

}  // namespace detail

// ---- Host-facing --------------------------------------------------------------------------------
winexbox::IEventSink* InputSink() { return detail::SinkObject(); }

namespace sys {
using namespace detail;

BOOL WINAPI NtUserPeekMessage(MSG* msg, HWND hwnd, UINT first, UINT last, UINT flags) {
    PumpDriver();
    LockGuard lock;
    UserThread& t = CurrentUserThread();
    MSG m{};
    if (!PeekOnce(t, &m, hwnd, first, last, flags)) {
        FlushAllDirty();
        return FALSE;
    }
    if (msg) *msg = m;
    return TRUE;
}

BOOL WINAPI NtUserGetMessage(MSG* msg, HWND hwnd, UINT first, UINT last) {
    PumpDriver();
    LockGuard lock;
    UserThread& t = CurrentUserThread();
    if (hwnd && hwnd != reinterpret_cast<HWND>(-1) && !Win(hwnd)) {
        SetLastErr(ERROR_INVALID_WINDOW_HANDLE);
        return -1;
    }
    MSG m{};
    for (;;) {
        if (PeekOnce(t, &m, hwnd, first, last, PM_REMOVE)) break;
        FlushAllDirty();
        WaitForQueue(t, 0, nullptr, INFINITE, false, false);
    }
    if (msg) *msg = m;
    return m.message != WM_QUIT;
}

BOOL WINAPI NtUserPostMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    LockGuard lock;
    TRACE(user, "%p %04x %llx %llx", hwnd, msg, static_cast<unsigned long long>(wparam), static_cast<unsigned long long>(lparam));
    return PostMessageInternal(hwnd, msg, wparam, lparam);
}

BOOL WINAPI NtUserPostThreadMessage(DWORD thread, UINT msg, WPARAM wparam, LPARAM lparam) {
    LockGuard lock;
    UserThread* t = thread == GetCurrentThreadId() ? &CurrentUserThread() : FindUserThreadById(thread);
    if (!t) {
        SetLastErr(ERROR_INVALID_THREAD_ID);
        return FALSE;
    }
    return PostThreadMessageInternal(t, nullptr, msg, wparam, lparam);
}

BOOL WINAPI NtUserPostQuitMessage(INT exit_code) {
    LockGuard lock;
    UserThread& t = CurrentUserThread();
    t.quit = true;
    t.quit_code = exit_code;
    WakeThread(&t, QS_POSTMESSAGE);
    return TRUE;
}

LRESULT WINAPI NtUserMessageCall(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, void* result_info, DWORD type,
                                 BOOL ansi) {
    LockGuard lock;
    switch (type) {
    case abi::McDefWindowProc:
    case abi::McDesktopWindowProc:
        return DefWindowProcInternal(hwnd, msg, wparam, lparam, ansi != 0);
    case abi::McScrollBarWndProc:
        return ScrollBarWndProcInternal(hwnd, msg, wparam, lparam, ansi != 0);   // win_scroll.cpp
    case abi::McPopupMenuWndProc:
        return MenuPopupWndProc(hwnd, msg, wparam, lparam, ansi != 0);   // win_menu.cpp
    case abi::McCallWindowProc: {
        auto* p = static_cast<abi::WinProcParams*>(result_info);
        if (!p || !p->func) return FALSE;
        const WinProc wp = ResolveWinProc(p->func, ansi != 0);
        p->hwnd = FullHandle(hwnd);
        p->msg = msg;
        p->wparam = wparam;
        p->lparam = lparam;
        p->ansi = p->ansi_dst = ansi;
        p->mapping = abi::WmCharMapCallWindowProc;
        WindowObj* w = Win(hwnd);
        p->dpi_context = w ? w->dpi_context : abi::kDpiUnaware;
        if (ansi) {
            if (wp.a) p->ansi_dst = TRUE;
            else if (wp.w) p->ansi_dst = FALSE;
        } else {
            if (wp.w) p->ansi_dst = FALSE;
            else if (wp.a) p->ansi_dst = TRUE;
        }
        p->procA = wp.a ? wp.a : p->func;
        p->procW = wp.w ? wp.w : p->func;
        return TRUE;
    }
    case abi::McSendMessage: {
        auto* p = static_cast<abi::WinProcParams*>(result_info);
        WindowObj* w = Win(hwnd);
        if (p && w && w->tid == GetCurrentThreadId() && !(w->flags & (kWinIsDesktop | kWinIsMsgParent)) && !w->proc.Empty() &&
            (!CurrentUserThread().info || CurrentUserThread().info->recursion_count <= kMaxRecursion)) {
            // the caller dispatches it itself (user32 dispatch_send_message)
            FillWinProcParams(p, w, msg, wparam, lparam, ansi != 0, abi::WmCharMapSendMessage);
            return 0;
        }
        return SendMessageInternal(hwnd, msg, wparam, lparam, ansi != 0);
    }
    case abi::McSendMessageTimeout: {
        auto* p = static_cast<abi::SendMessageTimeoutParams*>(result_info);
        const bool valid = hwnd == HWND_BROADCAST || hwnd == HWND_TOPMOST || Win(hwnd);
        const LRESULT r = valid ? SendMessageInternal(hwnd, msg, wparam, lparam, ansi != 0) : 0;
        if (p) p->result = valid;
        if (!valid) SetLastErr(ERROR_INVALID_WINDOW_HANDLE);
        return r;
    }
    case abi::McSendNotifyMessage: {
        WindowObj* w = Win(hwnd);
        if (hwnd != HWND_BROADCAST && !w) {
            SetLastErr(ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }
        if (w && w->tid != GetCurrentThreadId()) return PostMessageInternal(hwnd, msg, wparam, lparam);
        SendMessageInternal(hwnd, msg, wparam, lparam, ansi != 0);
        return TRUE;
    }
    case abi::McSendMessageCallback: {
        auto* p = static_cast<abi::SendMessageCallbackParams*>(result_info);
        if (!Win(hwnd) && hwnd != HWND_BROADCAST) {
            SetLastErr(ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }
        const LRESULT r = SendMessageInternal(hwnd, msg, wparam, lparam, ansi != 0);
        if (p && p->callback) {
            abi::SendAsyncParams a{p->callback, FullHandle(hwnd), msg, p->data, r};
            UnlockScope unlock;
            UserModeCallback(abi::CbCallSendAsyncCallback, &a, sizeof a, nullptr, nullptr);
        }
        return TRUE;
    }
    case abi::McGetDispatchParams: {
        if (!hwnd) return FALSE;
        auto* p = static_cast<abi::WinProcParams*>(result_info);
        WindowObj* w = Win(hwnd);
        if (!w || !p || w->tid != GetCurrentThreadId() || w->proc.Empty()) {
            SetLastErr(w ? ERROR_MESSAGE_SYNC_ONLY : ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }
        FillWinProcParams(p, w, msg, wparam, lparam, ansi != 0, abi::WmCharMapDispatchMessage);
        BumpStat(&Stats::messages_dispatched);
        return TRUE;
    }
    case abi::McSpyGetMsgName: {
        if (result_info && wparam) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "msg 0x%04x", msg);
            std::strncpy(static_cast<char*>(result_info), buf, wparam);
            static_cast<char*>(result_info)[wparam - 1] = 0;
        }
        return 0;
    }
    case abi::McSpyEnter:
    case abi::McSpyExit:
        return 0;
    }
    FIXME(user, "NtUserMessageCall type %#lx (hwnd %p msg %04x)", type, hwnd, msg);
    SetLastErr(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

LRESULT WINAPI NtUserDispatchMessage(const MSG* msg) {
    LockGuard lock;
    if (!msg) return 0;
    WindowObj* w = Win(msg->hwnd);
    if (!w) {
        if (msg->hwnd) SetLastErr(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    if (w->tid != GetCurrentThreadId()) {
        SetLastErr(ERROR_MESSAGE_SYNC_ONLY);
        return 0;
    }
    const HWND hwnd = w->handle;
    BumpStat(&Stats::messages_dispatched);
    if ((msg->message == WM_TIMER || msg->message == abi::kWmSysTimer) && msg->lParam) {
        // a timer procedure: TIMERPROC(hwnd, msg, id, time)
        abi::WinProcParams p{};
        p.func = reinterpret_cast<WNDPROC>(msg->lParam);
        p.hwnd = hwnd; p.msg = msg->message; p.wparam = msg->wParam; p.lparam = static_cast<LPARAM>(Now());
        p.procA = p.procW = p.func;
        p.dpi_context = w->dpi_context;
        UnlockScope unlock;
        void* ret;
        ULONG len;
        UserModeCallback(abi::CbCallWinProc, &p, sizeof p, &ret, &len);
        return 0;
    }
    const LRESULT r = CallWindowProcInternal(hwnd, msg->message, msg->wParam, msg->lParam, false);
    if (msg->message == WM_PAINT) {
        // the window procedure did not validate: still deliver the frame / background erase
        if (WindowObj* cur = Win(hwnd); cur && (cur->nc_pending || cur->erase_pending)) SendEraseMessages(hwnd, nullptr, nullptr);
    }
    return r;
}

BOOL WINAPI NtUserTranslateMessage(const MSG* msg, UINT flags) {
    LockGuard lock;
    (void)flags;
    if (!msg || msg->message < WM_KEYFIRST || msg->message > WM_KEYLAST) return FALSE;
    if (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN) return TRUE;
    const int ch = CharOf(static_cast<UINT>(msg->wParam), S().input_key_state);
    if (ch) PostMessageInternal(msg->hwnd, msg->message == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, static_cast<WPARAM>(ch), msg->lParam);
    return TRUE;
}

UINT_PTR WINAPI NtUserSetTimer(HWND hwnd, UINT_PTR id, UINT timeout, TIMERPROC proc, ULONG tolerance) {
    LockGuard lock;
    (void)tolerance;
    UserThread* t;
    State& s = S();
    if (hwnd) {
        WindowObj* w = WinChecked(hwnd);
        if (!w) return 0;
        hwnd = w->handle;
        t = FindUserThreadById(w->tid);
        if (!t) return 0;
    } else {
        t = &CurrentUserThread();
        bool exists = false;
        for (const Timer& tm : t->timers)
            if (!tm.hwnd && tm.id == id) exists = true;
        if (!exists) {
            id = ++s.next_timer_id;
            if (s.next_timer_id >= 0xfffe) s.next_timer_id = 0x7fff;
        }
    }
    timeout = std::min(std::max(kUserTimerMinimum, timeout), kUserTimerMaximum);
    for (Timer& tm : t->timers)
        if (tm.hwnd == hwnd && tm.id == id && tm.msg == WM_TIMER) {
            tm.timeout = timeout;
            tm.next = GetTickCount64() + timeout;
            tm.proc = proc;
            WakeThread(t, 0);
            return id ? id : 1;
        }
    Timer tm;
    tm.hwnd = hwnd;
    tm.id = id;
    tm.msg = WM_TIMER;
    tm.timeout = timeout;
    tm.next = GetTickCount64() + timeout;
    tm.proc = proc;
    t->timers.push_back(tm);
    WakeThread(t, 0);   // a thread blocked in GetMessage recomputes its deadline
    TRACE(user, "%p id %llu every %u ms", hwnd, static_cast<unsigned long long>(id), timeout);
    return id ? id : 1;
}

namespace {
BOOL KillTimerImpl(HWND hwnd, UINT_PTR id, UINT msg) {
    LockGuard lock;
    if (hwnd && !WinChecked(hwnd)) return FALSE;
    hwnd = FullHandle(hwnd);
    State& s = S();
    for (UserThread* t : s.threads) {
        for (auto it = t->timers.begin(); it != t->timers.end(); ++it)
            if (it->hwnd == hwnd && it->id == id && it->msg == msg) {
                if (!hwnd && t->tid != GetCurrentThreadId()) continue;
                t->timers.erase(it);
                TRACE(user, "%p id %llu killed", hwnd, static_cast<unsigned long long>(id));
                return TRUE;
            }
    }
    SetLastErr(ERROR_INVALID_PARAMETER);
    return FALSE;
}
}  // namespace

BOOL WINAPI NtUserKillTimer(HWND hwnd, UINT_PTR id) { return KillTimerImpl(hwnd, id, WM_TIMER); }

UINT_PTR WINAPI NtUserSetSystemTimer(HWND hwnd, UINT_PTR id, UINT timeout) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w) return 0;
    UserThread* t = FindUserThreadById(w->tid);
    if (!t) return 0;
    timeout = std::min(std::max(kUserTimerMinimum, timeout), kUserTimerMaximum);
    for (Timer& tm : t->timers)
        if (tm.hwnd == w->handle && tm.id == id && tm.msg == abi::kWmSysTimer) {
            tm.timeout = timeout;
            tm.next = GetTickCount64() + timeout;
            return id;
        }
    Timer tm;
    tm.hwnd = w->handle;
    tm.id = id;
    tm.msg = abi::kWmSysTimer;
    tm.timeout = timeout;
    tm.next = GetTickCount64() + timeout;
    t->timers.push_back(tm);
    WakeThread(t, 0);
    return id;
}

BOOL WINAPI NtUserKillSystemTimer(HWND hwnd, UINT_PTR id) { return KillTimerImpl(hwnd, id, abi::kWmSysTimer); }

ULONG_PTR WINAPI NtUserGetThreadState(int cls) {
    if (cls == abi::TsInputState) PumpDriver();
    LockGuard lock;
    UserThread& t = CurrentUserThread();
    switch (cls) {
    case abi::TsFocusWindow: return reinterpret_cast<ULONG_PTR>(Win(t.focus) ? t.focus : nullptr);
    case abi::TsActiveWindow: return reinterpret_cast<ULONG_PTR>(Win(t.active) ? t.active : nullptr);
    case abi::TsCaptureWindow: return reinterpret_cast<ULONG_PTR>(Win(t.capture) ? t.capture : nullptr);
    case abi::TsDefaultImeWindow:
    case abi::TsDefaultInputContext: return 0;
    case abi::TsInputState: return QueueBits(t) & (QS_KEY | QS_MOUSEBUTTON);
    case abi::TsCursor: return winexbox::CursorId{} + (Driver() ? Driver()->CursorCurrent() : 0);
    case abi::TsExtraInfo: return t.info ? static_cast<ULONG_PTR>(t.info->message_extra) : 0;
    case abi::TsInSendMessage: return t.send_depth ? ISMEX_SEND : ISMEX_NOSEND;
    case abi::TsMessageTime: return t.info ? t.info->message_time : 0;
    case abi::TsIsForeground: return t.active && t.active == S().foreground;
    }
    FIXME(user, "NtUserGetThreadState(%d)", cls);
    return 0;
}

DWORD WINAPI NtUserGetQueueStatus(UINT flags) {
    PumpDriver();
    LockGuard lock;
    UserThread& t = CurrentUserThread();
    const UINT now = QueueBits(t);
    const UINT changed = t.changed_bits & now;
    t.changed_bits &= ~flags;
    return MAKELONG(changed & flags, now & flags);
}

BOOL WINAPI NtUserWaitMessage() {
    PumpDriver();
    LockGuard lock;
    UserThread& t = CurrentUserThread();
    t.changed_bits = 0;
    while (!t.changed_bits && !(QueueBits(t) & (QS_ALLINPUT & ~QS_PAINT))) {
        FlushAllDirty();
        WaitForQueue(t, 0, nullptr, INFINITE, false, false);
        if (FindPaintWindow(&t)) break;
    }
    return TRUE;
}

DWORD WINAPI NtUserMsgWaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, DWORD timeout, DWORD mask,
                                               DWORD flags) {
    PumpDriver();
    LockGuard lock;
    UserThread& t = CurrentUserThread();
    if (count > MAXIMUM_WAIT_OBJECTS - 2) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    ProcessSentMessages(t);
    const ULONGLONG start = GetTickCount64();
    const bool input_available = (flags & MWMO_INPUTAVAILABLE) != 0;
    if (input_available ? (QueueBits(t) & mask) : (t.changed_bits & QueueBits(t) & mask)) return WAIT_OBJECT_0 + count;
    t.changed_bits = 0;
    for (;;) {
        FlushAllDirty();
        DWORD left = INFINITE;
        if (timeout != INFINITE) {
            const ULONGLONG el = GetTickCount64() - start;
            left = el >= timeout ? 0 : static_cast<DWORD>(timeout - el);
        }
        const DWORD r = WaitForQueue(t, count, handles, left, (flags & MWMO_ALERTABLE) != 0, (flags & MWMO_WAITALL) != 0);
        if (r == WAIT_IO_COMPLETION || r == WAIT_FAILED) return r;
        if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + count) return r;
        if (r < WAIT_OBJECT_0 + count) return r;
        ProcessSentMessages(t);
        if (QueueBits(t) & mask) return WAIT_OBJECT_0 + count;
        if (r == WAIT_TIMEOUT) {
            if (timeout != INFINITE && GetTickCount64() - start >= timeout) return WAIT_TIMEOUT;
        }
    }
}

DWORD WINAPI NtUserWaitForInputIdle(HANDLE process, DWORD timeout, BOOL wow) {
    (void)process; (void)timeout; (void)wow;
    return 0;   // every GUI thread of this process waits for input as soon as it pumps
}

// GetKeyState and GetKeyboardState report the state as of the last input message the application took off its
// queue (a handler for WM_KEYDOWN sees that key down), GetAsyncKeyState the hardware's.
SHORT WINAPI NtUserGetKeyState(INT vkey) {
    LockGuard lock;
    if (vkey < 0 || vkey > 255) return 0;
    const BYTE k = S().input_key_state[vkey];
    return static_cast<SHORT>(((k & 0x80) ? 0xff80 : 0) | (k & 0x01));
}

SHORT WINAPI NtUserGetAsyncKeyState(INT key) {
    PumpDriver();
    LockGuard lock;
    if (key < 0 || key > 255) return 0;
    return (S().key_state[key] & 0x80) ? static_cast<SHORT>(0x8000) : 0;
}

BOOL WINAPI NtUserGetKeyboardState(BYTE* state) {
    LockGuard lock;
    if (!state) return FALSE;
    std::memcpy(state, S().input_key_state, 256);
    return TRUE;
}

BOOL WINAPI NtUserSetKeyboardState(BYTE* state) {
    LockGuard lock;
    if (!state) return FALSE;
    std::memcpy(S().input_key_state, state, 256);
    return TRUE;
}

BOOL WINAPI NtUserGetCursorPos(POINT* pt) {
    LockGuard lock;
    if (!pt) return FALSE;
    winexbox::Point p{};
    if (Driver() && Driver()->CursorGetPos(&p)) S().cursor = POINT{p.x, p.y};
    *pt = S().cursor;
    return TRUE;
}

BOOL WINAPI NtUserSetCursorPos(INT x, INT y) {
    LockGuard lock;
    if (Driver()) Driver()->CursorSetPos(x, y);
    S().cursor = POINT{x, y};
    return TRUE;
}

HCURSOR WINAPI NtUserSetCursor(HCURSOR cursor) {
    LockGuard lock;
    winexbox::IWinexboxDriver* d = Driver();
    const HCURSOR prev = d ? reinterpret_cast<HCURSOR>(d->CursorCurrent()) : nullptr;
    if (d) d->CursorSet(0, reinterpret_cast<winexbox::CursorId>(cursor));
    return prev;
}

HCURSOR WINAPI NtUserGetCursor() {
    LockGuard lock;
    winexbox::IWinexboxDriver* d = Driver();
    return d ? reinterpret_cast<HCURSOR>(d->CursorCurrent()) : nullptr;
}

namespace { LONG g_cursor_count = 0; }

INT WINAPI NtUserShowCursor(BOOL show) {
    return show ? InterlockedIncrement(&g_cursor_count) : InterlockedDecrement(&g_cursor_count);
}

BOOL WINAPI NtUserClipCursor(const RECT* rect) {
    LockGuard lock;
    winexbox::IWinexboxDriver* d = Driver();
    if (!d) return FALSE;
    if (!rect) return d->CursorClip(nullptr, true);
    const winexbox::Rect r{rect->left, rect->top, rect->right, rect->bottom};
    return d->CursorClip(&r, false);
}

BOOL WINAPI NtUserGetClipCursor(RECT* rect) {
    LockGuard lock;
    if (!rect) return FALSE;
    ScreenRect(rect);
    return TRUE;
}

HKL WINAPI NtUserGetKeyboardLayout(DWORD thread_id) {
    (void)thread_id;
    return reinterpret_cast<HKL>(static_cast<ULONG_PTR>(0x04090409));
}

UINT WINAPI NtUserGetKeyboardLayoutList(INT size, HKL* layouts) {
    if (layouts && size >= 1) layouts[0] = reinterpret_cast<HKL>(static_cast<ULONG_PTR>(0x04090409));
    return 1;
}

BOOL WINAPI NtUserGetKeyboardLayoutName(WCHAR* name) {
    if (!name) return FALSE;
    wcscpy(name, L"00000409");
    return TRUE;
}

HKL WINAPI NtUserActivateKeyboardLayout(HKL layout, UINT flags) {
    (void)flags;
    if (HandleToUlong(layout) != 0x04090409 && layout != reinterpret_cast<HKL>(HKL_NEXT) && layout != reinterpret_cast<HKL>(HKL_PREV)) {
        FIXME(user, "keyboard layout %p: only the US layout exists", layout);
        SetLastErr(ERROR_CALL_NOT_IMPLEMENTED);
        return nullptr;
    }
    return reinterpret_cast<HKL>(static_cast<ULONG_PTR>(0x04090409));
}

UINT WINAPI NtUserMapVirtualKeyEx(UINT code, UINT type, HKL layout) {
    (void)layout;
    switch (type) {
    case MAPVK_VK_TO_VSC:
    case MAPVK_VK_TO_VSC_EX: {
        if (code == VK_SHIFT) code = VK_LSHIFT;
        return ScanOf(code);
    }
    case MAPVK_VSC_TO_VK:
    case MAPVK_VSC_TO_VK_EX: {
        const UINT vk = VkOfScan(code);
        if (type == MAPVK_VSC_TO_VK) {   // the side-less key
            if (vk == VK_LSHIFT || vk == VK_RSHIFT) return VK_SHIFT;
            if (vk == VK_LCONTROL || vk == VK_RCONTROL) return VK_CONTROL;
            if (vk == VK_LMENU || vk == VK_RMENU) return VK_MENU;
            return vk;
        }
        // _EX: the table holds the side-less keys for the shared scan codes, the left one is the answer
        if (vk == VK_SHIFT) return VK_LSHIFT;
        if (vk == VK_CONTROL) return VK_LCONTROL;
        if (vk == VK_MENU) return VK_LMENU;
        return vk;
    }
    case MAPVK_VK_TO_CHAR: {
        const KeyMap* k = KeyByVk(code);
        if (!k || !k->normal) return 0;
        return (code >= 'A' && code <= 'Z') ? code : static_cast<UINT>(static_cast<unsigned char>(k->normal));
    }
    }
    return 0;
}

INT WINAPI NtUserToUnicodeEx(UINT virt, UINT scan, const BYTE* state, WCHAR* str, int size, UINT flags, HKL layout) {
    (void)layout; (void)flags;
    if (!state || !str || size <= 0) return 0;
    if (scan & 0x8000) return 0;   // key up
    const int ch = CharOf(virt, state);
    if (!ch) return 0;
    str[0] = static_cast<WCHAR>(ch);
    if (size > 1) str[1] = 0;
    return 1;
}

WORD WINAPI NtUserVkKeyScanEx(WCHAR chr, HKL layout) {
    (void)layout;
    for (const KeyMap& k : kKeys) {
        if (k.normal && static_cast<WCHAR>(static_cast<unsigned char>(k.normal)) == chr) return k.vk;
        if (k.shifted && static_cast<WCHAR>(static_cast<unsigned char>(k.shifted)) == chr) return static_cast<WORD>(k.vk | 0x100);
    }
    if (chr >= 1 && chr <= 26) return static_cast<WORD>(('A' + chr - 1) | 0x200);
    return 0xffff;
}

INT WINAPI NtUserGetKeyNameText(LONG lparam, WCHAR* buffer, INT size) {
    if (!buffer || size <= 0) return 0;
    const UINT vk = VkOfScan((lparam >> 16) & 0xff);
    wchar_t name[32] = {};
    const KeyMap* k = KeyByVk(vk);
    if (vk >= VK_F1 && vk <= VK_F12) swprintf(name, 32, L"F%u", vk - VK_F1 + 1);
    else if (k && k->normal > ' ') name[0] = static_cast<wchar_t>(k->shifted >= 'A' && k->shifted <= 'Z' ? k->shifted : k->normal);
    else {
        switch (vk) {
        case VK_SPACE: wcscpy(name, L"Space"); break;
        case VK_RETURN: wcscpy(name, L"Enter"); break;
        case VK_ESCAPE: wcscpy(name, L"Esc"); break;
        case VK_BACK: wcscpy(name, L"Backspace"); break;
        case VK_TAB: wcscpy(name, L"Tab"); break;
        case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: wcscpy(name, L"Shift"); break;
        case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: wcscpy(name, L"Ctrl"); break;
        case VK_LMENU: case VK_RMENU: case VK_MENU: wcscpy(name, L"Alt"); break;
        default: return 0;
        }
    }
    const INT n = std::min<INT>(size - 1, static_cast<INT>(wcslen(name)));
    std::wmemcpy(buffer, name, static_cast<std::size_t>(n));
    buffer[n] = 0;
    return n;
}

UINT WINAPI NtUserSendInput(UINT count, INPUT* inputs, int size) {
    LockGuard lock;
    if (!inputs || size != static_cast<int>(sizeof(INPUT))) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return 0;
    }
    State& s = S();
    RECT screen;
    ScreenRect(&screen);
    for (UINT i = 0; i < count; ++i) {
        const INPUT& in = inputs[i];
        winexbox::InputEvent ev;
        if (in.type == INPUT_KEYBOARD) {
            const DWORD time = in.ki.time ? in.ki.time : Now();
            if (in.ki.dwFlags & KEYEVENTF_UNICODE) {
                if (!(in.ki.dwFlags & KEYEVENTF_KEYUP)) {
                    UserThread* t = ForegroundThread();
                    HWND f = t ? (Win(t->focus) ? t->focus : t->active) : nullptr;
                    if (Win(f)) {
                        QueuedMsg q;
                        q.msg = MSG{f, WM_CHAR, in.ki.wScan, 1, time, s.cursor};
                        QueueInput(t, q);
                    } else {
                        TRACE(user, "SendInput: unicode character %04x dropped: no focus or active window", in.ki.wScan);
                    }
                }
                continue;
            }
            ev.kind = (in.ki.dwFlags & KEYEVENTF_KEYUP) ? winexbox::InputKind::KeyUp : winexbox::InputKind::KeyDown;
            ev.vk = in.ki.wVk;
            ev.scan = in.ki.wScan;
            ev.flags = (in.ki.dwFlags & KEYEVENTF_EXTENDEDKEY) ? winexbox::kInputExtendedKey : 0;
            RouteInput(ev, time, true);
        } else if (in.type == INPUT_MOUSE) {
            const DWORD f = in.mi.dwFlags;
            const DWORD time = in.mi.time ? in.mi.time : Now();
            POINT p = s.cursor;
            if (f & MOUSEEVENTF_MOVE) {
                if (f & MOUSEEVENTF_ABSOLUTE) {
                    p.x = static_cast<LONG>(static_cast<long long>(in.mi.dx) * (screen.right - screen.left) / 65536) + screen.left;
                    p.y = static_cast<LONG>(static_cast<long long>(in.mi.dy) * (screen.bottom - screen.top) / 65536) + screen.top;
                } else {
                    p.x += in.mi.dx;
                    p.y += in.mi.dy;
                }
                ev.kind = winexbox::InputKind::MouseMove;
                ev.x = p.x; ev.y = p.y;
                RouteInput(ev, time, true);
                p = s.cursor;
            }
            ev.x = p.x; ev.y = p.y;
            auto button = [&](DWORD down_flag, DWORD up_flag, winexbox::MouseButton b) {
                if (f & down_flag) { ev.kind = winexbox::InputKind::MouseButtonDown; ev.button = b; RouteInput(ev, time, true); }
                if (f & up_flag) { ev.kind = winexbox::InputKind::MouseButtonUp; ev.button = b; RouteInput(ev, time, true); }
            };
            button(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, winexbox::MouseButton::Left);
            button(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, winexbox::MouseButton::Right);
            button(MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, winexbox::MouseButton::Middle);
            button(MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP,
                   in.mi.mouseData == XBUTTON2 ? winexbox::MouseButton::X2 : winexbox::MouseButton::X1);
            if (f & (MOUSEEVENTF_WHEEL | MOUSEEVENTF_HWHEEL)) {
                ev.kind = winexbox::InputKind::MouseWheel;
                ev.wheel_delta = static_cast<std::int32_t>(static_cast<int>(in.mi.mouseData));
                ev.flags = (f & MOUSEEVENTF_HWHEEL) ? winexbox::kInputHorizontal : 0;
                RouteInput(ev, time, true);
            }
        } else {
            FIXME(user, "SendInput type %lu", in.type);
        }
    }
    return count;
}

BOOL WINAPI NtUserTrackMouseEvent(TRACKMOUSEEVENT* info) {
    if (!info || info->cbSize != sizeof(*info)) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    static volatile LONG reported = 0;   // plain flag
    if (!InterlockedExchange(&reported, 1)) FIXME(user, "TrackMouseEvent: WM_MOUSELEAVE / WM_MOUSEHOVER are never generated");
    if (info->dwFlags & TME_QUERY) {
        info->dwFlags = 0;
        info->hwndTrack = nullptr;
    }
    return TRUE;
}

UINT WINAPI NtUserGetDoubleClickTime() { return 500; }

BOOL WINAPI NtUserMessageBeep(UINT type) {
    static volatile LONG reported = 0;   // plain flag
    if (!InterlockedExchange(&reported, 1)) FIXME(user, "MessageBeep(%u): no sound output", type);
    return TRUE;
}

// ---- Caret: position and visibility are tracked; it is not drawn yet --------------------------
namespace {
struct CaretState { HWND hwnd; POINT pos; SIZE size; int hidden; };
CaretState g_caret{};
void NoteCaret() {
    static volatile LONG reported = 0;   // plain flag
    if (!InterlockedExchange(&reported, 1)) FIXME(user, "the caret is tracked but not drawn");
}
}  // namespace

BOOL WINAPI NtUserCreateCaret(HWND hwnd, HBITMAP bitmap, int width, int height) {
    LockGuard lock;
    (void)bitmap;
    if (!WinChecked(hwnd)) return FALSE;
    NoteCaret();
    g_caret = CaretState{FullHandle(hwnd), POINT{0, 0}, SIZE{width ? width : 1, height ? height : 1}, 1};
    return TRUE;
}

BOOL WINAPI NtUserDestroyCaret() {
    LockGuard lock;
    if (!g_caret.hwnd) return FALSE;
    g_caret = CaretState{};
    return TRUE;
}

BOOL WINAPI NtUserShowCaret(HWND hwnd) {
    LockGuard lock;
    if (!g_caret.hwnd || (hwnd && FullHandle(hwnd) != g_caret.hwnd)) return FALSE;
    if (g_caret.hidden > 0) --g_caret.hidden;
    return TRUE;
}

BOOL WINAPI NtUserHideCaret(HWND hwnd) {
    LockGuard lock;
    if (!g_caret.hwnd || (hwnd && FullHandle(hwnd) != g_caret.hwnd)) return FALSE;
    ++g_caret.hidden;
    return TRUE;
}

BOOL WINAPI NtUserSetCaretPos(INT x, INT y) {
    LockGuard lock;
    if (!g_caret.hwnd) return FALSE;
    g_caret.pos = POINT{x, y};
    return TRUE;
}

BOOL WINAPI NtUserGetCaretPos(POINT* point) {
    LockGuard lock;
    if (!point || !g_caret.hwnd) return FALSE;
    *point = g_caret.pos;
    return TRUE;
}

UINT WINAPI NtUserGetCaretBlinkTime() { return 530; }

}  // namespace sys
}  // namespace win10xbox::user
