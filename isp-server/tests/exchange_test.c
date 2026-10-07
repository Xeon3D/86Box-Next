/*
 * 86Box-Next: isp-server's telephone exchange (isp-server/isp_srv.c),
 * the real server started in-process on ports of its own, and modems played
 * by sockets speaking its protocol.  No test framework; non-zero on failure.
 *
 *   numbers: asked for, taken, assigned; a dial with a prefix
 *   a call: RINGING, RING on the callee's line, BUSY for a third, ANSWER,
 *           CONNECT on both, bytes both ways (a lot of them, exactly),
 *           either end hanging up ends it for both
 *   the caller giving up (CANCEL), an answer too late (GONE), the callee's
 *   line unplugged while it rang (NOANSWER), the page hanging up, dialling
 *   oneself (BUSY)
 *   the ISP: an unknown number (CONNECT, then PPP), switched off (UNKNOWN),
 *   the ISP's own number, and a plain TCP line with no greeting.
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
#include "isp.h"
#include "isp_plat.h"
#include "isp_srv.h"
#include "isp_web.h"
#include "ppp_client.h"

static int failures;
static int port;

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
    if (getenv("EXCHANGE_TEST_VERBOSE"))
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
dial_server(void)
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
say(SOCKET s, const char *fmt, ...)
{
    char    buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    send(s, buf, (int) strlen(buf), 0);
}

/* One line from the exchange, CR and LF removed; "" on timeout, "<closed>"
   if it hung up. */
static const char *
hear(SOCKET s, int ms)
{
    static char    line[256];
    size_t         n     = 0;
    const uint32_t until = ppp_client_ms() + (uint32_t) ms;

    while ((int32_t) (ppp_client_ms() - until) < 0) {
        char c;
        int  r = recv(s, &c, 1, 0);

        if (r == 1) {
            if (c == '\n') {
                line[n] = '\0';
                return line;
            }
            if ((c != '\r') && (n < (sizeof(line) - 1)))
                line[n++] = c;
        } else if ((r == 0) || !sock_wouldblock()) {
            return "<closed>";
        } else
            sleep_ms(1);
    }
    line[n] = '\0';
    return "";
}

/* 1 if the far end has closed the connection within `ms`. */
static int
closed_within(SOCKET s, int ms)
{
    const uint32_t until = ppp_client_ms() + (uint32_t) ms;
    char           buf[512];

    while ((int32_t) (ppp_client_ms() - until) < 0) {
        const int r = recv(s, buf, sizeof(buf), 0);

        if ((r == 0) || ((r < 0) && !sock_wouldblock()))
            return 1;
        if (r < 0)
            sleep_ms(5);
    }
    return 0;
}

static SOCKET
plug_in(const char *number, const char *label, char *got, size_t len)
{
    SOCKET      s = dial_server();
    const char *l;

    say(s, ISP_EXCHANGE_GREETING "REGISTER %s %s\r\n", number, label);
    l = hear(s, 3000);
    snprintf(got, len, "%s", !strncmp(l, "NUMBER ", 7) ? l + 7 : l);
    return s;
}

static int
line_state(const char *number)
{
    isp_srv_line_t lines[16];
    const int      n = isp_srv_lines(lines, 16);

    for (int i = 0; i < n; i++)
        if (!strcmp(lines[i].number, number))
            return lines[i].state;
    return -1;
}

/* Exactly `len` bytes each way at once, through the exchange. */
static int
transfer(SOCKET a, SOCKET b, size_t len)
{
    uint8_t       *out_a = (uint8_t *) malloc(len);
    uint8_t       *out_b = (uint8_t *) malloc(len);
    uint8_t       *in_a  = (uint8_t *) malloc(len);
    uint8_t       *in_b  = (uint8_t *) malloc(len);
    size_t         sa = 0, sb = 0, ra = 0, rb = 0;
    const uint32_t until = ppp_client_ms() + 20000;
    int            ok;

    for (size_t i = 0; i < len; i++) {
        out_a[i] = (uint8_t) (i * 31 + (i >> 9));
        out_b[i] = (uint8_t) (i * 17 + 5);
    }
    while (((ra < len) || (rb < len)) && ((int32_t) (ppp_client_ms() - until) < 0)) {
        int r;

        if (sa < len && (r = send(a, (const char *) out_a + sa, (int) (len - sa), 0)) > 0)
            sa += (size_t) r;
        if (sb < len && (r = send(b, (const char *) out_b + sb, (int) (len - sb), 0)) > 0)
            sb += (size_t) r;
        if ((r = recv(b, (char *) in_b + rb, (int) (len - rb), 0)) > 0)
            rb += (size_t) r;
        if ((r = recv(a, (char *) in_a + ra, (int) (len - ra), 0)) > 0)
            ra += (size_t) r;
        sleep_ms(1);
    }
    ok = (ra == len) && (rb == len) && !memcmp(in_b, out_a, len) && !memcmp(in_a, out_b, len);
    free(out_a);
    free(out_b);
    free(in_a);
    free(in_b);
    return ok;
}

/* ------------------------------------------------- the ISP over a socket */

typedef struct {
    SOCKET s;
} line_t;

static size_t
tcp_write(void *o, const uint8_t *b, size_t n)
{
    const int r = send(((line_t *) o)->s, (const char *) b, (int) n, 0);

    return (r > 0) ? (size_t) r : 0;
}

static size_t
tcp_read(void *o, uint8_t *b, size_t n)
{
    const int r = recv(((line_t *) o)->s, (char *) b, (int) n, 0);

    return (r > 0) ? (size_t) r : 0;
}

static void
tcp_idle(void *o)
{
    (void) o;
    sleep_ms(1);
}

static int
ppp_over(SOCKET s, uint32_t *ip)
{
    line_t       l = { s };
    ppp_client_t c;
    int          ok;

    memset(&c, 0, sizeof(c));
    c.write  = tcp_write;
    c.read   = tcp_read;
    c.idle   = tcp_idle;
    c.opaque = &l;
    ppp_client_init(&c);
    ok = ppp_client_connect(&c, 10000);
    if (ip != NULL)
        *ip = c.my_ip;
    return ok;
}

/* ------------------------------------------------------------------- main */

int
main(void)
{
    isp_srv_config_t cfg;
    char             n1[32], n2[32], n3[32];
    SOCKET           l1, l2, l3;
    SOCKET           a, b;
    const char      *r;
    int              id;

#ifdef _WIN32
    WSADATA wsa;

    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.listen, sizeof(cfg.listen), "127.0.0.1");
    cfg.port      = 0;  /* any free one */
    cfg.http_port = -1; /* no page: isp_web is tested on its own */
    cfg.log       = quiet_log;
    isp_srv_load(&cfg);
    if (isp_srv_start(&cfg) != 0) {
        printf("the server will not start\n");
        return 1;
    }
    port = cfg.port;
    printf("== isp-server's telephone exchange, on port %d ==\n", port);

    /* Numbers. */
    l1 = plug_in("555-0101", "Win98 (COM2)", n1, sizeof(n1));
    l2 = plug_in("5550101", "WfW 3.11 (COM1)", n2, sizeof(n2));
    l3 = plug_in("-", "OS/2 (COM3)", n3, sizeof(n3));
    check("a modem gets the number it asks for", !strcmp(n1, "5550101"));
    check("...another asking for it gets the next free one", !strcmp(n2, "5550102"));
    check("...one asking for none is given one", !strcmp(n3, "5550103"));
    check("three lines in the phone book, idle",
          (line_state(n1) == ISP_LINE_IDLE) && (line_state(n2) == ISP_LINE_IDLE) && (line_state(n3) == ISP_LINE_IDLE));

    /* 101 calls 102, with an outside-line prefix. */
    a = dial_server();
    say(a, ISP_EXCHANGE_GREETING "DIAL %s 9,555-0102\r\n", n1);
    check("the caller hears RINGING", !strcmp(hear(a, 3000), "RINGING"));
    r = hear(l2, 3000);
    check("the callee's line gets RING, with the caller's number",
          (sscanf(r, "RING %d", &id) == 1) && (strstr(r, " 5550101") != NULL));
    check("...and is listed as ringing", line_state(n2) == ISP_LINE_RINGING);
    {
        SOCKET c = dial_server();

        say(c, ISP_EXCHANGE_GREETING "DIAL %s 5550102\r\n", n3);
        check("a third modem dialling it now: BUSY", !strcmp(hear(c, 3000), "BUSY"));
        closesocket(c);
    }
    b = dial_server();
    say(b, ISP_EXCHANGE_GREETING "ANSWER %d\r\n", id);
    check("the callee answers: CONNECT", !strcmp(hear(b, 3000), "CONNECT"));
    check("...and the caller hears CONNECT", !strcmp(hear(a, 3000), "CONNECT"));
    check("256 KB each way at once, exactly", transfer(a, b, 256 * 1024));
    {
        isp_srv_pcall_t pc[4];
        const int       n = isp_srv_phone_calls(pc, 4);

        check("the call is listed, connected, with its traffic",
              (n == 1) && (pc[0].state == ISP_PCALL_ACTIVE) && !strcmp(pc[0].from, n1) && !strcmp(pc[0].to, n2) &&
              (pc[0].from_bytes == 256 * 1024) && (pc[0].to_bytes == 256 * 1024));
        check("...both lines busy", (line_state(n1) == ISP_LINE_BUSY) && (line_state(n2) == ISP_LINE_BUSY));
    }
    closesocket(a);
    check("the caller hangs up: the callee's line drops", closed_within(b, 3000));
    closesocket(b);
    sleep_ms(200);
    {
        isp_srv_pcall_t pc[4];

        check("...and both are idle again, the call gone",
              (line_state(n1) == ISP_LINE_IDLE) && (line_state(n2) == ISP_LINE_IDLE) && (isp_srv_phone_calls(pc, 4) == 0));
    }

    /* The caller gives up while it rings. */
    a = dial_server();
    say(a, ISP_EXCHANGE_GREETING "DIAL %s 5550102\r\n", n3);
    hear(a, 3000);
    r = hear(l2, 3000);
    sscanf(r, "RING %d", &id);
    closesocket(a);
    {
        char want[32];

        snprintf(want, sizeof(want), "CANCEL %d", id);
        check("the caller gives up: the callee hears CANCEL", !strcmp(hear(l2, 3000), want));
    }
    b = dial_server();
    say(b, ISP_EXCHANGE_GREETING "ANSWER %d\r\n", id);
    check("...and an answer after that: GONE", !strcmp(hear(b, 3000), "GONE"));
    closesocket(b);

    /* The callee hangs up first; the page hangs up a call. */
    a = dial_server();
    say(a, ISP_EXCHANGE_GREETING "DIAL %s 5550103\r\n", n2);
    hear(a, 3000);
    sscanf(hear(l3, 3000), "RING %d", &id);
    b = dial_server();
    say(b, ISP_EXCHANGE_GREETING "ANSWER %d\r\n", id);
    hear(b, 3000);
    hear(a, 3000);
    closesocket(b);
    check("the callee hangs up: the caller's line drops", closed_within(a, 3000));
    closesocket(a);

    a = dial_server();
    say(a, ISP_EXCHANGE_GREETING "DIAL %s 5550103\r\n", n1);
    hear(a, 3000);
    sscanf(hear(l3, 3000), "RING %d", &id);
    b = dial_server();
    say(b, ISP_EXCHANGE_GREETING "ANSWER %d\r\n", id);
    hear(b, 3000);
    hear(a, 3000);
    check("the page hangs up a call", isp_srv_hangup_phone(id));
    check("...both lines drop", closed_within(a, 3000) && closed_within(b, 3000));
    closesocket(a);
    closesocket(b);

    /* Voice calls: each end's CONNECT says what the other is. */
    {
        static const struct {
            int         caller, callee;
            const char *caller_hears, *callee_hears, *what;
        } kinds[] = {
            { 1, 1, "CONNECT VOICE", "CONNECT VOICE", "a voice call answered as one: CONNECT VOICE, both ends" },
            { 1, 0, "CONNECT", "CONNECT VOICE", "a voice call answered by a modem: each hears what the other is" },
            { 0, 1, "CONNECT VOICE", "CONNECT", "a modem's call answered by a voice: likewise" }
        };

        for (int k = 0; k < 3; k++) {
            isp_srv_pcall_t pc[4];
            int             np;

            a = dial_server();
            say(a, ISP_EXCHANGE_GREETING "DIAL %s 5550103%s\r\n", n1, kinds[k].caller ? " VOICE" : "");
            hear(a, 3000);
            sscanf(hear(l3, 3000), "RING %d", &id);
            b = dial_server();
            say(b, ISP_EXCHANGE_GREETING "ANSWER %d%s\r\n", id, kinds[k].callee ? " VOICE" : "");
            {
                char cb[64], ca[64];

                snprintf(cb, sizeof(cb), "%s", hear(b, 3000));
                snprintf(ca, sizeof(ca), "%s", hear(a, 3000));
                check(kinds[k].what, !strcmp(ca, kinds[k].caller_hears) && !strcmp(cb, kinds[k].callee_hears));
            }
            np = isp_srv_phone_calls(pc, 4);
            check("...and the page knows which end is a voice",
                  (np == 1) && (pc[0].voice == ((kinds[k].caller ? ISP_PCALL_FROM_VOICE : 0) |
                                                (kinds[k].callee ? ISP_PCALL_TO_VOICE : 0))));
            closesocket(a);
            closed_within(b, 3000);
            closesocket(b);
            {
                const uint64_t until = isp_now_ms() + 3000;

                while ((isp_srv_phone_calls(pc, 4) > 0) && (isp_now_ms() < until))
                    sleep_ms(10);
            }
        }
    }

    /* Dialling oneself. */
    a = dial_server();
    say(a, ISP_EXCHANGE_GREETING "DIAL %s 5550101\r\n", n1);
    check("a modem dialling its own number: BUSY", !strcmp(hear(a, 3000), "BUSY"));
    closesocket(a);

    /* The callee's line is unplugged while it rings. */
    a = dial_server();
    say(a, ISP_EXCHANGE_GREETING "DIAL %s 5550103\r\n", n1);
    hear(a, 3000);
    hear(l3, 3000);
    closesocket(l3);
    check("the callee unplugged while it rang: NOANSWER", !strcmp(hear(a, 3000), "NOANSWER"));
    closesocket(a);
    sleep_ms(300);
    check("...and its line has left the phone book", line_state(n3) == -1);

    /* The ISP. */
    a = dial_server();
    say(a, ISP_EXCHANGE_GREETING "DIAL %s 0191 555 1234\r\n", n1);
    check("a number that is no modem's reaches the ISP: CONNECT", !strcmp(hear(a, 3000), "CONNECT"));
    {
        uint32_t        ip = 0;
        isp_call_info_t calls[4];
        int             n;

        check("...and PPP comes up over it", ppp_over(a, &ip) && (ip != 0));
        n = isp_list_calls(calls, 4);
        check("...listed as an ISP call from 555-0101 (Win98)",
              (n == 1) && strstr(calls[0].label, "555-0101") && strstr(calls[0].label, "Win98"));
        check("...its line busy meanwhile", line_state(n1) == ISP_LINE_BUSY);
    }
    closesocket(a);
    sleep_ms(500);

    isp_srv_set_phone(0, "0191");
    a = dial_server();
    say(a, ISP_EXCHANGE_GREETING "DIAL %s 0800 4711\r\n", n1);
    check("with other numbers switched off: UNKNOWN", !strcmp(hear(a, 3000), "UNKNOWN"));
    closesocket(a);
    a = dial_server();
    say(a, ISP_EXCHANGE_GREETING "DIAL %s 0191\r\n", n1);
    check("...but the ISP's own number still answers", !strcmp(hear(a, 3000), "CONNECT"));
    closesocket(a);
    isp_srv_set_phone(1, "");

    /* A plain TCP line: PPP from the first byte, no greeting. */
    a = dial_server();
    check("a plain TCP line with no greeting gets PPP from the ISP", ppp_over(a, NULL));
    closesocket(a);

    closesocket(l1);
    closesocket(l2);
    isp_srv_stop();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed", failures,
           (failures == 1) ? "" : "s");
    return failures ? 1 : 0;
}
