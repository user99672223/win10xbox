// core/user GDI objects: the shared handle table at PEB.GdiSharedHandleTable
// (Wine's encoding, stock objects in Wine's init_stock_objects order so that
// gdi32's GetStockObject -- a table lookup at FIRST_GDI_HANDLE + n -- finds
// them), brushes, pens, fonts (LOGFONT only: no font engine), bitmaps and DIB
// sections, regions, palettes, and the NtGdi* object syscalls.
#include "user_internal.h"
#include "user_syscalls.h"

#include <algorithm>
#include <cstring>

namespace win10xbox::user {
namespace detail {

// ---- Regions -----------------------------------------------------------------------

RECT Rgn::Box() const {
    RECT b{0, 0, 0, 0};
    for (const RECT& r : rects) b = RectUnion(b, r);
    return b;
}

bool Rgn::Contains(int x, int y) const {
    for (const RECT& r : rects)
        if (PtInRectangle(r, x, y)) return true;
    return false;
}

namespace {
bool CellIn(const Rgn& g, int x, int y) { return g.Contains(x, y); }
}  // namespace

// Grid decomposition: every distinct x and y edge of both regions splits the
// plane into cells; a cell is in the result when the boolean op says so. The
// result is rebuilt row by row (vertical bands) with horizontal runs merged,
// then identical consecutive bands are merged -- a valid non-overlapping set.
Rgn CombineRgns(const Rgn& a, const Rgn& b, int mode) {
    if (mode == RGN_COPY) return a;
    if (mode == RGN_AND && (a.Empty() || b.Empty())) return Rgn{};
    if (mode == RGN_OR || mode == RGN_XOR) {
        if (a.Empty()) return b;
        if (b.Empty()) return a;
    }
    if (mode == RGN_DIFF && (a.Empty() || b.Empty())) return a;
    std::vector<int> xs, ys;
    for (const Rgn* g : {&a, &b})
        for (const RECT& r : g->rects) {
            xs.push_back(r.left); xs.push_back(r.right);
            ys.push_back(r.top); ys.push_back(r.bottom);
        }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    Rgn out;
    std::vector<RECT> prev_band;
    std::size_t prev_band_start = 0;
    for (std::size_t j = 0; j + 1 < ys.size(); ++j) {
        std::vector<RECT> band;
        const int y = ys[j];
        for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
            const int x = xs[i];
            const bool ia = CellIn(a, x, y), ib = CellIn(b, x, y);
            bool in = false;
            switch (mode) {
            case RGN_AND: in = ia && ib; break;
            case RGN_OR: in = ia || ib; break;
            case RGN_XOR: in = ia != ib; break;
            case RGN_DIFF: in = ia && !ib; break;
            default: in = ia; break;
            }
            if (!in) continue;
            if (!band.empty() && band.back().right == xs[i]) band.back().right = xs[i + 1];
            else band.push_back(RECT{xs[i], ys[j], xs[i + 1], ys[j + 1]});
        }
        // merge with the previous band when the runs are identical and the bands touch
        bool merged = false;
        if (!band.empty() && band.size() == prev_band.size() && !prev_band.empty() &&
            prev_band[0].bottom == ys[j]) {
            bool same = true;
            for (std::size_t k = 0; k < band.size() && same; ++k)
                same = band[k].left == prev_band[k].left && band[k].right == prev_band[k].right;
            if (same) {
                for (std::size_t k = 0; k < band.size(); ++k) out.rects[prev_band_start + k].bottom = ys[j + 1];
                for (RECT& r : prev_band) r.bottom = ys[j + 1];
                merged = true;
            }
        }
        if (!merged) {
            prev_band_start = out.rects.size();
            out.rects.insert(out.rects.end(), band.begin(), band.end());
            prev_band = band;
        }
    }
    return out;
}

// ---- The handle table ------------------------------------------------------------------

HGDIOBJ GdiHandleTable::Alloc(GdiObject* obj, DWORD type, void* user_pointer) {
    std::uint32_t idx;
    if (!free_.empty()) {
        idx = free_.back();
        free_.pop_back();
    } else if (next_unused_ < abi::kGdiMaxHandles) {
        idx = next_unused_++;
    } else {
        ERR(gdi, "out of GDI handles (%u live)", live_);
        return nullptr;
    }
    abi::GdiHandleEntry& e = entries_[idx];
    const UCHAR ext = static_cast<UCHAR>(type >> abi::kHandleTypeShift);
    UCHAR gen = static_cast<UCHAR>(abi::UniqueGeneration(e.Unique) + 1);
    if (gen >= 0x80) gen = 1;   // keeps bit 31 of the handle clear, like Wine
    e.Unique = abi::MakeUnique(ext, false, gen);
    e.Type = static_cast<UCHAR>(ext & 0x1f);
    e.Flags = 0;
    e.Owner = GetCurrentProcessId() & 0xffff;
    e.Object = reinterpret_cast<UINT64>(obj);
    e.UserPointer = reinterpret_cast<UINT64>(user_pointer);
    objects_[idx] = obj;
    ++live_;
    HGDIOBJ h = EntryHandle(idx, e.Unique);
    obj->type = type;
    obj->handle = h;
    return h;
}

GdiObject* GdiHandleTable::Lookup(HGDIOBJ handle, DWORD type) const {
    const auto v = static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(handle));
    const std::uint32_t idx = v & 0xffff;
    if (!v || idx >= abi::kGdiMaxHandles || !entries_[idx].Type) return nullptr;
    const USHORT hi = static_cast<USHORT>(v >> 16);
    if (hi && hi != entries_[idx].Unique) return nullptr;
    GdiObject* obj = objects_[idx];
    if (!obj) return nullptr;
    if (type) {
        const DWORD t = obj->type;
        const bool ok = t == type || (type == abi::kObjDc && t == abi::kObjMemDc) ||
                        (type == abi::kObjPen && t == abi::kObjExtPen);
        if (!ok) return nullptr;
    }
    return obj;
}

GdiObject* GdiHandleTable::Free(HGDIOBJ handle) {
    GdiObject* obj = Lookup(handle);
    if (!obj) return nullptr;
    const std::uint32_t idx = static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(handle)) & 0xffff;
    abi::GdiHandleEntry& e = entries_[idx];
    e.Type = 0;
    e.Object = 0;
    e.UserPointer = 0;
    e.Unique = abi::MakeUnique(0, false, abi::UniqueGeneration(e.Unique));
    objects_[idx] = nullptr;
    free_.push_back(idx);
    --live_;
    return obj;
}

void GdiHandleTable::MarkStock(HGDIOBJ handle) {
    GdiObject* obj = Lookup(handle);
    if (!obj) return;
    const std::uint32_t idx = static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(handle)) & 0xffff;
    abi::GdiHandleEntry& e = entries_[idx];
    e.Unique = static_cast<USHORT>(e.Unique | 0x80);
    obj->system = true;
    obj->handle = EntryHandle(idx, e.Unique);
}

HGDIOBJ GdiHandleTable::FullHandle(HGDIOBJ handle) const {
    GdiObject* obj = Lookup(handle);
    return obj ? obj->handle : nullptr;
}

// ---- Module-level object management -------------------------------------------------

GdiObject* GdiGet(HGDIOBJ h, DWORD type) {
    State& s = S();
    return s.gdi->Lookup(h, type);
}

HGDIOBJ GdiAlloc(GdiObject* obj, DWORD type, void* user_pointer) {
    State& s = S();
    HGDIOBJ h = s.gdi->Alloc(obj, type, user_pointer);
    if (!h) delete obj;
    return h;
}

namespace {

void DestroyObject(GdiObject* obj) {
    if (obj->type == abi::kObjBitmap) {
        auto* bmp = static_cast<BitmapObj*>(obj);
        if (bmp->dib_owned && bmp->dib_bits) VirtualFree(bmp->dib_bits, 0, MEM_RELEASE);
        else if (bmp->dib_section && bmp->dib_bits) UnmapViewOfFile(static_cast<std::uint8_t*>(bmp->dib_bits) - bmp->dib_offset);
    }
    if (obj->type == abi::kObjBrush) {
        auto* br = static_cast<BrushObj*>(obj);
        if (br->pattern) {
            State& s = S();
            if (GdiObject* p = s.gdi->Free(br->pattern)) DestroyObject(p);
        }
    }
    delete obj;
}

}  // namespace

bool GdiDelete(HGDIOBJ h) {
    State& s = S();
    GdiObject* obj = s.gdi->Lookup(h);
    if (!obj) return false;
    if (obj->system) return true;   // Windows: deleting a stock object "succeeds" and does nothing
    if (obj->type == abi::kObjDc || obj->type == abi::kObjMemDc) {
        auto* dc = static_cast<DcObj*>(obj);
        if (dc->cache || dc->own_dc) {
            WARN(gdi, "DeleteDC(%p) of a window DC: use ReleaseDC", h);
            return false;
        }
        FreeDcObj(static_cast<HDC>(h));
        return true;
    }
    if (obj->selected) {
        // Selected into a DC: Windows refuses for bitmaps and defers for the rest; deferring for all is
        // the behaviour programs count on (delete after the DC is gone).
        if (obj->type == abi::kObjBitmap) return false;
        obj->delete_pending = true;
        return true;
    }
    s.gdi->Free(h);
    DestroyObject(obj);
    BumpStat(&Stats::gdi_objects, 0);
    return true;
}

void GdiReleaseSelected(HGDIOBJ h) {
    State& s = S();
    GdiObject* obj = s.gdi->Lookup(h);
    if (!obj) return;
    if (obj->selected) --obj->selected;
    if (!obj->selected && obj->delete_pending) {
        s.gdi->Free(h);
        DestroyObject(obj);
    }
}

HBRUSH CreateSolidBrushObj(COLORREF color, bool system) {
    auto* br = new BrushObj;
    br->style = BS_SOLID;
    br->color = color & 0xffffff;
    auto h = static_cast<HBRUSH>(GdiAlloc(br, abi::kObjBrush));
    if (h && system) br->system = true;
    return h;
}

// ---- Default fonts ------------------------------------------------------------------------
namespace {

LOGFONTW MakeFont(const wchar_t* face, LONG height, LONG weight, BYTE pitch, BYTE charset = ANSI_CHARSET) {
    LOGFONTW lf{};
    lf.lfHeight = height;
    lf.lfWeight = weight;
    lf.lfCharSet = charset;
    lf.lfPitchAndFamily = pitch;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = DEFAULT_QUALITY;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    return lf;
}

HGDIOBJ StockFont(const LOGFONTW& lf) {
    auto* f = new FontObj;
    f->lf = lf;
    return GdiAlloc(f, abi::kObjFont);
}

HGDIOBJ StockBrush(UINT style, COLORREF c) {
    auto* br = new BrushObj;
    br->style = style;
    br->color = c;
    return GdiAlloc(br, abi::kObjBrush);
}

HGDIOBJ StockPen(UINT style, COLORREF c) {
    auto* p = new PenObj;
    p->style = style;
    p->color = c;
    return GdiAlloc(p, abi::kObjPen);
}

HGDIOBJ MakeBitmap(int w, int h, int bpp);

// Default system colours (Windows 10 defaults; GetSysColor).
const COLORREF kSysColors[31] = {
    RGB(200, 200, 200),   // COLOR_SCROLLBAR
    RGB(0, 0, 0),         // COLOR_BACKGROUND (the desktop)
    RGB(153, 180, 209),   // COLOR_ACTIVECAPTION
    RGB(191, 205, 219),   // COLOR_INACTIVECAPTION
    RGB(240, 240, 240),   // COLOR_MENU
    RGB(255, 255, 255),   // COLOR_WINDOW
    RGB(100, 100, 100),   // COLOR_WINDOWFRAME
    RGB(0, 0, 0),         // COLOR_MENUTEXT
    RGB(0, 0, 0),         // COLOR_WINDOWTEXT
    RGB(0, 0, 0),         // COLOR_CAPTIONTEXT
    RGB(180, 180, 180),   // COLOR_ACTIVEBORDER
    RGB(244, 247, 252),   // COLOR_INACTIVEBORDER
    RGB(171, 171, 171),   // COLOR_APPWORKSPACE
    RGB(0, 120, 215),     // COLOR_HIGHLIGHT
    RGB(255, 255, 255),   // COLOR_HIGHLIGHTTEXT
    RGB(240, 240, 240),   // COLOR_BTNFACE
    RGB(160, 160, 160),   // COLOR_BTNSHADOW
    RGB(109, 109, 109),   // COLOR_GRAYTEXT
    RGB(0, 0, 0),         // COLOR_BTNTEXT
    RGB(0, 0, 0),         // COLOR_INACTIVECAPTIONTEXT
    RGB(255, 255, 255),   // COLOR_BTNHIGHLIGHT
    RGB(105, 105, 105),   // COLOR_3DDKSHADOW
    RGB(227, 227, 227),   // COLOR_3DLIGHT
    RGB(0, 0, 0),         // COLOR_INFOTEXT
    RGB(255, 255, 225),   // COLOR_INFOBK
    RGB(0, 0, 0),         // 25: unused
    RGB(0, 102, 204),     // COLOR_HOTLIGHT
    RGB(185, 209, 234),   // COLOR_GRADIENTACTIVECAPTION
    RGB(215, 228, 242),   // COLOR_GRADIENTINACTIVECAPTION
    RGB(0, 120, 215),     // COLOR_MENUHILIGHT
    RGB(240, 240, 240),   // COLOR_MENUBAR
};
// The 20 static colours of the default palette.
const BYTE kStaticPalette[20][3] = {
    {0, 0, 0}, {128, 0, 0}, {0, 128, 0}, {128, 128, 0}, {0, 0, 128}, {128, 0, 128}, {0, 128, 128},
    {192, 192, 192}, {192, 220, 192}, {166, 202, 240}, {255, 251, 240}, {160, 160, 164}, {128, 128, 128},
    {255, 0, 0}, {0, 255, 0}, {255, 255, 0}, {0, 0, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
COLORREF g_sys_colors[31];
bool g_sys_colors_init = false;

}  // namespace

COLORREF SysColor(int index) {
    if (!g_sys_colors_init) {
        std::memcpy(g_sys_colors, kSysColors, sizeof g_sys_colors);
        g_sys_colors_init = true;
    }
    if (index < 0 || index >= 31) return 0;
    return g_sys_colors[index];
}

void SetSysColor(int index, COLORREF c) {
    SysColor(0);
    if (index < 0 || index >= 31) return;
    g_sys_colors[index] = c & 0xffffff;
    State& s = S();
    if (auto* br = GdiGetT<BrushObj>(s.sys_brush[index], abi::kObjBrush)) br->color = c & 0xffffff;
    if (auto* pen = GdiGetT<PenObj>(s.sys_pen[index], abi::kObjPen)) pen->color = c & 0xffffff;
}

HBRUSH SysColorBrush(int index) {
    State& s = S();
    if (index < 0 || index >= 31) return nullptr;
    return s.sys_brush[index];
}

HPEN SysColorPen(int index) {
    State& s = S();
    if (index < 0 || index >= 31) return nullptr;
    return s.sys_pen[index];
}

void CreateGdiTable(State& s) {
    const SIZE_T bytes = sizeof(abi::GdiHandleEntry) * abi::kGdiMaxHandles;
    s.gdi_entries = static_cast<abi::GdiHandleEntry*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    s.gdi = new GdiHandleTable(s.gdi_entries);
    // The stock objects, in init_stock_objects order (FIRST_GDI_HANDLE + n == stock object n).
    s.stock[WHITE_BRUSH] = StockBrush(BS_SOLID, RGB(255, 255, 255));
    s.stock[LTGRAY_BRUSH] = StockBrush(BS_SOLID, RGB(192, 192, 192));
    s.stock[GRAY_BRUSH] = StockBrush(BS_SOLID, RGB(128, 128, 128));
    s.stock[DKGRAY_BRUSH] = StockBrush(BS_SOLID, RGB(64, 64, 64));
    s.stock[BLACK_BRUSH] = StockBrush(BS_SOLID, RGB(0, 0, 0));
    s.stock[NULL_BRUSH] = StockBrush(BS_NULL, 0);
    s.stock[WHITE_PEN] = StockPen(PS_SOLID, RGB(255, 255, 255));
    s.stock[BLACK_PEN] = StockPen(PS_SOLID, RGB(0, 0, 0));
    s.stock[NULL_PEN] = StockPen(PS_NULL, 0);
    const LOGFONTW oem = MakeFont(L"Terminal", 12, FW_NORMAL, FIXED_PITCH | FF_MODERN, OEM_CHARSET);
    s.stock[9] = StockFont(oem);   // slot 9: the DPI-scaled OEM fixed font
    s.stock[OEM_FIXED_FONT] = StockFont(oem);
    s.stock[ANSI_FIXED_FONT] = StockFont(MakeFont(L"Courier", 12, FW_NORMAL, FIXED_PITCH | FF_MODERN));
    s.stock[ANSI_VAR_FONT] = StockFont(MakeFont(L"MS Sans Serif", 12, FW_NORMAL, VARIABLE_PITCH | FF_SWISS));
    const LOGFONTW system = MakeFont(L"System", 16, FW_BOLD, VARIABLE_PITCH | FF_SWISS);
    const LOGFONTW system_fixed = MakeFont(L"Fixedsys", 16, FW_NORMAL, FIXED_PITCH | FF_MODERN);
    const LOGFONTW gui = MakeFont(L"MS Shell Dlg", -11, FW_NORMAL, VARIABLE_PITCH | FF_SWISS);
    s.stock[SYSTEM_FONT] = StockFont(system);
    s.stock[DEVICE_DEFAULT_FONT] = StockFont(system);
    {
        auto* pal = new PaletteObj;   // DEFAULT_PALETTE: the 20 static colours
        for (const auto& c : kStaticPalette) pal->entries.push_back(PALETTEENTRY{c[0], c[1], c[2], 0});
        s.stock[DEFAULT_PALETTE] = GdiAlloc(pal, abi::kObjPal);
    }
    s.stock[SYSTEM_FIXED_FONT] = StockFont(system_fixed);
    s.stock[DEFAULT_GUI_FONT] = StockFont(gui);
    s.stock[DC_BRUSH] = StockBrush(BS_SOLID, RGB(255, 255, 255));
    s.stock[DC_PEN] = StockPen(PS_SOLID, RGB(0, 0, 0));
    s.stock[STOCK_LAST + 1] = MakeBitmap(1, 1, 1);   // DEFAULT_BITMAP
    s.stock[STOCK_LAST + 2] = StockFont(system);
    s.stock[STOCK_LAST + 3] = StockFont(system_fixed);
    s.stock[STOCK_LAST + 4] = StockFont(gui);
    for (HGDIOBJ& h : s.stock) {
        const std::uint32_t idx = static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(h)) & 0xffff;
        s.gdi->MarkStock(h);
        h = s.gdi->FullHandle(reinterpret_cast<HGDIOBJ>(static_cast<ULONG_PTR>(idx)));
    }
    if ((reinterpret_cast<ULONG_PTR>(s.stock[STOCK_LAST + 1]) & 0xffff) != abi::kFirstGdiHandle + STOCK_LAST + 1)
        ERR(gdi, "stock objects out of order: DEFAULT_BITMAP at %p", s.stock[STOCK_LAST + 1]);
    for (int i = 0; i < 31; ++i) {
        s.sys_brush[i] = CreateSolidBrushObj(SysColor(i), true);
        s.sys_pen[i] = static_cast<HPEN>(StockPen(PS_SOLID, SysColor(i)));
        if (auto* p = GdiGet(s.sys_pen[i])) p->system = true;
    }
}

// ---- Bitmaps and DIBs ------------------------------------------------------------------------

namespace {

HGDIOBJ MakeBitmap(int w, int h, int bpp) {
    auto* bmp = new BitmapObj;
    bmp->width = w;
    bmp->height = h;
    bmp->bpp = bpp;
    // Monochrome bitmaps start black (0), colour ones too.
    bmp->pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0xff000000u);
    return GdiAlloc(bmp, abi::kObjBitmap);
}

int DdbStride(int width, int bpp) { return ((width * bpp + 15) / 16) * 2; }
int DibStride(int width, int bpp) { return ((width * bpp + 31) / 32) * 4; }

// Reads one pixel of a packed row in `bpp` format as BGRA.
std::uint32_t ReadPacked(const std::uint8_t* row, int x, int bpp, const RGBQUAD* colors, int ncolors,
                         const DWORD* masks) {
    auto pal = [&](unsigned idx) -> std::uint32_t {
        if (!colors || static_cast<int>(idx) >= ncolors) return idx ? 0xffffffffu : 0xff000000u;
        const RGBQUAD& q = colors[idx];
        return 0xff000000u | (static_cast<std::uint32_t>(q.rgbRed) << 16) | (q.rgbGreen << 8) | q.rgbBlue;
    };
    switch (bpp) {
    case 1: return pal((row[x >> 3] >> (7 - (x & 7))) & 1);
    case 4: return pal((row[x >> 1] >> ((x & 1) ? 0 : 4)) & 0xf);
    case 8: return pal(row[x]);
    case 16: {
        const std::uint16_t v = static_cast<std::uint16_t>(row[2 * x] | (row[2 * x + 1] << 8));
        if (masks && masks[0] == 0xf800) {   // 565
            const unsigned r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;
            return 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
        }
        const unsigned r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, b = v & 0x1f;
        return 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
    }
    case 24: return 0xff000000u | (row[3 * x + 2] << 16) | (row[3 * x + 1] << 8) | row[3 * x];
    case 32: {
        std::uint32_t v;
        std::memcpy(&v, row + 4 * x, 4);
        return v | 0xff000000u;
    }
    }
    return 0xff000000u;
}

void WritePacked(std::uint8_t* row, int x, int bpp, std::uint32_t bgra, const RGBQUAD* colors, int ncolors,
                 const DWORD* masks) {
    auto nearest = [&](std::uint32_t c) -> unsigned {
        if (!colors || ncolors <= 0) return ((c & 0xffffff) == 0xffffff) ? 1u : 0u;
        int best = 0, best_d = 1 << 30;
        const int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
        for (int i = 0; i < ncolors; ++i) {
            const int d = (colors[i].rgbRed - r) * (colors[i].rgbRed - r) + (colors[i].rgbGreen - g) * (colors[i].rgbGreen - g) +
                          (colors[i].rgbBlue - b) * (colors[i].rgbBlue - b);
            if (d < best_d) { best_d = d; best = i; }
        }
        return static_cast<unsigned>(best);
    };
    switch (bpp) {
    case 1: {
        const unsigned bit = nearest(bgra) & 1;
        const std::uint8_t m = static_cast<std::uint8_t>(0x80 >> (x & 7));
        row[x >> 3] = static_cast<std::uint8_t>(bit ? (row[x >> 3] | m) : (row[x >> 3] & ~m));
        break;
    }
    case 4: {
        const unsigned v = nearest(bgra) & 0xf;
        std::uint8_t& b = row[x >> 1];
        b = static_cast<std::uint8_t>((x & 1) ? ((b & 0xf0) | v) : ((b & 0x0f) | (v << 4)));
        break;
    }
    case 8: row[x] = static_cast<std::uint8_t>(nearest(bgra)); break;
    case 16: {
        const unsigned r = (bgra >> 16) & 0xff, g = (bgra >> 8) & 0xff, b = bgra & 0xff;
        std::uint16_t v;
        if (masks && masks[0] == 0xf800) v = static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        else v = static_cast<std::uint16_t>(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
        row[2 * x] = static_cast<std::uint8_t>(v);
        row[2 * x + 1] = static_cast<std::uint8_t>(v >> 8);
        break;
    }
    case 24:
        row[3 * x] = static_cast<std::uint8_t>(bgra);
        row[3 * x + 1] = static_cast<std::uint8_t>(bgra >> 8);
        row[3 * x + 2] = static_cast<std::uint8_t>(bgra >> 16);
        break;
    case 32: {
        const std::uint32_t v = bgra & 0x00ffffffu;
        std::memcpy(row + 4 * x, &v, 4);
        break;
    }
    }
}

bool Is32Bgra(const BITMAPINFOHEADER& h, const DWORD* masks) {
    if (h.biBitCount != 32) return false;
    if (h.biCompression == BI_RGB) return true;
    return h.biCompression == BI_BITFIELDS && masks[0] == 0xff0000 && masks[1] == 0xff00 && masks[2] == 0xff;
}

int ColorCount(const BITMAPINFOHEADER& h) {
    if (h.biBitCount > 8) return 0;
    if (h.biClrUsed && h.biClrUsed < (1u << h.biBitCount)) return static_cast<int>(h.biClrUsed);
    return 1 << h.biBitCount;
}

}  // namespace

bool MakeDibView(const BITMAPINFO* bmi, UINT coloruse, const void* bits, DibView* out) {
    if (!bmi) return false;
    const BITMAPINFOHEADER& h = bmi->bmiHeader;
    if (h.biSize < sizeof(BITMAPINFOHEADER)) {
        if (h.biSize == sizeof(BITMAPCOREHEADER)) {
            FIXME(gdi, "BITMAPCOREHEADER DIBs are not supported");
        }
        return false;
    }
    if (h.biCompression != BI_RGB && h.biCompression != BI_BITFIELDS) {
        FIXME(gdi, "DIB compression %lu not supported", h.biCompression);
        return false;
    }
    if (h.biWidth <= 0 || !h.biHeight || h.biPlanes != 1) return false;
    switch (h.biBitCount) { case 1: case 4: case 8: case 16: case 24: case 32: break; default: return false; }
    out->width = h.biWidth;
    out->height = h.biHeight < 0 ? -h.biHeight : h.biHeight;
    out->bottom_up = h.biHeight > 0;
    out->bpp = h.biBitCount;
    out->stride = DibStride(h.biWidth, h.biBitCount);
    out->bits = static_cast<const std::uint8_t*>(bits);
    const auto* after = reinterpret_cast<const std::uint8_t*>(bmi) + h.biSize;
    out->masks[0] = out->masks[1] = out->masks[2] = 0;
    if (h.biCompression == BI_BITFIELDS) {
        // In a BITMAPINFOHEADER the masks follow it; V4/V5 headers carry them inside.
        if (h.biSize >= sizeof(BITMAPV4HEADER)) std::memcpy(out->masks, reinterpret_cast<const std::uint8_t*>(bmi) + 40, 12);
        else { std::memcpy(out->masks, after, 12); after += 12; }
    } else if (h.biBitCount == 16) {
        out->masks[0] = 0x7c00; out->masks[1] = 0x3e0; out->masks[2] = 0x1f;
    } else if (h.biBitCount == 32) {
        out->masks[0] = 0xff0000; out->masks[1] = 0xff00; out->masks[2] = 0xff;
    }
    out->colors.clear();
    const int ncolors = ColorCount(h);
    if (ncolors) {
        if (coloruse == DIB_PAL_COLORS) {
            // Indices into the DC's palette: use the default palette.
            State& s = S();
            auto* pal = GdiGetT<PaletteObj>(s.stock[DEFAULT_PALETTE], abi::kObjPal);
            const auto* idx = reinterpret_cast<const WORD*>(after);
            for (int i = 0; i < ncolors; ++i) {
                PALETTEENTRY pe = pal && idx[i] < pal->entries.size() ? pal->entries[idx[i]] : PALETTEENTRY{};
                out->colors.push_back(RGBQUAD{pe.peBlue, pe.peGreen, pe.peRed, 0});
            }
        } else {
            const auto* q = reinterpret_cast<const RGBQUAD*>(after);
            out->colors.assign(q, q + ncolors);
        }
    }
    return true;
}

std::uint32_t DibViewPixel(const DibView& v, int x, int y) {
    if (x < 0 || y < 0 || x >= v.width || y >= v.height) return 0xff000000u;
    const int row = v.bottom_up ? v.height - 1 - y : y;
    return ReadPacked(v.bits + static_cast<std::ptrdiff_t>(row) * v.stride, x, v.bpp, v.colors.data(),
                      static_cast<int>(v.colors.size()), v.masks);
}

std::uint32_t BitmapPixel(const BitmapObj& bmp, int x, int y) {
    if (x < 0 || y < 0 || x >= bmp.width || y >= bmp.height) return 0xff000000u;
    if (!bmp.dib) return bmp.pixels[static_cast<std::size_t>(y) * bmp.width + x] | 0xff000000u;
    const int row = bmp.dib_bottom_up ? bmp.height - 1 - y : y;
    return ReadPacked(static_cast<const std::uint8_t*>(bmp.dib_bits) + static_cast<std::ptrdiff_t>(row) * bmp.dib_stride,
                      x, bmp.dib_header.biBitCount, bmp.dib_colors.data(), static_cast<int>(bmp.dib_colors.size()),
                      bmp.dib_masks);
}

void SetBitmapPixel(BitmapObj& bmp, int x, int y, std::uint32_t bgra) {
    if (x < 0 || y < 0 || x >= bmp.width || y >= bmp.height) return;
    if (!bmp.dib) {
        if (bmp.bpp == 1) bgra = ((bgra & 0xffffff) == 0xffffff) ? 0xffffffffu : 0xff000000u;
        bmp.pixels[static_cast<std::size_t>(y) * bmp.width + x] = bgra | 0xff000000u;
        return;
    }
    const int row = bmp.dib_bottom_up ? bmp.height - 1 - y : y;
    WritePacked(static_cast<std::uint8_t*>(bmp.dib_bits) + static_cast<std::ptrdiff_t>(row) * bmp.dib_stride, x,
                bmp.dib_header.biBitCount, bgra, bmp.dib_colors.data(), static_cast<int>(bmp.dib_colors.size()),
                bmp.dib_masks);
}

bool BitmapTarget(BitmapObj& bmp, PixelTarget* out) {
    if (!bmp.dib) {
        if (bmp.bpp == 1) return false;   // drawn pixel by pixel (black/white only)
        out->bits = reinterpret_cast<std::uint8_t*>(bmp.pixels.data());
        out->width = bmp.width;
        out->height = bmp.height;
        out->stride = static_cast<std::ptrdiff_t>(bmp.width) * 4;
        return true;
    }
    if (!Is32Bgra(bmp.dib_header, bmp.dib_masks)) return false;
    out->width = bmp.width;
    out->height = bmp.height;
    if (bmp.dib_bottom_up) {
        out->bits = static_cast<std::uint8_t*>(bmp.dib_bits) + static_cast<std::ptrdiff_t>(bmp.height - 1) * bmp.dib_stride;
        out->stride = -static_cast<std::ptrdiff_t>(bmp.dib_stride);
    } else {
        out->bits = static_cast<std::uint8_t*>(bmp.dib_bits);
        out->stride = bmp.dib_stride;
    }
    return true;
}

}  // namespace detail

// ---- Syscalls -------------------------------------------------------------------------------
namespace sys {
using namespace detail;

HBRUSH WINAPI NtGdiCreateSolidBrush(COLORREF color, HBRUSH brush) {
    LockGuard lock;
    if (brush) FIXME(gdi, "reusing brush %p not supported", brush);
    HBRUSH h = CreateSolidBrushObj(color, false);
    TRACE(gdi, "%06lx -> %p", color, h);
    return h;
}

HBRUSH WINAPI NtGdiCreateHatchBrushInternal(INT style, COLORREF color, BOOL pen) {
    LockGuard lock;
    if (style < HS_HORIZONTAL || style > HS_DIAGCROSS) return nullptr;
    auto* br = new BrushObj;
    br->style = BS_HATCHED;
    br->color = color & 0xffffff;
    br->hatch = static_cast<ULONG_PTR>(style);
    (void)pen;
    return static_cast<HBRUSH>(GdiAlloc(br, abi::kObjBrush));
}

HBRUSH WINAPI NtGdiCreatePatternBrushInternal(HBITMAP hbitmap, BOOL pen, BOOL is_8x8) {
    LockGuard lock;
    auto* src = GdiGetT<BitmapObj>(hbitmap, abi::kObjBitmap);
    if (!src) return nullptr;
    // A private copy (the application may delete or change the bitmap).
    auto* copy = new BitmapObj;
    copy->width = src->width;
    copy->height = src->height;
    copy->bpp = src->bpp == 1 ? 1 : 32;
    copy->pixels.resize(static_cast<std::size_t>(copy->width) * copy->height);
    for (int y = 0; y < copy->height; ++y)
        for (int x = 0; x < copy->width; ++x) copy->pixels[static_cast<std::size_t>(y) * copy->width + x] = BitmapPixel(*src, x, y);
    HGDIOBJ hcopy = GdiAlloc(copy, abi::kObjBitmap);
    if (!hcopy) return nullptr;
    auto* br = new BrushObj;
    br->style = BS_PATTERN;
    br->pattern = hcopy;
    (void)pen; (void)is_8x8;
    return static_cast<HBRUSH>(GdiAlloc(br, abi::kObjBrush));
}

HPEN WINAPI NtGdiCreatePen(INT style, INT width, COLORREF color, HBRUSH brush) {
    LockGuard lock;
    (void)brush;
    auto* p = new PenObj;
    p->style = static_cast<UINT>(style) & PS_STYLE_MASK;
    p->width = width < 0 ? -width : width;
    p->color = color & 0xffffff;
    if (p->style > PS_INSIDEFRAME) p->style = PS_SOLID;
    return static_cast<HPEN>(GdiAlloc(p, abi::kObjPen));
}

HPEN WINAPI NtGdiExtCreatePen(DWORD style, DWORD width, ULONG brush_style, ULONG color, ULONG_PTR client_hatch,
                              ULONG_PTR hatch, DWORD style_count, const DWORD* style_bits, ULONG dib_size,
                              BOOL old_style, HBRUSH brush) {
    LockGuard lock;
    (void)client_hatch; (void)hatch; (void)style_count; (void)style_bits; (void)dib_size; (void)brush;
    auto* p = new PenObj;
    p->style = style & PS_STYLE_MASK;
    p->width = static_cast<int>(width);
    p->color = brush_style == BS_NULL ? 0 : (color & 0xffffff);
    if (brush_style == BS_NULL) p->style = PS_NULL;
    p->ext = !old_style;
    if (brush_style != BS_SOLID && brush_style != BS_NULL) FIXME(gdi, "pen brush style %lu drawn solid", brush_style);
    return static_cast<HPEN>(GdiAlloc(p, p->ext ? abi::kObjExtPen : abi::kObjPen));
}

HFONT WINAPI NtGdiHfontCreate(const void* logfont, ULONG unk2, ULONG unk3, ULONG unk4, void* data) {
    LockGuard lock;
    (void)unk2; (void)unk3; (void)unk4; (void)data;
    if (!logfont) return nullptr;
    auto* f = new FontObj;
    // ENUMLOGFONTEXDVW starts with ENUMLOGFONTEXW which starts with LOGFONTW.
    std::memcpy(&f->lf, logfont, sizeof(LOGFONTW));
    TRACE(gdi, "font %ls height %ld weight %ld", f->lf.lfFaceName, f->lf.lfHeight, f->lf.lfWeight);
    return static_cast<HFONT>(GdiAlloc(f, abi::kObjFont));
}

HBITMAP WINAPI NtGdiCreateBitmap(INT width, INT height, UINT planes, UINT bpp, const void* bits) {
    LockGuard lock;
    if (width < 0 || height < 0 || planes != 1) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (!width || !height) return static_cast<HBITMAP>(S().stock[STOCK_LAST + 1]);
    if (bpp > 1 && bpp <= 32) bpp = 32;   // DDBs of every colour depth are kept at the screen's
    else if (bpp != 1) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    HGDIOBJ h = MakeBitmap(width, height, static_cast<int>(bpp));
    if (h && bits) {
        auto* bmp = GdiGetT<BitmapObj>(h, abi::kObjBitmap);
        const int stride = DdbStride(width, static_cast<int>(bpp));
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                SetBitmapPixel(*bmp, x, y, ReadPacked(static_cast<const std::uint8_t*>(bits) + static_cast<std::ptrdiff_t>(y) * stride,
                                                      x, static_cast<int>(bpp), nullptr, 0, nullptr));
    }
    return static_cast<HBITMAP>(h);
}

HBITMAP WINAPI NtGdiCreateCompatibleBitmap(HDC hdc, INT width, INT height) {
    LockGuard lock;
    DcObj* dc = DcGet(hdc);
    if (!dc || width < 0 || height < 0) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    int bpp = 32;
    if (dc->kind == DcKind::Memory) {
        // The classic: compatible with a memory DC means compatible with the bitmap selected in it.
        if (auto* sel = GdiGetT<BitmapObj>(dc->bitmap, abi::kObjBitmap)) bpp = sel->dib ? sel->dib_header.biBitCount : sel->bpp;
        if (bpp != 1) bpp = 32;
    }
    if (!width || !height) return static_cast<HBITMAP>(S().stock[STOCK_LAST + 1]);
    HGDIOBJ h = MakeBitmap(width, height, bpp);
    TRACE(gdi, "%p %dx%d bpp %d -> %p", hdc, width, height, bpp, h);
    return static_cast<HBITMAP>(h);
}

HBITMAP WINAPI NtGdiCreateDIBSection(HDC hdc, HANDLE section, DWORD offset, const BITMAPINFO* bmi, UINT usage,
                                     UINT header_size, ULONG flags, ULONG_PTR color_space, void** bits) {
    LockGuard lock;
    (void)hdc; (void)header_size; (void)flags; (void)color_space;
    if (bits) *bits = nullptr;
    DibView v;
    if (!MakeDibView(bmi, usage, nullptr, &v)) {
        SetLastErr(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    auto* bmp = new BitmapObj;
    bmp->dib = true;
    bmp->width = v.width;
    bmp->height = v.height;
    bmp->bpp = v.bpp;
    bmp->dib_stride = v.stride;
    bmp->dib_bottom_up = v.bottom_up;
    bmp->dib_header = bmi->bmiHeader;
    bmp->dib_header.biSizeImage = static_cast<DWORD>(v.stride * v.height);
    std::memcpy(bmp->dib_masks, v.masks, sizeof v.masks);
    bmp->dib_colors = v.colors;
    const SIZE_T size = static_cast<SIZE_T>(v.stride) * v.height;
    if (section) {
        const DWORD map_offset = offset & ~0xffffu;
        void* view = MapViewOfFile(section, FILE_MAP_WRITE, 0, map_offset, size + (offset - map_offset));
        if (!view) {
            FIXME(gdi, "DIB section on section %p: MapViewOfFile failed (%lu)", section, GetLastError());
            delete bmp;
            return nullptr;
        }
        bmp->dib_section = section;
        bmp->dib_offset = offset - map_offset;
        bmp->dib_bits = static_cast<std::uint8_t*>(view) + bmp->dib_offset;
    } else {
        bmp->dib_bits = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        bmp->dib_owned = true;
        if (!bmp->dib_bits) {
            delete bmp;
            SetLastErr(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
    }
    void* p = bmp->dib_bits;
    HGDIOBJ h = GdiAlloc(bmp, abi::kObjBitmap);
    if (h && bits) *bits = p;
    TRACE(gdi, "%dx%d %d bpp%s -> %p bits %p", v.width, v.height, v.bpp, v.bottom_up ? " bottom-up" : "", h, p);
    return static_cast<HBITMAP>(h);
}

INT WINAPI NtGdiSetDIBitsInternal(HDC hdc, HBITMAP hbitmap, UINT startscan, UINT lines, const void* bits,
                                  const BITMAPINFO* info, UINT coloruse, UINT max_bits, UINT max_info);

HBITMAP WINAPI NtGdiCreateDIBitmapInternal(HDC hdc, INT width, INT height, DWORD init, const void* bits,
                                           const BITMAPINFO* data, UINT coloruse, UINT max_info, UINT max_bits,
                                           ULONG flags, HANDLE xform) {
    (void)flags; (void)xform;
    HBITMAP h;
    {
        LockGuard lock;
        int bpp = 32;
        if (!hdc) bpp = 1;   // CreateDIBitmap(NULL, ...) makes a monochrome bitmap
        if (width < 0 || height < 0) return nullptr;
        h = static_cast<HBITMAP>((!width || !height) ? S().stock[STOCK_LAST + 1] : MakeBitmap(width, height, bpp));
    }
    if (h && (init & CBM_INIT) && bits && data)
        NtGdiSetDIBitsInternal(hdc, h, 0, static_cast<UINT>(height < 0 ? -height : height), bits, data, coloruse,
                               max_bits, max_info);
    return h;
}

BOOL WINAPI NtGdiDeleteObjectApp(HGDIOBJ obj) {
    LockGuard lock;
    const bool ok = GdiDelete(obj);
    TRACE(gdi, "%p%s", obj, ok ? "" : " failed");
    if (!ok) SetLastErr(ERROR_INVALID_HANDLE);
    return ok;
}

INT WINAPI NtGdiExtGetObjectW(HGDIOBJ handle, INT count, void* buffer) {
    LockGuard lock;
    GdiObject* obj = GdiGet(handle);
    if (!obj) {
        SetLastErr(ERROR_INVALID_HANDLE);
        return 0;
    }
    auto put = [&](const void* data, int size) -> INT {
        if (!buffer) return size;
        const int n = count < size ? count : size;
        if (n > 0) std::memcpy(buffer, data, static_cast<std::size_t>(n));
        return n;
    };
    switch (obj->type) {
    case abi::kObjBrush: {
        auto* br = static_cast<BrushObj*>(obj);
        LOGBRUSH lb{br->style, br->color, br->style == BS_PATTERN ? reinterpret_cast<ULONG_PTR>(br->pattern) : br->hatch};
        return put(&lb, sizeof lb);
    }
    case abi::kObjPen: {
        auto* p = static_cast<PenObj*>(obj);
        LOGPEN lp{p->style, POINT{p->width, 0}, p->color};
        return put(&lp, sizeof lp);
    }
    case abi::kObjExtPen: {
        auto* p = static_cast<PenObj*>(obj);
        EXTLOGPEN ep{};
        ep.elpPenStyle = p->style | PS_GEOMETRIC;
        ep.elpWidth = static_cast<DWORD>(p->width);
        ep.elpBrushStyle = p->style == PS_NULL ? BS_NULL : BS_SOLID;
        ep.elpColor = p->color;
        return put(&ep, sizeof ep);
    }
    case abi::kObjFont: {
        auto* f = static_cast<FontObj*>(obj);
        return put(&f->lf, sizeof f->lf);
    }
    case abi::kObjPal: {
        auto* pal = static_cast<PaletteObj*>(obj);
        const WORD n = static_cast<WORD>(pal->entries.size());
        return put(&n, sizeof n);
    }
    case abi::kObjBitmap: {
        auto* bmp = static_cast<BitmapObj*>(obj);
        DIBSECTION ds{};
        ds.dsBm.bmType = 0;
        ds.dsBm.bmWidth = bmp->width;
        ds.dsBm.bmHeight = bmp->height;
        ds.dsBm.bmPlanes = 1;
        if (bmp->dib) {
            ds.dsBm.bmBitsPixel = bmp->dib_header.biBitCount;
            ds.dsBm.bmWidthBytes = bmp->dib_stride;
            ds.dsBm.bmBits = bmp->dib_bits;
            ds.dsBmih = bmp->dib_header;
            ds.dsBmih.biHeight = bmp->height;   // GetObject reports a positive height
            std::memcpy(ds.dsBitfields, bmp->dib_masks, sizeof ds.dsBitfields);
            ds.dshSection = bmp->dib_section;
            ds.dsOffset = bmp->dib_offset;
            if (count >= static_cast<INT>(sizeof(DIBSECTION))) return put(&ds, sizeof ds);
        } else {
            ds.dsBm.bmBitsPixel = static_cast<WORD>(bmp->bpp);
            ds.dsBm.bmWidthBytes = DdbStride(bmp->width, bmp->bpp);
        }
        return put(&ds.dsBm, sizeof(BITMAP));
    }
    case abi::kObjRegion:
        return 0;
    default:
        return 0;
    }
}

BOOL WINAPI NtGdiUnrealizeObject(HGDIOBJ obj) {
    LockGuard lock;
    return GdiGet(obj) != nullptr;
}

LONG WINAPI NtGdiGetBitmapBits(HBITMAP bitmap, LONG count, void* bits) {
    LockGuard lock;
    auto* bmp = GdiGetT<BitmapObj>(bitmap, abi::kObjBitmap);
    if (!bmp) return 0;
    const int bpp = bmp->dib ? bmp->dib_header.biBitCount : bmp->bpp;
    const int stride = DdbStride(bmp->width, bpp);
    const LONG total = stride * bmp->height;
    if (!bits) return total;
    if (count > total) count = total;
    std::vector<std::uint8_t> row(static_cast<std::size_t>(stride));
    LONG done = 0;
    for (int y = 0; y < bmp->height && done < count; ++y) {
        std::fill(row.begin(), row.end(), 0);
        for (int x = 0; x < bmp->width; ++x) WritePacked(row.data(), x, bpp, BitmapPixel(*bmp, x, y), nullptr, 0, nullptr);
        const LONG n = std::min<LONG>(stride, count - done);
        std::memcpy(static_cast<std::uint8_t*>(bits) + done, row.data(), static_cast<std::size_t>(n));
        done += n;
    }
    return done;
}

LONG WINAPI NtGdiSetBitmapBits(HBITMAP hbitmap, LONG count, const void* bits) {
    LockGuard lock;
    auto* bmp = GdiGetT<BitmapObj>(hbitmap, abi::kObjBitmap);
    if (!bmp || !bits || count < 0) return 0;
    const int bpp = bmp->dib ? bmp->dib_header.biBitCount : bmp->bpp;
    const int stride = DdbStride(bmp->width, bpp);
    LONG used = 0;
    for (int y = 0; y < bmp->height && used + stride <= count; ++y, used += stride)
        for (int x = 0; x < bmp->width; ++x)
            SetBitmapPixel(*bmp, x, y, ReadPacked(static_cast<const std::uint8_t*>(bits) + used, x, bpp, nullptr, 0, nullptr));
    return used;
}

BOOL WINAPI NtGdiGetBitmapDimension(HBITMAP bitmap, SIZE* size) {
    LockGuard lock;
    auto* bmp = GdiGetT<BitmapObj>(bitmap, abi::kObjBitmap);
    if (!bmp || !size) return FALSE;
    *size = bmp->dimension;
    return TRUE;
}

BOOL WINAPI NtGdiSetBitmapDimension(HBITMAP hbitmap, INT x, INT y, SIZE* prev_size) {
    LockGuard lock;
    auto* bmp = GdiGetT<BitmapObj>(hbitmap, abi::kObjBitmap);
    if (!bmp) return FALSE;
    if (prev_size) *prev_size = bmp->dimension;
    bmp->dimension = SIZE{x, y};
    return TRUE;
}

INT WINAPI NtGdiGetDIBitsInternal(HDC hdc, HBITMAP hbitmap, UINT startscan, UINT lines, void* bits, BITMAPINFO* info,
                                  UINT coloruse, UINT max_bits, UINT max_info) {
    LockGuard lock;
    (void)hdc; (void)max_bits; (void)max_info;
    auto* bmp = GdiGetT<BitmapObj>(hbitmap, abi::kObjBitmap);
    if (!bmp || !info) return 0;
    BITMAPINFOHEADER& h = info->bmiHeader;
    if (h.biSize < sizeof(BITMAPINFOHEADER)) {
        FIXME(gdi, "GetDIBits with header size %lu", h.biSize);
        return 0;
    }
    const int src_bpp = bmp->dib ? bmp->dib_header.biBitCount : bmp->bpp;
    if (!h.biBitCount) {
        // Query: describe the bitmap.
        h.biWidth = bmp->width;
        h.biHeight = bmp->dib && !bmp->dib_bottom_up ? -bmp->height : bmp->height;
        h.biPlanes = 1;
        h.biBitCount = static_cast<WORD>(src_bpp);
        h.biCompression = BI_RGB;
        if (src_bpp == 16 || src_bpp == 32) {
            if (bmp->dib && bmp->dib_header.biCompression == BI_BITFIELDS) {
                h.biCompression = BI_BITFIELDS;
                std::memcpy(info->bmiColors, bmp->dib_masks, 12);
            }
        }
        h.biSizeImage = static_cast<DWORD>(DibStride(bmp->width, src_bpp) * bmp->height);
        h.biXPelsPerMeter = h.biYPelsPerMeter = 0;
        h.biClrUsed = h.biClrImportant = 0;
        return static_cast<INT>(bmp->height);
    }
    const int bpp = h.biBitCount;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return 0;
    const int width = h.biWidth;
    const bool bottom_up = h.biHeight > 0;
    const int height = bottom_up ? h.biHeight : -h.biHeight;
    if (width <= 0 || !height) return 0;
    // Colour table for paletted formats: a grey ramp for 8/4 bpp, black/white for 1 bpp.
    const int ncolors = bpp <= 8 ? (1 << bpp) : 0;
    std::vector<RGBQUAD> colors;
    if (ncolors) {
        if (bmp->dib && static_cast<int>(bmp->dib_colors.size()) == ncolors) colors = bmp->dib_colors;
        else
            for (int i = 0; i < ncolors; ++i) {
                const BYTE v = static_cast<BYTE>(ncolors == 2 ? i * 255 : i * 255 / (ncolors - 1));
                colors.push_back(RGBQUAD{v, v, v, 0});
            }
        if (coloruse == DIB_RGB_COLORS) std::memcpy(info->bmiColors, colors.data(), colors.size() * sizeof(RGBQUAD));
        h.biClrUsed = 0;
    }
    DWORD masks[3] = {0x7c00, 0x3e0, 0x1f};
    if (h.biCompression == BI_BITFIELDS) std::memcpy(masks, info->bmiColors, 12);
    else if (bpp == 32) { masks[0] = 0xff0000; masks[1] = 0xff00; masks[2] = 0xff; }
    const int stride = DibStride(width, bpp);
    h.biSizeImage = static_cast<DWORD>(stride * height);
    if (!bits) return static_cast<INT>(height);
    if (startscan >= static_cast<UINT>(height)) return 0;
    if (lines > static_cast<UINT>(height) - startscan) lines = static_cast<UINT>(height) - startscan;
    for (UINT i = 0; i < lines; ++i) {
        const int scan = static_cast<int>(startscan + i);          // DIB scan line index (0 = first row in memory)
        const int y = bottom_up ? height - 1 - scan : scan;        // top-down image row
        auto* row = static_cast<std::uint8_t*>(bits) + static_cast<std::ptrdiff_t>(i) * stride;
        std::memset(row, 0, static_cast<std::size_t>(stride));
        for (int x = 0; x < width; ++x) {
            const std::uint32_t px = (x < bmp->width && y < bmp->height) ? BitmapPixel(*bmp, x, y) : 0xff000000u;
            WritePacked(row, x, bpp, px, colors.data(), ncolors, masks);
        }
    }
    return static_cast<INT>(lines);
}

INT WINAPI NtGdiSetDIBitsInternal(HDC hdc, HBITMAP hbitmap, UINT startscan, UINT lines, const void* bits,
                                  const BITMAPINFO* info, UINT coloruse, UINT max_bits, UINT max_info) {
    LockGuard lock;
    (void)hdc; (void)max_bits; (void)max_info;
    auto* bmp = GdiGetT<BitmapObj>(hbitmap, abi::kObjBitmap);
    DibView v;
    if (!bmp || !bits || !MakeDibView(info, coloruse, bits, &v)) return 0;
    if (startscan >= static_cast<UINT>(v.height)) return 0;
    if (lines > static_cast<UINT>(v.height) - startscan) lines = static_cast<UINT>(v.height) - startscan;
    // `bits` holds `lines` scan lines starting at `startscan` (scan 0 is the bottom row of a bottom-up DIB).
    for (UINT i = 0; i < lines; ++i) {
        const int scan = static_cast<int>(startscan + i);
        const int y = v.bottom_up ? v.height - 1 - scan : scan;
        const std::uint8_t* row = v.bits + static_cast<std::ptrdiff_t>(i) * v.stride;
        for (int x = 0; x < v.width && x < bmp->width; ++x)
            SetBitmapPixel(*bmp, x, y, ReadPacked(row, x, v.bpp, v.colors.data(), static_cast<int>(v.colors.size()), v.masks));
    }
    return static_cast<INT>(lines);
}

// ---- Regions ----------------------------------------------------------------------------------

HRGN WINAPI NtGdiCreateRectRgn(INT left, INT top, INT right, INT bottom) {
    LockGuard lock;
    auto* r = new RegionObj;
    RECT rc{std::min(left, right), std::min(top, bottom), std::max(left, right), std::max(top, bottom)};
    r->rgn.Set(rc);
    return static_cast<HRGN>(GdiAlloc(r, abi::kObjRegion));
}

HRGN WINAPI NtGdiExtCreateRegion(const XFORM* xform, DWORD count, const RGNDATA* data) {
    LockGuard lock;
    if (!data || data->rdh.iType != RDH_RECTANGLES) return nullptr;
    if (xform) FIXME(gdi, "region transform ignored");
    (void)count;
    auto* r = new RegionObj;
    const auto* rects = reinterpret_cast<const RECT*>(data->Buffer);
    Rgn acc;
    for (DWORD i = 0; i < data->rdh.nCount; ++i) {
        Rgn one;
        one.Set(rects[i]);
        acc = CombineRgns(acc, one, RGN_OR);
    }
    r->rgn = acc;
    return static_cast<HRGN>(GdiAlloc(r, abi::kObjRegion));
}

INT WINAPI NtGdiCombineRgn(HRGN dest, HRGN src1, HRGN src2, INT mode) {
    LockGuard lock;
    auto* d = GdiGetT<RegionObj>(dest, abi::kObjRegion);
    auto* a = GdiGetT<RegionObj>(src1, abi::kObjRegion);
    if (!d || !a) return ERROR;
    if (mode == RGN_COPY) {
        d->rgn = a->rgn;
        return d->rgn.Complexity();
    }
    auto* b = GdiGetT<RegionObj>(src2, abi::kObjRegion);
    if (!b || mode < RGN_AND || mode > RGN_COPY) return ERROR;
    d->rgn = CombineRgns(a->rgn, b->rgn, mode);
    return d->rgn.Complexity();
}

INT WINAPI NtGdiGetRgnBox(HRGN hrgn, RECT* rect) {
    LockGuard lock;
    auto* r = GdiGetT<RegionObj>(hrgn, abi::kObjRegion);
    if (!r || !rect) return ERROR;
    *rect = r->rgn.Box();
    return r->rgn.Complexity();
}

BOOL WINAPI NtGdiSetRectRgn(HRGN hrgn, INT left, INT top, INT right, INT bottom) {
    LockGuard lock;
    auto* r = GdiGetT<RegionObj>(hrgn, abi::kObjRegion);
    if (!r) return FALSE;
    r->rgn.Set(RECT{std::min(left, right), std::min(top, bottom), std::max(left, right), std::max(top, bottom)});
    return TRUE;
}

INT WINAPI NtGdiOffsetRgn(HRGN hrgn, INT x, INT y) {
    LockGuard lock;
    auto* r = GdiGetT<RegionObj>(hrgn, abi::kObjRegion);
    if (!r) return ERROR;
    r->rgn.Offset(x, y);
    return r->rgn.Complexity();
}

BOOL WINAPI NtGdiPtInRegion(HRGN hrgn, INT x, INT y) {
    LockGuard lock;
    auto* r = GdiGetT<RegionObj>(hrgn, abi::kObjRegion);
    return r && r->rgn.Contains(x, y);
}

BOOL WINAPI NtGdiRectInRegion(HRGN hrgn, const RECT* rect) {
    LockGuard lock;
    auto* r = GdiGetT<RegionObj>(hrgn, abi::kObjRegion);
    if (!r || !rect) return FALSE;
    for (const RECT& rc : r->rgn.rects)
        if (!RectEmpty(RectIntersect(rc, *rect))) return TRUE;
    return FALSE;
}

BOOL WINAPI NtGdiEqualRgn(HRGN hrgn1, HRGN hrgn2) {
    LockGuard lock;
    auto* a = GdiGetT<RegionObj>(hrgn1, abi::kObjRegion);
    auto* b = GdiGetT<RegionObj>(hrgn2, abi::kObjRegion);
    if (!a || !b) return FALSE;
    return CombineRgns(a->rgn, b->rgn, RGN_XOR).Empty();
}

DWORD WINAPI NtGdiGetRegionData(HRGN hrgn, DWORD count, RGNDATA* data) {
    LockGuard lock;
    auto* r = GdiGetT<RegionObj>(hrgn, abi::kObjRegion);
    if (!r) return 0;
    const DWORD size = static_cast<DWORD>(r->rgn.rects.size() * sizeof(RECT));
    const DWORD total = static_cast<DWORD>(sizeof(RGNDATAHEADER)) + size;
    if (!data) return total;
    if (count < total) return 0;
    data->rdh.dwSize = sizeof(RGNDATAHEADER);
    data->rdh.iType = RDH_RECTANGLES;
    data->rdh.nCount = static_cast<DWORD>(r->rgn.rects.size());
    data->rdh.nRgnSize = size;
    data->rdh.rcBound = r->rgn.Box();
    if (size) std::memcpy(data->Buffer, r->rgn.rects.data(), size);
    return total;
}

// ---- Palettes (32 bpp display: palettes only matter for DIB colour tables) -----------------

HPALETTE WINAPI NtGdiCreatePaletteInternal(const LOGPALETTE* palette, UINT count) {
    LockGuard lock;
    if (!palette) return nullptr;
    auto* pal = new PaletteObj;
    pal->entries.assign(palette->palPalEntry, palette->palPalEntry + std::min<UINT>(count, palette->palNumEntries));
    return static_cast<HPALETTE>(GdiAlloc(pal, abi::kObjPal));
}

LONG WINAPI NtGdiDoPalette(HGDIOBJ handle, WORD start, WORD count, void* entries, DWORD func, BOOL inbound) {
    LockGuard lock;
    (void)inbound;
    enum { Animate, SetEntries, GetEntries, GetSystemEntries, GetDIBColorTable, SetDIBColorTable };
    switch (func) {
    case SetEntries:
    case GetEntries:
    case Animate: {
        auto* pal = GdiGetT<PaletteObj>(handle, abi::kObjPal);
        if (!pal) return 0;
        if (func == GetEntries && !entries) return static_cast<LONG>(pal->entries.size());
        if (start >= pal->entries.size()) return 0;
        const WORD n = static_cast<WORD>(std::min<std::size_t>(count, pal->entries.size() - start));
        auto* pe = static_cast<PALETTEENTRY*>(entries);
        for (WORD i = 0; i < n; ++i) {
            if (func == GetEntries) pe[i] = pal->entries[start + i];
            else pal->entries[start + i] = pe[i];
        }
        return n;
    }
    case GetSystemEntries: {
        State& s = S();
        auto* pal = GdiGetT<PaletteObj>(s.stock[DEFAULT_PALETTE], abi::kObjPal);
        if (!entries) return 256;
        auto* pe = static_cast<PALETTEENTRY*>(entries);
        for (WORD i = 0; i < count; ++i) {
            const unsigned idx = start + i;
            pe[i] = idx < 10 ? pal->entries[idx] : idx >= 246 ? pal->entries[idx - 236] : PALETTEENTRY{};
        }
        return count;
    }
    case GetDIBColorTable:
    case SetDIBColorTable: {
        DcObj* dc = DcGet(static_cast<HDC>(handle));
        auto* bmp = dc ? GdiGetT<BitmapObj>(dc->bitmap, abi::kObjBitmap) : nullptr;
        if (!bmp || !bmp->dib || bmp->dib_colors.empty() || start >= bmp->dib_colors.size()) return 0;
        const WORD n = static_cast<WORD>(std::min<std::size_t>(count, bmp->dib_colors.size() - start));
        auto* q = static_cast<RGBQUAD*>(entries);
        for (WORD i = 0; i < n; ++i) {
            if (func == GetDIBColorTable) q[i] = bmp->dib_colors[start + i];
            else bmp->dib_colors[start + i] = q[i];
        }
        return n;
    }
    }
    return 0;
}

UINT WINAPI NtGdiGetNearestPaletteIndex(HPALETTE hpalette, COLORREF color) {
    LockGuard lock;
    auto* pal = GdiGetT<PaletteObj>(hpalette, abi::kObjPal);
    if (!pal || pal->entries.empty()) return CLR_INVALID;
    UINT best = 0;
    int best_d = 1 << 30;
    for (UINT i = 0; i < pal->entries.size(); ++i) {
        const PALETTEENTRY& e = pal->entries[i];
        const int dr = e.peRed - GetRValue(color), dg = e.peGreen - GetGValue(color), db = e.peBlue - GetBValue(color);
        const int d = dr * dr + dg * dg + db * db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

COLORREF WINAPI NtGdiGetNearestColor(HDC hdc, COLORREF color) {
    LockGuard lock;
    if (!DcGet(hdc)) return CLR_INVALID;
    return color & 0xffffff;   // 32 bpp: every colour is exact
}

HPALETTE WINAPI NtUserSelectPalette(HDC hdc, HPALETTE palette, WORD force_background) {
    LockGuard lock;
    (void)force_background;
    DcObj* dc = DcGet(hdc);
    if (!dc || !GdiGet(palette, abi::kObjPal)) return nullptr;
    HGDIOBJ old = dc->palette;
    dc->palette = palette;
    return static_cast<HPALETTE>(old);
}

UINT WINAPI NtUserRealizePalette(HDC hdc) {
    LockGuard lock;
    return DcGet(hdc) ? 0 : GDI_ERROR;   // nothing to realize on a true-colour display
}

UINT WINAPI NtGdiGetSystemPaletteUse(HDC hdc) {
    (void)hdc;
    return SYSPAL_STATIC;
}

}  // namespace sys
}  // namespace win10xbox::user
