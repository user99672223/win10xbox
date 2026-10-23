// core/user: the parts of the win32k emulation whose encoding Wine's PE
// user32/gdi32 depend on bit for bit -- the GDI handle table (gdi32 decodes
// every HGDIOBJ from PEB.GdiSharedHandleTable itself, and GetStockObject is
// a lookup at FIRST_GDI_HANDLE + n), HWND encoding, regions and DIB reading.
// Case names start with "user_" so `test_core user_` runs just these.
#include "check.h"
#include "../../core/user/user_internal.h"
#include "../../core/user/user_syscalls.h"
#include "../../core/user/font8x16.h"
#include <windows.h>
#include <cstring>
#include <vector>

namespace ud = win10xbox::user::detail;
namespace abi = win10xbox::user::abi;

namespace {
std::uint32_t H32(HGDIOBJ h) { return static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(h)); }
}  // namespace

TEST(user_gdi_handle_encoding) {
    std::vector<abi::GdiHandleEntry> entries(abi::kGdiMaxHandles);
    ud::GdiHandleTable table(entries.data());
    auto* brush = new ud::BrushObj;
    HGDIOBJ h = table.Alloc(brush, abi::kObjBrush);
    // first handle: index FIRST_GDI_HANDLE, Unique = ext type 0x10 | generation 1 << 8
    CHECK_EQ(H32(h), 0x01100020u);
    CHECK_EQ(entries[32].Type, static_cast<UCHAR>(0x10));
    CHECK_EQ(entries[32].Unique, static_cast<USHORT>(0x0110));
    CHECK_EQ(H32(h) & abi::kHandleTypeMask, abi::kObjBrush);   // gdi32's gdi_handle_type()
    CHECK(table.Lookup(h) == brush);
    CHECK(table.Lookup(h, abi::kObjBrush) == brush);
    CHECK(table.Lookup(h, abi::kObjPen) == nullptr);
    // a LOWORD-only handle resolves (gdi32's handle_entry accepts HIWORD 0)
    CHECK(table.Lookup(reinterpret_cast<HGDIOBJ>(static_cast<ULONG_PTR>(32))) == brush);
    CHECK(table.FullHandle(reinterpret_cast<HGDIOBJ>(static_cast<ULONG_PTR>(32))) == h);
    // a wrong generation does not
    CHECK(table.Lookup(reinterpret_cast<HGDIOBJ>(static_cast<ULONG_PTR>(0x02100020))) == nullptr);
    // free and reuse: the slot comes back with the next generation
    CHECK(table.Free(h) == brush);
    CHECK_EQ(entries[32].Type, static_cast<UCHAR>(0));
    CHECK(table.Lookup(h) == nullptr);
    auto* pen = new ud::PenObj;
    HGDIOBJ h2 = table.Alloc(pen, abi::kObjPen);
    CHECK_EQ(H32(h2), 0x02300020u);
    CHECK_EQ(entries[32].Type, static_cast<UCHAR>(0x30 & 0x1f));   // pens share the brush base type
    CHECK(table.Lookup(h2, abi::kObjPen) == pen);
    // stock flag: bit 23 of the handle
    table.MarkStock(h2);
    HGDIOBJ s = table.FullHandle(reinterpret_cast<HGDIOBJ>(static_cast<ULONG_PTR>(32)));
    CHECK_EQ(H32(s), 0x02B00020u);
    CHECK((H32(s) & abi::kHandleStockObject) != 0);
    CHECK(pen->system);
    // DC handles carry the DC_ATTR in UserPointer
    auto* dc = new ud::DcObj;
    abi::DcAttr attr{};
    HGDIOBJ hdc = table.Alloc(dc, abi::kObjMemDc, &attr);
    const std::uint32_t idx = H32(hdc) & 0xffff;
    CHECK_EQ(entries[idx].UserPointer, reinterpret_cast<UINT64>(&attr));
    CHECK(table.Lookup(hdc, abi::kObjDc) == dc);   // memory DCs are DCs (gdi32: type & 0x1f0000)
    CHECK_EQ(table.Live(), 2u);
    delete brush;
    delete table.Free(h2);
    delete table.Free(hdc);
}

TEST(user_gdi_generation_wraps_below_bit31) {
    std::vector<abi::GdiHandleEntry> entries(abi::kGdiMaxHandles);
    ud::GdiHandleTable table(entries.data());
    HGDIOBJ last = nullptr;
    for (int i = 0; i < 0x90; ++i) {
        auto* b = new ud::BrushObj;
        last = table.Alloc(b, abi::kObjBrush);
        CHECK((H32(last) & 0x80000000u) == 0);
        delete table.Free(last);
    }
    CHECK_EQ(H32(last) & 0xffff, 32u);
}

TEST(user_stock_objects_in_wine_order) {
    // The real table, installed at PEB.GdiSharedHandleTable; the test process's own value is put back.
    using win10xbox::dispatch::detail::PebField;
    namespace peb = win10xbox::dispatch::detail::peb;
    void* const saved = PebField<void*>(peb::GdiSharedHandleTable);
    ud::InstallProcessTables();
    auto* entries = static_cast<abi::GdiHandleEntry*>(PebField<void*>(peb::GdiSharedHandleTable));
    CHECK(entries != nullptr);
    if (entries) {
        const struct { int stock; DWORD type; } kOrder[] = {
            {WHITE_BRUSH, abi::kObjBrush}, {LTGRAY_BRUSH, abi::kObjBrush}, {GRAY_BRUSH, abi::kObjBrush},
            {DKGRAY_BRUSH, abi::kObjBrush}, {BLACK_BRUSH, abi::kObjBrush}, {NULL_BRUSH, abi::kObjBrush},
            {WHITE_PEN, abi::kObjPen}, {BLACK_PEN, abi::kObjPen}, {NULL_PEN, abi::kObjPen}, {9, abi::kObjFont},
            {OEM_FIXED_FONT, abi::kObjFont}, {ANSI_FIXED_FONT, abi::kObjFont}, {ANSI_VAR_FONT, abi::kObjFont},
            {SYSTEM_FONT, abi::kObjFont}, {DEVICE_DEFAULT_FONT, abi::kObjFont}, {DEFAULT_PALETTE, abi::kObjPal},
            {SYSTEM_FIXED_FONT, abi::kObjFont}, {DEFAULT_GUI_FONT, abi::kObjFont}, {DC_BRUSH, abi::kObjBrush},
            {DC_PEN, abi::kObjPen}, {STOCK_LAST + 1, abi::kObjBitmap}, {STOCK_LAST + 2, abi::kObjFont},
            {STOCK_LAST + 3, abi::kObjFont}, {STOCK_LAST + 4, abi::kObjFont},
        };
        for (const auto& o : kOrder) {
            const abi::GdiHandleEntry& e = entries[abi::kFirstGdiHandle + o.stock];
            CHECK_MSG(static_cast<DWORD>(abi::UniqueExtType(e.Unique)) << 16 == o.type, "stock object %d has type %#x",
                      o.stock, abi::UniqueExtType(e.Unique));
            CHECK_MSG((e.Unique & 0x80) != 0, "stock object %d lacks the stock flag", o.stock);
        }
        // the first non-stock handle comes after them
        CHECK_EQ(entries[abi::kFirstGdiHandle + STOCK_LAST + 5].Type != 0, true);   // system colour brushes
    }
    PebField<void*>(peb::GdiSharedHandleTable) = saved;
}

TEST(user_hwnd_encoding) {
    HWND h = ud::EncodeHwnd(0, 1);
    CHECK_EQ(static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(h)), 0x00010020u);
    std::uint32_t slot = 99;
    WORD gen = 0;
    CHECK(ud::DecodeHwnd(h, &slot, &gen));
    CHECK_EQ(slot, 0u);
    CHECK_EQ(gen, static_cast<WORD>(1));
    // never 0, never one of the magic values, fits 32 bits with bit 31 clear
    HWND big = ud::EncodeHwnd(0xffcf, 0x7fff);
    const auto v = reinterpret_cast<ULONG_PTR>(big);
    CHECK(v != 0 && v <= 0x7fffffffu);
    CHECK(big != HWND_BROADCAST && big != HWND_MESSAGE && big != HWND_TOPMOST && big != HWND_NOTOPMOST);
    CHECK(!ud::DecodeHwnd(nullptr, &slot, &gen));
    CHECK(!ud::DecodeHwnd(HWND_BROADCAST, &slot, &gen));
    CHECK(!ud::DecodeHwnd(reinterpret_cast<HWND>(static_cast<ULONG_PTR>(1)), &slot, &gen));   // HWND_BOTTOM
}

TEST(user_region_combine) {
    ud::Rgn a, b;
    a.Set(RECT{0, 0, 10, 10});
    b.Set(RECT{5, 5, 15, 15});
    const ud::Rgn u = ud::CombineRgns(a, b, RGN_OR);
    CHECK(u.Contains(0, 0) && u.Contains(14, 14) && u.Contains(9, 4) && !u.Contains(12, 2));
    const RECT box = u.Box();
    CHECK(box.left == 0 && box.top == 0 && box.right == 15 && box.bottom == 15);
    const ud::Rgn i = ud::CombineRgns(a, b, RGN_AND);
    CHECK_EQ(i.rects.size(), static_cast<std::size_t>(1));
    CHECK(i.rects[0].left == 5 && i.rects[0].top == 5 && i.rects[0].right == 10 && i.rects[0].bottom == 10);
    const ud::Rgn d = ud::CombineRgns(a, b, RGN_DIFF);
    CHECK(d.Contains(0, 0) && !d.Contains(5, 5) && d.Contains(9, 4) && d.Contains(4, 9));
    const ud::Rgn x = ud::CombineRgns(a, b, RGN_XOR);
    CHECK(!x.Contains(6, 6) && x.Contains(1, 1) && x.Contains(12, 12));
    // the rectangles of a result never overlap
    int area = 0;
    for (const RECT& r : u.rects) area += (r.right - r.left) * (r.bottom - r.top);
    CHECK_EQ(area, 100 + 100 - 25);
}

TEST(user_dib_view_pixels) {
    // a 2x2 bottom-up 24 bpp DIB: rows are stored bottom first, padded to 4 bytes
    struct { BITMAPINFOHEADER h; } bmi{};
    bmi.h.biSize = sizeof(BITMAPINFOHEADER);
    bmi.h.biWidth = 2;
    bmi.h.biHeight = 2;
    bmi.h.biPlanes = 1;
    bmi.h.biBitCount = 24;
    bmi.h.biCompression = BI_RGB;
    const std::uint8_t bits[] = {
        0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0, 0,   // bottom row: red, green
        0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0, 0,   // top row: blue, white
    };
    ud::DibView v;
    CHECK(ud::MakeDibView(reinterpret_cast<const BITMAPINFO*>(&bmi), DIB_RGB_COLORS, bits, &v));
    CHECK_EQ(ud::DibViewPixel(v, 0, 0), 0xff0000ffu);   // top-left: blue (BGRA as 0xAARRGGBB)
    CHECK_EQ(ud::DibViewPixel(v, 1, 0), 0xffffffffu);
    CHECK_EQ(ud::DibViewPixel(v, 0, 1), 0xffff0000u);   // bottom-left: red
    CHECK_EQ(ud::DibViewPixel(v, 1, 1), 0xff00ff00u);
    CHECK_EQ(ud::ColorToPixel(RGB(0, 80, 220)), 0xff0050dcu);
    CHECK_EQ(ud::PixelToColor(0xff0050dcu), static_cast<COLORREF>(RGB(0, 80, 220)));
}

TEST(user_abi_layouts) {
    CHECK_EQ(sizeof(abi::WinProcParams), static_cast<std::size_t>(72));
    CHECK_EQ(sizeof(abi::DcAttr), static_cast<std::size_t>(192));
    CHECK_EQ(sizeof(abi::GdiHandleEntry), static_cast<std::size_t>(24));
    CHECK_EQ(sizeof(abi::ThreadInfo), static_cast<std::size_t>(64));
    CHECK(sizeof(abi::ThreadInfo) <= 8 * (win10xbox::dispatch::detail::teb::Win32ClientInfoCount - 1));
}

// The plain US layout behind VkKeyScan / MapVirtualKey / ToUnicode / TranslateMessage: no guest, no module state.
TEST(user_us_layout_is_consistent) {
    namespace sys = win10xbox::user::sys;
    const HKL us = nullptr;   // the layout argument is ignored: there is only one
    for (WCHAR c = 0x20; c < 0x7f; ++c) {
        const WORD vks = sys::NtUserVkKeyScanEx(c, us);
        CHECK(vks != 0xffff);
        const BYTE vk = LOBYTE(vks);
        const UINT scan = sys::NtUserMapVirtualKeyEx(vk, MAPVK_VK_TO_VSC, us);
        CHECK(scan != 0);
        BYTE state[256] = {};
        if (HIBYTE(vks) & 1) state[VK_SHIFT] = 0x80;
        WCHAR out[4] = {};
        CHECK_EQ(sys::NtUserToUnicodeEx(vk, scan, state, out, 4, 0, us), 1);
        CHECK_EQ(out[0], c);
        CHECK_EQ(sys::NtUserMapVirtualKeyEx(scan, MAPVK_VSC_TO_VK, us), static_cast<UINT>(vk));
    }
    CHECK_EQ(sys::NtUserVkKeyScanEx(L'a', us), static_cast<WORD>('A'));
    CHECK_EQ(sys::NtUserVkKeyScanEx(L'A', us), static_cast<WORD>(0x100 | 'A'));
    CHECK_EQ(sys::NtUserVkKeyScanEx(0x00e9, us), static_cast<WORD>(0xffff));   // no such key on a US keyboard
    CHECK_EQ(sys::NtUserMapVirtualKeyEx('A', MAPVK_VK_TO_VSC, us), 0x1eu);
    CHECK_EQ(sys::NtUserMapVirtualKeyEx(VK_LSHIFT, MAPVK_VK_TO_VSC, us), 0x2au);
    CHECK_EQ(sys::NtUserMapVirtualKeyEx(0x2a, MAPVK_VSC_TO_VK, us), static_cast<UINT>(VK_SHIFT));
    CHECK_EQ(sys::NtUserMapVirtualKeyEx(0x2a, MAPVK_VSC_TO_VK_EX, us), static_cast<UINT>(VK_LSHIFT));
    CHECK_EQ(sys::NtUserMapVirtualKeyEx(0x1d, MAPVK_VSC_TO_VK_EX, us), static_cast<UINT>(VK_LCONTROL));
    CHECK_EQ(sys::NtUserMapVirtualKeyEx('Q', MAPVK_VK_TO_CHAR, us), static_cast<UINT>('Q'));
    // caps lock flips letters only; shift and caps together cancel; control makes control characters
    BYTE state[256] = {};
    WCHAR out[4] = {};
    state[VK_CAPITAL] = 1;
    CHECK_EQ(sys::NtUserToUnicodeEx('A', 0x1e, state, out, 4, 0, us), 1);
    CHECK_EQ(out[0], L'A');
    CHECK_EQ(sys::NtUserToUnicodeEx('1', 0x02, state, out, 4, 0, us), 1);
    CHECK_EQ(out[0], L'1');
    state[VK_SHIFT] = 0x80;
    CHECK_EQ(sys::NtUserToUnicodeEx('A', 0x1e, state, out, 4, 0, us), 1);
    CHECK_EQ(out[0], L'a');
    state[VK_CAPITAL] = 0;
    state[VK_SHIFT] = 0;
    state[VK_CONTROL] = 0x80;
    CHECK_EQ(sys::NtUserToUnicodeEx('A', 0x1e, state, out, 4, 0, us), 1);
    CHECK_EQ(out[0], static_cast<WCHAR>(1));
    CHECK_EQ(sys::NtUserToUnicodeEx('A', 0x1e | 0x8000, state, out, 4, 0, us), 0);   // a key up produces nothing
}

// ---- Text: the 8x16 glyph table and ExtTextOut on a memory DC ----------------------------------

TEST(user_font8x16_contract) {
    namespace f = win10xbox::user::font8x16;
    CHECK_EQ(f::kCellWidth, 8);
    CHECK_EQ(f::kCellHeight, 16);
    CHECK_EQ(f::kAscent, 12);
    CHECK_EQ(f::kDescent, 4);
    const std::uint8_t* h = f::GlyphRows(U'H');
    CHECK(f::GlyphPixel(h, 1, 3));
    CHECK(f::GlyphPixel(h, 6, 3));
    CHECK(!f::GlyphPixel(h, 3, 3));
    CHECK(f::GlyphPixel(f::GlyphRows(U'I'), 3, 3));
    // nothing is drawn below the baseline by capitals, and every printable glyph has pixels
    for (char32_t c = U'A'; c <= U'Z'; ++c)
        if (c != U'Q')   // Q has a tail
            for (int y = 12; y < 16; ++y) CHECK_EQ(f::GlyphRows(c)[y], 0);
    for (char32_t c = 0x21; c <= 0x7e; ++c) {
        int set = 0;
        for (int y = 0; y < 16; ++y) set += f::GlyphRows(c)[y] != 0;
        CHECK_MSG(set > 0, "glyph %#x is blank", static_cast<unsigned>(c));
    }
    for (int y = 0; y < 16; ++y) CHECK_EQ(f::GlyphRows(U' ')[y], 0);
    // everything else is the box glyph
    CHECK(f::GlyphRows(0x7) == f::kBox);
    CHECK(f::GlyphRows(0x7f) == f::kBox);
    CHECK(f::GlyphRows(0x4e2d) == f::kBox);
    CHECK(f::GlyphRows(0x1f600) == f::kBox);
    CHECK(f::GlyphPixel(f::kBox, 1, 3));
}

namespace {
// A 64x48 32 bpp memory DC on the real GDI table; the test process's PEB value is put back by the owner.
struct TextDc {
    using PebSaved = void*;
    PebSaved saved;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    const std::uint32_t blank = 0xff123456u;   // the bitmap starts out as this colour
    explicit TextDc(int w = 64, int h = 48) {
        namespace peb = win10xbox::dispatch::detail::peb;
        saved = win10xbox::dispatch::detail::PebField<void*>(peb::GdiSharedHandleTable);
        ud::InstallProcessTables();
        dc = win10xbox::user::sys::NtGdiCreateCompatibleDC(nullptr);
        bmp = win10xbox::user::sys::NtGdiCreateBitmap(w, h, 1, 32, nullptr);
        win10xbox::user::sys::NtGdiSelectBitmap(dc, bmp);
        auto* b = ud::GdiGetT<ud::BitmapObj>(bmp, abi::kObjBitmap);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) ud::SetBitmapPixel(*b, x, y, blank);
    }
    ~TextDc() {
        namespace peb = win10xbox::dispatch::detail::peb;
        win10xbox::user::sys::NtGdiDeleteObjectApp(dc);
        win10xbox::user::sys::NtGdiDeleteObjectApp(bmp);
        win10xbox::dispatch::detail::PebField<void*>(peb::GdiSharedHandleTable) = saved;
    }
    abi::DcAttr* Attr() const { return ud::DcGet(dc)->attr; }
    std::uint32_t Px(int x, int y) const { return ud::BitmapPixel(*ud::GdiGetT<ud::BitmapObj>(bmp, abi::kObjBitmap), x, y); }
    BOOL Out(int x, int y, UINT flags, const RECT* r, const wchar_t* s, const INT* dx = nullptr) const {
        return win10xbox::user::sys::NtGdiExtTextOutW(dc, x, y, flags, r, s, static_cast<UINT>(wcslen(s)), dx, 0);
    }
};
constexpr std::uint32_t kBlack = 0xff000000u, kWhite = 0xffffffffu, kRed = 0xffff0000u, kBlue = 0xff0000ffu;
}  // namespace

TEST(user_ext_text_out_default_placement) {
    TextDc t;
    t.Attr()->text_color = RGB(255, 0, 0);
    t.Attr()->background_color = RGB(0, 0, 255);   // OPAQUE is the default background mode
    CHECK(t.Out(4, 2, 0, nullptr, L"HI"));
    // the first cell's top-left is (4,2): 'H' pixels (1,3) / (6,3) set, (3,3) clear (background)
    CHECK_EQ(t.Px(4 + 1, 2 + 3), kRed);
    CHECK_EQ(t.Px(4 + 6, 2 + 3), kRed);
    CHECK_EQ(t.Px(4 + 3, 2 + 3), kBlue);
    CHECK_EQ(t.Px(4 + 8 + 3, 2 + 3), kRed);        // 'I' starts one cell (8 px) further
    CHECK_EQ(t.Px(4, 2), kBlue);                   // background covers the whole 16 x 16 text box
    CHECK_EQ(t.Px(4 + 15, 2 + 15), kBlue);
    CHECK_EQ(t.Px(3, 2), t.blank);                      // nothing outside it
    CHECK_EQ(t.Px(4 + 16, 2), t.blank);
    CHECK_EQ(t.Px(4, 2 + 16), t.blank);
    CHECK_EQ(t.Px(4 + 3, 2 + 1) >> 24, 0xffu);
}

TEST(user_ext_text_out_transparent_and_opaque_rect) {
    TextDc t;
    t.Attr()->background_color = RGB(255, 255, 255);
    const RECT r{0, 0, 40, 20};
    CHECK(t.Out(0, 0, ETO_OPAQUE, &r, L""));        // an empty string still fills
    CHECK_EQ(t.Px(0, 0), kWhite);
    CHECK_EQ(t.Px(39, 19), kWhite);
    CHECK_EQ(t.Px(40, 19), t.blank);
    t.Attr()->background_mode = TRANSPARENT;
    t.Attr()->background_color = RGB(255, 0, 0);
    CHECK(t.Out(2, 1, 0, nullptr, L"H"));
    CHECK_EQ(t.Px(2 + 1, 1 + 3), kBlack);           // text colour defaults to black
    CHECK_EQ(t.Px(2 + 3, 1 + 3), kWhite);           // the background was not painted over
    // ETO_OPAQUE with a rectangle and text: the rectangle takes the background colour, text goes over it
    t.Attr()->background_mode = OPAQUE;
    const RECT r2{0, 24, 30, 44};
    CHECK(t.Out(2, 25, ETO_OPAQUE, &r2, L"H"));
    CHECK_EQ(t.Px(29, 43), kRed);
    CHECK_EQ(t.Px(2 + 1, 25 + 3), kBlack);
}

TEST(user_ext_text_out_alignment) {
    TextDc t;
    t.Attr()->background_mode = TRANSPARENT;
    t.Attr()->text_align = TA_RIGHT | TA_BOTTOM;    // (x,y) is the bottom-right of the text box
    CHECK(t.Out(40, 40, 0, nullptr, L"H"));
    CHECK_EQ(t.Px(32 + 1, 24 + 3), kBlack);
    t.Attr()->text_align = TA_CENTER | TA_BASELINE; // centred; y is the baseline (cell top = y - 12)
    CHECK(t.Out(32, 30, 0, nullptr, L"HH"));
    CHECK_EQ(t.Px(24 + 1, 18 + 3), kBlack);         // first H
    CHECK_EQ(t.Px(32 + 6, 18 + 3), kBlack);         // second H
    // the baseline: capitals end on row 11, so the pixel row y-1 is the last inked row of an 'H'
    CHECK_EQ(t.Px(24 + 1, 29), kBlack);
    CHECK_EQ(t.Px(24 + 1, 30), t.blank);
}

TEST(user_ext_text_out_dx_and_clip) {
    TextDc t;
    t.Attr()->background_mode = TRANSPARENT;
    const INT dx[] = {12, 8};
    CHECK(t.Out(0, 0, 0, nullptr, L"HH", dx));
    CHECK_EQ(t.Px(1, 3), kBlack);
    CHECK_EQ(t.Px(12 + 1, 3), kBlack);              // the second cell starts 12 px in
    CHECK_EQ(t.Px(8 + 1, 3), t.blank);
    // ETO_CLIPPED limits the glyphs to the rectangle
    const RECT clip{0, 20, 4, 36};
    CHECK(t.Out(0, 20, ETO_CLIPPED, &clip, L"H"));
    CHECK_EQ(t.Px(1, 23), kBlack);                  // inside
    CHECK_EQ(t.Px(6, 23), t.blank);                      // outside (the glyph has a pixel there)
    // a negative origin and a window past the bitmap clip silently
    CHECK(t.Out(-5, -3, 0, nullptr, L"HI"));
    CHECK(t.Out(60, 44, 0, nullptr, L"HI"));
}

TEST(user_ext_text_out_current_position) {
    TextDc t;
    t.Attr()->text_align = TA_UPDATECP;
    t.Attr()->background_mode = TRANSPARENT;
    t.Attr()->cur_pos = POINT{2, 4};
    CHECK(t.Out(50, 50, 0, nullptr, L"HI"));        // x,y ignored: the current position is the origin
    CHECK_EQ(t.Px(2 + 1, 4 + 3), kBlack);
    CHECK_EQ(t.Attr()->cur_pos.x, 18);
    CHECK_EQ(t.Attr()->cur_pos.y, 4);
}

TEST(user_text_metrics_and_extents) {
    using namespace win10xbox::user::sys;
    TextDc t;
    TEXTMETRICW tm{};
    CHECK(NtGdiGetTextMetricsW(t.dc, &tm, 0));
    CHECK_EQ(tm.tmHeight, 16);
    CHECK_EQ(tm.tmAscent, 12);
    CHECK_EQ(tm.tmDescent, 4);
    CHECK_EQ(tm.tmAveCharWidth, 8);
    CHECK_EQ(tm.tmMaxCharWidth, 8);
    CHECK((tm.tmPitchAndFamily & TMPF_FIXED_PITCH) == 0);
    SIZE sz{};
    INT dxs[4] = {}, nfit = -1;
    CHECK(NtGdiGetTextExtentExW(t.dc, L"abc", 3, 20, &nfit, dxs, &sz, 0));
    CHECK_EQ(sz.cx, 24);
    CHECK_EQ(sz.cy, 16);
    CHECK_EQ(nfit, 2);
    CHECK_EQ(dxs[0], 8);
    CHECK_EQ(dxs[1], 16);
    CHECK_EQ(dxs[2], 24);
    INT widths[3] = {};
    CHECK(NtGdiGetCharWidthW(t.dc, 'a', 'c', nullptr, 0x02, widths));
    CHECK_EQ(widths[0], 8);
    CHECK_EQ(widths[2], 8);
    abi::CharWidthInfo info{1, 1, 1};
    CHECK(NtGdiGetCharWidthInfo(t.dc, &info));
    CHECK_EQ(info.lsb, 0);
    // every stock font reports the same cell
    ud::State& s = ud::S();
    for (int stock : {OEM_FIXED_FONT, ANSI_FIXED_FONT, ANSI_VAR_FONT, SYSTEM_FONT, SYSTEM_FIXED_FONT, DEFAULT_GUI_FONT}) {
        NtGdiSelectFont(t.dc, s.stock[stock]);
        TEXTMETRICW m{};
        CHECK(NtGdiGetTextMetricsW(t.dc, &m, 0));
        CHECK_MSG(m.tmHeight == 16 && m.tmAveCharWidth == 8, "stock font %d: %ldx%ld", stock, m.tmAveCharWidth, m.tmHeight);
    }
    // tall LOGFONTs scale by whole factors, small ones stay at 1
    LOGFONTW lf{};
    lf.lfHeight = -48;
    HFONT big = NtGdiHfontCreate(&lf, sizeof lf, 0, 0, nullptr);
    CHECK(big != nullptr);
    NtGdiSelectFont(t.dc, big);
    CHECK(NtGdiGetTextMetricsW(t.dc, &tm, 0));
    CHECK_EQ(tm.tmHeight, 48);
    CHECK_EQ(tm.tmAscent, 36);
    CHECK_EQ(tm.tmAveCharWidth, 24);
    t.Attr()->background_mode = TRANSPARENT;
    CHECK(t.Out(0, 0, 0, nullptr, L"H"));           // each glyph pixel becomes a 3x3 block
    CHECK_EQ(t.Px(1 * 3, 3 * 3), kBlack);
    CHECK_EQ(t.Px(1 * 3 + 2, 3 * 3 + 2), kBlack);
    CHECK_EQ(t.Px(3 * 3, 3 * 3), t.blank);
    lf.lfHeight = -12;
    HFONT small = NtGdiHfontCreate(&lf, sizeof lf, 0, 0, nullptr);
    NtGdiSelectFont(t.dc, small);
    CHECK(NtGdiGetTextMetricsW(t.dc, &tm, 0));
    CHECK_EQ(tm.tmHeight, 16);
    NtGdiSelectFont(t.dc, s.stock[SYSTEM_FONT]);
    NtGdiDeleteObjectApp(big);
    NtGdiDeleteObjectApp(small);
}

// ---- cursors and icons (win_cursoricon.cpp) ---------------------------------------------------------

TEST(user_cursor_icon_lifecycle) {
    namespace sys = win10xbox::user::sys;
    TextDc t;
    auto make_frame = [&](std::uint32_t colour, abi::CursorIconFrame* fr) {
        fr->width = 4;
        fr->height = 4;
        fr->color = sys::NtGdiCreateBitmap(4, 4, 1, 32, nullptr);
        fr->alpha = nullptr;
        fr->mask = sys::NtGdiCreateBitmap(4, 4, 1, 1, nullptr);   // starts black: every pixel is drawn
        fr->hotspot = POINT{1, 2};
        auto* c = ud::GdiGetT<ud::BitmapObj>(fr->color, abi::kObjBitmap);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) ud::SetBitmapPixel(*c, x, y, colour);
    };
    HICON icon = ud::CreateCursorIconHandle(true);
    CHECK(icon != nullptr);
    CHECK_EQ(H32(icon) & 0xFFC00000u, 0x00C00000u);
    abi::CursorIconFrame fr{};
    make_frame(kRed, &fr);
    abi::CursorIconDesc desc{};
    desc.frames = &fr;
    CHECK(sys::NtUserSetCursorIconData(icon, nullptr, nullptr, &desc));
    CHECK(!sys::NtUserSetCursorIconData(icon, nullptr, nullptr, &desc));   // already initialised
    LONG w = 0, h = 0;
    CHECK(sys::NtUserGetIconSize(icon, 0, &w, &h));
    CHECK_EQ(w, 4);
    CHECK_EQ(h, 8);   // win32u reports twice the frame height
    ICONINFO info{};
    CHECK(sys::NtUserGetIconInfo(icon, &info, nullptr, nullptr, nullptr, 0));
    CHECK(info.fIcon);
    CHECK_EQ(info.xHotspot, 1u);
    CHECK_EQ(info.yHotspot, 2u);
    CHECK(info.hbmColor != fr.color);   // a copy the caller owns
    CHECK_EQ(ud::BitmapPixel(*ud::GdiGetT<ud::BitmapObj>(info.hbmColor, abi::kObjBitmap), 2, 2), kRed);
    sys::NtGdiDeleteObjectApp(info.hbmColor);
    sys::NtGdiDeleteObjectApp(info.hbmMask);
    // DI_NORMAL: an all-black mask (SRCAND) then the colour (SRCINVERT) leaves the colour
    CHECK(sys::NtUserDrawIconEx(t.dc, 10, 10, icon, 0, 0, 0, nullptr, DI_NORMAL));
    CHECK_EQ(t.Px(10, 10), kRed);
    CHECK_EQ(t.Px(13, 13), kRed);
    CHECK_EQ(t.Px(14, 14), t.blank);
    CHECK_EQ(t.Px(9, 10), t.blank);
    // stretched to 8x8
    CHECK(sys::NtUserDrawIconEx(t.dc, 20, 20, icon, 8, 8, 0, nullptr, DI_NORMAL));
    CHECK_EQ(t.Px(27, 27), kRed);
    CHECK_EQ(t.Px(28, 28), t.blank);
    // destroyed: the handle is gone and its bitmaps with it
    HBITMAP color = fr.color;
    CHECK(sys::NtUserDestroyCursor(icon, 0));
    CHECK(!sys::NtUserGetIconSize(icon, 0, &w, &h));
    CHECK(ud::GdiGetT<ud::BitmapObj>(color, abi::kObjBitmap) == nullptr);
    CHECK(!sys::NtUserDestroyCursor(icon, 0));
}

TEST(user_cursor_icon_shared_cache_and_animation) {
    namespace sys = win10xbox::user::sys;
    TextDc t;
    // a shared icon loaded from a module is found again by (module, resource handle) and is not destroyed
    HICON shared = ud::CreateCursorIconHandle(false);
    abi::CursorIconFrame fr{4, 4, sys::NtGdiCreateBitmap(4, 4, 1, 32, nullptr), nullptr, sys::NtGdiCreateBitmap(4, 4, 1, 1, nullptr), POINT{0, 0}};
    abi::CursorIconDesc desc{};
    desc.flags = LR_SHARED;
    desc.frames = &fr;
    int rsrc_marker = 0;
    desc.rsrc = reinterpret_cast<HRSRC>(&rsrc_marker);
    wchar_t mod[] = L"user32.dll";
    UNICODE_STRING module{static_cast<USHORT>(sizeof mod - 2), sizeof mod, mod};
    UNICODE_STRING res{0, 0, reinterpret_cast<PWSTR>(static_cast<ULONG_PTR>(32512))};   // IDC_ARROW
    CHECK(sys::NtUserSetCursorIconData(shared, &module, &res, &desc));
    CHECK(sys::NtUserFindExistingCursorIcon(&module, &res, &rsrc_marker) == shared);
    int other = 0;
    CHECK(sys::NtUserFindExistingCursorIcon(&module, &res, &other) == nullptr);
    wchar_t mod2[] = L"other.dll";
    UNICODE_STRING module2{static_cast<USHORT>(sizeof mod2 - 2), sizeof mod2, mod2};
    CHECK(sys::NtUserFindExistingCursorIcon(&module2, &res, &rsrc_marker) == nullptr);
    CHECK(sys::NtUserDestroyCursor(shared, 0));
    LONG w = 0, h = 0;
    CHECK(sys::NtUserGetIconSize(shared, 0, &w, &h));   // still alive: shared cursors are never freed
    // GetIconInfo reports where it came from
    ICONINFO info{};
    wchar_t buf[32] = {};
    UNICODE_STRING out_mod{0, sizeof buf, buf};
    UNICODE_STRING out_res{0, 0, nullptr};
    CHECK(sys::NtUserGetIconInfo(shared, &info, &out_mod, &out_res, nullptr, 0));
    CHECK(!info.fIcon);
    CHECK_EQ(out_mod.Length, 10);   // in characters
    CHECK_EQ(out_res.Length, 0);
    CHECK_EQ(reinterpret_cast<ULONG_PTR>(out_res.Buffer), static_cast<ULONG_PTR>(32512));
    sys::NtGdiDeleteObjectApp(info.hbmColor);
    sys::NtGdiDeleteObjectApp(info.hbmMask);

    // an animated cursor: two frames, three steps (0, 1, 0)
    HICON ani = ud::CreateCursorIconHandle(false);
    abi::CursorIconFrame frames[2] = {
        {4, 4, sys::NtGdiCreateBitmap(4, 4, 1, 32, nullptr), nullptr, sys::NtGdiCreateBitmap(4, 4, 1, 1, nullptr), POINT{0, 0}},
        {4, 4, sys::NtGdiCreateBitmap(4, 4, 1, 32, nullptr), nullptr, sys::NtGdiCreateBitmap(4, 4, 1, 1, nullptr), POINT{1, 1}}};
    DWORD seq[3] = {0, 1, 0}, rates[3] = {5, 6, 7};
    abi::CursorIconDesc ad{};
    ad.num_steps = 3;
    ad.num_frames = 2;
    ad.delay = 9;
    ad.frames = frames;
    ad.frame_seq = seq;
    ad.frame_rates = rates;
    CHECK(sys::NtUserSetCursorIconData(ani, nullptr, nullptr, &ad));
    DWORD rate = 0, steps = 0;
    HCURSOR f0 = sys::NtUserGetCursorFrameInfo(ani, 0, &rate, &steps);
    CHECK_EQ(rate, 5u);
    HCURSOR f1 = sys::NtUserGetCursorFrameInfo(ani, 1, &rate, &steps);
    CHECK_EQ(rate, 6u);
    HCURSOR f2 = sys::NtUserGetCursorFrameInfo(ani, 2, &rate, &steps);
    CHECK(f0 != nullptr && f1 != nullptr && f0 != f1);
    CHECK(f2 == f0);   // step 2 repeats frame 0 and its object, with frame 0's rate
    CHECK_EQ(rate, 5u);
    CHECK_EQ(steps, 3u);
    CHECK(sys::NtUserGetIconSize(ani, 1, &w, &h));
    CHECK(sys::NtUserDestroyCursor(ani, 0));
    CHECK(!sys::NtUserGetIconSize(ani, 1, &w, &h));
    CHECK(!sys::NtUserGetIconSize(f1, 0, &w, &h));   // the frames went with it
    // the icon param (16-bit thunks only) round-trips
    HICON p = ud::CreateCursorIconHandle(true);
    abi::FreeIconParams fp{{0}, 0x1234};
    CHECK_EQ(ud::SetIconParam(p, &fp), 0u);
    CHECK_EQ(ud::GetIconParam(p), static_cast<ULONG_PTR>(0x1234));
}

TEST(user_query_display_config) {
    namespace sys = win10xbox::user::sys;
    UINT32 np = 0, nm = 0;
    CHECK_EQ(sys::NtUserGetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm), static_cast<LONG>(ERROR_SUCCESS));
    DISPLAYCONFIG_PATH_INFO path{};
    DISPLAYCONFIG_MODE_INFO modes[2]{};
    np = 1;
    nm = 1;   // too small for source + target
    CHECK_EQ(sys::NtUserQueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, &path, &nm, modes, nullptr), static_cast<LONG>(ERROR_INSUFFICIENT_BUFFER));
    nm = 2;
    CHECK_EQ(sys::NtUserQueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, &path, &nm, modes, nullptr), static_cast<LONG>(ERROR_SUCCESS));
    CHECK_EQ(np, 1u);
    CHECK_EQ(nm, 2u);
    CHECK(path.flags & DISPLAYCONFIG_PATH_ACTIVE);
    CHECK_EQ(modes[0].infoType, DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE);
    CHECK(modes[0].sourceMode.width > 0 && modes[0].sourceMode.height > 0);
    CHECK_EQ(modes[1].infoType, DISPLAYCONFIG_MODE_INFO_TYPE_TARGET);
    CHECK_EQ(modes[1].targetMode.targetVideoSignalInfo.activeSize.cx, modes[0].sourceMode.width);
    // the names of the path's source and target
    DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
    src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    src.header.size = sizeof src;
    src.header.adapterId = path.sourceInfo.adapterId;
    src.header.id = path.sourceInfo.id;
    CHECK_EQ(sys::NtUserDisplayConfigGetDeviceInfo(&src.header), static_cast<NTSTATUS>(0));
    CHECK(wcscmp(src.viewGdiDeviceName, L"\\\\.\\DISPLAY1") == 0);
    DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
    tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tgt.header.size = sizeof tgt;
    tgt.header.adapterId = path.targetInfo.adapterId;
    tgt.header.id = path.targetInfo.id;
    CHECK_EQ(sys::NtUserDisplayConfigGetDeviceInfo(&tgt.header), static_cast<NTSTATUS>(0));
    CHECK(tgt.monitorFriendlyDeviceName[0] != 0);
    // an unknown adapter is refused
    src.header.adapterId.LowPart = 99;
    CHECK(sys::NtUserDisplayConfigGetDeviceInfo(&src.header) != 0);
}

TEST(user_stretch_dibits_into_monochrome_bitmap) {
    namespace sys = win10xbox::user::sys;
    TextDc t;
    HDC mdc = sys::NtGdiCreateCompatibleDC(nullptr);
    HBITMAP mono = sys::NtGdiCreateBitmap(8, 2, 1, 1, nullptr);
    sys::NtGdiSelectBitmap(mdc, mono);
    // a 1 bpp bottom-up DIB, black and white palette: memory row 0 is the bottom image row
    struct { BITMAPINFOHEADER h; RGBQUAD pal[2]; } bmi{};
    bmi.h.biSize = sizeof bmi.h;
    bmi.h.biWidth = 8;
    bmi.h.biHeight = 2;
    bmi.h.biPlanes = 1;
    bmi.h.biBitCount = 1;
    bmi.h.biClrUsed = 2;
    bmi.pal[0] = RGBQUAD{0, 0, 0, 0};
    bmi.pal[1] = RGBQUAD{255, 255, 255, 0};
    const std::uint8_t bits[8] = {0xF0, 0, 0, 0, 0x0F, 0, 0, 0};   // scan rows padded to 4 bytes
    CHECK_EQ(sys::NtGdiStretchDIBitsInternal(mdc, 0, 0, 8, 2, 0, 0, 8, 2, bits, reinterpret_cast<const BITMAPINFO*>(&bmi),
                                             DIB_RGB_COLORS, SRCCOPY, 0, 0, nullptr),
             2);
    auto* b = ud::GdiGetT<ud::BitmapObj>(mono, abi::kObjBitmap);
    for (int x = 0; x < 8; ++x) {
        CHECK_EQ(ud::BitmapPixel(*b, x, 1), x < 4 ? kWhite : kBlack);   // bottom row = memory row 0
        CHECK_EQ(ud::BitmapPixel(*b, x, 0), x < 4 ? kBlack : kWhite);
    }
    // SetDIBitsToDevice reaches the same destination
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 8; ++x) ud::SetBitmapPixel(*b, x, y, kBlack);
    CHECK_EQ(sys::NtGdiSetDIBitsToDeviceInternal(mdc, 0, 0, 8, 2, 0, 0, 0, 2, bits, reinterpret_cast<const BITMAPINFO*>(&bmi),
                                                 DIB_RGB_COLORS, 0, 0, FALSE, nullptr),
             2);
    CHECK_EQ(ud::BitmapPixel(*b, 0, 1), kWhite);
    CHECK_EQ(ud::BitmapPixel(*b, 7, 0), kWhite);
    CHECK_EQ(ud::BitmapPixel(*b, 7, 1), kBlack);
    sys::NtGdiDeleteObjectApp(mdc);
    sys::NtGdiDeleteObjectApp(mono);
}
