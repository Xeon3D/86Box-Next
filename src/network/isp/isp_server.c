/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             isp-server: the virtual ISP behind a TCP port.  Set a COM port
 *             modem's line to "Dial out to a TCP/IP host", 127.0.0.1, port
 *             2323, and any number the guest dials reaches this: a PPP
 *             server that gives the guest an address and DNS and NATs it to
 *             the host's Internet.  Each connection is one call, with a PPP
 *             session, a /24 and a libslirp instance of its own.
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
typedef int socklen_t;
#    define sock_error()     WSAGetLastError()
#    define SOCK_WOULDBLOCK  WSAEWOULDBLOCK
#    define sock_close(s)    closesocket(s)
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
#    define INVALID_SOCKET   (-1)
#    define SOCKET_ERROR     (-1)
#    define sock_error()     errno
#    define SOCK_WOULDBLOCK  EWOULDBLOCK
#    define sock_close(s)    close(s)
#endif
#include <86box/isp.h>
#include "isp_plat.h"

#define MAX_CONNS 64
#define IO_CHUNK  16384

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

static void
session_log(void *opaque, const char *msg)
{
    conn_t *c = (conn_t *) opaque;

    if (!quiet || strstr(msg, "IPCP: up") || strstr(msg, "finished") || strstr(msg, "failed"))
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
    conn_t                 *c = (conn_t *) arg;
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
#ifdef _WIN32
    WSAEventSelect(c->sock, sock_ev, FD_READ | FD_WRITE | FD_CLOSE);
#endif

    while (!stop_requested) {
        int blocked_in = 0;
        int failed     = 0;

        /* The guest's bytes, into the session.  When it will take no more they
           stay in the socket, and TCP flow control holds the modem back. */
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

#ifdef _WIN32
        {
            HANDLE h[2] = { sock_ev, (HANDLE) isp_event_handle(c->wake) };

            WaitForMultipleObjects(2, h, FALSE, blocked_in ? 10 : 250);
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
            if ((poll(p, 2, blocked_in ? 10 : 250) > 0) && (p[1].revents & POLLIN))
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

static void
usage(void)
{
    printf("isp-server: the 86Box-Next virtual ISP on a TCP port.\n"
           "\n"
           "  --listen ADDR   address to listen on (default 127.0.0.1)\n"
           "  --port N        TCP port (default 2323)\n"
           "  --net A.B.0.0   the /16 the per-call /24s come from (default 10.86.0.0)\n"
           "  --max N         concurrent calls (default 16, at most %d)\n"
           "  --pap           ask the guest for a name and password (any will do)\n"
           "  --echo SECS     LCP keepalive interval (default 0: none)\n"
           "  --quiet         log calls, addresses and failures only\n"
           "\n"
           "Point a modem's line at it: \"Dial out to a TCP/IP host\", 127.0.0.1, port 2323.\n"
           "Any number dials it; any name and password are accepted.\n",
           MAX_CONNS);
}

int
main(int argc, char **argv)
{
    const char        *listen_addr = "127.0.0.1";
    int                port        = 2323;
    isp_settings_t     st;
    struct sockaddr_in sa;
    SOCKET             ls;
    int                next_id = 1;
    int                yes     = 1;

    isp_get_settings(&st);
    st.max_sessions = 16;

    for (int i = 1; i < argc; i++) {
        const char *a    = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(a, "--listen") && next) {
            listen_addr = next;
            i++;
        } else if (!strcmp(a, "--port") && next) {
            port = atoi(next);
            i++;
        } else if (!strcmp(a, "--net") && next) {
            unsigned b[4];

            if (sscanf(next, "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) != 4 || b[0] > 255 || b[1] > 255) {
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
        } else if (!strcmp(a, "--quiet"))
            quiet = 1;
        else {
            usage();
            return (!strcmp(a, "--help") || !strcmp(a, "-h") || !strcmp(a, "/?")) ? 0 : 2;
        }
    }
    if ((port < 1) || (port > 65535) || (st.max_sessions < 1) || (st.max_sessions > MAX_CONNS)) {
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
#else
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
#endif

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((unsigned short) port);
    if (inet_pton(AF_INET, listen_addr, &sa.sin_addr) != 1) {
        fprintf(stderr, "not an IPv4 address: %s\n", listen_addr);
        return 2;
    }

    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", sock_error());
        return 1;
    }
#ifdef _WIN32
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *) &yes, sizeof(yes));
#else
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
    if ((bind(ls, (struct sockaddr *) &sa, sizeof(sa)) == SOCKET_ERROR) || (listen(ls, 16) == SOCKET_ERROR)) {
        fprintf(stderr, "cannot listen on %s:%d (error %d) -- is another isp-server running?\n",
                listen_addr, port, sock_error());
        sock_close(ls);
        return 1;
    }

    conns_lock = isp_mutex_new();
    logf_("isp-server listening on %s:%d; calls get %u.%u.N.15/24 (N = 1..%d), DNS %u.%u.N.3; %s",
          listen_addr, port, st.base_net >> 24, (st.base_net >> 16) & 0xff, st.max_sessions,
          st.base_net >> 24, (st.base_net >> 16) & 0xff,
          st.require_pap ? "PAP asked for (any name and password)" : "no authentication");
    if ((ntohl(sa.sin_addr.s_addr) >> 24) != 127)
        logf_("warning: not loopback -- anyone who can reach this port gets Internet access through this host");

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
    reap(1);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
