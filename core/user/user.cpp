// core/user: module state, the host-facing API (user.h), per-thread records,
// user-mode callbacks and the registration of every NtUser*/NtGdi* syscall
// this module implements (UserImpls). The window manager, message queues, GDI
// objects and drawing live in their own files (docs/GUI.md lists them).
#include "user_internal.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

WIN10XBOX_DECLARE_CHANNEL(user);
WIN10XBOX_DECLARE_CHANNEL(gdi);

namespace win10xbox::user {
namespace detail {
namespace {

// Namespace scope, trivially initialised (see dispatcher.cpp: no function-local
// statics on anything a guest thread could reach first).
SRWLOCK g_lock = SRWLOCK_INIT;
winexbox::IWinexboxDriver* g_driver = nullptr;
volatile LONG64 g_frame_serial = 0;
Stats g_stats;

constexpr NTSTATUS kStatusNoCallbackActive = static_cast<NTSTATUS>(0xC0000240);

std::uint8_t* CurrentTeb() { return reinterpret_cast<std::uint8_t*>(__readgsqword(0x30)); }
UserThread*& ThreadSlot() {
    // TEB.Win32ClientInfo[61]: the last of the 62 slots; the first ones are user32's ntuser_thread_info.
    return *reinterpret_cast<UserThread**>(CurrentTeb() + dispatch::detail::teb::Win32ClientInfo +
                                           8 * (dispatch::detail::teb::Win32ClientInfoCount - 1));
}

}  // namespace

SRWLOCK& UserLock() { return g_lock; }

UserThread* FindUserThread() { return ThreadSlot(); }

UserThread& CurrentUserThread() {
    UserThread*& slot = ThreadSlot();
    if (!slot) {
        slot = new UserThread;
        slot->tid = GetCurrentThreadId();
        TRACE(user, "thread %lu: user thread record %p", slot->tid, slot);
    }
    return *slot;
}

void NoteUnimplemented(const char* name) {
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_stats.unimplemented));
    FIXME(user, "%s: not implemented", name);
}

// ---- Callbacks ----------------------------------------------------------------------

using KernelCallbackFn = NTSTATUS(NTAPI*)(void* args, ULONG len);

NTSTATUS UserModeCallback(ULONG id, const void* args, ULONG len, void** ret_ptr, ULONG* ret_len) {
    using dispatch::detail::peb::KernelCallbackTable;
    if (ret_ptr) *ret_ptr = nullptr;
    if (ret_len) *ret_len = 0;
    auto* table = dispatch::detail::PebField<KernelCallbackFn*>(KernelCallbackTable);
    if (!table || !table[id]) {
        ERR(user, "callback %lu: no KernelCallbackTable entry (user32 not initialised?)", id);
        return dispatch::detail::st::NotImplemented;
    }
    UserThread& t = CurrentUserThread();
    const std::uint32_t depth = t.callback_depth++;
    if (t.callbacks.size() <= depth) t.callbacks.resize(depth + 1);
    CallbackFrame& frame = t.callbacks[depth];
    frame.returned = false;
    frame.status = 0;
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_stats.callbacks));
    // Everything the transfer needs is resolved before the view switch (CallInGuestView's contract).
    const KernelCallbackFn fn = table[id];
    NTSTATUS status = dispatch::detail::CallInGuestView(fn, const_cast<void*>(args), len);
    --t.callback_depth;
    if (frame.returned) {
        status = frame.status;
        if (ret_ptr) *ret_ptr = frame.result.data();
        if (ret_len) *ret_len = static_cast<ULONG>(frame.result.size());
    }
    return status;
}

NTSTATUS NTAPI User_NtCallbackReturn(void* result, ULONG size, NTSTATUS status) {
    UserThread* t = FindUserThread();
    if (!t || !t->callback_depth) return kStatusNoCallbackActive;
    CallbackFrame& frame = t->callbacks[t->callback_depth - 1];
    frame.result.assign(static_cast<const std::uint8_t*>(result), static_cast<const std::uint8_t*>(result) + (result ? size : 0));
    frame.status = status;
    frame.returned = true;
    return status;
}

}  // namespace detail

// ---- Host-facing API -------------------------------------------------------------

void SetDisplayDriver(winexbox::IWinexboxDriver* driver) { detail::g_driver = driver; }

winexbox::IWinexboxDriver* DisplayDriver() {
    // core cannot link winexbox (winexbox links core), so no on-demand fallback here: a host that
    // runs GUI guests sets one (host-win32 and host-uwp both do). Null means "no display".
    if (!detail::g_driver) {
        static volatile LONG reported = 0;   // plain flag, no magic static
        if (!InterlockedExchange(&reported, 1)) ERR(user, "no display driver set by the host: GUI calls will fail");
    }
    return detail::g_driver;
}

bool ComposeFrame(std::uint32_t* bgra, std::int32_t width, std::int32_t height, std::int32_t stride_bytes) {
    if (detail::NativeGui()) return detail::NativeComposeFrame(bgra, width, height, stride_bytes);   // route B
    // Filled in by the window manager (win_paint.cpp): until then there is nothing to compose.
    (void)bgra; (void)width; (void)height; (void)stride_bytes;
    return false;
}

std::uint64_t FrameSerial() {
    return static_cast<std::uint64_t>(detail::g_frame_serial) + detail::NativeFrameSerial();   // + route B's
}

void ScreenSize(std::int32_t* width, std::int32_t* height) {
    if (width) *width = 0;
    if (height) *height = 0;
    if (detail::NativeGui()) { detail::NativeScreenSize(width, height); return; }   // route B: the real screen
    if (!detail::g_driver) return;
    const winexbox::DisplayConfig dc = detail::g_driver->QueryDisplay();
    for (const winexbox::MonitorInfo& m : dc.monitors) {
        if (!m.primary && dc.monitors.size() > 1) continue;
        if (width) *width = m.monitor.Width();
        if (height) *height = m.monitor.Height();
        return;
    }
}

bool WriteScreenshot(const std::wstring& path) {
    std::int32_t w = 0, h = 0;
    ScreenSize(&w, &h);
    if (w <= 0 || h <= 0) {
        WARN(user, "screenshot %ls: no screen (driver not initialised)", path.c_str());
        return false;
    }
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(w) * h);
    if (!ComposeFrame(pixels.data(), w, h, w * 4)) {
        WARN(user, "screenshot %ls: nothing to compose", path.c_str());
        return false;
    }
    // 32-bit top-down BMP (negative height), BI_RGB: what every viewer opens.
    const std::uint32_t image_bytes = static_cast<std::uint32_t>(pixels.size() * 4);
    std::uint8_t hdr[14 + 40] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    auto put32 = [&](std::size_t off, std::uint32_t v) { std::memcpy(hdr + off, &v, 4); };
    auto put16 = [&](std::size_t off, std::uint16_t v) { std::memcpy(hdr + off, &v, 2); };
    put32(2, 14 + 40 + image_bytes);
    put32(10, 14 + 40);
    put32(14, 40);
    put32(18, static_cast<std::uint32_t>(w));
    put32(22, static_cast<std::uint32_t>(-h));
    put16(26, 1);
    put16(28, 32);
    put32(30, 0);
    put32(34, image_bytes);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        WARN(user, "screenshot %ls: cannot create (errno %d)", path.c_str(), errno);
        return false;
    }
    const bool ok = std::fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr &&
                    std::fwrite(pixels.data(), 1, image_bytes, f) == image_bytes;
    std::fclose(f);
    TRACE(user, "screenshot %ls: %dx%d%s", path.c_str(), w, h, ok ? "" : " (write failed)");
    return ok;
}

Stats GetStats() { return detail::g_stats; }

}  // namespace win10xbox::user

// ---- Registration ------------------------------------------------------------------
namespace win10xbox::dispatch::detail {

#define W10X_USER_IMPL(sys, fn) {#sys, reinterpret_cast<void*>(&fn)}

const ImplDef* UserImpls() {
    static const ImplDef k[] = {
        W10X_USER_IMPL(NtCallbackReturn, user::detail::User_NtCallbackReturn),
        {nullptr, nullptr},
    };
    return k;
}

}  // namespace win10xbox::dispatch::detail
