/* ctrun -- runs RUNALL.BAT's scripted extras and writes the summary
   (console program: command.com waits for it).

     CTRUN /JOBS file     run every job in file (see EXTRAS.TXT)
     CTRUN /SUMMARY       write C:\CTRES\SUMMARY.TXT from the logs

   A job line is   TYPE|timeout seconds|label|file|pattern|command line
     WAIT  start the command, watch `file` (deleted first) for a line
           containing `pattern`; when it appears, record that line, close
           the program (WM_CLOSE, then TerminateProcess) and go on.
     RUN   start the command with its output going to `file` ("-" = none)
           and wait for it to end.
   Either kind is stopped when the timeout runs out. %CD% in a line is the
   drive the job file is on. Results are appended to C:\CTRES\EXTRAS.LOG:
     EXTRA <label> RESULT <line> | DONE exit=N | TIMEOUT | EXITED | NOSTART
   Programs start in their own folder. */
#include "ct.h"

static HANDLE out;

static void
say(const char *s)
{
    DWORD n;

    WriteFile(out, s, lstrlenA(s), &n, NULL);
    WriteFile(out, "\r\n", 2, &n, NULL);
}

static void
append(const char *file, const char *line)
{
    DWORD  n;
    HANDLE f;

    f = CreateFileA(file, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(f, 0, NULL, FILE_END);
    WriteFile(f, line, lstrlenA(line), &n, NULL);
    WriteFile(f, "\r\n", 2, &n, NULL);
    CloseHandle(f);
}

static void
extra(const char *label, const char *what, const char *detail)
{
    char  line[700];
    HDC   dc  = GetDC(NULL);
    int   bpp = GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES);

    ReleaseDC(NULL, dc);
    wsprintfA(line, "EXTRA %s %s %s (at %dx%dx%d)", label, what, detail, GetSystemMetrics(SM_CXSCREEN),
              GetSystemMetrics(SM_CYSCREEN), bpp);
    append("C:\\CTRES\\EXTRAS.LOG", line);
    say(line);
}

/* Whole file into a new buffer (NUL-terminated), or NULL. Shared read:
   the program writing it may still have it open. */
static char *
slurp(const char *file, DWORD *len)
{
    HANDLE f = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD  size, n = 0;
    char  *b;

    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    size = GetFileSize(f, NULL);
    b    = (char *) GlobalAlloc(GPTR, size + 1);
    if (b)
        ReadFile(f, b, size, &n, NULL);
    CloseHandle(f);
    if (b)
        b[n] = 0;
    if (len)
        *len = n;
    return b;
}

static char
lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Find a line containing pat (case-blind); copy it to line. */
static int
find_line(const char *text, const char *pat, char *line, int size)
{
    const char *s = text, *e;
    int         pl = lstrlenA(pat), i, j, n;

    while (*s) {
        for (e = s; *e && *e != '\n' && *e != '\r'; e++)
            ;
        for (i = 0; s + i + pl <= e; i++) {
            for (j = 0; j < pl && lc(s[i + j]) == lc(pat[j]); j++)
                ;
            if (j == pl) {
                n = (int) (e - s) < size - 1 ? (int) (e - s) : size - 1;
                memcpy(line, s, n);
                line[n] = 0;
                return 1;
            }
        }
        s = e;
        while (*s == '\n' || *s == '\r')
            s++;
    }
    return 0;
}

static DWORD close_pid;

static BOOL CALLBACK
close_cb(HWND h, LPARAM l)
{
    DWORD pid;

    GetWindowThreadProcessId(h, &pid);
    if (pid == close_pid)
        PostMessageA(h, WM_CLOSE, 0, 0);
    return TRUE;
}

static void
stop(PROCESS_INFORMATION *pi)
{
    close_pid = pi->dwProcessId;
    EnumWindows(close_cb, 0);
    if (WaitForSingleObject(pi->hProcess, 10000) != WAIT_OBJECT_0)
        TerminateProcess(pi->hProcess, 1);
    WaitForSingleObject(pi->hProcess, 5000);
}

/* The folder of the program a command line starts. */
static void
exe_dir(const char *cmd, char *dir)
{
    int i = 0, last = -1;

    while (cmd[i] && cmd[i] != ' ' && i < MAX_PATH - 1) {
        dir[i] = cmd[i];
        if (cmd[i] == '\\')
            last = i;
        i++;
    }
    dir[last > 0 ? last : 0] = 0;
    if (last == 2) /* "C:\X.EXE" -> "C:\" */
        lstrcpyA(dir + 2, "\\");
}

static void
job(char *f[6])
{
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE              h   = NULL;
    int                 run = !lstrcmpiA(f[0], "RUN"), t, secs = 0;
    char                dir[MAX_PATH], line[512], *text, num[32];
    DWORD               code = 0, start = GetTickCount();

    t = 0;
    for (text = f[1]; *text >= '0' && *text <= '9'; text++)
        t = t * 10 + *text - '0';
    if (!t)
        t = 600;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (run && lstrcmpA(f[3], "-")) {
        sa.nLength              = sizeof(sa);
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle       = TRUE;
        h = CreateFileA(f[3], GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            si.dwFlags    = STARTF_USESTDHANDLES;
            si.hStdOutput = h;
            si.hStdError  = h;
            si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        } else
            h = NULL;
    }
    if (!run)
        DeleteFileA(f[3]);
    exe_dir(f[5], dir);
    say(f[5]);
    if (!CreateProcessA(NULL, f[5], NULL, NULL, TRUE, 0, NULL, dir[0] ? dir : NULL, &si, &pi)) {
        wsprintfA(line, "error %d: %s", (int) GetLastError(), f[5]);
        extra(f[2], "NOSTART", line);
        if (h)
            CloseHandle(h);
        return;
    }
    if (run) {
        if (WaitForSingleObject(pi.hProcess, t * 1000) != WAIT_OBJECT_0) {
            TerminateProcess(pi.hProcess, 1);
            extra(f[2], "TIMEOUT", f[3]);
        } else {
            GetExitCodeProcess(pi.hProcess, &code);
            wsprintfA(num, "exit=%d secs=%d", (int) code, (int) ((GetTickCount() - start) / 1000));
            extra(f[2], "DONE", num);
        }
    } else {
        for (;;) {
            if (WaitForSingleObject(pi.hProcess, 1000) == WAIT_OBJECT_0) {
                /* one last look: it may have written the line and quit */
                text = slurp(f[3], NULL);
                if (text && find_line(text, f[4], line, sizeof(line)))
                    extra(f[2], "RESULT", line);
                else
                    extra(f[2], "EXITED", "before the result line appeared");
                if (text)
                    GlobalFree(text);
                break;
            }
            if (++secs >= t) {
                stop(&pi);
                extra(f[2], "TIMEOUT", f[3]);
                break;
            }
            text = slurp(f[3], NULL);
            if (text && find_line(text, f[4], line, sizeof(line))) {
                GlobalFree(text);
                Sleep(3000);
                stop(&pi);
                extra(f[2], "RESULT", line);
                break;
            }
            if (text)
                GlobalFree(text);
        }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (h)
        CloseHandle(h);
    Sleep(2000);
}

static void
jobs(const char *file)
{
    char *text = slurp(file, NULL), *p, *e, line[1024], *f[6];
    int   i, n, k;

    if (!text) {
        say("no job file");
        return;
    }
    for (p = text; *p; p = e) {
        for (e = p; *e && *e != '\n'; e++)
            ;
        /* copy the line, expanding %CD% to the job file's drive */
        for (i = 0, n = 0; p + i < e && n < (int) sizeof(line) - 3; i++) {
            if (p[i] == '\r')
                continue;
            if (!memcmp(p + i, "%CD%", 4) && p + i + 4 <= e) {
                line[n++] = file[0];
                line[n++] = ':';
                i += 3;
            } else
                line[n++] = p[i];
        }
        line[n] = 0;
        if (*e)
            e++;
        if (!line[0] || line[0] == '#' || line[0] == ';')
            continue;
        for (k = 0, f[0] = line, i = 0; line[i] && k < 5; i++)
            if (line[i] == '|') {
                line[i] = 0;
                f[++k]  = line + i + 1;
            }
        if (k == 5)
            job(f);
    }
    GlobalFree(text);
}

/* SUMMARY.TXT: per colour depth, each tool's DONE line and its failing
   and warning cases; then the depth changes and the extras. */
static void
tool_summary(const char *dir, const char *tag, const char *logname)
{
    char  path[MAX_PATH], line[700], fails[400], warns[400], done[200], *text, *s, *e;
    int   fl = 0, wl = 0;

    wsprintfA(path, "%s\\%s", dir, logname);
    text = slurp(path, NULL);
    if (!text)
        return;
    fails[0] = warns[0] = 0;
    lstrcpyA(done, "(no DONE line: crashed or still running?)");
    for (s = text; *s; s = e) {
        for (e = s; *e && *e != '\n'; e++)
            ;
        if (e - s > 6 && !memcmp(s, "CASE  ", 6)) {
            char  name[40];
            char *q = s + 6;
            int   i = 0;

            while (q < e && *q != ' ' && i < 39)
                name[i++] = *q++;
            name[i] = 0;
            while (q < e && *q == ' ')
                q++;
            if (e - q >= 4 && !memcmp(q, "FAIL", 4) && fl < 360) {
                lstrcatA(fails, " ");
                lstrcatA(fails, name);
                fl += i + 1;
            } else if (e - q >= 4 && !memcmp(q, "WARN", 4) && wl < 360) {
                lstrcatA(warns, " ");
                lstrcatA(warns, name);
                wl += i + 1;
            }
        } else if (e - s > 6 && !memcmp(s, "DONE  ", 6)) {
            int n = (int) (e - s - 6) < 199 ? (int) (e - s - 6) : 199;

            memcpy(done, s + 6, n);
            done[n] = 0;
            if (n && done[n - 1] == '\r')
                done[n - 1] = 0;
        }
        if (*e)
            e++;
    }
    GlobalFree(text);
    lstrcpyA(path, logname);
    path[lstrlenA(path) - 4] = 0;
    wsprintfA(line, "%-6s %-7s %s", tag, path, done);
    if (fails[0]) {
        lstrcatA(line, "  FAIL:");
        lstrcatA(line, fails);
    }
    if (warns[0]) {
        lstrcatA(line, "  WARN:");
        lstrcatA(line, warns);
    }
    append("C:\\CTRES\\SUMMARY.TXT", line);
    say(line);
}

static void
dir_summary(const char *dir, const char *tag)
{
    static const char *tools[] = { "VGACHK.LOG", "VBECHK.LOG", "GDICHK.LOG", "DDCHK.LOG", "D3DCHK.LOG", "GLCHK.LOG" };
    int                i;

    for (i = 0; i < 6; i++)
        tool_summary(dir, tag, tools[i]);
}

static void
copy_lines(const char *file)
{
    char *text = slurp(file, NULL), *s, *e, line[700];
    int   n;

    if (!text)
        return;
    for (s = text; *s; s = e) {
        for (e = s; *e && *e != '\n'; e++)
            ;
        n = (int) (e - s) < 699 ? (int) (e - s) : 699;
        memcpy(line, s, n);
        line[n] = 0;
        if (n && line[n - 1] == '\r')
            line[n - 1] = 0;
        if (line[0])
            append("C:\\CTRES\\SUMMARY.TXT", line);
        if (*e)
            e++;
    }
    GlobalFree(text);
}

/* glean's output: "test:  PASS|FAIL <pixel format>" lines. */
static void
glean_summary(void)
{
    char *text = slurp("C:\\CTRES\\GLEAN.TXT", NULL), *s, *e, line[700], fails[400], name[40];
    int   pass = 0, fail = 0, i;

    if (!text)
        return;
    fails[0] = 0;
    for (s = text; *s; s = e) {
        for (e = s; *e && *e != '\n'; e++)
            ;
        for (i = 0; s + i < e && s[i] != ':' && s[i] != ' ' && i < 39; i++)
            name[i] = s[i];
        name[i] = 0;
        if (s + i + 7 <= e && s[i] == ':' && !memcmp(s + i + 1, "  PASS", 6))
            pass++;
        else if (s + i + 7 <= e && s[i] == ':' && !memcmp(s + i + 1, "  FAIL", 6)) {
            fail++;
            /* each failing test once ("fails" is " a b c " style) */
            if (lstrlenA(fails) < 350) {
                char key[44];

                wsprintfA(key, " %s ", name);
                if (!fails[0])
                    lstrcpyA(fails, " ");
                if (!find_line(fails, key, line, sizeof(line))) {
                    lstrcatA(fails, name);
                    lstrcatA(fails, " ");
                }
            }
        }
        if (*e)
            e++;
    }
    GlobalFree(text);
    wsprintfA(line, "GLEAN  results=%d pass=%d fail=%d%s%s", pass + fail, pass, fail, fails[0] ? "  FAIL:" : "",
              fails);
    append("C:\\CTRES\\SUMMARY.TXT", line);
    say(line);
}

static void
summary(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    SYSTEMTIME       st;
    OSVERSIONINFOA   ov;
    char             line[300], dir[MAX_PATH];
    static char      names[16][MAX_PATH];
    int              n = 0, i, j;

    DeleteFileA("C:\\CTRES\\SUMMARY.TXT");
    GetLocalTime(&st);
    ov.dwOSVersionInfoSize = sizeof(ov);
    GetVersionExA(&ov);
    wsprintfA(line, "CARDTEST SUMMARY %04d-%02d-%02d %02d:%02d  os %d.%d.%d platform %d", st.wYear, st.wMonth,
              st.wDay, st.wHour, st.wMinute, ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber & 0xffff,
              ov.dwPlatformId);
    append("C:\\CTRES\\SUMMARY.TXT", line);
    dir_summary("C:\\CTRES", "-");
    /* the per-depth folders, in numeric order */
    h = FindFirstFileA("C:\\CTRES\\*BPP", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < 16)
                lstrcpyA(names[n++], fd.cFileName);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (lstrlenA(names[j]) < lstrlenA(names[i]) ||
                (lstrlenA(names[j]) == lstrlenA(names[i]) && lstrcmpiA(names[j], names[i]) < 0)) {
                lstrcpyA(dir, names[i]);
                lstrcpyA(names[i], names[j]);
                lstrcpyA(names[j], dir);
            }
    for (i = 0; i < n; i++) {
        wsprintfA(dir, "C:\\CTRES\\%s", names[i]);
        dir_summary(dir, names[i]);
    }
    copy_lines("C:\\CTRES\\DEPTH.LOG");
    copy_lines("C:\\CTRES\\EXTRAS.LOG");
    glean_summary();
    append("C:\\CTRES\\SUMMARY.TXT", "END");
}

void __stdcall
entry(void)
{
    const char *p = GetCommandLineA();
    char        a[MAX_PATH];
    int         q = 0, n = 0;

    out = GetStdHandle(STD_OUTPUT_HANDLE);
    CreateDirectoryA("C:\\CTRES", NULL);
    /* skip the program name */
    while (*p && (q || (*p != ' ' && *p != '\t'))) {
        if (*p == '"')
            q = !q;
        p++;
    }
    while (*p == ' ' || *p == '\t')
        p++;
    while (*p && *p != ' ' && n < MAX_PATH - 1)
        a[n++] = *p++;
    a[n] = 0;
    while (*p == ' ')
        p++;
    CharUpperA(a);
    if (!lstrcmpA(a, "/JOBS") && *p) {
        lstrcpynA(a, p, MAX_PATH);
        for (n = lstrlenA(a); n && (a[n - 1] == ' ' || a[n - 1] == '\r'); n--)
            a[n - 1] = 0;
        jobs(a);
    } else if (!lstrcmpA(a, "/SUMMARY"))
        summary();
    else {
        say("CTRUN /JOBS file | /SUMMARY");
        ExitProcess(1);
    }
    ExitProcess(0);
}
