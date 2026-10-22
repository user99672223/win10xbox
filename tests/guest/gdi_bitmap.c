/* guest_gdi_bitmap: GDI bitmap operations through gdi32 -> win32u syscalls -> core/user, verified
 * pixel-exact with GetPixel / GetDIBits. Memory DCs only (no window needed). Prints one line per
 * check ("bmp: <name> ok|FAIL ...") and "bmp: done"; exits 42 only if every check passed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
/* msimg32 is not shipped as a builtin; gdi32 exports the same functions as Gdi*. */
typedef BOOL (WINAPI *alphablend_t)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
typedef BOOL (WINAPI *transblt_t)(HDC, int, int, int, int, HDC, int, int, int, int, UINT);
static alphablend_t pAlphaBlend;
static transblt_t pTransparentBlt;
#define AlphaBlend pAlphaBlend
#define TransparentBlt pTransparentBlt
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

int main(void) {
    HDC a = CreateCompatibleDC(NULL), b = CreateCompatibleDC(NULL);
    DWORD *pa = NULL, *pb = NULL;
    HBITMAP ba, bb, oa, ob;
    BITMAPINFO bi;
    int i;
    {
        HMODULE g = GetModuleHandleA("gdi32.dll");
        pAlphaBlend = (alphablend_t)GetProcAddress(g, "GdiAlphaBlend");
        pTransparentBlt = (transblt_t)GetProcAddress(g, "GdiTransparentBlt");
        if (!pAlphaBlend || !pTransparentBlt) { printf("bmp: setup failed (gdi32 exports)\n"); return 1; }
    }
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
    printf("bmp: failures=%d\n", g_fail);
    printf("bmp: done\n");
    fflush(stdout);
    return g_fail ? 4 : 42;
}
