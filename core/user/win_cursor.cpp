// core/user cursors and icons: the objects behind NtUserSetCursorIconData, the pointer's shape per thread, the show
// count, and the drawing of the cursor onto the composed frame and of an icon onto a DC (DrawIconEx).
//
// Wine's user32 loads the image itself (LoadCursorW(NULL, IDC_ARROW) reads the resource from user32.dll, builds the
// bitmaps with GDI and hands them over in a `struct cursoricon_desc`); this module only keeps them: the frame (a mask
// bitmap, a colour bitmap for colour images, a premultiplied alpha bitmap for 32 bpp images) and the hotspot. The
// handle is (generation << 16) | (slot + 1), never a window or GDI handle.
//
// Pixels (the same blender serves the composed cursor and DrawIconEx):
//  * a frame with an alpha bitmap: premultiplied source-over, dst = src + dst * (255 - a) / 255;
//  * a colour frame: dst = (dst AND mask) XOR colour, mask bit 1 = keep the destination;
//  * a monochrome frame (no colour bitmap): the mask bitmap is twice as high, the upper half is the AND mask and the
//    lower half the XOR bitmap: dst = (dst AND upper) XOR lower.
// Not done: animated cursors (SetCursorIconData with steps fails), sizing and other non-client cursors chosen by
// DefWindowProc (a non-client hit shows the built-in arrow), cursor shadows, DI_NOMIRROR, bitmaps and brushes other than
// a solid one as DrawIconEx's background.
#include "user_internal.h"
#include "user_syscalls.h"

#include <algorithm>
#include <cstring>

namespace win10xbox::user {
namespace detail {

struct CursorObj {
    HCURSOR handle = nullptr;
    bool is_icon = false;
    bool ready = false;                 // NtUserSetCursorIconData has run
    bool shared = false;                // LR_SHARED: DestroyCursor leaves it alone, FindExistingCursorIcon finds it
    UINT width = 0, height = 0;
    HGDIOBJ color = nullptr, alpha = nullptr, mask = nullptr;   // the object owns the three bitmaps
    POINT hotspot{};
    UINT delay = 0;
    std::wstring module;                // the DLL a resource-loaded image came from (full name)
    bool res_is_id = true;
    ULONG_PTR res_id = 0;
    std::wstring res_name;
    HRSRC rsrc = nullptr;
};

namespace {

constexpr UINT kDiNormal = DI_IMAGE | DI_MASK;

HCURSOR EncodeCursor(std::uint32_t slot, WORD generation) {
    return reinterpret_cast<HCURSOR>(static_cast<ULONG_PTR>((static_cast<std::uint32_t>(generation) << 16) | (slot + 1)));
}

CursorObj* CursorGet(HCURSOR h) {
    State& s = S();
    const ULONG_PTR v = reinterpret_cast<ULONG_PTR>(h);
    if (!v || (v >> 32)) return nullptr;
    const std::uint32_t slot = static_cast<std::uint32_t>(v & 0xffff) - 1;   // wraps for 0
    const WORD gen = static_cast<WORD>((v >> 16) & 0xffff);
    if (slot >= s.cursors.size() || !s.cursors[slot]) return nullptr;
    if (gen && gen != s.cursor_gens[slot]) return nullptr;
    return s.cursors[slot];
}

void FreeCursor(CursorObj* c) {
    State& s = S();
    const std::uint32_t slot = static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(c->handle) & 0xffff) - 1;
    for (UserThread* t : s.threads)
        if (t->cursor == c->handle) t->cursor = nullptr;
    for (HGDIOBJ h : {c->alpha, c->color, c->mask})
        if (h) GdiDelete(h);
    s.cursors[slot] = nullptr;
    WORD& gen = s.cursor_gens[slot];
    gen = gen >= 0x7fff ? 1 : static_cast<WORD>(gen + 1);
    s.cursor_free.push_back(slot);
    delete c;
}

bool MaskBit(const BitmapObj& m, int x, int y) { return (BitmapPixel(m, x, y) & 0xffffff) != 0; }

// The 32 bpp DIB section's pixel with its alpha byte (BitmapPixel forces 0xff).
std::uint32_t RawPixel(const BitmapObj& b, int x, int y) {
    if (!b.dib || b.dib_header.biBitCount != 32 || x < 0 || y < 0 || x >= b.width || y >= b.height) return BitmapPixel(b, x, y);
    const int row = b.dib_bottom_up ? b.height - 1 - y : y;
    std::uint32_t px;
    std::memcpy(&px, static_cast<const std::uint8_t*>(b.dib_bits) + static_cast<std::ptrdiff_t>(row) * b.dib_stride + 4 * static_cast<std::ptrdiff_t>(x), 4);
    return px;
}

std::uint32_t BlendPremultiplied(std::uint32_t dst, std::uint32_t src) {
    const unsigned a = src >> 24;
    auto ch = [&](int shift) {
        const unsigned d = (dst >> shift) & 0xff, s = (src >> shift) & 0xff;
        return std::min<unsigned>(255, s + (d * (255 - a) + 127) / 255) << shift;
    };
    return 0xff000000u | ch(16) | ch(8) | ch(0);
}

// Draws the frame of `c` stretched to dw x dh. `put(dx, dy)` is the destination pixel (dx, dy counted from the image's
// top-left) or null when it is clipped. `flags` are DrawIconEx's DI_IMAGE / DI_MASK.
template <class Put>
void RenderCursor(const CursorObj& c, int dw, int dh, UINT flags, Put put) {
    const auto* mask = GdiGetT<BitmapObj>(c.mask, abi::kObjBitmap);
    const auto* color = c.color ? GdiGetT<BitmapObj>(c.color, abi::kObjBitmap) : nullptr;
    const auto* alpha = c.alpha ? GdiGetT<BitmapObj>(c.alpha, abi::kObjBitmap) : nullptr;
    if (!mask || dw <= 0 || dh <= 0 || !c.width || !c.height) return;
    const bool image = (flags & DI_IMAGE) != 0, use_mask = (flags & DI_MASK) != 0;
    for (int dy = 0; dy < dh; ++dy) {
        const int sy = static_cast<int>(static_cast<long long>(dy) * c.height / dh);
        for (int dx = 0; dx < dw; ++dx) {
            std::uint32_t* px = put(dx, dy);
            if (!px) continue;
            const int sx = static_cast<int>(static_cast<long long>(dx) * c.width / dw);
            if (alpha && image) {
                *px = BlendPremultiplied(*px, RawPixel(*alpha, sx, sy));
                continue;
            }
            std::uint32_t v = *px & 0xffffff;
            const bool keep = MaskBit(*mask, sx, sy);
            if (use_mask) v = image ? (keep ? v : 0) : (keep ? 0xffffff : 0);
            if (image) {
                const std::uint32_t x = color ? (BitmapPixel(*color, sx, sy) & 0xffffff)
                                              : (MaskBit(*mask, sx, sy + static_cast<int>(c.height)) ? 0xffffffu : 0u);
                v = use_mask ? (v ^ x) : x;
            }
            *px = 0xff000000u | v;
        }
    }
}

// The arrow shown when no application cursor applies (a thread that never called SetCursor, the desktop, a non-client
// area): the classic white arrow with a black outline, hotspot (0,0). X black, O white.
constexpr const char* kBuiltinArrow[] = {
    "X           ", "XX          ", "XOX         ", "XOOX        ", "XOOOX       ", "XOOOOX      ",
    "XOOOOOX     ", "XOOOOOOX    ", "XOOOOOOOX   ", "XOOOOOOOOX  ", "XOOOOOOOOOX ", "XOOOOOOXXXXX",
    "XOOXOOX     ", "XOX XOOX    ", "XX  XOOX    ", "X    XOOX   ", "     XOOX   ", "      XX    ",
};
constexpr int kBuiltinArrowRows = sizeof(kBuiltinArrow) / sizeof(kBuiltinArrow[0]);

CursorObj* ValidCursor(HCURSOR h) {
    CursorObj* c = CursorGet(h);
    return c && c->ready ? c : nullptr;
}

// A private copy of a bitmap (what GetIconInfo hands out, owned by the caller).
HGDIOBJ CloneBitmap(HGDIOBJ h) {
    const auto* src = GdiGetT<BitmapObj>(h, abi::kObjBitmap);
    if (!src) return nullptr;
    auto* copy = new BitmapObj;
    copy->width = src->width;
    copy->height = src->height;
    copy->bpp = src->bpp == 1 ? 1 : 32;
    copy->pixels.resize(static_cast<std::size_t>(copy->width) * copy->height);
    for (int y = 0; y < copy->height; ++y)
        for (int x = 0; x < copy->width; ++x)
            copy->pixels[static_cast<std::size_t>(y) * copy->width + x] = BitmapPixel(*src, x, y);
    return GdiAlloc(copy, abi::kObjBitmap);
}

}  // namespace

HCURSOR CreateCursorObject(bool is_icon) {
    State& s = S();
    std::uint32_t slot;
    if (!s.cursor_free.empty()) {
        slot = s.cursor_free.back();
        s.cursor_free.pop_back();
    } else {
        if (s.cursors.size() >= 0xfffe) return nullptr;
        slot = static_cast<std::uint32_t>(s.cursors.size());
        s.cursors.push_back(nullptr);
        s.cursor_gens.push_back(1);
    }
    auto* c = new CursorObj;
    c->is_icon = is_icon;
    c->handle = EncodeCursor(slot, s.cursor_gens[slot]);
    s.cursors[slot] = c;
    return c->handle;
}

HCURSOR ThreadCursor(const UserThread& t) {
    return t.cursor && ValidCursor(t.cursor) ? t.cursor : nullptr;
}

bool SetThreadCursor(UserThread& t, HCURSOR cursor) {
    if (cursor && !ValidCursor(cursor)) return false;
    t.cursor = cursor;
    t.cursor_set = true;
    return true;
}

void ComposeCursor(std::uint32_t* bgra, std::int32_t width, std::int32_t height, std::int32_t stride_bytes) {
    State* sp = StateIfAny();
    if (!sp || sp->cursor_show < 0) return;
    POINT pos = sp->cursor;
    winexbox::Point dp{};
    if (Driver() && Driver()->CursorGetPos(&dp)) pos = POINT{dp.x, dp.y};
    const UserThread* owner = nullptr;
    if (WindowObj* w = Win(WindowFromScreenPoint(pos))) owner = w->thread;
    auto pixel = [&](int x, int y) -> std::uint32_t* {
        if (x < 0 || y < 0 || x >= width || y >= height) return nullptr;
        return reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(bgra) + static_cast<std::ptrdiff_t>(y) * stride_bytes) + x;
    };
    if (owner && owner->cursor_set) {
        const CursorObj* c = ValidCursor(owner->cursor);   // SetCursor(NULL): no cursor at all
        if (!c) return;
        const int ox = pos.x - c->hotspot.x, oy = pos.y - c->hotspot.y;
        RenderCursor(*c, static_cast<int>(c->width), static_cast<int>(c->height), kDiNormal,
                     [&](int dx, int dy) { return pixel(ox + dx, oy + dy); });
        return;
    }
    for (int dy = 0; dy < kBuiltinArrowRows; ++dy)
        for (int dx = 0; kBuiltinArrow[dy][dx]; ++dx) {
            const char ch = kBuiltinArrow[dy][dx];
            if (ch == ' ') continue;
            if (std::uint32_t* px = pixel(pos.x + dx, pos.y + dy)) *px = ch == 'X' ? 0xff000000u : 0xffffffffu;
        }
}

// ---- Syscalls -----------------------------------------------------------------------------------
}  // namespace detail

namespace sys {
using namespace detail;

HCURSOR WINAPI NtUserSetCursor(HCURSOR cursor) {
    LockGuard lock;
    UserThread& t = CurrentUserThread();
    const HCURSOR prev = ThreadCursor(t);
    if (!SetThreadCursor(t, cursor)) {
        SetLastErr(ERROR_INVALID_CURSOR_HANDLE);
        return nullptr;
    }
    return prev;
}

HCURSOR WINAPI NtUserGetCursor() {
    LockGuard lock;
    return ThreadCursor(CurrentUserThread());
}

INT WINAPI NtUserShowCursor(BOOL show) {
    LockGuard lock;
    State& s = S();
    s.cursor_show += show ? 1 : -1;
    return s.cursor_show;
}

BOOL WINAPI NtUserGetCursorInfo(CURSORINFO* info) {
    LockGuard lock;
    if (!info) return FALSE;
    State& s = S();
    info->hCursor = ThreadCursor(CurrentUserThread());
    info->flags = s.cursor_show >= 0 ? CURSOR_SHOWING : 0;
    info->ptScreenPos = s.cursor;
    winexbox::Point p{};
    if (Driver() && Driver()->CursorGetPos(&p)) info->ptScreenPos = s.cursor = POINT{p.x, p.y};
    return TRUE;
}

BOOL WINAPI NtUserClipCursor(const RECT* rect) {
    LockGuard lock;
    State& s = S();
    if (rect && (rect->left > rect->right || rect->top > rect->bottom)) return FALSE;
    winexbox::IWinexboxDriver* d = Driver();
    s.clip_set = rect != nullptr;
    if (rect) s.clip = *rect;
    if (!d) return TRUE;
    if (!rect) return d->CursorClip(nullptr, true);
    const winexbox::Rect r{rect->left, rect->top, rect->right, rect->bottom};
    return d->CursorClip(&r, false);
}

BOOL WINAPI NtUserGetClipCursor(RECT* rect) {
    LockGuard lock;
    if (!rect) return FALSE;
    ScreenRect(rect);
    State& s = S();
    if (s.clip_set) *rect = RectIntersect(*rect, s.clip);
    return TRUE;
}

BOOL WINAPI NtUserSetCursorIconData(HCURSOR cursor, UNICODE_STRING* module, UNICODE_STRING* res_name,
                                    abi::CursorIconDesc* desc) {
    LockGuard lock;
    CursorObj* c = CursorGet(cursor);
    if (!c || !desc) return FALSE;
    if (c->ready) {
        SetLastErr(ERROR_INVALID_CURSOR_HANDLE);
        return FALSE;
    }
    if (desc->num_steps) {
        FIXME(user, "animated cursor %p (%u steps) is not supported", cursor, desc->num_steps);
        SetLastErr(ERROR_CALL_NOT_IMPLEMENTED);
        return FALSE;
    }
    if (!desc->frames || !GdiGetT<BitmapObj>(desc->frames[0].mask, abi::kObjBitmap)) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const abi::CursorIconFrame& f = desc->frames[0];
    c->width = f.width;
    c->height = f.height;
    c->color = f.color;
    c->alpha = f.alpha;
    c->mask = f.mask;
    c->hotspot = f.hotspot;
    c->delay = desc->delay;
    if (res_name) {
        if (res_name->Length) {
            c->res_is_id = false;
            c->res_name.assign(res_name->Buffer, res_name->Length / sizeof(WCHAR));
        } else {
            c->res_id = LOWORD(reinterpret_cast<ULONG_PTR>(res_name->Buffer));
        }
    }
    if (module && module->Length && module->Buffer) c->module.assign(module->Buffer, module->Length / sizeof(WCHAR));
    if (desc->flags & LR_SHARED) {
        c->shared = true;
        c->rsrc = c->module.empty() ? nullptr : desc->rsrc;
    }
    c->ready = true;
    TRACE(user, "cursor %p: %ux%u hotspot %ld,%ld color %p alpha %p mask %p", cursor, c->width, c->height, c->hotspot.x,
          c->hotspot.y, c->color, c->alpha, c->mask);
    return TRUE;
}

HICON WINAPI NtUserFindExistingCursorIcon(UNICODE_STRING* module, UNICODE_STRING* res_name, void* desc) {
    LockGuard lock;
    (void)res_name;   // Wine matches on the module and the resource handle only
    if (!module) return nullptr;
    for (CursorObj* c : S().cursors) {
        if (!c || !c->ready || !c->shared || c->module.empty() || c->rsrc != desc) continue;
        if (c->module.size() * sizeof(WCHAR) != module->Length) continue;
        if (std::memcmp(c->module.data(), module->Buffer, module->Length)) continue;
        return c->handle;
    }
    return nullptr;
}

BOOL WINAPI NtUserDestroyCursor(HCURSOR cursor, ULONG arg) {
    LockGuard lock;
    (void)arg;
    CursorObj* c = CursorGet(cursor);
    if (!c) {
        SetLastErr(ERROR_INVALID_CURSOR_HANDLE);
        return FALSE;
    }
    const bool in_use = ThreadCursor(CurrentUserThread()) == c->handle;
    if (!c->shared) FreeCursor(c);
    return !in_use;
}

BOOL WINAPI NtUserGetIconSize(HICON handle, UINT step, LONG* width, LONG* height) {
    LockGuard lock;
    (void)step;
    CursorObj* c = ValidCursor(handle);
    if (!c || !width || !height) {
        SetLastErr(ERROR_INVALID_CURSOR_HANDLE);
        return FALSE;
    }
    *width = static_cast<LONG>(c->width);
    *height = static_cast<LONG>(c->height) * 2;   // user32 halves it again
    return TRUE;
}

HCURSOR WINAPI NtUserGetCursorFrameInfo(HCURSOR cursor, DWORD istep, DWORD* rate_jiffies, DWORD* num_steps) {
    LockGuard lock;
    (void)istep;
    if (!rate_jiffies || !num_steps || !ValidCursor(cursor)) return nullptr;
    *rate_jiffies = 0;   // no animated cursors: one step, no delay
    *num_steps = 1;
    return cursor;
}

BOOL WINAPI NtUserGetIconInfo(HICON icon, ICONINFO* info, UNICODE_STRING* module, UNICODE_STRING* res_name, DWORD* bpp,
                              LONG unk) {
    LockGuard lock;
    (void)bpp; (void)unk;
    CursorObj* c = ValidCursor(icon);
    if (!c || !info) {
        SetLastErr(ERROR_INVALID_CURSOR_HANDLE);
        return FALSE;
    }
    info->fIcon = c->is_icon;
    info->xHotspot = c->hotspot.x;
    info->yHotspot = c->hotspot.y;
    info->hbmColor = static_cast<HBITMAP>(c->color ? CloneBitmap(c->color) : nullptr);
    info->hbmMask = static_cast<HBITMAP>(CloneBitmap(c->mask));
    if (!info->hbmMask || (c->color && !info->hbmColor)) {
        if (info->hbmMask) GdiDelete(info->hbmMask);
        if (info->hbmColor) GdiDelete(info->hbmColor);
        info->hbmMask = info->hbmColor = nullptr;
        return FALSE;
    }
    if (!c->module.empty()) {
        if (module) {
            const size_t size = std::min<size_t>(module->MaximumLength, c->module.size() * sizeof(WCHAR));
            if (size) std::memcpy(module->Buffer, c->module.data(), size);
            module->Length = static_cast<USHORT>(size / sizeof(WCHAR));   // in characters, as Wine does
        }
        if (res_name) {
            if (c->res_is_id) {
                res_name->Buffer = reinterpret_cast<WCHAR*>(c->res_id);
                res_name->Length = 0;
            } else {
                const size_t size = std::min<size_t>(res_name->MaximumLength, c->res_name.size() * sizeof(WCHAR));
                if (size) std::memcpy(res_name->Buffer, c->res_name.data(), size);
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

BOOL WINAPI NtUserDrawIconEx(HDC hdc, INT x0, INT y0, HICON icon, INT width, INT height, UINT step, HBRUSH brush,
                             UINT flags) {
    LockGuard lock;
    (void)step;
    CursorObj* c = ValidCursor(icon);
    DcObj* dc = DcGet(hdc);
    if (!c || !dc) {
        SetLastErr(c ? ERROR_INVALID_HANDLE : ERROR_INVALID_CURSOR_HANDLE);
        return FALSE;
    }
    if (width == 0) width = (flags & DI_DEFAULTSIZE) ? SystemMetric(SM_CXICON) : static_cast<INT>(c->width);
    if (height == 0) height = (flags & DI_DEFAULTSIZE) ? SystemMetric(SM_CYICON) : static_cast<INT>(c->height);
    if (width <= 0 || height <= 0 || !(flags & (DI_IMAGE | DI_MASK))) return TRUE;
    DrawTarget t;
    if (!ResolveDrawTarget(dc, &t)) {
        FIXME(gdi, "DrawIconEx onto a DC without a 32 bpp surface is not supported");
        return FALSE;
    }
    const POINT o = LogicalToDevice(dc, x0, y0);
    const RECT box{o.x, o.y, o.x + width, o.y + height};
    if (auto* br = GdiGetT<BrushObj>(brush, abi::kObjBrush)) {
        if (br->style == BS_SOLID) FillTargetRect(t, box, ColorToPixel(br->color));
        else if (br->style != BS_NULL) FIXME(gdi, "DrawIconEx background brush style %u is not supported", br->style);
    }
    RenderCursor(*c, width, height, flags, [&](int dx, int dy) -> std::uint32_t* {
        const int x = o.x + dx, y = o.y + dy;
        if (!t.clip.Contains(x, y)) return nullptr;
        const int tx = x + t.origin.x, ty = y + t.origin.y;
        if (tx < 0 || ty < 0 || tx >= t.px.width || ty >= t.px.height) return nullptr;
        return t.px.Row(ty) + tx;
    });
    NoteDrawn(dc, t, box);
    return TRUE;
}

}  // namespace sys
}  // namespace win10xbox::user
