// core/user: AlphaBlend and TransparentBlt (NtGdiAlphaBlend / NtGdiTransparentBlt). The plain
// BitBlt/StretchBlt/PatBlt/DIB paths live in gdi_draw.cpp / gdi_objects.cpp; this file adds the
// blends that need per-pixel arithmetic. Nearest-neighbour sampling, DC clip regions ignored
// except for the target bounds.
#include "user_internal.h"
#include "user_syscalls.h"

#include <algorithm>


namespace win10xbox::user {
namespace detail {
namespace {

struct Surf {
    DrawTarget t;
    bool direct = false;
    BitmapObj* bmp = nullptr;
    bool Open(DcObj* dc) {
        if (ResolveDrawTarget(dc, &t)) { direct = true; return true; }
        if (dc->kind != DcKind::Memory) return false;
        bmp = GdiGetT<BitmapObj>(dc->bitmap, abi::kObjBitmap);
        return bmp != nullptr;
    }
    bool In(int x, int y) const {
        if (direct) {
            const int tx = x + t.origin.x, ty = y + t.origin.y;
            return tx >= 0 && ty >= 0 && tx < t.px.width && ty < t.px.height && t.clip.Contains(x, y);
        }
        return x >= 0 && y >= 0 && x < bmp->width && y < bmp->height;
    }
    std::uint32_t Get(int x, int y) const {
        if (!In(x, y)) return 0xff000000u;
        return direct ? t.px.Row(y + t.origin.y)[x + t.origin.x] : BitmapPixel(*bmp, x, y);
    }
    void Put(int x, int y, std::uint32_t v) {
        if (!In(x, y)) return;
        if (direct) t.px.Row(y + t.origin.y)[x + t.origin.x] = v;
        else SetBitmapPixel(*bmp, x, y, v);
    }
};

template <class F>
bool StretchLoop(HDC hdst, int xd, int yd, int wd, int hd, HDC hsrc, int xs, int ys, int ws, int hs, F&& px) {
    DcObj* dd = DcGet(hdst);
    DcObj* sd = DcGet(hsrc);
    if (!dd || !sd) return false;
    Surf dst, src;
    if (!dst.Open(dd) || !src.Open(sd)) return false;
    const POINT d0 = LogicalToDevice(dd, xd, yd), d1 = LogicalToDevice(dd, xd + wd, yd + hd);
    const POINT s0 = LogicalToDevice(sd, xs, ys), s1 = LogicalToDevice(sd, xs + ws, ys + hs);
    const RECT dr{std::min(d0.x, d1.x), std::min(d0.y, d1.y), std::max(d0.x, d1.x), std::max(d0.y, d1.y)};
    const int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    const int sw = std::abs(s1.x - s0.x), sh = std::abs(s1.y - s0.y);
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return true;
    const int sl = std::min(s0.x, s1.x), st = std::min(s0.y, s1.y);
    // snapshot the source rectangle so overlapping same-surface calls are well defined
    std::vector<std::uint32_t> snap(static_cast<std::size_t>(sw) * sh);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) snap[static_cast<std::size_t>(y) * sw + x] = src.Get(sl + x, st + y);
    for (int y = dr.top; y < dr.bottom; ++y)
        for (int x = dr.left; x < dr.right; ++x) {
            const int sx = static_cast<int>((static_cast<long long>(x - dr.left) * sw) / dw);
            const int sy = static_cast<int>((static_cast<long long>(y - dr.top) * sh) / dh);
            const std::uint32_t s = snap[static_cast<std::size_t>(sy) * sw + sx];
            std::uint32_t out = 0;
            if (px(s, dst.Get(x, y), &out)) dst.Put(x, y, out);
        }
    if (dst.direct) NoteDrawn(dd, dst.t, dr);
    return true;
}

inline std::uint32_t Div255(std::uint32_t v) { return (v + 127) / 255; }

}  // namespace
}  // namespace detail

namespace sys {
using namespace detail;

BOOL WINAPI NtGdiAlphaBlend(HDC hdc_dst, INT x_dst, INT y_dst, INT width_dst, INT height_dst, HDC hdc_src, INT x_src,
                            INT y_src, INT width_src, INT height_src, DWORD blend_function, HANDLE xform) {
    LockGuard lock;
    (void)xform;
    const BLENDFUNCTION bf = *reinterpret_cast<const BLENDFUNCTION*>(&blend_function);
    if (bf.BlendOp != AC_SRC_OVER || (bf.AlphaFormat & ~AC_SRC_ALPHA)) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const std::uint32_t sca = bf.SourceConstantAlpha;
    const bool per_pixel = (bf.AlphaFormat & AC_SRC_ALPHA) != 0;
    const bool ok = StretchLoop(hdc_dst, x_dst, y_dst, width_dst, height_dst, hdc_src, x_src, y_src, width_src,
                                height_src, [&](std::uint32_t s, std::uint32_t d, std::uint32_t* out) {
        std::uint32_t r = 0;
        if (per_pixel) {
            const std::uint32_t a = per_pixel ? (s >> 24) : 255;
            const std::uint32_t sa = Div255(a * sca);
            for (int sh = 0; sh < 24; sh += 8) {
                const std::uint32_t sc = Div255(((s >> sh) & 0xff) * sca);
                const std::uint32_t dc = (d >> sh) & 0xff;
                r |= std::min<std::uint32_t>(255, sc + Div255(dc * (255 - sa))) << sh;
            }
        } else {
            for (int sh = 0; sh < 24; sh += 8)
                r |= Div255(((s >> sh) & 0xff) * sca + ((d >> sh) & 0xff) * (255 - sca)) << sh;
        }
        *out = 0xff000000u | r;
        return true;
    });
    if (!ok) SetLastErr(ERROR_INVALID_HANDLE);
    return ok;
}

BOOL WINAPI NtGdiTransparentBlt(HDC hdc, INT x_dst, INT y_dst, INT width_dst, INT height_dst, HDC hdc_src, INT x_src,
                                INT y_src, INT width_src, INT height_src, UINT color) {
    LockGuard lock;
    const std::uint32_t key = ColorToPixel(color) & 0xffffff;
    const bool ok = StretchLoop(hdc, x_dst, y_dst, width_dst, height_dst, hdc_src, x_src, y_src, width_src, height_src,
                                [&](std::uint32_t s, std::uint32_t, std::uint32_t* out) {
        if ((s & 0xffffff) == key) return false;
        *out = 0xff000000u | (s & 0xffffff);
        return true;
    });
    if (!ok) SetLastErr(ERROR_INVALID_HANDLE);
    return ok;
}

}  // namespace sys
}  // namespace win10xbox::user
