/* gdichk -- GDI conformance: every case is drawn twice, once on the screen
   (the display driver, i.e. the emulated accelerator) and once into a DIB
   section of the screen's own pixel format (Windows' software DIB engine,
   the oracle). The screen is read back and both are compared exactly.

   Colours are the 16 VGA colours only, so 256-colour modes match without
   dithering. Run it once per colour depth.
   Switches: /Q no pauses between cases. */
#include "ct.h"

#define W 512
#define H 384

static const COLORREF vga[16] = {
    RGB(0, 0, 0), RGB(128, 0, 0), RGB(0, 128, 0), RGB(128, 128, 0),
    RGB(0, 0, 128), RGB(128, 0, 128), RGB(0, 128, 128), RGB(192, 192, 192),
    RGB(128, 128, 128), RGB(255, 0, 0), RGB(0, 255, 0), RGB(255, 255, 0),
    RGB(0, 0, 255), RGB(255, 0, 255), RGB(0, 255, 255), RGB(255, 255, 255)
};

typedef void (*opfn)(HDC dc, HDC off);

static HDC     scr, refdc, capdc, scroff, refoff, monodc;
static HBITMAP refbm, capbm, scroffbm, refoffbm, monobm;
static DWORD  *buf_s, *buf_r;
static int     quick;

static struct {
    BITMAPINFOHEADER h;
    union {
        DWORD   masks[3];
        RGBQUAD pal[256];
    } u;
} fmt;

/* The screen's pixel format as a DIB description. */
static void
screen_format(void)
{
    HBITMAP     b;
    PALETTEENTRY pe[256];
    int         bpp = GetDeviceCaps(scr, BITSPIXEL) * GetDeviceCaps(scr, PLANES), i;

    memset(&fmt, 0, sizeof(fmt));
    fmt.h.biSize     = sizeof(fmt.h);
    fmt.h.biWidth    = W;
    fmt.h.biHeight   = -H;
    fmt.h.biPlanes   = 1;
    fmt.h.biBitCount = (WORD) bpp;
    if (bpp == 16) {
        /* Ask GDI for the device's 16 bpp masks (555 or 565). */
        b = CreateCompatibleBitmap(scr, 1, 1);
        fmt.h.biBitCount = 0;
        GetDIBits(scr, b, 0, 1, NULL, (BITMAPINFO *) &fmt, DIB_RGB_COLORS);
        if (fmt.h.biCompression == BI_BITFIELDS)
            GetDIBits(scr, b, 0, 1, NULL, (BITMAPINFO *) &fmt, DIB_RGB_COLORS);
        DeleteObject(b);
        fmt.h.biSize     = sizeof(fmt.h);
        fmt.h.biWidth    = W;
        fmt.h.biHeight   = -H;
        fmt.h.biPlanes   = 1;
        fmt.h.biBitCount = 16;
        fmt.h.biSizeImage = 0;
        if (fmt.h.biCompression != BI_BITFIELDS) {
            fmt.h.biCompression = BI_BITFIELDS;
            fmt.u.masks[0]      = 0x7c00;
            fmt.u.masks[1]      = 0x03e0;
            fmt.u.masks[2]      = 0x001f;
        }
        ct_log("INFO  screen 16 bpp masks %04x %04x %04x", fmt.u.masks[0], fmt.u.masks[1], fmt.u.masks[2]);
    } else if (bpp <= 8) {
        GetSystemPaletteEntries(scr, 0, 1 << bpp, pe);
        for (i = 0; i < (1 << bpp); i++) {
            fmt.u.pal[i].rgbRed   = pe[i].peRed;
            fmt.u.pal[i].rgbGreen = pe[i].peGreen;
            fmt.u.pal[i].rgbBlue  = pe[i].peBlue;
        }
        fmt.h.biClrUsed = 1 << bpp;
        ct_log("INFO  screen %d bpp (system palette)", bpp);
    } else
        ct_log("INFO  screen %d bpp", bpp);
}

static HBITMAP
fmt_dib(HDC *dc)
{
    void   *bits;
    HBITMAP b = CreateDIBSection(scr, (BITMAPINFO *) &fmt, DIB_RGB_COLORS, &bits, NULL, 0);

    *dc = CreateCompatibleDC(scr);
    SelectObject(*dc, b);
    return b;
}

static void
to32(HBITMAP b, DWORD *out)
{
    BITMAPINFOHEADER h;

    memset(&h, 0, sizeof(h));
    h.biSize     = sizeof(h);
    h.biWidth    = W;
    h.biHeight   = -H;
    h.biPlanes   = 1;
    h.biBitCount = 32;
    GetDIBits(capdc, b, 0, H, out, (BITMAPINFO *) &h, DIB_RGB_COLORS);
}

static void
solid(HDC dc, int x, int y, int w, int h, COLORREF c, DWORD rop)
{
    HBRUSH b = CreateSolidBrush(c), o = SelectObject(dc, b);

    PatBlt(dc, x, y, w, h, rop);
    SelectObject(dc, o);
    DeleteObject(b);
}

/* A busy 128x64 picture used as a blit source. */
static void
picture(HDC dc, int x0, int y0)
{
    int i;

    for (i = 0; i < 16; i++)
        solid(dc, x0 + i * 8, y0, 8, 64, vga[i], PATCOPY);
    for (i = 0; i < 8; i++)
        solid(dc, x0, y0 + i * 8 + 2, 128, 3, vga[(i * 5 + 3) & 15], PATCOPY);
    solid(dc, x0 + 40, y0 + 20, 48, 24, vga[15], PATINVERT);
}

/* ---- cases ---------------------------------------------------------- */

static void
c_fill(HDC dc, HDC off)
{
    static const DWORD rops[5] = { PATCOPY, PATINVERT, DSTINVERT, BLACKNESS, WHITENESS };
    int                i, x, y;
    RECT               r;

    for (i = 0; i < 64; i++) {
        x = (i * 37) % 480;
        y = (i * 53) % 350;
        solid(dc, x, y, 1 + (i * 7) % 61, 1 + (i * 11) % 43, vga[i & 15], rops[i % 5]);
    }
    /* Single pixels, one-wide and one-high strips at odd alignments. */
    for (i = 0; i < 32; i++) {
        solid(dc, 10 + i * 3, 360, 1, 1, vga[i & 15], PATCOPY);
        solid(dc, 200 + i, 300, 1, 40 + i, vga[(i + 4) & 15], PATCOPY);
        solid(dc, 300, 100 + i * 2, 33 + i, 1, vga[(i + 9) & 15], PATCOPY);
    }
    SetRect(&r, 401, 301, 467, 377);
    FillRect(dc, &r, (HBRUSH) GetStockObject(GRAY_BRUSH));
    InvertRect(dc, &r);
}

static DWORD
rop_code(int i)
{
    /* Full codes for the named ROPs; elsewhere only the index byte
       (the part hardware and the DIB engine decode). */
    static const DWORD named[] = { 0x000042, 0x0F0001, 0x110A6, 0x330008, 0x440328, 0x500325,
                                   0x550009, 0x5A0049, 0x660046, 0x8800C6, 0xA000C9, 0xAC0744,
                                   0xB8074A, 0xBB0226, 0xC000CA, 0xCA0749, 0xCC0020, 0xE20746,
                                   0xEE0086, 0xF00021, 0xFB0A09, 0xFF0062 };
    int                n;

    for (n = 0; n < (int) (sizeof(named) / sizeof(named[0])); n++)
        if ((int) (named[n] >> 16) == i)
            return named[n];
    return (DWORD) i << 16;
}

static void
c_rop3(HDC dc, HDC off)
{
    HBRUSH b = CreateHatchBrush(HS_DIAGCROSS, vga[12]), o;
    int    i, x, y;

    picture(dc, 0, 0);
    SetBkColor(dc, vga[14]);
    o = SelectObject(dc, b);
    for (i = 0; i < 256; i++) {
        x = (i & 15) * 32;
        y = 70 + (i >> 4) * 19;
        solid(dc, x, y, 30, 9, vga[3], PATCOPY);
        solid(dc, x, y + 9, 30, 9, vga[13], PATCOPY);
        BitBlt(dc, x, y, 30, 18, dc, (i * 3) & 63, (i >> 2) & 31, rop_code(i));
    }
    SelectObject(dc, o);
    DeleteObject(b);
}

static void
c_scroll(HDC dc, HDC off)
{
    picture(dc, 100, 100);
    picture(dc, 228, 100);
    picture(dc, 100, 164);
    BitBlt(dc, 101, 100, 255, 128, dc, 100, 100, SRCCOPY);  /* right by 1 */
    BitBlt(dc, 100, 107, 256, 128, dc, 100, 100, SRCCOPY);  /* down by 7 */
    BitBlt(dc, 67, 100, 256, 128, dc, 100, 100, SRCCOPY);   /* left by 33 */
    BitBlt(dc, 100, 61, 256, 128, dc, 100, 100, SRCCOPY);   /* up by 39 */
    BitBlt(dc, 113, 117, 200, 100, dc, 100, 100, SRCCOPY);  /* diagonal */
    BitBlt(dc, 300, 250, 128, 100, dc, 305, 255, SRCCOPY);  /* up-left */
    BitBlt(dc, 3, 3, 500, 30, dc, 0, 0, SRCINVERT);         /* overlapping XOR */
}

static void
c_brush(HDC dc, HDC off)
{
    static const WORD  patw[8]  = { 0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81 };
    struct {
        BITMAPINFOHEADER h;
        RGBQUAD          c[16];
        BYTE             px[32];
    } dib;
    HBITMAP mono;
    HBRUSH  b, o;
    int     i, j;

    for (i = 0; i < 6; i++) {
        for (j = 0; j < 2; j++) {
            SetBkMode(dc, j ? TRANSPARENT : OPAQUE);
            SetBkColor(dc, vga[11]);
            solid(dc, i * 80 + 5, j * 70 + 5, 70, 60, vga[7], PATCOPY);
            b = CreateHatchBrush(i, vga[(i + 1) & 15]);
            o = SelectObject(dc, b);
            /* Rectangle respects the background mode for hatches. */
            SelectObject(dc, GetStockObject(NULL_PEN));
            Rectangle(dc, i * 80 + 5, j * 70 + 5, i * 80 + 76, j * 70 + 66);
            SelectObject(dc, o);
            DeleteObject(b);
        }
    }
    SetBkMode(dc, OPAQUE);
    /* Mono 8x8 pattern brush, text/background colours, brush origins. */
    mono = CreateBitmap(8, 8, 1, 1, patw);
    SetTextColor(dc, vga[4]);
    SetBkColor(dc, vga[14]);
    for (i = 0; i < 4; i++) {
        SetBrushOrgEx(dc, i * 3, i, NULL);
        b = CreatePatternBrush(mono);
        UnrealizeObject(b);
        o = SelectObject(dc, b);
        PatBlt(dc, 5 + i * 120, 150, 110, 70, PATCOPY);
        SelectObject(dc, o);
        DeleteObject(b);
    }
    DeleteObject(mono);
    /* Colour 8x8 DIB pattern brush (4 bpp). */
    memset(&dib, 0, sizeof(dib));
    dib.h.biSize     = sizeof(dib.h);
    dib.h.biWidth    = 8;
    dib.h.biHeight   = 8;
    dib.h.biPlanes   = 1;
    dib.h.biBitCount = 4;
    dib.h.biClrUsed  = 16;
    for (i = 0; i < 16; i++) {
        dib.c[i].rgbRed   = GetRValue(vga[i]);
        dib.c[i].rgbGreen = GetGValue(vga[i]);
        dib.c[i].rgbBlue  = GetBValue(vga[i]);
    }
    for (i = 0; i < 32; i++)
        dib.px[i] = (BYTE) (((i * 3) & 15) << 4 | ((i * 7 + 1) & 15));
    for (i = 0; i < 4; i++) {
        SetBrushOrgEx(dc, i, i * 5, NULL);
        b = CreateDIBPatternBrushPt(&dib, DIB_RGB_COLORS);
        UnrealizeObject(b);
        o = SelectObject(dc, b);
        PatBlt(dc, 5 + i * 120, 230, 110, 70, PATCOPY);
        PatBlt(dc, 5 + i * 120, 310, 110, 70, PATINVERT);
        SelectObject(dc, o);
        DeleteObject(b);
    }
    SetBrushOrgEx(dc, 0, 0, NULL);
}

static void
c_lines(HDC dc, HDC off)
{
    static const int rop2[4] = { R2_COPYPEN, R2_XORPEN, R2_NOT, R2_MERGEPEN };
    HPEN             p, o;
    int              i, a;

    /* All octants, every slope class, from two centres. */
    for (a = 0; a < 64; a++) {
        int dx = (a < 16) ? 100 : (a < 32) ? 100 - (a - 16) * 12 : (a < 48) ? -100 : -100 + (a - 48) * 12;
        int dy = (a < 16) ? -100 + a * 12 : (a < 32) ? 100 : (a < 48) ? 100 - (a - 32) * 12 : -100;

        p = CreatePen(PS_SOLID, 0, vga[1 + (a % 15)]);
        o = SelectObject(dc, p);
        MoveToEx(dc, 120, 120, NULL);
        LineTo(dc, 120 + dx, 120 + dy);
        MoveToEx(dc, 380 + dx / 3, 120 + dy / 3, NULL);
        LineTo(dc, 380, 120);
        SelectObject(dc, o);
        DeleteObject(p);
    }
    /* Solid pens in every ROP2 mode over a two-colour background. */
    solid(dc, 0, 240, 512, 70, vga[6], PATCOPY);
    solid(dc, 0, 275, 512, 35, vga[9], PATCOPY);
    for (i = 0; i < 16; i++) {
        SetROP2(dc, rop2[i & 3]);
        p = CreatePen(PS_SOLID, 0, vga[(i * 3 + 2) & 15]);
        o = SelectObject(dc, p);
        MoveToEx(dc, 4 + i * 31, 240, NULL);
        LineTo(dc, 30 + i * 31, 309);
        MoveToEx(dc, 2, 242 + i * 4, NULL);
        LineTo(dc, 509, 244 + i * 4);
        SelectObject(dc, o);
        DeleteObject(p);
    }
    SetROP2(dc, R2_COPYPEN);
    /* A wide pen polyline and exact-end horizontal/vertical lines. */
    {
        POINT pts[5] = { { 20, 330 }, { 120, 370 }, { 220, 320 }, { 320, 375 }, { 490, 325 } };

        p = CreatePen(PS_SOLID, 5, vga[4]);
        o = SelectObject(dc, p);
        Polyline(dc, pts, 5);
        SelectObject(dc, o);
        DeleteObject(p);
    }
    for (i = 0; i < 8; i++) {
        p = CreatePen(PS_SOLID, 0, vga[i + 8]);
        o = SelectObject(dc, p);
        MoveToEx(dc, 260 + i, 200, NULL);
        LineTo(dc, 260 + i, 200 + i * 4);
        MoveToEx(dc, 300, 200 + i * 3, NULL);
        LineTo(dc, 300 + i * 9 + 1, 200 + i * 3);
        SelectObject(dc, o);
        DeleteObject(p);
    }
}

/* Styled cosmetic pens: the dash lengths are the display driver's own
   ("style step" in GDIINFO), so only a loose comparison makes sense. */
static void
c_styled(HDC dc, HDC off)
{
    static const int styles[4] = { PS_DASH, PS_DOT, PS_DASHDOT, PS_DASHDOTDOT };
    HPEN             p, o;
    int              i;

    solid(dc, 0, 100, 512, 140, vga[6], PATCOPY);
    for (i = 0; i < 32; i++) {
        SetBkColor(dc, vga[11]);
        SetBkMode(dc, (i & 4) ? TRANSPARENT : OPAQUE);
        p = CreatePen(styles[i & 3], 0, vga[(i * 3 + 2) & 15]);
        o = SelectObject(dc, p);
        MoveToEx(dc, 4 + i * 15, 60, NULL);
        LineTo(dc, 30 + i * 15, 300);
        MoveToEx(dc, 2, 110 + i * 4, NULL);
        LineTo(dc, 509, 112 + i * 4);
        SelectObject(dc, o);
        DeleteObject(p);
    }
    SetBkMode(dc, OPAQUE);
}

static void
c_shapes(HDC dc, HDC off)
{
    POINT  tri[3]   = { { 20, 150 }, { 120, 20 }, { 200, 170 } };
    POINT  star[10] = { { 440, 25 }, { 457, 76 }, { 511, 77 }, { 468, 109 }, { 484, 160 }, { 440, 130 }, { 396, 160 }, { 412, 109 }, { 369, 77 }, { 423, 76 } };
    POINT  quad[4]  = { { 230, 30 }, { 330, 50 }, { 310, 160 }, { 220, 120 } };
    HBRUSH b, o;
    HPEN   p, op;
    int    i;

    for (i = 0; i < 2; i++) {
        SetPolyFillMode(dc, i ? WINDING : ALTERNATE);
        b  = CreateSolidBrush(vga[9 + i * 3]);
        o  = SelectObject(dc, b);
        op = SelectObject(dc, i ? GetStockObject(BLACK_PEN) : GetStockObject(NULL_PEN));
        if (i) {
            OffsetViewportOrgEx(dc, 0, 190, NULL);
        }
        Polygon(dc, tri, 3);
        Polygon(dc, quad, 4);
        Polygon(dc, star, 10);
        Ellipse(dc, 20 + 300 * i, 180 - 190 * i, 180 + 300 * i, 260 - 190 * i);
        SelectObject(dc, o);
        SelectObject(dc, op);
        DeleteObject(b);
        if (i)
            OffsetViewportOrgEx(dc, 0, -190, NULL);
    }
    p  = CreatePen(PS_SOLID, 0, vga[4]);
    op = SelectObject(dc, p);
    b  = CreateHatchBrush(HS_CROSS, vga[2]);
    o  = SelectObject(dc, b);
    RoundRect(dc, 200, 190, 330, 260, 30, 20);
    Pie(dc, 340, 190, 440, 290, 440, 240, 390, 190);
    Chord(dc, 20, 280, 140, 370, 140, 300, 20, 300);
    Arc(dc, 160, 280, 300, 380, 300, 330, 160, 330);
    Rectangle(dc, 330, 300, 500, 380);
    SelectObject(dc, o);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void
c_text(HDC dc, HDC off)
{
    static const char *faces[4] = { "System", "Courier", "Fixedsys", "Arial" };
    HFONT              f, o;
    RECT               r;
    int                i, y = 2;

    for (i = 0; i < 8; i++) {
        LOGFONTA lf;

        memset(&lf, 0, sizeof(lf));
        lf.lfHeight  = (i < 6) ? -(8 + (i & 3) * 4) : -(i == 6 ? 28 : 40);
        lf.lfWeight  = (i & 1) ? FW_BOLD : FW_NORMAL;
        lf.lfItalic  = (BYTE) (i == 7);
        lf.lfQuality = NONANTIALIASED_QUALITY;
        lstrcpyA(lf.lfFaceName, faces[i < 6 ? (i & 3) : 3]);
        f = CreateFontIndirectA(&lf);
        o = SelectObject(dc, f);
        SetTextColor(dc, vga[(i * 5 + 1) & 15]);
        SetBkColor(dc, vga[(i * 3 + 7) & 15]);
        SetBkMode(dc, (i & 2) ? TRANSPARENT : OPAQUE);
        TextOutA(dc, 4 + i * 3, y, "The quick brown fox 0123456789 !@#$%&*()", 41);
        y += (i < 6) ? 20 + (i & 3) * 5 : 45;
        SelectObject(dc, o);
        DeleteObject(f);
    }
    /* Opaquing rectangle plus clipping, on a background picture. */
    picture(dc, 300, 300);
    SetRect(&r, 280, 310, 470, 340);
    SetTextColor(dc, vga[15]);
    SetBkColor(dc, vga[4]);
    ExtTextOutA(dc, 260, 305, ETO_CLIPPED | ETO_OPAQUE, &r, "Clipped and opaqued text run", 28, NULL);
    SetRect(&r, 300, 350, 400, 380);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, vga[0]);
    ExtTextOutA(dc, 290, 352, ETO_CLIPPED, &r, "Transparent clipped", 19, NULL);
}

static void
c_mono(HDC dc, HDC off)
{
    static const DWORD rops[4] = { SRCCOPY, SRCAND, SRCPAINT, 0xB8074A };
    int                i;
    HBRUSH             b, o;

    for (i = 0; i < 4; i++) {
        picture(dc, i * 128, 0);
        SetTextColor(dc, vga[i * 4 + 1]);
        SetBkColor(dc, vga[14 - i]);
        b = CreateSolidBrush(vga[12]);
        o = SelectObject(dc, b);
        BitBlt(dc, i * 128, 0, 128, 64, monodc, 0, 0, rops[i]);
        BitBlt(dc, i * 128 + 3, 80 + i * 70, 125, 64, monodc, i, 0, rops[i]);
        SelectObject(dc, o);
        DeleteObject(b);
    }
}

static void
c_stretch(HDC dc, HDC off)
{
    picture(dc, 0, 0);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchBlt(dc, 0, 70, 384, 192, dc, 0, 0, 128, 64, SRCCOPY);   /* x3 */
    StretchBlt(dc, 140, 0, 64, 32, dc, 0, 0, 128, 64, SRCCOPY);    /* /2 */
    StretchBlt(dc, 210, 0, 100, 50, dc, 0, 0, 128, 64, SRCCOPY);   /* odd ratio */
    StretchBlt(dc, 440, 0, -60, 60, dc, 0, 0, 128, 64, SRCCOPY);   /* mirrored x */
    StretchBlt(dc, 400, 270, 100, -100, dc, 0, 0, 128, 64, SRCCOPY); /* mirrored y */
    /* Mono source: 1 bits take the background colour, 0 bits the text
       colour (set here, so nothing is inherited from earlier cases). */
    SetTextColor(dc, vga[0]);
    SetBkColor(dc, vga[15]);
    StretchBlt(dc, 10, 280, 300, 90, monodc, 0, 0, 64, 32, SRCCOPY);
}

static void
dibs(HDC dc, int stretch)
{
    static const int depths[6] = { 1, 4, 8, 16, 24, 32 };
    struct {
        BITMAPINFOHEADER h;
        RGBQUAD          c[256];
    } bi;
    BYTE *px = (BYTE *) GlobalAlloc(GPTR, 96 * 4 * 64);
    int   d, x, y, i;

    for (d = 0; d < 6; d++) {
        int bpp = depths[d], stride = ((96 * bpp + 31) / 32) * 4;

        memset(&bi, 0, sizeof(bi));
        bi.h.biSize     = sizeof(bi.h);
        bi.h.biWidth    = 96;
        bi.h.biHeight   = (d & 1) ? -64 : 64;
        bi.h.biPlanes   = 1;
        bi.h.biBitCount = (WORD) bpp;
        if (bpp <= 8) {
            for (i = 0; i < (1 << bpp); i++) {
                COLORREF c = vga[(i * 7 + 3) & 15];

                bi.c[i].rgbRed   = GetRValue(c);
                bi.c[i].rgbGreen = GetGValue(c);
                bi.c[i].rgbBlue  = GetBValue(c);
            }
        }
        for (y = 0; y < 64; y++) {
            for (x = 0; x < 96; x++) {
                int      idx = ((x / 6) + (y / 8) * 3) & 15;
                COLORREF c   = vga[idx];
                BYTE    *p   = px + y * stride;

                switch (bpp) {
                    case 1:
                        if (idx & 1)
                            p[x >> 3] |= (BYTE) (0x80 >> (x & 7));
                        break;
                    case 4:
                        p[x >> 1] |= (BYTE) ((idx & 15) << ((x & 1) ? 0 : 4));
                        break;
                    case 8:
                        p[x] = (BYTE) idx;
                        break;
                    case 16:
                        ((WORD *) p)[x] = (WORD) (((GetRValue(c) >> 3) << 10) | ((GetGValue(c) >> 3) << 5) | (GetBValue(c) >> 3));
                        break;
                    case 24:
                        p[x * 3]     = GetBValue(c);
                        p[x * 3 + 1] = GetGValue(c);
                        p[x * 3 + 2] = GetRValue(c);
                        break;
                    default:
                        ((DWORD *) p)[x] = (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
                        break;
                }
            }
        }
        if (!stretch) {
            SetDIBitsToDevice(dc, (d % 5) * 100 + 4, (d / 5) * 70 + 4, 96, 64, 0, 0, 0, 64, px,
                              (BITMAPINFO *) &bi, DIB_RGB_COLORS);
            /* A partial band. */
            SetDIBitsToDevice(dc, (d % 5) * 100 + 4, 150, 96, 32, 0, 16, 0, 64, px, (BITMAPINFO *) &bi,
                              DIB_RGB_COLORS);
        }
        if (stretch) {
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, d * 84 + 2, 200, 80 + d * 4, 50 + d * 20, 8, 8, 64, 48, px,
                          (BITMAPINFO *) &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        memset(px, 0, 96 * 4 * 64);
    }
    GlobalFree(px);
}

static void
c_dib(HDC dc, HDC off)
{
    dibs(dc, 0);
}

/* Stretches: hardware may step through the source with its own DDA, so
   these are compared with one pixel of position slack. */
static void
c_dibstr(HDC dc, HDC off)
{
    dibs(dc, 1);
}

static void
c_clip(HDC dc, HDC off)
{
    HRGN   a = CreateEllipticRgn(40, 40, 300, 250), b = CreateRectRgn(250, 150, 480, 360);
    HPEN   p, op;
    int    i;

    CombineRgn(a, a, b, RGN_XOR);
    SelectClipRgn(dc, a);
    for (i = 0; i < 16; i++)
        solid(dc, i * 32, 0, 32, H, vga[i], PATCOPY);
    picture(dc, 0, 0);
    BitBlt(dc, 200, 100, 128, 64, dc, 0, 0, SRCCOPY);
    BitBlt(dc, 260, 200, 128, 64, dc, 0, 0, NOTSRCCOPY);
    p  = CreatePen(PS_SOLID, 0, vga[15]);
    op = SelectObject(dc, p);
    for (i = 0; i < 20; i++) {
        MoveToEx(dc, 0, i * 19, NULL);
        LineTo(dc, 511, 383 - i * 19);
    }
    SelectObject(dc, op);
    DeleteObject(p);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, vga[0]);
    for (i = 0; i < 18; i++)
        TextOutA(dc, 10, i * 20, "Clipped text through a complex region XXXXXXXX", 46);
    SelectClipRgn(dc, NULL);
    DeleteObject(a);
    DeleteObject(b);
}

/* Off-screen: draw into a device bitmap (video memory on the card,
   a DIB on the reference), then blit it to the screen in pieces. */
static void
c_offscreen(HDC dc, HDC off)
{
    int i;

    PatBlt(off, 0, 0, W, H, WHITENESS);
    for (i = 0; i < 4; i++)
        picture(off, i * 128, 0);
    c_lines(off, NULL);
    BitBlt(dc, 0, 0, W, H, off, 0, 0, SRCCOPY);
    BitBlt(dc, 20, 300, 200, 80, off, 0, 0, SRCINVERT);
    BitBlt(off, 0, 200, 256, 128, dc, 256, 0, SRCCOPY); /* screen to off-screen */
    BitBlt(dc, 256, 256, 256, 128, off, 0, 200, SRCCOPY);
}

#define EXACT 0
#define LOOSE 1 /* device-defined sampling: 2 px position slack, colour rounding */
#define ROUND 2 /* exact positions, colour expansion rounding (555 -> 888) */

static const struct {
    const char *name;
    opfn        fn;
    int         loose;
} cases[] = {
    { "FILL", c_fill, EXACT },       { "ROP3", c_rop3, EXACT },       { "SCROLL", c_scroll, EXACT },
    { "BRUSH", c_brush, EXACT },     { "LINES", c_lines, EXACT },     { "STYLED", c_styled, LOOSE },
    { "SHAPES", c_shapes, EXACT },   { "TEXT", c_text, EXACT },       { "MONO", c_mono, EXACT },
    { "STRETCH", c_stretch, LOOSE }, { "DIB", c_dib, ROUND },         { "DIBSTR", c_dibstr, LOOSE },
    { "CLIP", c_clip, EXACT },       { "OFFSCRN", c_offscreen, EXACT },
};

static void
run(int i)
{
    PatBlt(scr, 0, 0, W, H, WHITENESS);
    PatBlt(refdc, 0, 0, W, H, WHITENESS);
    cases[i].fn(scr, scroff);
    cases[i].fn(refdc, refoff);
    GdiFlush();
    BitBlt(capdc, 0, 0, W, H, scr, 0, 0, SRCCOPY);
    GdiFlush();
    to32(capbm, buf_s);
    to32(refbm, buf_r);
    if (cases[i].loose == LOOSE)
        ct_grade_r(cases[i].name, buf_s, buf_r, W, H, 8, 2, 1, 1001, "(2 px position slack; device-defined, never FAIL)");
    else if (cases[i].loose == ROUND)
        ct_grade(cases[i].name, buf_s, buf_r, W, H, 8, 0, 0, "(colour rounding allowed)");
    else
        ct_grade(cases[i].name, buf_s, buf_r, W, H, 0, 0, 0, "");
    ct_pump(quick ? 50 : 700);
}

void __stdcall
entry(void)
{
    HWND hw;
    int  i;
    WORD mb[64 * 32 / 16];

    ct_open("GDICHK");
    quick = ct_flag("/Q");
    hw    = ct_window(W, H);
    ShowCursor(FALSE);
    scr = GetDC(hw);
    screen_format();
    refbm = fmt_dib(&refdc);
    capbm = fmt_dib(&capdc);
    refoffbm = fmt_dib(&refoff);
    scroffbm = CreateCompatibleBitmap(scr, W, H);
    scroff   = CreateCompatibleDC(scr);
    SelectObject(scroff, scroffbm);
    /* A 64x32 mono bitmap: checkerboard with a diagonal band. */
    for (i = 0; i < 64 * 32 / 16; i++) {
        int y = i / 4, wx = i % 4;

        mb[i] = (WORD) (((y & 4) ? 0xf0f0 : 0x0f0f) ^ ((wx == (y >> 3)) ? 0xffff : 0));
    }
    monobm = CreateBitmap(64, 32, 1, 1, mb);
    monodc = CreateCompatibleDC(scr);
    SelectObject(monodc, monobm);
    buf_s = (DWORD *) GlobalAlloc(GPTR, W * H * 4);
    buf_r = (DWORD *) GlobalAlloc(GPTR, W * H * 4);
    for (i = 0; i < (int) (sizeof(cases) / sizeof(cases[0])); i++)
        run(i);
    ct_close();
    ExitProcess(ct_nfail ? 1 : 0);
}
