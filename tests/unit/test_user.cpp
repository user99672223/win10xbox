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
