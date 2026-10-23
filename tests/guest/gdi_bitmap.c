/* guest_gdi_bitmap: GDI bitmap operations through gdi32 -> win32u syscalls -> core/user, verified
 * pixel-exact with GetPixel / GetDIBits and, for the window part, by the host's screenshot.
 *   part 1: memory DCs only (BitBlt ROPs, StretchBlt, PatBlt, AlphaBlend / TransparentBlt through msimg32,
 *           GetDIBits, SetDIBitsToDevice, StretchDIBits, CreateDIBSection at 32 / 24 / 8 bpp)
 *   part 2: a 400x300 popup window at (100,100): DIB sections written through their bits pointer are blitted
 *           to the window DC (SRCCOPY, NOTSRCCOPY, SRCINVERT, a 2x StretchBlt), the window DC is read back into
 *           a DIB section, AlphaBlend / TransparentBlt onto the window, and LoadImage of two BITMAP resources
 *           (gdi_bitmap.rc) with LR_CREATEDIBSECTION. The window DC pixels are checked with GetPixel; the
 *           screenshot (ctest) checks the same pixels on the composed screen.
 * Prints one line per check ("bmp: <name> ok|FAIL ...") and "bmp: done"; exits 42 only if every check passed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define IDB_RGB24 101
#define IDB_PAL8  102

static int g_fail;
static void check(const char* name, int ok, unsigned long got, unsigned long want) {
    if (ok) printf("bmp: %s ok\n", name);
    else { printf("bmp: %s FAIL got=%08lx want=%08lx\n", name, got, want); g_fail++; }
    fflush(stdout);
}
#define CHECK_PX(name, dc, x, y, want) do { COLORREF c_ = GetPixel(dc, x, y); check(name, c_ == (COLORREF)(want), c_, (COLORREF)(want)); } while (0)

static HBITMAP make_dib32(HDC dc, int w, int h, DWORD** bits) {
    BITMAPINFO bi;
    void* p = NULL;
    HBITMAP bm;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, NULL, 0);
    *bits = (DWORD*)p;
    return bm;
}

static void fill(HDC dc, int w, int h, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    RECT r = {0, 0, w, h};
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static int part1(void) {
    HDC a = CreateCompatibleDC(NULL), b = CreateCompatibleDC(NULL);
    DWORD *pa = NULL, *pb = NULL;
    HBITMAP ba, bb, oa, ob;
    BITMAPINFO bi;
    int i;
    if (!a || !b) { printf("bmp: setup failed (dc)\n"); return 1; }
    ba = make_dib32(a, 8, 8, &pa);
    bb = make_dib32(b, 8, 8, &pb);
    if (!ba || !bb || !pa || !pb) { printf("bmp: setup failed (dib)\n"); return 1; }
    oa = SelectObject(a, ba);
    ob = SelectObject(b, bb);
    printf("bmp: setup ok\n");

    /* DIB section memory is the bitmap */
    fill(a, 8, 8, RGB(0x10, 0x20, 0x30));
    check("dibsection-bits-bgr", (pa[0] & 0xffffff) == 0x102030, pa[0], 0x102030);

    /* BitBlt SRCCOPY / SRCPAINT / SRCAND / SRCINVERT */
    fill(a, 8, 8, RGB(0xf0, 0x0f, 0x33));
    fill(b, 8, 8, RGB(0x3c, 0x3c, 0x55));
    BitBlt(b, 0, 0, 8, 8, a, 0, 0, SRCCOPY);
    CHECK_PX("srccopy", b, 3, 3, RGB(0xf0, 0x0f, 0x33));
    fill(b, 8, 8, RGB(0x3c, 0x3c, 0x55));
    BitBlt(b, 0, 0, 8, 8, a, 0, 0, SRCPAINT);
    CHECK_PX("srcpaint", b, 1, 1, RGB(0xfc, 0x3f, 0x77));
    fill(b, 8, 8, RGB(0x3c, 0x3c, 0x55));
    BitBlt(b, 0, 0, 8, 8, a, 0, 0, SRCAND);
    CHECK_PX("srcand", b, 1, 1, RGB(0x30, 0x0c, 0x11));
    fill(b, 8, 8, RGB(0x3c, 0x3c, 0x55));
    BitBlt(b, 0, 0, 8, 8, a, 0, 0, SRCINVERT);
    CHECK_PX("srcinvert", b, 1, 1, RGB(0xcc, 0x33, 0x66));
    BitBlt(b, 0, 0, 8, 8, NULL, 0, 0, BLACKNESS);
    CHECK_PX("blackness", b, 2, 2, RGB(0, 0, 0));
    BitBlt(b, 0, 0, 8, 8, NULL, 0, 0, WHITENESS);
    CHECK_PX("whiteness", b, 2, 2, RGB(255, 255, 255));

    /* PatBlt PATCOPY with a solid brush, partial rectangle */
    fill(b, 8, 8, RGB(0, 0, 0));
    {
        HBRUSH br = CreateSolidBrush(RGB(1, 2, 3));
        HGDIOBJ old = SelectObject(b, br);
        PatBlt(b, 2, 2, 3, 3, PATCOPY);
        SelectObject(b, old);
        DeleteObject(br);
    }
    CHECK_PX("patcopy-in", b, 3, 3, RGB(1, 2, 3));
    CHECK_PX("patcopy-out", b, 5, 5, RGB(0, 0, 0));

    /* StretchBlt 2x upscale of a 2x2 pattern */
    fill(a, 8, 8, RGB(0, 0, 0));
    SetPixel(a, 0, 0, RGB(255, 0, 0));
    SetPixel(a, 1, 0, RGB(0, 255, 0));
    SetPixel(a, 0, 1, RGB(0, 0, 255));
    SetPixel(a, 1, 1, RGB(255, 255, 0));
    StretchBlt(b, 0, 0, 4, 4, a, 0, 0, 2, 2, SRCCOPY);
    CHECK_PX("stretch-00", b, 0, 0, RGB(255, 0, 0));
    CHECK_PX("stretch-11", b, 1, 1, RGB(255, 0, 0));
    CHECK_PX("stretch-20", b, 2, 0, RGB(0, 255, 0));
    CHECK_PX("stretch-03", b, 0, 3, RGB(0, 0, 255));
    CHECK_PX("stretch-33", b, 3, 3, RGB(255, 255, 0));

    /* AlphaBlend: constant alpha 255 / 0 / 128 */
    {
        BLENDFUNCTION bf;
        COLORREF c;
        int r, g, bl;
        bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 255; bf.AlphaFormat = 0;
        fill(a, 8, 8, RGB(200, 100, 50));
        fill(b, 8, 8, RGB(100, 100, 100));
        AlphaBlend(b, 0, 0, 8, 8, a, 0, 0, 8, 8, bf);
        CHECK_PX("alpha-255", b, 4, 4, RGB(200, 100, 50));
        bf.SourceConstantAlpha = 0;
        fill(b, 8, 8, RGB(100, 100, 100));
        AlphaBlend(b, 0, 0, 8, 8, a, 0, 0, 8, 8, bf);
        CHECK_PX("alpha-0", b, 4, 4, RGB(100, 100, 100));
        bf.SourceConstantAlpha = 128;
        fill(b, 8, 8, RGB(100, 100, 100));
        AlphaBlend(b, 0, 0, 8, 8, a, 0, 0, 8, 8, bf);
        c = GetPixel(b, 4, 4);
        r = GetRValue(c); g = GetGValue(c); bl = GetBValue(c);
        check("alpha-128", r >= 149 && r <= 151 && g == 100 && bl >= 74 && bl <= 76, c, RGB(150, 100, 75));
        /* per-pixel (premultiplied) source: alpha 255 replaces, alpha 0 leaves the destination */
        bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
        pa[0] = 0xff112233; pa[1] = 0x00000000;
        fill(b, 8, 8, RGB(9, 9, 9));
        AlphaBlend(b, 0, 0, 2, 1, a, 0, 0, 2, 1, bf);
        CHECK_PX("alpha-px-opaque", b, 0, 0, RGB(0x11, 0x22, 0x33));
        CHECK_PX("alpha-px-clear", b, 1, 0, RGB(9, 9, 9));
    }

    /* TransparentBlt: key colour pixels are skipped */
    fill(a, 8, 8, RGB(255, 0, 255));
    SetPixel(a, 2, 2, RGB(1, 2, 3));
    fill(b, 8, 8, RGB(7, 8, 9));
    TransparentBlt(b, 0, 0, 8, 8, a, 0, 0, 8, 8, RGB(255, 0, 255));
    CHECK_PX("transparent-key", b, 0, 0, RGB(7, 8, 9));
    CHECK_PX("transparent-px", b, 2, 2, RGB(1, 2, 3));

    /* GetDIBits (24 bpp, bottom-up) of the DIB section */
    fill(b, 8, 8, RGB(0x11, 0x22, 0x33));
    SetPixel(b, 0, 0, RGB(0xaa, 0xbb, 0xcc));
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 8; bi.bmiHeader.biHeight = 8; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
    {
        static BYTE buf[24 * 8];
        int lines;
        SelectObject(b, ob);   /* GetDIBits wants the bitmap deselected */
        lines = GetDIBits(b, bb, 0, 8, buf, &bi, DIB_RGB_COLORS);
        check("getdibits-lines", lines == 8, lines, 8);
        /* bottom-up: the top-left pixel is in the last memory row */
        check("getdibits-topleft", buf[24 * 7] == 0xcc && buf[24 * 7 + 1] == 0xbb && buf[24 * 7 + 2] == 0xaa,
              buf[24 * 7] | (buf[24 * 7 + 1] << 8) | (buf[24 * 7 + 2] << 16), 0xccbbaa);
        check("getdibits-other", buf[0] == 0x33 && buf[1] == 0x22 && buf[2] == 0x11,
              buf[0] | (buf[1] << 8) | (buf[2] << 16), 0x332211);
        SelectObject(b, bb);
    }

    /* SetDIBitsToDevice / StretchDIBits from an 8 bpp palettized DIB */
    {
        struct { BITMAPINFOHEADER h; RGBQUAD pal[4]; } b8;
        BYTE px8[4 * 4];
        memset(&b8, 0, sizeof b8);
        b8.h.biSize = sizeof b8.h; b8.h.biWidth = 4; b8.h.biHeight = 4; b8.h.biPlanes = 1; b8.h.biBitCount = 8;
        b8.h.biClrUsed = 4;
        b8.pal[0].rgbRed = 255; b8.pal[1].rgbGreen = 255; b8.pal[2].rgbBlue = 255;
        b8.pal[3].rgbRed = 1; b8.pal[3].rgbGreen = 2; b8.pal[3].rgbBlue = 3;
        for (i = 0; i < 16; i++) px8[i] = (BYTE)(i % 4);
        fill(b, 8, 8, RGB(0, 0, 0));
        SetDIBitsToDevice(b, 0, 0, 4, 4, 0, 0, 0, 4, px8, (BITMAPINFO*)&b8, DIB_RGB_COLORS);
        CHECK_PX("setdibits8-0", b, 0, 0, RGB(255, 0, 0));
        CHECK_PX("setdibits8-1", b, 1, 0, RGB(0, 255, 0));
        CHECK_PX("setdibits8-2", b, 2, 3, RGB(0, 0, 255));
        CHECK_PX("setdibits8-3", b, 3, 3, RGB(1, 2, 3));
        fill(b, 8, 8, RGB(0, 0, 0));
        StretchDIBits(b, 0, 0, 8, 8, 0, 0, 4, 4, px8, (BITMAPINFO*)&b8, DIB_RGB_COLORS, SRCCOPY);
        CHECK_PX("stretchdibits-0", b, 1, 1, RGB(255, 0, 0));
        CHECK_PX("stretchdibits-2", b, 4, 7, RGB(0, 0, 255));
    }

    /* 1 bpp source through StretchDIBits */
    {
        struct { BITMAPINFOHEADER h; RGBQUAD pal[2]; } b1;
        BYTE rows[16];
        memset(&b1, 0, sizeof b1);
        memset(rows, 0, sizeof rows);
        b1.h.biSize = sizeof b1.h; b1.h.biWidth = 4; b1.h.biHeight = 4; b1.h.biPlanes = 1; b1.h.biBitCount = 1;
        b1.pal[0].rgbRed = 10; b1.pal[1].rgbGreen = 200;
        rows[0] = 0xa0; rows[4] = 0x50; rows[8] = 0xa0; rows[12] = 0x50;   /* memory row 0 = bottom image row */
        fill(b, 8, 8, RGB(0, 0, 0));
        StretchDIBits(b, 0, 0, 4, 4, 0, 0, 4, 4, rows, (BITMAPINFO*)&b1, DIB_RGB_COLORS, SRCCOPY);
        CHECK_PX("dib1-bottom0", b, 0, 3, RGB(0, 200, 0));
        CHECK_PX("dib1-bottom1", b, 1, 3, RGB(10, 0, 0));
        CHECK_PX("dib1-top0", b, 0, 0, RGB(10, 0, 0));
    }

    SelectObject(a, oa); SelectObject(b, ob);
    DeleteObject(ba); DeleteObject(bb); DeleteDC(a); DeleteDC(b);
    return 0;
}

/* ---- CreateDIBSection at 24 and 8 bpp: the guest writes the bits, GDI reads them (and the other way round) ---- */

static HBITMAP make_dib24(HDC dc, int w, int h, BYTE** bits) {
    BITMAPINFO bi;
    void* p = NULL;
    HBITMAP bm;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;   /* bottom-up */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, NULL, 0);
    *bits = (BYTE*)p;
    return bm;
}

static const RGBQUAD g_pal[4] = {{0, 0, 255, 0}, {0, 255, 0, 0}, {255, 0, 0, 0}, {0, 255, 255, 0}};   /* B,G,R,0: red green blue yellow */
static const COLORREF g_pal_rgb[4] = {RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255), RGB(255, 255, 0)};

/* top-down 8 bpp, width w, height h, 4 palette entries given in the BITMAPINFO */
static HBITMAP make_dib8(HDC dc, int w, int h, BYTE** bits) {
    struct { BITMAPINFOHEADER h; RGBQUAD pal[4]; } bi;
    void* p = NULL;
    HBITMAP bm;
    memset(&bi, 0, sizeof bi);
    bi.h.biSize = sizeof bi.h;
    bi.h.biWidth = w;
    bi.h.biHeight = -h;
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 8;
    bi.h.biCompression = BI_RGB;
    bi.h.biClrUsed = 4;
    memcpy(bi.pal, g_pal, sizeof bi.pal);
    bm = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &p, NULL, 0);
    *bits = (BYTE*)p;
    return bm;
}

/* The patterns the guest writes through the bits pointers. */
static COLORREF pat32(int x, int y) { return RGB(x * 32, y * 32, 0x80); }
static COLORREF pat24(int x, int y) { return RGB(200 - x * 20, 30 + y * 50, x * 40 + y); }
static int pat8(int x, int y) { return (x + y) % 4; }

static void fill_dib32(DWORD* p, int w, int h) {
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            COLORREF c = pat32(x, y);
            p[y * w + x] = ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | GetBValue(c);
        }
}
/* 24 bpp, w x h, bottom-up, rows padded to 4 bytes */
static void fill_dib24(BYTE* p, int w, int h) {
    int stride = (w * 3 + 3) & ~3, x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            COLORREF c = pat24(x, y);
            BYTE* d = p + (h - 1 - y) * stride + x * 3;
            d[0] = GetBValue(c); d[1] = GetGValue(c); d[2] = GetRValue(c);
        }
}
static void fill_dib8(BYTE* p, int w, int h) {   /* top-down, stride = w rounded up to 4 */
    int stride = (w + 3) & ~3, x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) p[y * stride + x] = (BYTE)pat8(x, y);
}

static void part_dibs(void) {
    HDC dc = CreateCompatibleDC(NULL);
    BYTE *b24 = NULL, *b8 = NULL;
    DWORD* b32 = NULL;
    HBITMAP h32 = make_dib32(dc, 8, 8, &b32), h24 = make_dib24(dc, 5, 3, &b24), h8 = make_dib8(dc, 4, 2, &b8);
    DIBSECTION ds;
    HGDIOBJ old;
    int x, y, bad;
    if (!dc || !h32 || !h24 || !h8 || !b32 || !b24 || !b8) {
        printf("bmp: dibs setup FAIL (%p %p %p %p)\n", (void*)dc, (void*)h32, (void*)h24, (void*)h8);
        g_fail++;
        return;
    }
    /* GetObject reports the DIB section: size, depth, and the very bits pointer CreateDIBSection returned */
    memset(&ds, 0, sizeof ds);
    check("dib32-getobject", GetObject(h32, sizeof ds, &ds) == (int)sizeof ds && ds.dsBm.bmWidth == 8 &&
          ds.dsBm.bmHeight == 8 && ds.dsBm.bmBitsPixel == 32 && ds.dsBm.bmBits == (void*)b32, ds.dsBm.bmBitsPixel, 32);
    memset(&ds, 0, sizeof ds);
    check("dib24-getobject", GetObject(h24, sizeof ds, &ds) == (int)sizeof ds && ds.dsBm.bmWidth == 5 &&
          ds.dsBm.bmHeight == 3 && ds.dsBm.bmBitsPixel == 24 && ds.dsBm.bmBits == (void*)b24 &&
          ds.dsBm.bmWidthBytes == 16, ds.dsBm.bmWidthBytes, 16);
    memset(&ds, 0, sizeof ds);
    check("dib8-getobject", GetObject(h8, sizeof ds, &ds) == (int)sizeof ds && ds.dsBm.bmWidth == 4 &&
          ds.dsBm.bmHeight == 2 && ds.dsBm.bmBitsPixel == 8 && ds.dsBm.bmBits == (void*)b8, ds.dsBm.bmBitsPixel, 8);

    /* bits written by the guest are what GetPixel sees ... */
    fill_dib32(b32, 8, 8);
    old = SelectObject(dc, h32);
    for (bad = 0, y = 0; y < 8; y++) for (x = 0; x < 8; x++) if (GetPixel(dc, x, y) != pat32(x, y)) bad++;
    check("dib32-guest-bits-seen", bad == 0, bad, 0);
    /* ... and what GDI draws shows up in the bits */
    SetPixel(dc, 5, 6, RGB(0x12, 0x34, 0x56));
    check("dib32-gdi-bits-seen", (b32[6 * 8 + 5] & 0xffffff) == 0x123456, b32[6 * 8 + 5], 0x123456);
    SelectObject(dc, old);

    fill_dib24(b24, 5, 3);
    old = SelectObject(dc, h24);
    for (bad = 0, y = 0; y < 3; y++) for (x = 0; x < 5; x++) if (GetPixel(dc, x, y) != pat24(x, y)) bad++;
    check("dib24-guest-bits-seen", bad == 0, bad, 0);
    SetPixel(dc, 4, 0, RGB(0x11, 0x22, 0x33));   /* top row = the last memory row */
    check("dib24-gdi-bits-seen", b24[2 * 16 + 12] == 0x33 && b24[2 * 16 + 13] == 0x22 && b24[2 * 16 + 14] == 0x11,
          b24[2 * 16 + 12] | (b24[2 * 16 + 13] << 8) | (b24[2 * 16 + 14] << 16), 0x112233);
    SelectObject(dc, old);

    fill_dib8(b8, 4, 2);
    old = SelectObject(dc, h8);
    for (bad = 0, y = 0; y < 2; y++) for (x = 0; x < 4; x++) if (GetPixel(dc, x, y) != g_pal_rgb[pat8(x, y)]) bad++;
    check("dib8-guest-bits-seen", bad == 0, bad, 0);
    {
        RGBQUAD q[4];
        UINT n = GetDIBColorTable(dc, 0, 4, q);
        check("dib8-colortable", n == 4 && q[2].rgbBlue == 255 && q[2].rgbRed == 0 && q[3].rgbRed == 255 && q[3].rgbGreen == 255, n, 4);
        /* palette index 1 becomes magenta: pixels that use it follow */
        q[0] = g_pal[1];
        q[0].rgbRed = 255; q[0].rgbGreen = 0; q[0].rgbBlue = 255;
        SetDIBColorTable(dc, 1, 1, q);
        CHECK_PX("dib8-setcolortable", dc, 1, 0, RGB(255, 0, 255));
    }
    SelectObject(dc, old);
    DeleteObject(h32); DeleteObject(h24); DeleteObject(h8);
    DeleteDC(dc);
}

/* ---- part 2: a window ---- */

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void pump(void) {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}

/* The window part. Window-client coordinates equal screen coordinates minus (100,100). */
static void part_window(void) {
    WNDCLASSW wc;
    HWND hwnd;
    HDC wdc, m32, m24, m8, mr;
    BYTE *b24 = NULL, *b8 = NULL;
    DWORD *b32 = NULL, *br = NULL;
    HBITMAP h32, h24, h8, hr, o32, o24, o8, or_;
    int x, y, bad;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"W10XGdiBitmap";
    if (!RegisterClassW(&wc)) { printf("bmp: RegisterClassW failed (%lu)\n", GetLastError()); g_fail++; return; }
    hwnd = CreateWindowExW(0, L"W10XGdiBitmap", L"gdi_bitmap", WS_POPUP | WS_VISIBLE, 100, 100, 400, 300, NULL, NULL,
                           wc.hInstance, NULL);
    if (!hwnd) { printf("bmp: CreateWindowExW failed (%lu)\n", GetLastError()); g_fail++; return; }
    pump();
    printf("bmp: window created\n");
    fflush(stdout);

    wdc = GetDC(hwnd);
    m32 = CreateCompatibleDC(wdc); m24 = CreateCompatibleDC(wdc); m8 = CreateCompatibleDC(wdc); mr = CreateCompatibleDC(wdc);
    h32 = make_dib32(wdc, 8, 8, &b32);
    h24 = make_dib24(wdc, 5, 3, &b24);
    h8 = make_dib8(wdc, 4, 2, &b8);
    hr = make_dib32(wdc, 8, 8, &br);
    if (!wdc || !h32 || !h24 || !h8 || !hr || !b32 || !b24 || !b8 || !br) {
        printf("bmp: window setup FAIL\n"); g_fail++; return;
    }
    fill_dib32(b32, 8, 8); fill_dib24(b24, 5, 3); fill_dib8(b8, 4, 2);
    o32 = SelectObject(m32, h32); o24 = SelectObject(m24, h24); o8 = SelectObject(m8, h8); or_ = SelectObject(mr, hr);

    /* the window background, so nothing depends on what was there */
    {
        HBRUSH bg = CreateSolidBrush(RGB(20, 20, 20));
        RECT r = {0, 0, 400, 300};
        FillRect(wdc, &r, bg);
        DeleteObject(bg);
    }

    /* DIB sections, written through their bits pointers, blitted memory DC -> window DC */
    BitBlt(wdc, 10, 10, 8, 8, m32, 0, 0, SRCCOPY);
    CHECK_PX("win-dib32-srccopy", wdc, 13, 12, pat32(3, 2));
    BitBlt(wdc, 30, 10, 5, 3, m24, 0, 0, SRCCOPY);
    CHECK_PX("win-dib24-srccopy", wdc, 34, 12, pat24(4, 2));
    CHECK_PX("win-dib24-topleft", wdc, 30, 10, pat24(0, 0));
    BitBlt(wdc, 50, 10, 4, 2, m8, 0, 0, SRCCOPY);
    CHECK_PX("win-dib8-srccopy-1", wdc, 51, 10, g_pal_rgb[pat8(1, 0)]);
    CHECK_PX("win-dib8-srccopy-2", wdc, 53, 11, g_pal_rgb[pat8(3, 1)]);

    /* NOTSRCCOPY and SRCINVERT */
    BitBlt(wdc, 70, 10, 8, 8, m32, 0, 0, NOTSRCCOPY);
    CHECK_PX("win-notsrccopy", wdc, 73, 12, RGB(0xff - 96, 0xff - 64, 0xff - 0x80));
    {
        HBRUSH br1 = CreateSolidBrush(RGB(0xff, 0x0f, 0x00));
        RECT r = {90, 10, 98, 18};
        FillRect(wdc, &r, br1);
        DeleteObject(br1);
    }
    BitBlt(wdc, 90, 10, 8, 8, m32, 0, 0, SRCINVERT);
    CHECK_PX("win-srcinvert", wdc, 93, 12, RGB(96 ^ 0xff, 64 ^ 0x0f, 0x80));
    {   /* inverting twice restores the fill */
        HBRUSH br1 = CreateSolidBrush(RGB(0xff, 0x0f, 0x00));
        RECT r = {110, 10, 118, 18};
        FillRect(wdc, &r, br1);
        DeleteObject(br1);
    }
    BitBlt(wdc, 110, 10, 8, 8, m32, 0, 0, SRCINVERT);
    BitBlt(wdc, 110, 10, 8, 8, m32, 0, 0, SRCINVERT);
    CHECK_PX("win-srcinvert-twice", wdc, 113, 12, RGB(0xff, 0x0f, 0x00));

    /* 2x stretch memory DC -> window DC */
    StretchBlt(wdc, 10, 40, 16, 16, m32, 0, 0, 8, 8, SRCCOPY);
    CHECK_PX("win-stretch2x-a", wdc, 10 + 7, 40 + 5, pat32(3, 2));
    CHECK_PX("win-stretch2x-b", wdc, 10 + 8, 40 + 4, pat32(4, 2));
    for (bad = 0, y = 0; y < 16; y++) for (x = 0; x < 16; x++) if (GetPixel(wdc, 10 + x, 40 + y) != pat32(x / 2, y / 2)) bad++;
    check("win-stretch2x-all", bad == 0, bad, 0);

    /* window DC -> memory DC readback: the DIB section's bits receive the window pixels */
    BitBlt(mr, 0, 0, 8, 8, wdc, 10, 10, SRCCOPY);
    for (bad = 0, y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            COLORREF c = pat32(x, y);
            DWORD want = ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | GetBValue(c);
            if ((br[y * 8 + x] & 0xffffff) != want) bad++;
        }
    check("win-readback-bits", bad == 0, bad, 0);
    CHECK_PX("win-readback-getpixel", mr, 6, 7, pat32(6, 7));
    {
        static BYTE rows[24 * 8];
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = 8; bi.bmiHeader.biHeight = 8; bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
        SelectObject(mr, or_);   /* GetDIBits wants the bitmap deselected */
        /* bottom-up 24 bpp: image row 2 is memory row 5; pixel 3 starts at byte 9 (B, G, R) */
        check("win-readback-getdibits", GetDIBits(mr, hr, 0, 8, rows, &bi, DIB_RGB_COLORS) == 8 &&
              rows[5 * 24 + 9] == 0x80 && rows[5 * 24 + 10] == 64 && rows[5 * 24 + 11] == 96,
              rows[5 * 24 + 9] | (rows[5 * 24 + 10] << 8) | (rows[5 * 24 + 11] << 16), 0x604080);
        SelectObject(mr, hr);
    }

    /* AlphaBlend (msimg32 -> gdi32 -> NtGdiAlphaBlend) onto the window; constant alpha 128 */
    {
        BLENDFUNCTION bf;
        COLORREF c;
        HBRUSH g = CreateSolidBrush(RGB(100, 100, 100));
        RECT r = {130, 10, 138, 18};
        int rr, gg, bb;
        FillRect(wdc, &r, g);
        DeleteObject(g);
        for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) b32[y * 8 + x] = 0x00c86432;   /* RGB(200,100,50) */
        bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 128; bf.AlphaFormat = 0;
        check("win-alphablend-call", AlphaBlend(wdc, 130, 10, 8, 8, m32, 0, 0, 8, 8, bf), 0, 1);
        c = GetPixel(wdc, 134, 12);
        rr = GetRValue(c); gg = GetGValue(c); bb = GetBValue(c);
        check("win-alphablend-128", rr >= 149 && rr <= 151 && gg == 100 && bb >= 74 && bb <= 76, c, RGB(150, 100, 75));
        fill_dib32(b32, 8, 8);
    }
    /* TransparentBlt onto the window: magenta is the colour key, the pixel at (2,2) is not */
    {
        HBRUSH g = CreateSolidBrush(RGB(7, 8, 9));
        RECT r = {150, 10, 158, 18};
        FillRect(wdc, &r, g);
        DeleteObject(g);
        for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) b32[y * 8 + x] = 0x00ff00ff;
        b32[2 * 8 + 2] = 0x00010203;
        check("win-transparentblt-call", TransparentBlt(wdc, 150, 10, 8, 8, m32, 0, 0, 8, 8, RGB(255, 0, 255)), 0, 1);
        CHECK_PX("win-transparent-key", wdc, 150, 10, RGB(7, 8, 9));
        CHECK_PX("win-transparent-px", wdc, 152, 12, RGB(1, 2, 3));
        fill_dib32(b32, 8, 8);
    }

    /* LoadImage of BITMAP resources with LR_CREATEDIBSECTION */
    {
        HBITMAP l24 = (HBITMAP)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDB_RGB24), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
        HBITMAP l8 = (HBITMAP)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDB_PAL8), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
        DIBSECTION ds;
        HDC lm = CreateCompatibleDC(wdc);
        HGDIOBJ lo;
        if (!l24 || !l8) {
            printf("bmp: LoadImage FAIL (%p %p) err=%lu\n", (void*)l24, (void*)l8, GetLastError());
            g_fail++;
        } else {
            memset(&ds, 0, sizeof ds);
            check("loadimage24-getobject", GetObject(l24, sizeof ds, &ds) == (int)sizeof ds && ds.dsBm.bmWidth == 6 &&
                  ds.dsBm.bmHeight == 4 && ds.dsBm.bmBitsPixel == 24 && ds.dsBm.bmBits != NULL,
                  ds.dsBm.bmWidth | (ds.dsBm.bmHeight << 8) | (ds.dsBm.bmBitsPixel << 16), 6 | (4 << 8) | (24 << 16));
            memset(&ds, 0, sizeof ds);
            check("loadimage8-getobject", GetObject(l8, sizeof ds, &ds) == (int)sizeof ds && ds.dsBm.bmWidth == 4 &&
                  ds.dsBm.bmHeight == 4 && ds.dsBm.bmBitsPixel == 8 && ds.dsBm.bmBits != NULL,
                  ds.dsBm.bmWidth | (ds.dsBm.bmHeight << 8) | (ds.dsBm.bmBitsPixel << 16), 4 | (4 << 8) | (8 << 16));
            lo = SelectObject(lm, l24);
            BitBlt(wdc, 10, 70, 6, 4, lm, 0, 0, SRCCOPY);
            SelectObject(lm, lo);
            for (bad = 0, y = 0; y < 4; y++)
                for (x = 0; x < 6; x++)
                    if (GetPixel(wdc, 10 + x, 70 + y) != RGB(10 + x * 40, 20 + y * 60, 250 - x * 30 - y)) bad++;
            check("loadimage24-pixels", bad == 0, bad, 0);
            lo = SelectObject(lm, l8);
            BitBlt(wdc, 40, 70, 4, 4, lm, 0, 0, SRCCOPY);
            SelectObject(lm, lo);
            for (bad = 0, y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                    if (GetPixel(wdc, 40 + x, 70 + y) != g_pal_rgb[(x + y) % 4]) bad++;
            check("loadimage8-pixels", bad == 0, bad, 0);
            DeleteObject(l24); DeleteObject(l8);
        }
        DeleteDC(lm);
    }

    SelectObject(m32, o32); SelectObject(m24, o24); SelectObject(m8, o8); SelectObject(mr, or_);
    DeleteObject(h32); DeleteObject(h24); DeleteObject(h8); DeleteObject(hr);
    DeleteDC(m32); DeleteDC(m24); DeleteDC(m8); DeleteDC(mr);
    ReleaseDC(hwnd, wdc);   /* flushes the window: the screenshot is this frame */
    pump();
    printf("bmp: window drawn\n");
    fflush(stdout);
}

int main(void) {
    if (part1()) return 1;
    part_dibs();
    part_window();
    printf("bmp: failures=%d\n", g_fail);
    printf("bmp: done\n");
    fflush(stdout);
    return g_fail ? 4 : 42;
}
