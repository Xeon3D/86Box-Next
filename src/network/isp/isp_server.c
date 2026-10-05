/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             isp-server: the virtual ISP.  Set a COM port modem's line to
 *             "Dial out to a TCP/IP host", 127.0.0.1, port 2323 (the modem
 *             menu's "Dial the ISP" does it), and any number the guest dials
 *             reaches this: a PPP server that gives the guest an address and
 *             DNS and NATs it to the host's Internet.  Each connection is one
 *             call, with a PPP session, a /24 and a libslirp instance of its
 *             own; calls can reach each other (guest LAN).
 *
 *             Its window is a web page, http://127.0.0.1:2324/, opened in
 *             the browser at start (isp_web.c): the calls, hang-up, port
 *             forwards, settings.  Settings and forwards are kept in
 *             isp-server.ini next to the program.
 *
 *             The bytes after CONNECT are PPP and nothing else: no Telnet,
 *             no login prompt.  The modem connects when the dial starts and
 *             the guest only starts PPP after CONNECT, so a session waits,
 *             without timers, for the guest's first LCP frame.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    include <windows.h>
#    include <shellapi.h>
typedef int socklen_t;
#    define sock_error()    WSAGetLastError()
#    define SOCK_WOULDBLOCK WSAEWOULDBLOCK
#    define sock_close(s)   closesocket(s)
#else
#    include <arpa/inet.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#    include <poll.h>
#    include <sys/socket.h>
#    include <unistd.h>
typedef int SOCKET;
#    define INVALID_SOCKET  (-1)
#    define SOCKET_ERROR    (-1)
#    define sock_error()    errno
#    define SOCK_WOULDBLOCK EWOULDBLOCK
#    define sock_close(s)   close(s)
#endif
#include "isp.h"
#include "isp_plat.h"
#include "isp_web.h"

#define MAX_CONNS 64
#define IO_CHUNK  16384
#define HTTP_MAX  65536

typedef struct conn {
    int           id;
    SOCKET        sock;
    char          peer[64];
    isp_event_t  *wake;
    isp_thread_t *thread;
    volatile int  done;
} conn_t;

static volatile int stop_requested;
static isp_mutex_t *conns_lock;
static conn_t      *conns[MAX_CONNS];
static int          quiet;

/* What the page and the .ini know besides the ISP's own settings. */
static isp_mutex_t *srv_lock;
static char         cfg_path[1024];
static char         listen_addr[64] = "127.0.0.1";
static int          listen_port     = 2323;
static int          http_port       = 2324;
static char         fwd_spec[ISP_MAX_SESSIONS + 1][256]; /* by call number */

static void
logf_(const char *fmt, ...)
{
    char       stamp[32];
    time_t     t = time(NULL);
    struct tm *tm;
    va_list    ap;
    char       buf[512];

    tm = localtime(&t);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm);
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s  %s\n", stamp, buf);
    fflush(stdout);
}

/* -------------------------------------------------------------- the .ini */

static void
ini_load(void)
{
    FILE          *f = fopen(cfg_path, "r");
    isp_settings_t st;
    char           line[512];

    if (f == NULL)
        return;
    isp_get_settings(&st);
    while (fgets(line, sizeof(line), f) != NULL) {
        char *eq = strchr(line, '=');
        char *key;
        char *val;
        char *end;

        if ((line[0] == '#') || (line[0] == ';') || (line[0] == '[') || (eq == NULL))
            continue;
        *eq = '\0';
        key = line;
        val = eq + 1;
        while ((*key == ' ') || (*key == '\t'))
            key++;
        for (end = eq - 1; (end >= key) && ((*end == ' ') || (*end == '\t')); end--)
            *end = '\0';
        while ((*val == ' ') || (*val == '\t'))
            val++;
        val[strcspn(val, "\r\n")] = '\0';

        if (!strcmp(key, "listen"))
            snprintf(listen_addr, sizeof(listen_addr), "%s", val);
        else if (!strcmp(key, "port"))
            listen_port = atoi(val);
        else if (!strcmp(key, "http_port"))
            http_port = atoi(val);
        else if (!strcmp(key, "net")) {
            unsigned a, b;

            if ((sscanf(val, "%u.%u.", &a, &b) == 2) && (a < 256) && (b < 256))
                st.base_net = (a << 24) | (b << 16);
        } else if (!strcmp(key, "max"))
            st.max_sessions = atoi(val);
        else if (!strcmp(key, "pap"))
            st.require_pap = !!atoi(val);
        else if (!strcmp(key, "echo"))
            st.echo_secs = (uint32_t) atoi(val);
        else if (!strcmp(key, "lan"))
            st.guest_lan = !!atoi(val);
        else if (!strcmp(key, "throttle"))
            st.throttle = !!atoi(val);
        else if (!strcmp(key, "rate"))
            st.default_rate = (uint32_t) atoi(val);
        else if (!strncmp(key, "forward.", 8)) {
            const int     n = atoi(key + 8);
            isp_forward_t fwd[ISP_MAX_FORWARDS];

            if ((n >= 1) && (n <= ISP_MAX_SESSIONS))
                isp_forwards_format(fwd, isp_forwards_parse(val, fwd, ISP_MAX_FORWARDS), fwd_spec[n],
                                    sizeof(fwd_spec[n]));
        }
    }
    fclose(f);
    if ((st.max_sessions < 1) || (st.max_sessions > MAX_CONNS))
        st.max_sessions = MAX_CONNS;
    isp_set_settings(&st);
}

static int
ini_save(void)
{
    FILE          *f;
    isp_settings_t st;

    isp_get_settings(&st);
    isp_mutex_lock(srv_lock);
    f = fopen(cfg_path, "w");
    if (f != NULL) {
        fprintf(f, "# isp-server: the 86Box-Next virtual ISP.  Written by the status page.\n"
                   "[isp]\n");
        fprintf(f, "listen = %s\nport = %d\nhttp_port = %d\n", listen_addr, listen_port, http_port);
        fprintf(f, "net = %u.%u.0.0\nmax = %d\npap = %d\necho = %u\nlan = %d\nthrottle = %d\nrate = %u\n",
                st.base_net >> 24, (st.base_net >> 16) & 0xff, st.max_sessions, st.require_pap, st.echo_secs,
                st.guest_lan, st.throttle, st.default_rate);
        for (int n = 1; n <= ISP_MAX_SESSIONS; n++)
            if (fwd_spec[n][0] != '\0')
                fprintf(f, "forward.%d = %s\n", n, fwd_spec[n]);
        fclose(f);
    }
    isp_mutex_unlock(srv_lock);
    if (f == NULL)
        logf_("cannot write %s", cfg_path);
    return f != NULL;
}

/* ------------------------------------------------ what the page asks of us */

void
isp_srv_get_forwards(int number, char *spec, size_t len)
{
    spec[0] = '\0';
    if ((number < 1) || (number > ISP_MAX_SESSIONS))
        return;
    isp_mutex_lock(srv_lock);
    snprintf(spec, len, "%s", fwd_spec[number]);
    isp_mutex_unlock(srv_lock);
}

int
isp_srv_set_forwards(int number, const char *spec)
{
    isp_forward_t fwd[ISP_MAX_FORWARDS];
    int           n;

    if ((number < 1) || (number > ISP_MAX_SESSIONS))
        return 0;
    isp_mutex_lock(srv_lock);
    snprintf(fwd_spec[number], sizeof(fwd_spec[number]), "%s", spec);
    isp_mutex_unlock(srv_lock);
    n = isp_forwards_parse(spec, fwd, ISP_MAX_FORWARDS);
    isp_set_call_forwards(number, fwd, n); /* if that call is up */
    logf_("call %d forwards: %s", number, spec[0] ? spec : "none");
    return ini_save();
}

void
isp_srv_settings_changed(void)
{
    isp_settings_t st;

    isp_get_settings(&st);
    logf_("settings: %u.%u.0.0, up to %d calls, %s, keepalive %u s, guest LAN %s, %s",
          st.base_net >> 24, (st.base_net >> 16) & 0xff, st.max_sessions,
          st.require_pap ? "PAP" : "no authentication", st.echo_secs, st.guest_lan ? "on" : "off",
          st.throttle ? "held to modem speed" : "line speed");
    ini_save();
}

void
isp_srv_endpoint(char *listen, size_t len, int *port)
{
    snprintf(listen, len, "%s", listen_addr);
    *port = listen_port;
}

/* ------------------------------------------------------------ the calls */

static void
session_log(void *opaque, const char *msg)
{
    conn_t *c = (conn_t *) opaque;

    if (!quiet || strstr(msg, "IPCP: up") || strstr(msg, "finished") || strstr(msg, "failed") ||
        strstr(msg, "hung up") || strstr(msg, "forward"))
        logf_("[conn %d] %s", c->id, msg);
}

static void
session_notify(void *opaque)
{
    isp_event_set(((conn_t *) opaque)->wake);
}

static int
set_nonblocking(SOCKET s)
{
#ifdef _WIN32
    u_long yes = 1;

    return ioctlsocket(s, FIONBIO, &yes) == 0;
#else
    const int fl = fcntl(s, F_GETFL, 0);

    return (fl >= 0) && (fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0);
#endif
}

/* One call, from accept to hang-up. */
static void
conn_thread(void *arg)
{
    conn_t                 *c  = (conn_t *) arg;
    isp_session_callbacks_t cb = { session_notify, session_log, c };
    isp_session_t          *s;
    char                    err[128];
    const char             *why = "the ISP is shutting down";
    uint8_t                *in  = (uint8_t *) malloc(IO_CHUNK);
    uint8_t                *out = (uint8_t *) malloc(IO_CHUNK);
    size_t                  in_off = 0, in_len = 0;
    size_t                  out_off = 0, out_len = 0;
    int                     eof = 0;
#ifdef _WIN32
    WSAEVENT sock_ev = WSACreateEvent();
#endif

    s = ((in != NULL) && (out != NULL)) ? isp_session_open(&cb, err, sizeof(err)) : NULL;
    if (s == NULL) {
        logf_("[conn %d] refused: %s", c->id, (in && out) ? err : "out of memory");
        goto done;
    }
    isp_session_set_label(s, c->peer);
    {
        char          spec[256];
        isp_forward_t fwd[ISP_MAX_FORWARDS];

        isp_srv_get_forwards(isp_session_number(s), spec, sizeof(spec));
        if (spec[0] != '\0')
            isp_session_set_forwards(s, fwd, isp_forwards_parse(spec, fwd, ISP_MAX_FORWARDS));
    }
#ifdef _WIN32
    WSAEventSelect(c->sock, sock_ev, FD_READ | FD_WRITE | FD_CLOSE);
#endif

    while (!stop_requested) {
        int blocked_in = 0;
        int failed     = 0;

        /* The guest's bytes, into the session.  When it will take no more
           they stay in the socket, and TCP flow control holds the modem
           back. */
        while (!eof) {
            if (in_len == 0) {
                const int r = recv(c->sock, (char *) in, IO_CHUNK, 0);

                if (r > 0) {
                    in_off = 0;
                    in_len = (size_t) r;
                } else if (r == 0) {
                    eof = 1; /* the modem hung up */
                    why = "the guest hung up";
                } else {
                    if (sock_error() != SOCK_WOULDBLOCK) {
                        failed = 1;
                        why    = "connection lost";
                    }
                    break;
                }
            }
            if (in_len > 0) {
                const size_t n = isp_session_write(s, in + in_off, in_len);

                in_off += n;
                in_len -= n;
                if (in_len > 0) {
                    blocked_in = 1;
                    break;
                }
            }
        }
        if (failed || (eof && (in_len == 0)))
            break;

        /* The session's bytes, out to the modem; send() may take part. */
        for (;;) {
            int r;

            if (out_len == 0) {
                out_off = 0;
                out_len = isp_session_read(s, out, IO_CHUNK);
                if (out_len == 0)
                    break;
            }
            r = send(c->sock, (const char *) out + out_off, (int) out_len, 0);
            if (r > 0) {
                out_off += (size_t) r;
                out_len -= (size_t) r;
            } else {
                if ((r < 0) && (sock_error() != SOCK_WOULDBLOCK)) {
                    failed = 1;
                    why    = "connection lost";
                }
                break;
            }
        }
        if (failed)
            break;

        /* PPP is over and its last frame has gone: hang up, as an ISP does. */
        if ((out_len == 0) && isp_session_ended(s)) {
            why = "PPP finished; hanging up";
            break;
        }

        /* A throttled call is woken by time, not by data: poll it often. */
#ifdef _WIN32
        {
            HANDLE h[2] = { sock_ev, (HANDLE) isp_event_handle(c->wake) };

            WaitForMultipleObjects(2, h, FALSE, blocked_in ? 10 : 50);
            /* Re-arms FD_READ/FD_WRITE; the loop above finds out what is ready. */
            {
                WSANETWORKEVENTS ne;

                WSAEnumNetworkEvents(c->sock, sock_ev, &ne);
            }
        }
#else
        {
            struct pollfd p[2];

            p[0].fd      = c->sock;
            p[0].events  = (short) ((blocked_in ? 0 : POLLIN) | (out_len ? POLLOUT : 0));
            p[0].revents = 0;
            p[1].fd      = isp_event_fd(c->wake);
            p[1].events  = POLLIN;
            p[1].revents = 0;
            if ((poll(p, 2, blocked_in ? 10 : 50) > 0) && (p[1].revents & POLLIN))
                isp_event_clear(c->wake);
        }
#endif
    }

    logf_("[conn %d] disconnect: %s", c->id, why);
    isp_session_close(s);

done:
#ifdef _WIN32
    WSACloseEvent(sock_ev);
#endif
    sock_close(c->sock);
    free(in);
    free(out);
    c->done = 1;
}

#ifdef _WIN32
static BOOL WINAPI
on_ctrl(DWORD type)
{
    (void) type;
    stop_requested = 1;
    return TRUE;
}
#else
static void
on_signal(int sig)
{
    (void) sig;
    stop_requested = 1;
}
#endif

static void
reap(int all)
{
    isp_mutex_lock(conns_lock);
    for (int i = 0; i < MAX_CONNS; i++) {
        conn_t *c = conns[i];

        if ((c == NULL) || (!c->done && !all))
            continue;
        if (!c->done)
            isp_event_set(c->wake);
        isp_thread_join(c->thread);
        isp_event_free(c->wake);
        free(c);
        conns[i] = NULL;
    }
    isp_mutex_unlock(conns_lock);
}

/* ------------------------------------------------------------- the page */

/* A header's value in a request, or "". */
static void
http_header(const char *head, const char *name, char *out, size_t len)
{
    const size_t nlen = strlen(name);
    const char  *p    = strstr(head, "\r\n");

    out[0] = '\0';
    while ((p != NULL) && (p[2] != '\r') && (p[2] != '\0')) {
        const char *line = p + 2;
        const char *eol  = strstr(line, "\r\n");

        if (eol == NULL)
            break;
#ifdef _WIN32
        if (!_strnicmp(line, name, nlen) && (line[nlen] == ':')) {
#else
        if (!strncasecmp(line, name, nlen) && (line[nlen] == ':')) {
#endif
            const char *v = line + nlen + 1;
            size_t      n;

            while (*v == ' ')
                v++;
            n = (size_t) (eol - v);
            if (n >= len)
                n = len - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return;
        }
        p = eol;
    }
}

static void
http_serve(SOCKET cs)
{
    char              *buf = (char *) malloc(HTTP_MAX + 1);
    size_t             got = 0;
    char              *hend = NULL;
    size_t             need = 0;
    char               method[8];
    char               path[256];
    char               host[128];
    char               xisp[16];
    char               clen[16];
    isp_web_request_t  req;
    isp_web_response_t resp;
    char               head[512];
    const uint64_t     until = isp_now_ms() + 3000;

    if (buf == NULL)
        return;
    /* The request: headers, then Content-Length bytes of body. */
    while ((int64_t) (until - isp_now_ms()) > 0) {
        int r;

        if ((hend == NULL) || (got < need)) {
            fd_set         rd;
            struct timeval tv = { 0, 200000 };

            FD_ZERO(&rd);
            FD_SET(cs, &rd);
            if (select((int) cs + 1, &rd, NULL, NULL, &tv) <= 0)
                continue;
            r = recv(cs, buf + got, (int) (HTTP_MAX - got), 0);
            if (r <= 0)
                break;
            got += (size_t) r;
            buf[got] = '\0';
        }
        if ((hend == NULL) && ((hend = strstr(buf, "\r\n\r\n")) != NULL)) {
            *hend = '\0';
            http_header(buf, "Content-Length", clen, sizeof(clen));
            need = (size_t) (hend + 4 - buf) + (size_t) atol(clen);
            if (need > HTTP_MAX)
                break;
        }
        if ((hend != NULL) && (got >= need))
            break;
        if (got >= HTTP_MAX)
            break;
    }
    if ((hend == NULL) || (got < need) || (sscanf(buf, "%7s %255s", method, path) != 2)) {
        free(buf);
        return;
    }
    path[strcspn(path, "?")] = '\0';
    http_header(buf, "Host", host, sizeof(host));
    http_header(buf, "X-ISP-Request", xisp, sizeof(xisp));
    buf[need] = '\0';

    memset(&req, 0, sizeof(req));
    req.method    = method;
    req.path      = path;
    req.host      = host;
    req.from_page = (xisp[0] != '\0');
    req.body      = hend + 4;
    req.http_port = http_port;
    isp_web_handle(&req, &resp);

    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
             "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nConnection: close\r\n\r\n",
             resp.status, (resp.status == 200) ? "OK" : "Error", resp.type, (unsigned) resp.len);
    {
        const char  *parts[2] = { head, resp.body };
        const size_t lens[2]  = { strlen(head), resp.len };

        for (int i = 0; i < 2; i++) {
            size_t off = 0;

            while (off < lens[i]) {
                const int r = send(cs, parts[i] + off, (int) (lens[i] - off), 0);

                if (r <= 0)
                    break;
                off += (size_t) r;
            }
        }
    }
    free(resp.body);
    free(buf);
}

static void
http_thread(void *arg)
{
    SOCKET hs = *(SOCKET *) arg;

    while (!stop_requested) {
        fd_set         rd;
        struct timeval tv = { 0, 250000 };
        SOCKET         cs;

        FD_ZERO(&rd);
        FD_SET(hs, &rd);
        if (select((int) hs + 1, &rd, NULL, NULL, &tv) <= 0)
            continue;
        cs = accept(hs, NULL, NULL);
        if (cs == INVALID_SOCKET)
            continue;
        http_serve(cs);
        sock_close(cs);
    }
}

/* ------------------------------------------------------------------ main */

static SOCKET
listen_on(const char *addr, int port, int backlog)
{
    struct sockaddr_in sa;
    SOCKET             s;
    int                yes = 1;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((unsigned short) port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1)
        return INVALID_SOCKET;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *) &yes, sizeof(yes));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
    if ((bind(s, (struct sockaddr *) &sa, sizeof(sa)) == SOCKET_ERROR) || (listen(s, backlog) == SOCKET_ERROR)) {
        sock_close(s);
        return INVALID_SOCKET;
    }
    return s;
}

static void
default_cfg_path(void)
{
#ifdef _WIN32
    char  exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *slash;

    if ((n > 0) && (n < sizeof(exe)) && ((slash = strrchr(exe, '\\')) != NULL)) {
        slash[1] = '\0';
        snprintf(cfg_path, sizeof(cfg_path), "%sisp-server.ini", exe);
        return;
    }
#endif
    snprintf(cfg_path, sizeof(cfg_path), "isp-server.ini");
}

static void
open_page(const char *url)
{
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "open '%s' >/dev/null 2>&1 &", url);
    (void) !system(cmd);
#else
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "xdg-open '%s' >/dev/null 2>&1 &", url);
    (void) !system(cmd);
#endif
}

static void
usage(void)
{
    printf("isp-server: the 86Box-Next virtual ISP.\n"
           "\n"
           "Point a modem at it -- the modem menu's \"Dial the ISP\", or \"Dial out to a\n"
           "TCP/IP host\", 127.0.0.1, port 2323 -- and dial any number; any name and\n"
           "password are accepted.  The status page opens in the browser.\n"
           "\n"
           "  --config FILE   settings and forwards (default: isp-server.ini by the program)\n"
           "  --listen ADDR   address modems connect to (default 127.0.0.1)\n"
           "  --port N        TCP port for modems (default 2323)\n"
           "  --http-port N   the status page, always on 127.0.0.1 (default 2324; 0: none)\n"
           "  --no-open       do not open the status page in the browser\n"
           "  --net A.B.0.0   the /16 the per-call /24s come from (default 10.86.0.0)\n"
           "  --max N         concurrent calls (default 16, at most %d)\n"
           "  --pap           ask the guest for a name and password (any will do)\n"
           "  --echo SECS     LCP keepalive interval (default 0: none)\n"
           "  --no-lan        calls cannot reach each other\n"
           "  --throttle BPS  hold every call to this modem speed\n"
           "  --forward N:SPEC  e.g. 1:tcp:2121:21 -- host port 2121 to port 21 of call 1\n"
           "  --quiet         log calls, addresses and failures only\n"
           "\n"
           "Command-line settings override the file for this run.\n",
           MAX_CONNS);
}

int
main(int argc, char **argv)
{
    isp_settings_t st;
    SOCKET         ls;
    SOCKET         hs        = INVALID_SOCKET;
    isp_thread_t  *http      = NULL;
    int            next_id   = 1;
    int            yes       = 1;
    int            open_it   = 1;

    srv_lock   = isp_mutex_new();
    conns_lock = isp_mutex_new();

    /* The file first, so that the command line can override it. */
    default_cfg_path();
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--config"))
            snprintf(cfg_path, sizeof(cfg_path), "%s", argv[i + 1]);
    isp_get_settings(&st);
    st.max_sessions = 16;
    isp_set_settings(&st);
    ini_load();
    isp_get_settings(&st);

    for (int i = 1; i < argc; i++) {
        const char *a    = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(a, "--config") && next)
            i++;
        else if (!strcmp(a, "--listen") && next) {
            snprintf(listen_addr, sizeof(listen_addr), "%s", next);
            i++;
        } else if (!strcmp(a, "--port") && next) {
            listen_port = atoi(next);
            i++;
        } else if (!strcmp(a, "--http-port") && next) {
            http_port = atoi(next);
            i++;
        } else if (!strcmp(a, "--no-open"))
            open_it = 0;
        else if (!strcmp(a, "--net") && next) {
            unsigned b[4];

            if ((sscanf(next, "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) != 4) || (b[0] > 255) || (b[1] > 255)) {
                fprintf(stderr, "--net wants an address like 10.86.0.0\n");
                return 2;
            }
            st.base_net = (b[0] << 24) | (b[1] << 16);
            i++;
        } else if (!strcmp(a, "--max") && next) {
            st.max_sessions = atoi(next);
            i++;
        } else if (!strcmp(a, "--pap"))
            st.require_pap = 1;
        else if (!strcmp(a, "--echo") && next) {
            st.echo_secs = (uint32_t) atoi(next);
            i++;
        } else if (!strcmp(a, "--no-lan"))
            st.guest_lan = 0;
        else if (!strcmp(a, "--throttle") && next) {
            st.throttle     = 1;
            st.default_rate = (uint32_t) atoi(next);
            i++;
        } else if (!strcmp(a, "--forward") && next) {
            const int     n = atoi(next);
            const char   *colon = strchr(next, ':');
            isp_forward_t fwd[ISP_MAX_FORWARDS];

            if ((n < 1) || (n > ISP_MAX_SESSIONS) || (colon == NULL) ||
                (isp_forwards_parse(colon + 1, fwd, ISP_MAX_FORWARDS) != 1)) {
                fprintf(stderr, "--forward wants CALL:tcp|udp:[*:]HOSTPORT:GUESTPORT, e.g. 1:tcp:2121:21\n");
                return 2;
            }
            snprintf(fwd_spec[n] + strlen(fwd_spec[n]), sizeof(fwd_spec[n]) - strlen(fwd_spec[n]), "%s%s",
                     fwd_spec[n][0] ? " " : "", colon + 1);
            i++;
        } else if (!strcmp(a, "--quiet"))
            quiet = 1;
        else {
            usage();
            return (!strcmp(a, "--help") || !strcmp(a, "-h") || !strcmp(a, "/?")) ? 0 : 2;
        }
    }
    if ((listen_port < 1) || (listen_port > 65535) || (http_port < 0) || (http_port > 65535) ||
        (st.max_sessions < 1) || (st.max_sessions > MAX_CONNS)) {
        usage();
        return 2;
    }
    isp_set_settings(&st);

#ifdef _WIN32
    {
        WSADATA wsa;

        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "WSAStartup failed\n");
            return 1;
        }
    }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    SetConsoleTitleA("86Box-Next ISP");
#else
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
#endif

    ls = listen_on(listen_addr, listen_port, 16);
    if (ls == INVALID_SOCKET) {
        fprintf(stderr, "cannot listen on %s:%d (error %d) -- is another isp-server running?\n", listen_addr,
                listen_port, sock_error());
        return 1;
    }
    if (http_port != 0) {
        /* The page controls the ISP: loopback only, whatever --listen says. */
        hs = listen_on("127.0.0.1", http_port, 8);
        if (hs == INVALID_SOCKET)
            logf_("warning: the status page cannot have 127.0.0.1:%d (error %d)", http_port, sock_error());
        else
            http = isp_thread_start(http_thread, &hs);
    }

    logf_("isp-server listening on %s:%d; calls get %u.%u.N.15/24 (N = 1..%d), DNS %u.%u.N.3; %s; guest LAN %s",
          listen_addr, listen_port, st.base_net >> 24, (st.base_net >> 16) & 0xff, st.max_sessions,
          st.base_net >> 24, (st.base_net >> 16) & 0xff,
          st.require_pap ? "PAP asked for (any name and password)" : "no authentication",
          st.guest_lan ? "on" : "off");
    logf_("settings and forwards: %s", cfg_path);
    if (http != NULL) {
        char url[64];

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", http_port);
        logf_("status page: %s", url);
        if (open_it)
            open_page(url);
    }
    {
        struct sockaddr_in sa;

        inet_pton(AF_INET, listen_addr, &sa.sin_addr);
        if ((ntohl(sa.sin_addr.s_addr) >> 24) != 127)
            logf_("warning: not loopback -- anyone who can reach port %d gets Internet access through this host",
                  listen_port);
    }

    while (!stop_requested) {
        fd_set         rd;
        struct timeval tv = { 0, 250000 };
        SOCKET         cs;
        conn_t        *c;
        int            slot = -1;

        reap(0);
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        if (select((int) ls + 1, &rd, NULL, NULL, &tv) <= 0)
            continue;

        {
            struct sockaddr_in peer;
            socklen_t          plen = sizeof(peer);

            cs = accept(ls, (struct sockaddr *) &peer, &plen);
            if (cs == INVALID_SOCKET)
                continue;
            c = (conn_t *) calloc(1, sizeof(conn_t));
            if (c == NULL) {
                sock_close(cs);
                continue;
            }
            c->id   = next_id++;
            c->sock = cs;
            inet_ntop(AF_INET, &peer.sin_addr, c->peer, sizeof(c->peer));
            snprintf(c->peer + strlen(c->peer), sizeof(c->peer) - strlen(c->peer), ":%u", ntohs(peer.sin_port));
        }
        set_nonblocking(cs);
        setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char *) &yes, sizeof(yes));

        isp_mutex_lock(conns_lock);
        for (int i = 0; i < MAX_CONNS; i++) {
            if (conns[i] == NULL) {
                slot = i;
                break;
            }
        }
        if (slot >= 0)
            conns[slot] = c;
        isp_mutex_unlock(conns_lock);

        logf_("[conn %d] call from %s", c->id, c->peer);
        c->wake = isp_event_new();
        if ((slot < 0) || (c->wake == NULL) || ((c->thread = isp_thread_start(conn_thread, c)) == NULL)) {
            logf_("[conn %d] refused: too many calls", c->id);
            if (slot >= 0) {
                isp_mutex_lock(conns_lock);
                conns[slot] = NULL;
                isp_mutex_unlock(conns_lock);
            }
            isp_event_free(c->wake);
            sock_close(cs);
            free(c);
        }
    }

    logf_("shutting down");
    sock_close(ls);
    if (http != NULL)
        isp_thread_join(http);
    if (hs != INVALID_SOCKET)
        sock_close(hs);
    reap(1);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
