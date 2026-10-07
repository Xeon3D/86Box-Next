/*
 * 86Box-Next: the SLiRP network driver (src/network/net_slirp.c), the real
 * one, behind a network card made of stubs.  The guest is the virtual ISP
 * tests' scripted client (isp-server/tests/ppp_client.c) through an adapter that
 * turns its IPv4 packets into Ethernet frames on the card's transmit queue,
 * turns frames from SLiRP back into packets, and answers SLiRP's ARP for the
 * guest.  No test framework; non-zero on failure.
 *
 *   - UDP to the host (10.0.2.2 is its loopback) and back;
 *   - an idle UDP socket must not bring ICMP port unreachables: libslirp
 *     sends one for every recvfrom() that fails, would-block included, so a
 *     driver that reports a socket readable when it is not floods the guest;
 *   - outbound TCP connects, many in a row, each answered with a SYN-ACK.
 *     With WSAEventSelect re-registered every pass, an FD_CONNECT that lands
 *     between one pass's look and the next registration is lost.
 *
 *   net_slirp_tests [--internet NAME]   also: DNS, and TCP to NAME:80, 10 times
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
#else
#    include <arpa/inet.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
typedef int SOCKET;
#    define closesocket close
#endif
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/thread.h>
#include <86box/network.h>
#include <86box/ini.h>
#include "isp_plat.h"
#include "ppp_framing.h"
#include "ppp_session.h"
#include "ppp_client.h"

extern const netdrv_t net_slirp_drv;
extern void          *net_slirp_init(const netcard_t *card, const uint8_t *mac_addr, void *priv, char *netdrv_errbuf);
extern void           net_slirp_close(void *priv);
extern void           net_slirp_in_available(void *priv);

/* ------------------------------------------------------------ 86Box stubs */

netcard_conf_t net_cards_conf[NET_CONF_MAX];
uint16_t       net_card_current;
uint64_t       tsc;
double         cpuclock = 1000000000.0; /* tsc counts nanoseconds */

void
pclog(const char *fmt, ...)
{
    va_list ap;

    if (!getenv("NET_SLIRP_TEST_VERBOSE"))
        return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void          *config_get_ini(void) { return NULL; }
ini_section_t  ini_find_section(ini_t ini, const char *name) { (void) ini; (void) name; return NULL; }
int            ini_section_get_int(ini_section_t s, const char *n, int def) { (void) s; (void) n; return def; }
char          *ini_section_get_string(ini_section_t s, const char *n, char *def) { (void) s; (void) n; return def; }
const char    *network_card_get_internal_name(int n) { (void) n; return "ne2k"; }

/* The RA timer, which an IPv4-only SLiRP never needs. */
void timer_add(pc_timer_t *t, void (*cb)(void *), void *p, int start) { (void) t; (void) cb; (void) p; (void) start; }
void timer_stop(pc_timer_t *t) { (void) t; }
void timer_on_auto(pc_timer_t *t, double period) { (void) t; (void) period; }

thread_t *
thread_create_named(void (*fn)(void *), void *param, const char *name)
{
    (void) name;
    return (thread_t *) isp_thread_start(fn, param);
}

int
thread_wait(thread_t *t)
{
    isp_thread_join((isp_thread_t *) t);
    return 0;
}

/* The card's queues: frames from the guest to SLiRP, and from SLiRP back. */
#define QLEN 512

typedef struct {
    uint8_t data[QLEN][NET_MAX_FRAME];
    int     len[QLEN];
    int     head;
    int     tail;
} fqueue_t;

static fqueue_t     txq, rxq;
static isp_mutex_t *qlock;

static int
fq_put(fqueue_t *q, const uint8_t *d, int len)
{
    int ok = 0;

    isp_mutex_lock(qlock);
    if ((((q->head + 1) % QLEN) != q->tail) && (len <= NET_MAX_FRAME)) {
        memcpy(q->data[q->head], d, (size_t) len);
        q->len[q->head] = len;
        q->head         = (q->head + 1) % QLEN;
        ok              = 1;
    }
    isp_mutex_unlock(qlock);
    return ok;
}

static int
fq_get(fqueue_t *q, uint8_t *d)
{
    int len = -1;

    isp_mutex_lock(qlock);
    if (q->tail != q->head) {
        len = q->len[q->tail];
        memcpy(d, q->data[q->tail], (size_t) len);
        q->tail = (q->tail + 1) % QLEN;
    }
    isp_mutex_unlock(qlock);
    return len;
}

int
network_tx_popv(netcard_t *card, netpkt_t *v, int n)
{
    int i = 0;

    (void) card;
    while (i < n) {
        const int len = fq_get(&txq, v[i].data);

        if (len < 0)
            break;
        v[i++].len = len;
    }
    return i;
}

int
network_rx_put_pkt(netcard_t *card, netpkt_t *pkt)
{
    (void) card;
    return fq_put(&rxq, pkt->data, pkt->len);
}

/* Frames SLiRP sends while handling the guest's: straight onto the queue. */
int
network_rx_on_tx_put_pkt(netcard_t *card, netpkt_t *pkt)
{
    return network_rx_put_pkt(card, pkt);
}

int
network_rx_on_tx_popv(netcard_t *card, netpkt_t *v, int n)
{
    (void) card;
    (void) v;
    (void) n;
    return 0;
}

/* -------------------------------------------- the guest, on Ethernet */

static const uint8_t guest_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint8_t       slirp_mac[6] = { 0x52, 0x55, 10, 0, 2, 2 };
#define GUEST_IP 0x0a00020f
#define HOST_IP  0x0a000202
#define DNS_IP   0x0a000203

static void   *slirp_priv;
static ppp_rx_t to_card; /* the client's PPP frames, decoded */
static uint32_t icmp_seen;
static SOCKET   tcp_listener = (SOCKET) -1;
static int      tcp_served;
static ppp_client_t *busy;      /* a guest that keeps sending while it connects */
static uint16_t      busy_port;

static void
send_frame(const uint8_t *f, int len)
{
    fq_put(&txq, f, len);
    net_slirp_in_available(slirp_priv);
}

static void
client_frame(void *opaque, const uint8_t *f, size_t n)
{
    uint8_t e[NET_MAX_FRAME];

    (void) opaque;
    if ((n < 4) || (f[0] != 0xff) || (f[2] != 0x00) || (f[3] != 0x21) || ((n - 4 + 14) > sizeof(e)))
        return; /* only IP goes on the wire */
    memcpy(&e[0], slirp_mac, 6);
    memcpy(&e[6], guest_mac, 6);
    e[12] = 0x08;
    e[13] = 0x00;
    memcpy(&e[14], f + 4, n - 4);
    send_frame(e, (int) (n - 4 + 14));
}

static size_t
eth_write(void *o, const uint8_t *b, size_t n)
{
    (void) o;
    ppp_rx_feed(&to_card, b, n, client_frame, NULL);
    return n;
}

static size_t
eth_read(void *o, uint8_t *b, size_t n)
{
    uint8_t f[NET_MAX_FRAME];
    int     len;

    (void) o;
    while ((len = fq_get(&rxq, f)) >= 0) {
        const int type = (f[12] << 8) | f[13];

        if ((type == 0x0806) && (len >= 42) && (f[21] == 1) &&
            ((((uint32_t) f[38] << 24) | ((uint32_t) f[39] << 16) | ((uint32_t) f[40] << 8) | f[41]) == GUEST_IP)) {
            /* Who has the guest's address: the guest does. */
            uint8_t r[42];

            memcpy(&r[0], &f[6], 6);
            memcpy(&r[6], guest_mac, 6);
            r[12] = 0x08;
            r[13] = 0x06;
            memcpy(&r[14], &f[14], 6); /* hardware, protocol, sizes */
            r[20] = 0;
            r[21] = 2;
            memcpy(&r[22], guest_mac, 6);
            memcpy(&r[28], &f[38], 4);
            memcpy(&r[32], &f[22], 6);
            memcpy(&r[38], &f[28], 4);
            memcpy(slirp_mac, &f[6], 6);
            send_frame(r, 42);
            continue;
        }
        if ((type != 0x0800) || (len < 34))
            continue;
        if (f[14 + 9] == 1)
            icmp_seen++;
        return ppp_encode(PPP_ACCM_ALL, PPP_PROTO_IP, &f[14], (size_t) len - 14, b, n);
    }
    return 0;
}

/* The host's side of the TCP test: a loopback server that says hello. */
static void
serve_tcp(void)
{
    SOCKET s;

    if (tcp_listener == (SOCKET) -1)
        return;
    while ((s = accept(tcp_listener, NULL, NULL)) != (SOCKET) -1) {
#ifdef _WIN32
        if (s == INVALID_SOCKET)
            break;
#endif
        send(s, "hello\n", 6, 0);
        closesocket(s);
        tcp_served++;
    }
}

static void
eth_idle(void *o)
{
    (void) o;
    tsc = isp_now_ns();
    serve_tcp();
    if (busy != NULL)
        ppp_client_send_udp(busy, HOST_IP, 9001, busy_port, (const uint8_t *) "noise", 5);
#ifdef _WIN32
    Sleep(1);
#else
    usleep(1000);
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
host_socket(int type, uint16_t *port)
{
    struct sockaddr_in sa;
    socklen_t          len = sizeof(sa);
    SOCKET             s   = socket(AF_INET, type, 0);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(0x7f000001);
    bind(s, (struct sockaddr *) &sa, sizeof(sa));
    getsockname(s, (struct sockaddr *) &sa, &len);
    *port = ntohs(sa.sin_port);
    if (type == SOCK_STREAM)
        listen(s, 64);
    set_nonblocking(s);
    return s;
}

/* --------------------------------------------------------------- the test */

static int failures;

static void
check(const char *what, int ok)
{
    if (!ok)
        failures++;
    printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    fflush(stdout);
}

static int
udp_round_trip(ppp_client_t *c, SOCKET hs, uint16_t hport, const char *msg)
{
    struct sockaddr_in from;
    socklen_t          flen = sizeof(from);
    char               buf[256];
    uint8_t            back[256];
    int                n     = -1;
    const uint32_t     until = ppp_client_ms() + 3000;

    ppp_client_send_udp(c, HOST_IP, 9000, hport, (const uint8_t *) msg, strlen(msg));
    while ((int32_t) (ppp_client_ms() - until) < 0) {
        ppp_client_poll(c);
        n = (int) recvfrom(hs, buf, sizeof(buf), 0, (struct sockaddr *) &from, &flen);
        if (n > 0)
            break;
        eth_idle(NULL);
    }
    if ((n != (int) strlen(msg)) || memcmp(buf, msg, (size_t) n))
        return 0;
    sendto(hs, "back", 4, 0, (struct sockaddr *) &from, flen);
    n = ppp_client_recv_udp(c, 9000, back, sizeof(back), NULL, NULL, 3000);
    return (n == 4) && !memcmp(back, "back", 4);
}

int
main(int argc, char **argv)
{
    const char  *internet = ((argc == 3) && !strcmp(argv[1], "--internet")) ? argv[2] : NULL;
    netcard_t   *card     = (netcard_t *) calloc(1, sizeof(netcard_t));
    char         err[NET_DRV_ERRBUF_SIZE];
    ppp_client_t c;
    SOCKET       us;
    uint16_t     uport, tport;
    int          ok;

#ifdef _WIN32
    WSADATA wsa;

    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    qlock = isp_mutex_new();
    tsc   = isp_now_ns();
    ppp_rx_init(&to_card);

    printf("== net_slirp.c behind a stub network card ==\n");
    card->card_num = 0;
    slirp_priv     = net_slirp_init(card, guest_mac, NULL, err);
    check("SLiRP starts", slirp_priv != NULL);
    if (slirp_priv == NULL)
        return 1;

    memset(&c, 0, sizeof(c));
    c.write = eth_write;
    c.read  = eth_read;
    c.idle  = eth_idle;
    ppp_client_init(&c);
    c.my_ip  = GUEST_IP;
    c.isp_ip = HOST_IP;
    c.dns    = DNS_IP;

    us = host_socket(SOCK_DGRAM, &uport);
    check("UDP to the host and back", udp_round_trip(&c, us, uport, "one"));
    check("...again", udp_round_trip(&c, us, uport, "two"));

    /* Idle: nothing should arrive at all. */
    icmp_seen = 0;
    {
        const uint32_t until = ppp_client_ms() + 1500;

        while ((int32_t) (ppp_client_ms() - until) < 0) {
            ppp_client_poll(&c);
            eth_idle(NULL);
        }
    }
    printf("    ICMP received in 1.5 s idle: %u\n", icmp_seen);
    check("no ICMP unreachables from idle UDP sockets", icmp_seen == 0);

    /* Outbound TCP, again and again. */
    tcp_listener = host_socket(SOCK_STREAM, &tport);
    ok           = 0;
    for (int i = 0; i < 40; i++) {
        uint8_t resp[64];
        int     n = ppp_client_tcp_fetch(&c, HOST_IP, tport, "", resp, sizeof(resp), 4000);

        ok += (n == 6) && !memcmp(resp, "hello\n", 6);
    }
    printf("    %d of 40 connects to the host answered (%d accepted)\n", ok, tcp_served);
    check("40 outbound TCP connections, every one answered", ok == 40);

    /* The same while the guest keeps the transmit side busy: every TX event
       is a pass in which the old driver registered every socket afresh. */
    busy      = &c;
    busy_port = uport;
    ok        = 0;
    tcp_served = 0;
    for (int i = 0; i < 40; i++) {
        uint8_t resp[64];
        int     n = ppp_client_tcp_fetch(&c, HOST_IP, tport, "", resp, sizeof(resp), 4000);

        ok += (n == 6) && !memcmp(resp, "hello\n", 6);
    }
    printf("    %d of 40 connects to the host answered with the guest busy (%d accepted)\n", ok, tcp_served);
    check("...and with the guest sending all the while", ok == 40);

    if (internet != NULL) {
        uint32_t ip = ppp_client_resolve(&c, internet, 5000);

        printf("    %s is %s\n", internet, ppp_client_ip_str(ip));
        check("DNS through SLiRP", ip != 0);
        ok = 0;
        for (int i = 0; (ip != 0) && (i < 10); i++) {
            static uint8_t page[4096];
            char           req[160];
            int            n;

            snprintf(req, sizeof(req), "HEAD / HTTP/1.0\r\nHost: %s\r\n\r\n", internet);
            busy = (i & 1) ? &c : NULL; /* half of them with the guest busy */
            n = ppp_client_tcp_fetch(&c, ip, 80, req, page, sizeof(page), 8000);
            ok += (n > 5) && !memcmp(page, "HTTP/", 5);
        }
        printf("    %d of 10 HTTP requests to %s answered\n", ok, internet);
        check("10 TCP connections to the Internet, every one answered", ok == 10);
    }

    busy = NULL;
    net_slirp_close(slirp_priv);
    check("SLiRP closes", 1);
    closesocket(us);
    closesocket(tcp_listener);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed", failures,
           (failures == 1) ? "" : "s");
    return failures ? 1 : 0;
}
