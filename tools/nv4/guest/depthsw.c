/*
 * depthsw.c -- flip the desktop colour depth back and forth, the way
 * Display Properties does it without a restart.
 *
 *   depthsw [N] [A] [B]
 *
 * N switches (6), alternating between A and B bits per pixel (8, 16),
 * starting with A, then back to the depth it started at. A pause of 5 s
 * after each. Each ChangeDisplaySettings result is appended to
 * C:\DEPTHSW.TXT (0 = done, 1 = needs a restart, negative = failed).
 * Built without the C runtime (build.sh).
 */
#include <windows.h>

static HANDLE out;
static char   buf[256];

void *
memset(void *d, int c, size_t n)
{
    BYTE *p = d;

    while (n--)
        *p++ = (BYTE) c;
    return d;
}

static void
put(const char *s)
{
    DWORD n;

    WriteFile(out, s, lstrlen(s), &n, NULL);
}

/* the next number on the command line, or def */
static int
num(const char **p, int def)
{
    int v = 0, any = 0;

    while (**p == ' ')
        (*p)++;
    while ((**p >= '0') && (**p <= '9')) {
        v = v * 10 + (*(*p)++ - '0');
        any = 1;
    }
    return any ? v : def;
}

static void
set(int i, int bpp)
{
    DEVMODE dm;
    LONG    r;
    HDC     dc;

    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize       = sizeof(dm);
    dm.dmBitsPerPel = bpp;
    dm.dmFields     = DM_BITSPERPEL;
    r  = ChangeDisplaySettings(&dm, 0);
    dc = GetDC(NULL);
    wsprintf(buf, "%d: %d bpp -> %ld, now %d bpp\r\n", i, bpp, r, GetDeviceCaps(dc, BITSPIXEL));
    ReleaseDC(NULL, dc);
    put(buf);
    Sleep(5000);
}

void
entry(void)
{
    const char *p = GetCommandLine();
    int         n, a, b, start, i;
    HDC         dc;

    /* skip the program name */
    if (*p == '"') {
        for (p++; *p && (*p != '"'); p++)
            ;
        if (*p)
            p++;
    } else
        while (*p && (*p != ' '))
            p++;
    n = num(&p, 6);
    a = num(&p, 8);
    b = num(&p, 16);

    out = CreateFile("C:\\DEPTHSW.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    SetFilePointer(out, 0, NULL, FILE_END);
    dc    = GetDC(NULL);
    start = GetDeviceCaps(dc, BITSPIXEL);
    ReleaseDC(NULL, dc);
    wsprintf(buf, "depthsw %d switches %d/%d from %d bpp\r\n", n, a, b, start);
    put(buf);
    for (i = 0; i < n; i++)
        set(i + 1, (i & 1) ? b : a);
    set(0, start);
    put("done\r\n");
    CloseHandle(out);
    ExitProcess(0);
}
