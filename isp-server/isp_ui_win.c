/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             isp-server on Windows: a window with the log in place of a
 *             console, and an icon in the notification area.  Minimizing
 *             the window leaves only the icon; clicking it brings the log
 *             back, and its menu opens the status page or quits.  Closing
 *             the window quits too.
 *
 *             The server's threads only append to a buffer and post a
 *             message; the window's thread moves the text into the log.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "isp_ui.h"

#define TITLE         L"86Box-Next ISP"
#define WM_ISP_LOG    (WM_APP + 1) /* the buffer has text           */
#define WM_ISP_TRAY   (WM_APP + 2) /* the notification icon's mouse */
#define TRAY_ID       1
#define IDI_ISP       1 /* isp-server.rc */

#define IDM_SHOW      100
#define IDM_PAGE      101
#define IDM_CLEAR     102
#define IDM_EXIT      103

#define PENDING_MAX   (1 << 20) /* bytes waiting for the window; more is dropped */
#define LOG_MAX_CHARS (1 << 20) /* the log keeps the last ~1M characters        */

static CRITICAL_SECTION lock;
static int              lock_ready;
static char            *pending;
static size_t           pending_len;
static size_t           pending_cap;
static int              posted;
static HANDLE           log_file = INVALID_HANDLE_VALUE; /* stdout, when redirected to a file */

static HWND            win;
static HWND            edit;
static HFONT           font;
static HBRUSH          edit_bg;
static HMENU           bar;
static NOTIFYICONDATAW nid;
static UINT            msg_taskbar_created;
static char            page_url[128];

static UINT(WINAPI *get_dpi_for_window)(HWND);

static void
lock_init(void)
{
    if (!lock_ready) {
        InitializeCriticalSection(&lock);
        lock_ready = 1;
    }
}

/* ---------------------------------------------------------------- text */

static wchar_t *
widen(const char *s, int len)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    wchar_t  *w = malloc(((size_t) n + 1) * sizeof(wchar_t));

    if (w != NULL) {
        MultiByteToWideChar(CP_UTF8, 0, s, len, w, n);
        w[n] = L'\0';
    }
    return w;
}

/* LF to CRLF, which is what an edit control and Notepad want. */
static char *
crlf(const char *text)
{
    size_t      n = strlen(text) + 1;
    const char *p;
    char       *out;
    char       *q;

    for (p = text; *p; p++)
        n += (*p == '\n');
    if ((out = malloc(n)) == NULL)
        return NULL;
    for (p = text, q = out; *p; p++) {
        if ((*p == '\n') && ((p == text) || (p[-1] != '\r')))
            *q++ = '\r';
        *q++ = *p;
    }
    *q = '\0';
    return out;
}

static int
usable(HANDLE h)
{
    const DWORD type = ((h != NULL) && (h != INVALID_HANDLE_VALUE)) ? GetFileType(h) : FILE_TYPE_UNKNOWN;

    return (type == FILE_TYPE_DISK) || (type == FILE_TYPE_PIPE) || (type == FILE_TYPE_CHAR);
}

void
isp_ui_message(int error, const char *text)
{
    char  *t = crlf(text);
    HANDLE h = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD  done;

    if (t == NULL)
        return;
    /* Started from a shell that gave it an output, or from a console: the
       text goes there.  Otherwise (Explorer, a shortcut) a message box. */
    if (!usable(h) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        h = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, "\r\n", 2, &done, NULL);
            WriteFile(h, t, (DWORD) strlen(t), &done, NULL);
            CloseHandle(h);
            free(t);
            return;
        }
    }
    if (usable(h))
        WriteFile(h, t, (DWORD) strlen(t), &done, NULL);
    else {
        wchar_t *w = widen(t, -1);

        if (w != NULL)
            MessageBoxW(NULL, w, TITLE, MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
        free(w);
    }
    free(t);
}

void
isp_ui_log(const char *line)
{
    char       buf[700];
    char       stamp[32];
    time_t     t  = time(NULL);
    struct tm *tm = localtime(&t);
    int        n;

    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm);
    n = snprintf(buf, sizeof(buf), "%s  %s\r\n", stamp, line);
    if (n < 0)
        return;
    if ((size_t) n >= sizeof(buf)) {
        n = (int) sizeof(buf) - 1;
        memcpy(buf + n - 2, "\r\n", 2);
    }

    lock_init();
    EnterCriticalSection(&lock);
    if (log_file != INVALID_HANDLE_VALUE) {
        DWORD done;

        WriteFile(log_file, buf, (DWORD) n, &done, NULL);
    }
    if (pending_len + (size_t) n <= PENDING_MAX) {
        if (pending_len + (size_t) n > pending_cap) {
            size_t cap = pending_cap ? pending_cap : 4096;
            char  *p;

            while (cap < pending_len + (size_t) n)
                cap *= 2;
            if ((p = realloc(pending, cap)) != NULL) {
                pending     = p;
                pending_cap = cap;
            }
        }
        if (pending_len + (size_t) n <= pending_cap) {
            memcpy(pending + pending_len, buf, (size_t) n);
            pending_len += (size_t) n;
        }
    }
    if ((win != NULL) && !posted && (pending_len > 0))
        posted = PostMessageW(win, WM_ISP_LOG, 0, 0);
    LeaveCriticalSection(&lock);
}

/* On the window's thread: the buffer into the log. */
static void
drain(void)
{
    char      *text;
    size_t     len;
    wchar_t   *w;
    SCROLLINFO si;
    DWORD      sel_from;
    DWORD      sel_to;
    int        at_end;
    int        end;

    EnterCriticalSection(&lock);
    text        = pending;
    len         = pending_len;
    pending     = NULL;
    pending_len = pending_cap = 0;
    posted      = 0;
    LeaveCriticalSection(&lock);
    if (len == 0) {
        free(text);
        return;
    }
    w = widen(text, (int) len);
    free(text);
    if (w == NULL)
        return;

    /* Follow the end only if that is where the reader is. */
    si.cbSize = sizeof(si);
    si.fMask  = SIF_ALL;
    at_end    = !GetScrollInfo(edit, SB_VERT, &si) || (si.nPos + (int) si.nPage >= si.nMax);
    SendMessageW(edit, EM_GETSEL, (WPARAM) &sel_from, (LPARAM) &sel_to);
    SendMessageW(edit, WM_SETREDRAW, FALSE, 0);

    end = GetWindowTextLengthW(edit);
    if (end + (int) wcslen(w) > LOG_MAX_CHARS) {
        /* Drop the oldest lines, a quarter at a time. */
        const int line = (int) SendMessageW(edit, EM_LINEFROMCHAR, LOG_MAX_CHARS / 4, 0);
        const int cut  = (int) SendMessageW(edit, EM_LINEINDEX, line + 1, 0);

        if (cut > 0) {
            SendMessageW(edit, EM_SETSEL, 0, cut);
            SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM) L"");
            sel_from = (sel_from > (DWORD) cut) ? sel_from - cut : 0;
            sel_to   = (sel_to > (DWORD) cut) ? sel_to - cut : 0;
            end      = GetWindowTextLengthW(edit);
        }
    }
    SendMessageW(edit, EM_SETSEL, end, end);
    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM) w);
    free(w);
    SendMessageW(edit, EM_SETSEL, sel_from, sel_to);
    if (at_end)
        SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);

    SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(edit, NULL, TRUE);
}

/* ---------------------------------------------------------------- window */

static UINT
dpi_of(HWND h)
{
    UINT dpi = 0;

    if (get_dpi_for_window != NULL)
        dpi = get_dpi_for_window(h);
    if (dpi == 0) {
        HDC dc = GetDC(NULL);

        dpi = (UINT) GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(NULL, dc);
    }
    return dpi ? dpi : 96;
}

static void
set_font(void)
{
    HFONT old = font;

    font = CreateFontW(-MulDiv(9, (int) dpi_of(win), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(edit, WM_SETFONT, (WPARAM) font, TRUE);
    if (old != NULL)
        DeleteObject(old);
}

static void
show_log(void)
{
    ShowWindow(win, IsIconic(win) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(win);
}

static void
open_page(void)
{
    if (page_url[0] != '\0')
        ShellExecuteA(NULL, "open", page_url, NULL, NULL, SW_SHOWNORMAL);
}

static void
tray_add(void)
{
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void
tray_menu(void)
{
    HMENU m = CreatePopupMenu();
    POINT pt;

    AppendMenuW(m, MF_STRING, IDM_SHOW, L"&Show log");
    AppendMenuW(m, MF_STRING | (page_url[0] ? 0 : MF_GRAYED), IDM_PAGE, L"Open status &page");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit");
    SetMenuDefaultItem(m, IDM_SHOW, FALSE);

    /* Without the foreground, the menu would not close on a click elsewhere. */
    GetCursorPos(&pt);
    SetForegroundWindow(win);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, win, NULL);
    PostMessageW(win, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK
wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_CREATE:
            edit = CreateWindowExW(0, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                       ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                   0, 0, 0, 0, h, NULL, GetModuleHandleW(NULL), NULL);
            SendMessageW(edit, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
            return 0;

        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) /* Win+D and the like, not only the button */
                ShowWindow(h, SW_HIDE);
            else
                MoveWindow(edit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
            return 0;

        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_MINIMIZE) {
                ShowWindow(h, SW_HIDE);
                return 0;
            }
            break;

        case WM_SETFOCUS:
            SetFocus(edit);
            return 0;

        case WM_CTLCOLORSTATIC:
            /* A read-only edit is grey by default; a log reads better white. */
            if ((HWND) lp == edit) {
                SetTextColor((HDC) wp, GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor((HDC) wp, GetSysColor(COLOR_WINDOW));
                return (LRESULT) edit_bg;
            }
            break;

        case WM_DPICHANGED: {
            const RECT *r = (const RECT *) lp;

            set_font();
            SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_SHOW:
                    show_log();
                    return 0;
                case IDM_PAGE:
                    open_page();
                    return 0;
                case IDM_CLEAR:
                    SetWindowTextW(edit, L"");
                    return 0;
                case IDM_EXIT:
                    DestroyWindow(h);
                    return 0;
            }
            break;

        case WM_ISP_LOG:
            drain();
            return 0;

        case WM_ISP_TRAY:
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP:
                    show_log();
                    break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    tray_menu();
                    break;
            }
            return 0;

        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &nid);
            EnterCriticalSection(&lock);
            win = NULL;
            LeaveCriticalSection(&lock);
            PostQuitMessage(0);
            return 0;

        default:
            /* Explorer restarted: the icon has to be added again. */
            if ((msg == msg_taskbar_created) && (msg != 0)) {
                tray_add();
                return 0;
            }
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int
isp_ui_open(int minimized)
{
    WNDCLASSEXW wc;
    STARTUPINFOW si;
    HMENU       server;
    HICON       big;
    HICON       small;
    int         w;
    int         ht;

    lock_init();
    /* Run with its output in a file: the log goes there too. */
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_DISK)
        log_file = GetStdHandle(STD_OUTPUT_HANDLE);

    /* A shortcut set to "Run: Minimized" starts it in the notification area. */
    GetStartupInfoW(&si);
    if ((si.dwFlags & STARTF_USESHOWWINDOW) &&
        ((si.wShowWindow == SW_MINIMIZE) || (si.wShowWindow == SW_SHOWMINIMIZED) ||
         (si.wShowWindow == SW_SHOWMINNOACTIVE) || (si.wShowWindow == SW_HIDE)))
        minimized = 1;

    *(FARPROC *) &get_dpi_for_window = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    edit_bg             = GetSysColorBrush(COLOR_WINDOW);

    big   = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_ISP), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                       GetSystemMetrics(SM_CYICON), 0);
    small = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_ISP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                       GetSystemMetrics(SM_CYSMICON), 0);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hIcon         = big;
    wc.hIconSm       = small;
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR) IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    wc.lpszClassName = L"86BoxNextIspServer";
    if (!RegisterClassExW(&wc))
        return -1;

    bar    = CreateMenu();
    server = CreatePopupMenu();
    AppendMenuW(server, MF_STRING | MF_GRAYED, IDM_PAGE, L"Open status &page");
    AppendMenuW(server, MF_STRING, IDM_CLEAR, L"&Clear log");
    AppendMenuW(server, MF_SEPARATOR, 0, NULL);
    AppendMenuW(server, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR) server, L"&Server");

    /* 100 columns by 25 lines or so, at the screen's DPI. */
    {
        HDC       dc  = GetDC(NULL);
        const int dpi = GetDeviceCaps(dc, LOGPIXELSY);

        ReleaseDC(NULL, dc);
        w  = MulDiv(820, dpi, 96);
        ht = MulDiv(440, dpi, 96);
    }
    win = CreateWindowExW(0, wc.lpszClassName, TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, ht, NULL,
                          bar, wc.hInstance, NULL);
    if (win == NULL)
        return -1;
    set_font();

    memset(&nid, 0, sizeof(nid));
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = win;
    nid.uID              = TRAY_ID;
    nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_ISP_TRAY;
    nid.hIcon            = small;
    wcscpy(nid.szTip, TITLE);
    tray_add();

    if (!minimized) {
        ShowWindow(win, SW_SHOWNORMAL);
        UpdateWindow(win);
    }

    /* Whatever was logged before the window. */
    EnterCriticalSection(&lock);
    if (!posted && (pending_len > 0))
        posted = PostMessageW(win, WM_ISP_LOG, 0, 0);
    LeaveCriticalSection(&lock);
    return 0;
}

void
isp_ui_set_page(const char *url, int open_it)
{
    char line[160];

    snprintf(page_url, sizeof(page_url), "%s", url);
    if (bar != NULL)
        EnableMenuItem(bar, IDM_PAGE, MF_BYCOMMAND | MF_ENABLED);
    snprintf(line, sizeof(line), "status page: %s", url);
    isp_ui_log(line);
    if (open_it)
        open_page();
}

void
isp_ui_run(int running)
{
    MSG m;

    if (!running && (win != NULL)) {
        SetWindowTextW(win, TITLE L" - not running");
        show_log();
    }
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void
isp_ui_close(void)
{
    if (win != NULL)
        DestroyWindow(win);
    if (font != NULL)
        DeleteObject(font);
}
