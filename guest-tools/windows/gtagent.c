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

/* Bulk data: whole dwords with REP INSD/OUTSD, the tail a byte at a time. */
static void
gt_get_bytes(void *data, DWORD len)
{
    uint8_t *p = data;
    DWORD    n = len / 4;

    __asm__ __volatile__("cld; rep insl" : "+D"(p), "+c"(n) : "d"((uint16_t) GT_PORT_DATA) : "memory");
    for (len &= 3; len > 0; len--) {
        uint8_t val;

        __asm__ __volatile__("inb %1, %0" : "=a"(val) : "Nd"((uint16_t) GT_PORT_DATA));
        *p++ = val;
    }
}

static void
gt_put_block(const void *data, DWORD len)
{
    const uint8_t *p = data;
    DWORD          n = len / 4;

    __asm__ __volatile__("cld; rep outsl" : "+S"(p), "+c"(n) : "d"((uint16_t) GT_PORT_DATA) : "memory");
    for (len &= 3; len > 0; len--, p++)
        gt_outb(GT_PORT_DATA, *p);
}

static LONG WINAPI
no_channel(EXCEPTION_POINTERS *info)
{
    (void) info;
    MessageBoxA(NULL,
                "86Box Guest Tools are not enabled for this machine.\n\n"
                "Turn on \"86Box Guest Tools\" in the machine's Settings > Other peripherals, "
                "then restart the machine.",
                AGENT_TITLE, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
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
    gt_outl(GT_PORT_DATA, GT_CAP_POINTER | GT_CAP_CLIPBOARD | GT_CAP_FILES);
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

/* ---- Files ------------------------------------------------------------- */

/* DROPFILES, the CF_HDROP header. */
typedef struct {
    DWORD pFiles;
    POINT pt;
    BOOL  fNC;
    BOOL  fWide;
} DROPFILES_T;

#define PATH_CHARS     (MAX_PATH + 1)
#define HOST_LIST_MAX  (16UL * 1024UL * 1024UL)

/* The Unicode file functions, looked up on Windows NT only: Windows 95's
   are stubs at most. Without them paths go through the ANSI code page. */
static struct {
    HANDLE(WINAPI *CreateFile)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    BOOL(WINAPI *CreateDirectory)(LPCWSTR, LPSECURITY_ATTRIBUTES);
    HANDLE(WINAPI *FindFirstFile)(LPCWSTR, LPWIN32_FIND_DATAW);
    BOOL(WINAPI *FindNextFile)(HANDLE, LPWIN32_FIND_DATAW);
    BOOL(WINAPI *DeleteFile)(LPCWSTR);
    BOOL(WINAPI *RemoveDirectory)(LPCWSTR);
    BOOL(WINAPI *SetFileAttributes)(LPCWSTR, DWORD);
    DWORD(WINAPI *GetTempPath)(DWORD, LPWSTR);
} fsw;
static int fs_wide;

typedef struct {
    DWORD    attr;
    uint64_t size;
    WCHAR    name[PATH_CHARS];
} fs_find_t;

/* One file or folder to copy. */
typedef struct {
    uint32_t flags;
    uint64_t size;
    WCHAR   *path; /* guest to host: where it is */
    WCHAR   *name; /* the name the other side sees, '\' between the parts */
} file_entry_t;

typedef struct {
    file_entry_t *e;
    DWORD         count;
    DWORD         alloc;
    uint64_t      total;
} file_list_t;

/* Host to guest: the host's list, and the folder it was last copied to. */
static file_list_t       host_files;
static WCHAR             rendered_dir[PATH_CHARS];
static UINT              cf_drop_effect;

/* Guest to host: one upload at a time. */
static HANDLE            upload_thread;
static volatile LONG     upload_cancel;

static DWORD
wlen(const WCHAR *s)
{
    DWORD n = 0;

    while (s[n])
        n++;
    return n;
}

static WCHAR *
wdup(const WCHAR *s, DWORD n)
{
    WCHAR *d = mem_alloc((n + 1) * sizeof(WCHAR));

    if (d != NULL) {
        memcpy(d, s, n * sizeof(WCHAR));
        d[n] = 0;
    }
    return d;
}

/* a + '\' + b into out (PATH_CHARS); 0 if too long. */
static int
wjoin(WCHAR *out, const WCHAR *a, const WCHAR *b)
{
    DWORD la = wlen(a);
    DWORD lb = wlen(b);
    DWORD o  = 0;

    if ((la + 1 + lb) >= PATH_CHARS)
        return 0;
    memcpy(out, a, la * sizeof(WCHAR));
    o = la;
    if ((o > 0) && (out[o - 1] != L'\\'))
        out[o++] = L'\\';
    memcpy(out + o, b, lb * sizeof(WCHAR));
    out[o + lb] = 0;
    return 1;
}

static void
fs_init(void)
{
    HMODULE k;

    if (!is_nt)
        return;
    k = GetModuleHandleA("KERNEL32.DLL");
    *(FARPROC *) &fsw.CreateFile        = GetProcAddress(k, "CreateFileW");
    *(FARPROC *) &fsw.CreateDirectory   = GetProcAddress(k, "CreateDirectoryW");
    *(FARPROC *) &fsw.FindFirstFile     = GetProcAddress(k, "FindFirstFileW");
    *(FARPROC *) &fsw.FindNextFile      = GetProcAddress(k, "FindNextFileW");
    *(FARPROC *) &fsw.DeleteFile        = GetProcAddress(k, "DeleteFileW");
    *(FARPROC *) &fsw.RemoveDirectory   = GetProcAddress(k, "RemoveDirectoryW");
    *(FARPROC *) &fsw.SetFileAttributes = GetProcAddress(k, "SetFileAttributesW");
    *(FARPROC *) &fsw.GetTempPath       = GetProcAddress(k, "GetTempPathW");
    fs_wide = fsw.CreateFile && fsw.CreateDirectory && fsw.FindFirstFile && fsw.FindNextFile &&
              fsw.DeleteFile && fsw.RemoveDirectory && fsw.SetFileAttributes && fsw.GetTempPath;
}

static int
to_ansi(const WCHAR *src, char *dst, int size)
{
    return WideCharToMultiByte(CP_ACP, 0, src, -1, dst, size, "_", NULL) > 0;
}

static HANDLE
fs_open(const WCHAR *path, int write)
{
    DWORD access = write ? GENERIC_WRITE : GENERIC_READ;
    DWORD share  = write ? 0 : (FILE_SHARE_READ | FILE_SHARE_WRITE);
    DWORD disp   = write ? CREATE_ALWAYS : OPEN_EXISTING;
    char  a[PATH_CHARS * 2];

    if (fs_wide)
        return fsw.CreateFile(path, access, share, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!to_ansi(path, a, sizeof(a)))
        return INVALID_HANDLE_VALUE;
    return CreateFileA(a, access, share, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
}

static int
fs_mkdir(const WCHAR *path)
{
    char a[PATH_CHARS * 2];
    BOOL ok;

    if (fs_wide)
        ok = fsw.CreateDirectory(path, NULL);
    else
        ok = to_ansi(path, a, sizeof(a)) && CreateDirectoryA(a, NULL);
    return ok || (GetLastError() == ERROR_ALREADY_EXISTS);
}

static void
fs_find_copy_a(const WIN32_FIND_DATAA *fd, fs_find_t *f)
{
    f->attr = fd->dwFileAttributes;
    f->size = fd->nFileSizeLow | ((uint64_t) fd->nFileSizeHigh << 32);
    if (MultiByteToWideChar(CP_ACP, 0, fd->cFileName, -1, f->name, PATH_CHARS) <= 0)
        f->name[0] = 0;
}

static void
fs_find_copy_w(const WIN32_FIND_DATAW *fd, fs_find_t *f)
{
    f->attr = fd->dwFileAttributes;
    f->size = fd->nFileSizeLow | ((uint64_t) fd->nFileSizeHigh << 32);
    memcpy(f->name, fd->cFileName, sizeof(fd->cFileName));
    f->name[MAX_PATH] = 0;
}

static HANDLE
fs_find_first(const WCHAR *pattern, fs_find_t *f)
{
    HANDLE h;

    if (fs_wide) {
        WIN32_FIND_DATAW fd;

        h = fsw.FindFirstFile(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE)
            fs_find_copy_w(&fd, f);
    } else {
        WIN32_FIND_DATAA fd;
        char             a[PATH_CHARS * 2];

        if (!to_ansi(pattern, a, sizeof(a)))
            return INVALID_HANDLE_VALUE;
        h = FindFirstFileA(a, &fd);
        if (h != INVALID_HANDLE_VALUE)
            fs_find_copy_a(&fd, f);
    }
    return h;
}

static int
fs_find_next(HANDLE h, fs_find_t *f)
{
    if (fs_wide) {
        WIN32_FIND_DATAW fd;

        if (!fsw.FindNextFile(h, &fd))
            return 0;
        fs_find_copy_w(&fd, f);
    } else {
        WIN32_FIND_DATAA fd;

        if (!FindNextFileA(h, &fd))
            return 0;
        fs_find_copy_a(&fd, f);
    }
    return 1;
}

static int
dot_name(const WCHAR *n)
{
    return (n[0] == L'.') && ((n[1] == 0) || ((n[1] == L'.') && (n[2] == 0)));
}

/* Remove a folder and everything in it. */
static void
fs_delete_tree(const WCHAR *dir)
{
    WCHAR     pattern[PATH_CHARS];
    WCHAR     child[PATH_CHARS];
    char      a[PATH_CHARS * 2];
    fs_find_t f;
    HANDLE    h;

    if (!wjoin(pattern, dir, L"*"))
        return;
    h = fs_find_first(pattern, &f);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (dot_name(f.name) || !wjoin(child, dir, f.name))
                continue;
            if (f.attr & FILE_ATTRIBUTE_DIRECTORY)
                fs_delete_tree(child);
            else if (fs_wide) {
                fsw.SetFileAttributes(child, FILE_ATTRIBUTE_NORMAL);
                fsw.DeleteFile(child);
            } else if (to_ansi(child, a, sizeof(a))) {
                SetFileAttributesA(a, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(a);
            }
        } while (fs_find_next(h, &f));
        FindClose(h);
    }
    if (fs_wide)
        fsw.RemoveDirectory(dir);
    else if (to_ansi(dir, a, sizeof(a)))
        RemoveDirectoryA(a);
}

/* %TEMP%\86BOXGT: where the host's files land before a paste takes them. */
static int
temp_root(WCHAR *out)
{
    WCHAR tmp[PATH_CHARS];

    if (fs_wide) {
        if (fsw.GetTempPath(PATH_CHARS, tmp) == 0)
            return 0;
    } else {
        char a[PATH_CHARS];

        if ((GetTempPathA(sizeof(a), a) == 0) || (MultiByteToWideChar(CP_ACP, 0, a, -1, tmp, PATH_CHARS) <= 0))
            return 0;
    }
    return wjoin(out, tmp, L"86BOXGT");
}

static void
list_free(file_list_t *l)
{
    for (DWORD i = 0; i < l->count; i++) {
        mem_free(l->e[i].path);
        mem_free(l->e[i].name);
    }
    mem_free(l->e);
    memset(l, 0, sizeof(*l));
}

static int
list_add(file_list_t *l, uint32_t flags, uint64_t size, const WCHAR *path, const WCHAR *name, DWORD name_chars)
{
    file_entry_t *e;

    if (l->count >= GT_FILES_MAX_ENTRIES)
        return 0;
    if (l->count == l->alloc) {
        DWORD         alloc = l->alloc ? (l->alloc * 2) : 64;
        file_entry_t *grown = mem_alloc(alloc * sizeof(file_entry_t));

        if (grown == NULL)
            return 0;
        if (l->e != NULL)
            memcpy(grown, l->e, l->count * sizeof(file_entry_t));
        mem_free(l->e);
        l->e     = grown;
        l->alloc = alloc;
    }
    e        = &l->e[l->count];
    e->flags = flags;
    e->size  = size;
    e->path  = (path != NULL) ? wdup(path, wlen(path)) : NULL;
    e->name  = wdup(name, name_chars);
    if (e->name == NULL)
        return 0;
    l->count++;
    l->total += size;
    return 1;
}

/* ---- Files, host to guest ---------------------------------------------- */

/* A host name made safe here: no part may leave the folder, and the
   characters Windows refuses become '_'. 0 if it cannot be used. */
static int
safe_name(WCHAR *name)
{
    WCHAR *part = name;

    for (WCHAR *p = name;; p++) {
        if ((*p == L'\\') || (*p == 0)) {
            DWORD n = (DWORD) (p - part);

            if ((n == 0) || ((n == 1) && (part[0] == L'.')) || ((n == 2) && (part[0] == L'.') && (part[1] == L'.')))
                return 0;
            if (*p == 0)
                return 1;
            part = p + 1;
        } else if ((*p < 0x20) || (*p == L'/') || (*p == L':') || (*p == L'*') || (*p == L'?') ||
                   (*p == L'"') || (*p == L'<') || (*p == L'>') || (*p == L'|'))
            *p = L'_';
    }
}

static void
host_files_take(const uint8_t *buf, DWORD bytes)
{
    DWORD count;
    DWORD pos = 4;

    list_free(&host_files);
    if (bytes < 4)
        return;
    count = buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((DWORD) buf[3] << 24);
    for (DWORD i = 0; (i < count) && ((pos + 16) <= bytes); i++) {
        const uint8_t *p     = buf + pos;
        uint32_t       flags = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
        uint64_t       size  = (p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t) p[7] << 24)) |
                               ((uint64_t) (p[8] | (p[9] << 8) | (p[10] << 16) | ((uint32_t) p[11] << 24)) << 32);
        DWORD          nb    = p[12] | (p[13] << 8) | (p[14] << 16) | ((DWORD) p[15] << 24);
        WCHAR         *name;

        pos += 16;
        if ((nb > (bytes - pos)) || (nb & 1) || (nb == 0))
            break;
        name = wdup((const WCHAR *) (buf + pos), nb / 2);
        pos += nb;
        /* Keep the indexes the host's: an entry that cannot be used stays,
           without a name. */
        if ((name == NULL) || !safe_name(name) || ((wlen(name) + 1) >= MAX_PATH)) {
            mem_free(name);
            name = NULL;
        }
        if (!list_add(&host_files, flags, size, NULL, (name != NULL) ? name : L"", (name != NULL) ? wlen(name) : 0)) {
            mem_free(name);
            break;
        }
        mem_free(name);
    }
}

/* Offer the host's files on the guest clipboard; they are only copied
   over when a program pastes them (WM_RENDERFORMAT). */
static void
host_files_offer(void)
{
    if (!OpenClipboard(hwnd))
        return;
    EmptyClipboard();
    SetClipboardData(CF_HDROP, NULL);
    if (cf_drop_effect != 0) {
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, sizeof(DWORD));

        if (mem != NULL) {
            *(DWORD *) GlobalLock(mem) = 1; /* DROPEFFECT_COPY */
            GlobalUnlock(mem);
            if (SetClipboardData(cf_drop_effect, mem) == NULL)
                GlobalFree(mem);
        }
    }
    CloseClipboard();
}

static int
read_host_file(DWORD index, HANDLE out, uint64_t size, uint8_t *buf)
{
    uint64_t off = 0;

    while (off < size) {
        DWORD    want = ((size - off) > GT_FILE_CHUNK) ? GT_FILE_CHUNK : (DWORD) (size - off);
        uint32_t status;
        DWORD    got = 0;
        DWORD    written;

        EnterCriticalSection(&io_lock);
        gt_outl(GT_PORT_DATA, index);
        gt_outl(GT_PORT_DATA, (uint32_t) off);
        gt_outl(GT_PORT_DATA, (uint32_t) (off >> 32));
        gt_outl(GT_PORT_DATA, want);
        status = gt_command(GT_CMD_FILE_READ);
        if (status == GT_OK) {
            got = gt_inl(GT_PORT_DATA);
            if (got > want)
                got = want;
            gt_get_bytes(buf, got);
        }
        LeaveCriticalSection(&io_lock);

        if ((status != GT_OK) || (got == 0))
            return 0;
        if (!WriteFile(out, buf, got, &written, NULL) || (written != got))
            return 0;
        off += got;
    }
    return 1;
}

/* Copy the host's files into a new folder under %TEMP%\86BOXGT and hand a
   CF_HDROP of the top-level ones. */
static HGLOBAL
host_files_render(void)
{
    WCHAR    root[PATH_CHARS];
    WCHAR    dir[PATH_CHARS];
    WCHAR    path[PATH_CHARS];
    WCHAR    stamp[16];
    uint8_t *buf;
    DWORD    chars = 1;
    HGLOBAL  mem;
    uint8_t *drop;
    HCURSOR  old;

    if ((host_files.count == 0) || !temp_root(root))
        return NULL;
    fs_mkdir(root);
    /* The last paste is over by now. */
    if (rendered_dir[0])
        fs_delete_tree(rendered_dir);
    rendered_dir[0] = 0;
    {
        DWORD t = GetTickCount();

        for (int i = 0; i < 8; i++)
            stamp[i] = L"0123456789ABCDEF"[(t >> ((7 - i) * 4)) & 15];
        stamp[8] = 0;
    }
    if (!wjoin(dir, root, stamp) || !fs_mkdir(dir))
        return NULL;
    memcpy(rendered_dir, dir, sizeof(dir));

    buf = mem_alloc(GT_FILE_CHUNK);
    if (buf == NULL)
        return NULL;
    old = SetCursor(LoadCursorA(NULL, (LPCSTR) IDC_WAIT));
    for (DWORD i = 0; i < host_files.count; i++) {
        const file_entry_t *e = &host_files.e[i];

        if (!e->name[0] || !wjoin(path, dir, e->name))
            continue;
        if (e->flags & GT_FILE_DIR)
            fs_mkdir(path);
        else {
            HANDLE out = fs_open(path, 1);

            if (out != INVALID_HANDLE_VALUE) {
                read_host_file(i, out, e->size, buf);
                CloseHandle(out);
            }
        }
    }
    SetCursor(old);
    mem_free(buf);

    /* The top-level entries, as full paths. */
    for (DWORD i = 0; i < host_files.count; i++) {
        const WCHAR *n = host_files.e[i].name;
        int          top = n[0] != 0;

        for (const WCHAR *p = n; *p; p++)
            top &= (*p != L'\\');
        if (top)
            chars += wlen(dir) + 1 + wlen(n) + 1;
    }
    mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE | GMEM_ZEROINIT, sizeof(DROPFILES_T) + (chars * 2 * sizeof(WCHAR)));
    if (mem == NULL)
        return NULL;
    drop = GlobalLock(mem);
    ((DROPFILES_T *) drop)->pFiles = sizeof(DROPFILES_T);
    ((DROPFILES_T *) drop)->fWide  = is_nt;
    {
        WCHAR *wout = (WCHAR *) (drop + sizeof(DROPFILES_T));
        char  *aout = (char *) (drop + sizeof(DROPFILES_T));

        for (DWORD i = 0; i < host_files.count; i++) {
            const WCHAR *n   = host_files.e[i].name;
            int          top = n[0] != 0;

            for (const WCHAR *p = n; *p; p++)
                top &= (*p != L'\\');
            if (!top || !wjoin(path, dir, n))
                continue;
            if (is_nt) {
                DWORD l = wlen(path);

                memcpy(wout, path, (l + 1) * sizeof(WCHAR));
                wout += l + 1;
            } else {
                int l = WideCharToMultiByte(CP_ACP, 0, path, -1, aout, PATH_CHARS * 2, "_", NULL);

                if (l > 0)
                    aout += l;
            }
        }
    }
    GlobalUnlock(mem);
    return mem;
}

/* ---- Files, guest to host ---------------------------------------------- */

static void
walk_add(file_list_t *l, const WCHAR *path, const WCHAR *name, const fs_find_t *f)
{
    WCHAR     pattern[PATH_CHARS];
    WCHAR     child[PATH_CHARS];
    WCHAR     child_name[PATH_CHARS];
    fs_find_t cf;
    HANDLE    h;

    if (upload_cancel)
        return;
    if (!(f->attr & FILE_ATTRIBUTE_DIRECTORY)) {
        list_add(l, 0, f->size, path, name, wlen(name));
        return;
    }
    if (!list_add(l, GT_FILE_DIR, 0, path, name, wlen(name)) || !wjoin(pattern, path, L"*"))
        return;
    h = fs_find_first(pattern, &cf);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (dot_name(cf.name) || !wjoin(child, path, cf.name) || !wjoin(child_name, name, cf.name))
            continue;
        walk_add(l, child, child_name, &cf);
    } while (!upload_cancel && fs_find_next(h, &cf));
    FindClose(h);
}

static int
upload_file(DWORD index, const file_entry_t *e, uint8_t *buf)
{
    HANDLE   in = fs_open(e->path, 0);
    uint64_t off = 0;
    int      ok  = 1;

    if (in == INVALID_HANDLE_VALUE)
        return 0;
    while (ok && !upload_cancel && (off < e->size)) {
        DWORD got = 0;

        if (!ReadFile(in, buf, GT_FILE_CHUNK, &got, NULL) || (got == 0))
            break;
        EnterCriticalSection(&io_lock);
        gt_outl(GT_PORT_DATA, index);
        gt_outl(GT_PORT_DATA, (uint32_t) off);
        gt_outl(GT_PORT_DATA, (uint32_t) (off >> 32));
        gt_outl(GT_PORT_DATA, got);
        gt_put_block(buf, got);
        ok = gt_command(GT_CMD_FILE_WRITE) == GT_OK;
        LeaveCriticalSection(&io_lock);
        off += got;
    }
    CloseHandle(in);
    return ok && !upload_cancel;
}

/* The upload thread: param is a file_list_t of the copied items (path and
   top-level name), which it frees. */
static DWORD WINAPI
upload_proc(LPVOID param)
{
    file_list_t *top = param;
    file_list_t  all;
    uint8_t     *buf;
    uint32_t     status;
    int          ok = 1;

    memset(&all, 0, sizeof(all));
    for (DWORD i = 0; !upload_cancel && (i < top->count); i++) {
        fs_find_t f;
        HANDLE    h = fs_find_first(top->e[i].path, &f);

        if (h == INVALID_HANDLE_VALUE)
            continue;
        FindClose(h);
        walk_add(&all, top->e[i].path, top->e[i].name, &f);
    }
    list_free(top);
    mem_free(top);

    buf = mem_alloc(GT_FILE_CHUNK);
    if (upload_cancel || (all.count == 0) || (buf == NULL)) {
        list_free(&all);
        mem_free(buf);
        return 0;
    }

    EnterCriticalSection(&io_lock);
    gt_outl(GT_PORT_DATA, all.count);
    gt_outl(GT_PORT_DATA, (uint32_t) all.total);
    gt_outl(GT_PORT_DATA, (uint32_t) (all.total >> 32));
    status = gt_command(GT_CMD_FILES_BEGIN);
    LeaveCriticalSection(&io_lock);

    if (status == GT_OK) {
        for (DWORD i = 0; ok && !upload_cancel && (i < all.count); i++) {
            const file_entry_t *e     = &all.e[i];
            DWORD               bytes = wlen(e->name) * sizeof(WCHAR);
            DWORD               index = 0;

            EnterCriticalSection(&io_lock);
            gt_outl(GT_PORT_DATA, e->flags);
            gt_outl(GT_PORT_DATA, (uint32_t) e->size);
            gt_outl(GT_PORT_DATA, (uint32_t) (e->size >> 32));
            gt_outl(GT_PORT_DATA, bytes);
            gt_put_block(e->name, bytes);
            status = gt_command(GT_CMD_FILE_ADD);
            if (status == GT_OK)
                index = gt_inl(GT_PORT_DATA);
            LeaveCriticalSection(&io_lock);

            /* A file that cannot be read is left out; anything else ends it. */
            if (status != GT_OK)
                ok = 0;
            else if (!(e->flags & GT_FILE_DIR) && (e->size > 0))
                upload_file(index, e, buf);
        }

        EnterCriticalSection(&io_lock);
        gt_outl(GT_PORT_DATA, (ok && !upload_cancel) ? GT_FILES_CLIPBOARD : GT_FILES_CANCEL);
        gt_command(GT_CMD_FILES_END);
        LeaveCriticalSection(&io_lock);
    }

    list_free(&all);
    mem_free(buf);
    return 0;
}

static void
upload_stop(void)
{
    if (upload_thread == NULL)
        return;
    upload_cancel = 1;
    WaitForSingleObject(upload_thread, 10000);
    CloseHandle(upload_thread);
    upload_thread = NULL;
    upload_cancel = 0;
}

/* The guest copied files (CF_HDROP): send them to the host. */
static void
guest_files_changed(void)
{
    file_list_t *top = mem_alloc(sizeof(file_list_t));
    HGLOBAL      mem;
    DWORD        tid;

    if (top == NULL)
        return;
    memset(top, 0, sizeof(*top));
    if (!OpenClipboard(hwnd)) {
        mem_free(top);
        return;
    }
    mem = GetClipboardData(CF_HDROP);
    if (mem != NULL) {
        const uint8_t     *base = GlobalLock(mem);
        DWORD              size = GlobalSize(mem);
        const DROPFILES_T *df   = (const DROPFILES_T *) base;

        if ((base != NULL) && (size > sizeof(DROPFILES_T)) && (df->pFiles < size)) {
            DWORD pos = df->pFiles;

            while (pos < size) {
                WCHAR path[PATH_CHARS];
                DWORD n = 0;

                if (df->fWide) {
                    const WCHAR *s = (const WCHAR *) (base + pos);

                    while (((pos + (n + 1) * 2) <= size) && s[n])
                        n++;
                    if ((n == 0) || (n >= PATH_CHARS))
                        break;
                    memcpy(path, s, n * sizeof(WCHAR));
                    path[n] = 0;
                    pos += (n + 1) * 2;
                } else {
                    const char *s = (const char *) (base + pos);

                    while (((pos + n + 1) <= size) && s[n])
                        n++;
                    if (n == 0)
                        break;
                    pos += n + 1;
                    n = MultiByteToWideChar(CP_ACP, 0, s, (int) n, path, PATH_CHARS - 1);
                    if (n == 0)
                        continue;
                    path[n] = 0;
                }
                /* The name the host gets: the last part of the path. */
                {
                    const WCHAR *name = path;

                    for (const WCHAR *p = path; *p; p++) {
                        if ((*p == L'\\') && p[1])
                            name = p + 1;
                    }
                    list_add(top, 0, 0, path, name, wlen(name));
                }
            }
        }
        if (base != NULL)
            GlobalUnlock(mem);
    }
    CloseClipboard();

    upload_stop();
    if (top->count == 0) {
        list_free(top);
        mem_free(top);
        return;
    }
    upload_thread = CreateThread(NULL, 0, upload_proc, top, 0, &tid);
    if (upload_thread == NULL) {
        list_free(top);
        mem_free(top);
    }
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

    if ((host_features & GT_FEAT_FILES) && IsClipboardFormatAvailable(CF_HDROP)) {
        guest_files_changed();
        return;
    }

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

    uint8_t *list   = NULL;

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
        } else if ((format == GT_CLIP_FILES) && (bytes <= HOST_LIST_MAX)) {
            list = mem_alloc(bytes);
            if (list != NULL)
                gt_get_bytes(list, bytes);
        }
    }
    LeaveCriticalSection(&io_lock);

    if (list != NULL) {
        host_files_take(list, bytes);
        mem_free(list);
        remember(NULL, 0);
        if (host_files.count > 0)
            host_files_offer();
        return;
    }

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
              "Clipboard, guest to host: %s\n"
              "Files through the clipboard: %s\n\n"
              "Set these in the machine's Settings > Other peripherals > 86Box Guest Tools.",
              (host_features & GT_FEAT_POINTER) ? "on" : "off",
              (host_features & GT_FEAT_CLIP_IN) ? "on" : "off",
              (host_features & GT_FEAT_CLIP_OUT) ? "on" : "off",
              (host_features & GT_FEAT_FILES) ? "on" : "off");
    MessageBoxA(hwnd, text, AGENT_TITLE, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
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

        case WM_RENDERFORMAT:
            /* A program pastes the host's files: copy them over now. */
            if (wp == CF_HDROP) {
                HGLOBAL mem = host_files_render();

                if ((mem != NULL) && (SetClipboardData(CF_HDROP, mem) == NULL))
                    GlobalFree(mem);
            }
            return 0;

        case WM_RENDERALLFORMATS:
            /* Going away: the files stay on the host. */
            return 0;

        case WM_DESTROYCLIPBOARD:
            list_free(&host_files);
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
            upload_stop();
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

/* Ask a running agent to exit and wait until its process is gone (its
   window goes first; the exe stays locked until the process ends). */
static void
stop_running_agent(void)
{
    HWND   other = FindWindowA(AGENT_CLASS, NULL);
    DWORD  pid   = 0;
    HANDLE proc  = NULL;

    if (other == NULL)
        return;
    GetWindowThreadProcessId(other, &pid);
    if (pid != 0)
        proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    for (int i = 0; (other != NULL) && (i < 50); i++) {
        PostMessageA(other, WM_COMMAND, ID_EXIT, 0);
        Sleep(100);
        other = FindWindowA(AGENT_CLASS, NULL);
    }
    if (proc != NULL) {
        WaitForSingleObject(proc, 5000);
        CloseHandle(proc);
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
        int copied = 0;

        stop_running_agent();
        /* Copied from a CD, the file comes read-only: CopyFile would refuse
           to replace it next time. */
        SetFileAttributesA(dest, FILE_ATTRIBUTE_NORMAL);
        for (int i = 0; !copied && (i < 20); i++) {
            copied = CopyFileA(self, dest, FALSE);
            if (!copied)
                Sleep(250);
        }
        if (!copied) {
            if (!quiet)
                MessageBoxA(NULL, "Could not copy the agent into the Windows directory.",
                            AGENT_TITLE, MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
            return 0;
        }
        SetFileAttributesA(dest, FILE_ATTRIBUTE_NORMAL);
    }
    if (!run_key_set(dest)) {
        if (!quiet)
            MessageBoxA(NULL, "Could not make the agent start with Windows.",
                        AGENT_TITLE, MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
        return 0;
    }
    if (!quiet)
        MessageBoxA(NULL, AGENT_TITLE " is installed and will start with Windows.",
                    AGENT_TITLE, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
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
        MessageBoxA(NULL, AGENT_TITLE " is removed.", AGENT_TITLE, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
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
                        AGENT_TITLE, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
        return 1;
    }

    GetModuleFileNameA(NULL, self, MAX_PATH);
    installed_path(dest);
    if (lstrcmpiA(self, dest) != 0) {
        /* Started from the disk rather than the installed copy: install (or
           offer to), then hand over to the installed copy -- a program
           running from a disk breaks when the disk is ejected. */
        int do_install = has_switch(cmdline, "install");

        if (!do_install && !quiet &&
            (MessageBoxA(NULL, "Install " AGENT_TITLE " so it starts with Windows?\n\n"
                               "(No runs it this time only.)",
                         AGENT_TITLE, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) == IDYES))
            do_install = 1;
        if (do_install) {
            char run[MAX_PATH + 2];

            if (!install(quiet))
                return 1;
            wsprintfA(run, "\"%s\"", dest);
            WinExec(run, SW_SHOWNORMAL);
            return 0;
        }
    }

    mutex = CreateMutexA(NULL, TRUE, AGENT_MUTEX);
    if ((mutex != NULL) && (GetLastError() == ERROR_ALREADY_EXISTS))
        return 0;

    InitializeCriticalSection(&io_lock);
    fs_init();
    cf_drop_effect = RegisterClipboardFormatA("Preferred DropEffect");
    {
        /* What earlier pastes left behind. */
        WCHAR root[PATH_CHARS];

        if (temp_root(root))
            fs_delete_tree(root);
    }
    opt_pointer   = setting_get("Pointer", 1) != 0;
    opt_clipboard = setting_get("Clipboard", 1) != 0;

    if (!gt_hello()) {
        if (!quiet)
            MessageBoxA(NULL, "The emulator did not answer the agent.", AGENT_TITLE, MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
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
