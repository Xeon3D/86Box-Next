/* depth -- colour depth helper for RUNALL.BAT (console program, so
   command.com waits for it and sees its errorlevel on Windows 9x).

     DEPTH /LIST            every display mode the driver offers (* = current)
     DEPTH /SET n           switch to n bpp at the current resolution
     DEPTH /SAVE file       remember the current width, height and depth
     DEPTH /RESTORE file    switch back to what /SAVE remembered
     DEPTH /SHUTDOWN        shut Windows down (and power off if it can)

   Errorlevel: 0 done, 1 no such mode / failed, 2 the change needs a
   restart (Windows 95 without dynamic colour depth). Each /SET and
   /RESTORE also appends a line to C:\CTRES\DEPTH.LOG. */
#include "ct.h"

static HANDLE out;

static void
say(const char *fmt, ...)
{
    char    buf[512];
    DWORD   n;
    va_list ap;

    va_start(ap, fmt);
    n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    buf[n++] = '\r';
    buf[n++] = '\n';
    WriteFile(out, buf, n, &n, NULL);
}

static void
logline(const char *fmt, ...)
{
    char    buf[512];
    DWORD   n;
    va_list ap;
    HANDLE  f;

    va_start(ap, fmt);
    n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    buf[n++] = '\r';
    buf[n++] = '\n';
    CreateDirectoryA("C:\\CTRES", NULL);
    f = CreateFileA("C:\\CTRES\\DEPTH.LOG", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        SetFilePointer(f, 0, NULL, FILE_END);
        WriteFile(f, buf, n, &n, NULL);
        CloseHandle(f);
    }
}

static int
cur_bpp(void)
{
    HDC dc = GetDC(NULL);
    int b  = GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES);

    ReleaseDC(NULL, dc);
    return b;
}

static int
atoi_(const char *s)
{
    int v = 0;

    while (*s == ' ')
        s++;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + *s++ - '0';
    return v;
}

/* Next argument after p (spaces separate, quotes group); returns the
   position after it, or NULL at the end. */
static const char *
arg(const char *p, char *buf, int size)
{
    int n = 0, q = 0;

    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p)
        return NULL;
    while (*p && (q || (*p != ' ' && *p != '\t'))) {
        if (*p == '"')
            q = !q;
        else if (n < size - 1)
            buf[n++] = *p;
        p++;
    }
    buf[n] = 0;
    return p;
}

static int
set_mode(int w, int h, int bpp, const char *why)
{
    DEVMODEA dm;
    int      i, found = 0;
    LONG     r;

    if (cur_bpp() == bpp && GetSystemMetrics(SM_CXSCREEN) == w && GetSystemMetrics(SM_CYSCREEN) == h) {
        say("already %dx%dx%d", w, h, bpp);
        logline("DEPTH %d OK %dx%d (already) %s", bpp, w, h, why);
        return 0;
    }
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    for (i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++)
        if ((int) dm.dmPelsWidth == w && (int) dm.dmPelsHeight == h && (int) dm.dmBitsPerPel == bpp) {
            found = 1;
            break;
        }
    if (!found) {
        say("no %dx%dx%d mode", w, h, bpp);
        logline("DEPTH %d NOMODE %dx%d %s", bpp, w, h, why);
        return 1;
    }
    memset(&dm, 0, sizeof(dm));
    dm.dmSize       = sizeof(dm);
    dm.dmFields     = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
    dm.dmPelsWidth  = w;
    dm.dmPelsHeight = h;
    dm.dmBitsPerPel = bpp;
    r               = ChangeDisplaySettingsA(&dm, 0);
    if (r == DISP_CHANGE_RESTART) {
        say("%d bpp needs a restart", bpp);
        logline("DEPTH %d RESTART %dx%d %s", bpp, w, h, why);
        return 2;
    }
    if (r != DISP_CHANGE_SUCCESSFUL) {
        say("ChangeDisplaySettings failed (%d)", (int) r);
        logline("DEPTH %d FAILED %dx%d code %d %s", bpp, w, h, (int) r, why);
        return 1;
    }
    Sleep(3000); /* let the driver and the desktop settle */
    if (cur_bpp() != bpp) {
        say("switched, but the screen reports %d bpp", cur_bpp());
        logline("DEPTH %d FAILED %dx%d now %d bpp %s", bpp, w, h, cur_bpp(), why);
        return 1;
    }
    say("now %dx%dx%d", w, h, bpp);
    logline("DEPTH %d OK %dx%d %s", bpp, w, h, why);
    return 0;
}

static void
list_modes(void)
{
    DEVMODEA dm;
    int      i, w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN), b = cur_bpp();
    DWORD    lw = 0, lh = 0, lb = 0, lf = 0;

    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    for (i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++) {
        if (dm.dmPelsWidth == lw && dm.dmPelsHeight == lh && dm.dmBitsPerPel == lb && dm.dmDisplayFrequency == lf)
            continue;
        lw = dm.dmPelsWidth;
        lh = dm.dmPelsHeight;
        lb = dm.dmBitsPerPel;
        lf = dm.dmDisplayFrequency;
        say("%c %dx%dx%d %dHz", ((int) lw == w && (int) lh == h && (int) lb == b) ? '*' : ' ', lw, lh, lb, lf);
    }
}

static int
save(const char *file)
{
    char   buf[64];
    DWORD  n;
    HANDLE f = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (f == INVALID_HANDLE_VALUE)
        return 1;
    n = wsprintfA(buf, "%d %d %d\r\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), cur_bpp());
    WriteFile(f, buf, n, &n, NULL);
    CloseHandle(f);
    say("saved %s", buf);
    return 0;
}

static int
restore(const char *file)
{
    char   buf[64];
    DWORD  n = 0;
    int    w, h, b;
    char  *p;
    HANDLE f = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (f == INVALID_HANDLE_VALUE)
        return 1;
    ReadFile(f, buf, sizeof(buf) - 1, &n, NULL);
    CloseHandle(f);
    buf[n] = 0;
    p      = buf;
    w      = atoi_(p);
    while (*p && *p != ' ')
        p++;
    h = atoi_(p);
    while (*p == ' ')
        p++;
    while (*p && *p != ' ')
        p++;
    b = atoi_(p);
    if (!w || !h || !b)
        return 1;
    return set_mode(w, h, b, "(restore)");
}

static int
shutdown_(void)
{
    /* NT needs the shutdown privilege; advapi32 is loaded at run time so
       the program still starts on Windows 95. */
    HMODULE a = LoadLibraryA("ADVAPI32.DLL");

    if (a) {
        BOOL(WINAPI * pOPT)(HANDLE, DWORD, PHANDLE) = (void *) GetProcAddress(a, "OpenProcessToken");
        BOOL(WINAPI * pLPV)(LPCSTR, LPCSTR, PLUID) = (void *) GetProcAddress(a, "LookupPrivilegeValueA");
        BOOL(WINAPI * pATP)(HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD, PTOKEN_PRIVILEGES, PDWORD) =
            (void *) GetProcAddress(a, "AdjustTokenPrivileges");
        HANDLE           tok;
        TOKEN_PRIVILEGES tp;

        if (pOPT && pLPV && pATP && pOPT(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
            tp.PrivilegeCount           = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (pLPV(NULL, "SeShutdownPrivilege", &tp.Privileges[0].Luid))
                pATP(tok, FALSE, &tp, 0, NULL, NULL);
            CloseHandle(tok);
        }
    }
    say("shutting down");
    logline("SHUTDOWN");
    {
        /* Windows 9x: ExitWindowsEx from inside the DOS box running the
           batch file stalls for 10+ minutes (Windows waits on that DOS
           session). Hand the shutdown to a Windows process instead and
           return at once, so the batch file ends first. */
        OSVERSIONINFOA      ov;
        STARTUPINFOA        si;
        PROCESS_INFORMATION pi;
        char                cmd[] = "RUNDLL32.EXE SHELL32.DLL,SHExitWindowsEx 5"; /* shutdown | force */

        ov.dwOSVersionInfoSize = sizeof(ov);
        GetVersionExA(&ov);
        if (ov.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS) {
            memset(&si, 0, sizeof(si));
            si.cb = sizeof(si);
            if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return 0;
            }
        }
    }
    /* FORCE: the DOS box running RUNALL.BAT must not stop the shutdown. */
    return ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF | EWX_FORCE, 0) ? 0 : 1;
}

void __stdcall
entry(void)
{
    char        a1[MAX_PATH], a2[MAX_PATH];
    const char *p = GetCommandLineA();
    int         r = 1;

    out = GetStdHandle(STD_OUTPUT_HANDLE);
    p   = arg(p, a1, sizeof(a1)); /* the program itself */
    if (p)
        p = arg(p, a1, sizeof(a1));
    if (!p) {
        say("DEPTH /LIST | /SET bpp | /SAVE file | /RESTORE file | /SHUTDOWN");
        ExitProcess(1);
    }
    CharUpperA(a1);
    if (!lstrcmpA(a1, "/LIST")) {
        list_modes();
        r = 0;
    } else if (!lstrcmpA(a1, "/SET") && arg(p, a2, sizeof(a2)))
        r = set_mode(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), atoi_(a2), "");
    else if (!lstrcmpA(a1, "/SAVE") && arg(p, a2, sizeof(a2)))
        r = save(a2);
    else if (!lstrcmpA(a1, "/RESTORE") && arg(p, a2, sizeof(a2)))
        r = restore(a2);
    else if (!lstrcmpA(a1, "/SHUTDOWN"))
        r = shutdown_();
    ExitProcess(r);
}
