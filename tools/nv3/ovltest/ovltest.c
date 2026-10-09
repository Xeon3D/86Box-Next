/* ovltest: DirectDraw overlay test for the NV3 emulation (Win9x, DirectX 5+).
 *
 * Fullscreen 800x600x16; shows a 320x240 YUY2 (or UYVY) overlay with colour bars in three phases,
 * 8 s each: A scaled to 480x360 at (100,100); B 1:1 at (400,250); C scaled to 640x480 at (80,60)
 * with a destination colour key (magenta) painted as a rectangle and a frame on a grey primary.
 * Logs caps and every HRESULT to C:\OVLTEST.TXT.
 *
 * No C runtime: MinGW's prebuilt CRT uses CMOV, which the Pentium MMX lacks.
 * Build (WSL): i686-w64-mingw32-gcc -O2 -march=pentium-mmx -fno-builtin -mwindows -nostdlib -e _entry@0
 *              -o OVLTEST.EXE ovltest.c -lddraw -ldxguid -lgdi32 -luser32 -lkernel32
 */
#include <windows.h>
#include <ddraw.h>

static HANDLE logf;

void *
memset(void *d, int c, size_t n)
{
    volatile BYTE *p = d;
    while (n--)
        *p++ = (BYTE) c;
    return d;
}

static void
LOG(const char *fmt, ...)
{
    char    buf[1024];
    DWORD   n;
    va_list ap;

    va_start(ap, fmt);
    n = (DWORD) wvsprintfA(buf, fmt, ap);
    va_end(ap);
    WriteFile(logf, buf, n, &n, NULL);
    FlushFileBuffers(logf);
}

static LRESULT CALLBACK
wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_ESCAPE)
        PostQuitMessage(0);
    return DefWindowProc(h, m, w, l);
}

static void
fill_primary(LPDIRECTDRAWSURFACE prim, int x0, int y0, int x1, int y1, DWORD color)
{
    DDBLTFX fx;
    RECT    r = { x0, y0, x1, y1 };

    memset(&fx, 0, sizeof(fx));
    fx.dwSize      = sizeof(fx);
    fx.dwFillColor = color;
    IDirectDrawSurface_Blt(prim, &r, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
}

/* 75% colour bars on top, a luma ramp in the middle, a chroma ramp at the bottom */
static void
fill_overlay(LPDIRECTDRAWSURFACE ovl, int uyvy)
{
    static const BYTE bars[8][3] = { /* Y U V */
        { 180, 128, 128 }, { 162, 44, 142 }, { 131, 156, 44 }, { 112, 72, 58 },
        { 84, 184, 198 },  { 65, 100, 212 }, { 35, 212, 114 }, { 16, 128, 128 },
    };
    DDSURFACEDESC sd;
    HRESULT       hr;

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    hr        = IDirectDrawSurface_Lock(ovl, NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
    LOG("Lock overlay: %08lx pitch %ld at %08lx\n", hr, sd.lPitch, (DWORD) sd.lpSurface);
    if (FAILED(hr))
        return;
    for (DWORD y = 0; y < sd.dwHeight; y++) {
        BYTE *p = (BYTE *) sd.lpSurface + y * sd.lPitch;
        for (DWORD x = 0; x < sd.dwWidth; x += 2) {
            BYTE Y0, Y1, U, V;
            if (y < sd.dwHeight / 2) {
                const BYTE *b = bars[x * 8 / sd.dwWidth];
                Y0 = Y1 = b[0];
                U       = b[1];
                V       = b[2];
            } else if (y < sd.dwHeight * 3 / 4) {
                Y0 = (BYTE) (16 + x * 219 / sd.dwWidth);
                Y1 = (BYTE) (16 + (x + 1) * 219 / sd.dwWidth);
                U = V = 128;
            } else {
                Y0 = Y1 = 128;
                U       = (BYTE) (16 + x * 224 / sd.dwWidth);
                V       = (BYTE) (240 - x * 224 / sd.dwWidth);
            }
            if (uyvy) {
                p[x * 2 + 0] = U;
                p[x * 2 + 1] = Y0;
                p[x * 2 + 2] = V;
                p[x * 2 + 3] = Y1;
            } else {
                p[x * 2 + 0] = Y0;
                p[x * 2 + 1] = U;
                p[x * 2 + 2] = Y1;
                p[x * 2 + 3] = V;
            }
        }
    }
    IDirectDrawSurface_Unlock(ovl, NULL);
}

static void
pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG   msg;

    while ((int) (end - GetTickCount()) > 0) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                return;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Sleep(20);
    }
}

int WINAPI
WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    LPDIRECTDRAW        dd;
    LPDIRECTDRAWSURFACE prim = NULL, ovl = NULL;
    DDSURFACEDESC       sd;
    DDCAPS              caps;
    DDOVERLAYFX         ofx;
    HRESULT             hr;
    WNDCLASS            wc;
    HWND                wnd;
    DWORD               codes[32], ncodes = 32;
    int                 uyvy = 0;
    RECT                dst;
    DWORD               grey = 0x4210, magenta = 0x7C1F, green = 0x03E0; /* X1R5G5B5 unless the primary is R5G6B5 */

    (void) prev;
    (void) cmd;
    (void) show;
    logf = CreateFileA("C:\\OVLTEST.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (logf == INVALID_HANDLE_VALUE)
        return 1;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.lpszClassName = "ovltest";
    wc.hCursor       = NULL;
    RegisterClass(&wc);
    wnd = CreateWindowEx(WS_EX_TOPMOST, "ovltest", "ovltest", WS_POPUP | WS_VISIBLE, 0, 0, 800, 600, NULL, NULL, inst, NULL);

    hr = DirectDrawCreate(NULL, &dd, NULL);
    LOG("DirectDrawCreate: %08lx\n", hr);
    if (FAILED(hr))
        return 1;
    hr = IDirectDraw_SetCooperativeLevel(dd, wnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    LOG("SetCooperativeLevel: %08lx\n", hr);
    hr = IDirectDraw_SetDisplayMode(dd, 800, 600, 16);
    LOG("SetDisplayMode 800x600x16: %08lx\n", hr);

    memset(&caps, 0, sizeof(caps));
    caps.dwSize = sizeof(caps);
    hr          = IDirectDraw_GetCaps(dd, &caps, NULL);
    LOG("GetCaps: %08lx caps %08lx ckey %08lx fx %08lx max_ovl %lu cur_ovl %lu stretch %lu..%lu align bndry src %lu dst %lu size src %lu dst %lu\n",
        hr, caps.dwCaps, caps.dwCKeyCaps, caps.dwFXCaps, caps.dwMaxVisibleOverlays, caps.dwCurrVisibleOverlays,
        caps.dwMinOverlayStretch, caps.dwMaxOverlayStretch, caps.dwAlignBoundarySrc, caps.dwAlignBoundaryDest,
        caps.dwAlignSizeSrc, caps.dwAlignSizeDest);
    if (SUCCEEDED(IDirectDraw_GetFourCCCodes(dd, &ncodes, codes))) {
        LOG("FourCC codes (%lu):", ncodes);
        for (DWORD i = 0; i < ncodes && i < 32; i++)
            LOG(" %.4s", (char *) &codes[i]);
        LOG("\n");
    }

    memset(&sd, 0, sizeof(sd));
    sd.dwSize         = sizeof(sd);
    sd.dwFlags        = DDSD_CAPS;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    hr                = IDirectDraw_CreateSurface(dd, &sd, &prim, NULL);
    LOG("CreateSurface primary: %08lx\n", hr);
    if (FAILED(hr))
        goto out;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    IDirectDrawSurface_GetSurfaceDesc(prim, &sd);
    LOG("primary: %lu bpp, masks %08lx %08lx %08lx\n", sd.ddpfPixelFormat.dwRGBBitCount, sd.ddpfPixelFormat.dwRBitMask,
        sd.ddpfPixelFormat.dwGBitMask, sd.ddpfPixelFormat.dwBBitMask);
    if (sd.ddpfPixelFormat.dwGBitMask == 0x07E0) {
        grey    = 0x8410;
        magenta = 0xF81F;
        green   = 0x07E0;
    }

    for (int attempt = 0; attempt < 2 && !ovl; attempt++) {
        memset(&sd, 0, sizeof(sd));
        sd.dwSize                    = sizeof(sd);
        sd.dwFlags                   = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        sd.ddsCaps.dwCaps            = DDSCAPS_OVERLAY | DDSCAPS_VIDEOMEMORY;
        sd.dwWidth                   = 320;
        sd.dwHeight                  = 240;
        sd.ddpfPixelFormat.dwSize    = sizeof(DDPIXELFORMAT);
        sd.ddpfPixelFormat.dwFlags   = DDPF_FOURCC;
        sd.ddpfPixelFormat.dwFourCC  = attempt ? MAKEFOURCC('U', 'Y', 'V', 'Y') : MAKEFOURCC('Y', 'U', 'Y', '2');
        hr                           = IDirectDraw_CreateSurface(dd, &sd, &ovl, NULL);
        LOG("CreateSurface overlay %s: %08lx\n", attempt ? "UYVY" : "YUY2", hr);
        uyvy = attempt;
    }
    if (!ovl)
        goto out;

    fill_primary(prim, 0, 0, 800, 600, grey);
    fill_overlay(ovl, uyvy);

    /* A: scaled 1.5x */
    SetRect(&dst, 100, 100, 100 + 480, 100 + 360);
    hr = IDirectDrawSurface_UpdateOverlay(ovl, NULL, prim, &dst, DDOVER_SHOW, NULL);
    LOG("A UpdateOverlay 480x360 at 100,100: %08lx\n", hr);
    pump(8000);

    /* B: 1:1, moved */
    hr = IDirectDrawSurface_SetOverlayPosition(ovl, 400, 250);
    LOG("B SetOverlayPosition 400,250: %08lx\n", hr);
    SetRect(&dst, 400, 250, 400 + 320, 250 + 240);
    hr = IDirectDrawSurface_UpdateOverlay(ovl, NULL, prim, &dst, DDOVER_SHOW, NULL);
    LOG("B UpdateOverlay 320x240 at 400,250: %08lx\n", hr);
    pump(8000);

    /* C: destination colour key: the overlay shows only where the primary is magenta */
    fill_primary(prim, 0, 0, 800, 600, grey);
    fill_primary(prim, 160, 120, 640, 480, magenta);
    fill_primary(prim, 300, 220, 500, 380, green);
    memset(&ofx, 0, sizeof(ofx));
    ofx.dwSize                              = sizeof(ofx);
    ofx.dckDestColorkey.dwColorSpaceLowValue  = magenta;
    ofx.dckDestColorkey.dwColorSpaceHighValue = magenta;
    SetRect(&dst, 80, 60, 80 + 640, 60 + 480);
    hr = IDirectDrawSurface_UpdateOverlay(ovl, NULL, prim, &dst, DDOVER_SHOW | DDOVER_KEYDESTOVERRIDE, &ofx);
    LOG("C UpdateOverlay 640x480 at 80,60, dest key F81F: %08lx\n", hr);
    pump(8000);

    hr = IDirectDrawSurface_UpdateOverlay(ovl, NULL, prim, NULL, DDOVER_HIDE, NULL);
    LOG("hide: %08lx\n", hr);

out:
    if (ovl)
        IDirectDrawSurface_Release(ovl);
    if (prim)
        IDirectDrawSurface_Release(prim);
    IDirectDraw_RestoreDisplayMode(dd);
    IDirectDraw_Release(dd);
    LOG("done\n");
    CloseHandle(logf);
    return 0;
}

void WINAPI
entry(void)
{
    ExitProcess((UINT) WinMain(GetModuleHandleA(NULL), NULL, NULL, SW_SHOW));
}
