/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The status page's phone (isp_phone.h).  The page opens a
 *             WebSocket to /api/phone; this takes it over and is a
 *             telephone on the exchange -- registered over loopback like any
 *             modem (REGISTER, asking for 555-0100), rung (RING/CANCEL),
 *             dialling and answering as a voice call (DIAL/ANSWER ... VOICE),
 *             and talking in the exchange's voice frames (modem_voice.h:
 *             8000 Hz mu-law, DTMF digits).
 *
 *             On the WebSocket, the page sends JSON commands as text --
 *             {"cmd":"dial","number":"5550101"}, {"cmd":"answer"},
 *             {"cmd":"hangup"}, {"cmd":"dtmf","digit":"5"} -- and its
 *             microphone as binary messages of 16-bit little-endian samples
 *             at 8000 Hz; it is sent JSON events ({"type":"number"...},
 *             {"type":"state","state":...}, {"type":"dtmf"...}) and the far
 *             end's audio, binary, the same way.  The tones (ringback, busy,
 *             the ring) are the page's own to play.
 *
 *             The upgrade is refused unless the Host is the page's own and
 *             the Origin is that same page: another site cannot open it.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    include <windows.h>
#    define sock_error()    WSAGetLastError()
#    define SOCK_WOULDBLOCK WSAEWOULDBLOCK
#    define SOCK_INPROGRESS WSAEWOULDBLOCK
#    define sock_close(s)   closesocket(s)
#    define strncasecmp     _strnicmp
#else
#    include <arpa/inet.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#    include <strings.h>
#    include <sys/select.h>
#    include <sys/socket.h>
#    include <unistd.h>
typedef int SOCKET;
#    define INVALID_SOCKET  (-1)
#    define sock_error()    errno
#    define SOCK_WOULDBLOCK EWOULDBLOCK
#    define SOCK_INPROGRESS EINPROGRESS
#    define sock_close(s)   close(s)
#endif
#include <86box/modem_voice.h>
#include "isp_plat.h"
#include "isp_srv.h"
#include "isp_phone.h"

#define MAX_PHONES 8
#define WS_MAX     (64 * 1024) /* a WebSocket message, at most */
#define MIC_KEEP   1600        /* 200 ms of the browser's microphone waiting: catch up */

enum {
    P_IDLE = 0,
    P_RINGING,  /* a call rings the page                  */
    P_CALLING,  /* dialled, the exchange not answered yet */
    P_RINGBACK, /* the far end rings                      */
    P_ANSWERING,
    P_TALKING
};

static const char *state_names[] = { "idle", "ringing", "calling", "ringback", "answering", "talking" };

typedef struct phone {
    int            used;
    SOCKET         ws;
    SOCKET         ctl;  /* the line, registered */
    SOCKET         call; /* the call in progress */
    int            exchange_port;
    int            state;
    int            peer_voice;
    int            ring_id;
    char           from[24];
    char           number[24];
    char           ctl_in[256];
    int            ctl_len;
    char           sig_in[64];
    int            sig_len;
    int            waiting_reply; /* the call's connection: replies from the exchange */
    voice_deframer_t defr;
    int16_t        mic[4096];
    unsigned       mic_head, mic_tail;
    voice_tone_t   tone;
    uint8_t        wsbuf[WS_MAX + 16];
    size_t         wslen;
    uint64_t       next_frame;
    isp_thread_t  *thread;
    volatile int   stop;
} phone_t;

static phone_t      phones[MAX_PHONES];
static isp_mutex_t *phones_lock;

/* --------------------------------------------------------------- sockets */

static void
set_nonblocking(SOCKET s)
{
#ifdef _WIN32
    u_long yes = 1;

    ioctlsocket(s, FIONBIO, &yes);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static int
wait_write(SOCKET s, int ms)
{
    fd_set         set;
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };

    FD_ZERO(&set);
    FD_SET(s, &set);
    return select((int) s + 1, NULL, &set, NULL, &tv) > 0;
}

static int
send_all(SOCKET s, const void *data, size_t len)
{
    const char    *buf   = (const char *) data;
    const uint64_t until = isp_now_ms() + 3000;

    while (len > 0) {
        const int r = send(s, buf, (int) len, 0);

        if (r > 0) {
            buf += r;
            len -= (size_t) r;
        } else if ((r < 0) && (sock_error() == SOCK_WOULDBLOCK) && (isp_now_ms() < until))
            wait_write(s, 50);
        else
            return 0;
    }
    return 1;
}

/* The exchange, over loopback, with a request: its connection, or
   INVALID_SOCKET. */
static SOCKET
exchange_open(int port, const char *fmt, ...)
{
    struct sockaddr_in sa;
    SOCKET             s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    char               req[200];
    va_list            ap;
    int                n;

    if (s == INVALID_SOCKET)
        return s;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons((unsigned short) port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
        sock_close(s);
        return INVALID_SOCKET;
    }
    {
        int one = 1;

        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *) &one, sizeof(one));
    }
    set_nonblocking(s);
    n = snprintf(req, sizeof(req), "%s", ISP_EXCHANGE_GREETING);
    va_start(ap, fmt);
    n += vsnprintf(req + n, sizeof(req) - (size_t) n - 2, fmt, ap);
    va_end(ap);
    req[n++] = '\r';
    req[n++] = '\n';
    if (!send_all(s, req, (size_t) n)) {
        sock_close(s);
        return INVALID_SOCKET;
    }
    return s;
}

/* -------------------------------------------- SHA-1, base64: the handshake */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t  block[64];
    size_t   n;
} sha1_t;

static uint32_t
rol(uint32_t x, int k)
{
    return (x << k) | (x >> (32 - k));
}

static void
sha1_block(sha1_t *c, const uint8_t *p)
{
    uint32_t w[80], a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4], cc = c->h[2];

    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t) p[i * 4] << 24) | ((uint32_t) p[i * 4 + 1] << 16) | ((uint32_t) p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k, t;

        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ cc ^ d;
            k = 0xca62c1d6;
        }
        t  = rol(a, 5) + f + e + k + w[i];
        e  = d;
        d  = cc;
        cc = rol(b, 30);
        b  = a;
        a  = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void
sha1(const void *data, size_t len, uint8_t out[20])
{
    sha1_t         c = { { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 }, 0, { 0 }, 0 };
    const uint8_t *p = (const uint8_t *) data;
    uint8_t        pad[72];
    size_t         padn;

    c.len = (uint64_t) len * 8;
    while (len >= 64) {
        sha1_block(&c, p);
        p += 64;
        len -= 64;
    }
    memcpy(pad, p, len);
    pad[len++] = 0x80;
    padn       = (len <= 56) ? 64 : 128;
    memset(pad + len, 0, sizeof(pad) - len);
    {
        uint8_t last[128];

        memset(last, 0, sizeof(last));
        memcpy(last, pad, len);
        for (int i = 0; i < 8; i++)
            last[padn - 1 - i] = (uint8_t) (c.len >> (8 * i));
        sha1_block(&c, last);
        if (padn == 128)
            sha1_block(&c, last + 64);
    }
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t) (c.h[i] >> 24);
        out[i * 4 + 1] = (uint8_t) (c.h[i] >> 16);
        out[i * 4 + 2] = (uint8_t) (c.h[i] >> 8);
        out[i * 4 + 3] = (uint8_t) c.h[i];
    }
}

static void
base64(const uint8_t *in, size_t n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t            o   = 0;

    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = ((uint32_t) in[i] << 16) | ((i + 1 < n) ? ((uint32_t) in[i + 1] << 8) : 0) |
                           ((i + 2 < n) ? in[i + 2] : 0);

        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? t[v & 63] : '=';
    }
    out[o] = '\0';
}

/* A header's value, or "". */
static void
header(const char *head, const char *name, char *out, size_t len)
{
    const size_t nlen = strlen(name);
    const char  *p    = strstr(head, "\r\n");

    out[0] = '\0';
    while (p != NULL) {
        const char *line = p + 2;
        const char *eol  = strstr(line, "\r\n");
        size_t      n;

        if (!strncasecmp(line, name, nlen) && (line[nlen] == ':')) {
            const char *v = line + nlen + 1;

            while (*v == ' ')
                v++;
            n = eol ? (size_t) (eol - v) : strlen(v);
            if (n >= len)
                n = len - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return;
        }
        p = eol;
    }
}

/* ---------------------------------------------------------------- frames */

static int
ws_send(phone_t *ph, int opcode, const void *data, size_t len)
{
    uint8_t hdr[10];
    size_t  h = 0;

    hdr[h++] = (uint8_t) (0x80 | opcode);
    if (len < 126)
        hdr[h++] = (uint8_t) len;
    else if (len < 65536) {
        hdr[h++] = 126;
        hdr[h++] = (uint8_t) (len >> 8);
        hdr[h++] = (uint8_t) len;
    } else
        return 0;
    return send_all(ph->ws, hdr, h) && ((len == 0) || send_all(ph->ws, data, len));
}

static void
ws_event(phone_t *ph, const char *fmt, ...)
{
    char    buf[256];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        (void) ws_send(ph, 1, buf, (size_t) ((n < (int) sizeof(buf)) ? n : (int) sizeof(buf) - 1));
}

/* Only digits, *, #, A-D and dashes go into the JSON or a request. */
static void
clean_number(const char *in, char *out, size_t len)
{
    size_t n = 0;

    for (; *in && (n < len - 1); in++)
        if (((*in >= '0') && (*in <= '9')) || (*in == '*') || (*in == '#') || (*in == '-') ||
            ((*in >= 'A') && (*in <= 'D')))
            out[n++] = *in;
    out[n] = '\0';
}

static void
set_state(phone_t *ph, int state, const char *why)
{
    ph->state = state;
    ws_event(ph, "{\"type\":\"state\",\"state\":\"%s\",\"why\":\"%s\",\"peer\":\"%s\",\"voice\":%d}",
             state_names[state], why ? why : "", (state == P_RINGING) ? ph->from : "", ph->peer_voice);
}

static void
call_end(phone_t *ph, const char *why)
{
    if (ph->call != INVALID_SOCKET) {
        sock_close(ph->call);
        ph->call = INVALID_SOCKET;
    }
    ph->waiting_reply = 0;
    ph->peer_voice    = 0;
    set_state(ph, P_IDLE, why);
}

/* ------------------------------------------------------- the page's side */

/* A JSON string member's value -- the page's own, simple messages. */
static int
json_str(const char *msg, const char *key, char *out, size_t len)
{
    char        pat[32];
    const char *p;
    size_t      n = 0;

    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    if ((p = strstr(msg, pat)) == NULL)
        return 0;
    p += strlen(pat);
    while (*p && (*p != '"') && (n < len - 1))
        out[n++] = *p++;
    out[n] = '\0';
    return 1;
}

static void
page_command(phone_t *ph, const char *msg)
{
    char cmd[16], arg[48];

    if (!json_str(msg, "cmd", cmd, sizeof(cmd)))
        return;
    if (!strcmp(cmd, "dial") && (ph->state == P_IDLE) && json_str(msg, "number", arg, sizeof(arg))) {
        char num[48];

        clean_number(arg, num, sizeof(num));
        if (num[0] == '\0')
            return;
        ph->call = exchange_open(ph->exchange_port, "DIAL %s %s VOICE", ph->number[0] ? ph->number : "-", num);
        if (ph->call == INVALID_SOCKET) {
            set_state(ph, P_IDLE, "the exchange did not answer");
            return;
        }
        ph->waiting_reply = 1;
        ph->sig_len       = 0;
        memset(&ph->defr, 0, sizeof(ph->defr));
        set_state(ph, P_CALLING, "");
    } else if (!strcmp(cmd, "answer") && (ph->state == P_RINGING)) {
        ph->call = exchange_open(ph->exchange_port, "ANSWER %d VOICE", ph->ring_id);
        if (ph->call == INVALID_SOCKET) {
            set_state(ph, P_IDLE, "the exchange did not answer");
            return;
        }
        ph->waiting_reply = 1;
        ph->sig_len       = 0;
        memset(&ph->defr, 0, sizeof(ph->defr));
        set_state(ph, P_ANSWERING, "");
    } else if (!strcmp(cmd, "hangup")) {
        if (ph->call != INVALID_SOCKET)
            call_end(ph, "hung up");
    } else if (!strcmp(cmd, "dtmf") && (ph->state == P_TALKING) && json_str(msg, "digit", arg, sizeof(arg))) {
        double        f1, f2;
        const uint8_t d = (uint8_t) arg[0];

        if (voice_dtmf_freqs(arg[0], &f1, &f2)) {
            uint8_t frame[VOICE_FRAME_HDR + 1];

            voice_tone_start(&ph->tone, f1, f2, 120, 9000.0);
            if (ph->peer_voice)
                (void) send_all(ph->call, frame, voice_frame(frame, VOICE_FRAME_DTMF, &d, 1));
        }
    }
}

/* The microphone: 16-bit little-endian samples. */
static void
page_audio(phone_t *ph, const uint8_t *p, size_t len)
{
    const unsigned size = sizeof(ph->mic) / sizeof(ph->mic[0]);

    for (size_t i = 0; i + 1 < len; i += 2) {
        ph->mic[ph->mic_head % size] = (int16_t) (uint16_t) (p[i] | (p[i + 1] << 8));
        ph->mic_head++;
    }
    if ((ph->mic_head - ph->mic_tail) > MIC_KEEP)
        ph->mic_tail = ph->mic_head - (MIC_KEEP / 2);
}

/* What the page sent: frames, masked as a client's are.  0 when it closed. */
static int
page_read(phone_t *ph)
{
    const int r = recv(ph->ws, (char *) ph->wsbuf + ph->wslen, (int) (sizeof(ph->wsbuf) - ph->wslen), 0);

    if (r == 0)
        return 0;
    if (r < 0)
        return sock_error() == SOCK_WOULDBLOCK;
    ph->wslen += (size_t) r;
    for (;;) {
        size_t   h = 2, len;
        uint8_t *mask, *pl;
        int      op;

        if (ph->wslen < 2)
            break;
        op  = ph->wsbuf[0] & 0x0f;
        len = ph->wsbuf[1] & 0x7f;
        if (len == 126) {
            if (ph->wslen < 4)
                break;
            len = ((size_t) ph->wsbuf[2] << 8) | ph->wsbuf[3];
            h   = 4;
        } else if (len == 127)
            return 0; /* far more than a phone sends */
        if (!(ph->wsbuf[1] & 0x80) || (len > WS_MAX))
            return 0; /* a client's frames are masked */
        if (ph->wslen < (h + 4 + len))
            break;
        mask = &ph->wsbuf[h];
        pl   = mask + 4;
        for (size_t i = 0; i < len; i++)
            pl[i] ^= mask[i & 3];
        switch (op) {
            case 1: {
                char   msg[512];
                size_t n = (len < sizeof(msg)) ? len : (sizeof(msg) - 1);

                memcpy(msg, pl, n);
                msg[n] = '\0';
                page_command(ph, msg);
                break;
            }
            case 2:
                page_audio(ph, pl, len);
                break;
            case 8:
                (void) ws_send(ph, 8, NULL, 0);
                return 0;
            case 9:
                (void) ws_send(ph, 10, pl, len);
                break;
            default:
                break;
        }
        memmove(ph->wsbuf, pl + len, ph->wslen - (h + 4 + len));
        ph->wslen -= h + 4 + len;
    }
    if (ph->wslen >= sizeof(ph->wsbuf) - 16)
        return 0;
    return 1;
}

/* ------------------------------------------------------ the exchange side */

static void
ctl_line(phone_t *ph, const char *line)
{
    int  id;
    char from[24];

    if (!strncmp(line, "NUMBER ", 7)) {
        clean_number(line + 7, ph->number, sizeof(ph->number));
        ws_event(ph, "{\"type\":\"number\",\"number\":\"%s\"}", ph->number);
    } else if (sscanf(line, "RING %d %23s", &id, from) == 2) {
        if (ph->state == P_IDLE) {
            ph->ring_id = id;
            clean_number(from, ph->from, sizeof(ph->from));
            set_state(ph, P_RINGING, "");
        }
    } else if (sscanf(line, "CANCEL %d", &id) == 1) {
        if ((ph->state == P_RINGING) && (ph->ring_id == id))
            set_state(ph, P_IDLE, "the caller gave up");
    }
}

static void
frame_in(int type, const uint8_t *payload, size_t len, void *priv)
{
    phone_t *ph = (phone_t *) priv;

    if (type == VOICE_FRAME_AUDIO) {
        uint8_t pcm[VOICE_FRAME_MAX * 2];

        for (size_t i = 0; i < len; i++) {
            const int16_t v = voice_ulaw_decode(payload[i]);

            pcm[i * 2]     = (uint8_t) v;
            pcm[i * 2 + 1] = (uint8_t) ((uint16_t) v >> 8);
        }
        (void) ws_send(ph, 2, pcm, len * 2);
    } else if ((type == VOICE_FRAME_DTMF) && (len >= 1)) {
        char d[2] = { (char) payload[0], 0 };

        clean_number(d, d, sizeof(d));
        ws_event(ph, "{\"type\":\"dtmf\",\"digit\":\"%s\"}", d);
    }
}

/* The call's connection: the exchange's replies, then the far end. */
static void
call_read(phone_t *ph)
{
    while (ph->waiting_reply) {
        char      ch;
        const int r = recv(ph->call, &ch, 1, 0);

        if (r <= 0) {
            if ((r < 0) && (sock_error() == SOCK_WOULDBLOCK))
                return;
            call_end(ph, (ph->state == P_ANSWERING) ? "the caller gave up" : "no answer");
            return;
        }
        if (ch == '\r')
            continue;
        if (ch != '\n') {
            if (ph->sig_len < (int) sizeof(ph->sig_in) - 1)
                ph->sig_in[ph->sig_len++] = ch;
            continue;
        }
        ph->sig_in[ph->sig_len] = '\0';
        ph->sig_len             = 0;
        if (!strcmp(ph->sig_in, "RINGING"))
            set_state(ph, P_RINGBACK, "");
        else if (!strncmp(ph->sig_in, "CONNECT", 7)) {
            ph->waiting_reply = 0;
            ph->peer_voice    = !strcmp(ph->sig_in, "CONNECT VOICE");
            ph->mic_tail      = ph->mic_head;
            ph->next_frame    = isp_now_ms();
            set_state(ph, P_TALKING, ph->peer_voice ? "" : "a modem answered");
        } else {
            call_end(ph, !strcmp(ph->sig_in, "BUSY")       ? "busy"
                         : !strcmp(ph->sig_in, "NOANSWER") ? "no answer"
                         : !strcmp(ph->sig_in, "UNKNOWN")  ? "no such number"
                         : !strcmp(ph->sig_in, "GONE")     ? "the caller gave up"
                                                           : "refused");
            return;
        }
    }
    for (;;) {
        uint8_t   buf[2048];
        const int r = recv(ph->call, (char *) buf, sizeof(buf), 0);

        if (r > 0) {
            if (ph->peer_voice && (voice_deframe(&ph->defr, buf, (size_t) r, frame_in, ph) != 0)) {
                call_end(ph, "not a voice call");
                return;
            }
            continue;
        }
        if ((r == 0) || (sock_error() != SOCK_WOULDBLOCK))
            call_end(ph, "the other end hung up");
        return;
    }
}

/* 20 ms of the page's microphone, and its DTMF, to the far end. */
static void
call_tick(phone_t *ph)
{
    const unsigned size = sizeof(ph->mic) / sizeof(ph->mic[0]);
    int16_t        s[VOICE_FRAME_SAMPLES];
    uint8_t        u[VOICE_FRAME_SAMPLES];
    uint8_t        frame[VOICE_FRAME_HDR + VOICE_FRAME_SAMPLES];

    for (int i = 0; i < VOICE_FRAME_SAMPLES; i++)
        s[i] = (ph->mic_tail != ph->mic_head) ? ph->mic[ph->mic_tail++ % size] : 0;
    (void) voice_tone_mix(&ph->tone, s, VOICE_FRAME_SAMPLES);
    for (int i = 0; i < VOICE_FRAME_SAMPLES; i++)
        u[i] = voice_ulaw_encode(s[i]);
    if (!send_all(ph->call, frame, voice_frame(frame, VOICE_FRAME_AUDIO, u, sizeof(u))))
        call_end(ph, "the line failed");
}

/* ----------------------------------------------------------- the thread */

static void
ctl_read(phone_t *ph)
{
    for (;;) {
        char      buf[256];
        const int r = recv(ph->ctl, buf, sizeof(buf), 0);

        if (r > 0) {
            for (int i = 0; i < r; i++) {
                if (buf[i] == '\r')
                    continue;
                if (buf[i] == '\n') {
                    ph->ctl_in[ph->ctl_len] = '\0';
                    ph->ctl_len             = 0;
                    ctl_line(ph, ph->ctl_in);
                } else if (ph->ctl_len < (int) sizeof(ph->ctl_in) - 1)
                    ph->ctl_in[ph->ctl_len++] = buf[i];
            }
            continue;
        }
        if ((r == 0) || (sock_error() != SOCK_WOULDBLOCK)) {
            /* isp-server is stopping. */
            sock_close(ph->ctl);
            ph->ctl = INVALID_SOCKET;
            ph->stop = 1;
        }
        return;
    }
}

static void
phone_thread(void *arg)
{
    phone_t *ph = (phone_t *) arg;

    ph->ctl = exchange_open(ph->exchange_port, "REGISTER %s Status page (browser)", ISP_PHONE_NUMBER);
    if (ph->ctl == INVALID_SOCKET)
        ws_event(ph, "{\"type\":\"error\",\"why\":\"the exchange did not answer\"}");
    set_state(ph, P_IDLE, "");
    while (!ph->stop && (ph->ctl != INVALID_SOCKET)) {
        fd_set         rd;
        struct timeval tv;
        SOCKET         top = ph->ws;
        const uint64_t now = isp_now_ms();
        int64_t        wait;

        FD_ZERO(&rd);
        FD_SET(ph->ws, &rd);
        FD_SET(ph->ctl, &rd);
        if (ph->ctl > top)
            top = ph->ctl;
        if (ph->call != INVALID_SOCKET) {
            FD_SET(ph->call, &rd);
            if (ph->call > top)
                top = ph->call;
        }
        wait = ((ph->state == P_TALKING) && ph->peer_voice) ? (int64_t) (ph->next_frame - now) : 100;
        if (wait < 0)
            wait = 0;
        tv.tv_sec  = 0;
        tv.tv_usec = (long) (wait * 1000);
        if (select((int) top + 1, &rd, NULL, NULL, &tv) < 0)
            break;
        if (FD_ISSET(ph->ws, &rd) && !page_read(ph))
            break;
        if (FD_ISSET(ph->ctl, &rd))
            ctl_read(ph);
        if ((ph->call != INVALID_SOCKET) && FD_ISSET(ph->call, &rd))
            call_read(ph);
        if ((ph->state == P_TALKING) && ph->peer_voice && (ph->call != INVALID_SOCKET)) {
            const uint64_t t = isp_now_ms();

            if ((int64_t) (t - ph->next_frame) > 200)
                ph->next_frame = t; /* fell behind: from now */
            while ((ph->call != INVALID_SOCKET) && (ph->state == P_TALKING) && (t >= ph->next_frame)) {
                call_tick(ph);
                ph->next_frame += VOICE_FRAME_MS;
            }
        }
    }
    if (ph->call != INVALID_SOCKET)
        sock_close(ph->call);
    if (ph->ctl != INVALID_SOCKET)
        sock_close(ph->ctl);
    (void) ws_send(ph, 8, NULL, 0);
    sock_close(ph->ws);
    ph->call = ph->ctl = ph->ws = INVALID_SOCKET;
}

/* ------------------------------------------------------------- the API */

int
isp_phone_accept(uintptr_t sock, const char *head, int allowed, int exchange_port)
{
    const SOCKET s = (SOCKET) sock;
    char         method[8] = "", path[64] = "";
    char         upgrade[32], key[64], version[8];
    char         reply[256];
    uint8_t      digest[20];
    char         accept[32];
    char         keyguid[128];
    phone_t     *ph = NULL;

    if ((sscanf(head, "%7s %63s", method, path) != 2) || strcmp(method, "GET"))
        return 0;
    path[strcspn(path, "?")] = '\0';
    if (strcmp(path, "/api/phone"))
        return 0;
    header(head, "Upgrade", upgrade, sizeof(upgrade));
    header(head, "Sec-WebSocket-Key", key, sizeof(key));
    header(head, "Sec-WebSocket-Version", version, sizeof(version));

    if (!allowed || strncasecmp(upgrade, "websocket", 9) || (key[0] == '\0') || strcmp(version, "13")) {
        static const char refused[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

        (void) send_all(s, refused, sizeof(refused) - 1);
        return 0;
    }

    if (phones_lock == NULL)
        phones_lock = isp_mutex_new();
    isp_mutex_lock(phones_lock);
    for (int i = 0; i < MAX_PHONES; i++) {
        if (phones[i].used && (phones[i].ws == INVALID_SOCKET) && (phones[i].thread != NULL)) {
            isp_thread_join(phones[i].thread); /* one that has finished */
            phones[i].thread = NULL;
            phones[i].used   = 0;
        }
        if (!phones[i].used && (ph == NULL)) {
            ph       = &phones[i];
            ph->used = 1;
        }
    }
    isp_mutex_unlock(phones_lock);
    if (ph == NULL) {
        static const char busy[] = "HTTP/1.1 503 Busy\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

        (void) send_all(s, busy, sizeof(busy) - 1);
        return 0;
    }

    snprintf(keyguid, sizeof(keyguid), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    sha1(keyguid, strlen(keyguid), digest);
    base64(digest, sizeof(digest), accept);
    snprintf(reply, sizeof(reply),
             "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n",
             accept);
    if (!send_all(s, reply, strlen(reply))) {
        ph->used = 0;
        return 0;
    }

    {
        isp_thread_t *t;

        memset(((char *) ph) + sizeof(ph->used), 0, sizeof(*ph) - sizeof(ph->used));
        ph->ws            = s;
        ph->ctl           = INVALID_SOCKET;
        ph->call          = INVALID_SOCKET;
        ph->exchange_port = exchange_port;
        set_nonblocking(s);
        t = isp_thread_start(phone_thread, ph);
        isp_mutex_lock(phones_lock);
        ph->thread = t;
        isp_mutex_unlock(phones_lock);
    }
    return 1;
}

void
isp_phone_stop_all(void)
{
    if (phones_lock == NULL)
        return;
    for (int i = 0; i < MAX_PHONES; i++)
        if (phones[i].used)
            phones[i].stop = 1;
    for (int i = 0; i < MAX_PHONES; i++) {
        isp_thread_t *t;

        isp_mutex_lock(phones_lock);
        t                = phones[i].thread;
        phones[i].thread = NULL;
        isp_mutex_unlock(phones_lock);
        if (t != NULL)
            isp_thread_join(t);
        phones[i].used = 0;
    }
}
