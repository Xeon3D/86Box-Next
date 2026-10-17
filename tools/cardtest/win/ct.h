/* ct.h -- shared helpers for the CARDTEST Win32 tools (no C runtime:
   Windows 95/98/NT4's msvcrt lacks what mingw-w64's start-up needs).

   Every tool logs to C:\CTRES\<TOOL>.LOG and saves its pictures as
   C:\CTRES\<TOOL>\<CASE>_S.BMP (the card), _R.BMP (the software
   reference) and _D.BMP (difference, only when they differ). The
   CTRES environment variable can name another folder than C:\CTRES.
   Log lines start with a fixed keyword so the host can parse them:
     INFO  <text>
     CASE  <name> PASS|WARN|FAIL|SKIP|VISUAL <details>
     DONE  pass=N warn=N fail=N skip=N                                  */
#include <windows.h>

void *
memset(void *d, int c, unsigned int n)
{
    unsigned char *p = d;

    while (n--)
        *p++ = (unsigned char) c;
    return d;
}

void *
memcpy(void *d, const void *s, unsigned int n)
{
    unsigned char       *p = d;
    const unsigned char *q = s;

    while (n--)
        *p++ = *q++;
    return d;
}

int
memcmp(const void *a, const void *b, unsigned int n)
{
    const unsigned char *p = a, *q = b;

    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

static HANDLE ct_logf = INVALID_HANDLE_VALUE;
static char   ct_name[16];
static char   ct_dir[MAX_PATH];
static int    ct_npass, ct_nwarn, ct_nfail, ct_nskip;

static void
ct_log(const char *fmt, ...)
{
    char    buf[1100];
    DWORD   n;
    va_list ap;

    va_start(ap, fmt);
    n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    buf[n++] = '\r';
    buf[n++] = '\n';
    if (ct_logf != INVALID_HANDLE_VALUE)
        WriteFile(ct_logf, buf, n, &n, NULL);
}

/* Long strings (extension lists) go out in 1000-byte pieces. */
static void
ct_log_long(const char *tag, const char *s)
{
    char piece[1001];
    int  i;

    while (*s) {
        for (i = 0; i < 1000 && s[i]; i++)
            piece[i] = s[i];
        piece[i] = 0;
        ct_log("INFO  %s %s", tag, piece);
        s += i;
    }
}

/* Results go to C:\CTRES, or to the folder in the CTRES environment
   variable (RUNALL.BAT sets C:\CTRES\<n>BPP for each colour depth). */
static void
ct_open(const char *name)
{
    char           path[MAX_PATH], base[MAX_PATH];
    OSVERSIONINFOA ov;
    HDC            dc;
    int            bpp;

    lstrcpyA(ct_name, name);
    CreateDirectoryA("C:\\CTRES", NULL);
    /* /OUT=folder on the command line wins (start /w on Windows 9x may not
       hand a batch file's environment on), then CTRES, then C:\CTRES. */
    {
        const char *c = GetCommandLineA();
        int         i;

        base[0] = 0;
        for (; *c; c++)
            if ((c[0] == '/' || c[0] == '-') && (c[1] | 32) == 'o' && (c[2] | 32) == 'u' && (c[3] | 32) == 't' &&
                c[4] == '=') {
                for (c += 5, i = 0; *c && *c != ' ' && i < MAX_PATH - 1; c++)
                    base[i++] = *c;
                base[i] = 0;
                break;
            }
    }
    if (!base[0] && (!GetEnvironmentVariableA("CTRES", base, sizeof(base)) || !base[0]))
        lstrcpyA(base, "C:\\CTRES");
    CreateDirectoryA(base, NULL);
    wsprintfA(ct_dir, "%s\\%s", base, name);
    CreateDirectoryA(ct_dir, NULL);
    wsprintfA(path, "%s\\%s.LOG", base, name);
    ct_logf = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    ov.dwOSVersionInfoSize = sizeof(ov);
    GetVersionExA(&ov);
    /* GetDeviceCaps works on Windows 95 too (no ENUM_CURRENT_SETTINGS there). */
    dc  = GetDC(NULL);
    bpp = GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES);
    ReleaseDC(NULL, dc);
    ct_log("INFO  %s CARDTEST 1.0", name);
    ct_log("INFO  os %d.%d.%d platform %d", ov.dwMajorVersion, ov.dwMinorVersion,
           ov.dwBuildNumber & 0xffff, ov.dwPlatformId);
    ct_log("INFO  mode %dx%dx%d", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), bpp);
}

static void
ct_close(void)
{
    ct_log("DONE  pass=%d warn=%d fail=%d skip=%d", ct_npass, ct_nwarn, ct_nfail, ct_nskip);
    CloseHandle(ct_logf);
}

/* Write a 32 bpp top-down buffer as a 24 bpp bottom-up BMP. */
static void
ct_save_bmp(const char *cas, const char *suffix, const DWORD *px, int w, int h)
{
    char             path[MAX_PATH];
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    HANDLE           f;
    DWORD            n;
    int              stride = (w * 3 + 3) & ~3, x, y;
    BYTE            *row;

    wsprintfA(path, "%s\\%s_%s.BMP", ct_dir, cas, suffix);
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    fh.bfType    = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize    = fh.bfOffBits + stride * h;
    ih.biSize     = sizeof(ih);
    ih.biWidth    = w;
    ih.biHeight   = h;
    ih.biPlanes   = 1;
    ih.biBitCount = 24;
    WriteFile(f, &fh, sizeof(fh), &n, NULL);
    WriteFile(f, &ih, sizeof(ih), &n, NULL);
    row = (BYTE *) GlobalAlloc(GPTR, stride);
    for (y = h - 1; y >= 0; y--) {
        for (x = 0; x < w; x++) {
            DWORD p        = px[y * w + x];
            row[x * 3]     = (BYTE) p;
            row[x * 3 + 1] = (BYTE) (p >> 8);
            row[x * 3 + 2] = (BYTE) (p >> 16);
        }
        WriteFile(f, row, stride, &n, NULL);
    }
    GlobalFree(row);
    CloseHandle(f);
}

/* Compare two 32 bpp (xRGB) buffers. A pixel is "bad" when any channel
   differs by more than tol. Returns the number of bad pixels; *maxd gets
   the largest channel difference, *mse100 the mean squared error x100. */
static int
ct_compare(const DWORD *a, const DWORD *b, int n, int tol, int *maxd, int *mse100)
{
    int                bad = 0, i, c, m = 0;
    unsigned long long sq  = 0;

    for (i = 0; i < n; i++) {
        int worst = 0;

        for (c = 0; c < 24; c += 8) {
            int d = (int) ((a[i] >> c) & 0xff) - (int) ((b[i] >> c) & 0xff);

            if (d < 0)
                d = -d;
            sq += (unsigned) (d * d);
            if (d > worst)
                worst = d;
        }
        if (worst > m)
            m = worst;
        if (worst > tol)
            bad++;
    }
    *maxd   = m;
    *mse100 = (int) (sq * 100 / ((unsigned long long) n * 3));
    return bad;
}

/* Grade a card-vs-reference comparison, save the pictures and log it.
   A pixel is "bad" when every reference pixel within `radius` of it
   differs from it by more than `tol` in some channel (radius 0 = an
   exact position match; 1-2 forgives edge rules and half-texel offsets,
   where real hardware and a software rasterizer legitimately differ).
   warn_pm/fail_pm are per-mille of bad pixels; an exact test passes
   tol=0, radius=0, warn_pm=0, fail_pm=0. The strict (radius 0) count is
   logged too. Diff picture: red = bad, orange = differs only within the
   tolerance or position slack, dimmed card picture elsewhere. */
static int
ct_px_close(DWORD a, DWORD b, int tol)
{
    int c, d;

    for (c = 0; c < 24; c += 8) {
        d = (int) ((a >> c) & 0xff) - (int) ((b >> c) & 0xff);
        if (d > tol || d < -tol)
            return 0;
    }
    return 1;
}

static void
ct_grade_r(const char *cas, const DWORD *card, const DWORD *ref, int w, int h, int tol, int radius,
           int warn_pm, int fail_pm, const char *note)
{
    int    maxd, mse, strict, bad = 0, pm, spm, x, y, dx, dy;
    DWORD *diff = (DWORD *) GlobalAlloc(GPTR, w * h * 4);

    strict = ct_compare(card, ref, w * h, tol, &maxd, &mse);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            DWORD c = card[y * w + x];
            int   ok = 0;

            for (dy = -radius; dy <= radius && !ok; dy++)
                for (dx = -radius; dx <= radius && !ok; dx++) {
                    int xx = x + dx, yy = y + dy;

                    if (xx >= 0 && yy >= 0 && xx < w && yy < h && ct_px_close(c, ref[yy * w + xx], tol))
                        ok = 1;
                }
            if (!ok) {
                bad++;
                diff[y * w + x] = 0xff0000;
            } else if ((c ^ ref[y * w + x]) & 0xffffff)
                diff[y * w + x] = 0xffa000;
            else
                diff[y * w + x] = (c >> 2) & 0x3f3f3f;
        }
    pm  = (int) ((long long) bad * 1000 / (w * h));
    spm = (int) ((long long) strict * 1000 / (w * h));
    ct_save_bmp(cas, "S", card, w, h);
    ct_save_bmp(cas, "R", ref, w, h);
    if (maxd)
        ct_save_bmp(cas, "D", diff, w, h);
    GlobalFree(diff);
    if (bad == 0) {
        ct_npass++;
        ct_log("CASE  %s PASS bad=0 strict=%d.%d%% maxd=%d mse100=%d %s", cas, spm / 10, spm % 10, maxd, mse, note);
    } else if (pm >= fail_pm) {
        ct_nfail++;
        ct_log("CASE  %s FAIL bad=%d (%d.%d%%) strict=%d.%d%% maxd=%d mse100=%d %s", cas, bad, pm / 10, pm % 10,
               spm / 10, spm % 10, maxd, mse, note);
    } else if (pm >= warn_pm) {
        ct_nwarn++;
        ct_log("CASE  %s WARN bad=%d (%d.%d%%) strict=%d.%d%% maxd=%d mse100=%d %s", cas, bad, pm / 10, pm % 10,
               spm / 10, spm % 10, maxd, mse, note);
    } else {
        ct_npass++;
        ct_log("CASE  %s PASS bad=%d (%d.%d%%) strict=%d.%d%% maxd=%d mse100=%d %s", cas, bad, pm / 10, pm % 10,
               spm / 10, spm % 10, maxd, mse, note);
    }
}

static void
ct_grade(const char *cas, const DWORD *card, const DWORD *ref, int w, int h, int tol, int warn_pm, int fail_pm,
         const char *note)
{
    ct_grade_r(cas, card, ref, w, h, tol, 0, warn_pm, fail_pm, note);
}

static void
ct_skip(const char *cas, const char *why)
{
    ct_nskip++;
    ct_log("CASE  %s SKIP %s", cas, why);
}

/* Command line: returns non-zero when the switch (e.g. "/Q") is present. */
static int
ct_flag(const char *sw)
{
    const char *c = GetCommandLineA();
    int         n = lstrlenA(sw), i;

    for (; *c; c++) {
        for (i = 0; i < n; i++) {
            char a = c[i], b = sw[i];

            if (a >= 'a' && a <= 'z')
                a -= 32;
            if (a != b)
                break;
        }
        if (i == n)
            return 1;
    }
    return 0;
}

/* Pump messages for ms milliseconds (keeps the window painted and lets
   a visual case stay on screen). */
static void
ct_pump(int ms)
{
    DWORD end = GetTickCount() + ms;
    MSG   m;

    do {
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        Sleep(10);
    } while ((int) (end - GetTickCount()) > 0);
}

static LRESULT CALLBACK
ct_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_SETCURSOR) {
        SetCursor(NULL);
        return TRUE;
    }
    if (m == WM_ERASEBKGND)
        return 1;
    return DefWindowProcA(h, m, w, l);
}

/* A borderless topmost window at the screen's top-left; the mouse
   pointer is parked in the far corner so it never lands in a capture. */
static HWND
ct_window(int w, int h)
{
    WNDCLASSA wc;
    HWND      hw;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = ct_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "CARDTEST";
    wc.style         = CS_OWNDC;
    RegisterClassA(&wc);
    hw = CreateWindowExA(WS_EX_TOPMOST, "CARDTEST", ct_name, WS_POPUP | WS_VISIBLE, 0, 0, w, h,
                         NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(hw);
    SetCursorPos(GetSystemMetrics(SM_CXSCREEN) - 1, GetSystemMetrics(SM_CYSCREEN) - 1);
    ct_pump(300);
    return hw;
}
