/*
 * 86Box-Next: the status page's phone (isp_phone.c), against the real
 * server in-process: this test is the page (a WebSocket client, as the
 * browser) and a modem on the exchange (sockets speaking its protocol).
 *
 *   another site's Origin is refused; the page's own gets the RFC 6455
 *   handshake right, and the phone its number, 555-0100, in the phone book
 *   the page calls a modem: RING, ANSWER ... VOICE, talking; a tone from the
 *   page's microphone reaches the modem as mu-law frames, and the modem's
 *   reaches the page as 16-bit samples; DTMF both ways; the modem hangs up
 *   a modem calls the page: ringing, answered, the page hangs up
 *   the page goes: so does its line
 *
 * No test framework; non-zero on failure.
 */
#include <math.h>
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
#    define sock_wouldblock() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#    include <arpa/inet.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
typedef int SOCKET;
#    define INVALID_SOCKET    (-1)
#    define closesocket       close
#    define sock_wouldblock() ((errno == EWOULDBLOCK) || (errno == EAGAIN))
#endif
#include <86box/modem_voice.h>
#include "isp.h"
#include "isp_plat.h"
#include "isp_srv.h"
#include "isp_web.h"

#ifndef M_PI
#    define M_PI 3.14159265358979323846
#endif

static int failures;
static int ex_port, http_port;

static void
check(const char *what, int ok)
{
    if (!ok)
        failures++;
    printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
    fflush(stdout);
}

static void
quiet_log(const char *line)
{
    if (getenv("WEB_PHONE_TEST_VERBOSE"))
        printf("      | %s\n", line);
}

static void
sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

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

static SOCKET
connect_to(int port)
{
    struct sockaddr_in sa;
    SOCKET             s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons((unsigned short) port);
    sa.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(s, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    set_nonblocking(s);
    return s;
}

static void
send_str(SOCKET s, const char *str)
{
    send(s, str, (int) strlen(str), 0);
}

/* A line from the exchange; "" on timeout, "<closed>" if it hung up. */
static const char *
hear(SOCKET s, int ms)
{
    static char    line[256];
    size_t         n     = 0;
    const uint64_t until = isp_now_ms() + (uint64_t) ms;

    while (isp_now_ms() < until) {
        char c;
        int  r = recv(s, &c, 1, 0);

        if (r == 1) {
            if (c == '\n') {
                line[n] = '\0';
                return line;
            }
            if ((c != '\r') && (n < (sizeof(line) - 1)))
                line[n++] = c;
        } else if ((r == 0) || !sock_wouldblock())
            return "<closed>";
        else
            sleep_ms(1);
    }
    line[n] = '\0';
    return "";
}

static double
tone_level(const int16_t *s, size_t n, double hz)
{
    const double c = 2.0 * cos(2.0 * M_PI * hz / 8000.0);
    double       q1 = 0.0, q2 = 0.0;

    for (size_t i = 0; i < n; i++) {
        const double q0 = c * q1 - q2 + s[i];

        q2 = q1;
        q1 = q0;
    }
    return (n > 0) ? (sqrt(q1 * q1 + q2 * q2 - c * q1 * q2) / (double) n) : 0.0;
}

/* Which of 600 and 1000 Hz, by a clear margin; 0 if neither. */
static int
which_tone(const int16_t *s, size_t n)
{
    const double a = tone_level(s, n, 600.0), b = tone_level(s, n, 1000.0);

    return (a > 5.0 * b) ? 600 : ((b > 5.0 * a) ? 1000 : 0);
}

/* ------------------------------------------------------- the page's side */

typedef struct {
    SOCKET  s;
    uint8_t buf[65536];
    size_t  n;
} ws_t;

/* The handshake with a given Origin: the server's status line and accept
   key, or "" for none. */
static int
ws_open(ws_t *w, const char *origin, char *status, size_t slen, char *accept, size_t alen)
{
    char           req[512];
    char           resp[1024];
    size_t         n     = 0;
    const uint64_t until = isp_now_ms() + 3000;

    status[0] = accept[0] = '\0';
    memset(w, 0, sizeof(*w));
    w->s = connect_to(http_port);
    if (w->s == INVALID_SOCKET)
        return 0;
    snprintf(req, sizeof(req),
             "GET /api/phone HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\nOrigin: %s\r\n\r\n",
             http_port, origin);
    send_str(w->s, req);
    while ((isp_now_ms() < until) && (n < sizeof(resp) - 1)) {
        const int r = recv(w->s, &resp[n], 1, 0);

        if (r == 1) {
            n++;
            resp[n] = '\0';
            if ((n >= 4) && !memcmp(&resp[n - 4], "\r\n\r\n", 4))
                break;
        } else if ((r == 0) || !sock_wouldblock())
            break;
        else
            sleep_ms(1);
    }
    resp[n] = '\0';
    snprintf(status, slen, "%.*s", (int) strcspn(resp, "\r\n"), resp);
    {
        const char *a = strstr(resp, "Sec-WebSocket-Accept: ");

        if (a != NULL)
            snprintf(accept, alen, "%.*s", (int) strcspn(a + 22, "\r\n"), a + 22);
    }
    return 1;
}

static void
ws_send(ws_t *w, int op, const void *data, size_t len)
{
    uint8_t        hdr[8];
    size_t         h       = 0;
    const uint8_t  mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    uint8_t       *m       = (uint8_t *) malloc(len + 1);

    hdr[h++] = (uint8_t) (0x80 | op);
    if (len < 126)
        hdr[h++] = (uint8_t) (0x80 | len);
    else {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (uint8_t) (len >> 8);
        hdr[h++] = (uint8_t) len;
    }
    memcpy(&hdr[h], mask, 4);
    h += 4;
    for (size_t i = 0; i < len; i++)
        m[i] = ((const uint8_t *) data)[i] ^ mask[i & 3];
    send(w->s, (const char *) hdr, (int) h, 0);
    if (len > 0)
        send(w->s, (const char *) m, (int) len, 0);
    free(m);
}

static void
ws_cmd(ws_t *w, const char *json)
{
    ws_send(w, 1, json, strlen(json));
}

/* The next message within `ms`: its opcode (1 text, 2 binary, 8 close), 0 on
   timeout, -1 if the connection went. */
static int
ws_next(ws_t *w, uint8_t *out, size_t *len, int ms)
{
    const uint64_t until = isp_now_ms() + (uint64_t) ms;

    for (;;) {
        if (w->n >= 2) {
            size_t h = 2, l = w->buf[1] & 0x7f;

            if (l == 126 && w->n >= 4) {
                l = ((size_t) w->buf[2] << 8) | w->buf[3];
                h = 4;
            }
            if ((l != 126) && (w->n >= h + l)) {
                const int op = w->buf[0] & 0x0f;

                memcpy(out, &w->buf[h], l);
                out[l] = 0;
                *len   = l;
                memmove(w->buf, &w->buf[h + l], w->n - (h + l));
                w->n -= h + l;
                return op;
            }
        }
        {
            const int r = recv(w->s, (char *) &w->buf[w->n], (int) (sizeof(w->buf) - w->n), 0);

            if (r > 0) {
                w->n += (size_t) r;
                continue;
            }
            if ((r == 0) || !sock_wouldblock())
                return -1;
        }
        if (isp_now_ms() >= until)
            return 0;
        sleep_ms(1);
    }
}

/* Waits for a text message containing `what`; audio meanwhile is kept in
   `audio` if given. */
static int16_t page_heard[16000];
static size_t  page_heard_n;

static int
ws_wait(ws_t *w, const char *what, int ms)
{
    static uint8_t msg[65536];
    const uint64_t until = isp_now_ms() + (uint64_t) ms;

    while (isp_now_ms() < until) {
        size_t    len;
        const int op = ws_next(w, msg, &len, 50);

        if (op < 0)
            return 0;
        if ((op == 1) && strstr((const char *) msg, what))
            return 1;
        if (op == 2)
            for (size_t i = 0; (i + 1 < len) && (page_heard_n < 16000); i += 2)
                page_heard[page_heard_n++] = (int16_t) (uint16_t) (msg[i] | (msg[i + 1] << 8));
    }
    return 0;
}

/* --------------------------------------------------- the modem's side */

static int16_t modem_heard[16000];
static size_t  modem_heard_n;
static char    modem_digits[16];

static void
modem_frame(int type, const uint8_t *p, size_t len, void *priv)
{
    (void) priv;
    if (type == VOICE_FRAME_AUDIO)
        for (size_t i = 0; (i < len) && (modem_heard_n < 16000); i++)
            modem_heard[modem_heard_n++] = voice_ulaw_decode(p[i]);
    else if ((type == VOICE_FRAME_DTMF) && (len >= 1) && (strlen(modem_digits) < sizeof(modem_digits) - 1))
        modem_digits[strlen(modem_digits)] = (char) p[0];
}

/* `ms` of the call: the page sends its microphone (a tone at page_hz, or
   nothing), the modem its own (modem_hz), each paced at 20 ms; what each
   hears is kept. */
static void
talk(ws_t *w, SOCKET m, double page_hz, double modem_hz, int ms)
{
    voice_deframer_t d;
    const uint64_t   t0 = isp_now_ms();
    uint64_t         next = t0;
    int              k    = 0;

    memset(&d, 0, sizeof(d));
    while (isp_now_ms() < t0 + (uint64_t) ms) {
        static uint8_t msg[65536];
        size_t         len;
        int            op;

        if (isp_now_ms() >= next) {
            int16_t pcm[160];
            uint8_t u[160], frame[VOICE_FRAME_HDR + 160];

            for (int i = 0; i < 160; i++, k++) {
                pcm[i] = (int16_t) ((page_hz > 0) ? 9000.0 * sin(2 * M_PI * page_hz * k / 8000.0) : 0);
                u[i]   = voice_ulaw_encode((int16_t) ((modem_hz > 0) ? 9000.0 * sin(2 * M_PI * modem_hz * k / 8000.0) : 0));
            }
            if (page_hz > 0)
                ws_send(w, 2, pcm, sizeof(pcm));
            if (modem_hz > 0)
                send(m, (const char *) frame, (int) voice_frame(frame, VOICE_FRAME_AUDIO, u, 160), 0);
            next += 20;
        }
        while ((op = ws_next(w, msg, &len, 0)) > 0)
            if (op == 2)
                for (size_t i = 0; (i + 1 < len) && (page_heard_n < 16000); i += 2)
                    page_heard[page_heard_n++] = (int16_t) (uint16_t) (msg[i] | (msg[i + 1] << 8));
        {
            uint8_t   buf[4096];
            const int r = recv(m, (char *) buf, sizeof(buf), 0);

            if (r > 0)
                (void) voice_deframe(&d, buf, (size_t) r, modem_frame, NULL);
        }
        sleep_ms(1);
    }
}

static int
in_phone_book(const char *number, char *label, size_t len)
{
    isp_srv_line_t lines[16];
    const int      n = isp_srv_lines(lines, 16);

    for (int i = 0; i < n; i++)
        if (!strcmp(lines[i].number, number)) {
            if (label != NULL)
                snprintf(label, len, "%s", lines[i].label);
            return 1;
        }
    return 0;
}

int
main(void)
{
    isp_srv_config_t cfg;
    ws_t            *w = (ws_t *) calloc(1, sizeof(ws_t));
    char             status[128], accept[64], label[64];
    SOCKET           ctl, call;
    int              id = 0;

#ifdef _WIN32
    WSADATA wsa;

    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.listen, sizeof(cfg.listen), "127.0.0.1");
    cfg.port      = 0;
    cfg.http_port = 0;
    cfg.log       = quiet_log;
    isp_srv_load(&cfg);
    if (isp_srv_start(&cfg) != 0) {
        printf("the server will not start\n");
        return 1;
    }
    ex_port   = cfg.port;
    http_port = cfg.http_port;
    printf("== the status page's phone (page on %d, exchange on %d) ==\n", http_port, ex_port);

    {
        char origin[64];

        ws_open(w, "http://evil.example", status, sizeof(status), accept, sizeof(accept));
        check("another site's page cannot open the phone: 403", !strncmp(status, "HTTP/1.1 403", 12));
        closesocket(w->s);
        snprintf(origin, sizeof(origin), "http://127.0.0.1:%d", http_port);
        ws_open(w, origin, status, sizeof(status), accept, sizeof(accept));
    }
    check("the page's own: 101 Switching Protocols", !strncmp(status, "HTTP/1.1 101", 12));
    check("...with RFC 6455's accept key for its sample key", !strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    check("the phone gets its number: 555-0100", ws_wait(w, "\"number\":\"5550100\"", 3000));
    check("...in the phone book as the status page",
          in_phone_book("5550100", label, sizeof(label)) && strstr(label, "Status page"));

    /* A modem on the exchange. */
    ctl = connect_to(ex_port);
    send_str(ctl, ISP_EXCHANGE_GREETING "REGISTER 555-0101 Win98 (COM1)\r\n");
    hear(ctl, 3000);

    /* The page calls it. */
    ws_cmd(w, "{\"cmd\":\"dial\",\"number\":\"555-0101\"}");
    check("the page dials 555-0101: it rings, from 5550100",
          sscanf(hear(ctl, 3000), "RING %d 5550100", &id) == 1);
    check("...the page hears it ringing", ws_wait(w, "\"state\":\"ringback\"", 3000));
    call = connect_to(ex_port);
    {
        char req[96];

        snprintf(req, sizeof(req), ISP_EXCHANGE_GREETING "ANSWER %d VOICE\r\n", id);
        send_str(call, req);
    }
    check("the modem answers as a voice: CONNECT VOICE", !strcmp(hear(call, 3000), "CONNECT VOICE"));
    check("...and the page is talking", ws_wait(w, "\"state\":\"talking\"", 3000));
    page_heard_n = modem_heard_n = 0;
    talk(w, call, 1000.0, 600.0, 1500);
    check("the page's microphone (1 kHz) reaches the modem",
          (modem_heard_n > 6000) && (which_tone(modem_heard + 2000, modem_heard_n - 2000) == 1000));
    check("the modem's voice (600 Hz) reaches the page",
          (page_heard_n > 6000) && (which_tone(page_heard + 2000, page_heard_n - 2000) == 600));
    ws_cmd(w, "{\"cmd\":\"dtmf\",\"digit\":\"7\"}");
    talk(w, call, 0, 0, 300);
    check("the page presses 7: the modem gets the digit", strchr(modem_digits, '7') != NULL);
    {
        uint8_t       frame[VOICE_FRAME_HDR + 1];
        const uint8_t d = '3';

        send(call, (const char *) frame, (int) voice_frame(frame, VOICE_FRAME_DTMF, &d, 1), 0);
    }
    check("the modem presses 3: the page is told", ws_wait(w, "\"digit\":\"3\"", 2000));
    closesocket(call);
    check("the modem hangs up: the page is idle, told why",
          ws_wait(w, "\"state\":\"idle\",\"why\":\"the other end hung up\"", 3000));

    /* The modem calls the page. */
    call = connect_to(ex_port);
    send_str(call, ISP_EXCHANGE_GREETING "DIAL 5550101 5550100 VOICE\r\n");
    check("the modem dials 555-0100: the page rings, with its number",
          ws_wait(w, "\"state\":\"ringing\",\"why\":\"\",\"peer\":\"5550101\"", 3000));
    check("...the modem hears RINGING", !strcmp(hear(call, 3000), "RINGING"));
    ws_cmd(w, "{\"cmd\":\"answer\"}");
    check("the page answers: the modem hears CONNECT VOICE", !strcmp(hear(call, 3000), "CONNECT VOICE"));
    check("...and the page is talking", ws_wait(w, "\"state\":\"talking\"", 3000));
    ws_cmd(w, "{\"cmd\":\"hangup\"}");
    {
        int            gone  = 0;
        const uint64_t until = isp_now_ms() + 3000;

        /* The page's frames keep coming until it hangs up: read them all. */
        while (!gone && (isp_now_ms() < until)) {
            char      buf[4096];
            const int r = recv(call, buf, sizeof(buf), 0);

            gone = (r == 0) || ((r < 0) && !sock_wouldblock());
            if (r < 0)
                sleep_ms(5);
        }
        check("the page hangs up: the modem's line drops", gone);
    }
    closesocket(call);

    /* The page goes away. */
    closesocket(w->s);
    {
        const uint64_t until = isp_now_ms() + 3000;

        while (in_phone_book("5550100", NULL, 0) && (isp_now_ms() < until))
            sleep_ms(20);
        check("the page closes: its line leaves the phone book", !in_phone_book("5550100", NULL, 0));
    }

    closesocket(ctl);
    isp_srv_stop();
    free(w);
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed", failures, (failures == 1) ? "" : "s");
    return failures ? 1 : 0;
}
