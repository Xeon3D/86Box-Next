/*
 * 86Box Guest Tools -- Windows 3.x agent (GTWIN.EXE), Windows 3.1 and
 * Windows for Workgroups 3.11.
 *
 * Moves the Windows pointer to where the host pointer is, so the emulator
 * needs no mouse capture, and shares clipboard text both ways, like the
 * Win32 agent. It sits as an icon; its window menu turns the features on
 * and off. The channel is described in guest_tools_proto.h; the emulator
 * opens its ports to programs, so there is no driver.
 *
 *   GTWIN /install     copy to the Windows directory, start from WIN.INI
 *   GTWIN /uninstall   stop it and remove it
 *   GTWIN /quiet       no messages
 *
 * Authors: Marcos Alves, <xeon4d@gmail.com>
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <windows.h>
#include <conio.h>
#include <string.h>

#include "guest_tools_proto.h"
#include "guest_tools_version.h"

#define AGENT_TITLE "86Box Guest Tools"
#define AGENT_CLASS "GT86Box"
#define AGENT_EXE   "GTWIN.EXE"
#define SECTION     "86Box Guest Tools"

#define IDI_AGENT   1

/* Window menu commands (below 0xF000, low four bits clear). */
#define ID_POINTER   0x0100
#define ID_CLIPBOARD 0x0110
#define ID_ABOUT     0x0120

#define WM_HOST_CLIP (WM_USER + 1)

#define CLIP_MAX     16000 /* characters: one 64K segment, CR LF included */

static HINSTANCE hinst;
static HWND      hwnd;
static HWND      next_viewer;
static int       opt_pointer   = 1;
static int       opt_clipboard = 1;
static DWORD     host_features;
static WORD      last_x        = 0xffff;
static WORD      last_y        = 0xffff;
static DWORD     last_seq;
static int       have_seq;
static DWORD     last_move;

/* The clipboard text last exchanged with the host (Unicode). */
static HGLOBAL   last_mem;
static WORD      last_chars;

/* Windows-1252 0x80-0x9F; 0xA0-0xFF are Latin-1. */
static const WORD cp1252[32] = {
    0x20ac, 0x003f, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x003f, 0x017d, 0x003f,
    0x003f, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x003f, 0x017e, 0x0178
};

static WORD
from_ansi(BYTE c)
{
    if ((c >= 0x80) && (c < 0xa0))
        return cp1252[c - 0x80];
    return c;
}

static BYTE
to_ansi(WORD u)
{
    int i;

    if ((u < 0x80) || ((u >= 0xa0) && (u < 0x100)))
        return (BYTE) u;
    for (i = 0; i < 32; i++) {
        if ((cp1252[i] == u) && (u != 0x003f))
            return (BYTE) (0x80 + i);
    }
    return '?';
}

/* ---- The channel ------------------------------------------------------- */

static void
put_word(WORD w)
{
    outpw(GT_PORT_DATA, w);
}

static void
put_dword(DWORD d)
{
    outpw(GT_PORT_DATA, LOWORD(d));
    outpw(GT_PORT_DATA, HIWORD(d));
}

static WORD
get_word(void)
{
    return inpw(GT_PORT_DATA);
}

static DWORD
get_dword(void)
{
    WORD lo = inpw(GT_PORT_DATA);
    WORD hi = inpw(GT_PORT_DATA);

    return MAKELONG(lo, hi);
}

static DWORD
command(BYTE cmd)
{
    outp(GT_PORT_CMD, cmd);
    return get_dword();
}

static int
gt_present(void)
{
    return (inpw(GT_PORT_CMD) == 0x3638) && (inpw(GT_PORT_CMD + 2) == 0x5842);
}

static int
gt_hello(void)
{
    put_word(GT_PROTO_VERSION);
    put_word(GT_OS_WIN16);
    put_dword(GT_CAP_POINTER | GT_CAP_CLIPBOARD);
    put_dword(GT_TOOLS_VERSION);
    if (command(GT_CMD_HELLO) != GT_OK)
        return 0;
    get_dword();
    host_features = get_dword();
    return 1;
}

/* ---- Pointer ----------------------------------------------------------- */

static int
pointer_confined(void)
{
    RECT clip;

    GetClipCursor(&clip);
    return (clip.left > 0) || (clip.top > 0) ||
           (clip.right < GetSystemMetrics(SM_CXSCREEN)) ||
           (clip.bottom < GetSystemMetrics(SM_CYSCREEN));
}

/* One POLL; returns non-zero while the host pointer is over the display
   and integration is on, i.e. while it is worth polling quickly. */
static int
poll(void)
{
    DWORD state;
    DWORD flags;
    DWORD seq;
    WORD  x;
    WORD  y;
    int   follow = opt_pointer && !pointer_confined();
    DWORD status;

    state = follow ? GT_STATE_POINTER : 0;
    put_dword(state);
    status = command(GT_CMD_POLL);
    if (status == GT_ERR_NO_AGENT) {
        gt_hello();
        last_x = 0xffff;
        have_seq = 0;
        return 0;
    }
    if (status != GT_OK)
        return 0;

    flags = get_dword();
    x     = get_word();
    y     = get_word();
    seq   = get_dword();

    if (opt_clipboard && (flags & GT_POLL_CLIP) && (!have_seq || (seq != last_seq))) {
        last_seq = seq;
        have_seq = 1;
        PostMessage(hwnd, WM_HOST_CLIP, 0, 0L);
    }

    if (!follow || !(flags & GT_POLL_POINTER) || !(flags & GT_POLL_INSIDE))
        return 0;

    if ((x != last_x) || (y != last_y)) {
        int cx = GetSystemMetrics(SM_CXSCREEN);
        int cy = GetSystemMetrics(SM_CYSCREEN);

        SetCursorPos((int) (((DWORD) x * (DWORD) (cx - 1) + GT_POINTER_MAX / 2) / GT_POINTER_MAX),
                     (int) (((DWORD) y * (DWORD) (cy - 1) + GT_POINTER_MAX / 2) / GT_POINTER_MAX));
        last_x    = x;
        last_y    = y;
        last_move = GetTickCount();
    }
    return 1;
}

/* ---- Clipboard --------------------------------------------------------- */

static int
same_as_last(WORD FAR *text, WORD chars)
{
    WORD FAR *last;
    WORD      i;
    int       same = 1;

    if ((last_mem == NULL) || (chars != last_chars))
        return 0;
    last = (WORD FAR *) GlobalLock(last_mem);
    for (i = 0; i < chars; i++) {
        if (text[i] != last[i]) {
            same = 0;
            break;
        }
    }
    GlobalUnlock(last_mem);
    return same;
}

static void
remember(HGLOBAL mem, WORD chars)
{
    if (last_mem != NULL)
        GlobalFree(last_mem);
    last_mem   = mem;
    last_chars = chars;
}

static void
guest_clipboard_changed(void)
{
    HGLOBAL    clip;
    HGLOBAL    mem;
    BYTE FAR  *src;
    WORD FAR  *text;
    WORD       n = 0;
    WORD       i;

    if (!opt_clipboard || !(host_features & GT_FEAT_CLIP_OUT) || (GetClipboardOwner() == hwnd))
        return;
    if (!OpenClipboard(hwnd))
        return;
    clip = GetClipboardData(CF_TEXT);
    if (clip == NULL) {
        CloseClipboard();
        return;
    }
    mem = GlobalAlloc(GMEM_MOVEABLE, (DWORD) CLIP_MAX * 2);
    if (mem == NULL) {
        CloseClipboard();
        return;
    }
    src  = (BYTE FAR *) GlobalLock(clip);
    text = (WORD FAR *) GlobalLock(mem);
    for (i = 0; src[i] && (n < CLIP_MAX); i++) {
        if ((src[i] == '\r') && (src[i + 1] == '\n'))
            continue;
        text[n++] = from_ansi(src[i]);
        if (i == 0xfffe)
            break;
    }
    GlobalUnlock(clip);
    CloseClipboard();

    if (same_as_last(text, n)) {
        GlobalUnlock(mem);
        GlobalFree(mem);
        return;
    }

    put_dword(GT_CLIP_TEXT);
    put_dword((DWORD) n * 2);
    for (i = 0; i < n; i++)
        put_word(text[i]);
    command(GT_CMD_CLIP_SET);

    GlobalUnlock(mem);
    remember(mem, n);
}

static void
host_clipboard_changed(void)
{
    DWORD      format;
    DWORD      bytes;
    WORD       chars;
    WORD       lines = 0;
    WORD       i;
    WORD       o;
    HGLOBAL    mem;
    HGLOBAL    clip;
    WORD FAR  *text;
    BYTE FAR  *dst;

    if (!opt_clipboard || !(host_features & GT_FEAT_CLIP_IN))
        return;
    if (command(GT_CMD_CLIP_GET) != GT_OK)
        return;
    format = get_dword();
    bytes  = get_dword();
    if ((format != GT_CLIP_TEXT) || (bytes == 0))
        return;
    chars = (bytes / 2 > CLIP_MAX) ? CLIP_MAX : (WORD) (bytes / 2);

    mem = GlobalAlloc(GMEM_MOVEABLE, (DWORD) CLIP_MAX * 2);
    if (mem == NULL)
        return;
    text = (WORD FAR *) GlobalLock(mem);
    for (i = 0; i < chars; i++) {
        text[i] = get_word();
        lines += (text[i] == '\n');
    }

    if (same_as_last(text, chars)) {
        GlobalUnlock(mem);
        GlobalFree(mem);
        return;
    }

    /* LF -> CR LF, Unicode -> Windows-1252. */
    clip = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, (DWORD) chars + lines + 1);
    if (clip != NULL) {
        dst = (BYTE FAR *) GlobalLock(clip);
        for (i = 0, o = 0; i < chars; i++) {
            if (text[i] == '\n')
                dst[o++] = '\r';
            dst[o++] = to_ansi(text[i]);
        }
        dst[o] = 0;
        GlobalUnlock(clip);
        if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            SetClipboardData(CF_TEXT, clip);
            CloseClipboard();
        } else
            GlobalFree(clip);
    }

    GlobalUnlock(mem);
    remember(mem, chars);
}

/* ---- Install ----------------------------------------------------------- */

static void
installed_path(char *path)
{
    GetWindowsDirectory(path, 128);
    if (path[lstrlen(path) - 1] != '\\')
        lstrcat(path, "\\");
    lstrcat(path, AGENT_EXE);
}

static int
copy_file(const char *from, const char *to)
{
    OFSTRUCT of1;
    OFSTRUCT of2;
    HFILE    in;
    HFILE    out;
    HGLOBAL  mem;
    char FAR *buf;
    UINT     n;
    int      ok = 1;

    in = OpenFile(from, &of1, OF_READ | OF_SHARE_DENY_NONE);
    if (in == HFILE_ERROR)
        return 0;
    out = OpenFile(to, &of2, OF_CREATE | OF_WRITE);
    if (out == HFILE_ERROR) {
        _lclose(in);
        return 0;
    }
    mem = GlobalAlloc(GMEM_MOVEABLE, 8192);
    buf = (char FAR *) GlobalLock(mem);
    while ((n = _lread(in, buf, 8192)) > 0) {
        if ((n == HFILE_ERROR) || (_lwrite(out, buf, n) != n)) {
            ok = 0;
            break;
        }
    }
    GlobalUnlock(mem);
    GlobalFree(mem);
    _lclose(out);
    _lclose(in);
    return ok;
}

/* WIN.INI [windows] load= gets (or loses) our name. */
static void
set_load(int add)
{
    char  load[256];
    char  out[256];
    char *p;
    char *word;
    int   found = 0;

    GetProfileString("windows", "load", "", load, sizeof(load));
    out[0] = 0;
    for (p = load; *p;) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        word = p;
        while (*p && (*p != ' '))
            p++;
        if (*p)
            *p++ = 0;
        if ((lstrcmpi(word, AGENT_EXE) == 0) || (lstrlen(word) > 9 &&
            lstrcmpi(word + lstrlen(word) - 10, "\\" AGENT_EXE) == 0)) {
            found = 1;
            if (!add)
                continue;
        }
        if (out[0])
            lstrcat(out, " ");
        lstrcat(out, word);
    }
    if (add && !found) {
        if (out[0])
            lstrcat(out, " ");
        lstrcat(out, AGENT_EXE);
    }
    WriteProfileString("windows", "load", out);
}

static int
install(int quiet)
{
    char self[128];
    char dest[128];

    GetModuleFileName(hinst, self, sizeof(self));
    installed_path(dest);
    if ((lstrcmpi(self, dest) != 0) && !copy_file(self, dest)) {
        if (!quiet)
            MessageBox(NULL, "Could not copy the agent into the Windows directory.",
                       AGENT_TITLE, MB_OK | MB_ICONSTOP);
        return 0;
    }
    set_load(1);
    if (!quiet)
        MessageBox(NULL, AGENT_TITLE " is installed and will start with Windows.",
                   AGENT_TITLE, MB_OK | MB_ICONINFORMATION);
    return 1;
}

static void
uninstall(int quiet)
{
    HWND     other = FindWindow(AGENT_CLASS, NULL);
    char     path[128];
    OFSTRUCT of;

    if (other != NULL)
        SendMessage(other, WM_CLOSE, 0, 0L);
    set_load(0);
    installed_path(path);
    OpenFile(path, &of, OF_DELETE);
    if (!quiet)
        MessageBox(NULL, AGENT_TITLE " is removed.", AGENT_TITLE, MB_OK | MB_ICONINFORMATION);
}

/* ---- Window ------------------------------------------------------------ */

static void
check_menu(void)
{
    HMENU menu = GetSystemMenu(hwnd, FALSE);

    CheckMenuItem(menu, ID_POINTER, MF_BYCOMMAND | (opt_pointer ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_CLIPBOARD, MF_BYCOMMAND | (opt_clipboard ? MF_CHECKED : MF_UNCHECKED));
}

static void
about(void)
{
    char text[400];

    wsprintf(text,
             AGENT_TITLE " " GT_TOOLS_VERSION_STR " for Windows 3.x\n\n"
             "Mouse integration: %s\n"
             "Clipboard, host to guest: %s\n"
             "Clipboard, guest to host: %s\n\n"
             "Set these in the machine's Settings > Other peripherals > 86Box Guest Tools.",
             (LPSTR) ((host_features & GT_FEAT_POINTER) ? "on" : "off"),
             (LPSTR) ((host_features & GT_FEAT_CLIP_IN) ? "on" : "off"),
             (LPSTR) ((host_features & GT_FEAT_CLIP_OUT) ? "on" : "off"));
    MessageBox(hwnd, text, AGENT_TITLE, MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK __export
wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_CREATE:
            hwnd        = wnd;
            next_viewer = SetClipboardViewer(wnd);
            SetTimer(wnd, 1, 50, NULL);
            return 0;

        case WM_QUERYOPEN: /* stay an icon */
            return 0;

        case WM_TIMER:
            poll();
            return 0;

        case WM_DRAWCLIPBOARD:
            guest_clipboard_changed();
            if (next_viewer != NULL)
                SendMessage(next_viewer, msg, wp, lp);
            return 0;

        case WM_CHANGECBCHAIN:
            if ((HWND) wp == next_viewer)
                next_viewer = (HWND) LOWORD(lp);
            else if (next_viewer != NULL)
                SendMessage(next_viewer, msg, wp, lp);
            return 0;

        case WM_HOST_CLIP:
            host_clipboard_changed();
            return 0;

        case WM_SYSCOMMAND:
            switch (wp & 0xfff0) {
                case ID_POINTER:
                    opt_pointer = !opt_pointer;
                    WriteProfileString(SECTION, "Pointer", opt_pointer ? "1" : "0");
                    check_menu();
                    return 0;
                case ID_CLIPBOARD:
                    opt_clipboard = !opt_clipboard;
                    WriteProfileString(SECTION, "Clipboard", opt_clipboard ? "1" : "0");
                    check_menu();
                    return 0;
                case ID_ABOUT:
                    about();
                    return 0;
            }
            break;

        case WM_ENDSESSION:
            if (wp)
                command(GT_CMD_BYE);
            return 0;

        case WM_DESTROY:
            KillTimer(wnd, 1);
            ChangeClipboardChain(wnd, next_viewer);
            command(GT_CMD_BYE);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(wnd, msg, wp, lp);
}

static int
has_switch(LPSTR cmdline, const char *name)
{
    int  n = lstrlen(name);
    char word[16];

    for (; *cmdline; cmdline++) {
        if ((*cmdline == '/') || (*cmdline == '-')) {
            int i;

            for (i = 0; (i < n) && (i < (int) sizeof(word) - 1) && cmdline[1 + i]; i++)
                word[i] = cmdline[1 + i];
            word[i] = 0;
            if ((lstrcmpi(word, name) == 0) && ((cmdline[1 + n] == 0) || (cmdline[1 + n] == ' ')))
                return 1;
        }
    }
    return 0;
}

int PASCAL
WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    int      quiet = has_switch(cmdline, "quiet");
    char     self[128];
    char     dest[128];
    WNDCLASS wc;
    MSG      msg;
    HMENU    menu;

    (void) show; /* it always starts as an icon */
    hinst = inst;

    if (has_switch(cmdline, "uninstall")) {
        uninstall(quiet);
        return 0;
    }
    if (prev != NULL)
        return 0;

    if (!gt_present()) {
        if (!quiet)
            MessageBox(NULL, "This is not an 86Box machine with 86Box Guest Tools turned on\n"
                             "(Settings > Other peripherals).", AGENT_TITLE, MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    GetModuleFileName(inst, self, sizeof(self));
    installed_path(dest);
    if (lstrcmpi(self, dest) != 0) {
        /* Started from the disk: install (or offer to), and keep running
           (Windows 3.x would take the installed copy for a second instance
           of this one). Code and icon are preloaded and fixed (wlink and
           gtwin.rc), so the disk can be ejected. */
        int do_install = has_switch(cmdline, "install");

        if (!do_install && !quiet &&
            (MessageBox(NULL, "Install " AGENT_TITLE " so it starts with Windows?\n\n"
                              "(No runs it this time only.)",
                        AGENT_TITLE, MB_YESNO | MB_ICONQUESTION) == IDYES))
            do_install = 1;
        if (do_install && !install(quiet))
            return 1;
    }

    opt_pointer   = GetProfileInt(SECTION, "Pointer", 1) != 0;
    opt_clipboard = GetProfileInt(SECTION, "Clipboard", 1) != 0;

    if (!gt_hello()) {
        if (!quiet)
            MessageBox(NULL, "The emulator did not answer the agent.", AGENT_TITLE, MB_OK | MB_ICONSTOP);
        return 1;
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = inst;
    wc.hIcon         = LoadIcon(inst, MAKEINTRESOURCE(IDI_AGENT));
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    wc.lpszClassName = AGENT_CLASS;
    if (!RegisterClass(&wc))
        return 1;

    if (CreateWindow(AGENT_CLASS, AGENT_TITLE, WS_OVERLAPPEDWINDOW,
                     CW_USEDEFAULT, CW_USEDEFAULT, 200, 100, NULL, NULL, inst, NULL) == NULL)
        return 1;

    menu = GetSystemMenu(hwnd, FALSE);
    AppendMenu(menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(menu, MF_STRING, ID_POINTER, "&Mouse integration");
    AppendMenu(menu, MF_STRING, ID_CLIPBOARD, "Shared &clipboard");
    AppendMenu(menu, MF_STRING, ID_ABOUT, "&About...");
    check_menu();
    ShowWindow(hwnd, SW_SHOWMINNOACTIVE);

    /* Windows 3.x timers tick every 55 ms: while the host pointer moves over
       the display, poll from the message loop instead (other programs still
       run: PeekMessage yields to them). */
    for (;;) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }
        if ((GetTickCount() - last_move) < 1000) {
            poll();
            continue;
        }
        WaitMessage();
    }

    if (last_mem != NULL)
        GlobalFree(last_mem);
    return msg.wParam;
}
