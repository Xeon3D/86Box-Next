/*
 * 86Box-Next: isp_probe -- two scripted guests against a running isp-server,
 * over TCP, the way two modems dialling it would reach it.
 *
 *   isp_probe [--host 127.0.0.1] [--port 2323] [--internet NAME]
 *
 * Both negotiate PPP and must get different addresses; each sends UDP to the
 * host through its gateway and gets the answer; one hangs up (LCP Terminate)
 * and the server must hang up on it while the other carries on.  With
 * --internet, the first also resolves NAME through the ISP's DNS and fetches
 * http://NAME/ over its own minimal TCP.  Non-zero on failure.
 */
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
#    define closesocket       close
#    define sock_wouldblock() ((errno == EWOULDBLOCK) || (errno == EAGAIN))
#endif
#include "isp_plat.h"
#include "ppp_client.h"

typedef struct {
    SOCKET s;
    int    eof;
} line_t;

static int failures;

static void
check(const char *what, int ok)
{
    if (!ok)
        failures++;
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    fflush(stdout);
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

static size_t
tcp_write(void *o, const uint8_t *b, size_t n)
{
    line_t   *l = (line_t *) o;
    const int r = send(l->s, (const char *) b, (int) n, 0);

    return (r > 0) ? (size_t) r : 0;
}

static size_t
tcp_read(void *o, uint8_t *b, size_t n)
{
    line_t   *l = (line_t *) o;
    const int r = recv(l->s, (char *) b, (int) n, 0);

    if (r == 0)
        l->eof = 1;
    else if ((r < 0) && !sock_wouldblock())
        l->eof = 1;
    return (r > 0) ? (size_t) r : 0;
}

static void
tcp_idle(void *o)
{
    (void) o;
    sleep_ms(1);
}

static int
dial(line_t *l, const char *host, int port)
{
    struct sockaddr_in sa;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((unsigned short) port);
    inet_pton(AF_INET, host, &sa.sin_addr);
    l->eof = 0;
    l->s   = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(l->s, (struct sockaddr *) &sa, sizeof(sa)) != 0)
        return 0;
#ifdef _WIN32
    {
        u_long yes = 1;

        ioctlsocket(l->s, FIONBIO, &yes);
    }
#else
    fcntl(l->s, F_SETFL, fcntl(l->s, F_GETFL, 0) | O_NONBLOCK);
#endif
    return 1;
}

static SOCKET
udp_listener(uint16_t *port)
{
    struct sockaddr_in sa;
    socklen_t          len = sizeof(sa);
    SOCKET             s   = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(0x7f000001);
    bind(s, (struct sockaddr *) &sa, sizeof(sa));
    getsockname(s, (struct sockaddr *) &sa, &len);
    *port = ntohs(sa.sin_port);
#ifdef _WIN32
    {
        u_long yes = 1;

        ioctlsocket(s, FIONBIO, &yes);
    }
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    return s;
}

static int
udp_round_trip(ppp_client_t *c, SOCKET hs, uint16_t hport, const char *msg)
{
    struct sockaddr_in from;
    socklen_t          flen = sizeof(from);
    char               buf[256];
    uint8_t            back[256];
    char               reply[300];
    int                n     = -1;
    const uint32_t     until = ppp_client_ms() + 3000;

    ppp_client_send_udp(c, c->isp_ip, 6000, hport, (const uint8_t *) msg, strlen(msg));
    while ((int32_t) (ppp_client_ms() - until) < 0) {
        ppp_client_poll(c);
        n = (int) recvfrom(hs, buf, sizeof(buf) - 1, 0, (struct sockaddr *) &from, &flen);
        if (n > 0)
            break;
        sleep_ms(1);
    }
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    snprintf(reply, sizeof(reply), "re: %s", buf);
    sendto(hs, reply, (int) strlen(reply), 0, (struct sockaddr *) &from, flen);
    n = ppp_client_recv_udp(c, 6000, back, sizeof(back) - 1, NULL, NULL, 3000);
    if (n <= 0)
        return 0;
    back[n] = '\0';
    return !strcmp(buf, msg) && !strcmp((const char *) back, reply);
}

int
main(int argc, char **argv)
{
    const char  *host     = "127.0.0.1";
    int          port     = 2323;
    const char  *internet = NULL;
    line_t       l1, l2;
    ppp_client_t c1, c2;
    SOCKET       hs;
    uint16_t     hport;
    uint32_t     ip1, ip2;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && (i + 1 < argc))
            host = argv[++i];
        else if (!strcmp(argv[i], "--port") && (i + 1 < argc))
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--internet") && (i + 1 < argc))
            internet = argv[++i];
        else {
            printf("isp_probe [--host H] [--port P] [--internet NAME]\n");
            return 2;
        }
    }
#ifdef _WIN32
    {
        WSADATA wsa;

        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
#endif
    hs = udp_listener(&hport);

    printf("== two calls to isp-server at %s:%d ==\n", host, port);
    check("call 1 connects", dial(&l1, host, port));
    check("call 2 connects", dial(&l2, host, port));
    memset(&c1, 0, sizeof(c1));
    memset(&c2, 0, sizeof(c2));
    c1.write = c2.write = tcp_write;
    c1.read = c2.read = tcp_read;
    c1.idle = c2.idle = tcp_idle;
    c1.opaque  = &l1;
    c2.opaque  = &l2;
    c1.verbose = c2.verbose = getenv("PROBE_VERBOSE") ? atoi(getenv("PROBE_VERBOSE")) : 1;
    c1.user = "alice";
    c1.password = "wonderland";
    c2.user = "bob";
    c2.password = "hunter2";
    ppp_client_init(&c1);
    ppp_client_init(&c2);

    /* The modem would spend fifteen seconds dialling and training here; the
       server must not mind. */
    sleep_ms(1500);
    check("call 1: PPP up", ppp_client_connect(&c1, 15000));
    check("call 2: PPP up", ppp_client_connect(&c2, 15000));
    check("different addresses", c1.my_ip && c2.my_ip && (c1.my_ip != c2.my_ip));
    check("call 1: UDP to the host and back", udp_round_trip(&c1, hs, hport, "probe one"));
    check("call 2: UDP to the host and back", udp_round_trip(&c2, hs, hport, "probe two"));

    if (internet != NULL) {
        uint32_t ip = ppp_client_resolve(&c1, internet, 5000);
        static uint8_t page[65536];
        char           req[256];
        int            n;

        printf("    %s resolves to %s\n", internet, ppp_client_ip_str(ip));
        check("DNS through the ISP's relay", ip != 0);
        if (ip != 0) {
            snprintf(req, sizeof(req), "GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", internet);
            n = ppp_client_tcp_fetch(&c1, ip, 80, req, page, sizeof(page) - 1, 15000);
            if (n > 0) {
                char *eol;

                page[n] = '\0';
                eol     = strchr((char *) page, '\r');
                if (eol)
                    *eol = '\0';
                printf("    http://%s/: %d bytes, \"%s\"\n", internet, n, (char *) page);
            }
            check("TCP to the Internet: an HTTP response", (n > 0) && !strncmp((char *) page, "HTTP/", 5));
        }
    }

    check("call 1 hangs up PPP (Terminate-Request acknowledged)", ppp_client_terminate(&c1, 3000));
    {
        const uint32_t until = ppp_client_ms() + 8000;

        while (!l1.eof && ((int32_t) (ppp_client_ms() - until) < 0)) {
            ppp_client_poll(&c1);
            sleep_ms(10);
        }
        check("...and the server hangs up on it", l1.eof);
    }
    check("call 2 is still on line", udp_round_trip(&c2, hs, hport, "still here"));
    closesocket(l2.s); /* carrier loss, no Terminate */
    closesocket(l1.s);
    ip1 = c1.my_ip;
    ip2 = c2.my_ip;

    /* The freed address comes back. */
    sleep_ms(300);
    check("a third call connects", dial(&l1, host, port));
    ppp_client_init(&c1);
    check("...gets PPP up", ppp_client_connect(&c1, 15000));
    printf("    third call got %s (calls 1 and 2 had %s and %s)\n", ppp_client_ip_str(c1.my_ip),
           ppp_client_ip_str(ip1), ppp_client_ip_str(ip2));
    check("...on the lowest freed address", c1.my_ip == ((ip1 < ip2) ? ip1 : ip2));
    check("...and reaches the host", udp_round_trip(&c1, hs, hport, "third"));
    closesocket(l1.s);
    closesocket(hs);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed", failures,
           (failures == 1) ? "" : "s");
    return failures ? 1 : 0;
}
