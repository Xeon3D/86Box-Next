/* ddchk -- DirectDraw conformance (DirectX 3 interfaces, so it runs on
   NT 4 too). The card's video-memory surfaces are compared against the
   same operations on system-memory surfaces (the HEL, the oracle).

   Windowed part: colour fills, uploads, overlapping video-to-video blits,
   source colour key, mirror, stretch, Lock-after-Blt coherence, GDI on a
   surface. Full-screen part (640x480x16): flip rate vs monitor rate,
   vertical-blank waits and scan line, then an overlay shown for the
   viewer (VISUAL -- an overlay is not in the frame buffer, so it can only
   be judged by eye or by a host screenshot).
   Switches: /Q shorter visual cases, /NOFS skip the full-screen part. */
#include "ct.h"
#include <ddraw.h>

#define SW 256
#define SH 256

static IDirectDraw2       *dd;
static DDCAPS              hal, hel;
static DDSURFACEDESC       pdesc;
static DWORD              *buf_s, *buf_r;
static int                 quick;
static HRESULT(WINAPI *pDDCreate)(GUID *, LPDIRECTDRAW *, IUnknown *);

static void
log_caps(const char *who, DDCAPS *c)
{
    ct_log("INFO  %s caps=%08x caps2=%08x ckey=%08x fx=%08x palcaps=%08x", who, c->dwCaps, c->dwCaps2,
           c->dwCKeyCaps, c->dwFXCaps, c->dwPalCaps);
    ct_log("INFO  %s vidmem total=%d free=%d overlays max=%d align src=%d/%d dst=%d/%d", who,
           c->dwVidMemTotal, c->dwVidMemFree, c->dwMaxVisibleOverlays, c->dwAlignBoundarySrc,
           c->dwAlignSizeSrc, c->dwAlignBoundaryDest, c->dwAlignSizeDest);
    ct_log("INFO  %s stretch min=%d max=%d rops=%08x %08x", who, c->dwMinOverlayStretch,
           c->dwMaxOverlayStretch, c->dwRops[0], c->dwRops[1]);
}

static HRESULT WINAPI
mode_cb(LPDDSURFACEDESC d, LPVOID ctx)
{
    ct_log("INFO  ddmode %dx%dx%d %dHz", d->dwWidth, d->dwHeight, d->ddpfPixelFormat.dwRGBBitCount,
           d->dwRefreshRate);
    return DDENUMRET_OK;
}

static IDirectDrawSurface *
mksurf(DWORD caps, int w, int h)
{
    DDSURFACEDESC       d;
    IDirectDrawSurface *s = NULL;

    memset(&d, 0, sizeof(d));
    d.dwSize         = sizeof(d);
    d.dwFlags        = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | caps;
    d.dwWidth        = w;
    d.dwHeight       = h;
    d.ddpfPixelFormat = pdesc.ddpfPixelFormat;
    if (IDirectDraw2_CreateSurface(dd, &d, &s, NULL) != DD_OK)
        return NULL;
    return s;
}

static int
mshift(DWORD m)
{
    int s = 0;

    if (!m)
        return 0;
    while (!(m & 1)) {
        m >>= 1;
        s++;
    }
    return s;
}

static int
mbits(DWORD m)
{
    int n = 0;

    while (m) {
        n += m & 1;
        m >>= 1;
    }
    return n;
}

static DWORD
expand(DWORD v, DWORD mask)
{
    int n = mbits(mask);

    if (!n)
        return 0;
    v = (v & mask) >> mshift(mask);
    v <<= 8 - n;
    return (v | (v >> n)) & 0xff;
}

/* Read a surface into a 32 bpp xRGB buffer (8 bpp: index as grey). */
static int
read32(IDirectDrawSurface *s, DWORD *out)
{
    DDSURFACEDESC d;
    int           x, y, bpp;

    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    if (IDirectDrawSurface_Lock(s, NULL, &d, DDLOCK_WAIT, NULL) != DD_OK)
        return 0;
    bpp = d.ddpfPixelFormat.dwRGBBitCount;
    for (y = 0; y < (int) d.dwHeight; y++) {
        BYTE *row = (BYTE *) d.lpSurface + y * d.lPitch;

        for (x = 0; x < (int) d.dwWidth; x++) {
            DWORD p;

            switch (bpp) {
                case 8:
                    out[y * d.dwWidth + x] = row[x] * 0x010101;
                    continue;
                case 16:
                    p = ((WORD *) row)[x];
                    break;
                case 24:
                    p = row[x * 3] | (row[x * 3 + 1] << 8) | (row[x * 3 + 2] << 16);
                    break;
                default:
                    p = ((DWORD *) row)[x];
                    break;
            }
            out[y * d.dwWidth + x] = (expand(p, d.ddpfPixelFormat.dwRBitMask) << 16) |
                                     (expand(p, d.ddpfPixelFormat.dwGBitMask) << 8) |
                                     expand(p, d.ddpfPixelFormat.dwBBitMask);
        }
    }
    IDirectDrawSurface_Unlock(s, NULL);
    return 1;
}

/* A colour in the surface's format from 8-bit RGB (8 bpp: an index). */
static DWORD
pcol(int r, int g, int b)
{
    DDPIXELFORMAT *f = &pdesc.ddpfPixelFormat;

    if (f->dwRGBBitCount == 8)
        return (DWORD) ((r * 3 + g * 5 + b) >> 3) & 0xff;
    return (((DWORD) r >> (8 - mbits(f->dwRBitMask))) << mshift(f->dwRBitMask)) |
           (((DWORD) g >> (8 - mbits(f->dwGBitMask))) << mshift(f->dwGBitMask)) |
           (((DWORD) b >> (8 - mbits(f->dwBBitMask))) << mshift(f->dwBBitMask));
}

static void
fill(IDirectDrawSurface *s, int x, int y, int w, int h, DWORD c)
{
    DDBLTFX fx;
    RECT    r;

    memset(&fx, 0, sizeof(fx));
    fx.dwSize      = sizeof(fx);
    fx.dwFillColor = c;
    SetRect(&r, x, y, x + w, y + h);
    IDirectDrawSurface_Blt(s, &r, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
}

/* CPU-written test picture. */
static void
cpu_picture(IDirectDrawSurface *s, int seed)
{
    DDSURFACEDESC d;
    int           x, y, bpp;

    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    if (IDirectDrawSurface_Lock(s, NULL, &d, DDLOCK_WAIT, NULL) != DD_OK)
        return;
    bpp = d.ddpfPixelFormat.dwRGBBitCount / 8;
    for (y = 0; y < (int) d.dwHeight; y++) {
        BYTE *row = (BYTE *) d.lpSurface + y * d.lPitch;

        for (x = 0; x < (int) d.dwWidth; x++) {
            DWORD c = pcol((x * 4 + seed) & 255, (y * 4) & 255, ((x ^ y) * 2 + seed * 3) & 255);

            if (((x >> 4) + (y >> 4)) % 5 == 0)
                c = pcol(255, 0, 255); /* the colour key */
            memcpy(row + x * bpp, &c, bpp);
        }
    }
    IDirectDrawSurface_Unlock(s, NULL);
}

typedef void (*ddop)(IDirectDrawSurface *dst, IDirectDrawSurface *src);

static void
op_fill(IDirectDrawSurface *d, IDirectDrawSurface *s)
{
    int i;

    for (i = 0; i < 80; i++)
        fill(d, (i * 37) % 230, (i * 53) % 230, 1 + (i * 7) % 60, 1 + (i * 11) % 50,
             pcol(i * 29 & 255, i * 71 & 255, i * 13 & 255));
}

static void
op_upload(IDirectDrawSurface *d, IDirectDrawSurface *s)
{
    RECT a, b;
    int  i;

    IDirectDrawSurface_Blt(d, NULL, s, NULL, DDBLT_WAIT, NULL);
    for (i = 0; i < 12; i++) {
        SetRect(&a, i * 3, i * 7, i * 3 + 1 + i * 9, i * 7 + 2 + i * 5);
        SetRect(&b, 100 + i * 11, 20 + i * 17, 100 + i * 11 + 1 + i * 9, 20 + i * 17 + 2 + i * 5);
        IDirectDrawSurface_Blt(d, &b, s, &a, DDBLT_WAIT, NULL);
    }
}

static void
op_overlap(IDirectDrawSurface *d, IDirectDrawSurface *s)
{
    static const int off[6][2] = { { 1, 0 }, { 0, 3 }, { -17, 0 }, { 0, -9 }, { 5, 5 }, { -3, -7 } };
    RECT             a, b;
    int              i;

    IDirectDrawSurface_Blt(d, NULL, s, NULL, DDBLT_WAIT, NULL);
    for (i = 0; i < 6; i++) {
        SetRect(&a, 40, 40, 200, 200);
        SetRect(&b, 40 + off[i][0], 40 + off[i][1], 200 + off[i][0], 200 + off[i][1]);
        IDirectDrawSurface_Blt(d, &b, d, &a, DDBLT_WAIT, NULL);
    }
}

static void
op_ckey(IDirectDrawSurface *d, IDirectDrawSurface *s)
{
    DDBLTFX fx;
    RECT    a, b;

    op_fill(d, NULL);
    memset(&fx, 0, sizeof(fx));
    fx.dwSize                         = sizeof(fx);
    fx.ddckSrcColorkey.dwColorSpaceLowValue  = pcol(255, 0, 255);
    fx.ddckSrcColorkey.dwColorSpaceHighValue = pcol(255, 0, 255);
    SetRect(&a, 0, 0, 200, 200);
    SetRect(&b, 30, 20, 230, 220);
    IDirectDrawSurface_Blt(d, &b, s, &a, DDBLT_KEYSRCOVERRIDE | DDBLT_WAIT, &fx);
}

static void
op_mirror(IDirectDrawSurface *d, IDirectDrawSurface *s)
{
    DDBLTFX fx;

    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);
    fx.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
    IDirectDrawSurface_Blt(d, NULL, s, NULL, DDBLT_DDFX | DDBLT_WAIT, &fx);
}

static void
op_stretch(IDirectDrawSurface *d, IDirectDrawSurface *s)
{
    RECT a, b;

    op_fill(d, NULL);
    SetRect(&a, 0, 0, 64, 64);
    SetRect(&b, 0, 0, 192, 160); /* up 3x / 2.5x */
    IDirectDrawSurface_Blt(d, &b, s, &a, DDBLT_WAIT, NULL);
    SetRect(&a, 0, 0, 256, 256);
    SetRect(&b, 192, 160, 256, 224); /* down 4x */
    IDirectDrawSurface_Blt(d, &b, s, &a, DDBLT_WAIT, NULL);
}

static void
op_gdi(IDirectDrawSurface *d, IDirectDrawSurface *s)
{
    HDC   dc;
    HFONT f = (HFONT) GetStockObject(SYSTEM_FONT);
    int   i;

    op_fill(d, NULL);
    if (IDirectDrawSurface_GetDC(d, &dc) != DD_OK)
        return;
    SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (i = 0; i < 10; i++)
        TextOutA(dc, 4, i * 24, "GetDC on a surface", 18);
    for (i = 0; i < 30; i++) {
        MoveToEx(dc, 0, i * 8, NULL);
        LineTo(dc, 255, 255 - i * 8);
    }
    IDirectDrawSurface_ReleaseDC(d, dc);
}

static void
ddcase(const char *name, ddop op, int need, DWORD capbit, int tol, int warn, int failpm,
       IDirectDrawSurface *vsrc, IDirectDrawSurface *ssrc)
{
    IDirectDrawSurface *v = mksurf(DDSCAPS_VIDEOMEMORY, SW, SH);
    IDirectDrawSurface *s = mksurf(DDSCAPS_SYSTEMMEMORY, SW, SH);

    if (need && !(capbit)) {
        ct_skip(name, "not in HAL caps");
    } else if (!v || !s) {
        ct_skip(name, "cannot create surfaces");
    } else {
        fill(v, 0, 0, SW, SH, pcol(0, 0, 0));
        fill(s, 0, 0, SW, SH, pcol(0, 0, 0));
        op(v, vsrc);
        op(s, ssrc);
        read32(v, buf_s);
        read32(s, buf_r);
        ct_grade(name, buf_s, buf_r, SW, SH, tol, warn, failpm, "");
    }
    if (v)
        IDirectDrawSurface_Release(v);
    if (s)
        IDirectDrawSurface_Release(s);
}

/* Blt then Lock straight away: the lock must see the finished blit. */
static void
lock_coherence(void)
{
    IDirectDrawSurface *v = mksurf(DDSCAPS_VIDEOMEMORY, SW, SH);
    DDSURFACEDESC       d;
    int                 i, bad = 0, bpp;
    DWORD               want, got;

    if (!v) {
        ct_skip("LOCKSYNC", "cannot create surface");
        return;
    }
    for (i = 0; i < 300; i++) {
        want = pcol(i * 7 & 255, i * 13 & 255, (i * 5 + 128) & 255);
        fill(v, 0, 0, SW, SH, want);
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        if (IDirectDrawSurface_Lock(v, NULL, &d, DDLOCK_WAIT, NULL) != DD_OK)
            break;
        bpp = d.ddpfPixelFormat.dwRGBBitCount / 8;
        got = 0;
        memcpy(&got, (BYTE *) d.lpSurface + (SH - 1) * d.lPitch + (SW - 1) * bpp, bpp);
        IDirectDrawSurface_Unlock(v, NULL);
        if (got != want)
            bad++;
    }
    IDirectDrawSurface_Release(v);
    if (bad) {
        ct_nfail++;
        ct_log("CASE  LOCKSYNC FAIL %d of 300 locks saw an unfinished fill", bad);
    } else {
        ct_npass++;
        ct_log("CASE  LOCKSYNC PASS 300 fill+lock pairs");
    }
}

static void
fullscreen(HWND hw)
{
    DDSURFACEDESC       d;
    DDSCAPS             bc;
    IDirectDrawSurface *prim = NULL, *back = NULL, *ovl = NULL;
    DWORD               t0, t1, freq = 0, line, maxline = 0;
    int                 i, hz100, n;

    if (IDirectDraw2_SetCooperativeLevel(dd, hw, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN) != DD_OK ||
        IDirectDraw2_SetDisplayMode(dd, 640, 480, 16, 0, 0) != DD_OK) {
        ct_skip("FLIP", "cannot enter 640x480x16 full screen");
        return;
    }
    memset(&d, 0, sizeof(d));
    d.dwSize            = sizeof(d);
    d.dwFlags           = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    d.ddsCaps.dwCaps    = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    d.dwBackBufferCount = 1;
    if (IDirectDraw2_CreateSurface(dd, &d, &prim, NULL) != DD_OK) {
        ct_skip("FLIP", "no flipping primary");
        goto out;
    }
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.dwSize = sizeof(pdesc);
    IDirectDrawSurface_GetSurfaceDesc(prim, &pdesc);
    bc.dwCaps = DDSCAPS_BACKBUFFER;
    IDirectDrawSurface_GetAttachedSurface(prim, &bc, &back);
    IDirectDraw2_GetMonitorFrequency(dd, &freq);

    /* Flip rate: each flip waits for a vertical blank. */
    for (i = 0; i < 8; i++)
        IDirectDrawSurface_Flip(prim, NULL, DDFLIP_WAIT);
    t0 = GetTickCount();
    for (i = 0; i < 120; i++) {
        fill(back, 0, 0, 640, 480, (i & 1) ? pcol(0, 0, 160) : pcol(160, 0, 0));
        fill(back, (i * 5) % 600, 200, 40, 80, pcol(255, 255, 255));
        IDirectDrawSurface_Flip(prim, NULL, DDFLIP_WAIT);
    }
    t1    = GetTickCount();
    hz100 = (t1 - t0) ? (int) (120 * 100000 / (t1 - t0)) : 0;
    if (freq < 40 || freq > 200) /* drivers often answer nonsense */
        freq = 0;
    if (hz100 < 4000 || hz100 > 16000 || (freq && (hz100 < (int) freq * 85 || hz100 > (int) freq * 115))) {
        ct_nfail++;
        ct_log("CASE  FLIP FAIL %d.%02d flips/s, monitor %d Hz", hz100 / 100, hz100 % 100, freq);
    } else {
        ct_npass++;
        ct_log("CASE  FLIP PASS %d.%02d flips/s, monitor %d Hz", hz100 / 100, hz100 % 100, freq);
    }

    /* WaitForVerticalBlank rate and the scan line counter. */
    t0 = GetTickCount();
    for (i = 0; i < 60; i++)
        IDirectDraw2_WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL);
    t1    = GetTickCount();
    hz100 = (t1 - t0) ? (int) (60 * 100000 / (t1 - t0)) : 0;
    /* Sample the scan line for 300 ms (several frames), a few at a time. */
    t0 = GetTickCount();
    for (i = 0, n = 0; GetTickCount() - t0 < 300; i++) {
        if (IDirectDraw2_GetScanLine(dd, &line) == DD_OK) {
            n++;
            if (line > maxline)
                maxline = line;
        }
        if ((i & 15) == 15)
            Sleep(1);
    }
    if (hz100 < 4000 || hz100 > 16000 || (n && maxline < 400)) {
        ct_nfail++;
        ct_log("CASE  VBLANK FAIL %d.%02d vblanks/s; scan line read %d of %d times, max %d", hz100 / 100,
               hz100 % 100, n, i, maxline);
    } else {
        ct_npass++;
        ct_log("CASE  VBLANK PASS %d.%02d vblanks/s; scan line read %d of %d times, max %d", hz100 / 100,
               hz100 % 100, n, i, maxline);
    }

    /* Overlay: YUY2 or UYVY colour bars over a key-coloured hole. */
    if (!(hal.dwCaps & DDCAPS_OVERLAY)) {
        ct_skip("OVERLAY", "no overlay in HAL caps");
        goto out;
    }
    {
        static const DWORD fcc[2]   = { MAKEFOURCC('Y', 'U', 'Y', '2'), MAKEFOURCC('U', 'Y', 'V', 'Y') };
        static const BYTE  bars[8][3] = { { 235, 128, 128 }, { 210, 16, 146 }, { 170, 166, 16 },
                                          { 145, 54, 34 }, { 106, 202, 222 }, { 81, 90, 240 },
                                          { 41, 240, 110 }, { 16, 128, 128 } };
        DDOVERLAYFX        ofx;
        RECT               dr;
        int                k, x, y, ok = 0;

        for (k = 0; k < 2 && !ovl; k++) {
            memset(&d, 0, sizeof(d));
            d.dwSize                 = sizeof(d);
            d.dwFlags                = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            d.ddsCaps.dwCaps         = DDSCAPS_OVERLAY | DDSCAPS_VIDEOMEMORY;
            d.dwWidth                = 320;
            d.dwHeight               = 240;
            d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            d.ddpfPixelFormat.dwFlags  = DDPF_FOURCC;
            d.ddpfPixelFormat.dwFourCC = fcc[k];
            if (IDirectDraw2_CreateSurface(dd, &d, &ovl, NULL) != DD_OK)
                ovl = NULL;
        }
        /* The loop leaves k one past the format that worked: 1 = YUY2. */
        if (!ovl) {
            ct_skip("OVERLAY", "no YUY2/UYVY overlay surface");
            goto out;
        }
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        if (IDirectDrawSurface_Lock(ovl, NULL, &d, DDLOCK_WAIT, NULL) == DD_OK) {
            for (y = 0; y < 240; y++)
                for (x = 0; x < 320; x += 2) {
                    const BYTE *b = bars[x / 40];
                    BYTE       *p = (BYTE *) d.lpSurface + y * d.lPitch + x * 2;

                    if (k == 1) { /* YUY2 */
                        p[0] = b[0]; p[1] = b[1]; p[2] = b[0]; p[3] = b[2];
                    } else {      /* UYVY */
                        p[0] = b[1]; p[1] = b[0]; p[2] = b[2]; p[3] = b[0];
                    }
                }
            IDirectDrawSurface_Unlock(ovl, NULL);
        }
        fill(prim, 0, 0, 640, 480, pcol(0, 80, 0));
        fill(prim, 100, 100, 440, 280, pcol(255, 0, 255));
        memset(&ofx, 0, sizeof(ofx));
        ofx.dwSize = sizeof(ofx);
        ofx.dckDestColorkey.dwColorSpaceLowValue  = pcol(255, 0, 255);
        ofx.dckDestColorkey.dwColorSpaceHighValue = pcol(255, 0, 255);
        SetRect(&dr, 80, 80, 560, 400); /* 1.5x, larger than the hole */
        ok = IDirectDrawSurface_UpdateOverlay(ovl, NULL, prim, &dr,
                                              DDOVER_SHOW | ((hal.dwCKeyCaps & DDCKEYCAPS_DESTOVERLAY) ? DDOVER_KEYDESTOVERRIDE : 0),
                                              &ofx) == DD_OK;
        ct_log("CASE  OVERLAY VISUAL %s %s: 8 colour bars (white..black) scaled 1.5x, %s", k == 1 ? "YUY2" : "UYVY",
               ok ? "shown" : "UpdateOverlay failed",
               (hal.dwCKeyCaps & DDCKEYCAPS_DESTOVERLAY) ? "visible only inside the magenta hole (dest key)"
                                                         : "no dest key (fills 80,80-560,400)");
        ct_pump(quick ? 1500 : 5000);
        IDirectDrawSurface_UpdateOverlay(ovl, NULL, prim, NULL, DDOVER_HIDE, NULL);
    }
out:
    if (ovl)
        IDirectDrawSurface_Release(ovl);
    if (prim)
        IDirectDrawSurface_Release(prim);
    IDirectDraw2_RestoreDisplayMode(dd);
    IDirectDraw2_SetCooperativeLevel(dd, hw, DDSCL_NORMAL);
}

void __stdcall
entry(void)
{
    IDirectDraw        *dd1;
    IDirectDrawSurface *prim, *vsrc, *ssrc;
    DDSURFACEDESC       d;
    HWND                hw;
    DWORD               codes[64], n = 64, i;

    ct_open("DDCHK");
    quick = ct_flag("/Q");
    pDDCreate = (HRESULT(WINAPI *)(GUID *, LPDIRECTDRAW *, IUnknown *)) GetProcAddress(LoadLibraryA("DDRAW.DLL"),
                                                                                    "DirectDrawCreate");
    if (!pDDCreate || pDDCreate(NULL, &dd1, NULL) != DD_OK ||
        IDirectDraw_QueryInterface(dd1, &IID_IDirectDraw2, (void **) &dd) != DD_OK) {
        ct_skip("ALL", "DirectDraw is not installed or DirectDrawCreate failed");
        ct_close();
        ExitProcess(2);
    }
    memset(&hal, 0, sizeof(hal));
    memset(&hel, 0, sizeof(hel));
    hal.dwSize = hel.dwSize = sizeof(DDCAPS);
    IDirectDraw2_GetCaps(dd, &hal, &hel);
    log_caps("hal", &hal);
    log_caps("hel", &hel);
    if (IDirectDraw2_GetFourCCCodes(dd, &n, codes) == DD_OK)
        for (i = 0; i < n && i < 64; i++)
            ct_log("INFO  fourcc %c%c%c%c", (char) codes[i], (char) (codes[i] >> 8), (char) (codes[i] >> 16),
                   (char) (codes[i] >> 24));
    IDirectDraw2_EnumDisplayModes(dd, DDEDM_REFRESHRATES, NULL, NULL, mode_cb);

    hw = ct_window(320, 200);
    IDirectDraw2_SetCooperativeLevel(dd, hw, DDSCL_NORMAL);
    memset(&d, 0, sizeof(d));
    d.dwSize         = sizeof(d);
    d.dwFlags        = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDraw2_CreateSurface(dd, &d, &prim, NULL);
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.dwSize = sizeof(pdesc);
    IDirectDrawSurface_GetSurfaceDesc(prim, &pdesc);
    ct_log("INFO  primary %d bpp masks %08x %08x %08x", pdesc.ddpfPixelFormat.dwRGBBitCount,
           pdesc.ddpfPixelFormat.dwRBitMask, pdesc.ddpfPixelFormat.dwGBitMask, pdesc.ddpfPixelFormat.dwBBitMask);
    buf_s = (DWORD *) GlobalAlloc(GPTR, SW * SH * 4);
    buf_r = (DWORD *) GlobalAlloc(GPTR, SW * SH * 4);

    /* Sources: same picture in video and system memory. */
    vsrc = mksurf(DDSCAPS_VIDEOMEMORY, SW, SH);
    ssrc = mksurf(DDSCAPS_SYSTEMMEMORY, SW, SH);
    if (vsrc && ssrc) {
        cpu_picture(vsrc, 1);
        cpu_picture(ssrc, 1);
        ddcase("COLORFILL", op_fill, 1, hal.dwCaps & DDCAPS_BLTCOLORFILL, 0, 0, 0, NULL, NULL);
        ddcase("UPLOAD", op_upload, 1, hal.dwCaps & DDCAPS_BLT, 0, 0, 0, ssrc, ssrc);
        ddcase("VIDBLT", op_upload, 1, hal.dwCaps & DDCAPS_BLT, 0, 0, 0, vsrc, ssrc);
        ddcase("OVERLAP", op_overlap, 1, hal.dwCaps & DDCAPS_BLT, 0, 0, 0, vsrc, ssrc);
        ddcase("SRCKEY", op_ckey, 1, hal.dwCKeyCaps & DDCKEYCAPS_SRCBLT, 0, 0, 0, vsrc, ssrc);
        ddcase("MIRROR", op_mirror, 1, hal.dwFXCaps & DDFXCAPS_BLTMIRRORLEFTRIGHT, 0, 0, 0, vsrc, ssrc);
        /* Hardware may filter a stretch; judge it loosely. */
        ddcase("STRETCH", op_stretch, 1, hal.dwCaps & DDCAPS_BLTSTRETCH, 48, 20, 150, vsrc, ssrc);
        ddcase("GETDC", op_gdi, 0, 0, 0, 0, 0, NULL, NULL);
        lock_coherence();
    } else
        ct_skip("BLITS", "cannot create source surfaces");
    if (vsrc)
        IDirectDrawSurface_Release(vsrc);
    if (ssrc)
        IDirectDrawSurface_Release(ssrc);
    IDirectDrawSurface_Release(prim);
    if (!ct_flag("/NOFS"))
        fullscreen(hw);
    IDirectDraw2_Release(dd);
    ct_close();
    ExitProcess(ct_nfail ? 1 : 0);
}
