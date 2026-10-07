/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             isp-server's server.  See isp_srv.h.
 *
 *             Every connection gets a thread.  It reads the first bytes: the
 *             exchange's greeting makes it a telephone line, a dial or an
 *             answer; anything else is a modem on a plain TCP line, and the
 *             bytes so far are the start of its PPP.
 *
 *             The telephone exchange: modems on a "Telephone network" line
 *             register a number on a connection they keep open, and are rung
 *             on it.  A dial to a registered number rings that modem; when it
 *             answers, its answer connection is handed to the caller's
 *             thread, which carries bytes between the two until either hangs
 *             up.  A dial to any other number reaches the ISP.
 *
 *             The exchange's tables are under ex_lock; a line's control
 *             socket is only written under it, so RING and CANCEL from
 *             different threads cannot interleave.  srv_lock covers the
 *             .ini and the forward table.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
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
#    include <strings.h>
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
#include "isp_srv.h"
#include "isp_phone.h"

#define MAX_CONNS     64
#define MAX_LINES     64
#define IO_CHUNK      16384
#define HTTP_MAX      65536
#define RING_TIMEOUT  120 /* s a call rings before the exchange gives up */
#define FIRST_NUMBER  5550101

typedef struct conn {
    int           id;
    SOCKET        sock;
    char          peer[64];
    isp_event_t  *wake;
    isp_thread_t *thread;
    volatile int  done;
} conn_t;

/* A modem's telephone line. */
typedef struct {
    int      used;
    char     number[24];
    char     label[64];
    SOCKET   ctl;      /* where it is rung */
    int      ringing;  /* the call ringing it, 0 if none */
    int      busy;     /* calls it is in */
    uint64_t since;
} line_t;

/* A call from one modem to another. */
typedef struct {
    int      used;
    int      id;
    char     from[24];
    char     to[24];
    int      state;      /* ISP_PCALL_* */
    int      gone;       /* the callee's line went away while it rang */
    int      hangup_req; /* the status page hangs it up */
    int      from_voice; /* the caller dialled a voice call */
    int      to_voice;   /* the callee picked up as one     */
    SOCKET   answer;     /* the callee's connection, once it answers */
    uint64_t t0;
    uint64_t from_bytes;
    uint64_t to_bytes;
} pcall_t;

static isp_srv_config_t cfg;
static volatile int     stop_requested;
static isp_mutex_t     *conns_lock;
static conn_t          *conns[MAX_CONNS];
static isp_thread_t    *accept_thread;
static isp_thread_t    *http_thread_h;
static SOCKET           listen_sock = INVALID_SOCKET;
static SOCKET           http_sock   = INVALID_SOCKET;
static int              next_conn_id = 1;

static isp_mutex_t *srv_lock;
static char         fwd_spec[ISP_MAX_SESSIONS + 1][256]; /* by call number */
static int          unknown_to_isp = 1;
static char         isp_numbers[128];

static isp_mutex_t *ex_lock;
static line_t       lines[MAX_LINES];
static pcall_t      pcalls[MAX_LINES];
static int          next_call_id = 1;

/* ---------------------------------------------------------------- logging */

static void
logf_(const char *fmt, ...)
{
    char    buf[600];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (cfg.log != NULL)
        cfg.log(buf);
    else {
        char       stamp[32];
        time_t     t  = time(NULL);
        struct tm *tm = localtime(&t);

        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm);
        printf("%s  %s\n", stamp, buf);
        fflush(stdout);
    }
}

void
isp_srv_format_number(const char *digits, char *out, size_t len)
{
    const size_t n = strlen(digits);

    if ((n == 7) && (strspn(digits, "0123456789") == 7))
        snprintf(out, len, "%.3s-%s", digits, digits + 3);
    else
        snprintf(out, len, "%s", digits);
}

/* The digits of a dial string (and * and #): "ATDT 9,555-0101" -> 95550101. */
static void
number_digits(const char *s, char *out, size_t len)
{
    size_t n = 0;

    for (; *s && (n < (len - 1)); s++)
        if (((*s >= '0') && (*s <= '9')) || (*s == '*') || (*s == '#'))
            out[n++] = *s;
    out[n] = '\0';
}

/* ------------------------------------------------------------- the .ini */

int
isp_srv_load(isp_srv_config_t *c)
{
    FILE          *f;
    isp_settings_t st;
    char           line[512];

    if (srv_lock == NULL) {
        srv_lock   = isp_mutex_new();
        conns_lock = isp_mutex_new();
        ex_lock    = isp_mutex_new();
    }
    cfg = *c;
    if ((cfg.ini[0] == '\0') || ((f = fopen(cfg.ini, "r")) == NULL))
        return 0;

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
            snprintf(c->listen, sizeof(c->listen), "%s", val);
        else if (!strcmp(key, "port"))
            c->port = atoi(val);
        else if (!strcmp(key, "http_port"))
            c->http_port = atoi(val) ? atoi(val) : -1; /* 0: no page */
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
        else if (!strcmp(key, "unknown_to_isp"))
            unknown_to_isp = !!atoi(val);
        else if (!strcmp(key, "isp_numbers"))
            snprintf(isp_numbers, sizeof(isp_numbers), "%s", val);
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
    cfg = *c;
    return 0;
}

int
isp_srv_save(void)
{
    FILE          *f;
    isp_settings_t st;

    if (cfg.ini[0] == '\0')
        return 1;
    isp_get_settings(&st);
    isp_mutex_lock(srv_lock);
    f = fopen(cfg.ini, "w");
    if (f != NULL) {
        fprintf(f, "# isp-server: the 86Box-Next virtual ISP.  Written by the status page.\n"
                   "[isp]\n");
        fprintf(f, "listen = %s\nport = %d\nhttp_port = %d\n", cfg.listen, cfg.port, cfg.http_port);
        fprintf(f, "net = %u.%u.0.0\nmax = %d\npap = %d\necho = %u\nlan = %d\nthrottle = %d\nrate = %u\n",
                st.base_net >> 24, (st.base_net >> 16) & 0xff, st.max_sessions, st.require_pap, st.echo_secs,
                st.guest_lan, st.throttle, st.default_rate);
        fprintf(f, "unknown_to_isp = %d\nisp_numbers = %s\n", unknown_to_isp, isp_numbers);
        for (int n = 1; n <= ISP_MAX_SESSIONS; n++)
            if (fwd_spec[n][0] != '\0')
                fprintf(f, "forward.%d = %s\n", n, fwd_spec[n]);
        fclose(f);
    }
    isp_mutex_unlock(srv_lock);
    if (f == NULL)
        logf_("cannot write %s", cfg.ini);
    return f != NULL;
}

void
isp_srv_add_forward(int number, const char *spec)
{
    if ((number < 1) || (number > ISP_MAX_SESSIONS))
        return;
    isp_mutex_lock(srv_lock);
    snprintf(fwd_spec[number] + strlen(fwd_spec[number]), sizeof(fwd_spec[number]) - strlen(fwd_spec[number]),
             "%s%s", fwd_spec[number][0] ? " " : "", spec);
    isp_mutex_unlock(srv_lock);
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
    return isp_srv_save();
}

void
isp_srv_settings_changed(void)
{
    isp_settings_t st;

    isp_get_settings(&st);
    logf_("settings: %u.%u.0.0, up to %d calls, %s, keepalive %u s, guest LAN %s, %s", st.base_net >> 24,
          (st.base_net >> 16) & 0xff, st.max_sessions, st.require_pap ? "PAP" : "no authentication", st.echo_secs,
          st.guest_lan ? "on" : "off", st.throttle ? "held to modem speed" : "line speed");
    isp_srv_save();
}

void
isp_srv_endpoint(char *listen, size_t len, int *port)
{
    snprintf(listen, len, "%s", cfg.listen);
    *port = cfg.port;
}

void
isp_srv_get_phone(int *unknown, char *numbers, size_t len)
{
    isp_mutex_lock(srv_lock);
    *unknown = unknown_to_isp;
    snprintf(numbers, len, "%s", isp_numbers);
    isp_mutex_unlock(srv_lock);
}

void
isp_srv_set_phone(int unknown, const char *numbers)
{
    isp_mutex_lock(srv_lock);
    unknown_to_isp = !!unknown;
    snprintf(isp_numbers, sizeof(isp_numbers), "%s", numbers);
    isp_mutex_unlock(srv_lock);
    logf_("exchange: other numbers %s; ISP numbers: %s", unknown ? "reach the ISP" : "are unknown",
          numbers[0] ? numbers : "none");
    isp_srv_save();
}

int
isp_srv_lines(isp_srv_line_t *out, int max)
{
    const uint64_t now = isp_now_ms();
    int            n   = 0;

    isp_mutex_lock(ex_lock);
    for (int i = 0; (i < MAX_LINES) && (n < max); i++) {
        if (!lines[i].used)
            continue;
        snprintf(out[n].number, sizeof(out[n].number), "%s", lines[i].number);
        snprintf(out[n].label, sizeof(out[n].label), "%s", lines[i].label);
        out[n].state   = lines[i].busy ? ISP_LINE_BUSY : (lines[i].ringing ? ISP_LINE_RINGING : ISP_LINE_IDLE);
        out[n].seconds = (uint32_t) ((now - lines[i].since) / 1000);
        n++;
    }
    isp_mutex_unlock(ex_lock);
    return n;
}

static line_t *
line_by_number(const char *number)
{
    for (int i = 0; i < MAX_LINES; i++)
        if (lines[i].used && !strcmp(lines[i].number, number))
            return &lines[i];
    return NULL;
}

int
isp_srv_phone_calls(isp_srv_pcall_t *out, int max)
{
    const uint64_t now = isp_now_ms();
    int            n   = 0;

    isp_mutex_lock(ex_lock);
    for (int i = 0; (i < MAX_LINES) && (n < max); i++) {
        const pcall_t *p = &pcalls[i];
        const line_t  *l;

        if (!p->used)
            continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id = p->id;
        snprintf(out[n].from, sizeof(out[n].from), "%s", p->from);
        snprintf(out[n].to, sizeof(out[n].to), "%s", p->to);
        if ((l = line_by_number(p->from)) != NULL)
            snprintf(out[n].from_label, sizeof(out[n].from_label), "%s", l->label);
        if ((l = line_by_number(p->to)) != NULL)
            snprintf(out[n].to_label, sizeof(out[n].to_label), "%s", l->label);
        out[n].state      = p->state;
        out[n].seconds    = (uint32_t) ((now - p->t0) / 1000);
        out[n].from_bytes = p->from_bytes;
        out[n].to_bytes   = p->to_bytes;
        out[n].voice      = (p->from_voice ? ISP_PCALL_FROM_VOICE : 0) | (p->to_voice ? ISP_PCALL_TO_VOICE : 0);
        n++;
    }
    isp_mutex_unlock(ex_lock);
    return n;
}

int
isp_srv_hangup_phone(int id)
{
    int found = 0;

    isp_mutex_lock(ex_lock);
    for (int i = 0; i < MAX_LINES; i++) {
        if (pcalls[i].used && (pcalls[i].id == id)) {
            pcalls[i].hangup_req = 1;
            found                = 1;
        }
    }
    isp_mutex_unlock(ex_lock);
    return found;
}

/* ------------------------------------------------------------ sockets */

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

/* Waits for `s` to be readable (or writable); 1 if it is. */
static int
wait_sock(SOCKET s, int for_write, int ms)
{
    fd_set         set;
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };

    FD_ZERO(&set);
    FD_SET(s, &set);
    return select((int) s + 1, for_write ? NULL : &set, for_write ? &set : NULL, NULL, &tv) > 0;
}

/* All of it, waiting while the socket is full; 0 on failure. */
static int
send_all(SOCKET s, const char *buf, size_t len)
{
    const uint64_t until = isp_now_ms() + 5000;

    while (len > 0) {
        const int r = send(s, buf, (int) len, 0);

        if (r > 0) {
            buf += r;
            len -= (size_t) r;
        } else if ((r < 0) && (sock_error() == SOCK_WOULDBLOCK) && (isp_now_ms() < until))
            wait_sock(s, 1, 100);
        else
            return 0;
    }
    return 1;
}

static int
send_line(SOCKET s, const char *fmt, ...)
{
    char    buf[256];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return 0;
    if (n > (int) sizeof(buf) - 3)
        n = (int) sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    return send_all(s, buf, (size_t) n);
}

/* 1 if the far end has closed (without taking what it sent). */
static int
peer_closed(SOCKET s)
{
    char c;
    int  r;

    if (!wait_sock(s, 0, 0))
        return 0;
    r = recv(s, &c, 1, MSG_PEEK);
    return (r == 0) || ((r < 0) && (sock_error() != SOCK_WOULDBLOCK));
}

/* --------------------------------------------------------- ISP calls */

static void
session_log(void *opaque, const char *msg)
{
    conn_t *c = (conn_t *) opaque;

    if (!cfg.quiet || strstr(msg, "IPCP: up") || strstr(msg, "finished") || strstr(msg, "failed") ||
        strstr(msg, "hung up") || strstr(msg, "forward"))
        logf_("[conn %d] %s", c->id, msg);
}

static void
session_notify(void *opaque)
{
    isp_event_set(((conn_t *) opaque)->wake);
}

/* A call to the ISP: PPP over this connection until either side hangs up.
   `pre` holds bytes already read from it. */
static void
isp_call(conn_t *c, const char *label, const uint8_t *pre, size_t pre_len)
{
    isp_session_callbacks_t cb = { session_notify, session_log, c };
    isp_session_t          *s;
    char                    err[128];
    const char             *why = "the ISP is shutting down";
    uint8_t                *in  = (uint8_t *) malloc(IO_CHUNK);
    uint8_t                *out = (uint8_t *) malloc(IO_CHUNK);
    size_t                  in_off = 0, in_len = 0;
    size_t                  out_off = 0, out_len = 0;
    int                     eof = 0;

    s = ((in != NULL) && (out != NULL)) ? isp_session_open(&cb, err, sizeof(err)) : NULL;
    if (s == NULL) {
        logf_("[conn %d] refused: %s", c->id, (in && out) ? err : "out of memory");
        free(in);
        free(out);
        return;
    }
    isp_session_set_label(s, label);
    {
        char          spec[256];
        isp_forward_t fwd[ISP_MAX_FORWARDS];

        isp_srv_get_forwards(isp_session_number(s), spec, sizeof(spec));
        if (spec[0] != '\0')
            isp_session_set_forwards(s, fwd, isp_forwards_parse(spec, fwd, ISP_MAX_FORWARDS));
    }
    if (pre_len > IO_CHUNK)
        pre_len = IO_CHUNK;
    memcpy(in, pre, pre_len);
    in_len = pre_len;

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

        /* A throttled call is woken by time, not by data: look often. */
        if (blocked_in)
            isp_event_wait(c->wake, 10);
        else {
            fd_set         rd, wr;
            struct timeval tv = { 0, 20000 };

            FD_ZERO(&rd);
            FD_ZERO(&wr);
            FD_SET(c->sock, &rd);
            if (out_len)
                FD_SET(c->sock, &wr);
            select((int) c->sock + 1, &rd, &wr, NULL, &tv);
        }
    }

    logf_("[conn %d] disconnect: %s", c->id, why);
    isp_session_close(s);
    free(in);
    free(out);
}

/* ------------------------------------------------------------ the exchange */

static void
line_unregister(SOCKET ctl)
{
    isp_mutex_lock(ex_lock);
    for (int i = 0; i < MAX_LINES; i++) {
        if (!lines[i].used || (lines[i].ctl != ctl))
            continue;
        /* A call ringing it cannot be answered now. */
        for (int k = 0; k < MAX_LINES; k++)
            if (pcalls[k].used && (pcalls[k].state == ISP_PCALL_RINGING) && !strcmp(pcalls[k].to, lines[i].number))
                pcalls[k].gone = 1;
        lines[i].used = 0;
    }
    isp_mutex_unlock(ex_lock);
}

/* REGISTER: the modem's line, held open until it is unplugged. */
static void
ex_register(conn_t *c, const char *args)
{
    char    want[24]  = "";
    char    label[64] = "";
    char    number[24];
    line_t *l = NULL;

    if (sscanf(args, "%23s %63[^\r\n]", want, label) < 1) {
        send_line(c->sock, "ERROR register what?");
        return;
    }
    number_digits(want, number, sizeof(number));

    isp_mutex_lock(ex_lock);
    /* The number asked for, unless another line has it; then the first free
       one of 555-0101 on. */
    if ((number[0] == '\0') || (strlen(number) < 3) || (line_by_number(number) != NULL)) {
        for (int n = FIRST_NUMBER; n < (FIRST_NUMBER + 9000); n++) {
            snprintf(number, sizeof(number), "%d", n);
            if (line_by_number(number) == NULL)
                break;
        }
    }
    for (int i = 0; i < MAX_LINES; i++) {
        if (!lines[i].used) {
            l = &lines[i];
            break;
        }
    }
    if (l != NULL) {
        memset(l, 0, sizeof(*l));
        l->used = 1;
        l->ctl  = c->sock;
        l->since = isp_now_ms();
        snprintf(l->number, sizeof(l->number), "%s", number);
        snprintf(l->label, sizeof(l->label), "%s", label[0] ? label : c->peer);
        send_line(c->sock, "NUMBER %s", number);
    }
    isp_mutex_unlock(ex_lock);
    if (l == NULL) {
        send_line(c->sock, "ERROR the exchange is full");
        return;
    }
    {
        char shown[80];

        isp_srv_format_number(number, shown, sizeof(shown));
        logf_("[conn %d] line %s plugged in: %s", c->id, shown, label[0] ? label : c->peer);
    }

    /* Nothing more comes this way but a keepalive; the far end closing is
       the line being unplugged. */
    while (!stop_requested) {
        char buf[256];
        int  r;

        if (!wait_sock(c->sock, 0, 250))
            continue;
        r = recv(c->sock, buf, sizeof(buf), 0);
        if ((r == 0) || ((r < 0) && (sock_error() != SOCK_WOULDBLOCK)))
            break;
    }
    line_unregister(c->sock);
    {
        char shown[80];

        isp_srv_format_number(number, shown, sizeof(shown));
        logf_("[conn %d] line %s unplugged", c->id, shown);
    }
}

/* Bytes both ways between two modems until either hangs up. */
static void
bridge(SOCKET a, SOCKET b, pcall_t *p)
{
    static const int CAP = IO_CHUNK;
    char            *ab  = (char *) malloc(CAP);
    char            *ba  = (char *) malloc(CAP);
    size_t           ab_off = 0, ab_len = 0;
    size_t           ba_off = 0, ba_len = 0;

    while (!stop_requested && (ab != NULL) && (ba != NULL)) {
        fd_set         rd, wr;
        struct timeval tv = { 0, 100000 };
        int            hang;

        isp_mutex_lock(ex_lock);
        hang = p->hangup_req;
        isp_mutex_unlock(ex_lock);
        if (hang)
            break;

        FD_ZERO(&rd);
        FD_ZERO(&wr);
        if (ab_len == 0)
            FD_SET(a, &rd);
        else
            FD_SET(b, &wr);
        if (ba_len == 0)
            FD_SET(b, &rd);
        else
            FD_SET(a, &wr);
        if (select((int) ((a > b) ? a : b) + 1, &rd, &wr, NULL, &tv) <= 0)
            continue;

        if ((ab_len == 0) && FD_ISSET(a, &rd)) {
            const int r = recv(a, ab, CAP, 0);

            if ((r == 0) || ((r < 0) && (sock_error() != SOCK_WOULDBLOCK)))
                break;
            if (r > 0) {
                ab_off = 0;
                ab_len = (size_t) r;
            }
        }
        if ((ba_len == 0) && FD_ISSET(b, &rd)) {
            const int r = recv(b, ba, CAP, 0);

            if ((r == 0) || ((r < 0) && (sock_error() != SOCK_WOULDBLOCK)))
                break;
            if (r > 0) {
                ba_off = 0;
                ba_len = (size_t) r;
            }
        }
        if (ab_len > 0) {
            const int r = send(b, ab + ab_off, (int) ab_len, 0);

            if (r > 0) {
                ab_off += (size_t) r;
                ab_len -= (size_t) r;
                isp_mutex_lock(ex_lock);
                p->from_bytes += (uint64_t) r;
                isp_mutex_unlock(ex_lock);
            } else if ((r < 0) && (sock_error() != SOCK_WOULDBLOCK))
                break;
        }
        if (ba_len > 0) {
            const int r = send(a, ba + ba_off, (int) ba_len, 0);

            if (r > 0) {
                ba_off += (size_t) r;
                ba_len -= (size_t) r;
                isp_mutex_lock(ex_lock);
                p->to_bytes += (uint64_t) r;
                isp_mutex_unlock(ex_lock);
            } else if ((r < 0) && (sock_error() != SOCK_WOULDBLOCK))
                break;
        }
    }
    free(ab);
    free(ba);
}

/* Is `dialled` one of the ISP's own numbers? */
static int
is_isp_number(const char *dialled)
{
    char list[128];
    int  unknown;

    isp_srv_get_phone(&unknown, list, sizeof(list));
    for (char *tok = strtok(list, " ,;"); tok != NULL; tok = strtok(NULL, " ,;")) {
        char d[24];

        number_digits(tok, d, sizeof(d));
        if ((d[0] != '\0') && !strcmp(d, dialled))
            return 1;
    }
    return 0;
}

/* The line a dial reaches: the number exactly, or one it ends with (so that
   "9,555-0101", an outside line first, still reaches 555-0101).  Under
   ex_lock. */
static line_t *
line_for_dial(const char *dialled)
{
    line_t      *best = line_by_number(dialled);
    const size_t dl   = strlen(dialled);

    if (best != NULL)
        return best;
    for (int i = 0; i < MAX_LINES; i++) {
        const size_t nl = lines[i].used ? strlen(lines[i].number) : 0;

        if ((nl >= 3) && (nl < dl) && !strcmp(dialled + dl - nl, lines[i].number) &&
            ((best == NULL) || (nl > strlen(best->number))))
            best = &lines[i];
    }
    return best;
}

static void
set_busy(const char *number, int delta)
{
    line_t *l = line_by_number(number);

    if (l != NULL) {
        l->busy += delta;
        if (l->busy < 0)
            l->busy = 0;
    }
}

/* DIAL: one call, to another modem or to the ISP. */
static void
ex_dial(conn_t *c, const char *args)
{
    char     from_arg[24] = "";
    char     dial_arg[96] = "";
    char     from[24];
    char     dialled[64];
    char     from_shown[80];
    char     to_shown[80];
    line_t  *t;
    pcall_t *p = NULL;
    int      unknown;
    char     ispnums[128];
    uint64_t until;
    int      voice = 0;

    if (sscanf(args, "%23s %95[^\r\n]", from_arg, dial_arg) < 2) {
        send_line(c->sock, "ERROR dial what?");
        return;
    }
    /* "... VOICE": a voice call -- dialled in voice mode, or on the
       modem's handset. */
    {
        size_t len = strlen(dial_arg);

        while ((len > 0) && (dial_arg[len - 1] == ' '))
            dial_arg[--len] = '\0';
        if ((len >= 6) && !strcmp(&dial_arg[len - 6], " VOICE")) {
            dial_arg[len - 6] = '\0';
            voice             = 1;
        }
    }
    number_digits(from_arg, from, sizeof(from));
    number_digits(dial_arg, dialled, sizeof(dialled));
    isp_srv_format_number(from[0] ? from : "?", from_shown, sizeof(from_shown));
    isp_srv_format_number(dialled, to_shown, sizeof(to_shown));
    isp_srv_get_phone(&unknown, ispnums, sizeof(ispnums));

    isp_mutex_lock(ex_lock);
    t = is_isp_number(dialled) ? NULL : line_for_dial(dialled);
    if ((t != NULL) && !strcmp(t->number, from)) {
        /* Its own number: engaged, as a real line is. */
        isp_mutex_unlock(ex_lock);
        logf_("[conn %d] %s dialled itself: busy", c->id, from_shown);
        send_line(c->sock, "BUSY");
        return;
    }
    if (t == NULL) {
        isp_mutex_unlock(ex_lock);
        if (!unknown && !is_isp_number(dialled)) {
            logf_("[conn %d] %s dialled %s: no such number", c->id, from_shown, to_shown);
            send_line(c->sock, "UNKNOWN");
            return;
        }
        /* The ISP answers. */
        {
            char    label[96];
            line_t *fl;

            isp_mutex_lock(ex_lock);
            set_busy(from, +1);
            fl = from[0] ? line_by_number(from) : NULL;
            snprintf(label, sizeof(label), "%s%s%s%s", from[0] ? from_shown : c->peer, fl ? " (" : "",
                     fl ? fl->label : "", fl ? ")" : "");
            isp_mutex_unlock(ex_lock);
            logf_("[conn %d] %s dialled %s: the ISP", c->id, from[0] ? from_shown : c->peer, to_shown);
            if (send_line(c->sock, "CONNECT"))
                isp_call(c, label, NULL, 0);
            isp_mutex_lock(ex_lock);
            set_busy(from, -1);
            isp_mutex_unlock(ex_lock);
        }
        return;
    }
    if (t->busy || t->ringing) {
        isp_mutex_unlock(ex_lock);
        logf_("[conn %d] %s dialled %s: busy", c->id, from_shown, to_shown);
        send_line(c->sock, "BUSY");
        return;
    }
    for (int i = 0; i < MAX_LINES; i++) {
        if (!pcalls[i].used) {
            p = &pcalls[i];
            break;
        }
    }
    if (p == NULL) {
        isp_mutex_unlock(ex_lock);
        send_line(c->sock, "BUSY");
        return;
    }
    memset(p, 0, sizeof(*p));
    p->used   = 1;
    p->id     = next_call_id++;
    p->state  = ISP_PCALL_RINGING;
    p->answer     = INVALID_SOCKET;
    p->t0         = isp_now_ms();
    p->from_voice = voice;
    snprintf(p->from, sizeof(p->from), "%s", from);
    snprintf(p->to, sizeof(p->to), "%s", t->number);
    isp_srv_format_number(t->number, to_shown, sizeof(to_shown));
    t->ringing = p->id;
    set_busy(from, +1);
    send_line(t->ctl, "RING %d %s", p->id, from[0] ? from : "-");
    isp_mutex_unlock(ex_lock);
    logf_("[conn %d] %s dialled %s: ringing %s (call %d)", c->id, from_shown, to_shown, t->label, p->id);
    send_line(c->sock, "RINGING");

    /* Until it answers, the caller gives up, or nobody does. */
    until = isp_now_ms() + (RING_TIMEOUT * 1000);
    for (;;) {
        SOCKET      answer;
        int         gone;
        int         hang;
        const char *why = NULL;

        isp_mutex_lock(ex_lock);
        answer = p->answer;
        gone   = p->gone;
        hang   = p->hangup_req;
        isp_mutex_unlock(ex_lock);

        if (answer != INVALID_SOCKET) {
            isp_mutex_lock(ex_lock);
            p->state = ISP_PCALL_ACTIVE;
            p->t0    = isp_now_ms();
            if ((t = line_by_number(p->to)) != NULL) {
                if (t->ringing == p->id)
                    t->ringing = 0;
                t->busy++;
            }
            isp_mutex_unlock(ex_lock);
            logf_("[conn %d] call %d answered: %s and %s connected%s", c->id, p->id, from_shown, to_shown,
                  (p->from_voice && p->to_voice) ? " (voice)"
                  : (p->from_voice || p->to_voice) ? " (voice at one end, a modem at the other)" : "");
            /* What picked up: a voice call, or a modem. */
            if (send_line(c->sock, p->to_voice ? "CONNECT VOICE" : "CONNECT"))
                bridge(c->sock, answer, p);
            sock_close(answer);
            logf_("[conn %d] call %d ended", c->id, p->id);
            isp_mutex_lock(ex_lock);
            set_busy(p->to, -1);
            break;
        }
        if (gone)
            why = "NOANSWER"; /* its line was unplugged */
        else if (hang || stop_requested || peer_closed(c->sock))
            why = "";
        else if (isp_now_ms() >= until)
            why = "NOANSWER";
        if (why != NULL) {
            isp_mutex_lock(ex_lock);
            if (((t = line_by_number(p->to)) != NULL) && (t->ringing == p->id)) {
                t->ringing = 0;
                send_line(t->ctl, "CANCEL %d", p->id);
            }
            isp_mutex_unlock(ex_lock);
            if (why[0])
                send_line(c->sock, "%s", why);
            logf_("[conn %d] call %d: %s", c->id, p->id, why[0] ? "no answer" : "the caller hung up");
            isp_mutex_lock(ex_lock);
            break;
        }
        isp_event_wait(c->wake, 50);
    }
    /* Under ex_lock here. */
    set_busy(p->from, -1);
    p->used = 0;
    isp_mutex_unlock(ex_lock);
}

/* ANSWER: the ringing modem picks up; its connection becomes the call's. */
static void
ex_answer(conn_t *c, const char *args)
{
    const int id     = atoi(args);
    const int voice  = (strstr(args, " VOICE") != NULL);
    int       handed = 0;

    isp_mutex_lock(ex_lock);
    for (int i = 0; i < MAX_LINES; i++) {
        pcall_t *p = &pcalls[i];

        if (p->used && (p->id == id) && (p->state == ISP_PCALL_RINGING) && (p->answer == INVALID_SOCKET) &&
            !p->gone && !p->hangup_req) {
            if (send_line(c->sock, p->from_voice ? "CONNECT VOICE" : "CONNECT")) {
                p->to_voice = voice;
                p->answer   = c->sock;
                c->sock   = INVALID_SOCKET; /* the caller's thread has it now */
                handed    = 1;
            }
            break;
        }
    }
    isp_mutex_unlock(ex_lock);
    if (!handed)
        send_line(c->sock, "GONE");
}

/* ------------------------------------------------------------ connections */

static void
conn_thread(void *arg)
{
    conn_t        *c = (conn_t *) arg;
    static const char greet[] = ISP_EXCHANGE_GREETING;
    char           first[256];
    size_t         n   = 0;
    int            raw = 0;
    const uint64_t t0  = isp_now_ms();

    /* The greeting, or not: a byte at a time until it cannot be, or the
       line is complete.  A plain modem says nothing until its guest starts
       PPP, so there is no time limit on that; there is one on finishing a
       greeting begun. */
    while (!stop_requested) {
        int r;

        if (!wait_sock(c->sock, 0, 250)) {
            if ((n > 0) && ((isp_now_ms() - t0) > 10000))
                break;
            continue;
        }
        r = recv(c->sock, &first[n], 1, 0);
        if (r <= 0) {
            if ((r < 0) && (sock_error() == SOCK_WOULDBLOCK))
                continue;
            n = 0;
            break; /* gone before it said anything */
        }
        n++;
        if ((n <= (sizeof(greet) - 1)) && (first[n - 1] != greet[n - 1])) {
            raw = 1;
            break;
        }
        if ((n > (sizeof(greet) - 1)) && (first[n - 1] == '\n'))
            break;
        if (n >= (sizeof(first) - 1))
            break;
    }

    if (raw) {
        /* A plain TCP line: PPP to the ISP, the bytes so far its first. */
        logf_("[conn %d] PPP from %s (plain TCP line)", c->id, c->peer);
        isp_call(c, c->peer, (const uint8_t *) first, n);
    } else if ((n > (sizeof(greet) - 1)) && (first[n - 1] == '\n')) {
        char *cmd = first + sizeof(greet) - 1;

        first[n] = '\0';
        cmd[strcspn(cmd, "\r\n")] = '\0';
        if (!strncmp(cmd, "REGISTER ", 9))
            ex_register(c, cmd + 9);
        else if (!strncmp(cmd, "DIAL ", 5))
            ex_dial(c, cmd + 5);
        else if (!strncmp(cmd, "ANSWER ", 7))
            ex_answer(c, cmd + 7);
        else
            send_line(c->sock, "ERROR unknown request");
    }

    if (c->sock != INVALID_SOCKET)
        sock_close(c->sock);
    c->done = 1;
}

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
accept_loop(void *arg)
{
    int yes = 1;

    (void) arg;
    while (!stop_requested) {
        struct sockaddr_in peer;
        socklen_t          plen = sizeof(peer);
        SOCKET             cs;
        conn_t            *c;
        int                slot = -1;

        reap(0);
        if (!wait_sock(listen_sock, 0, 250))
            continue;
        cs = accept(listen_sock, (struct sockaddr *) &peer, &plen);
        if (cs == INVALID_SOCKET)
            continue;
        c = (conn_t *) calloc(1, sizeof(conn_t));
        if (c == NULL) {
            sock_close(cs);
            continue;
        }
        c->id   = next_conn_id++;
        c->sock = cs;
        inet_ntop(AF_INET, &peer.sin_addr, c->peer, sizeof(c->peer));
        snprintf(c->peer + strlen(c->peer), sizeof(c->peer) - strlen(c->peer), ":%u", ntohs(peer.sin_port));
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

        if (!cfg.quiet)
            logf_("[conn %d] connection from %s", c->id, c->peer);
        c->wake = isp_event_new();
        if ((slot < 0) || (c->wake == NULL) || ((c->thread = isp_thread_start(conn_thread, c)) == NULL)) {
            logf_("[conn %d] refused: too many connections", c->id);
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

/* One request.  1 if the socket was taken over (the page's phone). */
static int
http_serve(SOCKET cs)
{
    char              *buf  = (char *) malloc(HTTP_MAX + 1);
    size_t             got  = 0;
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
        return 0;
    /* The request: headers, then Content-Length bytes of body. */
    while (isp_now_ms() < until) {
        if ((hend == NULL) || (got < need)) {
            int r;

            if (!wait_sock(cs, 0, 200))
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
        return 0;
    }
    /* The page's phone: a WebSocket, which is the phone's from here. */
    if (!strncmp(path, "/api/phone", 10)) {
        const int taken = isp_phone_accept((uintptr_t) cs, buf, cfg.http_port, cfg.port);

        free(buf);
        return taken;
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
    req.http_port = cfg.http_port;
    isp_web_handle(&req, &resp);

    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
             "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nConnection: close\r\n\r\n",
             resp.status, (resp.status == 200) ? "OK" : "Error", resp.type, (unsigned) resp.len);
    send_all(cs, head, strlen(head));
    send_all(cs, resp.body, resp.len);
    free(resp.body);
    free(buf);
    return 0;
}

static void
http_loop(void *arg)
{
    (void) arg;
    while (!stop_requested) {
        SOCKET cs;

        if (!wait_sock(http_sock, 0, 250))
            continue;
        cs = accept(http_sock, NULL, NULL);
        if (cs == INVALID_SOCKET)
            continue;
        set_nonblocking(cs);
        if (!http_serve(cs))
            sock_close(cs);
    }
}

/* ------------------------------------------------------------ start, stop */

static SOCKET
listen_on(const char *addr, int *port, int backlog)
{
    struct sockaddr_in sa;
    socklen_t          len = sizeof(sa);
    SOCKET             s;
    int                yes = 1;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((unsigned short) *port);
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
    getsockname(s, (struct sockaddr *) &sa, &len);
    *port = ntohs(sa.sin_port);
    return s;
}

int
isp_srv_start(isp_srv_config_t *c)
{
    isp_settings_t st;
#ifdef _WIN32
    WSADATA wsa;

    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    if (srv_lock == NULL)
        isp_srv_load(c);
    cfg            = *c;
    stop_requested = 0;

    listen_sock = listen_on(cfg.listen, &cfg.port, 16);
    if (listen_sock == INVALID_SOCKET) {
        logf_("cannot listen on %s:%d (error %d) -- is another isp-server running?", cfg.listen, cfg.port,
              sock_error());
        return -1;
    }
    if (cfg.http_port >= 0) {
        /* The page controls the ISP: loopback only, whatever `listen` says. */
        http_sock = listen_on("127.0.0.1", &cfg.http_port, 8);
        if (http_sock == INVALID_SOCKET)
            logf_("warning: the status page cannot have 127.0.0.1:%d (error %d)", cfg.http_port, sock_error());
        else
            http_thread_h = isp_thread_start(http_loop, NULL);
    }
    accept_thread = isp_thread_start(accept_loop, NULL);
    c->port       = cfg.port;
    c->http_port  = (http_sock != INVALID_SOCKET) ? cfg.http_port : -1;

    isp_get_settings(&st);
    logf_("isp-server listening on %s:%d; calls get %u.%u.N.15/24 (N = 1..%d), DNS %u.%u.N.3; %s; guest LAN %s",
          cfg.listen, cfg.port, st.base_net >> 24, (st.base_net >> 16) & 0xff, st.max_sessions, st.base_net >> 24,
          (st.base_net >> 16) & 0xff, st.require_pap ? "PAP asked for (any name and password)" : "no authentication",
          st.guest_lan ? "on" : "off");
    logf_("telephone exchange: modems on a \"Telephone network\" line get numbers from 555-0101; other numbers %s",
          unknown_to_isp ? "reach the ISP" : "are unknown");
    if (cfg.ini[0] != '\0')
        logf_("settings and forwards: %s", cfg.ini);
    {
        struct sockaddr_in sa;

        inet_pton(AF_INET, cfg.listen, &sa.sin_addr);
        if ((ntohl(sa.sin_addr.s_addr) >> 24) != 127)
            logf_("warning: not loopback -- anyone who can reach port %d gets Internet access through this host",
                  cfg.port);
    }
    return 0;
}

void
isp_srv_stop(void)
{
    stop_requested = 1;
    if (accept_thread != NULL)
        isp_thread_join(accept_thread);
    if (http_thread_h != NULL)
        isp_thread_join(http_thread_h);
    accept_thread = http_thread_h = NULL;
    isp_phone_stop_all();
    if (listen_sock != INVALID_SOCKET)
        sock_close(listen_sock);
    if (http_sock != INVALID_SOCKET)
        sock_close(http_sock);
    listen_sock = http_sock = INVALID_SOCKET;
    reap(1);
    logf_("isp-server stopped");
}
