/* autogui -- runs a GUI program unattended from a small script, inside the
   guest (Windows 95/98/ME/NT4/2000). Used to install and run the benchmarks
   that have no command line (3DMark, Final Reality, WinBench, WHQL kits...).

     AUTOGUI script.ags [/DUMP]

   The script's name (without extension) names the run: the log goes to
   C:\CTRES\GUI\<NAME>.LOG and screenshots to C:\CTRES\GUI\<NAME>\*.BMP.
   /DUMP only logs every visible window and its controls (to learn a UI).

   Script: one command per line; '#' starts a comment; ":label" is a label.
   Arguments are separated by spaces; quote an argument that has spaces.
   %CD1% / %CD2% are the drives holding CARDTEST.TAG / CARDTST2.TAG, %HCT%
   the drive of a Microsoft HCT disc (\HCT\HWTEST.EXE), %WIN%
   the Windows folder, %PF% "C:\Program Files", %OUT% the screenshot folder.
   Any other %NAME% is the environment variable NAME when it is set.
   A window is matched by a case-blind substring of its title; "class:X"
   matches the window class X instead. Times are in seconds.

     run CMDLINE [DIR]        start a program (CreateProcess)
     open FILE [DIR]          open a document through its association
     copy SRC DEST            copy a file
     copyx SRC DEST           copy a text file, filling in %CD1% %CD2% ...
     copydir SRC DEST [PAT]   copy the files (not folders) of SRC into DEST
     wait WINDOW [T=60]       wait for a visible window, make it current
     waitgone WINDOW [T=60]   wait until no such window is visible
     waitexit [T=600]         wait for the last started program to end
     waitmine [T=60]          wait for a window of the last started program
     waitmode [T=3600]        wait for a full-screen run to end: the screen
                              left the desktop mode and is back for 10 s
     waittext TEXT [T=60]     wait for TEXT in any control of the current window
     ifwin WINDOW LABEL       jump when such a window is visible now
     ifexist PATH LABEL       jump when the file/folder exists
     find NAME [ROOT] [LABEL] search ROOT (default C:, 5 levels) for a file
                              (wildcards allowed); %F% = its full path;
                              jump to LABEL when not found
     goto LABEL
     closeclass CLASS         close every visible window of a class (e.g.
                              CabinetWClass: folder windows setups leave open)
     dir PATTERN              log the files matching PATTERN
     runonce CMDLINE [NAME]   run CMDLINE once at the next logon (RunOnce)
     regdelrun PATTERN        delete Run/RunOnce values whose command has PATTERN
     reboot / shutdown        restart / power off Windows (forced)
     click CAPTION            click a button of the current window by caption
     check CAPTION [0|1]      set a checkbox/radio button (default 1)
     settext N TEXT           set the text of the current window's Nth edit box
     keys TEXT                type into the current window; {ENTER} {TAB}
                              {ESC} {SPACE} {UP} {DOWN} {LEFT} {RIGHT} {F1}..
                              {F12} {ALT+x} {CTRL+x} {SHIFT+x} {WAIT}
     clickxy X Y              mouse click at client coordinates of the window
     read [NAME]              log the text of every control of the window
     readlist                 log the items of its combo boxes / list boxes
     mode W H [BPP]|restore   desktop resolution for this run / as it was
     taskbar off|restore      taskbar not always-on-top (WinBench and the DCT
                              insist) / back as it was
     choose TEXT [N]          select TEXT (prefix) in the window's Nth combo box
     waitfile PATH [T=3600]   wait until a file exists
     collect ROOT [PAT]       copy files matching PAT (default *.log) found
                              below ROOT into %OUT%, named <folders>_<file>
     shot NAME                screenshot of the current window
     shotscreen NAME          screenshot of the whole screen
     close                    WM_CLOSE to the current window
     kill                     end the last started program
     sleep T                  wait, answering dialogs meanwhile
     pause T                  wait without touching any window (use while a
                              full-screen DirectX program runs on Win9x)
     grep NAME FILES TEXT     RESULT NAME <file>: <line> for every line of FILES
                              (wildcards allowed)
                              that contains TEXT (fails when none does)
     wizard [T=300]           walk an installer: accept licences, Next,
                              Install, Finish; answer "restart?" with No
     manual PATTERN [BUTTON]  like "on", for manual-verification prompts ("did
                              you see ...?"): screenshotted, logged as MANUAL
                              for later review, answered BUTTON (default No)
     skip NAME [TEXT]         log a CASE ... SKIP line
     on PATTERN BUTTON        while waiting, a dialog whose title or text
                              contains PATTERN gets BUTTON clicked (logged,
                              screenshotted)
     autodismiss 0|1          while waiting, click OK/Yes/Close on any other
                              plain message box (logged, screenshotted)
     result NAME TEXT         log "RESULT NAME TEXT" (for the host summary)
     resultfrom NAME CAPTION  RESULT with the text of the control after the
                              one captioned CAPTION (label: value pairs)
     resultid NAME ID         RESULT with the text of control ID (see dump)
     log TEXT
     pass NAME [TEXT] / fail NAME [TEXT]   log a CASE line
     dump                     log all windows and controls

   Exit code: 0 = script finished, 1 = a command failed (timeout etc.). */
#include "ct.h"
#include <shellapi.h>

#define MAXLINES  2000
#define MAXARGS   8
#define MAXHANDLED 256

static char  *lines[MAXLINES];
static int    nlines;
static HWND   cur;
static HANDLE proc;
static DWORD  procid, t0;
static char   cd1[4], cd2[4], hctcd[4], windir[MAX_PATH];
static char   outdir[MAX_PATH], found[MAX_PATH];
static int    autodismiss = 0, shotn = 0, desk_w, desk_h, desk_bpp;

static int
screen_bpp(void)
{
    HDC dc = GetDC(NULL);
    int b  = GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES);

    ReleaseDC(NULL, dc);
    return b;
}
static struct {
    char pattern[128];
    char button[64];
    int  manual; /* a manual-verification prompt: logged as MANUAL, never answered "yes" */
} rules[32];
static int  nrules;
static HWND handled[MAXHANDLED];
static int  nhandled;

/* ---- strings --------------------------------------------------------- */

static int
starts_ci(const char *s, const char *pre)
{
    for (; *pre; s++, pre++)
        if (!*s || ((*s >= 'A' && *s <= 'Z') ? *s + 32 : *s) != ((*pre >= 'A' && *pre <= 'Z') ? *pre + 32 : *pre))
            return 0;
    return 1;
}

static int
lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* case-blind substring */
static int
contains(const char *hay, const char *needle)
{
    int i, j;

    if (!*needle)
        return 1;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j] && lower(hay[i + j]) == lower(needle[j]); j++)
            ;
        if (!needle[j])
            return 1;
    }
    return 0;
}

/* caption without '&', dots, '<' '>' and spaces, lower case */
static void
norm(const char *s, char *d, int n)
{
    int k = 0;

    for (; *s && k < n - 1; s++)
        if (*s != '&' && *s != '.' && *s != '<' && *s != '>' && *s != ' ')
            d[k++] = (char) lower(*s);
    d[k] = 0;
}

static int
atoi_(const char *s)
{
    int v = 0, neg = 0;

    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + *s++ - '0';
    return neg ? -v : v;
}

/* expand %CD1% %CD2% %WIN% %PF% %OUT% */
static void
expand(const char *s, char *d, int n)
{
    static const char *names[7] = { "%CD1%", "%CD2%", "%WIN%", "%PF%", "%OUT%", "%F%", "%HCT%" };
    const char        *vals[7];
    int                k = 0, i;

    vals[0] = cd1;
    vals[1] = cd2;
    vals[2] = windir;
    vals[3] = "C:\\Program Files";
    vals[4] = outdir;
    vals[5] = found;
    vals[6] = hctcd;
    while (*s && k < n - 1) {
        for (i = 0; i < 7; i++)
            if (starts_ci(s, names[i]))
                break;
        if (i < 7) {
            const char *v = vals[i];

            while (*v && k < n - 1)
                d[k++] = *v++;
            s += lstrlenA(names[i]);
        } else if (*s == '%') {
            /* any other %NAME%: the environment variable, if it is set */
            static char ename[64], eval[512];
            int         e = 0;

            while (e < 63 && s[1 + e] && s[1 + e] != '%' && s[1 + e] != ' ' && s[1 + e] != '"') {
                ename[e] = s[1 + e];
                e++;
            }
            ename[e] = 0;
            if (e && s[1 + e] == '%' && GetEnvironmentVariableA(ename, eval, sizeof(eval))) {
                const char *v = eval;

                while (*v && k < n - 1)
                    d[k++] = *v++;
                s += e + 2;
            } else
                d[k++] = *s++;
        } else
            d[k++] = *s++;
    }
    d[k] = 0;
}

/* split a line into arguments (quotes group) */
static int
split(char *s, char **av)
{
    int n = 0;

    while (*s && n < MAXARGS) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        if (*s == '"') {
            av[n++] = ++s;
            while (*s && *s != '"')
                s++;
        } else {
            av[n++] = s;
            while (*s && *s != ' ' && *s != '\t')
                s++;
        }
        if (*s)
            *s++ = 0;
    }
    return n;
}

/* ---- log -------------------------------------------------------------- */

static void
alog(const char *fmt, ...)
{
    char    buf[1100];
    DWORD   n, t = (GetTickCount() - t0) / 100;
    va_list ap;

    n = wsprintfA(buf, "[%4d.%d] ", t / 10, t % 10);
    va_start(ap, fmt);
    n += wvsprintfA(buf + n, fmt, ap);
    va_end(ap);
    buf[n++] = '\r';
    buf[n++] = '\n';
    if (ct_logf != INVALID_HANDLE_VALUE) {
        WriteFile(ct_logf, buf, n, &n, NULL);
        FlushFileBuffers(ct_logf); /* so the host can follow the run on the disk image */
    }
}

/* ---- windows ----------------------------------------------------------- */

typedef struct {
    const char *pat;
    HWND        found;
} findctx;

static int
win_matches(HWND h, const char *pat)
{
    char t[256];

    if (!IsWindowVisible(h))
        return 0;
    if (!lstrcmpiA(pat, "*"))
        return 1;
    if (starts_ci(pat, "class:")) {
        GetClassNameA(h, t, sizeof(t));
        return !lstrcmpiA(t, pat + 6);
    }
    GetWindowTextA(h, t, sizeof(t));
    return t[0] && contains(t, pat);
}

static BOOL CALLBACK
find_cb(HWND h, LPARAM l)
{
    findctx *c = (findctx *) l;

    if (win_matches(h, c->pat)) {
        c->found = h;
        return FALSE;
    }
    return TRUE;
}

static BOOL CALLBACK
mine_cb(HWND h, LPARAM l)
{
    DWORD pid;

    GetWindowThreadProcessId(h, &pid);
    if (pid == procid && IsWindowVisible(h)) {
        *(HWND *) l = h;
        return FALSE;
    }
    return TRUE;
}

/* a visible top-level window of the last started program */
static HWND
find_mine(void)
{
    HWND h = NULL;

    if (procid)
        EnumWindows(mine_cb, (LPARAM) &h);
    return h;
}

static HWND
find_top(const char *pat)
{
    findctx c;

    c.pat   = pat;
    c.found = NULL;
    EnumWindows(find_cb, (LPARAM) &c);
    return c.found;
}

/* nth child of a class (1-based), or a child button by caption */
typedef struct {
    const char *cls;
    const char *cap;
    int         n;
    HWND        found;
    int         buttons_only;
} childctx;

static BOOL CALLBACK
child_cb(HWND h, LPARAM l)
{
    childctx *c = (childctx *) l;
    char      t[256], a[128], b[128];

    if (!IsWindowVisible(h))
        return TRUE;
    if (c->cls) {
        GetClassNameA(h, t, sizeof(t));
        if (contains(t, c->cls) && --c->n == 0) {
            c->found = h;
            return FALSE;
        }
        return TRUE;
    }
    if (c->buttons_only) {
        GetClassNameA(h, t, sizeof(t));
        if (lstrcmpiA(t, "Button"))
            return TRUE;
    }
    GetWindowTextA(h, t, sizeof(t));
    norm(t, a, sizeof(a));
    norm(c->cap, b, sizeof(b));
    if (a[0] && !lstrcmpA(a, b)) {
        c->found = h;
        return FALSE;
    }
    if (a[0] && !c->found && !c->buttons_only && contains(a, b) && lstrlenA(b) >= 3)
        c->n = (int) (INT_PTR) h; /* remember a substring match as a fallback */
    return TRUE;
}

static HWND
find_child_ex(HWND parent, const char *cap, int buttons_only)
{
    childctx c;

    c.cls          = NULL;
    c.cap          = cap;
    c.n            = 0;
    c.found        = NULL;
    c.buttons_only = buttons_only;
    EnumChildWindows(parent, child_cb, (LPARAM) &c);
    if (!c.found && c.n)
        c.found = (HWND) (INT_PTR) c.n;
    return c.found;
}

typedef struct {
    int  id;
    HWND found;
} idfind_t;

static BOOL CALLBACK
id_cb(HWND h, LPARAM lp)
{
    idfind_t *f = (idfind_t *) lp;

    if (GetDlgCtrlID(h) == f->id) {
        f->found = h;
        return FALSE;
    }
    return TRUE;
}

static HWND
find_id(HWND parent, int id)
{
    idfind_t f;

    f.id    = id;
    f.found = NULL;
    EnumChildWindows(parent, id_cb, (LPARAM) &f);
    return f.found;
}

/* a value: starts with a digit and has no letters (rejects "2D image
   processing", "Progress1" and the like) */
static int
is_value(const char *t)
{
    int i;

    while (*t == ' ')
        t++;
    if (*t < '0' || *t > '9')
        return 0;
    for (i = 0; t[i]; i++)
        if ((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z'))
            return 0;
    return 1;
}

static HWND
find_child(HWND parent, const char *cap)
{
    return find_child_ex(parent, cap, 0);
}

/* exact caption, buttons only (the dialog watcher must never hit text) */
static HWND
find_button(HWND parent, const char *cap)
{
    return find_child_ex(parent, cap, 1);
}

static HWND
find_nth(HWND parent, const char *cls, int n)
{
    childctx c;

    c.cls          = cls;
    c.cap          = NULL;
    c.n            = n;
    c.found        = NULL;
    c.buttons_only = 0;
    EnumChildWindows(parent, child_cb, (LPARAM) &c);
    return c.found;
}

/* all control texts of a window, joined (for text matching and logs) */
typedef struct {
    char *buf;
    int   len, max;
} textctx;

static BOOL CALLBACK
text_cb(HWND h, LPARAM l)
{
    textctx *c = (textctx *) l;
    char     t[512];
    int      n;

    if (!IsWindowVisible(h))
        return TRUE;
    n = (int) SendMessageTimeoutA(h, WM_GETTEXT, sizeof(t), (LPARAM) t, SMTO_ABORTIFHUNG, 500, NULL) ? lstrlenA(t) : 0;
    if (n && c->len + n + 3 < c->max) {
        memcpy(c->buf + c->len, t, n);
        c->len += n;
        c->buf[c->len++] = ' ';
        c->buf[c->len++] = '|';
        c->buf[c->len++] = ' ';
    }
    return TRUE;
}

static void
all_text(HWND h, char *buf, int max)
{
    textctx c;

    c.buf = buf;
    c.len = 0;
    c.max = max - 1;
    GetWindowTextA(h, buf, max);
    c.len = lstrlenA(buf);
    if (c.len + 3 < c.max) {
        buf[c.len++] = ' ';
        buf[c.len++] = '|';
        buf[c.len++] = ' ';
    }
    EnumChildWindows(h, text_cb, (LPARAM) &c);
    buf[c.len] = 0;
}

/* items of every combo box / list box of a window (logged as LIST lines) */
static BOOL CALLBACK
list_cb(HWND h, LPARAM l)
{
    char c[64], t[300];
    int  n, i, combo, lb;

    GetClassNameA(h, c, sizeof(c));
    combo = !lstrcmpiA(c, "ComboBox");
    lb    = !lstrcmpiA(c, "ListBox") || !lstrcmpiA(c, "ComboLBox");
    if (!combo && !lb)
        return TRUE;
    n = (int) SendMessageA(h, combo ? CB_GETCOUNT : LB_GETCOUNT, 0, 0);
    for (i = 0; i < n && i < 200; i++) {
        int len = (int) SendMessageA(h, combo ? CB_GETLBTEXTLEN : LB_GETTEXTLEN, i, 0);

        t[0] = 0;
        if (len > 0 && len < (int) sizeof(t))
            SendMessageA(h, combo ? CB_GETLBTEXT : LB_GETTEXT, i, (LPARAM) t);
        alog("LIST  %s id=%d [%d] %s", c, GetDlgCtrlID(h), i, t);
    }
    return TRUE;
}

/* ---- screenshots -------------------------------------------------------- */

static void
shot_rect(const char *name, RECT *r)
{
    int              w = r->right - r->left, h = r->bottom - r->top;
    HDC              scr, mem;
    HBITMAP          bm;
    BITMAPINFOHEADER bi;
    void            *bits;
    char             nm[64];

    if (w <= 0 || h <= 0)
        return;
    memset(&bi, 0, sizeof(bi));
    bi.biSize     = sizeof(bi);
    bi.biWidth    = w;
    bi.biHeight   = -h;
    bi.biPlanes   = 1;
    bi.biBitCount = 32;
    scr           = GetDC(NULL);
    mem           = CreateCompatibleDC(scr);
    bm            = CreateDIBSection(scr, (BITMAPINFO *) &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(mem, bm);
    BitBlt(mem, 0, 0, w, h, scr, r->left, r->top, SRCCOPY);
    GdiFlush();
    wsprintfA(nm, "%02d_%s", ++shotn, name);
    /* ct_save_bmp appends _<suffix>.BMP; use "S" */
    ct_save_bmp(nm, "S", (DWORD *) bits, w, h);
    alog("SHOT  %s_S.BMP (%dx%d)", nm, w, h);
    DeleteDC(mem);
    DeleteObject(bm);
    ReleaseDC(NULL, scr);
}

static void
shot_win(HWND h, const char *name)
{
    RECT r;

    if (!h || !IsWindow(h))
        return;
    SetForegroundWindow(h);
    Sleep(300);
    GetWindowRect(h, &r);
    if (r.left < 0)
        r.left = 0;
    if (r.top < 0)
        r.top = 0;
    if (r.right > GetSystemMetrics(SM_CXSCREEN))
        r.right = GetSystemMetrics(SM_CXSCREEN);
    if (r.bottom > GetSystemMetrics(SM_CYSCREEN))
        r.bottom = GetSystemMetrics(SM_CYSCREEN);
    shot_rect(name, &r);
}

/* ---- input -------------------------------------------------------------- */

static void
press(BYTE vk, int down)
{
    keybd_event(vk, (BYTE) MapVirtualKeyA(vk, 0), down ? 0 : KEYEVENTF_KEYUP, 0);
}

static void
tap(BYTE vk, int shift, int ctrl, int alt)
{
    if (shift)
        press(VK_SHIFT, 1);
    if (ctrl)
        press(VK_CONTROL, 1);
    if (alt)
        press(VK_MENU, 1);
    press(vk, 1);
    press(vk, 0);
    if (alt)
        press(VK_MENU, 0);
    if (ctrl)
        press(VK_CONTROL, 0);
    if (shift)
        press(VK_SHIFT, 0);
    Sleep(40);
}

static void
send_keys(const char *s)
{
    static const struct {
        const char *n;
        BYTE        vk;
    } names[] = { { "ENTER", VK_RETURN }, { "TAB", VK_TAB },   { "ESC", VK_ESCAPE }, { "SPACE", VK_SPACE },
                  { "UP", VK_UP },        { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT },  { "RIGHT", VK_RIGHT },
                  { "HOME", VK_HOME },    { "END", VK_END },   { "DEL", VK_DELETE }, { "BS", VK_BACK } };
    int i;

    if (cur)
        SetForegroundWindow(cur);
    Sleep(200);
    while (*s) {
        if (*s == '{') {
            char tok[32];
            int  k = 0, shift = 0, ctrl = 0, alt = 0;

            s++;
            while (*s && *s != '}' && k < 31)
                tok[k++] = *s++;
            tok[k] = 0;
            if (*s == '}')
                s++;
            k = 0;
            for (;;) {
                if (starts_ci(tok + k, "ALT+"))
                    alt = 1, k += 4;
                else if (starts_ci(tok + k, "CTRL+"))
                    ctrl = 1, k += 5;
                else if (starts_ci(tok + k, "SHIFT+"))
                    shift = 1, k += 6;
                else
                    break;
            }
            if (!lstrcmpiA(tok + k, "WAIT")) {
                Sleep(1000);
                continue;
            }
            if ((tok[k] == 'F' || tok[k] == 'f') && tok[k + 1] >= '1' && tok[k + 1] <= '9') {
                tap((BYTE) (VK_F1 + atoi_(tok + k + 1) - 1), shift, ctrl, alt);
                continue;
            }
            for (i = 0; i < (int) (sizeof(names) / sizeof(names[0])); i++)
                if (!lstrcmpiA(tok + k, names[i].n)) {
                    tap(names[i].vk, shift, ctrl, alt);
                    break;
                }
            if (i == (int) (sizeof(names) / sizeof(names[0])) && tok[k]) {
                SHORT v = VkKeyScanA(tok[k]);

                tap((BYTE) (v & 0xff), shift || (v & 0x100), ctrl, alt);
            }
            continue;
        }
        {
            SHORT v = VkKeyScanA(*s);

            if (v != -1)
                tap((BYTE) (v & 0xff), (v & 0x100) != 0, (v & 0x200) != 0, (v & 0x400) != 0);
        }
        s++;
    }
}

static void
click_button(HWND b)
{
    HWND top = b, p;

    /* GetAncestor is not in Windows 95 / NT 4 */
    while ((p = GetParent(top)) != NULL && (GetWindowLongA(top, GWL_STYLE) & WS_CHILD))
        top = p;
    SetForegroundWindow(top);
    /* BM_CLICK is posted: a modal loop it starts must not block us */
    PostMessageA(b, BM_CLICK, 0, 0);
    Sleep(400);
}

static void
set_check(HWND b, int on)
{
    if ((SendMessageA(b, BM_GETCHECK, 0, 0) == BST_CHECKED) != (on != 0))
        click_button(b);
    if ((SendMessageA(b, BM_GETCHECK, 0, 0) == BST_CHECKED) != (on != 0))
        SendMessageA(b, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

/* ---- dialog watcher ------------------------------------------------------- */

static int
was_handled(HWND h)
{
    int i;

    for (i = 0; i < nhandled; i++)
        if (handled[i] == h)
            return 1;
    return 0;
}

static void
mark_handled(HWND h)
{
    if (nhandled < MAXHANDLED)
        handled[nhandled++] = h;
    else
        handled[nhandled++ % MAXHANDLED] = h;
}

typedef struct {
    HWND list[64];
    int  n;
} dlglist;

static BOOL CALLBACK
dlg_cb(HWND h, LPARAM l)
{
    dlglist *d = (dlglist *) l;
    char     c[64];

    if (!IsWindowVisible(h) || d->n >= 64)
        return TRUE;
    GetClassNameA(h, c, sizeof(c));
    if (!lstrcmpA(c, "#32770"))
        d->list[d->n++] = h;
    return TRUE;
}

/* scroll every edit / rich edit box of a dialog to its end (licence boxes
   whose accept button only enables after the text was read to the bottom) */
static BOOL CALLBACK
scroll_cb(HWND h, LPARAM l)
{
    char c[32];

    GetClassNameA(h, c, sizeof(c));
    if (contains(c, "Edit")) {
        int i;

        SendMessageA(h, EM_SETSEL, 0x7fff, 0x7fff);
        SendMessageA(h, EM_SCROLLCARET, 0, 0);
        for (i = 0; i < 400; i++)
            SendMessageA(h, WM_VSCROLL, SB_PAGEDOWN, 0);
        SendMessageA(h, WM_VSCROLL, SB_BOTTOM, 0);
        SendMessageA(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), EN_VSCROLL), (LPARAM) h);
        SendMessageA(GetParent(h), WM_VSCROLL, SB_BOTTOM, (LPARAM) h);
    }
    return TRUE;
}

/* Rules first, then (autodismiss) plain message boxes; never the window the
   script is working with. Returns 1 if something was handled. */
static int
watch_dialogs(HWND except)
{
    static dlglist d;
    static char    txt[2048];
    int     i, r, done = 0;

    d.n = 0;
    EnumWindows(dlg_cb, (LPARAM) &d);
    for (i = 0; i < d.n; i++) {
        HWND h = d.list[i];

        if (h == except || was_handled(h))
            continue;
        all_text(h, txt, sizeof(txt));
        for (r = 0; r < nrules; r++)
            if (contains(txt, rules[r].pattern)) {
                HWND b = find_button(h, rules[r].button);

                if (rules[r].manual)
                    alog("MANUAL \"%s\" (answered %s, review later): %s", rules[r].pattern, rules[r].button, txt);
                else
                    alog("RULE  \"%s\" -> %s: %s", rules[r].pattern, rules[r].button, txt);
                shot_win(h, rules[r].manual ? "manual" : "rule");
                if (b && !IsWindowEnabled(b)) {
                    /* e.g. "scroll to the end of the agreement first" */
                    EnumChildWindows(h, scroll_cb, 0);
                    Sleep(500);
                }
                if (b && !IsWindowEnabled(b)) {
                    alog("NOTE  %s still disabled after scrolling; retrying", rules[r].button);
                    done = 1;
                    break;
                }
                if (b)
                    click_button(b);
                mark_handled(h);
                done = 1;
                break;
            }
        if (r < nrules || !autodismiss)
            continue;
        /* plain message boxes only: no edit box, not a wizard page */
        if (!find_nth(h, "Edit", 1) && !find_button(h, "Next") && !find_button(h, "Finish") &&
            !find_button(h, "Back") && !find_button(h, "Install")) {
            static const char *ok[]   = { "OK", "Yes", "Close", "Continue", "Ignore" };
            static const char *safe[] = { "No", "Cancel", "OK", "Close" };
            const char       **list   = ok;
            int                k, nlist = 5;

            /* never agree to a restart, a deletion or a format behind the script's back */
            if (contains(txt, "restart") || contains(txt, "reboot") || contains(txt, "delete") ||
                contains(txt, "remove") || contains(txt, "uninstall") || contains(txt, "format")) {
                list  = safe;
                nlist = 4;
            }
            for (k = 0; k < nlist; k++) {
                HWND b = find_button(h, list[k]);

                if (b && IsWindowEnabled(b)) {
                    alog("DISMISS %s: %s", list[k], txt);
                    shot_win(h, "dismiss");
                    click_button(b);
                    mark_handled(h);
                    done = 1;
                    break;
                }
            }
        }
    }
    return done;
}

/* sleep for ms while handling dialogs */
static void
idle(int ms)
{
    DWORD end = GetTickCount() + ms;

    do {
        watch_dialogs(cur);
        Sleep(100);
    } while ((int) (end - GetTickCount()) > 0);
}

/* ---- the installer walker --------------------------------------------------- */

typedef struct {
    HWND btn[48];
    char cap[48][64];
    int  style[48];
    int  n;
} btnlist;

static BOOL CALLBACK
btn_cb(HWND h, LPARAM l)
{
    btnlist *b = (btnlist *) l;
    char     c[32], t[128];

    if (!IsWindowVisible(h) || b->n >= 48)
        return TRUE;
    GetClassNameA(h, c, sizeof(c));
    if (lstrcmpiA(c, "Button"))
        return TRUE;
    GetWindowTextA(h, t, sizeof(t));
    norm(t, b->cap[b->n], 64);
    b->style[b->n] = (int) (GetWindowLongA(h, GWL_STYLE) & 0x0f);
    b->btn[b->n++] = h;
    return TRUE;
}

/* Find the window to act on: the most recently shown enabled top-level
   window that has buttons, is not ours and not the shell. */
typedef struct {
    HWND found;
} wizctx;

static BOOL CALLBACK
wiz_cb(HWND h, LPARAM l)
{
    wizctx        *w = (wizctx *) l;
    char           c[64];
    static btnlist b;

    if (!IsWindowVisible(h) || !IsWindowEnabled(h))
        return TRUE;
    GetClassNameA(h, c, sizeof(c));
    if (!lstrcmpA(c, "Shell_TrayWnd") || !lstrcmpA(c, "Progman") || !lstrcmpA(c, "CARDTEST"))
        return TRUE;
    b.n = 0;
    EnumChildWindows(h, btn_cb, (LPARAM) &b);
    if (b.n) {
        w->found = h; /* EnumWindows goes top of Z order first */
        return FALSE;
    }
    return TRUE;
}

static int
wizard(int seconds)
{
    static const char *accept[] = { "iaccept", "iagree", "accept", "agree", "yes,i" };
    static const char *next[]   = { "iagree", "iaccept", "accept", "agree", "yes", "next", "install", "continue",
                                    "finish", "ok", "done", "close", "exit", "start" };
    DWORD              end = GetTickCount() + seconds * 1000, seen = GetTickCount();
    HWND               lastw = NULL;
    static char        lastc[64], txt[2048];
    int                repeats = 0, steps = 0, i, k;

    lastc[0] = 0;
    for (;;) {
        wizctx         w;
        static btnlist b;
        int            restart, acted = 0;

        if ((int) (end - GetTickCount()) < 0) {
            alog("WIZARD timeout after %d steps", steps);
            return steps ? 0 : -1;
        }
        watch_dialogs(NULL);
        w.found = NULL;
        EnumWindows(wiz_cb, (LPARAM) &w);
        if (!w.found) {
            /* done once no installer window has been seen for 20 s (setup
               launchers exit early and leave their real setup running) */
            if (steps && GetTickCount() - seen > 20000) {
                alog("WIZARD done after %d steps", steps);
                return 0;
            }
            Sleep(500);
            continue;
        }
        seen = GetTickCount();
        b.n = 0;
        EnumChildWindows(w.found, btn_cb, (LPARAM) &b);
        all_text(w.found, txt, sizeof(txt));
        restart = contains(txt, "restart") || contains(txt, "reboot");
        /* radio buttons / checkboxes first */
        for (i = 0; i < b.n; i++) {
            int radio = b.style[i] == BS_AUTORADIOBUTTON || b.style[i] == BS_RADIOBUTTON;
            int box   = b.style[i] == BS_AUTOCHECKBOX || b.style[i] == BS_CHECKBOX;

            if (radio && !contains(b.cap[i], "not") && !contains(b.cap[i], "dont")) {
                for (k = 0; k < 5; k++)
                    if (contains(b.cap[i], accept[k]) && SendMessageA(b.btn[i], BM_GETCHECK, 0, 0) != BST_CHECKED) {
                        alog("WIZARD accept \"%s\"", b.cap[i]);
                        set_check(b.btn[i], 1);
                    }
            }
            if (radio && restart && (contains(b.cap[i], "later") || contains(b.cap[i], "no,")) &&
                SendMessageA(b.btn[i], BM_GETCHECK, 0, 0) != BST_CHECKED) {
                alog("WIZARD restart later \"%s\"", b.cap[i]);
                set_check(b.btn[i], 1);
            }
            if (box && (contains(b.cap[i], "readme") || contains(b.cap[i], "launch") || contains(b.cap[i], "run") ||
                        contains(b.cap[i], "view") || contains(b.cap[i], "register") || contains(b.cap[i], "online")) &&
                SendMessageA(b.btn[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
                alog("WIZARD untick \"%s\"", b.cap[i]);
                set_check(b.btn[i], 0);
            }
        }
        /* a plain "restart now?" box: No */
        if (restart)
            for (i = 0; i < b.n && !acted; i++)
                if (!lstrcmpA(b.cap[i], "no")) {
                    alog("WIZARD restart? -> No: %s", txt);
                    shot_win(w.found, "wizard");
                    click_button(b.btn[i]);
                    acted = 1;
                }
        for (k = 0; k < (int) (sizeof(next) / sizeof(next[0])) && !acted; k++)
            for (i = 0; i < b.n; i++) {
                int push = b.style[i] == BS_PUSHBUTTON || b.style[i] == BS_DEFPUSHBUTTON;

                if (push && IsWindowEnabled(b.btn[i]) && !lstrcmpA(b.cap[i], next[k])) {
                    if (w.found == lastw && !lstrcmpA(lastc, b.cap[i])) {
                        if (++repeats > 8) {
                            alog("WIZARD stuck on \"%s\": %s", b.cap[i], txt);
                            shot_win(w.found, "stuck");
                            return -1;
                        }
                    } else
                        repeats = 0;
                    alog("WIZARD step %d: \"%s\" | %s", steps + 1, b.cap[i], txt);
                    if (steps < 40)
                        shot_win(w.found, "wizard");
                    click_button(b.btn[i]);
                    lastw = w.found;
                    lstrcpyA(lastc, b.cap[i]);
                    steps++;
                    acted = 1;
                    break;
                }
            }
        Sleep(acted ? 1500 : 700);
    }
}

/* ---- dump ------------------------------------------------------------------- */

static int depth;

static BOOL CALLBACK
dumpchild_cb(HWND h, LPARAM l)
{
    char c[64], t[256];
    RECT r;

    if (GetParent(h) != (HWND) l)
        return TRUE;
    GetClassNameA(h, c, sizeof(c));
    t[0] = 0;
    SendMessageTimeoutA(h, WM_GETTEXT, sizeof(t), (LPARAM) t, SMTO_ABORTIFHUNG, 500, NULL);
    GetWindowRect(h, &r);
    static const char pad[] = "                              ";
    alog("DUMP  %s%08x %s id=%d style=%08x %s \"%s\" (%d,%d %dx%d)", pad + 30 - (depth > 15 ? 30 : depth * 2), (DWORD) (INT_PTR) h, c,
         GetDlgCtrlID(h), (DWORD) GetWindowLongA(h, GWL_STYLE), IsWindowVisible(h) ? "vis" : "hid", t, r.left,
         r.top, r.right - r.left, r.bottom - r.top);
    depth++;
    EnumChildWindows(h, dumpchild_cb, (LPARAM) h);
    depth--;
    return TRUE;
}

static BOOL CALLBACK
dumptop_cb(HWND h, LPARAM l)
{
    char  c[64], t[256];
    RECT  r;
    DWORD pid;

    if (!IsWindowVisible(h))
        return TRUE;
    GetClassNameA(h, c, sizeof(c));
    GetWindowTextA(h, t, sizeof(t));
    GetWindowRect(h, &r);
    GetWindowThreadProcessId(h, &pid);
    alog("DUMP  TOP %08x %s pid=%x \"%s\" (%d,%d %dx%d)", (DWORD) (INT_PTR) h, c, pid, t, r.left, r.top,
         r.right - r.left, r.bottom - r.top);
    depth = 1;
    EnumChildWindows(h, dumpchild_cb, (LPARAM) h);
    return TRUE;
}

static void
dump(void)
{
    EnumWindows(dumptop_cb, 0);
}

/* ---- commands ------------------------------------------------------------------ */

static int
find_label(const char *name)
{
    int i;

    for (i = 0; i < nlines; i++)
        if (lines[i][0] == ':' && !lstrcmpiA(lines[i] + 1, name))
            return i;
    alog("ERROR no label %s", name);
    return -1;
}

/* wait for a window; returns it or NULL after the timeout */
static HWND
wait_win(const char *pat, int seconds, int gone)
{
    DWORD end = GetTickCount() + seconds * 1000;
    HWND  h;

    do {
        h = find_top(pat);
        if (gone ? !h : h != NULL)
            return gone ? (HWND) 1 : h;
        watch_dialogs(h);
        Sleep(250);
    } while ((int) (end - GetTickCount()) > 0);
    return NULL;
}

static void
drives(void)
{
    char p[] = "A:\\CARDTEST.TAG", q[] = "A:\\CARDTST2.TAG", r[] = "A:\\HCT\\HWTEST.EXE";
    int  d;

    for (d = 'C'; d <= 'Z'; d++) {
        p[0] = q[0] = r[0] = (char) d;
        if (!hctcd[0] && GetFileAttributesA(r) != 0xffffffff)
            wsprintfA(hctcd, "%c:", d);
        if (!cd1[0] && GetFileAttributesA(p) != 0xffffffff)
            wsprintfA(cd1, "%c:", d);
        if (!cd2[0] && GetFileAttributesA(q) != 0xffffffff)
            wsprintfA(cd2, "%c:", d);
    }
}

/* depth-limited recursive search for a file name; the folder it is in
   goes to `found` (with the name appended) */
static int
find_file(const char *dir, const char *name, int depth)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    char             pat[MAX_PATH];

    wsprintfA(pat, "%s\\%s", dir, name);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        wsprintfA(found, "%s\\%s", dir, fd.cFileName);
        return 1;
    }
    if (depth <= 0)
        return 0;
    wsprintfA(pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.' &&
            lstrcmpiA(fd.cFileName, "WINDOWS") && lstrcmpiA(fd.cFileName, "WINNT") &&
            lstrcmpiA(fd.cFileName, "RECYCLED") && lstrcmpiA(fd.cFileName, "CTRES")) {
            char sub[MAX_PATH];

            wsprintfA(sub, "%s\\%s", dir, fd.cFileName);
            if (find_file(sub, name, depth - 1)) {
                FindClose(h);
                return 1;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

/* recursive copy of the files matching pat below dir into dest; the copy
   is named <subfolder>_<file> so same-named logs do not overwrite */
static int
collect_files(const char *dir, const char *pat, const char *dest, const char *prefix, int depth)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    char             p[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    int              n = 0;

    wsprintfA(p, "%s\\%s", dir, pat);
    h = FindFirstFileA(p, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            wsprintfA(a, "%s\\%s", dir, fd.cFileName);
            wsprintfA(b, "%s\\%s%s", dest, prefix, fd.cFileName);
            if (CopyFileA(a, b, FALSE))
                n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (depth <= 0)
        return n;
    wsprintfA(p, "%s\\*", dir);
    h = FindFirstFileA(p, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return n;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
            char sub[MAX_PATH], pre[MAX_PATH];

            wsprintfA(sub, "%s\\%s", dir, fd.cFileName);
            wsprintfA(pre, "%s%s_", prefix, fd.cFileName);
            if (lstrlenA(pre) < 60)
                n += collect_files(sub, pat, dest, pre, depth - 1);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static int
run_script(void)
{
    int         pc, fails = 0;
    static char buf[1024];

    for (pc = 0; pc < nlines; pc++) {
        char *av[MAXARGS], *cmd;
        int   ac;

        if (!lines[pc][0] || lines[pc][0] == '#' || lines[pc][0] == ':')
            continue;
        expand(lines[pc], buf, sizeof(buf));
        alog("CMD   %s", buf);
        ac = split(buf, av);
        if (!ac)
            continue;
        cmd = av[0];
#define IS(x) (!lstrcmpiA(cmd, x))
#define ARG(i, d) (ac > (i) ? av[i] : (d))
        if (IS("run")) {
            STARTUPINFOA        si;
            PROCESS_INFORMATION pi;

            memset(&si, 0, sizeof(si));
            si.cb = sizeof(si);
            if (proc)
                CloseHandle(proc);
            proc = NULL;
            procid = 0;
            if (CreateProcessA(NULL, av[1], NULL, NULL, FALSE, 0, NULL, ac > 2 ? av[2] : NULL, &si, &pi)) {
                proc   = pi.hProcess;
                procid = pi.dwProcessId;
                CloseHandle(pi.hThread);
                WaitForInputIdle(proc, 10000);
            } else {
                alog("ERROR run failed (%d)", GetLastError());
                fails++;
            }
        } else if (IS("wait") || IS("waitgone")) {
            HWND h = wait_win(ARG(1, "*"), atoi_(ARG(2, "60")), IS("waitgone"));

            if (!h) {
                alog("ERROR timeout waiting for \"%s\"", ARG(1, ""));
                shot_rect("timeout", &(RECT) { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) });
                dump();
                fails++;
            } else if (IS("wait")) {
                cur = h;
                GetWindowTextA(h, buf, 256);
                alog("FOUND \"%s\"", buf);
            }
        } else if (IS("waitmine")) {
            DWORD end = GetTickCount() + atoi_(ARG(1, "60")) * 1000;
            HWND  h;

            while (!(h = find_mine()) && (int) (end - GetTickCount()) > 0)
                idle(250);
            if (h) {
                cur = h;
                GetWindowTextA(h, buf, 256);
                alog("FOUND \"%s\"", buf);
            } else {
                alog("ERROR no window from the started program");
                fails++;
            }
        } else if (IS("waitmode")) {
            /* a full-screen benchmark ran when the screen left the desktop
               mode; it is over once the desktop mode has been back for 10 s */
            DWORD end = GetTickCount() + atoi_(ARG(1, "3600")) * 1000, back = 0;
            int   left = 0;

            for (;;) {
                int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN), b = screen_bpp();

                if (w != desk_w || h != desk_h || b != desk_bpp) {
                    if (!left)
                        alog("NOTE  full screen %dx%dx%d", w, h, b);
                    left = 1;
                    back = 0;
                } else if (left) {
                    if (!back)
                        back = GetTickCount();
                    else if (GetTickCount() - back > 10000)
                        break;
                }
                if ((int) (end - GetTickCount()) < 0) {
                    alog("ERROR waitmode timeout (%s)", left ? "still full screen" : "never went full screen");
                    fails++;
                    break;
                }
                Sleep(1000);
            }
        } else if (IS("waitexit")) {
            DWORD end = GetTickCount() + atoi_(ARG(1, "600")) * 1000;

            while (proc && WaitForSingleObject(proc, 200) == WAIT_TIMEOUT && (int) (end - GetTickCount()) > 0)
                watch_dialogs(NULL);
            if (proc && WaitForSingleObject(proc, 0) == WAIT_TIMEOUT) {
                alog("ERROR program still running");
                fails++;
            }
        } else if (IS("waittext")) {
            DWORD end = GetTickCount() + atoi_(ARG(2, "60")) * 1000;
            static char txt[4096];
            int         ok = 0;

            do {
                if (cur && IsWindow(cur)) {
                    all_text(cur, txt, sizeof(txt));
                    if (contains(txt, av[1])) {
                        ok = 1;
                        break;
                    }
                }
                idle(500);
            } while ((int) (end - GetTickCount()) > 0);
            if (!ok) {
                alog("ERROR timeout waiting for text \"%s\"", av[1]);
                fails++;
            }
        } else if (IS("ifwin")) {
            if (find_top(av[1]) && (pc = find_label(av[2])) < 0)
                return 1;
        } else if (IS("ifexist")) {
            if (GetFileAttributesA(av[1]) != 0xffffffff && (pc = find_label(av[2])) < 0)
                return 1;
        } else if (IS("find")) {
            /* find NAME [ROOT] [LABEL-if-missing]: %F% = full path */
            found[0] = 0;
            if (find_file(ARG(2, "C:"), av[1], 5))
                alog("FOUND %s", found);
            else {
                alog("NOTE  %s not found", av[1]);
                if (ac > 3 && (pc = find_label(av[3])) < 0)
                    return 1;
            }
        } else if (IS("copy")) {
            /* files copied from a CD are read-only: clear that, before and after */
            SetFileAttributesA(av[2], FILE_ATTRIBUTE_NORMAL);
            if (CopyFileA(av[1], av[2], FALSE)) {
                SetFileAttributesA(av[2], FILE_ATTRIBUTE_NORMAL);
                alog("NOTE  copied to %s", av[2]);
            }
            else {
                alog("ERROR copy %s failed (%d)", av[1], GetLastError());
                fails++;
            }
        } else if (IS("copyx")) {
            /* copyx SRC DEST: copy a text file with %CD1% %CD2% %HCT% %WIN% ...
               filled in (e.g. a WinBench .ZDR that must name the CD drive) */
            HANDLE xf = CreateFileA(av[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

            if (xf == INVALID_HANDLE_VALUE) {
                alog("ERROR copyx: cannot read %s", av[1]);
                fails++;
            } else {
                DWORD xs = GetFileSize(xf, NULL), xn;
                char *xin = (char *) GlobalAlloc(GPTR, xs + 1), *xout = (char *) GlobalAlloc(GPTR, xs * 2 + 4096);

                ReadFile(xf, xin, xs, &xn, NULL);
                CloseHandle(xf);
                expand(xin, xout, xs * 2 + 4096);
                SetFileAttributesA(av[2], FILE_ATTRIBUTE_NORMAL);
                xf = CreateFileA(av[2], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (xf != INVALID_HANDLE_VALUE) {
                    WriteFile(xf, xout, lstrlenA(xout), &xn, NULL);
                    CloseHandle(xf);
                    alog("NOTE  wrote %s", av[2]);
                } else {
                    alog("ERROR copyx: cannot write %s", av[2]);
                    fails++;
                }
                GlobalFree(xin);
                GlobalFree(xout);
            }
        } else if (IS("copydir")) {
            /* copydir SRCDIR DESTDIR [PATTERN]: files only, flat, DEST created */
            WIN32_FIND_DATAA fd;
            HANDLE           fh;
            char             pat[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
            int              n = 0;

            CreateDirectoryA(av[2], NULL);
            wsprintfA(pat, "%s\\%s", av[1], ARG(3, "*.*"));
            fh = FindFirstFileA(pat, &fd);
            if (fh != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        continue;
                    wsprintfA(a, "%s\\%s", av[1], fd.cFileName);
                    wsprintfA(b, "%s\\%s", av[2], fd.cFileName);
                    SetFileAttributesA(b, FILE_ATTRIBUTE_NORMAL);
                    if (CopyFileA(a, b, FALSE)) {
                        SetFileAttributesA(b, FILE_ATTRIBUTE_NORMAL); /* CD files are read-only */
                        n++;
                    }
                } while (FindNextFileA(fh, &fd));
                FindClose(fh);
            }
            alog("NOTE  copied %d files from %s", n, av[1]);
            if (!n) {
                alog("ERROR copydir: nothing copied from %s", av[1]);
                fails++;
            }
        } else if (IS("open")) {
            /* a document through its file association; waitexit waits for it */
            SHELLEXECUTEINFOA se;

            memset(&se, 0, sizeof(se));
            se.cbSize       = sizeof(se);
            se.fMask        = SEE_MASK_NOCLOSEPROCESS;
            se.lpVerb       = "open";
            se.lpFile       = av[1];
            se.lpDirectory  = ac > 2 ? av[2] : NULL;
            se.nShow        = SW_SHOWNORMAL;
            if (proc)
                CloseHandle(proc);
            proc = NULL;
            if (ShellExecuteExA(&se) && se.hProcess) {
                proc = se.hProcess;
                WaitForInputIdle(proc, 10000);
            } else {
                alog("ERROR open failed (%d)", GetLastError());
                fails++;
            }
        } else if (IS("closeclass")) {
            /* WM_CLOSE to every visible top-level window of a class */
            HWND h = NULL;
            int  n = 0;

            while ((h = FindWindowExA(NULL, h, av[1], NULL)) != NULL)
                if (IsWindowVisible(h)) {
                    PostMessageA(h, WM_CLOSE, 0, 0);
                    n++;
                }
            alog("NOTE  closed %d %s windows", n, av[1]);
            Sleep(500);
        } else if (IS("dir")) {
            WIN32_FIND_DATAA fd;
            HANDLE           fh = FindFirstFileA(av[1], &fd);

            if (fh != INVALID_HANDLE_VALUE) {
                do
                    alog("DIR   %s %s %d", fd.cFileName,
                         (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "<dir>" : "", fd.nFileSizeLow);
                while (FindNextFileA(fh, &fd));
                FindClose(fh);
            } else
                alog("DIR   (nothing matches %s)", av[1]);
        } else if (IS("runonce")) {
            /* run CMDLINE once at the next logon (HKLM ... RunOnce) */
            HKEY k;

            if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 0, NULL, 0,
                                KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
                RegSetValueExA(k, ARG(2, "CardtestResume"), 0, REG_SZ, (const BYTE *) av[1], lstrlenA(av[1]) + 1);
                RegCloseKey(k);
                alog("NOTE  RunOnce %s", av[1]);
            } else {
                alog("ERROR cannot write RunOnce");
                fails++;
            }
        } else if (IS("regdelrun")) {
            /* regdelrun PATTERN: delete the Run / RunOnce values (machine and
               user) whose command contains PATTERN -- e.g. the resume entry a
               crashed ZD batch run leaves behind */
            static const char *rk[2] = { "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                         "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce" };
            HKEY                roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
            int                 ri, ki, n = 0;

            for (ri = 0; ri < 2; ri++)
                for (ki = 0; ki < 2; ki++) {
                    HKEY  k;
                    DWORD idx = 0;

                    if (RegOpenKeyExA(roots[ri], rk[ki], 0, KEY_READ | KEY_SET_VALUE, &k) != ERROR_SUCCESS)
                        continue;
                    for (;;) {
                        static char vn[256], vd[1024];
                        DWORD       nl = sizeof(vn), dl = sizeof(vd) - 1, ty;

                        if (RegEnumValueA(k, idx, vn, &nl, NULL, &ty, (BYTE *) vd, &dl) != ERROR_SUCCESS)
                            break;
                        vd[dl] = 0;
                        if (ty == REG_SZ && contains(vd, av[1])) {
                            RegDeleteValueA(k, vn);
                            alog("NOTE  deleted %s\\%s\\%s = %s", ri ? "HKCU" : "HKLM", rk[ki], vn, vd);
                            n++;
                        } else
                            idx++;
                    }
                    RegCloseKey(k);
                }
            if (!n)
                alog("NOTE  no Run/RunOnce value with \"%s\"", av[1]);
        } else if (IS("reboot") || IS("shutdown")) {
            /* NT needs the shutdown privilege; Windows 9x ignores the token calls */
            HANDLE           tok;
            TOKEN_PRIVILEGES tp;

            if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
                LookupPrivilegeValueA(NULL, "SeShutdownPrivilege", &tp.Privileges[0].Luid);
                tp.PrivilegeCount           = 1;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
                CloseHandle(tok);
            }
            /* C:\CTRES\GUI\REBOOT.FLG tells RUNGUI.BAT not to start the next
               script while Windows goes down (start /w returns when we die) */
            {
                HANDLE fl = CreateFileA("C:\\CTRES\\GUI\\REBOOT.FLG", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, NULL);
                if (fl != INVALID_HANDLE_VALUE)
                    CloseHandle(fl);
            }
            alog("NOTE  %s", IS("reboot") ? "rebooting" : "shutting down");
            alog("DONE  %s", IS("reboot") ? "reboot" : "shutdown");
            FlushFileBuffers(ct_logf);
            CloseHandle(ct_logf);
            ct_logf = INVALID_HANDLE_VALUE;
            ExitWindowsEx((IS("reboot") ? EWX_REBOOT : (EWX_SHUTDOWN | EWX_POWEROFF)) | EWX_FORCE, 0);
            Sleep(60000);
            ExitProcess(0);
        } else if (IS("goto")) {
            if ((pc = find_label(av[1])) < 0)
                return 1;
        } else if (IS("click") || IS("check")) {
            HWND b = cur ? find_child(cur, av[1]) : NULL;

            if (!b) {
                alog("ERROR no control \"%s\"", av[1]);
                if (cur)
                    shot_win(cur, "nocontrol");
                fails++;
            } else if (IS("click"))
                click_button(b);
            else
                set_check(b, atoi_(ARG(2, "1")));
        } else if (IS("settext")) {
            HWND e = cur ? find_nth(cur, "Edit", atoi_(av[1])) : NULL;

            if (e)
                SendMessageA(e, WM_SETTEXT, 0, (LPARAM) av[2]);
            else {
                alog("ERROR no edit box %s", av[1]);
                fails++;
            }
        } else if (IS("keys")) {
            send_keys(av[1]);
        } else if (IS("clickxy")) {
            POINT p;

            p.x = atoi_(av[1]);
            p.y = atoi_(av[2]);
            if (cur) {
                SetForegroundWindow(cur);
                ClientToScreen(cur, &p);
            }
            SetCursorPos(p.x, p.y);
            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            Sleep(80);
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            Sleep(300);
        } else if (IS("read")) {
            static char txt[4096];

            if (cur) {
                all_text(cur, txt, sizeof(txt));
                alog("READ  %s: %s", ARG(1, "window"), txt);
            }
        } else if (IS("collect")) {
            /* collect ROOT PATTERN: copy matching files below ROOT into %OUT% */
            int n = collect_files(av[1], ARG(2, "*.log"), outdir, "", 6);

            alog("NOTE  collected %d files (%s) from %s", n, ARG(2, "*.log"), av[1]);
        } else if (IS("waitfile")) {
            DWORD end = GetTickCount() + atoi_(ARG(2, "3600")) * 1000;

            while (GetFileAttributesA(av[1]) == 0xffffffff && (int) (end - GetTickCount()) > 0)
                idle(1000);
            if (GetFileAttributesA(av[1]) == 0xffffffff) {
                alog("ERROR timeout waiting for file %s", av[1]);
                fails++;
            } else
                alog("FOUND %s", av[1]);
        } else if (IS("choose")) {
            /* choose TEXT [N]: select TEXT in the Nth combo box (default 1)
               and tell its owner, as a user's choice would */
            HWND cb = cur ? find_nth(cur, "ComboBox", atoi_(ARG(2, "1"))) : NULL;
            LRESULT r = cb ? SendMessageA(cb, CB_SELECTSTRING, (WPARAM) -1, (LPARAM) av[1]) : CB_ERR;

            if (r == CB_ERR) {
                alog("ERROR cannot choose \"%s\"", av[1]);
                fails++;
            } else {
                SendMessageA(GetParent(cb), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(cb), CBN_SELCHANGE), (LPARAM) cb);
                alog("NOTE  chose [%d] %s", (int) r, av[1]);
            }
        } else if (IS("mode")) {
            /* mode W H [BPP]: desktop resolution (not saved in the registry);
               mode restore: back to the one before the first mode command */
            static DEVMODEA saved_dm;
            static int      have_saved;
            DEVMODEA        dm;
            LONG            r;

            if (!have_saved) {
                memset(&saved_dm, 0, sizeof(saved_dm));
                saved_dm.dmSize = sizeof(saved_dm);
                EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &saved_dm);
                saved_dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
                have_saved = 1;
            }
            if (!lstrcmpiA(av[1], "restore")) {
                r = ChangeDisplaySettingsA(&saved_dm, 0);
                alog("NOTE  mode restore %dx%dx%d: %d", (int) saved_dm.dmPelsWidth, (int) saved_dm.dmPelsHeight,
                     (int) saved_dm.dmBitsPerPel, (int) r);
            } else {
                memset(&dm, 0, sizeof(dm));
                dm.dmSize       = sizeof(dm);
                dm.dmPelsWidth  = atoi_(av[1]);
                dm.dmPelsHeight = atoi_(ARG(2, "0"));
                dm.dmBitsPerPel = atoi_(ARG(3, "0"));
                dm.dmFields     = DM_PELSWIDTH | DM_PELSHEIGHT | (dm.dmBitsPerPel ? DM_BITSPERPEL : 0);
                r               = ChangeDisplaySettingsA(&dm, 0);
                if (r == DISP_CHANGE_SUCCESSFUL)
                    alog("NOTE  mode %sx%s %s", av[1], ARG(2, ""), ARG(3, ""));
                else {
                    alog("ERROR mode %sx%s %s failed (%d)", av[1], ARG(2, ""), ARG(3, ""), (int) r);
                    fails++;
                }
            }
            Sleep(3000);
        } else if (IS("taskbar")) {
            /* taskbar off: hidden, not topmost, and the work area is the whole
               screen (WinBench and the DCT refuse to run under an always-on-top
               taskbar; ABM_SETSTATE only exists from Windows XP on);
               taskbar restore: as it was */
            static int  saved_top = -1, saved_vis;
            static RECT saved_work;
            HWND        tray = FindWindowA("Shell_TrayWnd", NULL);

            if (!tray) {
                alog("NOTE  no taskbar");
            } else if (lstrcmpiA(ARG(1, "off"), "restore")) {
                RECT full;

                if (saved_top < 0) {
                    saved_top = (GetWindowLongA(tray, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
                    saved_vis = IsWindowVisible(tray);
                    SystemParametersInfoA(SPI_GETWORKAREA, 0, &saved_work, 0);
                }
                SetWindowPos(tray, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                ShowWindow(tray, SW_HIDE);
                SetRect(&full, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
                SystemParametersInfoA(SPI_SETWORKAREA, 0, &full, SPIF_SENDCHANGE);
                alog("NOTE  taskbar hidden (was topmost %d, visible %d)", saved_top, saved_vis);
            } else if (saved_top >= 0) {
                SystemParametersInfoA(SPI_SETWORKAREA, 0, &saved_work, SPIF_SENDCHANGE);
                if (saved_vis)
                    ShowWindow(tray, SW_SHOWNA);
                SetWindowPos(tray, saved_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                alog("NOTE  taskbar restored");
            }
        } else if (IS("readlist")) {
            if (cur)
                EnumChildWindows(cur, list_cb, 0);
        } else if (IS("shot")) {
            shot_win(cur, ARG(1, "shot"));
        } else if (IS("shotscreen")) {
            RECT r;

            SetRect(&r, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
            shot_rect(ARG(1, "screen"), &r);
        } else if (IS("close")) {
            if (cur)
                PostMessageA(cur, WM_CLOSE, 0, 0);
            Sleep(500);
        } else if (IS("kill")) {
            if (proc)
                TerminateProcess(proc, 1);
        } else if (IS("pause")) {
            /* kernel-only sleep: no window calls, so it never waits on a
               full-screen DirectX program holding the Win16 lock */
            Sleep(atoi_(av[1]) * 1000);
        } else if (IS("grep")) {
            /* grep NAME FILE TEXT: RESULT NAME <line> for each line containing TEXT */
            WIN32_FIND_DATAA gfd;
            HANDLE           gfh = FindFirstFileA(av[2], &gfd), gf = INVALID_HANDLE_VALUE;
            int              hits = 0;
            char             gdir[MAX_PATH], gpath[MAX_PATH], *gs;

            lstrcpynA(gdir, av[2], sizeof(gdir));
            for (gs = gdir + lstrlenA(gdir); gs > gdir && gs[-1] != '\\' && gs[-1] != ':'; gs--)
                ;
            *gs = 0;
            while (gfh != INVALID_HANDLE_VALUE) {
                wsprintfA(gpath, "%s%s", gdir, gfd.cFileName);
                gf = CreateFileA(gpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                if (gf == INVALID_HANDLE_VALUE) {
                    if (!FindNextFileA(gfh, &gfd)) {
                        FindClose(gfh);
                        gfh = INVALID_HANDLE_VALUE;
                    }
                    continue;
                }
                DWORD gsz = GetFileSize(gf, NULL), gn;
                char *gb  = (char *) GlobalAlloc(GPTR, gsz + 2), *l, *e;

                ReadFile(gf, gb, gsz, &gn, NULL);
                CloseHandle(gf);
                for (l = gb; *l; l = e) {
                    for (e = l; *e && *e != 10; e++) /* '\n' */
                        ;
                    if (*e)
                        *e++ = 0;
                    if (e > l + 1 && e[-2] == '\r')
                        e[-2] = 0;
                    if (contains(l, ARG(3, "")) && lstrlenA(l) < 900) {
                        alog("RESULT %s %s: %s", av[1], gfd.cFileName, l);
                        hits++;
                    }
                }
                GlobalFree(gb);
                if (!FindNextFileA(gfh, &gfd)) {
                    FindClose(gfh);
                    gfh = INVALID_HANDLE_VALUE;
                }
            }
            if (!hits) {
                alog("ERROR grep: no \"%s\" in %s", ARG(3, ""), av[2]);
                fails++;
            }
        } else if (IS("sleep")) {
            idle(atoi_(av[1]) * 1000);
        } else if (IS("wizard")) {
            if (wizard(atoi_(ARG(1, "300"))) < 0)
                fails++;
        } else if (IS("on") || IS("manual")) {
            if (nrules < 32) {
                lstrcpynA(rules[nrules].pattern, av[1], 128);
                lstrcpynA(rules[nrules].button, ARG(2, IS("manual") ? "No" : "OK"), 64);
                rules[nrules].manual = IS("manual");
                nrules++;
            }
        } else if (IS("skip")) {
            alog("CASE  %s SKIP %s", av[1], ARG(2, ""));
        } else if (IS("autodismiss")) {
            autodismiss = atoi_(av[1]);
        } else if (IS("result")) {
            alog("RESULT %s %s", av[1], ARG(2, ""));
        } else if (IS("resultfrom")) {
            /* the control after the one whose caption matches */
            /* the first following control whose text is a number (skips icons
               and other labels) */
            HWND lab = cur ? find_child(cur, av[2]) : NULL;
            HWND val = lab;
            char t[256] = "";
            int  k, ok = 0;

            for (k = 0; val && k < 8 && !ok; k++) {
                val  = GetWindow(val, GW_HWNDNEXT);
                t[0] = 0;
                if (val)
                    SendMessageTimeoutA(val, WM_GETTEXT, sizeof(t), (LPARAM) t, SMTO_ABORTIFHUNG, 500, NULL);
                ok = is_value(t);
            }
            alog("RESULT %s %s", av[1], ok ? t : "(not found)");
            if (!ok) {
                alog("ERROR no value after \"%s\"", av[2]);
                fails++;
            }
        } else if (IS("resultid")) {
            /* resultid NAME ID: RESULT with the text of control ID of the current
               window (for dialogs whose labels are far from their values) */
            HWND val = cur ? find_id(cur, atoi_(av[2])) : NULL;
            char t[256] = "";

            if (val)
                SendMessageTimeoutA(val, WM_GETTEXT, sizeof(t), (LPARAM) t, SMTO_ABORTIFHUNG, 500, NULL);
            alog("RESULT %s %s", av[1], t[0] ? t : "(not found)");
            if (!t[0]) {
                alog("ERROR no control %s", av[2]);
                fails++;
            }
        } else if (IS("log")) {
            alog("NOTE  %s", av[1]);
        } else if (IS("pass") || IS("fail")) {
            alog("CASE  %s %s %s", av[1], IS("pass") ? "PASS" : "FAIL", ARG(2, ""));
            if (IS("fail"))
                fails++;
        } else if (IS("dump")) {
            dump();
        } else {
            alog("ERROR unknown command %s", cmd);
            fails++;
        }
#undef IS
#undef ARG
    }
    return fails ? 1 : 0;
}

void __stdcall
entry(void)
{
    char  *cl = GetCommandLineA(), path[MAX_PATH], name[16], logp[MAX_PATH];
    char  *av[MAXARGS], *data, *p;
    HANDLE f;
    DWORD  size, n;
    int    ac, i, k, dumponly = 0, rc;

    t0     = GetTickCount();
    desk_w = GetSystemMetrics(SM_CXSCREEN);
    desk_h = GetSystemMetrics(SM_CYSCREEN);
    desk_bpp = screen_bpp();
    lstrcpynA(path, cl, sizeof(path));
    ac = split(path, av);
    for (i = 1; i < ac; i++)
        if (!lstrcmpiA(av[i], "/DUMP"))
            dumponly = 1;
    drives();
    GetWindowsDirectoryA(windir, sizeof(windir));
    /* run name = script file name without folder and extension */
    lstrcpyA(name, dumponly && ac < 3 ? "DUMP" : "AUTOGUI");
    if (ac > 1 && av[1][0] != '/') {
        p = av[1] + lstrlenA(av[1]);
        while (p > av[1] && p[-1] != '\\' && p[-1] != ':')
            p--;
        for (k = 0; p[k] && p[k] != '.' && k < 15; k++)
            name[k] = p[k] >= 'a' && p[k] <= 'z' ? p[k] - 32 : p[k];
        name[k] = 0;
    }
    CreateDirectoryA("C:\\CTRES", NULL);
    CreateDirectoryA("C:\\CTRES\\GUI", NULL);
    wsprintfA(outdir, "C:\\CTRES\\GUI\\%s", name);
    CreateDirectoryA(outdir, NULL);
    {
        /* screenshots of an earlier run of this script would mix with ours */
        WIN32_FIND_DATAA fd;
        HANDLE           fh;
        char             pat[MAX_PATH], fn[MAX_PATH];

        wsprintfA(pat, "%s\\*.BMP", outdir);
        if ((fh = FindFirstFileA(pat, &fd)) != INVALID_HANDLE_VALUE) {
            do {
                wsprintfA(fn, "%s\\%s", outdir, fd.cFileName);
                DeleteFileA(fn);
            } while (FindNextFileA(fh, &fd));
            FindClose(fh);
        }
    }
    lstrcpyA(ct_dir, outdir);
    lstrcpyA(ct_name, name);
    wsprintfA(logp, "C:\\CTRES\\GUI\\%s.LOG", name);
    ct_logf = CreateFileA(logp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    alog("INFO  AUTOGUI 1.0 %s (CD1=%s CD2=%s HCT=%s)", name, cd1, cd2, hctcd);
    if (dumponly) {
        dump();
        CloseHandle(ct_logf);
        ExitProcess(0);
    }
    f = CreateFileA(av[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        alog("ERROR cannot open script %s", av[1]);
        CloseHandle(ct_logf);
        ExitProcess(2);
    }
    size = GetFileSize(f, NULL);
    data = (char *) GlobalAlloc(GPTR, size + 2);
    ReadFile(f, data, size, &n, NULL);
    CloseHandle(f);
    /* split into trimmed lines */
    for (p = data; *p && nlines < MAXLINES;) {
        char *e;

        while (*p == ' ' || *p == '\t')
            p++;
        lines[nlines] = p;
        while (*p && *p != '\r' && *p != '\n')
            p++;
        e = p;
        while (*p == '\r' || *p == '\n')
            *p++ = 0;
        while (e > lines[nlines] && (e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        nlines++;
    }
    rc = run_script();
    alog("DONE  %s", rc ? "with errors" : "ok");
    CloseHandle(ct_logf);
    ExitProcess(rc);
}
