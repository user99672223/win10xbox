// core/user cursors and icons: the HICON / HCURSOR objects Wine's PE user32 builds with CreateIconIndirect, LoadImage and
// the OEM resources, and the NtUser calls that keep them (dlls/win32u/cursoricon.c is the contract: user32 creates the
// handle with NtUserCallOneParam(CreateCursorIcon), fills it once with NtUserSetCursorIconData, and the frame bitmaps
// then belong to the object until NtUserDestroyCursor deletes them).
//
// Implemented: handle allocation, SetCursorIconData (static and animated), FindExistingCursorIcon (the cache of shared
// icons), DestroyCursor, GetIconInfo, GetIconSize, GetCursorFrameInfo, GetCursorInfo, DrawIconEx (mask / colour with
// the Windows raster operations, and pre-multiplied alpha frames composited per pixel), the icon "param" and its free
// callback. Not implemented: a per-cursor hook into the display driver (it is only handed the handle by SetCursor).
#include "user_internal.h"
#include "user_syscalls.h"

#include <algorithm>
#include <cstring>

namespace win10xbox::user {
namespace detail {
namespace {

constexpr ULONG kIconHandleBase = 0x00C00000;   // HICON = base | (slot + 1) << 4 | generation (1..15)
constexpr std::uint32_t kMaxIcons = 0x3FFFF;

struct IconObj {
    HICON handle = nullptr;
    bool is_icon = false, is_ani = false, is_shared = false;
    UINT delay = 0;
    abi::CursorIconFrame frame{};
    UINT num_frames = 0, num_steps = 0;
    std::vector<HICON> frames;            // animated: one handle per step
    std::wstring module;                  // icons loaded from resources
    bool res_is_int = true;
    ULONG_PTR res_id = 0;
    std::wstring res_name;
    HRSRC rsrc = nullptr;
    abi::FreeIconParams params{};
};

struct IconTable {
    std::vector<IconObj*> slots;
    std::vector<std::uint8_t> generations;
    std::vector<std::uint32_t> free_slots;
    std::vector<IconObj*> shared;          // the cache FindExistingCursorIcon searches, newest first
};
IconTable* g_icons = nullptr;              // trivially initialised; allocated under the lock

IconTable& T() {
    if (!g_icons) g_icons = new IconTable;
    return *g_icons;
}

std::uint32_t SlotOf(HICON h) { return ((HandleToUlong(h) & 0x003FFFF0u) >> 4) - 1; }

IconObj* Icon(HICON h) {
    const ULONG v = HandleToUlong(h);
    if ((v & 0xFFC00000u) != kIconHandleBase || ((v & 0x003FFFF0u) >> 4) == 0) return nullptr;
    IconTable& t = T();
    const std::uint32_t slot = SlotOf(h);
    if (slot >= t.slots.size()) return nullptr;
    IconObj* o = t.slots[slot];
    return o && o->handle == h ? o : nullptr;
}

HICON NewIcon(bool is_icon) {
    IconTable& t = T();
    std::uint32_t slot;
    if (!t.free_slots.empty()) {
        slot = t.free_slots.back();
        t.free_slots.pop_back();
    } else {
        if (t.slots.size() >= kMaxIcons) return nullptr;
        slot = static_cast<std::uint32_t>(t.slots.size());
        t.slots.push_back(nullptr);
        t.generations.push_back(0);
    }
    std::uint8_t& gen = t.generations[slot];
    gen = gen >= 15 ? 1 : static_cast<std::uint8_t>(gen + 1);
    auto* o = new IconObj;
    o->is_icon = is_icon;
    o->handle = reinterpret_cast<HICON>(static_cast<ULONG_PTR>(kIconHandleBase | ((slot + 1) << 4) | gen));
    t.slots[slot] = o;
    return o->handle;
}

// Frees the object and its bitmaps (the lock is held); the dispatch callbacks of what was freed are queued in *callbacks.
void FreeIcon(HICON h, std::vector<UINT64>* callbacks) {
    IconObj* o = Icon(h);
    if (!o) return;
    IconTable& t = T();
    const std::uint32_t slot = SlotOf(h);
    t.slots[slot] = nullptr;
    t.free_slots.push_back(slot);
    t.shared.erase(std::remove(t.shared.begin(), t.shared.end(), o), t.shared.end());
    if (o->params.dispatch.callback) callbacks->push_back(o->params.dispatch.callback);
    if (!o->is_ani) {
        if (o->frame.alpha) GdiDelete(o->frame.alpha);
        if (o->frame.color) GdiDelete(o->frame.color);
        if (o->frame.mask) GdiDelete(o->frame.mask);
    } else {
        std::vector<HICON> frames = o->frames;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            HICON f = frames[i];
            if (!f) continue;
            FreeIcon(f, callbacks);
            for (std::size_t j = i; j < frames.size(); ++j)
                if (frames[j] == f) frames[j] = nullptr;
        }
    }
    delete o;
}

// The object whose frame `step` is meant: an animated icon answers with the step's frame object.
IconObj* FrameObj(HICON h, UINT step) {
    IconObj* o = Icon(h);
    if (!o) return nullptr;
    if (!o->is_ani) return o;
    if (step >= o->num_steps) return nullptr;
    return Icon(o->frames[step]);
}

// A private copy of a bitmap (what GetIconInfo hands the caller, who owns and deletes it).
HBITMAP CopyIconBitmap(HBITMAP source) {
    if (!source) return nullptr;
    auto* src = GdiGetT<BitmapObj>(source, abi::kObjBitmap);
    if (!src) return nullptr;
    auto* copy = new BitmapObj;
    copy->width = src->width;
    copy->height = src->height;
    copy->bpp = src->dib ? src->dib_header.biBitCount : src->bpp;
    if (copy->bpp != 1) copy->bpp = 32;
    copy->pixels.resize(static_cast<std::size_t>(copy->width) * copy->height);
    for (int y = 0; y < copy->height; ++y)
        for (int x = 0; x < copy->width; ++x)
            copy->pixels[static_cast<std::size_t>(y) * copy->width + x] = BitmapPixel(*src, x, y);
    return static_cast<HBITMAP>(GdiAlloc(copy, abi::kObjBitmap));
}

}  // namespace

HICON CreateCursorIconHandle(bool is_icon) { return NewIcon(is_icon); }

ULONG_PTR GetIconParam(HICON h) {
    IconObj* o = Icon(h);
    return o ? static_cast<ULONG_PTR>(o->params.param) : 0;
}

ULONG_PTR SetIconParam(HICON h, const abi::FreeIconParams* params) {
    IconObj* o = Icon(h);
    if (!o || !params) return 0;
    const ULONG_PTR old = static_cast<ULONG_PTR>(o->params.param);
    o->params = *params;
    return old;
}

}  // namespace detail

namespace sys {
using namespace detail;

BOOL WINAPI NtUserSetCursorIconData(HCURSOR cursor, UNICODE_STRING* module, UNICODE_STRING* res_name, abi::CursorIconDesc* desc) {
    LockGuard lock;
    IconObj* o = Icon(cursor);
    if (!o || !desc) return FALSE;
    if (o->is_ani || o->frame.width) {   // already initialised
        SetLastErr(ERROR_INVALID_CURSOR_HANDLE);
        return FALSE;
    }
    o->delay = desc->delay;
    if (desc->num_steps) {
        o->frames.assign(desc->num_steps, nullptr);
        o->is_ani = true;
        o->num_steps = desc->num_steps;
        o->num_frames = desc->num_frames;
    } else if (desc->frames) {
        o->frame = desc->frames[0];
    }
    if (res_name) {
        if (res_name->Length) {
            o->res_is_int = false;
            o->res_name.assign(res_name->Buffer, res_name->Length / sizeof(WCHAR));
        } else {
            o->res_is_int = true;
            o->res_id = static_cast<ULONG_PTR>(LOWORD(reinterpret_cast<ULONG_PTR>(res_name->Buffer)));
        }
    }
    if (module && module->Length) o->module.assign(module->Buffer, module->Length / sizeof(WCHAR));

    if (o->is_ani) {
        for (UINT i = 0; i < desc->num_steps; ++i) {
            if (o->frames[i]) continue;   // already set (a repeated frame)
            DWORD frame_id = desc->frame_seq ? desc->frame_seq[i] : i;
            if (frame_id >= o->num_frames) {
                frame_id = o->num_frames ? o->num_frames - 1 : 0;
                ERR(user, "cursor sequence indicates a frame past the end of the list");
            }
            const HICON fh = NewIcon(o->is_icon);   // the objects are separate allocations: `o` stays valid
            if (!fh) return FALSE;
            o->frames[i] = fh;
            IconObj* fo = Icon(fh);
            fo->delay = desc->frame_rates ? desc->frame_rates[i] : desc->delay;
            fo->frame = desc->frames[frame_id];
            if (desc->frame_seq)
                for (UINT j = i + 1; j < o->num_steps; ++j)
                    if (desc->frame_seq[j] == frame_id) o->frames[j] = fh;
        }
    }
    if (desc->flags & LR_SHARED) {
        o->is_shared = true;
        if (!o->module.empty()) {
            o->rsrc = desc->rsrc;
            T().shared.insert(T().shared.begin(), o);
        }
    }
    return TRUE;
}

HICON WINAPI NtUserFindExistingCursorIcon(UNICODE_STRING* module, UNICODE_STRING* res_name, void* desc) {
    (void)res_name;
    LockGuard lock;
    if (!module) return nullptr;
    const std::size_t chars = module->Length / sizeof(WCHAR);
    for (IconObj* o : T().shared) {
        if (o->module.size() != chars) continue;
        if (chars && std::memcmp(o->module.data(), module->Buffer, module->Length)) continue;
        if (o->rsrc != static_cast<HRSRC>(desc)) continue;   // user32 passes the resource handle as `desc`
        return o->handle;
    }
    return nullptr;
}

BOOL WINAPI NtUserDestroyCursor(HCURSOR cursor, ULONG arg) {
    (void)arg;
    std::vector<UINT64> callbacks;
    BOOL ret;
    {
        LockGuard lock;
        IconObj* o = Icon(cursor);
        if (!o) return FALSE;
        const bool shared = o->is_shared;
        winexbox::IWinexboxDriver* d = Driver();
        ret = (d ? reinterpret_cast<HCURSOR>(d->CursorCurrent()) : nullptr) != cursor;
        if (!shared) FreeIcon(cursor, &callbacks);
    }
    for (UINT64 cb : callbacks) {   // user32's free-icon dispatch callback (16-bit thunks only), outside the lock
        abi::DispatchCallbackParams p{cb};
        void* rp = nullptr;
        ULONG rl = 0;
        UserModeCallback(abi::CbCallDispatchCallback, &p, sizeof p, &rp, &rl);
    }
    return ret;
}

BOOL WINAPI NtUserGetIconSize(HICON handle, UINT step, LONG* width, LONG* height) {
    LockGuard lock;
    IconObj* o = FrameObj(handle, step);
    if (!o || !width || !height) {
        SetLastErr(ERROR_INVALID_CURSOR_HANDLE);
        return FALSE;
    }
    *width = static_cast<LONG>(o->frame.width);
    *height = static_cast<LONG>(o->frame.height * 2);
    return TRUE;
}

HCURSOR WINAPI NtUserGetCursorFrameInfo(HCURSOR cursor, DWORD istep, DWORD* rate_jiffies, DWORD* num_steps) {
    if (!rate_jiffies || !num_steps) return nullptr;
    LockGuard lock;
    IconObj* o = Icon(cursor);
    if (!o) return nullptr;
    HCURSOR ret = nullptr;
    const UINT icon_steps = o->is_ani ? o->num_steps : 1;
    if (istep < icon_steps || !o->is_ani) {
        const UINT icon_frames = o->is_ani ? o->num_frames : 1;
        ret = (o->is_ani && icon_frames > 1) ? o->frames[istep] : cursor;
        if (icon_frames == 1) {
            *rate_jiffies = 0;
            *num_steps = 1;
        } else if (icon_steps == 1) {
            *num_steps = ~0u;
            *rate_jiffies = o->delay;
        } else if (istep < icon_steps) {
            IconObj* frame = Icon(o->frames[istep]);
            *num_steps = o->num_steps == 1 ? ~0u : o->num_steps;
            *rate_jiffies = frame ? frame->delay : 0;
        }
    }
    return ret;
}

BOOL WINAPI NtUserGetIconInfo(HICON icon, ICONINFO* info, UNICODE_STRING* module, UNICODE_STRING* res_name, DWORD* bpp, LONG unk) {
    (void)unk;
    LockGuard lock;
    IconObj* o = Icon(icon);
    if (!o || !info) {
        SetLastErr(ERROR_INVALID_CURSOR_HANDLE);
        return FALSE;
    }
    IconObj* f = FrameObj(icon, 0);
    if (!f) return FALSE;
    info->fIcon = o->is_icon;
    info->xHotspot = static_cast<DWORD>(f->frame.hotspot.x);
    info->yHotspot = static_cast<DWORD>(f->frame.hotspot.y);
    info->hbmColor = CopyIconBitmap(f->frame.color);
    info->hbmMask = CopyIconBitmap(f->frame.mask);
    if (!info->hbmMask || (!info->hbmColor && f->frame.color)) {
        if (info->hbmMask) GdiDelete(info->hbmMask);
        if (info->hbmColor) GdiDelete(info->hbmColor);
        info->hbmMask = info->hbmColor = nullptr;
        return FALSE;
    }
    if (bpp) {
        auto* c = GdiGetT<BitmapObj>(f->frame.color, abi::kObjBitmap);
        *bpp = c ? (c->dib ? c->dib_header.biBitCount : c->bpp) : 1;
    }
    if (!o->module.empty()) {
        if (module) {
            const std::size_t size = std::min<std::size_t>(module->MaximumLength, o->module.size() * sizeof(WCHAR));
            if (size) std::memcpy(module->Buffer, o->module.data(), size);
            module->Length = static_cast<USHORT>(size / sizeof(WCHAR));   // in characters, as win32u does
        }
        if (res_name) {
            if (o->res_is_int) {
                res_name->Buffer = reinterpret_cast<PWSTR>(o->res_id);
                res_name->Length = 0;
            } else {
                const std::size_t size = std::min<std::size_t>(res_name->MaximumLength, o->res_name.size() * sizeof(WCHAR));
                if (size) std::memcpy(res_name->Buffer, o->res_name.data(), size);
                res_name->Length = static_cast<USHORT>(size / sizeof(WCHAR));
            }
        }
    } else {
        if (module) module->Length = 0;
        if (res_name) {
            res_name->Length = 0;
            res_name->Buffer = nullptr;
        }
    }
    return TRUE;
}

BOOL WINAPI NtUserGetCursorInfo(CURSORINFO* info) {
    if (!info) return FALSE;
    LockGuard lock;
    if (info->cbSize != sizeof(CURSORINFO)) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    winexbox::IWinexboxDriver* d = Driver();
    info->hCursor = d ? reinterpret_cast<HCURSOR>(d->CursorCurrent()) : nullptr;
    info->flags = CursorShowCount() >= 0 ? CURSOR_SHOWING : 0;
    info->ptScreenPos = S().cursor;
    return TRUE;
}

namespace {
// Composites a pre-multiplied BGRA bitmap, nearest-neighbour scaled to width x height, over the DC at (x, y).
bool AlphaBlitIcon(HDC hdc, int x, int y, int width, int height, const abi::CursorIconFrame& fr) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    auto* src = GdiGetT<BitmapObj>(fr.alpha, abi::kObjBitmap);
    if (!dc || !src || width <= 0 || height <= 0 || src->width <= 0 || src->height <= 0) return false;
    PixelTarget sp;
    if (!BitmapTarget(*src, &sp)) return false;
    DrawTarget t;
    if (!ResolveDrawTarget(dc, &t)) return false;
    const POINT origin = LogicalToDevice(dc, x, y);
    for (int dy = 0; dy < height; ++dy) {
        const int sy = static_cast<int>(static_cast<long long>(dy) * src->height / height);
        const int py = origin.y + dy;
        for (int dx = 0; dx < width; ++dx) {
            const int px = origin.x + dx;
            if (!t.clip.Contains(px, py)) continue;
            const int tx = px + t.origin.x, ty = py + t.origin.y;
            if (tx < 0 || ty < 0 || tx >= t.px.width || ty >= t.px.height) continue;
            const int sx = static_cast<int>(static_cast<long long>(dx) * src->width / width);
            const std::uint32_t s = sp.Row(sy)[sx];
            const std::uint32_t sa = s >> 24;
            if (sa == 0) continue;
            std::uint32_t& d = t.px.Row(ty)[tx];
            if (sa == 255) {
                d = s | 0xff000000u;
                continue;
            }
            const std::uint32_t inv = 255 - sa;
            auto ch = [&](int shift) {
                const std::uint32_t sc = (s >> shift) & 0xff, dc2 = (d >> shift) & 0xff;
                return std::min<std::uint32_t>(255, sc + (dc2 * inv + 127) / 255) << shift;
            };
            d = 0xff000000u | ch(16) | ch(8) | ch(0);
        }
    }
    NoteDrawn(dc, t, RECT{origin.x, origin.y, origin.x + width, origin.y + height});
    return true;
}
}  // namespace

// DrawIconEx: win32u's sequence of blits through the GDI calls (they take the lock themselves).
BOOL WINAPI NtUserDrawIconEx(HDC hdc, INT x0, INT y0, HICON icon, INT width, INT height, UINT step, HBRUSH brush, UINT flags) {
    abi::CursorIconFrame fr{};
    bool is_brush = false;
    {
        LockGuard lock;
        IconObj* o = FrameObj(icon, step);
        if (!o) return FALSE;
        fr = o->frame;
        is_brush = brush && GdiGetT<BrushObj>(brush, abi::kObjBrush) != nullptr;
    }
    HDC mem_dc = NtGdiCreateCompatibleDC(hdc);
    if (!mem_dc) return FALSE;
    if (width == 0) width = (flags & DI_DEFAULTSIZE) ? 32 : static_cast<INT>(fr.width);
    if (height == 0) height = (flags & DI_DEFAULTSIZE) ? 32 : static_cast<INT>(fr.height);

    HDC dest = hdc;
    HBITMAP offscreen = nullptr;
    INT x = x0, y = y0;
    if (is_brush) {
        // Paint the background brush into an off-screen bitmap, draw the icon over it, copy the result.
        dest = NtGdiCreateCompatibleDC(hdc);
        offscreen = dest ? NtGdiCreateCompatibleBitmap(hdc, width, height) : nullptr;
        if (!offscreen) {
            if (dest) NtGdiDeleteObjectApp(dest);
            NtGdiDeleteObjectApp(mem_dc);
            return FALSE;
        }
        NtGdiSelectBitmap(dest, offscreen);
        HGDIOBJ prev = NtGdiSelectBrush(dest, brush);
        NtGdiPatBlt(dest, 0, 0, width, width, PATCOPY);
        if (prev) NtGdiSelectBrush(dest, static_cast<HBRUSH>(prev));
        x = y = 0;
    }

    DWORD old_fg = 0, old_bg = 0;
    NtGdiGetAndSetDCDword(hdc, abi::DcSetTextColor, RGB(0, 0, 0), &old_fg);
    NtGdiGetAndSetDCDword(hdc, abi::DcSetBkColor, RGB(255, 255, 255), &old_bg);

    bool done = false;
    if (fr.alpha && (flags & DI_IMAGE)) {
        bool alpha_ok = true;
        {
            LockGuard lock;
            DcObj* dd = DcGet(dest);
            if (dd && dd->kind == DcKind::Memory) {
                auto* sel = GdiGetT<BitmapObj>(dd->bitmap, abi::kObjBitmap);
                alpha_ok = sel && (sel->dib ? sel->dib_header.biBitCount : sel->bpp) > 8;
            }
        }
        if (alpha_ok) done = AlphaBlitIcon(dest, x, y, width, height, fr);
    }
    if (!done) {
        if (flags & DI_MASK) {
            const DWORD rop = (flags & DI_IMAGE) ? SRCAND : SRCCOPY;
            NtGdiSelectBitmap(mem_dc, fr.mask);
            NtGdiStretchBlt(dest, x, y, width, height, mem_dc, 0, 0, static_cast<INT>(fr.width), static_cast<INT>(fr.height), rop, 0);
        }
        if (flags & DI_IMAGE) {
            const DWORD rop = (flags & DI_MASK) ? SRCINVERT : SRCCOPY;
            if (fr.color) {
                NtGdiSelectBitmap(mem_dc, fr.color);
                NtGdiStretchBlt(dest, x, y, width, height, mem_dc, 0, 0, static_cast<INT>(fr.width), static_cast<INT>(fr.height), rop, 0);
            } else {
                // a monochrome icon: the colour half follows the mask in one bitmap
                NtGdiSelectBitmap(mem_dc, fr.mask);
                NtGdiStretchBlt(dest, x, y, width, height, mem_dc, 0, static_cast<INT>(fr.height), static_cast<INT>(fr.width),
                                static_cast<INT>(fr.height), rop, 0);
            }
        }
    }
    if (offscreen) NtGdiBitBlt(hdc, x0, y0, width, height, dest, 0, 0, SRCCOPY, 0, 0);
    DWORD ignored = 0;
    NtGdiGetAndSetDCDword(hdc, abi::DcSetTextColor, old_fg, &ignored);
    NtGdiGetAndSetDCDword(hdc, abi::DcSetBkColor, old_bg, &ignored);
    if (dest != hdc) NtGdiDeleteObjectApp(dest);
    if (offscreen) NtGdiDeleteObjectApp(offscreen);
    NtGdiDeleteObjectApp(mem_dc);
    return TRUE;
}

}  // namespace sys
}  // namespace win10xbox::user
