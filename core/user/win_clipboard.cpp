// core/user clipboard: the NtUser clipboard syscalls Wine's PE user32 calls, in-process.
//
// This is wineserver's server/clipboard.c (the format list, owner/viewer/listeners, the sequence
// number, synthesized formats) fused with win32u's dlls/win32u/clipboard.c (the per-process cache of
// the HANDLEs the application passed to SetClipboardData), because there is one process and no
// server here. The protocol with user32 is Wine's, not Windows': user32's SetClipboardData hands
// the marshalled bytes in set_clipboard_params, and GetClipboardData loops on get_clipboard_params
// (data_size == ~0 means "unmarshal params->data and cache it with a cache_only SetClipboardData").
//
// Differences from Wine, on purpose:
//  * text synthesis (CF_TEXT / CF_OEMTEXT / CF_UNICODETEXT and CF_LOCALE) is done here, at
//    GetClipboardData time, instead of through the NtUserRenderSynthesizedFormat callback.
//  * Bitmap / DIB / metafile synthesis is NOT done (logged once when it would have applied).
//  * The window-destroy and thread-exit hooks of the server are replaced by a sweep at the start of
//    every clipboard syscall that drops state belonging to windows / threads that are gone.
//  * Viewer and listener notifications are posted (WM_DRAWCLIPBOARD, WM_CLIPBOARDUPDATE).
#include "user_internal.h"
#include "user_syscalls.h"

#include <algorithm>
#include <cstring>

#ifndef WM_CLIPBOARDUPDATE
#define WM_CLIPBOARDUPDATE 0x031D
#endif

namespace win10xbox::user {
namespace detail {
namespace {

// NTSTATUS in the "Win32 error" facility, what wineserver's set_win32_error produces and
// RtlNtStatusToDosError turns back into the error code.
constexpr NTSTATUS Win32Status(DWORD err) { return static_cast<NTSTATUS>(0xC0010000u | err); }

struct Fmt {
    UINT id = 0;
    UINT from = 0;        // synthesized from this format (data rendered on demand)
    UINT seqno = 0;
    std::vector<std::uint8_t> data;
    bool has_data = false;
    bool Ready() const { return has_data && !data.empty(); }
};
struct Cache {            // win32u's cached_format: the application's own handle for a format
    UINT format = 0;
    UINT seqno = 0;
    HANDLE handle = nullptr;
};
struct Clip {
    std::vector<Fmt> formats;
    std::vector<Cache> cache;
    std::vector<Cache> to_free;   // stale entries, freed by the next Empty / Open
    DWORD open_tid = 0;
    HWND open_win = nullptr;
    HWND owner = nullptr;
    HWND viewer = nullptr;
    UINT seqno = 0;
    UINT open_seqno = 0;
    int rendering = 0;
    LCID lcid = 0;
    std::vector<HWND> listeners;
    bool warned_unsynth = false;
};

// Trivially initialised; allocated under the lock (see dispatcher.cpp on init guards).
Clip* g_clip = nullptr;

Clip& C() {
    if (!g_clip) g_clip = new Clip;
    return *g_clip;
}

Fmt* FindFmt(Clip& c, UINT id) {
    for (Fmt& f : c.formats)
        if (f.id == id) return &f;
    return nullptr;
}
bool HasFmt(Clip& c, UINT id) { return FindFmt(c, id) != nullptr; }

Fmt* AddFmt(Clip& c, UINT id) {
    c.formats.emplace_back();
    c.formats.back().id = id;
    return &c.formats.back();
}

Cache* FindCache(Clip& c, UINT format) {
    for (Cache& e : c.cache)
        if (e.format == format) return &e;
    return nullptr;
}

void EraseCache(Clip& c, UINT format) {
    c.cache.erase(std::remove_if(c.cache.begin(), c.cache.end(), [&](const Cache& e) { return e.format == format; }),
                  c.cache.end());
}

struct FreeCachedDataParams {   // include/ntuser.h: struct free_cached_data_params
    UINT format;
    HANDLE handle;
};

// win32u free_cached_data for each entry. Lock held on entry and exit; dropped around the callbacks.
void FreeCache(std::vector<Cache> list) {
    for (const Cache& e : list) {
        if (!e.handle) continue;
        switch (e.format) {
        case CF_BITMAP:
        case CF_PALETTE:
            if (GdiObject* o = GdiGet(e.handle)) o->system = false;
            GdiDelete(e.handle);
            break;
        case CF_DSPBITMAP:
            GdiDelete(e.handle);
            break;
        default: {
            FreeCachedDataParams p{e.format, e.handle};
            void* ret = nullptr;
            ULONG len = 0;
            UnlockScope unlock;
            UserModeCallback(abi::CbFreeCachedClipboardData, &p, sizeof p, &ret, &len);
            break;
        }
        }
    }
}

void DrainToFree(Clip& c, std::vector<Cache>* out) {
    out->insert(out->end(), c.to_free.begin(), c.to_free.end());
    c.to_free.clear();
}

// ---- notifications (server: notify_listeners) ------------------------------------------------

HWND NotifyListeners(Clip& c) {
    for (HWND l : c.listeners)
        if (Win(l)) PostMessageInternal(l, WM_CLIPBOARDUPDATE, 0, 0);
    return c.viewer;
}

void NotifyViewer(Clip& c, HWND viewer) {
    if (viewer && Win(viewer)) PostMessageInternal(viewer, WM_DRAWCLIPBOARD, reinterpret_cast<WPARAM>(c.owner), 0);
}

// ---- text conversion for synthesized formats ----------------------------------------------------

UINT TextCodePage(UINT format) { return format == CF_OEMTEXT ? CP_OEMCP : CP_ACP; }

bool IsText(UINT f) { return f == CF_TEXT || f == CF_OEMTEXT || f == CF_UNICODETEXT; }

// Converts `src` (a text format) to the text format `to`. Empty on failure.
std::vector<std::uint8_t> ConvertText(const Fmt& src, UINT to) {
    std::vector<std::uint8_t> out;
    std::vector<WCHAR> wide;
    const WCHAR* w = nullptr;
    int wlen = 0;
    if (src.id == CF_UNICODETEXT) {
        wlen = static_cast<int>(src.data.size() / sizeof(WCHAR));
        wide.resize(wlen + 1);
        std::memcpy(wide.data(), src.data.data(), wlen * sizeof(WCHAR));
        w = wide.data();
    } else {
        const UINT cp = TextCodePage(src.id);
        const int n = MultiByteToWideChar(cp, 0, reinterpret_cast<const char*>(src.data.data()),
                                          static_cast<int>(src.data.size()), nullptr, 0);
        if (n <= 0) return out;
        wide.resize(n + 1);
        MultiByteToWideChar(cp, 0, reinterpret_cast<const char*>(src.data.data()), static_cast<int>(src.data.size()),
                            wide.data(), n);
        w = wide.data();
        wlen = n;
    }
    if (to == CF_UNICODETEXT) {
        out.resize(wlen * sizeof(WCHAR));
        std::memcpy(out.data(), w, out.size());
        return out;
    }
    const UINT cp = TextCodePage(to);
    const int n = WideCharToMultiByte(cp, 0, w, wlen, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return out;
    out.resize(n);
    WideCharToMultiByte(cp, 0, w, wlen, reinterpret_cast<char*>(out.data()), n, nullptr, nullptr);
    return out;
}

constexpr UINT kSynthTable[][3] = {
    {CF_TEXT, CF_OEMTEXT, CF_UNICODETEXT},
    {CF_OEMTEXT, CF_UNICODETEXT, CF_TEXT},
    {CF_UNICODETEXT, CF_TEXT, CF_OEMTEXT},
};

// server: synthesize_formats (text only). Returns the number of formats added.
int SynthesizeFormats(Clip& c) {
    int total = 0;
    if (!HasFmt(c, CF_LOCALE) && (HasFmt(c, CF_TEXT) || HasFmt(c, CF_OEMTEXT) || HasFmt(c, CF_UNICODETEXT))) {
        Fmt* f = AddFmt(c, CF_LOCALE);
        f->seqno = c.seqno++;
        f->data.resize(sizeof(LCID));
        std::memcpy(f->data.data(), &c.lcid, sizeof(LCID));
        f->has_data = true;
    }
    for (const auto& row : kSynthTable) {
        if (HasFmt(c, row[0])) continue;
        UINT from;
        if (HasFmt(c, row[1])) from = row[1];
        else if (HasFmt(c, row[2])) from = row[2];
        else continue;
        Fmt* f = AddFmt(c, row[0]);
        f->from = from;
        f->seqno = c.seqno;
        ++total;
    }
    if (!c.warned_unsynth &&
        (HasFmt(c, CF_BITMAP) || HasFmt(c, CF_DIB) || HasFmt(c, CF_DIBV5) || HasFmt(c, CF_METAFILEPICT) ||
         HasFmt(c, CF_ENHMETAFILE))) {
        c.warned_unsynth = true;
        FIXME(user, "clipboard: bitmap/DIB/metafile format synthesis is not implemented");
    }
    return total;
}

// server: close_clipboard
void CloseInternal(Clip& c) {
    c.open_win = nullptr;
    c.open_tid = 0;
    if (c.seqno == c.open_seqno) return;   // unchanged
    if (SynthesizeFormats(c)) c.seqno++;
    HWND viewer = NotifyListeners(c);
    NotifyViewer(c, viewer);
}

// server: release_clipboard (the owner is gone)
void ReleaseOwner(Clip& c) {
    c.owner = nullptr;
    bool changed = false;
    for (std::size_t i = 0; i < c.formats.size();) {
        Fmt& f = c.formats[i];
        if (f.has_data || (f.from && HasFmt(c, f.from))) {
            ++i;
            continue;
        }
        c.formats.erase(c.formats.begin() + i);
        changed = true;
    }
    if (!changed) return;
    c.seqno++;
    NotifyViewer(c, NotifyListeners(c));
}

// Stands in for the server's cleanup_clipboard_window / cleanup_clipboard_thread hooks.
void Sweep(Clip& c) {
    if (c.open_tid) {
        UserThread* t = FindUserThreadById(c.open_tid);
        if (!t || t->exiting || (c.open_win && !Win(c.open_win))) CloseInternal(c);
    }
    if (c.owner && !Win(c.owner)) ReleaseOwner(c);
    if (c.viewer && !Win(c.viewer)) c.viewer = nullptr;
    c.listeners.erase(std::remove_if(c.listeners.begin(), c.listeners.end(), [](HWND h) { return !Win(h); }),
                      c.listeners.end());
}

bool OpenByMe(const Clip& c) { return c.open_tid && c.open_tid == GetCurrentThreadId(); }

// Asks the owner to render `format` (WM_RENDERFORMAT). Lock held on entry and exit; pointers into
// the clipboard are stale afterwards.
void RenderViaOwner(Clip& c, UINT format) {
    HWND owner = c.owner;
    if (!owner || !Win(owner)) return;
    ++c.rendering;
    SendMessageInternal(owner, WM_RENDERFORMAT, format, 0);
    --c.rendering;
}

// ABI of include/ntuser.h (not compatible with Windows).
struct GetClipboardParams {
    void* data;
    std::size_t size;
    std::size_t data_size;
    UINT seqno;
    BOOL data_only;
};
struct SetClipboardParams {
    void* data;
    std::size_t size;
    BOOL cache_only;
    UINT seqno;
};

}  // namespace
}  // namespace detail

namespace sys {
using namespace win10xbox::user::detail;

BOOL WINAPI NtUserOpenClipboard(HWND hwnd, ULONG unk) {
    (void)unk;
    std::vector<Cache> free_list;
    {
        LockGuard lock;
        Clip& c = C();
        Sweep(c);
        HWND win = nullptr;
        if (hwnd) {
            if (!Win(hwnd)) {
                SetLastErr(ERROR_INVALID_WINDOW_HANDLE);
                return FALSE;
            }
            win = FullHandle(hwnd);
        }
        CurrentUserThread();   // the sweep needs a record for the opening thread
        if (c.open_tid && (c.open_win != win || c.open_tid != GetCurrentThreadId())) {
            SetLastErr(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (!c.open_tid) c.open_seqno = c.seqno;   // first open
        c.open_win = win;
        c.open_tid = GetCurrentThreadId();
        DrainToFree(c, &free_list);
        FreeCache(std::move(free_list));
    }
    return TRUE;
}

BOOL WINAPI NtUserCloseClipboard() {
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    if (!OpenByMe(c)) {
        SetLastErr(ERROR_CLIPBOARD_NOT_OPEN);
        return FALSE;
    }
    CloseInternal(c);
    return TRUE;
}

BOOL WINAPI NtUserEmptyClipboard() {
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    if (!OpenByMe(c)) {
        SetLastErr(ERROR_CLIPBOARD_NOT_OPEN);
        return FALSE;
    }
    if (c.owner && Win(c.owner)) {
        HWND owner = c.owner;
        SendMessageInternal(owner, WM_DESTROYCLIPBOARD, 0, 0);
        if (!OpenByMe(c)) {   // the owner closed it from its handler
            SetLastErr(ERROR_CLIPBOARD_NOT_OPEN);
            return FALSE;
        }
    }
    c.formats.clear();
    c.owner = c.open_win;
    c.seqno++;
    std::vector<Cache> free_list;
    DrainToFree(c, &free_list);
    free_list.insert(free_list.end(), c.cache.begin(), c.cache.end());
    c.cache.clear();
    FreeCache(std::move(free_list));
    return TRUE;
}

NTSTATUS WINAPI NtUserSetClipboardData(UINT format, HANDLE data, void* raw_params) {
    auto* params = static_cast<SetClipboardParams*>(raw_params);
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    if (!params) return static_cast<NTSTATUS>(0xC000000D);   // STATUS_INVALID_PARAMETER

    if (params->cache_only) {
        Cache* e = FindCache(c, format);
        if (e && e->seqno == params->seqno) {
            e->handle = data;
            return 0;
        }
        return static_cast<NTSTATUS>(0xC0000001);   // STATUS_UNSUCCESSFUL: the data changed meanwhile
    }

    if (!format || !OpenByMe(c)) return Win32Status(ERROR_CLIPBOARD_NOT_OPEN);

    Fmt* f = FindFmt(c, format);
    if (!f) f = AddFmt(c, format);
    f->from = 0;
    f->seqno = c.seqno;
    f->data.clear();
    f->has_data = params->data != nullptr;
    if (params->data && params->size) {
        const auto* p = static_cast<const std::uint8_t*>(params->data);
        f->data.assign(p, p + params->size);
    }
    if (!c.rendering) c.seqno++;
    if (IsText(format)) c.lcid = GetUserDefaultLCID();
    const UINT seqno = f->seqno;

    // win32u: replace the cached handle of this format.
    std::vector<Cache> free_list;
    for (const Cache& e : c.cache)
        if (e.format == format) free_list.push_back(e);
    EraseCache(c, format);
    if (params->data && data) {
        if (format == CF_BITMAP || format == CF_PALETTE)
            if (GdiObject* o = GdiGet(data)) o->system = true;
        c.cache.push_back(Cache{format, seqno, data});
    }
    FreeCache(std::move(free_list));
    return 0;
}

HANDLE WINAPI NtUserGetClipboardData(UINT format, void* raw_params) {
    auto* p = static_cast<GetClipboardParams*>(raw_params);
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    if (!p) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (!OpenByMe(c)) {
        SetLastErr(ERROR_CLIPBOARD_NOT_OPEN);
        return nullptr;
    }
    const std::size_t capacity = p->size;
    bool render = true;
    for (;;) {
        Fmt* f = FindFmt(c, format);
        if (!f) {
            SetLastErr(ERROR_NOT_FOUND);   // no such format
            return nullptr;
        }
        Cache* cache = p->data_only ? nullptr : FindCache(c, format);
        if (f->Ready()) {
            if (cache && cache->handle && cache->seqno == f->seqno) return cache->handle;   // cache still valid
            const std::size_t total = f->data.size();
            p->size = total;
            if (total > capacity || !p->data) {   // user32 retries with a buffer of data_size
                p->data_size = total;
                return nullptr;
            }
            std::memcpy(p->data, f->data.data(), total);
            if (cache) {   // stale entry
                c.to_free.push_back(*cache);
                EraseCache(c, format);
            }
            if (p->data_only) return p->data;
            c.cache.push_back(Cache{format, f->seqno, nullptr});   // unmarshalled by user32, then cache_only Set
            p->seqno = f->seqno;
            p->data_size = ~static_cast<std::size_t>(0);
            return nullptr;
        }
        if (!render) return nullptr;   // rendering did not produce data
        render = false;
        if (f->from) {
            // Synthesized: make sure the source has data (render it through the owner if it is delayed), convert.
            const UINT from = f->from;
            Fmt* src = FindFmt(c, from);
            if (src && !src->Ready()) {
                RenderViaOwner(c, from);
                f = FindFmt(c, format);
                src = FindFmt(c, from);
            }
            if (!f || !src || !src->Ready()) return nullptr;
            std::vector<std::uint8_t> conv = ConvertText(*src, format);
            if (conv.empty()) return nullptr;
            f->data = std::move(conv);
            f->has_data = true;
            continue;
        }
        if (c.owner) {
            RenderViaOwner(c, format);   // WM_RENDERFORMAT: the owner calls SetClipboardData
            if (!OpenByMe(c)) {
                SetLastErr(ERROR_CLIPBOARD_NOT_OPEN);
                return nullptr;
            }
            continue;
        }
        return nullptr;
    }
}

INT WINAPI NtUserCountClipboardFormats() {
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    return static_cast<INT>(c.formats.size());
}

BOOL WINAPI NtUserIsClipboardFormatAvailable(UINT format) {
    if (!format) return FALSE;
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    return HasFmt(c, format) ? TRUE : FALSE;
}

UINT WINAPI NtUserEnumClipboardFormats(UINT format) {
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    if (!OpenByMe(c)) {
        SetLastErr(ERROR_CLIPBOARD_NOT_OPEN);
        return 0;
    }
    std::size_t i = 0;
    if (format) {
        while (i < c.formats.size() && c.formats[i].id != format) ++i;
        if (i < c.formats.size()) ++i;
    }
    if (i >= c.formats.size()) return 0;   // end of the list: no error
    SetLastErr(ERROR_SUCCESS);
    return c.formats[i].id;
}

BOOL WINAPI NtUserGetUpdatedClipboardFormats(UINT* formats, UINT size, UINT* out_size) {
    if (!out_size) {
        SetLastErr(ERROR_NOACCESS);
        return FALSE;
    }
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    const UINT count = static_cast<UINT>(c.formats.size());
    *out_size = count;
    if (count && (!formats || size < count)) {
        SetLastErr(formats ? ERROR_INSUFFICIENT_BUFFER : ERROR_NOACCESS);
        return FALSE;
    }
    for (UINT i = 0; i < count; ++i) formats[i] = c.formats[i].id;
    return TRUE;
}

INT WINAPI NtUserGetPriorityClipboardFormat(UINT* list, INT count) {
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    if (c.formats.empty()) return 0;
    for (INT i = 0; i < count; ++i)
        if (HasFmt(c, list[i])) return static_cast<INT>(list[i]);
    return -1;
}

INT WINAPI NtUserGetClipboardFormatName(UINT format, WCHAR* buffer, INT maxlen) {
    if (format < MAXINTATOM || format > 0xffff || !buffer || maxlen <= 0) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return 0;
    }
    UNICODE_STRING str{};
    str.Buffer = buffer;
    str.MaximumLength = static_cast<USHORT>(maxlen * sizeof(WCHAR));
    return static_cast<INT>(NtUserGetAtomName(static_cast<ATOM>(format), &str));
}

HWND WINAPI NtUserGetClipboardOwner() {
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    return c.owner;
}

HWND WINAPI NtUserGetClipboardViewer() {
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    return c.viewer;
}

HWND WINAPI NtUserGetOpenClipboardWindow() {
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    return c.open_win;
}

DWORD WINAPI NtUserGetClipboardSequenceNumber() {
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    return c.seqno;
}

HWND WINAPI NtUserSetClipboardViewer(HWND hwnd) {
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    HWND viewer = nullptr;
    if (hwnd) {
        if (!Win(hwnd)) {
            SetLastErr(ERROR_INVALID_WINDOW_HANDLE);
            return nullptr;
        }
        viewer = FullHandle(hwnd);
    }
    HWND prev = c.viewer;
    c.viewer = viewer;
    if (viewer) NotifyViewer(c, viewer);
    return prev;
}

BOOL WINAPI NtUserChangeClipboardChain(HWND hwnd, HWND next) {
    if (!hwnd) return FALSE;
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    HWND full = Win(hwnd) ? FullHandle(hwnd) : nullptr;
    if (!full) {
        SetLastErr(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    HWND nxt = nullptr;
    if (next) {
        if (!Win(next)) {
            SetLastErr(ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }
        nxt = FullHandle(next);
    }
    if (c.viewer == full) {
        c.viewer = nxt;
        return TRUE;
    }
    if (!c.viewer) return FALSE;
    // Somewhere down the chain: the viewer passes the message along (WM_CHANGECBCHAIN).
    return !SendMessageInternal(c.viewer, WM_CHANGECBCHAIN, reinterpret_cast<WPARAM>(full),
                                reinterpret_cast<LPARAM>(nxt));
}

BOOL WINAPI NtUserAddClipboardFormatListener(HWND hwnd) {
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    if (!Win(hwnd)) {
        SetLastErr(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    HWND full = FullHandle(hwnd);
    if (std::find(c.listeners.begin(), c.listeners.end(), full) != c.listeners.end()) {
        SetLastErr(ERROR_INVALID_PARAMETER);   // already registered
        return FALSE;
    }
    c.listeners.push_back(full);
    return TRUE;
}

BOOL WINAPI NtUserRemoveClipboardFormatListener(HWND hwnd) {
    LockGuard lock;
    Clip& c = C();
    Sweep(c);
    if (!Win(hwnd)) {
        SetLastErr(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    HWND full = FullHandle(hwnd);
    auto it = std::find(c.listeners.begin(), c.listeners.end(), full);
    if (it == c.listeners.end()) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    c.listeners.erase(it);
    return TRUE;
}

}  // namespace sys
}  // namespace win10xbox::user
