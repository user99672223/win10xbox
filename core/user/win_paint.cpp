// core/user painting: update regions (a bounding rectangle per window, in
// client coordinates, plus the erase / frame / internal-paint flags),
// InvalidateRect / RedrawWindow / BeginPaint / EndPaint, window DCs
// (GetDCEx / ReleaseDC), the non-client frame of captioned windows (solid
// colours, no caption text yet), window surfaces from the display driver
// (one per visible top-level window; children draw into their top-level's
// surface), the flush to the driver, and the composition of the virtual
// screen for the host (ComposeFrame).
#include "user_internal.h"
#include "user_syscalls.h"

#include <algorithm>
#include <cstring>

namespace win10xbox::user {
namespace detail {

namespace {

bool NeedsPaint(const WindowObj* w) {
    return !RectEmpty(w->update) || w->internal_paint || w->nc_pending;
}

bool HasNonClient(const WindowObj* w) {
    return w->window.left != w->client.left || w->window.top != w->client.top || w->window.right != w->client.right ||
           w->window.bottom != w->client.bottom;
}

void RetainFrame() {
    State& s = S();
    s.retained.clear();
    WindowObj* desk = Win(s.desktop);
    if (!desk) return;
    for (auto it = desk->children.rbegin(); it != desk->children.rend(); ++it) {
        WindowObj* w = Win(*it);
        if (!w || !(w->style & WS_VISIBLE) || !w->surface) continue;
        s.retained.push_back(RetainedLayer{w->surface, WindowRectScreen(w)});
    }
    s.retained_valid = true;
}

void Compose(const std::vector<RetainedLayer>& layers, std::uint32_t* bgra, std::int32_t width, std::int32_t height,
             std::int32_t stride) {
    const std::uint32_t desktop = ColorToPixel(SysColor(COLOR_BACKGROUND));
    for (std::int32_t y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(bgra) + static_cast<std::ptrdiff_t>(y) * stride);
        std::fill(row, row + width, desktop);
    }
    for (const RetainedLayer& l : layers) {
        winexbox::ISurface& s = *l.surface;
        const RECT r = RectIntersect(RECT{l.screen.left, l.screen.top, l.screen.left + s.Width(), l.screen.top + s.Height()},
                                     RECT{0, 0, width, height});
        if (RectEmpty(r)) continue;
        for (int y = r.top; y < r.bottom; ++y) {
            const auto* src = reinterpret_cast<const std::uint32_t*>(s.Bits() + static_cast<std::ptrdiff_t>(y - l.screen.top) * s.Stride());
            auto* dst = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(bgra) + static_cast<std::ptrdiff_t>(y) * stride);
            for (int x = r.left; x < r.right; ++x) dst[x] = src[x - l.screen.left] | 0xff000000u;
        }
    }
}

std::vector<RetainedLayer> LiveLayers() {
    std::vector<RetainedLayer> layers;
    State* s = StateIfAny();
    if (!s) return layers;
    WindowObj* desk = Win(s->desktop);
    if (!desk) return layers;
    for (auto it = desk->children.rbegin(); it != desk->children.rend(); ++it) {
        WindowObj* w = Win(*it);
        if (!w || !(w->style & WS_VISIBLE) || !w->surface) continue;
        layers.push_back(RetainedLayer{w->surface, WindowRectScreen(w)});
    }
    return layers;
}

}  // namespace

// ---- Surfaces ----------------------------------------------------------------------------------

void EnsureSurface(WindowObj* w) {
    if (!IsTopLevel(w) || !w->driver_created) return;
    winexbox::IWinexboxDriver* d = Driver();
    if (!d) return;
    const int ww = w->window.right - w->window.left, wh = w->window.bottom - w->window.top;
    if (!(w->style & WS_VISIBLE) && !w->surface) return;   // allocated at the first show
    if (w->surface && w->surface->Width() == ww && w->surface->Height() == wh) return;
    std::shared_ptr<winexbox::ISurface> old = w->surface;
    w->surface.reset();
    if (ww <= 0 || wh <= 0) return;
    std::shared_ptr<winexbox::ISurface> s = d->CreateWindowSurface(reinterpret_cast<winexbox::WindowId>(w->handle), false,
                                                                    winexbox::Rect{0, 0, ww, wh});
    if (!s || s->Format() != winexbox::PixelFormat::Bgra8888) {
        WARN(user, "%p: no %dx%d surface from the %s driver", w->handle, ww, wh, d->Name());
        return;
    }
    // Opaque black until the window paints; what it had before at the top-left corner otherwise.
    for (int y = 0; y < s->Height(); ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(s->Bits() + static_cast<std::ptrdiff_t>(y) * s->Stride());
        std::fill(row, row + s->Width(), 0xff000000u);
        if (old && y < old->Height()) {
            const auto* src = reinterpret_cast<const std::uint32_t*>(old->Bits() + static_cast<std::ptrdiff_t>(y) * old->Stride());
            std::copy(src, src + std::min(s->Width(), old->Width()), row);
        }
    }
    w->surface = s;
    w->dirty = RECT{0, 0, 0, 0};
    TRACE(user, "%p: %dx%d surface", w->handle, ww, wh);
}

void ReleaseSurface(WindowObj* w) {
    if (w->surface) {
        w->surface.reset();
        w->dirty = RECT{0, 0, 0, 0};
    }
}

void FlushWindow(WindowObj* top) {
    if (!top || !top->surface || RectEmpty(top->dirty)) return;
    const RECT d = top->dirty;
    top->dirty = RECT{0, 0, 0, 0};
    const winexbox::Rect r{d.left, d.top, d.right, d.bottom};
    top->surface->Flush(&r);
    if (winexbox::IWinexboxDriver* drv = Driver()) {
        drv->Invalidate(reinterpret_cast<winexbox::WindowId>(top->handle), &r);
        drv->Present();
    }
    BumpStat(&Stats::flushes);
    NoteScreenChanged();
    RetainFrame();
}

void FlushAllDirty() {
    State* s = StateIfAny();
    if (!s) return;
    WindowObj* desk = Win(s->desktop);
    if (!desk) return;
    for (HWND h : desk->children)
        if (WindowObj* w = Win(h); w && !RectEmpty(w->dirty)) FlushWindow(w);
}

// ---- Update regions -----------------------------------------------------------------------------

void InvalidateWindow(HWND hwnd, const RECT* client_rect, UINT flags) {
    WindowObj* w = Win(hwnd);
    if (!w) return;
    const RECT full{0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top};
    RECT r = client_rect ? RectIntersect(*client_rect, full) : full;
    if (flags & RDW_INVALIDATE) {
        if (IsWindowReallyVisible(w)) {
            if (!RectEmpty(r)) w->update = RectUnion(w->update, r);
            if (flags & RDW_ERASE) w->erase_pending = w->erase_pending || !RectEmpty(r);
            if ((flags & RDW_FRAME) && HasNonClient(w)) w->nc_pending = true;
            if (flags & RDW_INTERNALPAINT) w->internal_paint = true;
            if (NeedsPaint(w)) WakeThread(FindUserThreadById(w->tid), QS_PAINT);
        }
    } else if (flags & RDW_VALIDATE) {
        if (!client_rect || (r.left <= w->update.left && r.top <= w->update.top && r.right >= w->update.right &&
                             r.bottom >= w->update.bottom))
            w->update = RECT{0, 0, 0, 0};
        if (flags & RDW_NOERASE) w->erase_pending = false;
        if (flags & RDW_NOFRAME) w->nc_pending = false;
        if (flags & RDW_NOINTERNALPAINT) w->internal_paint = false;
        if (RectEmpty(w->update)) w->erase_pending = false;
    }
    const bool children = (flags & RDW_ALLCHILDREN) || (!(flags & RDW_NOCHILDREN) && !(w->style & WS_CLIPCHILDREN));
    if (!children) return;
    for (HWND ch : std::vector<HWND>(w->children)) {
        WindowObj* c = Win(ch);
        if (!c || !(c->style & WS_VISIBLE)) continue;
        // parent client -> child client coordinates
        const RECT in_child = RectOffset(RectIntersect(r, c->window), -c->client.left, -c->client.top);
        if (RectEmpty(RectIntersect(r, c->window))) continue;
        InvalidateWindow(ch, &in_child, flags | RDW_FRAME);
    }
}

HWND FindPaintWindow(UserThread* t) {
    State* s = StateIfAny();
    if (!s || !t) return nullptr;
    HWND found = nullptr;
    WindowObj* desk = Win(s->desktop);
    if (!desk) return nullptr;
    for (HWND top : std::vector<HWND>(desk->children)) {
        ForEachWindowPreorder(top, [&](WindowObj* w) {
            if (found) return false;
            if (!(w->style & WS_VISIBLE)) return false;   // nor its children
            if (w->tid == t->tid && NeedsPaint(w)) found = w->handle;
            return !found;
        });
        if (found) break;
    }
    return found;
}

void PaintNonClient(HWND hwnd) {
    WindowObj* w = Win(hwnd);
    if (!w || !HasNonClient(w) || !IsWindowReallyVisible(w)) return;
    HDC hdc = GetDCInternal(hwnd, nullptr, DCX_WINDOW | DCX_CACHE);
    DcObj* dc = DcGet(hdc);
    DrawTarget t;
    if (dc && ResolveDrawTarget(dc, &t)) {
        const int ww = w->window.right - w->window.left, wh = w->window.bottom - w->window.top;
        const RECT client = RectOffset(w->client, -w->window.left, -w->window.top);
        UserThread* th = FindUserThreadById(w->tid);
        const bool active = th && th->active == hwnd;
        const std::uint32_t frame = ColorToPixel(SysColor(active ? COLOR_ACTIVEBORDER : COLOR_INACTIVEBORDER));
        // the frame: everything outside the client area
        FillTargetRect(t, RECT{0, 0, ww, client.top}, frame);
        FillTargetRect(t, RECT{0, client.bottom, ww, wh}, frame);
        FillTargetRect(t, RECT{0, client.top, client.left, client.bottom}, frame);
        FillTargetRect(t, RECT{client.right, client.top, ww, client.bottom}, frame);
        if ((w->style & WS_CAPTION) == WS_CAPTION) {
            const int f = FrameInset(w), cap = CaptionHeightOf(w);
            const RECT caption{f, f, ww - f, f + cap - 1};
            FillTargetRect(t, caption, ColorToPixel(SysColor(active ? COLOR_ACTIVECAPTION : COLOR_INACTIVECAPTION)));
            FillTargetRect(t, RECT{f, f + cap - 1, ww - f, f + cap}, ColorToPixel(SysColor(COLOR_WINDOWFRAME)));
            // the system buttons: plain rectangles (no theme), a glyph each; close red, maximize blue, minimize grey
            int text_right = caption.right;
            const std::uint32_t black = ColorToPixel(RGB(0, 0, 0)), white = ColorToPixel(RGB(255, 255, 255));
            struct Button { int ht; COLORREF face; };
            const Button buttons[] = {{HTMINBUTTON, RGB(225, 225, 225)}, {HTMAXBUTTON, RGB(200, 216, 240)}, {HTCLOSE, RGB(199, 80, 80)}};
            for (const Button& b : buttons) {
                RECT r;
                if (!CaptionButtonRect(w, b.ht, &r)) continue;
                text_right = std::min<int>(text_right, r.left);
                FillTargetRect(t, r, ColorToPixel(b.face));
                const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
                if (b.ht == HTMINBUTTON) {
                    FillTargetRect(t, RECT{cx - 5, cy + 3, cx + 5, cy + 4}, black);
                } else if (b.ht == HTMAXBUTTON) {
                    FillTargetRect(t, RECT{cx - 5, cy - 5, cx + 5, cy - 4}, black);
                    FillTargetRect(t, RECT{cx - 5, cy + 4, cx + 5, cy + 5}, black);
                    FillTargetRect(t, RECT{cx - 5, cy - 5, cx - 4, cy + 5}, black);
                    FillTargetRect(t, RECT{cx + 4, cy - 5, cx + 5, cy + 5}, black);
                } else {
                    for (int i = 0; i < 10; ++i) {
                        FillTargetRect(t, RECT{cx - 5 + i, cy - 5 + i, cx - 4 + i, cy - 4 + i}, white);
                        FillTargetRect(t, RECT{cx - 5 + i, cy + 4 - i, cx - 4 + i, cy + 5 - i}, white);
                    }
                }
            }
            // the title, in the built-in face, between the left edge and the first button
            const RECT text_box{caption.left + 6, caption.top, text_right - 4, caption.bottom};
            DrawGlyphRun(t, text_box, text_box.left, caption.top + (cap - 1 - 16) / 2, w->text,
                         ColorToPixel(SysColor(active ? COLOR_CAPTIONTEXT : COLOR_INACTIVECAPTIONTEXT)));
        }
        NoteDrawn(dc, t, RECT{0, 0, ww, wh});
    }
    ReleaseDCInternal(hwnd, hdc);
}

void SendEraseMessages(HWND hwnd, HDC hdc_in, bool* erased) {
    WindowObj* w = Win(hwnd);
    if (erased) *erased = false;
    if (!w) return;
    if (w->nc_pending) {
        w->nc_pending = false;
        if (HasNonClient(w)) SendMessageInternal(hwnd, WM_NCPAINT, 1, 0);
        w = Win(hwnd);
        if (!w) return;
    }
    if (w->erase_pending) {
        w->erase_pending = false;
        HDC hdc = hdc_in;
        if (!hdc) {
            const RECT clip = w->update;
            hdc = GetDCInternal(hwnd, nullptr, abi::kDcxUseStyle | DCX_CACHE);
            if (DcObj* dc = DcGet(hdc)) {
                dc->has_paint_clip = true;
                dc->paint_clip = clip;
            }
        }
        const LRESULT r = SendMessageInternal(hwnd, WM_ERASEBKGND, reinterpret_cast<WPARAM>(hdc), 0);
        if (erased) *erased = r != 0;
        if (!hdc_in) ReleaseDCInternal(hwnd, hdc);
    }
}

bool UpdateWindowNow(HWND hwnd, UINT flags) {
    WindowObj* w = Win(hwnd);
    if (!w || !IsWindowReallyVisible(w)) return true;
    if (flags & RDW_UPDATENOW) {
        if (NeedsPaint(w)) {
            if (!RectEmpty(w->update) || w->internal_paint || w->nc_pending) {
                SendMessageInternal(hwnd, WM_PAINT, 0, 0);
                if ((w = Win(hwnd)) && (w->nc_pending || w->erase_pending)) SendEraseMessages(hwnd, nullptr, nullptr);
            }
        }
    } else if (flags & RDW_ERASENOW) {
        if (w->nc_pending || w->erase_pending) SendEraseMessages(hwnd, nullptr, nullptr);
    }
    if (!(w = Win(hwnd))) return true;
    const bool children = (flags & RDW_ALLCHILDREN) || !(flags & RDW_NOCHILDREN);
    if (children)
        for (HWND ch : std::vector<HWND>(w->children)) UpdateWindowNow(ch, flags);
    if (WindowObj* top = Win(TopLevelOf(hwnd))) FlushWindow(top);
    return true;
}

// ---- DCs ----------------------------------------------------------------------------------------

HDC GetDCInternal(HWND hwnd, HRGN clip, DWORD flags) {
    if (!hwnd) {
        HDC hdc = CreateDcObj(DcKind::Screen, nullptr);
        if (DcObj* dc = DcGet(hdc)) dc->cache = true;
        return hdc;
    }
    WindowObj* w = WinChecked(hwnd);
    if (!w) return nullptr;
    hwnd = w->handle;
    if (w->flags & kWinIsDesktop) {
        HDC hdc = CreateDcObj(DcKind::Screen, nullptr);
        if (DcObj* dc = DcGet(hdc)) dc->cache = true;
        return hdc;
    }
    const bool own = (w->cls->style & (CS_OWNDC | CS_CLASSDC)) && !(flags & DCX_CACHE) && !(flags & DCX_WINDOW);
    HDC hdc = nullptr;
    if (own && w->own_dc && DcGet(w->own_dc)) {
        hdc = w->own_dc;
    } else {
        hdc = CreateDcObj(DcKind::Window, hwnd);
        DcObj* dc = DcGet(hdc);
        if (!dc) return nullptr;
        if (own) {
            dc->own_dc = true;
            w->own_dc = hdc;
        } else {
            dc->cache = true;
        }
    }
    DcObj* dc = DcGet(hdc);
    dc->window_origin = (flags & DCX_WINDOW) != 0;
    dc->has_paint_clip = false;
    if (clip && (flags & DCX_INTERSECTRGN))
        if (auto* r = GdiGetT<RegionObj>(clip, abi::kObjRegion)) {
            dc->has_paint_clip = true;
            dc->paint_clip = r->rgn.Box();
        }
    if (clip && (flags & DCX_EXCLUDERGN)) FIXME(gdi, "GetDCEx(DCX_EXCLUDERGN) ignored");
    if (winexbox::IWinexboxDriver* d = Driver()) {
        const RECT wr = WindowRectScreen(w);
        WindowObj* top = Win(TopLevelOf(hwnd));
        const RECT tr = top ? WindowRectScreen(top) : wr;
        d->DcAcquire(reinterpret_cast<std::uint64_t>(hdc), reinterpret_cast<winexbox::WindowId>(hwnd),
                     reinterpret_cast<winexbox::WindowId>(top ? top->handle : hwnd), winexbox::Rect{wr.left, wr.top, wr.right, wr.bottom},
                     winexbox::Rect{tr.left, tr.top, tr.right, tr.bottom}, flags);
    }
    return hdc;
}

int ReleaseDCInternal(HWND hwnd, HDC hdc) {
    DcObj* dc = DcGet(hdc);
    if (!dc) return 0;
    if (dc->kind == DcKind::Window) {
        if (dc->drew)
            if (WindowObj* top = Win(TopLevelOf(dc->hwnd))) FlushWindow(top);
        dc->drew = false;
        if (winexbox::IWinexboxDriver* d = Driver())
            d->DcRelease(reinterpret_cast<winexbox::WindowId>(hwnd ? hwnd : dc->hwnd), reinterpret_cast<std::uint64_t>(hdc));
    }
    if (dc->own_dc) {
        dc->has_paint_clip = false;
        dc->paint_dc = false;
        return 1;
    }
    if (!dc->cache) return 0;   // not a DC from GetDC
    FreeDcObj(hdc);
    return 1;
}

}  // namespace detail

// ---- Host-facing ----------------------------------------------------------------------------------

bool ComposeFrame(std::uint32_t* bgra, std::int32_t width, std::int32_t height, std::int32_t stride_bytes) {
    if (!bgra || width <= 0 || height <= 0) return false;
    detail::LockGuard lock;
    if (!detail::StateIfAny()) return false;
    detail::Compose(detail::LiveLayers(), bgra, width, height, stride_bytes);
    return true;
}

bool ComposeLastPaintedFrame(std::uint32_t* bgra, std::int32_t width, std::int32_t height, std::int32_t stride_bytes) {
    if (!bgra || width <= 0 || height <= 0) return false;
    detail::LockGuard lock;
    detail::State* s = detail::StateIfAny();
    if (!s) return false;
    detail::Compose(s->retained_valid ? s->retained : detail::LiveLayers(), bgra, width, height, stride_bytes);
    return true;
}

// ---- Syscalls -----------------------------------------------------------------------------------
namespace sys {
using namespace detail;

HDC WINAPI NtUserBeginPaint(HWND hwnd, PAINTSTRUCT* ps) {
    LockGuard lock;
    return BeginPaintInternal(hwnd, ps);
}

BOOL WINAPI NtUserEndPaint(HWND hwnd, const PAINTSTRUCT* ps) {
    LockGuard lock;
    return EndPaintInternal(hwnd, ps);
}

}  // namespace sys

namespace detail {

HDC BeginPaintInternal(HWND hwnd, PAINTSTRUCT* ps) {
    WindowObj* w = WinChecked(hwnd);
    if (!w || !ps) return nullptr;
    hwnd = w->handle;
    const RECT update = w->update;
    const bool erase = w->erase_pending;
    const bool nc = w->nc_pending;
    w->update = RECT{0, 0, 0, 0};
    w->erase_pending = false;
    w->nc_pending = false;
    w->internal_paint = false;
    if (nc && HasNonClient(w)) {
        SendMessageInternal(hwnd, WM_NCPAINT, 1, 0);
        if (!(w = Win(hwnd))) return nullptr;
    }
    HDC hdc = GetDCInternal(hwnd, nullptr, abi::kDcxUseStyle);
    DcObj* dc = DcGet(hdc);
    if (!dc) return nullptr;
    dc->has_paint_clip = true;
    dc->paint_clip = update;
    dc->paint_dc = true;
    std::memset(ps, 0, sizeof *ps);
    ps->hdc = hdc;
    ps->rcPaint = update;
    ps->fErase = TRUE;
    if (erase) {
        const LRESULT r = SendMessageInternal(hwnd, WM_ERASEBKGND, reinterpret_cast<WPARAM>(hdc), 0);
        ps->fErase = r ? FALSE : TRUE;
    } else {
        ps->fErase = FALSE;
    }
    BumpStat(&Stats::paints);
    TRACE(user, "%p: hdc %p paint (%ld,%ld)-(%ld,%ld) erase %d", hwnd, hdc, update.left, update.top, update.right,
          update.bottom, ps->fErase);
    return hdc;
}

BOOL EndPaintInternal(HWND hwnd, const PAINTSTRUCT* ps) {
    if (!ps) return FALSE;
    ReleaseDCInternal(hwnd, ps->hdc);
    return TRUE;
}

}  // namespace detail

namespace sys {

HDC WINAPI NtUserGetDC(HWND hwnd) {
    LockGuard lock;
    return GetDCInternal(hwnd, nullptr, abi::kDcxUseStyle);
}

HDC WINAPI NtUserGetDCEx(HWND hwnd, HRGN clip_rgn, DWORD flags) {
    LockGuard lock;
    return GetDCInternal(hwnd, clip_rgn, flags);
}

HDC WINAPI NtUserGetWindowDC(HWND hwnd) {
    LockGuard lock;
    return GetDCInternal(hwnd, nullptr, abi::kDcxUseStyle | DCX_WINDOW);
}

INT WINAPI NtUserReleaseDC(HWND hwnd, HDC hdc) {
    LockGuard lock;
    return ReleaseDCInternal(hwnd, hdc);
}

namespace {
BOOL InvalidateAllTopLevels(UINT flags) {
    if (WindowObj* desk = Win(DesktopWindow()))
        for (HWND h : std::vector<HWND>(desk->children)) InvalidateWindow(h, nullptr, flags | RDW_ALLCHILDREN | RDW_FRAME);
    return TRUE;
}
bool RegionRect(HRGN hrgn, RECT* out) {
    auto* r = GdiGetT<RegionObj>(hrgn, abi::kObjRegion);
    if (!r) return false;
    *out = r->rgn.Box();
    return true;
}
}  // namespace

BOOL WINAPI NtUserInvalidateRect(HWND hwnd, const RECT* rect, BOOL erase) {
    LockGuard lock;
    const UINT flags = RDW_INVALIDATE | (erase ? RDW_ERASE : 0);
    if (!hwnd) return InvalidateAllTopLevels(flags);
    if (!WinChecked(hwnd)) return FALSE;
    InvalidateWindow(FullHandle(hwnd), rect, flags);
    return TRUE;
}

BOOL WINAPI NtUserInvalidateRgn(HWND hwnd, HRGN hrgn, BOOL erase) {
    LockGuard lock;
    if (!WinChecked(hwnd)) return FALSE;
    RECT r;
    const bool have = hrgn && RegionRect(hrgn, &r);
    if (hrgn && !have) return FALSE;
    InvalidateWindow(FullHandle(hwnd), have ? &r : nullptr, RDW_INVALIDATE | (erase ? RDW_ERASE : 0));
    return TRUE;
}

BOOL WINAPI NtUserValidateRect(HWND hwnd, const RECT* rect) {
    LockGuard lock;
    if (!hwnd) return TRUE;
    if (!WinChecked(hwnd)) return FALSE;
    InvalidateWindow(FullHandle(hwnd), rect, RDW_VALIDATE | RDW_NOCHILDREN);
    return TRUE;
}

BOOL WINAPI NtUserValidateRgn(HWND hwnd, HRGN hrgn) {
    LockGuard lock;
    if (!WinChecked(hwnd)) return FALSE;
    RECT r;
    const bool have = hrgn && RegionRect(hrgn, &r);
    InvalidateWindow(FullHandle(hwnd), have ? &r : nullptr, RDW_VALIDATE | RDW_NOCHILDREN);
    return TRUE;
}

BOOL WINAPI NtUserRedrawWindow(HWND hwnd, const RECT* rect, HRGN hrgn, UINT flags) {
    LockGuard lock;
    if (!hwnd || FullHandle(hwnd) == S().desktop) {
        if (flags & (RDW_INVALIDATE | RDW_VALIDATE)) InvalidateAllTopLevels(flags & (RDW_INVALIDATE | RDW_VALIDATE | RDW_ERASE));
        if (flags & (RDW_UPDATENOW | RDW_ERASENOW))
            if (WindowObj* desk = Win(S().desktop))
                for (HWND h : std::vector<HWND>(desk->children))
                    if (WindowObj* w = Win(h); w && w->tid == GetCurrentThreadId()) UpdateWindowNow(h, flags | RDW_ALLCHILDREN);
        return TRUE;
    }
    WindowObj* w = WinChecked(hwnd);
    if (!w) return FALSE;
    hwnd = w->handle;
    RECT r;
    const RECT* area = rect;
    if (!rect && hrgn && RegionRect(hrgn, &r)) area = &r;
    if (flags & (RDW_INVALIDATE | RDW_VALIDATE)) InvalidateWindow(hwnd, area, flags);
    if (flags & (RDW_UPDATENOW | RDW_ERASENOW)) UpdateWindowNow(hwnd, flags);
    return TRUE;
}

BOOL WINAPI NtUserGetUpdateRect(HWND hwnd, RECT* rect, BOOL erase) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w) return FALSE;
    hwnd = w->handle;
    if (erase && (w->erase_pending || w->nc_pending)) {
        SendEraseMessages(hwnd, nullptr, nullptr);
        if (!(w = Win(hwnd))) return FALSE;
    }
    if (rect) *rect = w->update;
    return !RectEmpty(w->update);
}

INT WINAPI NtUserGetUpdateRgn(HWND hwnd, HRGN hrgn, BOOL erase) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    auto* r = GdiGetT<RegionObj>(hrgn, abi::kObjRegion);
    if (!w || !r) return ERROR;
    hwnd = w->handle;
    if (erase && (w->erase_pending || w->nc_pending)) {
        SendEraseMessages(hwnd, nullptr, nullptr);
        if (!(w = Win(hwnd))) return ERROR;
        r = GdiGetT<RegionObj>(hrgn, abi::kObjRegion);
        if (!r) return ERROR;
    }
    r->rgn.Set(w->update);
    return r->rgn.Complexity();
}

INT WINAPI NtUserExcludeUpdateRgn(HDC hdc, HWND hwnd) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    DcObj* dc = DcGet(hdc);
    if (!w || !dc) return ERROR;
    if (RectEmpty(w->update)) return dc->has_clip ? dc->clip.Complexity() : SIMPLEREGION;
    Rgn upd;
    upd.Set(w->update);
    if (!dc->has_clip) {
        dc->clip.Set(RECT{0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top});
        dc->has_clip = true;
    }
    dc->clip = CombineRgns(dc->clip, upd, RGN_DIFF);
    return dc->clip.Empty() ? NULLREGION : dc->clip.Complexity();
}

HWND WINAPI NtUserWindowFromDC(HDC hdc) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    return dc && dc->kind == DcKind::Window ? dc->hwnd : nullptr;
}

BOOL WINAPI NtUserLockWindowUpdate(HWND hwnd) {
    static volatile LONG reported = 0;   // plain flag
    if (!InterlockedExchange(&reported, 1)) FIXME(user, "LockWindowUpdate(%p): drawing is not suppressed", hwnd);
    return TRUE;
}

INT WINAPI NtUserScrollWindowEx(HWND hwnd, INT dx, INT dy, const RECT* rect, const RECT* clip_rect, HRGN update_rgn,
                                RECT* update_rect, UINT flags) {
    LockGuard lock;
    WindowObj* w = WinChecked(hwnd);
    if (!w) return ERROR;
    hwnd = w->handle;
    const RECT client{0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top};
    RECT src = rect ? RectIntersect(*rect, client) : client;
    if (clip_rect) src = RectIntersect(src, *clip_rect);
    // move the pixels (the rasteriser snapshots overlapping source rectangles)
    HDC hdc = GetDCInternal(hwnd, nullptr, DCX_CACHE | abi::kDcxUseStyle);
    if (DcObj* dc = DcGet(hdc)) {
        DrawTarget t;
        if (ResolveDrawTarget(dc, &t) && !RectEmpty(src)) {
            const int w_ = src.right - src.left, h_ = src.bottom - src.top;
            std::vector<std::uint32_t> copy(static_cast<std::size_t>(w_) * h_, 0xff000000u);
            for (int y = 0; y < h_; ++y)
                for (int x = 0; x < w_; ++x) {
                    const int tx = src.left + x + t.origin.x, ty = src.top + y + t.origin.y;
                    if (tx >= 0 && ty >= 0 && tx < t.px.width && ty < t.px.height) copy[static_cast<std::size_t>(y) * w_ + x] = t.px.Row(ty)[tx];
                }
            const RECT dst = RectIntersect(RectOffset(src, dx, dy), clip_rect ? RectIntersect(*clip_rect, client) : client);
            for (int y = dst.top; y < dst.bottom; ++y)
                for (int x = dst.left; x < dst.right; ++x) {
                    if (!t.clip.Contains(x, y)) continue;
                    const int tx = x + t.origin.x, ty = y + t.origin.y;
                    if (tx < 0 || ty < 0 || tx >= t.px.width || ty >= t.px.height) continue;
                    t.px.Row(ty)[tx] = copy[static_cast<std::size_t>(y - dy - src.top) * w_ + (x - dx - src.left)];
                }
            NoteDrawn(dc, t, dst);
        }
    }
    ReleaseDCInternal(hwnd, hdc);
    // what was uncovered
    Rgn exposed;
    exposed.Set(src);
    Rgn moved;
    moved.Set(RectIntersect(RectOffset(src, dx, dy), src));
    exposed = CombineRgns(exposed, moved, RGN_DIFF);
    const RECT box = exposed.Box();
    if (update_rect) *update_rect = box;
    if (auto* r = GdiGetT<RegionObj>(update_rgn, abi::kObjRegion)) r->rgn = exposed;
    if (flags & SW_SCROLLCHILDREN) {
        for (HWND ch : std::vector<HWND>(w->children))
            if (WindowObj* c = Win(ch); c && (!rect || !RectEmpty(RectIntersect(c->window, *rect))))
                SetWindowPosInternal(ch, nullptr, c->window.left + dx, c->window.top + dy, 0, 0,
                                     SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
    }
    if (flags & (SW_INVALIDATE | SW_ERASE))
        for (const RECT& rr : exposed.rects)
            InvalidateWindow(hwnd, &rr, RDW_INVALIDATE | ((flags & SW_ERASE) ? RDW_ERASE : 0) | RDW_ALLCHILDREN);
    return exposed.Empty() ? NULLREGION : exposed.Complexity();
}

}  // namespace sys
}  // namespace win10xbox::user
