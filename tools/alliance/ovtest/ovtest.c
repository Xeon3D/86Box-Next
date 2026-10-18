/* ovtest -- a DirectDraw YUY2 overlay of colour bars (320x240, a grey stripe
   every 40 lines) shown in a window's client area, clipped to the window by
   a clipper on the primary surface and refreshed with UpdateOverlay five
   times a second, so covering the window makes the driver clip the
   overlay (the window opens where the Start menu covers its lower left).
   The title bar shows the last results. Build (in WSL):
   i686-w64-mingw32-gcc -O2 -mwindows -nostdlib -fno-builtin -Wl,-e,_entry@0
     -o OVTEST.EXE ovtest.c -lddraw -lgdi32 -luser32 -lkernel32 */
#include <windows.h>
#include <ddraw.h>
/* No C runtime: Windows 98's msvcrt lacks what mingw-w64's start-up needs. */
void *
memset(void *d, int c, unsigned int n)
{
    unsigned char *p = d;

    while (n--)
        *p++ = (unsigned char) c;
    return d;
}

#define SRC_W 320
#define SRC_H 240

static LPDIRECTDRAW        dd;
static LPDIRECTDRAWSURFACE primary;
static LPDIRECTDRAWSURFACE overlay;
static LPDIRECTDRAWCLIPPER clipper;
static HRESULT             hr_create = 1;
static HRESULT             hr_update = 1;

/* white, yellow, cyan, green, magenta, red, blue, black (BT.601) */
static const unsigned char bars[8][3] = {
    { 235, 128, 128 }, { 210, 16, 146 }, { 170, 166, 16 }, { 145, 54, 34 },
    { 106, 202, 222 }, { 81, 90, 240 },  { 41, 240, 110 }, { 16, 128, 128 }
};

static void
fill(void)
{
    DDSURFACEDESC sd;

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    if (IDirectDrawSurface_Lock(overlay, NULL, &sd, DDLOCK_WAIT, NULL) != DD_OK)
        return;
    for (int y = 0; y < SRC_H; y++) {
        unsigned char *p = (unsigned char *) sd.lpSurface + y * sd.lPitch;

        for (int x = 0; x < SRC_W; x += 2) {
            const unsigned char *c = bars[(x * 8) / SRC_W];
            int                  yy = ((y % 40) < 4) ? 128 : c[0];
            int                  u  = ((y % 40) < 4) ? 128 : c[1];
            int                  v  = ((y % 40) < 4) ? 128 : c[2];

            p[x * 2]     = yy;
            p[x * 2 + 1] = u;
            p[x * 2 + 2] = yy;
            p[x * 2 + 3] = v;
        }
    }
    IDirectDrawSurface_Unlock(overlay, NULL);
}

static void
setup(HWND w)
{
    DDSURFACEDESC sd;

    if ((hr_create = DirectDrawCreate(NULL, &dd, NULL)) != DD_OK)
        return;
    IDirectDraw_SetCooperativeLevel(dd, w, DDSCL_NORMAL);
    memset(&sd, 0, sizeof(sd));
    sd.dwSize         = sizeof(sd);
    sd.dwFlags        = DDSD_CAPS;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    if ((hr_create = IDirectDraw_CreateSurface(dd, &sd, &primary, NULL)) != DD_OK)
        return;
    if ((hr_create = IDirectDraw_CreateClipper(dd, 0, &clipper, NULL)) != DD_OK)
        return;
    IDirectDrawClipper_SetHWnd(clipper, 0, w);
    IDirectDrawSurface_SetClipper(primary, clipper);

    memset(&sd, 0, sizeof(sd));
    sd.dwSize                    = sizeof(sd);
    sd.dwFlags                   = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps            = DDSCAPS_OVERLAY | DDSCAPS_VIDEOMEMORY;
    sd.dwWidth                   = SRC_W;
    sd.dwHeight                  = SRC_H;
    sd.ddpfPixelFormat.dwSize    = sizeof(DDPIXELFORMAT);
    sd.ddpfPixelFormat.dwFlags   = DDPF_FOURCC;
    sd.ddpfPixelFormat.dwFourCC  = MAKEFOURCC('Y', 'U', 'Y', '2');
    if ((hr_create = IDirectDraw_CreateSurface(dd, &sd, &overlay, NULL)) != DD_OK)
        return;
    fill();
}

static void
show(HWND w)
{
    RECT  src = { 0, 0, SRC_W, SRC_H };
    RECT  dst;
    POINT o = { 0, 0 };
    char  t[96];

    if (overlay) {
        GetClientRect(w, &dst);
        ClientToScreen(w, &o);
        OffsetRect(&dst, o.x, o.y);
        hr_update = IDirectDrawSurface_UpdateOverlay(overlay, &src, primary, &dst, DDOVER_SHOW, NULL);
    }
    wsprintfA(t, "ovtest create %08lx update %08lx", (unsigned long) hr_create, (unsigned long) hr_update);
    SetWindowTextA(w, t);
}

static LRESULT CALLBACK
wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
        case WM_TIMER:
        case WM_MOVE:
        case WM_SIZE:
            show(w);
            return 0;
        case WM_DESTROY:
            if (overlay)
                IDirectDrawSurface_UpdateOverlay(overlay, NULL, primary, NULL, DDOVER_HIDE, NULL);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProc(w, m, wp, lp);
    }
}

int WINAPI
WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int sh)
{
    WNDCLASS wc = { 0 };
    MSG      msg;
    HWND     w;

    (void) prev;
    (void) cmd;
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "ovtest";
    RegisterClass(&wc);
    w = CreateWindow("ovtest", "ovtest", WS_OVERLAPPEDWINDOW, 92, 357, 408, 327, NULL, NULL, inst, NULL);
    ShowWindow(w, sh);
    UpdateWindow(w);
    setup(w);
    show(w);
    SetTimer(w, 1, 200, NULL);
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

void __stdcall
entry(void)
{
    ExitProcess(WinMain(GetModuleHandle(NULL), NULL, NULL, SW_SHOWNORMAL));
}
