// core/user: the parts of the win32k emulation whose encoding Wine's PE
// user32/gdi32 depend on bit for bit -- the GDI handle table (gdi32 decodes
// every HGDIOBJ from PEB.GdiSharedHandleTable itself, and GetStockObject is
// a lookup at FIRST_GDI_HANDLE + n), HWND encoding, regions and DIB reading.
// Case names start with "user_" so `test_core user_` runs just these.
#include "check.h"
#include "../../core/user/user_internal.h"
#include "../../core/user/user_syscalls.h"
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
