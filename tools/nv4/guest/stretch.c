/*
 * stretch.c -- a Windows 9x test of the display driver's stretched
 * images (StretchDIBits / StretchBlt), for the NV4 emulation.
 *
 * A banner-like source (coloured bands, small text, 1-pixel lines) is
 * drawn at six sizes (columns) from six sources (rows): 8, 4, 16, 24
 * and 32 bpp DIBs through StretchDIBits, and a device bitmap through
 * StretchBlt. The top-left cell of each row is 1:1.
 *
 *   stretch        draw on the screen (the display driver does it)
 *   stretch ref    draw into a 32 bpp DIB section first (GDI's own
 *                  software engine), then copy that 1:1 to the screen
 *   ... bw         keep the DC's default stretch mode (BLACKONWHITE)
 *                  instead of COLORONCOLOR
 *
 * Any key or click ends it. Built without the C runtime (build.sh), so
 * it runs on Windows 95/98 as is.
 */
#include <windows.h>

#define SRC_W 100
#define SRC_H 48
#define NCOLS 6
#define NROWS 6
#define ROW_H 112

static const int dst_w[NCOLS] = { 100, 98, 150, 200, 50, 113 };
static const int dst_h[NCOLS] = { 48, 46, 72, 96, 24, 53 };
static const char *row_name[NROWS] = { "8 bpp DIB", "4 bpp DIB", "16 bpp DIB", "24 bpp DIB",
                                       "32 bpp DIB", "device bitmap" };

static int   ref_mode;
static int   keep_mode;
static DWORD src32[SRC_W * SRC_H]; /* the source, top-down X8R8G8B8 */

/* The DIBs: header + palette, bits bottom-up as GDI likes them */
typedef struct {
    BITMAPINFOHEADER h;
    RGBQUAD          pal[256];
} bmi_t;

static bmi_t bmi[5];
static BYTE  bits8[SRC_W * SRC_H];
static BYTE  bits4[((SRC_W + 7) / 8 * 4) * SRC_H];
static WORD  bits16[((SRC_W + 1) & ~1) * SRC_H];
static BYTE  bits24[((SRC_W * 3 + 3) & ~3) * SRC_H];
static DWORD bits32[SRC_W * SRC_H];
static const void *bits[5] = { bits8, bits4, bits16, bits24, bits32 };

void *
memset(void *d, int c, size_t n)
{
    BYTE *p = d;

    while (n--)
        *p++ = (BYTE) c;
    return d;
}

static const DWORD vga16[16] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xc0c0c0,
    0x808080, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff
};

static int
nearest(const RGBQUAD *pal, int n, DWORD c)
{
    int best = 0;
    long bd = 0x7fffffff;

    for (int i = 0; i < n; i++) {
        long dr = (long) ((c >> 16) & 0xff) - pal[i].rgbRed;
        long dg = (long) ((c >> 8) & 0xff) - pal[i].rgbGreen;
        long db = (long) (c & 0xff) - pal[i].rgbBlue;
        long d  = dr * dr + dg * dg + db * db;

        if (d < bd) {
            bd   = d;
            best = i;
        }
    }
    return best;
}

/* The source: an orange band with text, a blue band with text, a
   column of 1-pixel stripes and a checkerboard corner. */
static void
make_source(void)
{
    BITMAPINFOHEADER h;
    void   *dib_bits;
    HDC     dc  = CreateCompatibleDC(NULL);
    HBITMAP dib;
    HFONT   f;
    RECT    r;

    memset(&h, 0, sizeof(h));
    h.biSize     = sizeof(h);
    h.biWidth    = SRC_W;
    h.biHeight   = -SRC_H;
    h.biPlanes   = 1;
    h.biBitCount = 32;
    dib = CreateDIBSection(dc, (BITMAPINFO *) &h, DIB_RGB_COLORS, &dib_bits, NULL, 0);
    SelectObject(dc, dib);
    r.left = 0; r.top = 0; r.right = SRC_W; r.bottom = SRC_H / 2;
    FillRect(dc, &r, CreateSolidBrush(RGB(0xff, 0x80, 0x00)));
    r.top = SRC_H / 2; r.bottom = SRC_H;
    FillRect(dc, &r, CreateSolidBrush(RGB(0x00, 0x00, 0x80)));
    f = CreateFont(-11, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, "Arial");
    SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    TextOut(dc, 3, 2, "THE GAMERS'", 11);
    TextOut(dc, 3, 12, "3DMARK 99", 9);
    SetTextColor(dc, RGB(0xff, 0xff, 0xff));
    TextOut(dc, 3, SRC_H / 2 + 2, "A FUSION OF", 11);
    TextOut(dc, 3, SRC_H / 2 + 12, "DIRECTX6", 8);
    GdiFlush();
    for (int y = 0; y < SRC_H; y++)
        for (int x = 0; x < SRC_W; x++) {
            DWORD c = ((DWORD *) dib_bits)[y * SRC_W + x] & 0xffffff;

            if ((x >= SRC_W - 16) && (x < SRC_W - 8))
                c = (x & 1) ? 0xffffff : 0x000000;
            else if ((x >= SRC_W - 8) && (y < 8))
                c = ((x ^ y) & 1) ? 0x00ff00 : 0xff0000;
            src32[y * SRC_W + x] = c;
        }
    DeleteDC(dc);
    DeleteObject(dib);
    DeleteObject(f);

    /* 8 bpp: a 6x6x6 cube plus the VGA colours; 4 bpp: the VGA colours */
    for (int i = 0; i < 5; i++) {
        bmi[i].h.biSize     = sizeof(BITMAPINFOHEADER);
        bmi[i].h.biWidth    = SRC_W;
        bmi[i].h.biHeight   = SRC_H;
        bmi[i].h.biPlanes   = 1;
        bmi[i].h.biBitCount = (WORD) ((i == 0) ? 8 : (i == 1) ? 4 : (i == 2) ? 16 : (i == 3) ? 24 : 32);
    }
    for (int i = 0; i < 216; i++) {
        bmi[0].pal[i].rgbRed   = (BYTE) ((i / 36) * 51);
        bmi[0].pal[i].rgbGreen = (BYTE) (((i / 6) % 6) * 51);
        bmi[0].pal[i].rgbBlue  = (BYTE) ((i % 6) * 51);
    }
    for (int i = 0; i < 16; i++) {
        RGBQUAD q;

        q.rgbRed      = (BYTE) (vga16[i] >> 16);
        q.rgbGreen    = (BYTE) (vga16[i] >> 8);
        q.rgbBlue     = (BYTE) vga16[i];
        q.rgbReserved = 0;
        bmi[0].pal[216 + i] = q;
        bmi[1].pal[i]       = q;
    }
    bmi[0].h.biClrUsed = 232;
    bmi[1].h.biClrUsed = 16;
    for (int y = 0; y < SRC_H; y++) {
        int row = SRC_H - 1 - y;

        for (int x = 0; x < SRC_W; x++) {
            DWORD c   = src32[y * SRC_W + x];
            int   i4  = nearest(bmi[1].pal, 16, c);
            BYTE *b24 = &bits24[row * ((SRC_W * 3 + 3) & ~3) + x * 3];
            BYTE *b4  = &bits4[row * ((SRC_W + 7) / 8 * 4) + x / 2];

            bits8[row * SRC_W + x] = (BYTE) nearest(bmi[0].pal, 232, c);
            if (x & 1)
                *b4 = (BYTE) ((*b4 & 0xf0) | i4);
            else
                *b4 = (BYTE) ((*b4 & 0x0f) | (i4 << 4));
            bits16[row * ((SRC_W + 1) & ~1) + x] = (WORD) (((c >> 9) & 0x7c00) | ((c >> 6) & 0x3e0) | ((c >> 3) & 0x1f));
            b24[0] = (BYTE) c;
            b24[1] = (BYTE) (c >> 8);
            b24[2] = (BYTE) (c >> 16);
            bits32[row * SRC_W + x] = c;
        }
    }
}

static void
draw_all(HDC dc, int scr_w)
{
    HDC     mdc = CreateCompatibleDC(dc);
    HBITMAP ddb = CreateCompatibleBitmap(dc, SRC_W, SRC_H);
    char    s[64];

    SetDIBits(dc, ddb, 0, SRC_H, bits32, (BITMAPINFO *) &bmi[4], DIB_RGB_COLORS);
    SelectObject(mdc, ddb);
    if (!keep_mode)
        SetStretchBltMode(dc, COLORONCOLOR);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xff, 0xff, 0xff));
    for (int r = 0; r < NROWS; r++) {
        int x = 8;
        int y = 4 + r * ROW_H;

        TextOut(dc, x, y, s, wsprintf(s, "%s%s", row_name[r], ref_mode ? " (GDI reference)" : ""));
        for (int c = 0; c < NCOLS; c++) {
            if (r < 5)
                StretchDIBits(dc, x, y + 16, dst_w[c], dst_h[c], 0, 0, SRC_W, SRC_H, bits[r],
                              (BITMAPINFO *) &bmi[r], DIB_RGB_COLORS, SRCCOPY);
            else
                StretchBlt(dc, x, y + 16, dst_w[c], dst_h[c], mdc, 0, 0, SRC_W, SRC_H, SRCCOPY);
            x += dst_w[c] + 8;
        }
    }
    (void) scr_w;
    DeleteDC(mdc);
    DeleteObject(ddb);
}

static LRESULT CALLBACK
wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC         dc = BeginPaint(w, &ps);
            int         sw = GetSystemMetrics(SM_CXSCREEN);
            int         sh = GetSystemMetrics(SM_CYSCREEN);

            if (ref_mode) {
                BITMAPINFOHEADER h;
                void   *b;
                HDC     mdc = CreateCompatibleDC(dc);
                HBITMAP dib;
                RECT    r = { 0, 0, sw, sh };

                memset(&h, 0, sizeof(h));
                h.biSize     = sizeof(h);
                h.biWidth    = sw;
                h.biHeight   = -sh;
                h.biPlanes   = 1;
                h.biBitCount = 32;
                dib = CreateDIBSection(dc, (BITMAPINFO *) &h, DIB_RGB_COLORS, &b, NULL, 0);
                SelectObject(mdc, dib);
                FillRect(mdc, &r, GetStockObject(BLACK_BRUSH));
                draw_all(mdc, sw);
                BitBlt(dc, 0, 0, sw, sh, mdc, 0, 0, SRCCOPY);
                DeleteDC(mdc);
                DeleteObject(dib);
            } else
                draw_all(dc, sw);
            EndPaint(w, &ps);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_LBUTTONDOWN:
            DestroyWindow(w);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(w, m, wp, lp);
}

static int
has_word(const char *cmd, const char *w)
{
    for (; *cmd; cmd++) {
        const char *a = cmd;
        const char *b = w;

        while (*b && ((*a | 0x20) == *b)) {
            a++;
            b++;
        }
        if (!*b)
            return 1;
    }
    return 0;
}

void
entry(void)
{
    const char *cmd = GetCommandLine();
    WNDCLASS    wc;
    MSG         msg;
    HINSTANCE   inst = GetModuleHandle(NULL);

    /* skip the program name */
    if (*cmd == '"') {
        for (cmd++; *cmd && (*cmd != '"'); cmd++)
            ;
    }
    while (*cmd && (*cmd != ' '))
        cmd++;
    ref_mode  = has_word(cmd, "ref");
    keep_mode = has_word(cmd, "bw");

    make_source();
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "nv4stretch";
    RegisterClass(&wc);
    CreateWindow("nv4stretch", "stretch", WS_POPUP | WS_VISIBLE, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                 GetSystemMetrics(SM_CYSCREEN), NULL, NULL, inst, NULL);
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    ExitProcess(0);
}
