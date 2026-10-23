// core/user rasteriser: GDI drawing on BGRA32 targets (window surfaces from the
// display driver, memory-DC bitmaps, 32 bpp DIB sections). Every primitive
// clips to the DC's region (DrawTarget::clip, device coordinates) and writes
// the alpha byte as 0xFF. Ternary raster operations are evaluated bitwise on
// the 24 colour bits, with fast paths for the common ones. Text is the built-in
// 8x16 bitmap face (font8x16.h), scaled by whole factors for tall LOGFONTs.
#include "user_internal.h"
#include "user_syscalls.h"
#include "font8x16.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace win10xbox::user {
namespace detail {

std::uint32_t ColorToPixel(COLORREF c) {
    return 0xff000000u | (static_cast<std::uint32_t>(GetRValue(c)) << 16) | (static_cast<std::uint32_t>(GetGValue(c)) << 8) |
           GetBValue(c);
}

COLORREF PixelToColor(std::uint32_t px) {
    return RGB((px >> 16) & 0xff, (px >> 8) & 0xff, px & 0xff);
}

void FillTargetRect(const DrawTarget& t, RECT r, std::uint32_t bgra) {
    for (const RECT& c : t.clip.rects) {
        const RECT x = RectIntersect(r, c);
        if (RectEmpty(x)) continue;
        for (int y = x.top; y < x.bottom; ++y) {
            const int ty = y + t.origin.y;
            if (ty < 0 || ty >= t.px.height) continue;
            std::uint32_t* row = t.px.Row(ty);
            const int x0 = std::max<int>(x.left + t.origin.x, 0), x1 = std::min<int>(x.right + t.origin.x, t.px.width);
            for (int xx = x0; xx < x1; ++xx) row[xx] = bgra;
        }
    }
}

namespace {

// ---- Raster operations -------------------------------------------------------------------
inline std::uint32_t Rop3(std::uint8_t code, std::uint32_t p, std::uint32_t s, std::uint32_t d) {
    switch (code) {
    case 0x00: return 0;
    case 0xff: return 0xffffff;
    case 0xcc: return s;
    case 0xf0: return p;
    case 0xaa: return d;
    case 0x33: return ~s;
    case 0x55: return ~d;
    case 0x0f: return ~p;
    case 0x66: return s ^ d;
    case 0x88: return s & d;
    case 0xee: return s | d;
    case 0x5a: return p ^ d;
    case 0xc0: return p & s;
    case 0xbb: return ~s | d;
    case 0x44: return s & ~d;
    case 0x22: return ~s & d;
    }
    std::uint32_t r = 0;
    for (int i = 0; i < 8; ++i)
        if (code & (1 << i)) r |= ((i & 4) ? p : ~p) & ((i & 2) ? s : ~s) & ((i & 1) ? d : ~d);
    return r;
}
inline bool RopUsesSource(std::uint8_t code) { return ((code >> 2) & 0x33) != (code & 0x33); }
inline bool RopUsesPattern(std::uint8_t code) { return ((code >> 4) & 0x0f) != (code & 0x0f); }

inline std::uint32_t Rop2(int rop2, std::uint32_t p, std::uint32_t d) {
    const unsigned code = static_cast<unsigned>(rop2 - 1) & 0xf;
    switch (rop2) {
    case R2_COPYPEN: return p;
    case R2_XORPEN: return p ^ d;
    case R2_NOT: return ~d;
    case R2_BLACK: return 0;
    case R2_WHITE: return 0xffffff;
    case R2_NOP: return d;
    }
    std::uint32_t r = 0;
    for (int i = 0; i < 4; ++i)
        if (code & (1u << i)) r |= ((i & 2) ? p : ~p) & ((i & 1) ? d : ~d);
    return r;
}

// ---- Brushes --------------------------------------------------------------------------------
const std::uint8_t kHatch[6][8] = {
    {0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00},   // HS_HORIZONTAL
    {0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08},   // HS_VERTICAL
    {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01},   // HS_FDIAGONAL
    {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80},   // HS_BDIAGONAL
    {0x08, 0x08, 0x08, 0xff, 0x08, 0x08, 0x08, 0x08},   // HS_CROSS
    {0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81},   // HS_DIAGCROSS
};

struct BrushPaint {
    bool null = true;
    bool solid = true;
    std::uint32_t color = 0;              // BGRA
    const BitmapObj* pattern = nullptr;
    int hatch = -1;
    std::uint32_t hatch_bg = 0;
    bool hatch_transparent = false;
    std::uint32_t fg = 0, bg = 0;         // mono pattern brushes: text / background colours
    POINT org{};
    std::uint32_t At(int x, int y) const {   // device coordinates
        if (solid) return color;
        int px = (x - org.x), py = (y - org.y);
        if (hatch >= 0) {
            px &= 7; py &= 7;
            return (kHatch[hatch][py] & (0x80 >> px)) ? color : hatch_bg;
        }
        if (pattern && pattern->width > 0 && pattern->height > 0) {
            px %= pattern->width; if (px < 0) px += pattern->width;
            py %= pattern->height; if (py < 0) py += pattern->height;
            const std::uint32_t v = BitmapPixel(*pattern, px, py);
            if (pattern->bpp == 1) return (v & 0xffffff) ? bg : fg;
            return v;
        }
        return color;
    }
};

BrushPaint BrushOf(const DcObj* dc, HGDIOBJ hbrush) {
    BrushPaint b;
    State& s = S();
    auto* br = GdiGetT<BrushObj>(hbrush, abi::kObjBrush);
    if (!br) return b;
    b.org = dc->attr->brush_org;
    if (br->style == BS_NULL) return b;
    b.null = false;
    if (br->handle == s.stock[DC_BRUSH]) {
        b.color = ColorToPixel(dc->attr->brush_color);
        return b;
    }
    b.color = ColorToPixel(br->color);
    if (br->style == BS_HATCHED) {
        b.solid = false;
        b.hatch = static_cast<int>(br->hatch) % 6;
        b.hatch_bg = ColorToPixel(dc->attr->background_color);
        if (dc->attr->background_mode == TRANSPARENT) FIXME(gdi, "transparent hatch brushes are drawn opaque");
    } else if (br->style == BS_PATTERN || br->style == BS_DIBPATTERN || br->style == BS_DIBPATTERNPT) {
        b.solid = false;
        b.pattern = GdiGetT<BitmapObj>(br->pattern, abi::kObjBitmap);
        b.fg = ColorToPixel(dc->attr->text_color);
        b.bg = ColorToPixel(dc->attr->background_color);
    }
    return b;
}

struct PenPaint {
    bool null = true;
    std::uint32_t color = 0;
    int width = 1;
};

PenPaint PenOf(const DcObj* dc) {
    PenPaint p;
    State& s = S();
    auto* pen = GdiGetT<PenObj>(dc->pen, abi::kObjPen);
    if (!pen || pen->style == PS_NULL) return p;
    p.null = false;
    p.color = ColorToPixel(pen->handle == s.stock[DC_PEN] ? dc->attr->pen_color : pen->color);
    p.width = std::max(1, LogicalToDeviceDx(dc, pen->width));
    if (pen->style != PS_SOLID && pen->style != PS_INSIDEFRAME) {
        static volatile LONG reported = 0;   // plain flag
        if (!InterlockedExchange(&reported, 1)) FIXME(gdi, "styled pens (dash/dot) are drawn solid");
    }
    return p;
}

// Applies `fn(x, y, &pixel)` over `r` (device coords) clipped to the target.
template <class Fn> void ForEachPixel(const DrawTarget& t, RECT r, Fn fn) {
    for (const RECT& c : t.clip.rects) {
        const RECT x = RectIntersect(r, c);
        if (RectEmpty(x)) continue;
        for (int y = x.top; y < x.bottom; ++y) {
            const int ty = y + t.origin.y;
            if (ty < 0 || ty >= t.px.height) continue;
            std::uint32_t* row = t.px.Row(ty);
            const int x0 = std::max<int>(x.left, -t.origin.x), x1 = std::min<int>(x.right, t.px.width - t.origin.x);
            for (int xx = x0; xx < x1; ++xx) fn(xx, y, row[xx + t.origin.x]);
        }
    }
}

// Horizontal span [x0, x1) at device row y.
template <class Fn> void ForSpan(const DrawTarget& t, int y, int x0, int x1, Fn fn) {
    if (x1 <= x0) return;
    ForEachPixel(t, RECT{x0, y, x1, y + 1}, fn);
}

void PaintRect(DcObj* dc, const DrawTarget& t, RECT r, const BrushPaint& b, std::uint8_t rop3 = 0xf0) {
    if (RectEmpty(r) || (b.null && RopUsesPattern(rop3))) return;
    if (b.solid && rop3 == 0xf0) {
        FillTargetRect(t, r, b.color);
    } else {
        ForEachPixel(t, r, [&](int x, int y, std::uint32_t& px) {
            px = 0xff000000u | (Rop3(rop3, b.At(x, y), 0, px) & 0xffffff);
        });
    }
    NoteDrawn(dc, t, r);
}

void PutPixel(const DrawTarget& t, int x, int y, std::uint32_t color, int rop2) {
    if (!t.clip.Contains(x, y)) return;
    const int tx = x + t.origin.x, ty = y + t.origin.y;
    if (tx < 0 || ty < 0 || tx >= t.px.width || ty >= t.px.height) return;
    std::uint32_t& px = t.px.Row(ty)[tx];
    px = 0xff000000u | (Rop2(rop2, color, px) & 0xffffff);
}

void PenDot(const DrawTarget& t, int x, int y, const PenPaint& pen, int rop2) {
    if (pen.width <= 1) {
        PutPixel(t, x, y, pen.color, rop2);
        return;
    }
    const int h = pen.width / 2;
    for (int yy = y - h; yy < y - h + pen.width; ++yy)
        for (int xx = x - h; xx < x - h + pen.width; ++xx) PutPixel(t, xx, yy, pen.color, rop2);
}

// Bresenham from (x0,y0) to (x1,y1), last point excluded (GDI convention).
void DrawLine(DcObj* dc, const DrawTarget& t, POINT a, POINT b, const PenPaint& pen) {
    if (pen.null) return;
    const int rop2 = dc->attr->rop_mode;
    int x0 = a.x, y0 = a.y;
    const int dx = std::abs(b.x - a.x), dy = -std::abs(b.y - a.y);
    const int sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
    int err = dx + dy;
    while (x0 != b.x || y0 != b.y) {
        PenDot(t, x0, y0, pen, rop2);
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    const int h = pen.width / 2;
    NoteDrawn(dc, t, RECT{std::min(a.x, b.x) - h, std::min(a.y, b.y) - h, std::max(a.x, b.x) + h + 1, std::max(a.y, b.y) + h + 1});
}

// Scanline polygon fill (even-odd or winding), device coordinates.
void FillPolygon(DcObj* dc, const DrawTarget& t, const std::vector<std::vector<POINT>>& polys, const BrushPaint& b,
                 bool winding) {
    if (b.null) return;
    int ymin = INT32_MAX, ymax = INT32_MIN;
    for (const auto& poly : polys)
        for (const POINT& p : poly) { ymin = std::min<int>(ymin, p.y); ymax = std::max<int>(ymax, p.y); }
    if (ymin >= ymax) return;
    RECT box{INT32_MAX, ymin, INT32_MIN, ymax};
    for (int y = ymin; y < ymax; ++y) {
        const double sy = y + 0.5;
        std::vector<std::pair<double, int>> xs;   // crossing x, direction
        for (const auto& poly : polys) {
            const std::size_t n = poly.size();
            for (std::size_t i = 0; i < n; ++i) {
                const POINT& p = poly[i];
                const POINT& q = poly[(i + 1) % n];
                if (p.y == q.y) continue;
                const bool up = p.y < q.y;
                const POINT& lo = up ? p : q;
                const POINT& hi = up ? q : p;
                if (sy < lo.y || sy >= hi.y) continue;
                const double x = lo.x + (sy - lo.y) * (hi.x - lo.x) / static_cast<double>(hi.y - lo.y);
                xs.emplace_back(x, up ? 1 : -1);
            }
        }
        std::sort(xs.begin(), xs.end());
        int wind = 0;
        for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
            wind += xs[i].second;
            const bool inside = winding ? wind != 0 : (i % 2 == 0);
            if (!inside) continue;
            const int x0 = static_cast<int>(std::ceil(xs[i].first - 0.5)), x1 = static_cast<int>(std::ceil(xs[i + 1].first - 0.5));
            box.left = std::min<int>(box.left, x0);
            box.right = std::max<int>(box.right, x1);
            ForSpan(t, y, x0, x1, [&](int x, int yy, std::uint32_t& px) { px = b.At(x, yy); });
        }
    }
    if (box.left < box.right) NoteDrawn(dc, t, box);
}

void FillEllipse(DcObj* dc, const DrawTarget& t, RECT r, const BrushPaint& b, const PenPaint& pen) {
    const double cx = (r.left + r.right) / 2.0, cy = (r.top + r.bottom) / 2.0;
    const double rx = (r.right - r.left) / 2.0, ry = (r.bottom - r.top) / 2.0;
    if (rx <= 0 || ry <= 0) return;
    const int pw = pen.null ? 0 : pen.width;
    for (int y = r.top; y < r.bottom; ++y) {
        const double dy = (y + 0.5 - cy) / ry;
        if (dy * dy > 1.0) continue;
        const double half = rx * std::sqrt(1.0 - dy * dy);
        const int x0 = static_cast<int>(std::lround(cx - half)), x1 = static_cast<int>(std::lround(cx + half));
        // inner ellipse for the outline
        const double irx = rx - pw, iry = ry - pw;
        int ix0 = x1, ix1 = x1;
        if (irx > 0 && iry > 0) {
            const double idy = (y + 0.5 - cy) / iry;
            if (idy * idy <= 1.0) {
                const double ih = irx * std::sqrt(1.0 - idy * idy);
                ix0 = static_cast<int>(std::lround(cx - ih));
                ix1 = static_cast<int>(std::lround(cx + ih));
            } else {
                ix0 = ix1 = x1;
            }
        }
        if (pw) {
            ForSpan(t, y, x0, std::min(ix0, x1), [&](int, int, std::uint32_t& px) { px = pen.color; });
            ForSpan(t, y, std::max(ix1, x0), x1, [&](int, int, std::uint32_t& px) { px = pen.color; });
        } else {
            ix0 = x0; ix1 = x1;
        }
        if (!b.null) ForSpan(t, y, ix0, ix1, [&](int x, int yy, std::uint32_t& px) { px = b.At(x, yy); });
    }
    NoteDrawn(dc, t, r);
}

// Source pixels for a blit.
struct Source {
    DcObj* dc = nullptr;
    DrawTarget t;
    bool direct = false;                 // reads through t.px
    BitmapObj* bmp = nullptr;            // otherwise through BitmapPixel
    bool mono = false;
    std::vector<std::uint32_t> copy;     // snapshot when source and destination overlap
    RECT copy_rect{};
    std::uint32_t Get(int x, int y) const {   // device coordinates of the source DC
        if (!copy.empty()) {
            if (!PtInRectangle(copy_rect, x, y)) return 0xff000000u;
            return copy[static_cast<std::size_t>(y - copy_rect.top) * (copy_rect.right - copy_rect.left) + (x - copy_rect.left)];
        }
        if (direct) {
            const int tx = x + t.origin.x, ty = y + t.origin.y;
            if (tx < 0 || ty < 0 || tx >= t.px.width || ty >= t.px.height) return 0xff000000u;
            return t.px.Row(ty)[tx];
        }
        if (bmp) return BitmapPixel(*bmp, x, y);
        return 0xff000000u;
    }
};

bool OpenSource(DcObj* src_dc, Source* s) {
    s->dc = src_dc;
    if (src_dc->kind == DcKind::Memory) {
        s->bmp = GdiGetT<BitmapObj>(src_dc->bitmap, abi::kObjBitmap);
        if (!s->bmp) return false;
        s->mono = !s->bmp->dib ? s->bmp->bpp == 1 : s->bmp->dib_header.biBitCount == 1;
        PixelTarget px;
        if (BitmapTarget(*s->bmp, &px)) {
            s->direct = true;
            s->t.px = px;
            s->t.origin = POINT{0, 0};
        }
        return true;
    }
    const bool had = src_dc->has_clip;
    src_dc->has_clip = false;   // reading ignores the source's clip region
    const bool ok = ResolveDrawTarget(src_dc, &s->t);
    src_dc->has_clip = had;
    s->direct = ok;
    return ok;
}

// Converts between colour and monochrome the way GDI does: mono 0 -> text colour, 1 -> background
// colour of the destination; colour -> mono: equal to the source's background colour -> 1.
std::uint32_t AdaptSource(std::uint32_t px, const Source& src, bool dst_mono, const DcObj* dst) {
    if (src.mono && !dst_mono)
        return (px & 0xffffff) ? ColorToPixel(dst->attr->background_color) : ColorToPixel(dst->attr->text_color);
    if (!src.mono && dst_mono)
        return ((px & 0xffffff) == (ColorToPixel(src.dc->attr->background_color) & 0xffffff)) ? 0xffffffffu : 0xff000000u;
    return px;
}

bool DestIsMono(const DcObj* dc) {
    if (dc->kind != DcKind::Memory) return false;
    auto* b = GdiGetT<BitmapObj>(dc->bitmap, abi::kObjBitmap);
    return b && (b->dib ? b->dib_header.biBitCount == 1 : b->bpp == 1);
}

// Draws into a bitmap that has no 32 bpp view (monochrome DDBs, paletted DIB sections) pixel by pixel.
struct SlowDest {
    BitmapObj* bmp = nullptr;
    Rgn clip;
};
bool OpenSlowDest(DcObj* dc, SlowDest* d) {
    if (dc->kind != DcKind::Memory) return false;
    d->bmp = GdiGetT<BitmapObj>(dc->bitmap, abi::kObjBitmap);
    if (!d->bmp) return false;
    d->clip.Set(RECT{0, 0, d->bmp->width, d->bmp->height});
    if (dc->has_clip) d->clip = CombineRgns(d->clip, dc->clip, RGN_AND);
    return true;
}

// Stretching / plain blit from `src` into `dst` with a ternary ROP. Rectangles in device coordinates;
// negative widths/heights mirror.
bool Blit(DcObj* dst_dc, RECT dst_rect, Source* src, RECT src_rect, std::uint8_t rop3, const BrushPaint* brush) {
    const bool flip_x = (dst_rect.right < dst_rect.left) != (src_rect.right < src_rect.left);
    const bool flip_y = (dst_rect.bottom < dst_rect.top) != (src_rect.bottom < src_rect.top);
    auto norm = [](RECT r) {
        return RECT{std::min(r.left, r.right), std::min(r.top, r.bottom), std::max(r.left, r.right), std::max(r.top, r.bottom)};
    };
    dst_rect = norm(dst_rect);
    src_rect = norm(src_rect);
    const int dw = dst_rect.right - dst_rect.left, dh = dst_rect.bottom - dst_rect.top;
    const int sw = src_rect.right - src_rect.left, sh = src_rect.bottom - src_rect.top;
    if (dw <= 0 || dh <= 0) return true;
    const bool use_src = RopUsesSource(rop3) && src;
    if (use_src && (sw <= 0 || sh <= 0)) return true;
    const bool dst_mono = DestIsMono(dst_dc);
    auto source_at = [&](int x, int y) -> std::uint32_t {
        int sx = src_rect.left + static_cast<int>((static_cast<long long>(x - dst_rect.left) * sw) / dw);
        int sy = src_rect.top + static_cast<int>((static_cast<long long>(y - dst_rect.top) * sh) / dh);
        if (flip_x) sx = src_rect.right - 1 - (sx - src_rect.left);
        if (flip_y) sy = src_rect.bottom - 1 - (sy - src_rect.top);
        return AdaptSource(src->Get(sx, sy), *src, dst_mono, dst_dc);
    };
    DrawTarget t;
    if (ResolveDrawTarget(dst_dc, &t)) {
        if (use_src && src->direct && !src->bmp && src->t.top && src->t.top == t.top) {
            // same surface: snapshot the source rectangle first so overlapping copies stay correct
            src->copy_rect = src_rect;
            src->copy.resize(static_cast<std::size_t>(sw) * sh);
            for (int y = 0; y < sh; ++y)
                for (int x = 0; x < sw; ++x)
                    src->copy[static_cast<std::size_t>(y) * sw + x] = src->Get(src_rect.left + x, src_rect.top + y);
        }
        if (use_src && rop3 == 0xcc && !flip_x && !flip_y && sw == dw && sh == dh && src->direct && src->copy.empty() &&
            !src->mono && !dst_mono) {
            // straight copy
            for (const RECT& c : t.clip.rects) {
                const RECT x = RectIntersect(dst_rect, c);
                if (RectEmpty(x)) continue;
                for (int y = x.top; y < x.bottom; ++y) {
                    const int ty = y + t.origin.y;
                    if (ty < 0 || ty >= t.px.height) continue;
                    std::uint32_t* drow = t.px.Row(ty);
                    for (int xx = x.left; xx < x.right; ++xx) {
                        const int tx = xx + t.origin.x;
                        if (tx < 0 || tx >= t.px.width) continue;
                        drow[tx] = src->Get(src_rect.left + (xx - dst_rect.left), src_rect.top + (y - dst_rect.top)) | 0xff000000u;
                    }
                }
            }
        } else {
            ForEachPixel(t, dst_rect, [&](int x, int y, std::uint32_t& px) {
                const std::uint32_t s = use_src ? source_at(x, y) : 0;
                const std::uint32_t p = brush && !brush->null ? brush->At(x, y) : 0;
                px = 0xff000000u | (Rop3(rop3, p, s, px) & 0xffffff);
            });
        }
        NoteDrawn(dst_dc, t, dst_rect);
        return true;
    }
    SlowDest d;
    if (!OpenSlowDest(dst_dc, &d)) return false;
    for (const RECT& c : d.clip.rects) {
        const RECT x = RectIntersect(dst_rect, c);
        for (int y = x.top; y < x.bottom; ++y)
            for (int xx = x.left; xx < x.right; ++xx) {
                const std::uint32_t s = use_src ? source_at(xx, y) : 0;
                const std::uint32_t p = brush && !brush->null ? brush->At(xx, y) : 0;
                const std::uint32_t old = BitmapPixel(*d.bmp, xx, y);
                SetBitmapPixel(*d.bmp, xx, y, 0xff000000u | (Rop3(rop3, p, s, old) & 0xffffff));
            }
    }
    return true;
}

RECT LogicalRect(const DcObj* dc, int x, int y, int w, int h) {
    const POINT a = LogicalToDevice(dc, x, y), b = LogicalToDevice(dc, x + w, y + h);
    return RECT{a.x, a.y, b.x, b.y};
}

RECT Normalized(RECT r) {
    return RECT{std::min(r.left, r.right), std::min(r.top, r.bottom), std::max(r.left, r.right), std::max(r.top, r.bottom)};
}

}  // namespace
}  // namespace detail

namespace sys {
using namespace detail;

BOOL WINAPI NtGdiPatBlt(HDC hdc, INT left, INT top, INT width, INT height, DWORD rop) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return FALSE;
    const std::uint8_t rop3 = static_cast<std::uint8_t>(rop >> 16);
    const RECT r = Normalized(LogicalRect(dc, left, top, width, height));
    const BrushPaint b = BrushOf(dc, dc->brush);
    TRACE(gdi, "%p (%d,%d %dx%d) rop %06lx -> device (%ld,%ld)-(%ld,%ld) colour %08x", hdc, left, top, width, height,
          rop, r.left, r.top, r.right, r.bottom, b.color);
    DrawTarget t;
    if (ResolveDrawTarget(dc, &t)) {
        if (rop3 == 0xf0) PaintRect(dc, t, r, b);
        else if (b.null && RopUsesPattern(rop3)) return TRUE;
        else {
            ForEachPixel(t, r, [&](int x, int y, std::uint32_t& px) {
                px = 0xff000000u | (Rop3(rop3, b.At(x, y), 0, px) & 0xffffff);
            });
            NoteDrawn(dc, t, r);
        }
        return TRUE;
    }
    return Blit(dc, r, nullptr, RECT{}, rop3, &b);
}

BOOL WINAPI NtGdiBitBlt(HDC hdc_dst, INT x_dst, INT y_dst, INT width, INT height, HDC hdc_src, INT x_src, INT y_src,
                        DWORD rop, DWORD bk_color, FLONG fl) {
    LockGuard lock;
    (void)bk_color; (void)fl;
    DcObj* dst = DcGet(hdc_dst);
    if (!dst) return FALSE;
    const std::uint8_t rop3 = static_cast<std::uint8_t>(rop >> 16);
    if (rop & NOMIRRORBITMAP) rop &= ~NOMIRRORBITMAP;
    const BrushPaint b = BrushOf(dst, dst->brush);
    const RECT dr = LogicalRect(dst, x_dst, y_dst, width, height);
    if (!RopUsesSource(rop3)) return Blit(dst, dr, nullptr, RECT{}, rop3, &b);
    DcObj* src = DcGet(hdc_src);
    if (!src) return FALSE;
    Source s;
    if (!OpenSource(src, &s)) return FALSE;
    const RECT sr = LogicalRect(src, x_src, y_src, width, height);
    TRACE(gdi, "%p (%d,%d %dx%d) <- %p (%d,%d) rop %06lx", hdc_dst, x_dst, y_dst, width, height, hdc_src, x_src, y_src, rop);
    return Blit(dst, dr, &s, sr, rop3, &b);
}

BOOL WINAPI NtGdiStretchBlt(HDC hdc, INT x_dst, INT y_dst, INT width_dst, INT height_dst, HDC hdc_src, INT x_src,
                            INT y_src, INT width_src, INT height_src, DWORD rop, COLORREF bk_color) {
    LockGuard lock;
    (void)bk_color;
    DcObj* dst = DcGet(hdc);
    if (!dst) return FALSE;
    const std::uint8_t rop3 = static_cast<std::uint8_t>(rop >> 16);
    const BrushPaint b = BrushOf(dst, dst->brush);
    const RECT dr = LogicalRect(dst, x_dst, y_dst, width_dst, height_dst);
    if (!RopUsesSource(rop3)) return Blit(dst, dr, nullptr, RECT{}, rop3, &b);
    DcObj* src = DcGet(hdc_src);
    if (!src) return FALSE;
    Source s;
    if (!OpenSource(src, &s)) return FALSE;
    const RECT sr = LogicalRect(src, x_src, y_src, width_src, height_src);
    return Blit(dst, dr, &s, sr, rop3, &b);
}

COLORREF WINAPI NtGdiGetPixel(HDC hdc, INT x, INT y) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return CLR_INVALID;
    const POINT p = LogicalToDevice(dc, x, y);
    DrawTarget t;
    if (ResolveDrawTarget(dc, &t)) {
        if (!t.clip.Contains(p.x, p.y)) return CLR_INVALID;
        const int tx = p.x + t.origin.x, ty = p.y + t.origin.y;
        if (tx < 0 || ty < 0 || tx >= t.px.width || ty >= t.px.height) return CLR_INVALID;
        const COLORREF c = PixelToColor(t.px.Row(ty)[tx]);
        TRACE(gdi, "%p (%d,%d) -> %06lx", hdc, x, y, c);
        return c;
    }
    if (dc->kind == DcKind::Memory)
        if (auto* bmp = GdiGetT<BitmapObj>(dc->bitmap, abi::kObjBitmap)) {
            if (p.x < 0 || p.y < 0 || p.x >= bmp->width || p.y >= bmp->height) return CLR_INVALID;
            return PixelToColor(BitmapPixel(*bmp, p.x, p.y));
        }
    return CLR_INVALID;
}

COLORREF WINAPI NtGdiSetPixel(HDC hdc, INT x, INT y, COLORREF color) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return CLR_INVALID;
    const POINT p = LogicalToDevice(dc, x, y);
    DrawTarget t;
    if (ResolveDrawTarget(dc, &t)) {
        if (!t.clip.Contains(p.x, p.y)) return CLR_INVALID;
        PutPixel(t, p.x, p.y, ColorToPixel(color), R2_COPYPEN);
        NoteDrawn(dc, t, RECT{p.x, p.y, p.x + 1, p.y + 1});
        return color & 0xffffff;
    }
    SlowDest d;
    if (OpenSlowDest(dc, &d) && d.clip.Contains(p.x, p.y)) {
        SetBitmapPixel(*d.bmp, p.x, p.y, ColorToPixel(color));
        return PixelToColor(BitmapPixel(*d.bmp, p.x, p.y));
    }
    return CLR_INVALID;
}

BOOL WINAPI NtGdiRectangle(HDC hdc, INT left, INT top, INT right, INT bottom) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return FALSE;
    RECT r = Normalized(LogicalRect(dc, left, top, right - left, bottom - top));
    DrawTarget t;
    if (!ResolveDrawTarget(dc, &t)) return TRUE;
    const BrushPaint b = BrushOf(dc, dc->brush);
    const PenPaint pen = PenOf(dc);
    const int pw = pen.null ? 0 : pen.width;
    if (!b.null) PaintRect(dc, t, RECT{r.left + pw, r.top + pw, r.right - pw, r.bottom - pw}, b);
    if (pw) {
        const int rop2 = dc->attr->rop_mode;
        auto edge = [&](RECT e) {
            ForEachPixel(t, e, [&](int, int, std::uint32_t& px) { px = 0xff000000u | (Rop2(rop2, pen.color, px) & 0xffffff); });
        };
        edge(RECT{r.left, r.top, r.right, r.top + pw});
        edge(RECT{r.left, r.bottom - pw, r.right, r.bottom});
        edge(RECT{r.left, r.top + pw, r.left + pw, r.bottom - pw});
        edge(RECT{r.right - pw, r.top + pw, r.right, r.bottom - pw});
        NoteDrawn(dc, t, r);
    }
    return TRUE;
}

BOOL WINAPI NtGdiEllipse(HDC hdc, INT left, INT top, INT right, INT bottom) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return FALSE;
    DrawTarget t;
    if (!ResolveDrawTarget(dc, &t)) return TRUE;
    FillEllipse(dc, t, Normalized(LogicalRect(dc, left, top, right - left, bottom - top)), BrushOf(dc, dc->brush), PenOf(dc));
    return TRUE;
}

BOOL WINAPI NtGdiMoveTo(HDC hdc, INT x, INT y, POINT* pt) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return FALSE;
    if (pt) *pt = dc->attr->cur_pos;
    dc->attr->cur_pos = POINT{x, y};
    return TRUE;
}

BOOL WINAPI NtGdiLineTo(HDC hdc, INT x, INT y) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return FALSE;
    const POINT from = dc->attr->cur_pos;
    dc->attr->cur_pos = POINT{x, y};
    DrawTarget t;
    if (!ResolveDrawTarget(dc, &t)) return TRUE;
    DrawLine(dc, t, LogicalToDevice(dc, from.x, from.y), LogicalToDevice(dc, x, y), PenOf(dc));
    return TRUE;
}

ULONG WINAPI NtGdiPolyPolyDraw(HDC hdc, const POINT* points, const ULONG* counts, DWORD count, UINT function) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc || !points || !counts) return FALSE;
    if (function == abi::PolyPolygonRgn) {
        // CreatePolyPolygonRgn: bounding boxes of the polygons (no scan conversion into regions yet)
        Rgn acc;
        std::size_t k = 0;
        for (DWORD i = 0; i < count; ++i) {
            RECT b{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
            for (ULONG j = 0; j < counts[i]; ++j, ++k) {
                b.left = std::min(b.left, points[k].x); b.top = std::min(b.top, points[k].y);
                b.right = std::max(b.right, points[k].x); b.bottom = std::max(b.bottom, points[k].y);
            }
            Rgn one;
            one.Set(b);
            acc = CombineRgns(acc, one, RGN_OR);
        }
        FIXME(gdi, "polygon regions are approximated by their bounding boxes");
        auto* r = new RegionObj;
        r->rgn = acc;
        return static_cast<ULONG>(reinterpret_cast<ULONG_PTR>(GdiAlloc(r, abi::kObjRegion)));
    }
    DrawTarget t;
    const bool ok = ResolveDrawTarget(dc, &t);
    std::vector<std::vector<POINT>> polys;
    std::size_t k = 0;
    for (DWORD i = 0; i < count; ++i) {
        std::vector<POINT> poly;
        for (ULONG j = 0; j < counts[i]; ++j, ++k) poly.push_back(LogicalToDevice(dc, points[k].x, points[k].y));
        polys.push_back(std::move(poly));
    }
    const PenPaint pen = PenOf(dc);
    switch (function) {
    case abi::PolyPolygon:
        if (ok) {
            FillPolygon(dc, t, polys, BrushOf(dc, dc->brush), dc->attr->poly_fill_mode == WINDING);
            for (const auto& poly : polys)
                for (std::size_t i = 0; i < poly.size(); ++i) DrawLine(dc, t, poly[i], poly[(i + 1) % poly.size()], pen);
        }
        return TRUE;
    case abi::PolyPolyline:
        if (ok)
            for (const auto& poly : polys)
                for (std::size_t i = 0; i + 1 < poly.size(); ++i) DrawLine(dc, t, poly[i], poly[i + 1], pen);
        return TRUE;
    case abi::PolylineTo: {
        if (polys.empty() || polys[0].empty()) return TRUE;
        POINT cur = LogicalToDevice(dc, dc->attr->cur_pos.x, dc->attr->cur_pos.y);
        if (ok)
            for (const POINT& p : polys[0]) { DrawLine(dc, t, cur, p, pen); cur = p; }
        dc->attr->cur_pos = points[counts[0] - 1];
        return TRUE;
    }
    case abi::PolyBezier:
    case abi::PolyBezierTo: {
        if (polys.empty()) return TRUE;
        std::vector<POINT> pts = polys[0];
        if (function == abi::PolyBezierTo) pts.insert(pts.begin(), LogicalToDevice(dc, dc->attr->cur_pos.x, dc->attr->cur_pos.y));
        for (std::size_t i = 0; ok && i + 3 < pts.size(); i += 3) {
            POINT prev = pts[i];
            for (int s = 1; s <= 16; ++s) {
                const double u = s / 16.0, v = 1 - u;
                const double x = v * v * v * pts[i].x + 3 * v * v * u * pts[i + 1].x + 3 * v * u * u * pts[i + 2].x + u * u * u * pts[i + 3].x;
                const double y = v * v * v * pts[i].y + 3 * v * v * u * pts[i + 1].y + 3 * v * u * u * pts[i + 2].y + u * u * u * pts[i + 3].y;
                const POINT p{static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))};
                DrawLine(dc, t, prev, p, pen);
                prev = p;
            }
        }
        if (function == abi::PolyBezierTo && counts[0]) dc->attr->cur_pos = points[counts[0] - 1];
        return TRUE;
    }
    }
    FIXME(gdi, "PolyPolyDraw function %u", function);
    return FALSE;
}

BOOL WINAPI NtGdiFillRgn(HDC hdc, HRGN hrgn, HBRUSH hbrush) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    auto* r = GdiGetT<RegionObj>(hrgn, abi::kObjRegion);
    if (!dc || !r) return FALSE;
    DrawTarget t;
    if (!ResolveDrawTarget(dc, &t)) return TRUE;
    const BrushPaint b = BrushOf(dc, hbrush);
    for (const RECT& rc : r->rgn.rects) {
        const POINT a = LogicalToDevice(dc, rc.left, rc.top), z = LogicalToDevice(dc, rc.right, rc.bottom);
        PaintRect(dc, t, Normalized(RECT{a.x, a.y, z.x, z.y}), b);
    }
    return TRUE;
}

BOOL WINAPI NtGdiFrameRgn(HDC hdc, HRGN hrgn, HBRUSH brush, INT width, INT height) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    auto* r = GdiGetT<RegionObj>(hrgn, abi::kObjRegion);
    if (!dc || !r) return FALSE;
    DrawTarget t;
    if (!ResolveDrawTarget(dc, &t)) return TRUE;
    // frame = region minus the region shrunk by (width, height)
    Rgn inner;
    for (const RECT& rc : r->rgn.rects) {
        Rgn one;
        one.Set(RECT{rc.left + width, rc.top + height, rc.right - width, rc.bottom - height});
        inner = CombineRgns(inner, one, RGN_OR);
    }
    const Rgn frame = CombineRgns(r->rgn, inner, RGN_DIFF);
    const BrushPaint b = BrushOf(dc, brush);
    for (const RECT& rc : frame.rects) {
        const POINT a = LogicalToDevice(dc, rc.left, rc.top), z = LogicalToDevice(dc, rc.right, rc.bottom);
        PaintRect(dc, t, Normalized(RECT{a.x, a.y, z.x, z.y}), b);
    }
    return TRUE;
}

BOOL WINAPI NtGdiInvertRgn(HDC hdc, HRGN hrgn) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    auto* r = GdiGetT<RegionObj>(hrgn, abi::kObjRegion);
    if (!dc || !r) return FALSE;
    DrawTarget t;
    if (!ResolveDrawTarget(dc, &t)) return TRUE;
    for (const RECT& rc : r->rgn.rects) {
        const POINT a = LogicalToDevice(dc, rc.left, rc.top), z = LogicalToDevice(dc, rc.right, rc.bottom);
        const RECT d = Normalized(RECT{a.x, a.y, z.x, z.y});
        ForEachPixel(t, d, [](int, int, std::uint32_t& px) { px = 0xff000000u | (~px & 0xffffff); });
        NoteDrawn(dc, t, d);
    }
    return TRUE;
}

BOOL WINAPI NtGdiExtTextOutW(HDC hdc, INT x, INT y, UINT flags, const RECT* rect, const WCHAR* str, UINT count,
                             const INT* dx, DWORD cp) {
    LockGuard lock;
    (void)cp;
    DcObj* dc = DcGet(hdc);
    if (!dc) return FALSE;
    abi::DcAttr* a = dc->attr;
    const UINT align = a->text_align;
    if (align & TA_UPDATECP) {
        x = a->cur_pos.x;
        y = a->cur_pos.y;
    }
    if (!str) count = 0;
    DrawTarget t;
    const bool ok = ResolveDrawTarget(dc, &t);
    RECT rect_dev{};
    const bool have_rect = rect && (flags & (ETO_OPAQUE | ETO_CLIPPED));
    if (have_rect) rect_dev = Normalized(LogicalRect(dc, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top));
    if (ok && rect && (flags & ETO_OPAQUE)) {
        BrushPaint b;
        b.null = false;
        b.color = ColorToPixel(a->background_color);
        PaintRect(dc, t, rect_dev, b);
    }

    // Advance of every character, in device units, and the total.
    const TextCell cell = TextCellOf(dc);
    const int stride = (flags & ETO_PDY) ? 2 : 1;
    auto advance = [&](UINT i) {
        return dx ? LogicalToDeviceDx(dc, dx[i * stride]) : cell.width + a->char_extra;
    };
    long long total = 0;
    long long total_logical = 0;
    for (UINT i = 0; i < count; ++i) {
        total += advance(i);
        total_logical += dx ? dx[i * stride] : cell.width + a->char_extra;
    }

    if (count && ok) {
        const POINT p = LogicalToDevice(dc, x, y);
        long left = p.x;
        switch (align & (TA_LEFT | TA_RIGHT | TA_CENTER)) {
        case TA_CENTER: left -= static_cast<long>(total / 2); break;
        case TA_RIGHT: left -= static_cast<long>(total); break;
        default: break;
        }
        long top = p.y;
        switch (align & TA_BASELINE) {
        case TA_BASELINE: top -= cell.ascent; break;
        case TA_BOTTOM: top -= cell.height; break;
        default: break;
        }
        const RECT box{left, top, left + static_cast<long>(total), top + cell.height};
        RECT limit = t.clip.Box();
        if (flags & ETO_CLIPPED) limit = RectIntersect(limit, rect_dev);
        TRACE(gdi, "%p (%d,%d) align %#x %.*ls -> device (%ld,%ld) +%lld", hdc, x, y, align, static_cast<int>(count), str,
              left, top, total);
        if (a->background_mode == OPAQUE) {
            DrawTarget bt = t;   // ETO_CLIPPED limits the text background as well
            if (flags & ETO_CLIPPED) {
                Rgn lim;
                lim.Set(rect_dev);
                bt.clip = CombineRgns(t.clip, lim, RGN_AND);
            }
            FillTargetRect(bt, box, ColorToPixel(a->background_color));
        }
        const std::uint32_t fg = ColorToPixel(a->text_color);
        const int s = cell.scale;
        long gx = left;
        for (UINT i = 0; i < count; ++i) {
            const RECT g{gx, top, gx + cell.width, top + cell.height};
            gx += advance(i);
            const RECT vis = RectIntersect(g, limit);
            if (RectEmpty(vis)) continue;
            const std::uint8_t* rows = font8x16::GlyphRows(str[i]);
            ForEachPixel(t, vis, [&](int px, int py, std::uint32_t& dst) {
                if (font8x16::GlyphPixel(rows, (px - g.left) / s, (py - g.top) / s)) dst = fg;
            });
        }
        NoteDrawn(dc, t, RectIntersect(box, limit));
    }
    if ((align & TA_UPDATECP) && (align & (TA_LEFT | TA_RIGHT | TA_CENTER)) == TA_LEFT)
        a->cur_pos.x = static_cast<LONG>(x + total_logical);
    return TRUE;
}

INT WINAPI NtGdiStretchDIBitsInternal(HDC hdc, INT x_dst, INT y_dst, INT width_dst, INT height_dst, INT x_src,
                                      INT y_src, INT width_src, INT height_src, const void* bits, const BITMAPINFO* bmi,
                                      UINT coloruse, DWORD rop, UINT max_info, UINT max_bits, HANDLE xform) {
    LockGuard lock;
    (void)max_info; (void)max_bits; (void)xform;
    DcObj* dc = DcGet(hdc);
    DibView v;
    if (!dc || !bits || !MakeDibView(bmi, coloruse, bits, &v)) return 0;
    const std::uint8_t rop3 = static_cast<std::uint8_t>(rop >> 16);
    // Source rectangle: DIB coordinates have y counting from the bottom for bottom-up DIBs.
    int sy = y_src;
    if (v.bottom_up) sy = v.height - y_src - height_src;
    const RECT dr = LogicalRect(dc, x_dst, y_dst, width_dst, height_dst);
    const RECT sr{x_src, sy, x_src + width_src, sy + height_src};
    const RECT nd = Normalized(dr);
    const int dw = nd.right - nd.left, dh = nd.bottom - nd.top;
    if (dw <= 0 || dh <= 0 || !width_src || !height_src) return 0;
    const bool flip_x = (dr.right < dr.left) != (width_src < 0);
    const bool flip_y = (dr.bottom < dr.top) != (height_src < 0);
    const RECT ns = Normalized(sr);
    const int sw = ns.right - ns.left, sh = ns.bottom - ns.top;
    const BrushPaint b = BrushOf(dc, dc->brush);
    auto source_at = [&](int x, int y) {
        int sx = ns.left + static_cast<int>((static_cast<long long>(x - nd.left) * sw) / dw);
        int syy = ns.top + static_cast<int>((static_cast<long long>(y - nd.top) * sh) / dh);
        if (flip_x) sx = ns.right - 1 - (sx - ns.left);
        if (flip_y) syy = ns.bottom - 1 - (syy - ns.top);
        return DibViewPixel(v, sx, syy);
    };
    auto combine = [&](int x, int y, std::uint32_t old) {
        const std::uint32_t s = source_at(x, y);
        return rop3 == 0xcc ? s | 0xff000000u : 0xff000000u | (Rop3(rop3, b.At(x, y), s, old) & 0xffffff);
    };
    DrawTarget t;
    if (ResolveDrawTarget(dc, &t)) {
        ForEachPixel(t, nd, [&](int x, int y, std::uint32_t& px) { px = combine(x, y, px); });
        NoteDrawn(dc, t, nd);
        return height_src;
    }
    // A bitmap without a 32 bpp view (a paletted or 24 bpp DIB section, a monochrome bitmap): pixel by pixel.
    // user32's LoadImage(LR_CREATEDIBSECTION) fills the DIB section it made through StretchDIBits on a memory DC.
    SlowDest d;
    if (!OpenSlowDest(dc, &d)) return height_src;
    for (const RECT& c : d.clip.rects) {
        const RECT x = RectIntersect(nd, c);
        for (int y = x.top; y < x.bottom; ++y)
            for (int xx = x.left; xx < x.right; ++xx)
                SetBitmapPixel(*d.bmp, xx, y, combine(xx, y, BitmapPixel(*d.bmp, xx, y)));
    }
    return height_src;
}

INT WINAPI NtGdiSetDIBitsToDeviceInternal(HDC hdc, INT x_dst, INT y_dst, DWORD cx, DWORD cy, INT x_src, INT y_src,
                                          UINT startscan, UINT lines, const void* bits, const BITMAPINFO* bmi,
                                          UINT coloruse, UINT max_bits, UINT max_info, BOOL xform_coords, HANDLE xform) {
    LockGuard lock;
    (void)max_bits; (void)max_info; (void)xform; (void)xform_coords;
    DcObj* dc = DcGet(hdc);
    DibView v;
    if (!dc || !bits || !MakeDibView(bmi, coloruse, bits, &v)) return 0;
    // `bits` holds `lines` scans starting at scan `startscan` (scan 0 = first row in memory).
    if (lines > static_cast<UINT>(v.height)) lines = static_cast<UINT>(v.height);
    const POINT o = LogicalToDevice(dc, x_dst, y_dst);
    const RECT dr{o.x, o.y, o.x + static_cast<LONG>(cx), o.y + static_cast<LONG>(cy)};
    // Source pixel for device position (x, y), or false when the scan is outside the `lines` given.
    auto source_at = [&](int x, int y, std::uint32_t* out) {
        const int sx = x_src + (x - dr.left);
        // DIB y of this destination row: y_src is measured from the bottom for bottom-up DIBs
        const int scan = v.bottom_up ? (y_src + static_cast<int>(cy) - 1 - (y - dr.top)) : (y_src + (y - dr.top));
        if (scan < static_cast<int>(startscan) || scan >= static_cast<int>(startscan + lines) || sx < 0 || sx >= v.width)
            return false;
        const std::uint8_t* row = v.bits + static_cast<std::ptrdiff_t>(scan - static_cast<int>(startscan)) * v.stride;
        DibView one = v;
        one.bits = row;
        one.height = 1;
        one.bottom_up = false;
        *out = DibViewPixel(one, sx, 0) | 0xff000000u;
        return true;
    };
    DrawTarget t;
    if (ResolveDrawTarget(dc, &t)) {
        ForEachPixel(t, dr, [&](int x, int y, std::uint32_t& px) {
            std::uint32_t s;
            if (source_at(x, y, &s)) px = s;
        });
        NoteDrawn(dc, t, dr);
        return static_cast<INT>(lines);
    }
    SlowDest d;   // a bitmap without a 32 bpp view: pixel by pixel
    if (!OpenSlowDest(dc, &d)) return static_cast<INT>(lines);
    for (const RECT& c : d.clip.rects) {
        const RECT r = RectIntersect(dr, c);
        for (int y = r.top; y < r.bottom; ++y)
            for (int x = r.left; x < r.right; ++x) {
                std::uint32_t s;
                if (source_at(x, y, &s)) SetBitmapPixel(*d.bmp, x, y, s);
            }
    }
    return static_cast<INT>(lines);
}

}  // namespace sys
}  // namespace win10xbox::user
