/*
 * winlist.c -- a Windows 9x probe of what USER and GDI think of the
 * top-level windows, for a full-screen program that draws nothing.
 *
 *   winlist [r] [N] [command line]
 *
 * Starts the command (if any), then every 5 s, N times (12), appends a
 * snapshot to C:\WINLIST.TXT: the screen size and depth, the foreground
 * window, and every top-level window that is visible or iconic: its
 * class, title, styles, window and client rectangles, and the clip box
 * of its window DC (what GDI lets it draw: empty when it is hidden).
 * With r, from the fifth snapshot on a minimized window of the program
 * is restored (ShowWindow SW_RESTORE) after the snapshot.
 * Built without the C runtime (build.sh).
 */
#include <windows.h>

static HANDLE out;
static HWND   iconic; /* a minimized window of the started program */
static DWORD  child;
static char   buf[1024];

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

static BOOL CALLBACK
one(HWND w, LPARAM lp)
{
    char  cls[64], title[64];
    RECT  wr, cr, clip;
    HDC   dc;
    int   kind;
    DWORD style = GetWindowLong(w, GWL_STYLE);

    (void) lp;
    if (!IsWindowVisible(w) && !IsIconic(w))
        return TRUE;
    cls[0] = title[0] = 0;
    GetClassName(w, cls, sizeof(cls));
    GetWindowText(w, title, sizeof(title));
    GetWindowRect(w, &wr);
    GetClientRect(w, &cr);
    dc   = GetDC(w);
    kind = GetClipBox(dc, &clip);
    ReleaseDC(w, dc);
    wsprintf(buf, "  %08x %-16s \"%s\" style %08x ex %08x win %d,%d-%d,%d client %dx%d%s%s clip %d: %d,%d-%d,%d\r\n",
             (DWORD) w, cls, title, style, GetWindowLong(w, GWL_EXSTYLE), wr.left, wr.top, wr.right, wr.bottom,
             cr.right, cr.bottom, IsIconic(w) ? " ICONIC" : "", IsWindowVisible(w) ? "" : " HIDDEN", kind,
             clip.left, clip.top, clip.right, clip.bottom);
    put(buf);
    if (IsIconic(w)) {
        DWORD pid;

        GetWindowThreadProcessId(w, &pid);
        if (pid == child)
            iconic = w;
    }
    return TRUE;
}

static int
number(const char **p)
{
    int n = 0;

    while (**p == ' ')
        (*p)++;
    if ((**p < '0') || (**p > '9'))
        return -1;
    while ((**p >= '0') && (**p <= '9'))
        n = n * 10 + *(*p)++ - '0';
    return n;
}

void
entry(void)
{
    const char         *cmd = GetCommandLine();
    STARTUPINFO         si;
    PROCESS_INFORMATION pi;
    HDC                 sdc;
    int                 n;
    int                 restore = 0;

    /* skip the program name */
    if (*cmd == '"') {
        for (cmd++; *cmd && (*cmd != '"'); cmd++)
            ;
        if (*cmd)
            cmd++;
    } else {
        while (*cmd && (*cmd != ' '))
            cmd++;
    }
    while (*cmd == ' ')
        cmd++;
    if ((cmd[0] == 'r') && (cmd[1] == ' ')) {
        restore = 1;
        cmd += 2;
    }
    n = number(&cmd);
    if (n < 0)
        n = 12;
    while (*cmd == ' ')
        cmd++;

    out = CreateFile("C:\\WINLIST.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    SetFilePointer(out, 0, NULL, FILE_END);
    wsprintf(buf, "winlist: %d snapshots%s, command \"%s\"\r\n", n, restore ? ", restoring" : "", cmd);
    put(buf);
    if (*cmd) {
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (!CreateProcess(NULL, (char *) cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            wsprintf(buf, "CreateProcess failed: %d\r\n", GetLastError());
            put(buf);
        } else
            child = pi.dwProcessId;
    }
    for (int i = 0; i < n; i++) {
        HWND fg;
        char title[64];

        Sleep(5000);
        sdc = GetDC(NULL);
        fg  = GetForegroundWindow();
        title[0] = 0;
        GetWindowText(fg, title, sizeof(title));
        wsprintf(buf, "[%d s] screen %dx%dx%d, foreground %08x \"%s\"\r\n", (i + 1) * 5, GetSystemMetrics(SM_CXSCREEN),
                 GetSystemMetrics(SM_CYSCREEN), GetDeviceCaps(sdc, BITSPIXEL), (DWORD) fg, title);
        ReleaseDC(NULL, sdc);
        put(buf);
        iconic = NULL;
        EnumWindows(one, 0);
        if (restore && (i >= 4) && iconic) {
            wsprintf(buf, "  restoring %08x: %d\r\n", (DWORD) iconic, ShowWindow(iconic, SW_RESTORE));
            put(buf);
        }
        FlushFileBuffers(out);
    }
    CloseHandle(out);
    ExitProcess(0);
}
