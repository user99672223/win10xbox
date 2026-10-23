// core/user device contexts: DC objects and their DC_ATTR (shared with gdi32,
// which reads and writes it directly), memory / window / screen DCs, object
// selection, SaveDC/RestoreDC, clipping, coordinate mapping, device caps and
// the text metrics of the built-in 8x16 face (docs/GUI.md).
#include "user_internal.h"
#include "user_syscalls.h"
#include "font8x16.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace win10xbox::user {
namespace detail {
namespace {

// DC_ATTRs live in guest-visible pages handed out in 64 KiB chunks and are
// recycled, never returned to the system: gdi32 may still hold a pointer to one
// after DeleteDC (it then fails the handle lookup before dereferencing it).
abi::DcAttr* AllocDcAttr(State& s) {
    if (s.dc_attr_free.empty()) {
        constexpr SIZE_T kChunk = 0x10000;
        auto* chunk = static_cast<abi::DcAttr*>(VirtualAlloc(nullptr, kChunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!chunk) return nullptr;
        for (std::size_t i = kChunk / sizeof(abi::DcAttr); i-- > 0;) s.dc_attr_free.push_back(chunk + i);
    }
    abi::DcAttr* a = s.dc_attr_free.back();
    s.dc_attr_free.pop_back();
    std::memset(a, 0, sizeof *a);
    return a;
}

void InitDcAttr(abi::DcAttr* a) {
    a->wnd_ext = SIZE{1, 1};
    a->vport_ext = SIZE{1, 1};
    a->miter_limit = 10.0f;
    a->rop_mode = R2_COPYPEN;
    a->font_code_page = CP_ACP;
    a->poly_fill_mode = ALTERNATE;
    a->stretch_blt_mode = BLACKONWHITE;
    a->rel_abs_mode = 1;   // ABSOLUTE
    a->background_mode = OPAQUE;
    a->background_color = RGB(255, 255, 255);
    a->brush_color = RGB(255, 255, 255);
    a->pen_color = RGB(0, 0, 0);
    a->text_color = RGB(0, 0, 0);
    a->text_align = TA_LEFT | TA_TOP | TA_NOUPDATECP;
    a->map_mode = MM_TEXT;
    a->graphics_mode = GM_COMPATIBLE;
    a->arc_direction = AD_COUNTERCLOCKWISE;
}

void Select(HGDIOBJ* slot, HGDIOBJ h) {
    if (*slot == h) return;
    if (GdiObject* o = GdiGet(h)) ++o->selected;
    HGDIOBJ old = *slot;
    *slot = h;
    if (old) GdiReleaseSelected(old);
}

}  // namespace

DcObj* DcGet(HDC hdc) {
    State& s = S();
    return static_cast<DcObj*>(s.gdi->Lookup(hdc, abi::kObjDc));
}

HDC CreateDcObj(DcKind kind, HWND hwnd) {
    State& s = S();
    auto* dc = new DcObj;
    dc->kind = kind;
    dc->hwnd = hwnd;
    dc->attr = AllocDcAttr(s);
    if (!dc->attr) {
        delete dc;
        return nullptr;
    }
    InitDcAttr(dc->attr);
    abi::DcAttr* attr = dc->attr;
    auto h = static_cast<HDC>(GdiAlloc(dc, kind == DcKind::Memory ? abi::kObjMemDc : abi::kObjDc, attr));
    if (!h) {
        s.dc_attr_free.push_back(attr);
        return nullptr;
    }
    attr->hdc = static_cast<UINT>(reinterpret_cast<ULONG_PTR>(h));
    Select(&dc->brush, s.stock[WHITE_BRUSH]);
    Select(&dc->pen, s.stock[BLACK_PEN]);
    Select(&dc->font, s.stock[SYSTEM_FONT]);
    Select(&dc->palette, s.stock[DEFAULT_PALETTE]);
    if (kind == DcKind::Memory) Select(&dc->bitmap, s.stock[STOCK_LAST + 1]);
    int sw = 0, sh = 0;
    ScreenSize(&sw, &sh);
    attr->vis_rect = RECT{0, 0, sw, sh};
    attr->virtual_res = SIZE{sw, sh};
    attr->virtual_size = SIZE{sw * 254 / 960, sh * 254 / 960};
    return h;
}

void FreeDcObj(HDC hdc) {
    State& s = S();
    DcObj* dc = DcGet(hdc);
    if (!dc) return;
    if (auto* bmp = GdiGetT<BitmapObj>(dc->bitmap, abi::kObjBitmap))
        if (bmp->selected_dc == hdc) bmp->selected_dc = nullptr;
    for (HGDIOBJ* slot : {&dc->brush, &dc->pen, &dc->font, &dc->bitmap, &dc->palette}) {
        HGDIOBJ h = *slot;
        *slot = nullptr;
        if (h) GdiReleaseSelected(h);
    }
    for (DcState& st : dc->saved)
        for (HGDIOBJ h : {st.brush, st.pen, st.font, st.bitmap, st.palette})
            if (h) GdiReleaseSelected(h);
    s.gdi->Free(hdc);
    abi::DcAttr* attr = dc->attr;
    std::memset(attr, 0, sizeof *attr);
    attr->disabled = 1;
    s.dc_attr_free.push_back(attr);
    delete dc;
}

POINT LogicalToDevice(const DcObj* dc, int x, int y) {
    const abi::DcAttr* a = dc->attr;
    double fx = x, fy = y;
    if (a->graphics_mode == GM_ADVANCED) {
        const XFORM& w = dc->world;
        const double nx = fx * w.eM11 + fy * w.eM21 + w.eDx;
        const double ny = fx * w.eM12 + fy * w.eM22 + w.eDy;
        fx = nx;
        fy = ny;
    }
    POINT p;
    if (a->map_mode == MM_TEXT || !a->wnd_ext.cx || !a->wnd_ext.cy) {
        p.x = static_cast<LONG>(std::lround(fx - a->wnd_org.x + a->vport_org.x));
        p.y = static_cast<LONG>(std::lround(fy - a->wnd_org.y + a->vport_org.y));
    } else {
        p.x = static_cast<LONG>(std::lround((fx - a->wnd_org.x) * a->vport_ext.cx / a->wnd_ext.cx + a->vport_org.x));
        p.y = static_cast<LONG>(std::lround((fy - a->wnd_org.y) * a->vport_ext.cy / a->wnd_ext.cy + a->vport_org.y));
    }
    return p;
}

int LogicalToDeviceDx(const DcObj* dc, int dx) {
    const abi::DcAttr* a = dc->attr;
    if (a->map_mode == MM_TEXT || !a->wnd_ext.cx) return dx;
    return static_cast<int>(std::lround(static_cast<double>(dx) * a->vport_ext.cx / a->wnd_ext.cx));
}

int LogicalToDeviceDy(const DcObj* dc, int dy) {
    const abi::DcAttr* a = dc->attr;
    if (a->map_mode == MM_TEXT || !a->wnd_ext.cy) return dy;
    return static_cast<int>(std::lround(static_cast<double>(dy) * a->vport_ext.cy / a->wnd_ext.cy));
}

bool ResolveDrawTarget(DcObj* dc, DrawTarget* out) {
    out->clip = Rgn{};
    out->top = nullptr;
    RECT base{};
    if (dc->kind == DcKind::Memory) {
        auto* bmp = GdiGetT<BitmapObj>(dc->bitmap, abi::kObjBitmap);
        if (!bmp || !BitmapTarget(*bmp, &out->px)) {
            if (bmp && (bmp->bpp == 1 || bmp->dib)) {
                FIXME(gdi, "drawing into a %d bpp bitmap is not supported", bmp->dib ? bmp->dib_header.biBitCount : bmp->bpp);
            }
            return false;
        }
        out->origin = POINT{0, 0};
        base = RECT{0, 0, bmp->width, bmp->height};
        out->clip.Set(base);
    } else if (dc->kind == DcKind::Window) {
        WindowObj* w = Win(dc->hwnd);
        if (!w) return false;
        WindowObj* top = Win(TopLevelOf(w->handle));
        if (!top || !top->surface) return false;   // hidden or never shown: nothing backs it
        winexbox::ISurface& surf = *top->surface;
        out->px.bits = surf.Bits();
        out->px.width = surf.Width();
        out->px.height = surf.Height();
        out->px.stride = surf.Stride();
        out->top = top->handle;
        const RECT top_screen = WindowRectScreen(top);
        const RECT wr = WindowRectScreen(w);
        const POINT dev0 = dc->window_origin ? POINT{wr.left, wr.top} : ClientOriginScreen(w);
        out->origin = POINT{dev0.x - top_screen.left, dev0.y - top_screen.top};
        const RECT area = dc->window_origin ? wr : ClientRectScreen(w);
        base = RectOffset(area, -dev0.x, -dev0.y);
        Rgn vis;
        vis.Set(base);
        // ancestors' client areas
        for (WindowObj* p = Win(w->parent); p && !(p->flags & kWinIsDesktop); p = Win(p->parent))
            vis = CombineRgns(vis, [&] { Rgn r; r.Set(RectOffset(ClientRectScreen(p), -dev0.x, -dev0.y)); return r; }(), RGN_AND);
        // the surface
        Rgn surf_rgn;
        surf_rgn.Set(RECT{-out->origin.x, -out->origin.y, out->px.width - out->origin.x, out->px.height - out->origin.y});
        vis = CombineRgns(vis, surf_rgn, RGN_AND);
        // children (WS_CLIPCHILDREN) and higher siblings (WS_CLIPSIBLINGS)
        if (w->style & WS_CLIPCHILDREN) {
            for (HWND ch : w->children) {
                WindowObj* c = Win(ch);
                if (!c || !(c->style & WS_VISIBLE)) continue;
                Rgn cr;
                cr.Set(RectOffset(WindowRectScreen(c), -dev0.x, -dev0.y));
                vis = CombineRgns(vis, cr, RGN_DIFF);
            }
        }
        if ((w->style & WS_CLIPSIBLINGS) && !IsTopLevel(w)) {
            if (WindowObj* p = Win(w->parent)) {
                for (HWND sib : p->children) {
                    if (sib == w->handle) break;   // only the ones above
                    WindowObj* c = Win(sib);
                    if (!c || !(c->style & WS_VISIBLE)) continue;
                    Rgn cr;
                    cr.Set(RectOffset(WindowRectScreen(c), -dev0.x, -dev0.y));
                    vis = CombineRgns(vis, cr, RGN_DIFF);
                }
            }
        }
        out->clip = vis;
    } else {
        static volatile LONG reported = 0;   // plain flag
        if (!InterlockedExchange(&reported, 1))
            FIXME(gdi, "drawing on the screen DC is not supported (no desktop surface): draw into a window");
        return false;
    }
    if (dc->has_paint_clip) {
        Rgn pc;
        pc.Set(dc->paint_clip);
        out->clip = CombineRgns(out->clip, pc, RGN_AND);
    }
    if (dc->has_clip) out->clip = CombineRgns(out->clip, dc->clip, RGN_AND);
    return true;
}

void NoteDrawn(DcObj* dc, const DrawTarget& t, RECT device_rect) {
    RECT clipped = RectIntersect(device_rect, t.clip.Box());
    if (RectEmpty(clipped)) return;
    if (dc->bounds_enabled) dc->bounds = RectUnion(dc->bounds, clipped);
    dc->drew = true;
    if (!t.top) return;
    if (WindowObj* top = Win(t.top)) top->dirty = RectUnion(top->dirty, RectOffset(clipped, t.origin.x, t.origin.y));
}

// ---- Font metrics (the built-in 8x16 face) ------------------------------------------------

TextCell TextCellOf(const DcObj* dc) {
    const LOGFONTW* lf = nullptr;
    if (auto* f = GdiGetT<FontObj>(dc->font, abi::kObjFont)) lf = &f->lf;
    // LOGFONT heights below 32 pixels get the plain cell; from 32 up the glyphs are scaled by whole factors.
    long h = lf ? lf->lfHeight : 0;
    if (h < 0) h = -h;
    TextCell c;
    c.scale = h >= 2 * font8x16::kCellHeight ? static_cast<int>(std::min<long>(h / font8x16::kCellHeight, 64)) : 1;
    c.width = font8x16::kCellWidth * c.scale;
    c.height = font8x16::kCellHeight * c.scale;
    c.ascent = font8x16::kAscent * c.scale;
    c.descent = font8x16::kDescent * c.scale;
    return c;
}

namespace {

const LOGFONTW* LogFontOf(const DcObj* dc) {
    auto* f = GdiGetT<FontObj>(dc->font, abi::kObjFont);
    return f ? &f->lf : nullptr;
}

}  // namespace
}  // namespace detail

namespace sys {
using namespace detail;

HDC WINAPI NtGdiCreateCompatibleDC(HDC hdc) {
    LockGuard lock;
    if (hdc && !DcGet(hdc)) {
        SetLastErr(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    HDC h = CreateDcObj(DcKind::Memory, nullptr);
    TRACE(gdi, "%p -> %p", hdc, h);
    return h;
}

HDC WINAPI NtGdiOpenDCW(UNICODE_STRING* device, const DEVMODEW* devmode, UNICODE_STRING* output, ULONG type,
                        BOOL is_display, HANDLE hspool, void* driver_info, void* pdev) {
    LockGuard lock;
    (void)devmode; (void)output; (void)hspool; (void)driver_info; (void)pdev;
    const bool display = is_display || !device || !device->Buffer ||
                         (device->Length >= 22 && !_wcsnicmp(device->Buffer, L"\\\\.\\DISPLAY", 11)) ||
                         (device->Length >= 14 && !_wcsnicmp(device->Buffer, L"DISPLAY", 7));
    if (!display) {
        FIXME(gdi, "printer / non-display DC %.*ls (type %lu) not supported", device ? device->Length / 2 : 0,
              device && device->Buffer ? device->Buffer : L"", type);
        SetLastErr(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    return CreateDcObj(type == 2 /* WINE_GDI_DRIVER IC */ ? DcKind::Info : DcKind::Screen, nullptr);
}

INT WINAPI NtGdiGetDeviceCaps(HDC hdc, INT cap) {
    LockGuard lock;
    if (!DcGet(hdc)) return 0;
    int w = 0, h = 0;
    ScreenSize(&w, &h);
    switch (cap) {
    case DRIVERVERSION: return 0x4000;
    case TECHNOLOGY: return DT_RASDISPLAY;
    case HORZSIZE: return w * 254 / 960;
    case VERTSIZE: return h * 254 / 960;
    case HORZRES: return w;
    case VERTRES: return h;
    case BITSPIXEL: return 32;
    case PLANES: return 1;
    case NUMBRUSHES: return -1;
    case NUMPENS: return -1;
    case NUMMARKERS: return 0;
    case NUMFONTS: return 0;
    case NUMCOLORS: return -1;
    case PDEVICESIZE: return 0;
    case CURVECAPS: return CC_CIRCLES | CC_PIE | CC_CHORD | CC_ELLIPSES | CC_WIDE | CC_STYLED | CC_WIDESTYLED |
                           CC_INTERIORS | CC_ROUNDRECT;
    case LINECAPS: return LC_POLYLINE | LC_MARKER | LC_POLYMARKER | LC_WIDE | LC_STYLED | LC_WIDESTYLED | LC_INTERIORS;
    case POLYGONALCAPS: return PC_POLYGON | PC_RECTANGLE | PC_WINDPOLYGON | PC_SCANLINE | PC_WIDE | PC_STYLED |
                               PC_WIDESTYLED | PC_INTERIORS;
    case TEXTCAPS: return TC_OP_CHARACTER | TC_OP_STROKE | TC_CP_STROKE | TC_CR_ANY | TC_SF_X_YINDEP | TC_SA_DOUBLE |
                          TC_SA_INTEGER | TC_SA_CONTIN | TC_UA_ABLE | TC_SO_ABLE | TC_RA_ABLE | TC_VA_ABLE;
    case CLIPCAPS: return CP_REGION;
    case RASTERCAPS: return RC_BITBLT | RC_BITMAP64 | RC_GDI20_OUTPUT | RC_DI_BITMAP | RC_DIBTODEV | RC_BIGFONT |
                            RC_STRETCHBLT | RC_STRETCHDIB | RC_DEVBITS;
    case ASPECTX: return 36;
    case ASPECTY: return 36;
    case ASPECTXY: return 51;
    case LOGPIXELSX: return 96;
    case LOGPIXELSY: return 96;
    case SIZEPALETTE: return 0;
    case NUMRESERVED: return 20;
    case COLORRES: return 24;
    case PHYSICALWIDTH: case PHYSICALHEIGHT: case PHYSICALOFFSETX: case PHYSICALOFFSETY: case SCALINGFACTORX:
    case SCALINGFACTORY: return 0;
    case VREFRESH: return 60;
    case DESKTOPVERTRES: return h;
    case DESKTOPHORZRES: return w;
    case BLTALIGNMENT: return 0;
    case SHADEBLENDCAPS: return SB_CONST_ALPHA | SB_PIXEL_ALPHA;
    case COLORMGMTCAPS: return CM_NONE;
    }
    FIXME(gdi, "GetDeviceCaps(%d)", cap);
    return 0;
}

BOOL WINAPI NtGdiGetAndSetDCDword(HDC hdc, UINT method, DWORD value, DWORD* result) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc || !result) return FALSE;
    abi::DcAttr* a = dc->attr;
    switch (method) {
    case abi::DcSetMapMode:
        *result = static_cast<DWORD>(a->map_mode);
        if (value < MM_MIN || value > MM_MAX) return FALSE;
        a->map_mode = static_cast<INT>(value);
        if (value == MM_TEXT) { a->wnd_ext = SIZE{1, 1}; a->vport_ext = SIZE{1, 1}; }
        else if (value != MM_ISOTROPIC && value != MM_ANISOTROPIC)
            FIXME(gdi, "map mode %lu mapped as MM_TEXT", value);
        return TRUE;
    case abi::DcSetBkColor: *result = a->background_color; a->background_color = value; return TRUE;
    case abi::DcSetBkMode: *result = a->background_mode; a->background_mode = static_cast<WORD>(value); return TRUE;
    case abi::DcSetTextColor: *result = a->text_color; a->text_color = value; return TRUE;
    case abi::DcSetDCBrushColor: *result = a->brush_color; a->brush_color = value; return TRUE;
    case abi::DcSetDCPenColor: *result = a->pen_color; a->pen_color = value; return TRUE;
    case abi::DcSetGraphicsMode: *result = static_cast<DWORD>(a->graphics_mode); a->graphics_mode = static_cast<INT>(value); return TRUE;
    case abi::DcSetROP2: *result = a->rop_mode; a->rop_mode = static_cast<WORD>(value); return TRUE;
    case abi::DcSetTextAlign: *result = a->text_align; a->text_align = static_cast<WORD>(value); return TRUE;
    }
    FIXME(gdi, "GetAndSetDCDword method %u", method);
    return FALSE;
}

BOOL WINAPI NtGdiGetDCDword(HDC hdc, UINT method, DWORD* result) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc || !result) return FALSE;
    abi::DcAttr* a = dc->attr;
    switch (method) {
    case abi::DcGetArcDirection: *result = static_cast<DWORD>(a->arc_direction); return TRUE;
    case abi::DcGetBkColor: *result = a->background_color; return TRUE;
    case abi::DcGetBkMode: *result = a->background_mode; return TRUE;
    case abi::DcGetDCBrushColor: *result = a->brush_color; return TRUE;
    case abi::DcGetDCPenColor: *result = a->pen_color; return TRUE;
    case abi::DcGetGraphicsMode: *result = static_cast<DWORD>(a->graphics_mode); return TRUE;
    case abi::DcGetLayout: *result = a->layout; return TRUE;
    case abi::DcGetPolyFillMode: *result = a->poly_fill_mode; return TRUE;
    case abi::DcGetROP2: *result = a->rop_mode; return TRUE;
    case abi::DcGetTextColor: *result = a->text_color; return TRUE;
    case abi::DcIsMemDC: *result = dc->kind == DcKind::Memory; return TRUE;
    case abi::DcHasOpenGL: *result = 0; return TRUE;
    }
    FIXME(gdi, "GetDCDword method %u", method);
    return FALSE;
}

BOOL WINAPI NtGdiGetDCPoint(HDC hdc, UINT method, POINT* result) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc || !result) return FALSE;
    switch (method) {
    case abi::DcGetBrushOrgEx: *result = dc->attr->brush_org; return TRUE;
    case abi::DcGetCurrentPosition: *result = dc->attr->cur_pos; return TRUE;
    case abi::DcGetDCOrg: {
        *result = POINT{0, 0};
        if (dc->kind == DcKind::Window)
            if (WindowObj* w = Win(dc->hwnd)) {
                const RECT wr = WindowRectScreen(w);
                *result = dc->window_origin ? POINT{wr.left, wr.top} : ClientOriginScreen(w);
            }
        return TRUE;
    }
    }
    return FALSE;
}

INT WINAPI NtGdiSaveDC(HDC hdc) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return 0;
    DcState st;
    st.attr = *dc->attr;
    st.brush = dc->brush; st.pen = dc->pen; st.font = dc->font; st.bitmap = dc->bitmap; st.palette = dc->palette;
    for (HGDIOBJ h : {st.brush, st.pen, st.font, st.bitmap, st.palette})
        if (GdiObject* o = GdiGet(h)) ++o->selected;
    st.has_clip = dc->has_clip;
    st.clip = dc->clip;
    dc->saved.push_back(st);
    dc->attr->save_level = static_cast<int>(dc->saved.size());
    return static_cast<INT>(dc->saved.size());
}

BOOL WINAPI NtGdiRestoreDC(HDC hdc, INT level) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return FALSE;
    const int n = static_cast<int>(dc->saved.size());
    if (level < 0) level = n + level + 1;
    if (level <= 0 || level > n) return FALSE;
    DcState st = dc->saved[static_cast<std::size_t>(level - 1)];
    for (int i = n; i >= level; --i) {
        DcState& d = dc->saved[static_cast<std::size_t>(i - 1)];
        for (HGDIOBJ h : {d.brush, d.pen, d.font, d.bitmap, d.palette})
            if (h) GdiReleaseSelected(h);
    }
    dc->saved.resize(static_cast<std::size_t>(level - 1));
    const UINT self = dc->attr->hdc;
    const UINT64 emf = dc->attr->emf, print = dc->attr->print, abort_proc = dc->attr->abort_proc;
    *dc->attr = st.attr;
    dc->attr->hdc = self;
    dc->attr->emf = emf;
    dc->attr->print = print;
    dc->attr->abort_proc = abort_proc;
    dc->attr->save_level = level - 1;
    Select(&dc->brush, st.brush);
    Select(&dc->pen, st.pen);
    Select(&dc->font, st.font);
    Select(&dc->palette, st.palette);
    if (dc->kind == DcKind::Memory && st.bitmap != dc->bitmap) {
        if (auto* old = GdiGetT<BitmapObj>(dc->bitmap, abi::kObjBitmap)) old->selected_dc = nullptr;
        Select(&dc->bitmap, st.bitmap);
        if (auto* b = GdiGetT<BitmapObj>(dc->bitmap, abi::kObjBitmap)) b->selected_dc = hdc;
    }
    dc->has_clip = st.has_clip;
    dc->clip = st.clip;
    return TRUE;
}

HGDIOBJ WINAPI NtGdiSelectBrush(HDC hdc, HGDIOBJ handle) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    GdiObject* obj = GdiGet(handle, abi::kObjBrush);
    if (!dc || !obj) return nullptr;
    HGDIOBJ old = dc->brush;
    Select(&dc->brush, obj->handle);
    return old;
}

HGDIOBJ WINAPI NtGdiSelectPen(HDC hdc, HGDIOBJ handle) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    GdiObject* obj = GdiGet(handle, abi::kObjPen);
    if (!dc || !obj) return nullptr;
    HGDIOBJ old = dc->pen;
    Select(&dc->pen, obj->handle);
    return old;
}

HGDIOBJ WINAPI NtGdiSelectFont(HDC hdc, HGDIOBJ handle) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    GdiObject* obj = GdiGet(handle, abi::kObjFont);
    if (!dc || !obj) return nullptr;
    HGDIOBJ old = dc->font;
    Select(&dc->font, obj->handle);
    return old;
}

HGDIOBJ WINAPI NtGdiSelectBitmap(HDC hdc, HGDIOBJ handle) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    auto* bmp = GdiGetT<BitmapObj>(handle, abi::kObjBitmap);
    if (!dc || !bmp || dc->kind != DcKind::Memory) return nullptr;
    if (bmp->selected_dc && bmp->selected_dc != hdc && !bmp->system) {
        WARN(gdi, "bitmap %p already selected into %p", handle, bmp->selected_dc);
        return nullptr;
    }
    HGDIOBJ old = dc->bitmap;
    if (auto* prev = GdiGetT<BitmapObj>(old, abi::kObjBitmap))
        if (prev->selected_dc == hdc) prev->selected_dc = nullptr;
    Select(&dc->bitmap, bmp->handle);
    if (!bmp->system) bmp->selected_dc = hdc;
    dc->has_clip = false;   // the clip region is reset to the new bitmap
    dc->clip = Rgn{};
    dc->attr->vis_rect = RECT{0, 0, bmp->width, bmp->height};
    TRACE(gdi, "%p: bitmap %p (%dx%d)", hdc, handle, bmp->width, bmp->height);
    return old;
}

HANDLE WINAPI NtGdiGetDCObject(HDC hdc, UINT type) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return nullptr;
    switch (type) {
    case abi::kObjBrush: return dc->brush;
    case abi::kObjPen: case abi::kObjExtPen: return dc->pen;
    case abi::kObjFont: return dc->font;
    case abi::kObjPal: return dc->palette;
    case abi::kObjSurf: case abi::kObjBitmap:
        return dc->kind == DcKind::Memory ? dc->bitmap : S().stock[STOCK_LAST + 1];
    }
    return nullptr;
}

namespace {
RECT DeviceRectFromLogical(const DcObj* dc, int l, int t, int r, int b) {
    POINT p1 = LogicalToDevice(dc, l, t), p2 = LogicalToDevice(dc, r, b);
    return RECT{std::min(p1.x, p2.x), std::min(p1.y, p2.y), std::max(p1.x, p2.x), std::max(p1.y, p2.y)};
}
// The DC's base visible area in device coordinates (what an empty app clip means).
RECT DcBaseRect(DcObj* dc) {
    DrawTarget t;
    const bool had = dc->has_clip;
    const Rgn saved = dc->clip;
    dc->has_clip = false;
    RECT r{0, 0, 0, 0};
    if (ResolveDrawTarget(dc, &t)) r = t.clip.Box();
    else {
        int w = 0, h = 0;
        ScreenSize(&w, &h);
        r = RECT{0, 0, w, h};
    }
    dc->has_clip = had;
    dc->clip = saved;
    return r;
}
INT ClipResult(DcObj* dc) {
    if (!dc->has_clip) return SIMPLEREGION;
    return dc->clip.Empty() ? NULLREGION : dc->clip.Complexity();
}
}  // namespace

INT WINAPI NtGdiIntersectClipRect(HDC hdc, INT left, INT top, INT right, INT bottom) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return ERROR;
    Rgn r;
    r.Set(DeviceRectFromLogical(dc, left, top, right, bottom));
    if (!dc->has_clip) {
        dc->clip = r;
        dc->has_clip = true;
    } else {
        dc->clip = CombineRgns(dc->clip, r, RGN_AND);
    }
    return ClipResult(dc);
}

INT WINAPI NtGdiExcludeClipRect(HDC hdc, INT left, INT top, INT right, INT bottom) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return ERROR;
    if (!dc->has_clip) {
        dc->clip.Set(DcBaseRect(dc));
        dc->has_clip = true;
    }
    Rgn r;
    r.Set(DeviceRectFromLogical(dc, left, top, right, bottom));
    dc->clip = CombineRgns(dc->clip, r, RGN_DIFF);
    return ClipResult(dc);
}

INT WINAPI NtGdiOffsetClipRgn(HDC hdc, INT x, INT y) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return ERROR;
    if (dc->has_clip) dc->clip.Offset(LogicalToDeviceDx(dc, x), LogicalToDeviceDy(dc, y));
    return ClipResult(dc);
}

INT WINAPI NtGdiExtSelectClipRgn(HDC hdc, HRGN region, INT mode) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return ERROR;
    if (!region) {
        if (mode != RGN_COPY) return ERROR;
        dc->has_clip = false;
        dc->clip = Rgn{};
        return SIMPLEREGION;
    }
    auto* r = GdiGetT<RegionObj>(region, abi::kObjRegion);
    if (!r) return ERROR;
    // The region is in device units already.
    if (mode == RGN_COPY || !dc->has_clip) {
        Rgn base;
        if (mode != RGN_COPY) base.Set(DcBaseRect(dc));
        dc->clip = mode == RGN_COPY ? r->rgn : CombineRgns(base, r->rgn, mode);
    } else {
        dc->clip = CombineRgns(dc->clip, r->rgn, mode);
    }
    dc->has_clip = true;
    return ClipResult(dc);
}

INT WINAPI NtGdiGetAppClipBox(HDC hdc, RECT* rect) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc || !rect) return ERROR;
    DrawTarget t;
    RECT box;
    INT ret;
    if (ResolveDrawTarget(dc, &t)) {
        box = t.clip.Box();
        ret = t.clip.Empty() ? NULLREGION : t.clip.Complexity();
    } else {
        box = DcBaseRect(dc);
        ret = SIMPLEREGION;
    }
    // device -> logical (MM_TEXT offsets)
    const abi::DcAttr* a = dc->attr;
    if (a->map_mode == MM_TEXT) box = RectOffset(box, a->wnd_org.x - a->vport_org.x, a->wnd_org.y - a->vport_org.y);
    *rect = box;
    return ret;
}

INT WINAPI NtGdiGetRandomRgn(HDC hdc, HRGN region, INT code) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    auto* r = GdiGetT<RegionObj>(region, abi::kObjRegion);
    if (!dc || !r) return -1;
    switch (code & 0xff) {
    case 1:   // CLIPRGN
        if (!dc->has_clip) return 0;
        r->rgn = dc->clip;
        return 1;
    case 4: {   // SYSRGN: screen coordinates
        DrawTarget t;
        if (!ResolveDrawTarget(dc, &t)) return 0;
        r->rgn = t.clip;
        if (dc->kind == DcKind::Window)
            if (WindowObj* w = Win(dc->hwnd)) {
                const RECT wr = WindowRectScreen(w);
                const POINT o = dc->window_origin ? POINT{wr.left, wr.top} : ClientOriginScreen(w);
                r->rgn.Offset(o.x, o.y);
            }
        return 1;
    }
    }
    FIXME(gdi, "GetRandomRgn code %d", code);
    return 0;
}

BOOL WINAPI NtGdiPtVisible(HDC hdc, INT x, INT y) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    DrawTarget t;
    if (!dc || !ResolveDrawTarget(dc, &t)) return FALSE;
    const POINT p = LogicalToDevice(dc, x, y);
    return t.clip.Contains(p.x, p.y);
}

BOOL WINAPI NtGdiRectVisible(HDC hdc, const RECT* rect) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    DrawTarget t;
    if (!dc || !rect || !ResolveDrawTarget(dc, &t)) return FALSE;
    const RECT d = DeviceRectFromLogical(dc, rect->left, rect->top, rect->right, rect->bottom);
    for (const RECT& c : t.clip.rects)
        if (!RectEmpty(RectIntersect(c, d))) return TRUE;
    return FALSE;
}

BOOL WINAPI NtGdiSetBrushOrg(HDC hdc, INT x, INT y, POINT* prev_org) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return FALSE;
    if (prev_org) *prev_org = dc->attr->brush_org;
    dc->attr->brush_org = POINT{x, y};
    return TRUE;
}

BOOL WINAPI NtGdiComputeXformCoefficients(HDC hdc) {
    LockGuard lock;
    return DcGet(hdc) != nullptr;   // the mapping is computed from DC_ATTR on every use
}

BOOL WINAPI NtGdiTransformPoints(HDC hdc, const POINT* points_in, POINT* points_out, INT count, UINT mode) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc || !points_in || !points_out) return FALSE;
    const abi::DcAttr* a = dc->attr;
    for (INT i = 0; i < count; ++i) {
        if (mode == 0) {   // LPtoDP
            points_out[i] = LogicalToDevice(dc, points_in[i].x, points_in[i].y);
        } else {           // DPtoLP
            double x = points_in[i].x, y = points_in[i].y;
            if (a->map_mode != MM_TEXT && a->vport_ext.cx && a->vport_ext.cy) {
                x = (x - a->vport_org.x) * a->wnd_ext.cx / a->vport_ext.cx + a->wnd_org.x;
                y = (y - a->vport_org.y) * a->wnd_ext.cy / a->vport_ext.cy + a->wnd_org.y;
            } else {
                x = x - a->vport_org.x + a->wnd_org.x;
                y = y - a->vport_org.y + a->wnd_org.y;
            }
            if (a->graphics_mode == GM_ADVANCED) {
                x -= dc->world.eDx;
                y -= dc->world.eDy;
            }
            points_out[i] = POINT{static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))};
        }
    }
    return TRUE;
}

BOOL WINAPI NtGdiGetTransform(HDC hdc, DWORD which, XFORM* xform) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc || !xform) return FALSE;
    if (which == 0x203) {   // world -> page
        *xform = dc->world;
        return TRUE;
    }
    const abi::DcAttr* a = dc->attr;
    // page -> device (0x304) and world -> device (0x204)
    XFORM p{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    if (a->map_mode != MM_TEXT && a->wnd_ext.cx && a->wnd_ext.cy) {
        p.eM11 = static_cast<FLOAT>(a->vport_ext.cx) / static_cast<FLOAT>(a->wnd_ext.cx);
        p.eM22 = static_cast<FLOAT>(a->vport_ext.cy) / static_cast<FLOAT>(a->wnd_ext.cy);
    }
    p.eDx = static_cast<FLOAT>(a->vport_org.x) - p.eM11 * static_cast<FLOAT>(a->wnd_org.x);
    p.eDy = static_cast<FLOAT>(a->vport_org.y) - p.eM22 * static_cast<FLOAT>(a->wnd_org.y);
    if (which == 0x304) {
        *xform = p;
        return TRUE;
    }
    if (which == 0x204) {
        const XFORM& w = dc->world;
        xform->eM11 = w.eM11 * p.eM11;
        xform->eM12 = w.eM12 * p.eM22;
        xform->eM21 = w.eM21 * p.eM11;
        xform->eM22 = w.eM22 * p.eM22;
        xform->eDx = w.eDx * p.eM11 + p.eDx;
        xform->eDy = w.eDy * p.eM22 + p.eDy;
        return TRUE;
    }
    FIXME(gdi, "GetTransform %#lx", which);
    return FALSE;
}

BOOL WINAPI NtGdiModifyWorldTransform(HDC hdc, const XFORM* xform, DWORD mode) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return FALSE;
    switch (mode) {
    case MWT_IDENTITY: dc->world = XFORM{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}; break;
    case MWT_LEFTMULTIPLY:
    case MWT_RIGHTMULTIPLY: {
        if (!xform) return FALSE;
        const XFORM& a = mode == MWT_LEFTMULTIPLY ? *xform : dc->world;
        const XFORM& b = mode == MWT_LEFTMULTIPLY ? dc->world : *xform;
        XFORM r;
        r.eM11 = a.eM11 * b.eM11 + a.eM12 * b.eM21;
        r.eM12 = a.eM11 * b.eM12 + a.eM12 * b.eM22;
        r.eM21 = a.eM21 * b.eM11 + a.eM22 * b.eM21;
        r.eM22 = a.eM21 * b.eM12 + a.eM22 * b.eM22;
        r.eDx = a.eDx * b.eM11 + a.eDy * b.eM21 + b.eDx;
        r.eDy = a.eDx * b.eM12 + a.eDy * b.eM22 + b.eDy;
        dc->world = r;
        break;
    }
    case 4:   // MWT_SET
        if (!xform) return FALSE;
        dc->world = *xform;
        break;
    default: return FALSE;
    }
    if (dc->world.eM12 != 0.0f || dc->world.eM21 != 0.0f) FIXME(gdi, "rotating world transforms are not supported");
    return TRUE;
}

DWORD WINAPI NtGdiSetLayout(HDC hdc, LONG wox, DWORD layout) {
    LockGuard lock;
    (void)wox;
    DcObj* dc = DcGet(hdc);
    if (!dc) return GDI_ERROR;
    const DWORD old = dc->attr->layout;
    if (layout & LAYOUT_RTL) FIXME(gdi, "right-to-left DC layout not supported");
    dc->attr->layout = layout;
    return old;
}

BOOL WINAPI NtGdiSetVirtualResolution(HDC hdc, DWORD horz_res, DWORD vert_res, DWORD horz_size, DWORD vert_size) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return FALSE;
    if (horz_res && vert_res) {
        dc->attr->virtual_res = SIZE{static_cast<LONG>(horz_res), static_cast<LONG>(vert_res)};
        dc->attr->virtual_size = SIZE{static_cast<LONG>(horz_size), static_cast<LONG>(vert_size)};
    }
    return TRUE;
}

BOOL WINAPI NtGdiScaleViewportExtEx(HDC hdc, INT x_num, INT x_denom, INT y_num, INT y_denom, SIZE* size) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc || !x_denom || !y_denom) return FALSE;
    abi::DcAttr* a = dc->attr;
    if (size) *size = a->vport_ext;
    if (a->map_mode != MM_ISOTROPIC && a->map_mode != MM_ANISOTROPIC) return TRUE;
    a->vport_ext.cx = a->vport_ext.cx * x_num / x_denom;
    a->vport_ext.cy = a->vport_ext.cy * y_num / y_denom;
    if (!a->vport_ext.cx) a->vport_ext.cx = 1;
    if (!a->vport_ext.cy) a->vport_ext.cy = 1;
    return TRUE;
}

BOOL WINAPI NtGdiScaleWindowExtEx(HDC hdc, INT x_num, INT x_denom, INT y_num, INT y_denom, SIZE* size) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc || !x_denom || !y_denom) return FALSE;
    abi::DcAttr* a = dc->attr;
    if (size) *size = a->wnd_ext;
    if (a->map_mode != MM_ISOTROPIC && a->map_mode != MM_ANISOTROPIC) return TRUE;
    a->wnd_ext.cx = a->wnd_ext.cx * x_num / x_denom;
    a->wnd_ext.cy = a->wnd_ext.cy * y_num / y_denom;
    if (!a->wnd_ext.cx) a->wnd_ext.cx = 1;
    if (!a->wnd_ext.cy) a->wnd_ext.cy = 1;
    return TRUE;
}

BOOL WINAPI NtGdiFlush() {
    LockGuard lock;
    FlushAllDirty();
    return TRUE;
}

UINT WINAPI NtGdiSetBoundsRect(HDC hdc, const RECT* rect, UINT flags) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return 0;
    const UINT ret = (dc->bounds_enabled ? DCB_ENABLE : DCB_DISABLE) | (RectEmpty(dc->bounds) ? DCB_RESET : DCB_SET);
    if (flags & DCB_RESET) dc->bounds = RECT{0, 0, 0, 0};
    if ((flags & DCB_ACCUMULATE) && rect) dc->bounds = RectUnion(dc->bounds, *rect);
    if (flags & DCB_ENABLE) dc->bounds_enabled = true;
    if (flags & DCB_DISABLE) dc->bounds_enabled = false;
    return ret;
}

UINT WINAPI NtGdiGetBoundsRect(HDC hdc, RECT* rect, UINT flags) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc) return 0;
    if (rect) *rect = dc->bounds;
    const UINT ret = RectEmpty(dc->bounds) ? DCB_RESET : DCB_SET;
    if (flags & DCB_RESET) dc->bounds = RECT{0, 0, 0, 0};
    return ret;
}

// ---- Text metrics ------------------------------------------------------------------------

BOOL WINAPI NtGdiGetTextMetricsW(HDC hdc, TEXTMETRICW* tm, ULONG flags) {
    LockGuard lock;
    (void)flags;
    DcObj* dc = DcGet(hdc);
    if (!dc || !tm) return FALSE;
    const TextCell c = TextCellOf(dc);
    const LOGFONTW* lf = LogFontOf(dc);
    std::memset(tm, 0, sizeof *tm);
    tm->tmHeight = c.height;
    tm->tmAscent = c.ascent;
    tm->tmDescent = c.descent;
    tm->tmAveCharWidth = c.width;
    tm->tmMaxCharWidth = c.width;
    tm->tmWeight = lf && lf->lfWeight ? lf->lfWeight : FW_NORMAL;
    tm->tmItalic = lf ? lf->lfItalic : 0;
    tm->tmUnderlined = lf ? lf->lfUnderline : 0;
    tm->tmStruckOut = lf ? lf->lfStrikeOut : 0;
    tm->tmFirstChar = 0x20;
    tm->tmLastChar = 0xff;
    tm->tmDefaultChar = 0x7f;
    tm->tmBreakChar = 0x20;
    // A fixed-pitch raster face: TMPF_FIXED_PITCH (GDI's inverted name for "variable") stays clear.
    tm->tmPitchAndFamily = static_cast<BYTE>(lf ? lf->lfPitchAndFamily & 0xf0 : 0);
    tm->tmCharSet = lf ? lf->lfCharSet : static_cast<BYTE>(ANSI_CHARSET);
    tm->tmDigitizedAspectX = tm->tmDigitizedAspectY = 96;
    return TRUE;
}

BOOL WINAPI NtGdiGetTextExtentExW(HDC hdc, const WCHAR* str, INT count, INT max_ext, INT* nfit, INT* dxs, SIZE* size,
                                  UINT flags) {
    LockGuard lock;
    (void)str; (void)flags;
    DcObj* dc = DcGet(hdc);
    if (!dc || count < 0) return FALSE;
    const TextCell c = TextCellOf(dc);
    const int advance = c.width + dc->attr->char_extra;
    int fit = 0;
    bool fits = true;
    for (INT i = 0; i < count; ++i) {
        const int ext = (i + 1) * advance;
        if (dxs) dxs[i] = ext;
        if (fits && (max_ext < 0 || ext <= max_ext)) fit = i + 1;
        else fits = false;
    }
    if (nfit) *nfit = fit;
    if (size) *size = SIZE{count * advance, c.height};
    return TRUE;
}

INT WINAPI NtGdiGetTextFaceW(HDC hdc, INT count, WCHAR* name, BOOL alias_name) {
    LockGuard lock;
    (void)alias_name;
    DcObj* dc = DcGet(hdc);
    if (!dc) return 0;
    auto* f = GdiGetT<FontObj>(dc->font, abi::kObjFont);
    const WCHAR* face = f && f->lf.lfFaceName[0] ? f->lf.lfFaceName : L"System";
    const INT len = static_cast<INT>(wcslen(face)) + 1;
    if (!name) return len;
    if (count <= 0) return 0;
    const INT n = std::min(count, len);
    std::memcpy(name, face, static_cast<std::size_t>(n - 1) * sizeof(WCHAR));
    name[n - 1] = 0;
    return n;
}

BOOL WINAPI NtGdiGetCharWidthW(HDC hdc, UINT first_char, UINT last_char, WCHAR* chars, ULONG flags, void* buffer) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc || !buffer) return FALSE;
    const int w = TextCellOf(dc).width;
    // With an explicit list of characters or glyph indices (NTGDI_GETCHARWIDTH_INDICES: GetCharWidthI with no list
    // means glyphs first..first+count-1) `last_char` is the count, as in Wine's win32u.
    const UINT n = (chars || (flags & 0x08 /* NTGDI_GETCHARWIDTH_INDICES */)) ? last_char : last_char - first_char + 1;
    for (UINT i = 0; i < n; ++i) {
        if (flags & 0x02 /* NTGDI_GETCHARWIDTH_INT */) static_cast<INT*>(buffer)[i] = w;
        else static_cast<FLOAT*>(buffer)[i] = static_cast<FLOAT>(w);
    }
    return TRUE;
}

BOOL WINAPI NtGdiGetCharWidthInfo(HDC hdc, abi::CharWidthInfo* info) {
    LockGuard lock;
    if (!DcGet(hdc) || !info) return FALSE;
    *info = abi::CharWidthInfo{};   // the glyphs fill their cells: no bearings
    return TRUE;
}

BOOL WINAPI NtGdiGetCharABCWidthsW(HDC hdc, UINT first, UINT last, WCHAR* chars, ULONG flags, void* buffer) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc || !buffer) return FALSE;
    const int w = TextCellOf(dc).width;
    const UINT n = (chars || (flags & 0x02 /* NTGDI_GETCHARABCWIDTHS_INDICES */)) ? last : last - first + 1;
    for (UINT i = 0; i < n; ++i) {
        if (flags & 0x01 /* NTGDI_GETCHARABCWIDTHS_INT */) static_cast<ABC*>(buffer)[i] = ABC{0, static_cast<UINT>(w), 0};
        else static_cast<ABCFLOAT*>(buffer)[i] = ABCFLOAT{0.0f, static_cast<FLOAT>(w), 0.0f};
    }
    return TRUE;
}

UINT WINAPI NtGdiGetTextCharsetInfo(HDC hdc, FONTSIGNATURE* fs, DWORD flags) {
    LockGuard lock;
    (void)flags;
    DcObj* dc = DcGet(hdc);
    if (!dc) return DEFAULT_CHARSET;
    if (fs) {
        std::memset(fs, 0, sizeof *fs);
        fs->fsCsb[0] = 1;   // FS_LATIN1
    }
    const LOGFONTW* lf = LogFontOf(dc);
    return lf ? lf->lfCharSet : static_cast<UINT>(ANSI_CHARSET);
}

DWORD WINAPI NtGdiGetFontData(HDC hdc, DWORD table, DWORD offset, void* buffer, DWORD length) {
    (void)hdc; (void)table; (void)offset; (void)buffer; (void)length;
    return GDI_ERROR;   // no font files behind the built-in face
}

UINT WINAPI NtGdiGetOutlineTextMetricsInternalW(HDC hdc, UINT cbData, OUTLINETEXTMETRICW* otm, ULONG opts) {
    (void)hdc; (void)cbData; (void)otm; (void)opts;
    NoteUnimplemented("NtGdiGetOutlineTextMetricsInternalW (no outline fonts)");
    return 0;
}

BOOL WINAPI NtGdiGetRealizationInfo(HDC hdc, void* info) {
    LockGuard lock;
    if (!DcGet(hdc) || !info) return FALSE;
    auto* ri = static_cast<DWORD*>(info);   // struct font_realization_info: size, flags, cache_num, instance_id, ...
    const DWORD size = ri[0];
    std::memset(ri + 1, 0, size > 4 ? size - 4 : 0);
    ri[1] = 3;   // scalable
    ri[3] = 1;   // instance id
    return TRUE;
}

DWORD WINAPI NtGdiGetGlyphIndicesW(HDC hdc, const WCHAR* str, INT count, WORD* indices, DWORD flags) {
    LockGuard lock;
    (void)flags;
    if (!DcGet(hdc) || !str || !indices || count < 0) return GDI_ERROR;
    for (INT i = 0; i < count; ++i) indices[i] = str[i];
    return static_cast<DWORD>(count);
}

BOOL WINAPI NtGdiFontIsLinked(HDC hdc) {
    (void)hdc;
    return FALSE;
}

DWORD WINAPI NtGdiGetKerningPairs(HDC hdc, DWORD count, KERNINGPAIR* kern_pair) {
    (void)hdc; (void)count; (void)kern_pair;
    return 0;   // the built-in face has no kerning
}

BOOL WINAPI NtGdiSetTextJustification(HDC hdc, INT extra, INT breaks) {
    LockGuard lock;
    (void)extra; (void)breaks;
    return DcGet(hdc) != nullptr;
}

}  // namespace sys
}  // namespace win10xbox::user
