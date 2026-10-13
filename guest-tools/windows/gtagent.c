/*
 * 86Box Guest Tools -- Windows agent (Windows 95 and NT 4.0 onwards).
 *
 * Moves the guest pointer to where the host pointer is, so the emulator
 * needs no mouse capture, and shares clipboard text both ways. It talks to
 * the emulator through two I/O ports (see guest_tools_proto.h), which the
 * emulator opens to user mode, so there is no driver.
 *
 * Built without a C runtime: it imports only from KERNEL32, USER32,
 * ADVAPI32, SHELL32 and WINMM, as Windows 95 has them.
 *
 * Authors: Marcos Alves, <xeon4d@gmail.com>
 *
 *          Copyright 2026 Marcos Alves.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <stdint.h>

#include "guest_tools_proto.h"
#include "gtagent.h"

#define AGENT_VERSION     "1.0"
#define AGENT_TITLE       "86Box Guest Tools"
#define AGENT_CLASS       "86BoxGuestToolsAgent"
#define AGENT_MUTEX       "86BoxGuestToolsAgentMutex"
#define AGENT_EXE         "86BOXGT.EXE"
#define RUN_KEY           "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define SETTINGS_KEY      "Software\\86Box\\Guest Tools"

#define WM_TRAY           (WM_APP + 1)
#define WM_HOST_CLIPBOARD (WM_APP + 2)

#define ID_POINTER        100
#define ID_CLIPBOARD      101
#define ID_ABOUT          102
#define ID_EXIT           103

/* Windows 95's NOTIFYICONDATA, which every later shell takes too. */
typedef struct {
    DWORD cbSize;
    HWND  hWnd;
    UINT  uID;
    UINT  uFlags;
    UINT  uCallbackMessage;
    HICON hIcon;
    CHAR  szTip[64];
} NOTIFYICONDATA_V1;

static HINSTANCE        hinst;
static HWND             hwnd;
static HWND             next_viewer;
static UINT             taskbar_created;
static HANDLE           poll_thread;
static volatile LONG    quitting;
static CRITICAL_SECTION io_lock;
static int              is_nt;
static uint32_t         host_features;

static volatile LONG    opt_pointer   = 1;
static volatile LONG    opt_clipboard = 1;

/* The clipboard text last exchanged with the host, so neither side gets
   its own text back. */
static WCHAR           *last_text;
static DWORD            last_chars;

/* ---- No C runtime ------------------------------------------------------ */

void *
memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = dst;
    const uint8_t *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void *
memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    while (n--)
        *d++ = (uint8_t) c;
    return dst;
}

static void *
mem_alloc(DWORD size)
{
    return HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
}

static void
mem_free(void *p)
{
    if (p != NULL)
        HeapFree(GetProcessHeap(), 0, p);
}

/* ---- The channel ------------------------------------------------------- */

static void
gt_outb(uint16_t port, uint8_t val)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void
gt_outw(uint16_t port, uint16_t val)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}

static void
gt_outl(uint16_t port, uint32_t val)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}

static uint16_t
gt_inw(uint16_t port)
{
    uint16_t val;

    __asm__ __volatile__("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static uint32_t
gt_inl(uint16_t port)
{
    uint32_t val;

    __asm__ __volatile__("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* Run a command on the request written so far; returns the reply status. */
static uint32_t
gt_command(uint8_t cmd)
{
    gt_outb(GT_PORT_CMD, cmd);
    return gt_inl(GT_PORT_DATA);
}

static void
gt_put_bytes(const void *data, DWORD len)
{
    const uint8_t *p = data;

    for (; len >= 4; len -= 4, p += 4)
        gt_outl(GT_PORT_DATA, p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24));
    for (; len > 0; len--, p++)
        gt_outb(GT_PORT_DATA, *p);
}

static void
gt_get_chars(WCHAR *text, DWORD chars)
{
    for (DWORD i = 0; i < chars; i++)
        text[i] = gt_inw(GT_PORT_DATA);
}

static LONG WINAPI
no_channel(EXCEPTION_POINTERS *info)
{
    (void) info;
    MessageBoxA(NULL,
                "86Box Guest Tools are not enabled for this machine.\n\n"
                "Turn on \"86Box Guest Tools\" in the machine's Settings > Other peripherals, "
                "then restart the machine.",
                AGENT_TITLE, MB_OK | MB_ICONINFORMATION);
    ExitProcess(1);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Elsewhere the port reads all ones -- or, under Windows NT, traps, which
   the exception filter turns into the message above. */
static int
gt_present(void)
{
    LPTOP_LEVEL_EXCEPTION_FILTER old = SetUnhandledExceptionFilter(no_channel);
    uint32_t                     magic = gt_inl(GT_PORT_CMD);

    SetUnhandledExceptionFilter(old);
    return magic == GT_MAGIC;
}

static int
gt_hello(void)
{
    uint32_t status;

    EnterCriticalSection(&io_lock);
    gt_outw(GT_PORT_DATA, GT_PROTO_VERSION);
    gt_outw(GT_PORT_DATA, is_nt ? GT_OS_WINNT : GT_OS_WIN9X);
    gt_outl(GT_PORT_DATA, GT_CAP_POINTER | GT_CAP_CLIPBOARD);
    status = gt_command(GT_CMD_HELLO);
    if (status == GT_OK) {
        gt_inl(GT_PORT_DATA); /* protocol version, reserved */
        host_features = gt_inl(GT_PORT_DATA);
    }
    LeaveCriticalSection(&io_lock);

    return status == GT_OK;
}

static void
gt_bye(void)
{
    EnterCriticalSection(&io_lock);
    gt_command(GT_CMD_BYE);
    LeaveCriticalSection(&io_lock);
}

/* ---- Settings ---------------------------------------------------------- */

static DWORD
setting_get(const char *name, DWORD def)
{
    HKEY  key;
    DWORD val  = def;
    DWORD size = sizeof(val);
    DWORD type;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        if ((RegQueryValueExA(key, name, NULL, &type, (BYTE *) &val, &size) != ERROR_SUCCESS) || (type != REG_DWORD))
            val = def;
        RegCloseKey(key);
    }
    return val;
}

static void
setting_set(const char *name, DWORD val)
{
    HKEY  key;
    DWORD disp;

    if (RegCreateKeyExA(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, &disp) == ERROR_SUCCESS) {
        RegSetValueExA(key, name, 0, REG_DWORD, (const BYTE *) &val, sizeof(val));
        RegCloseKey(key);
    }
}

/* ---- Pointer ----------------------------------------------------------- */

/* A program that confines the pointer (a game in a window, say) wants the
   mouse to itself: stop following the host then, so the emulator falls
   back to capturing the mouse. */
static int
pointer_confined(void)
{
    RECT clip;

    if (!GetClipCursor(&clip))
        return 0;
    return (clip.left > 0) || (clip.top > 0) ||
           (clip.right < GetSystemMetrics(SM_CXSCREEN)) ||
           (clip.bottom < GetSystemMetrics(SM_CYSCREEN));
}

static DWORD WINAPI
poll_proc(LPVOID param)
{
    uint32_t last_pos   = 0xffffffff;
    DWORD    last_move  = 0;
    uint32_t last_seq   = 0;
    int      have_seq   = 0;
    uint32_t state      = 0;

    (void) param;
    timeBeginPeriod(1);

    while (!quitting) {
        uint32_t status;
        uint32_t flags = 0;
        uint32_t pos   = 0;
        uint32_t seq   = 0;
        DWORD    now;

        EnterCriticalSection(&io_lock);
        gt_outl(GT_PORT_DATA, state);
        status = gt_command(GT_CMD_POLL);
        if (status == GT_OK) {
            flags = gt_inl(GT_PORT_DATA);
            pos   = gt_inl(GT_PORT_DATA);
            seq   = gt_inl(GT_PORT_DATA);
        }
        LeaveCriticalSection(&io_lock);

        now = GetTickCount();
        if (status == GT_ERR_NO_AGENT) {
            /* The machine was reset under us, or the device came back. */
            gt_hello();
            last_pos = 0xffffffff;
            have_seq = 0;
        } else if (status == GT_OK) {
            int follow = opt_pointer && (flags & GT_POLL_POINTER) && !pointer_confined();

            state = follow ? GT_STATE_POINTER : 0;
            if (follow && (flags & GT_POLL_INSIDE) && (pos != last_pos)) {
                int cx = GetSystemMetrics(SM_CXSCREEN);
                int cy = GetSystemMetrics(SM_CYSCREEN);
                int x  = (int) (((pos & 0xffff) * (uint32_t) (cx - 1) + (GT_POINTER_MAX / 2)) / GT_POINTER_MAX);
                int y  = (int) (((pos >> 16) * (uint32_t) (cy - 1) + (GT_POINTER_MAX / 2)) / GT_POINTER_MAX);

                SetCursorPos(x, y);
                last_pos  = pos;
                last_move = now;
            }
            if (opt_clipboard && (flags & GT_POLL_CLIP) && (!have_seq || (seq != last_seq))) {
                last_seq = seq;
                have_seq = 1;
                PostMessageA(hwnd, WM_HOST_CLIPBOARD, 0, 0);
            }
        }

        /* Quick while the pointer moves, easy on the guest while it rests. */
        Sleep(((now - last_move) < 1000) ? 10 : 40);
    }

    timeEndPeriod(1);
    return 0;
}

/* ---- Clipboard --------------------------------------------------------- */

static int
same_as_last(const WCHAR *text, DWORD chars)
{
    if ((last_text == NULL) || (chars != last_chars))
        return 0;
    for (DWORD i = 0; i < chars; i++) {
        if (text[i] != last_text[i])
            return 0;
    }
    return 1;
}

static void
remember(WCHAR *text, DWORD chars)
{
    mem_free(last_text);
    last_text  = text;
    last_chars = chars;
}

/* The guest's clipboard text, as UTF-16 with LF line ends. */
static WCHAR *
guest_clipboard_text(DWORD *chars)
{
    WCHAR  *text = NULL;
    HGLOBAL mem;
    DWORD   n = 0;

    *chars = 0;
    if (!OpenClipboard(hwnd))
        return NULL;

    if (is_nt && ((mem = GetClipboardData(CF_UNICODETEXT)) != NULL)) {
        const WCHAR *src = GlobalLock(mem);

        if (src != NULL) {
            DWORD max = GlobalSize(mem) / sizeof(WCHAR);

            while ((n < max) && src[n])
                n++;
            text = mem_alloc((n + 1) * sizeof(WCHAR));
            if (text != NULL)
                memcpy(text, src, n * sizeof(WCHAR));
            GlobalUnlock(mem);
        }
    } else if ((mem = GetClipboardData(CF_TEXT)) != NULL) {
        const char *src = GlobalLock(mem);

        if (src != NULL) {
            int len = MultiByteToWideChar(CP_ACP, 0, src, -1, NULL, 0);

            if (len > 0) {
                text = mem_alloc(len * sizeof(WCHAR));
                if (text != NULL)
                    n = MultiByteToWideChar(CP_ACP, 0, src, -1, text, len) - 1;
            }
            GlobalUnlock(mem);
        }
    }
    CloseClipboard();

    if (text == NULL)
        return NULL;

    /* CR LF -> LF */
    {
        DWORD out = 0;

        for (DWORD i = 0; i < n; i++) {
            if ((text[i] == L'\r') && ((i + 1) < n) && (text[i + 1] == L'\n'))
                continue;
            text[out++] = text[i];
        }
        n = out;
    }
    if (n > (GT_CLIP_MAX_BYTES / 2))
        n = GT_CLIP_MAX_BYTES / 2;
    *chars = n;
    return text;
}

static void
guest_clipboard_changed(void)
{
    WCHAR   *text;
    DWORD    chars;
    uint32_t bytes;

    if (!opt_clipboard || !(host_features & GT_FEAT_CLIP_OUT) || (GetClipboardOwner() == hwnd))
        return;

    text = guest_clipboard_text(&chars);
    if ((text == NULL) || same_as_last(text, chars)) {
        mem_free(text);
        return;
    }

    bytes = chars * sizeof(WCHAR);
    EnterCriticalSection(&io_lock);
    gt_outl(GT_PORT_DATA, GT_CLIP_TEXT);
    gt_outl(GT_PORT_DATA, bytes);
    gt_put_bytes(text, bytes);
    gt_command(GT_CMD_CLIP_SET);
    LeaveCriticalSection(&io_lock);

    remember(text, chars);
}

static void
host_clipboard_changed(void)
{
    uint32_t status;
    uint32_t format = GT_CLIP_NONE;
    uint32_t bytes  = 0;
    DWORD    chars  = 0;
    WCHAR   *text   = NULL;
    DWORD    lines  = 0;
    HGLOBAL  mem;

    if (!opt_clipboard || !(host_features & GT_FEAT_CLIP_IN))
        return;

    EnterCriticalSection(&io_lock);
    status = gt_command(GT_CMD_CLIP_GET);
    if (status == GT_OK) {
        format = gt_inl(GT_PORT_DATA);
        bytes  = gt_inl(GT_PORT_DATA);
        if ((format == GT_CLIP_TEXT) && (bytes <= GT_CLIP_MAX_BYTES)) {
            chars = bytes / sizeof(WCHAR);
            text  = mem_alloc((chars + 1) * sizeof(WCHAR));
            if (text != NULL)
                gt_get_chars(text, chars);
        }
    }
    LeaveCriticalSection(&io_lock);

    if ((text == NULL) || same_as_last(text, chars)) {
        mem_free(text);
        return;
    }

    /* LF -> CR LF, and a terminator. */
    for (DWORD i = 0; i < chars; i++)
        lines += (text[i] == L'\n');

    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        if (is_nt) {
            mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, (chars + lines + 1) * sizeof(WCHAR));
            if (mem != NULL) {
                WCHAR *dst = GlobalLock(mem);
                DWORD  o   = 0;

                for (DWORD i = 0; i < chars; i++) {
                    if (text[i] == L'\n')
                        dst[o++] = L'\r';
                    dst[o++] = text[i];
                }
                dst[o] = 0;
                GlobalUnlock(mem);
                if (SetClipboardData(CF_UNICODETEXT, mem) == NULL)
                    GlobalFree(mem);
            }
        } else {
            /* Windows 9x: ANSI text in the guest's code page. */
            WCHAR *crlf = mem_alloc((chars + lines + 1) * sizeof(WCHAR));

            if (crlf != NULL) {
                DWORD o = 0;
                int   len;

                for (DWORD i = 0; i < chars; i++) {
                    if (text[i] == L'\n')
                        crlf[o++] = L'\r';
                    crlf[o++] = text[i];
                }
                crlf[o] = 0;
                len     = WideCharToMultiByte(CP_ACP, 0, crlf, -1, NULL, 0, NULL, NULL);
                mem     = (len > 0) ? GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, len) : NULL;
                if (mem != NULL) {
                    WideCharToMultiByte(CP_ACP, 0, crlf, -1, GlobalLock(mem), len, NULL, NULL);
                    GlobalUnlock(mem);
                    if (SetClipboardData(CF_TEXT, mem) == NULL)
                        GlobalFree(mem);
                }
                mem_free(crlf);
            }
        }
        CloseClipboard();
    }

    remember(text, chars);
}

/* ---- Tray icon --------------------------------------------------------- */

static HICON
agent_icon(int size)
{
    HICON icon = LoadImageA(hinst, MAKEINTRESOURCEA(IDI_AGENT), IMAGE_ICON, size, size, 0);

    return icon ? icon : LoadIconA(NULL, (LPCSTR) IDI_APPLICATION);
}

static void
tray_icon(DWORD message)
{
    NOTIFYICONDATA_V1 nid;

    memset(&nid, 0, sizeof(nid));
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon            = agent_icon(16);
    lstrcpynA(nid.szTip, AGENT_TITLE, sizeof(nid.szTip));
    Shell_NotifyIconA(message, (PNOTIFYICONDATAA) &nid);
}

static void
tray_menu(void)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;

    AppendMenuA(menu, MF_STRING | (opt_pointer ? MF_CHECKED : 0) |
                ((host_features & GT_FEAT_POINTER) ? 0 : MF_GRAYED), ID_POINTER, "&Mouse integration");
    AppendMenuA(menu, MF_STRING | (opt_clipboard ? MF_CHECKED : 0) |
                ((host_features & (GT_FEAT_CLIP_IN | GT_FEAT_CLIP_OUT)) ? 0 : MF_GRAYED), ID_CLIPBOARD, "Shared &clipboard");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, ID_ABOUT, "&About...");
    AppendMenuA(menu, MF_STRING, ID_EXIT, "E&xit");

    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageA(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void
about(void)
{
    char text[512];

    wsprintfA(text,
              AGENT_TITLE " " AGENT_VERSION "\n\n"
              "Mouse integration: %s\n"
              "Clipboard, host to guest: %s\n"
              "Clipboard, guest to host: %s\n\n"
              "Set these in the machine's Settings > Other peripherals > 86Box Guest Tools.",
              (host_features & GT_FEAT_POINTER) ? "on" : "off",
              (host_features & GT_FEAT_CLIP_IN) ? "on" : "off",
              (host_features & GT_FEAT_CLIP_OUT) ? "on" : "off");
    MessageBoxA(hwnd, text, AGENT_TITLE, MB_OK | MB_ICONINFORMATION);
}

/* ---- Window ------------------------------------------------------------ */

static LRESULT CALLBACK
wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_CREATE:
            hwnd        = wnd;
            next_viewer = SetClipboardViewer(wnd);
            tray_icon(NIM_ADD);
            return 0;

        case WM_DRAWCLIPBOARD:
            guest_clipboard_changed();
            if (next_viewer != NULL)
                SendMessageA(next_viewer, msg, wp, lp);
            return 0;

        case WM_CHANGECBCHAIN:
            if ((HWND) wp == next_viewer)
                next_viewer = (HWND) lp;
            else if (next_viewer != NULL)
                SendMessageA(next_viewer, msg, wp, lp);
            return 0;

        case WM_HOST_CLIPBOARD:
            host_clipboard_changed();
            return 0;

        case WM_TRAY:
            if ((lp == WM_RBUTTONUP) || (lp == WM_LBUTTONUP))
                tray_menu();
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_POINTER:
                    opt_pointer = !opt_pointer;
                    setting_set("Pointer", opt_pointer);
                    break;
                case ID_CLIPBOARD:
                    opt_clipboard = !opt_clipboard;
                    setting_set("Clipboard", opt_clipboard);
                    break;
                case ID_ABOUT:
                    about();
                    break;
                case ID_EXIT:
                    DestroyWindow(wnd);
                    break;
            }
            return 0;

        case WM_ENDSESSION:
            if (wp)
                gt_bye();
            return 0;

        case WM_DESTROY:
            ChangeClipboardChain(wnd, next_viewer);
            tray_icon(NIM_DELETE);
            PostQuitMessage(0);
            return 0;

        default:
            if ((msg == taskbar_created) && (taskbar_created != 0)) {
                tray_icon(NIM_ADD);
                return 0;
            }
            break;
    }
    return DefWindowProcA(wnd, msg, wp, lp);
}

/* ---- Install ----------------------------------------------------------- */

static void
installed_path(char *path)
{
    GetWindowsDirectoryA(path, MAX_PATH - 16);
    if (path[lstrlenA(path) - 1] != '\\')
        lstrcatA(path, "\\");
    lstrcatA(path, AGENT_EXE);
}

static int
run_key_set(const char *path)
{
    HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };

    for (int i = 0; i < 2; i++) {
        HKEY  key;
        DWORD disp;

        if (RegCreateKeyExA(roots[i], RUN_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, &disp) == ERROR_SUCCESS) {
            LONG ret = RegSetValueExA(key, AGENT_TITLE, 0, REG_SZ, (const BYTE *) path, lstrlenA(path) + 1);

            RegCloseKey(key);
            if (ret == ERROR_SUCCESS)
                return 1;
        }
    }
    return 0;
}

static void
run_key_remove(void)
{
    HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };

    for (int i = 0; i < 2; i++) {
        HKEY key;

        if (RegOpenKeyExA(roots[i], RUN_KEY, 0, KEY_WRITE, &key) == ERROR_SUCCESS) {
            RegDeleteValueA(key, AGENT_TITLE);
            RegCloseKey(key);
        }
    }
}

static void
stop_running_agent(void)
{
    HWND other = FindWindowA(AGENT_CLASS, NULL);

    for (int i = 0; (other != NULL) && (i < 50); i++) {
        PostMessageA(other, WM_COMMAND, ID_EXIT, 0);
        Sleep(100);
        other = FindWindowA(AGENT_CLASS, NULL);
    }
}

/* Copy ourselves into the Windows directory and start with Windows. */
static int
install(int quiet)
{
    char self[MAX_PATH];
    char dest[MAX_PATH];

    GetModuleFileNameA(NULL, self, MAX_PATH);
    installed_path(dest);

    if (lstrcmpiA(self, dest) != 0) {
        stop_running_agent();
        if (!CopyFileA(self, dest, FALSE)) {
            if (!quiet)
                MessageBoxA(NULL, "Could not copy the agent into the Windows directory.",
                            AGENT_TITLE, MB_OK | MB_ICONERROR);
            return 0;
        }
    }
    if (!run_key_set(dest)) {
        if (!quiet)
            MessageBoxA(NULL, "Could not make the agent start with Windows.",
                        AGENT_TITLE, MB_OK | MB_ICONERROR);
        return 0;
    }
    if (!quiet)
        MessageBoxA(NULL, AGENT_TITLE " is installed and will start with Windows.",
                    AGENT_TITLE, MB_OK | MB_ICONINFORMATION);
    return 1;
}

static void
uninstall(int quiet)
{
    char path[MAX_PATH];

    stop_running_agent();
    run_key_remove();
    installed_path(path);
    DeleteFileA(path);
    if (!quiet)
        MessageBoxA(NULL, AGENT_TITLE " is removed.", AGENT_TITLE, MB_OK | MB_ICONINFORMATION);
}

/* ---- Start ------------------------------------------------------------- */

static int
has_switch(const char *cmdline, const char *name)
{
    int n = lstrlenA(name);

    for (const char *p = cmdline; *p; p++) {
        if ((*p == '/') || (*p == '-')) {
            char word[32];

            lstrcpynA(word, p + 1, (n + 1) < (int) sizeof(word) ? (n + 1) : (int) sizeof(word));
            if ((lstrcmpiA(word, name) == 0) && ((p[1 + n] == 0) || (p[1 + n] == ' ')))
                return 1;
        }
    }
    return 0;
}

static int
agent_main(void)
{
    const char *cmdline = GetCommandLineA();
    int         quiet   = has_switch(cmdline, "quiet");
    char        self[MAX_PATH];
    char        dest[MAX_PATH];
    WNDCLASSA   wc;
    MSG         msg;
    HANDLE      mutex;
    DWORD       tid;

    hinst = GetModuleHandleA(NULL);
    is_nt = !(GetVersion() & 0x80000000);

    if (has_switch(cmdline, "uninstall")) {
        uninstall(quiet);
        return 0;
    }

    if (!gt_present()) {
        if (!quiet)
            MessageBoxA(NULL, "This is not an 86Box machine with 86Box Guest Tools turned on.",
                        AGENT_TITLE, MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    if (has_switch(cmdline, "install")) {
        if (!install(quiet))
            return 1;
    } else {
        /* Started from the disk rather than the installed copy: offer to install. */
        GetModuleFileNameA(NULL, self, MAX_PATH);
        installed_path(dest);
        if (!quiet && (lstrcmpiA(self, dest) != 0) &&
            (MessageBoxA(NULL, "Install " AGENT_TITLE " so it starts with Windows?\n\n"
                               "(No runs it this time only.)",
                         AGENT_TITLE, MB_YESNO | MB_ICONQUESTION) == IDYES)) {
            if (!install(0))
                return 1;
        }
    }

    mutex = CreateMutexA(NULL, TRUE, AGENT_MUTEX);
    if ((mutex != NULL) && (GetLastError() == ERROR_ALREADY_EXISTS))
        return 0;

    InitializeCriticalSection(&io_lock);
    opt_pointer   = setting_get("Pointer", 1) != 0;
    opt_clipboard = setting_get("Clipboard", 1) != 0;

    if (!gt_hello()) {
        if (!quiet)
            MessageBoxA(NULL, "The emulator did not answer the agent.", AGENT_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }

    taskbar_created = RegisterWindowMessageA("TaskbarCreated");

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hinst;
    wc.hIcon         = agent_icon(32);
    wc.lpszClassName = AGENT_CLASS;
    if (!RegisterClassA(&wc))
        return 1;
    if (CreateWindowA(AGENT_CLASS, AGENT_TITLE, WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, hinst, NULL) == NULL)
        return 1;

    poll_thread = CreateThread(NULL, 0, poll_proc, NULL, 0, &tid);
    SetThreadPriority(poll_thread, THREAD_PRIORITY_ABOVE_NORMAL);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    quitting = 1;
    WaitForSingleObject(poll_thread, 1000);
    gt_bye();
    return 0;
}

void
entry(void)
{
    ExitProcess(agent_main());
}
