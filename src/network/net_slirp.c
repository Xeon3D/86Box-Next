/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Handle SLiRP library processing.
 *
 *          Some of the code was borrowed from libvdeslirp
 *          <https://github.com/virtualsquare/libvdeslirp>
 *
 * Authors: Fred N. van Kempen, <decwiz@yahoo.com>
 *          RichardG, <richardg867@gmail.com>
 *
 *          Copyright 2017-2019 Fred N. van Kempen.
 *          Copyright 2020 RichardG.
 */
#ifdef _WIN32
/* 86Box-Next: the Windows poll loop uses select(); a guest with a browser
   open has more sockets than Winsock's default set of 64. */
#    define FD_SETSIZE 1024
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/plat.h>
#include <86box/thread.h>
#include <86box/timer.h>
#include <86box/network.h>
#include <86box/machine.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <86box/video.h>
#include <86box/bswap.h>

#define _SSIZE_T_DEFINED
#include <slirp/libslirp.h>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#    include <ws2tcpip.h>
#else
#    include <poll.h>
#endif
#include <86box/net_event.h>

#define SLIRP_PKT_BATCH NET_QUEUE_LEN

enum {
    NET_EVENT_STOP = 0,
    NET_EVENT_TX,
    NET_EVENT_RX,
    NET_EVENT_MAX
};

typedef struct net_slirp_t {
    Slirp *        slirp;
    uint8_t        mac_addr[6];
    netcard_t *    card; /* netcard attached to us */
    thread_t *     poll_tid;
    net_evt_t      rx_event;
    net_evt_t      tx_event;
    net_evt_t      stop_event;
    netpkt_t       pkt;
    netpkt_t       pkt_tx_v[SLIRP_PKT_BATCH];
    int            during_tx;
    int            recv_on_tx;
#ifdef _WIN32
    /* 86Box-Next: a loopback UDP socket connected to itself stands in for
       the TX and stop events, so that select() can wait on it with the
       sockets; and what libslirp asked to have watched this pass. */
    SOCKET           wake;
    volatile int     stop;
    SOCKET          *poll_fd;
    int             *poll_ev;
    size_t           poll_n;
    size_t           poll_cap;
    fd_set           rd;
    fd_set           wr;
    fd_set           ex;
#else
    uint32_t       pfd_len;
    uint32_t       pfd_size;
    struct pollfd *pfd;
#endif
} net_slirp_t;

/* Pulled off from libslirp code. This is only needed for modem. */
#pragma pack(push, 1)
struct arphdr_local {
    unsigned char h_dest[6]; /* destination eth addr */
    unsigned char h_source[6]; /* source ether addr    */
    unsigned short h_proto; /* packet type ID field */

    unsigned short ar_hrd; /* format of hardware address */
    unsigned short ar_pro; /* format of protocol address */
    unsigned char ar_hln; /* length of hardware address */
    unsigned char ar_pln; /* length of protocol address */
    unsigned short ar_op; /* ARP opcode (command)       */

    /*
     *  Ethernet looks like this : This bit is variable sized however...
     */
    uint8_t ar_sha[6]; /* sender hardware address */
    uint32_t ar_sip; /* sender IP address       */
    uint8_t ar_tha[6]; /* target hardware address */
    uint32_t ar_tip; /* target IP address       */
};
#pragma pack(pop)

#ifdef ENABLE_SLIRP_LOG
int slirp_do_log = ENABLE_SLIRP_LOG;

static void
slirp_log(const char *fmt, ...)
{
    va_list ap;

    if (slirp_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define slirp_log(fmt, ...)
#endif

static void
net_slirp_guest_error(UNUSED(const char *msg), UNUSED(void *opaque))
{
    slirp_log("SLiRP: guest_error(): %s\n", msg);
}

static int64_t
net_slirp_clock_get_ns(UNUSED(void *opaque))
{
    return (int64_t) ((double) tsc / cpuclock * 1000000000.0);
}

static void *
net_slirp_timer_new(SlirpTimerCb cb, void *cb_opaque, UNUSED(void *opaque))
{
    pc_timer_t *timer = calloc(1, sizeof(pc_timer_t));
    timer_add(timer, cb, cb_opaque, 0);
    return timer;
}

static void
net_slirp_timer_free(void *timer, UNUSED(void *opaque))
{
    timer_stop(timer);
    free(timer);
}

static void
net_slirp_timer_mod(void *timer, int64_t expire_timer, UNUSED(void *opaque))
{
    timer_on_auto(timer, expire_timer * 1000);
}

static void
#if SLIRP_CHECK_VERSION(4, 9, 0)
net_slirp_register_poll_socket(slirp_os_socket fd, void *opaque)
#else
net_slirp_register_poll_fd(int fd, void *opaque)
#endif
{
    (void) fd;
    (void) opaque;
}

static void
#if SLIRP_CHECK_VERSION(4, 9, 0)
net_slirp_unregister_poll_socket(slirp_os_socket fd, void *opaque)
#else
net_slirp_unregister_poll_fd(int fd, void *opaque)
#endif
{
    (void) fd;
    (void) opaque;
}

static void
net_slirp_notify(void *opaque)
{
    (void) opaque;
}

#if SLIRP_CHECK_VERSION(4, 8, 0)
slirp_ssize_t
#else
ssize_t
#endif
net_slirp_send_packet(const void *qp, size_t pkt_len, void *opaque)
{
    net_slirp_t *slirp = (net_slirp_t *) opaque;

    slirp_log("SLiRP: received %d-byte packet\n", pkt_len);

    memcpy(slirp->pkt.data, (uint8_t *) qp, pkt_len);
    slirp->pkt.len = pkt_len;

    if (!(net_cards_conf[slirp->card->card_num].link_state & NET_LINK_DOWN)) {
        if (slirp->during_tx) {
            network_rx_on_tx_put_pkt(slirp->card, &slirp->pkt);
            slirp->recv_on_tx = 1;
        } else
            network_rx_put_pkt(slirp->card, &slirp->pkt);
    }

    return pkt_len;
}

#ifdef _WIN32
static int
#    if SLIRP_CHECK_VERSION(4, 9, 0)
net_slirp_add_poll(slirp_os_socket fd, int events, void *opaque)
#    else
net_slirp_add_poll(int fd, int events, void *opaque)
#    endif
{
    /* 86Box-Next: select(), not WSAEventSelect.  Every WSAEventSelect call
       clears what the socket has recorded, and this runs for every socket on
       every pass: an FD_CONNECT recorded between one pass's look and the next
       registration was lost, and a connect completes only once -- the guest's
       SYN went unanswered.  select() reports a state and loses nothing. */
    net_slirp_t *slirp = (net_slirp_t *) opaque;
    const SOCKET s     = (SOCKET) fd;

    if ((slirp->rd.fd_count >= FD_SETSIZE) || (slirp->wr.fd_count >= FD_SETSIZE) ||
        (slirp->ex.fd_count >= FD_SETSIZE))
        return -1; /* full: not watched this pass */
    if (slirp->poll_n >= slirp->poll_cap) {
        const size_t cap = slirp->poll_cap + 16;
        SOCKET      *fds = realloc(slirp->poll_fd, cap * sizeof(SOCKET));
        int         *evs;

        if (fds == NULL)
            return -1;
        slirp->poll_fd = fds;
        if ((evs = realloc(slirp->poll_ev, cap * sizeof(int))) == NULL)
            return -1;
        slirp->poll_ev  = evs;
        slirp->poll_cap = cap;
    }
    slirp->poll_fd[slirp->poll_n] = s;
    slirp->poll_ev[slirp->poll_n] = events;

    if (events & SLIRP_POLL_IN)
        FD_SET(s, &slirp->rd);
    /* A connect that fails shows in the exception set, one that succeeds in
       the write set. */
    if (events & SLIRP_POLL_OUT) {
        FD_SET(s, &slirp->wr);
        FD_SET(s, &slirp->ex);
    }
    if (events & SLIRP_POLL_PRI)
        FD_SET(s, &slirp->ex);
    return (int) slirp->poll_n++;
}
#else
static int
#    if SLIRP_CHECK_VERSION(4, 9, 0)
net_slirp_add_poll(slirp_os_socket fd, int events, void *opaque)
#    else
net_slirp_add_poll(int fd, int events, void *opaque)
#    endif
{
    net_slirp_t *slirp = (net_slirp_t *) opaque;

    if (slirp->pfd_len >= slirp->pfd_size) {
        int newsize = slirp->pfd_size + 16;
        struct pollfd *new = realloc(slirp->pfd, newsize * sizeof(struct pollfd));
        if (new) {
            slirp->pfd = new;
            slirp->pfd_size = newsize;
        }
    }
    if ((slirp->pfd_len < slirp->pfd_size)) {
        int idx = slirp->pfd_len++;
        slirp->pfd[idx].fd = fd;
        int pevents = 0;
        if (events & SLIRP_POLL_IN)
            pevents |= POLLIN;
        if (events & SLIRP_POLL_OUT)
            pevents |= POLLOUT;
        if (events & SLIRP_POLL_ERR)
            pevents |= POLLERR;
        if (events & SLIRP_POLL_PRI)
            pevents |= POLLPRI;
        if (events & SLIRP_POLL_HUP)
            pevents |= POLLHUP;
        slirp->pfd[idx].events = pevents;
        return idx;
    } else
        return -1;
}
#endif

#ifdef _WIN32
static int
net_slirp_get_revents(int idx, void *opaque)
{
    /* 86Box-Next: only what select() found.  This used to report "readable"
       when nothing had happened, but libslirp takes a UDP recvfrom() that
       would block for an error and answers it with an ICMP port unreachable
       to the guest: every pass, for every UDP socket, DNS included. */
    net_slirp_t *slirp = (net_slirp_t *) opaque;
    int          ret   = 0;
    SOCKET       s;
    int          ev;

    if ((idx < 0) || ((size_t) idx >= slirp->poll_n))
        return 0;
    s  = slirp->poll_fd[idx];
    ev = slirp->poll_ev[idx];
    if (FD_ISSET(s, &slirp->rd))
        ret |= SLIRP_POLL_IN; /* data, a connection to accept, or the far end closing */
    if (FD_ISSET(s, &slirp->wr))
        ret |= SLIRP_POLL_OUT;
    if (FD_ISSET(s, &slirp->ex)) {
        if (ev & SLIRP_POLL_PRI)
            ret |= SLIRP_POLL_PRI;
        if (ev & SLIRP_POLL_OUT)
            ret |= SLIRP_POLL_ERR;
    }
    return ret;
}
#else
static int
net_slirp_get_revents(int idx, void *opaque)
{
    net_slirp_t *slirp = (net_slirp_t *) opaque;
    int ret = 0;
    int events = slirp->pfd[idx].revents;
    if (events & POLLIN)
        ret |= SLIRP_POLL_IN;
    if (events & POLLOUT)
        ret |= SLIRP_POLL_OUT;
    if (events & POLLPRI)
        ret |= SLIRP_POLL_PRI;
    if (events & POLLERR)
        ret |= SLIRP_POLL_ERR;
    if (events & POLLHUP)
        ret |= SLIRP_POLL_HUP;
    return ret;
}
#endif

static const SlirpCb slirp_cb = {
    .send_packet        = net_slirp_send_packet,
    .guest_error        = net_slirp_guest_error,
    .clock_get_ns       = net_slirp_clock_get_ns,
    .timer_new          = net_slirp_timer_new,
    .timer_free         = net_slirp_timer_free,
    .timer_mod          = net_slirp_timer_mod,
#if SLIRP_CHECK_VERSION(4, 9, 0)
    .register_poll_socket   = net_slirp_register_poll_socket,
    .unregister_poll_socket = net_slirp_unregister_poll_socket,
#else
    .register_poll_fd   = net_slirp_register_poll_fd,
    .unregister_poll_fd = net_slirp_unregister_poll_fd,
#endif
    .notify             = net_slirp_notify
};

/* Send a packet to the SLiRP interface. */
static void
net_slirp_in(net_slirp_t *slirp, uint8_t *pkt, int pkt_len)
{
    if (!slirp)
        return;

    slirp_log("SLiRP: sending %d-byte packet to host network\n", pkt_len);

    slirp_input(slirp->slirp, (const uint8_t *) pkt, pkt_len);
}

void
net_slirp_in_available(void *priv)
{
    net_slirp_t *slirp = (net_slirp_t *) priv;
#ifdef _WIN32
    (void) send(slirp->wake, "t", 1, 0);
#else
    net_event_set(&slirp->tx_event);
#endif
}

static void
net_slirp_rx_deferred_packets(net_slirp_t *slirp)
{
    int packets = 0;

    if (slirp->recv_on_tx) {
        do {
            packets = network_rx_on_tx_popv(slirp->card, slirp->pkt_tx_v, SLIRP_PKT_BATCH);
            if (!(net_cards_conf[slirp->card->card_num].link_state & NET_LINK_DOWN)) {
                for (int i = 0; i < packets; i++)
                     network_rx_put_pkt(slirp->card, &(slirp->pkt_tx_v[i]));
            }
        } while (packets > 0);
        slirp->recv_on_tx = 0;
    }
}

#ifdef _WIN32
static void
net_slirp_thread(void *priv)
{
    net_slirp_t *slirp = (net_slirp_t *) priv;

    /* Start polling. */
    slirp_log("SLiRP: polling started.\n");

    while (!slirp->stop) {
        uint32_t       timeout = UINT32_MAX;
        struct timeval tv;
        int            ret;
        int            woken = 0;

        FD_ZERO(&slirp->rd);
        FD_ZERO(&slirp->wr);
        FD_ZERO(&slirp->ex);
        FD_SET(slirp->wake, &slirp->rd);
        slirp->poll_n = 0;
#    if SLIRP_CHECK_VERSION(4, 9, 0)
        slirp_pollfds_fill_socket(slirp->slirp, &timeout, net_slirp_add_poll, slirp);
#    else
        slirp_pollfds_fill(slirp->slirp, &timeout, net_slirp_add_poll, slirp);
#    endif

        tv.tv_sec  = (long) (timeout / 1000);
        tv.tv_usec = (long) ((timeout % 1000) * 1000);
        ret        = select(0, &slirp->rd, &slirp->wr, &slirp->ex, (timeout == UINT32_MAX) ? NULL : &tv);
        if (ret < 0) {
            /* Nothing is known to be ready: tell libslirp so, with empty sets. */
            FD_ZERO(&slirp->rd);
            FD_ZERO(&slirp->wr);
            FD_ZERO(&slirp->ex);
        } else if (FD_ISSET(slirp->wake, &slirp->rd)) {
            char buf[64];

            while (recv(slirp->wake, buf, sizeof(buf), 0) > 0)
                ;
            woken = 1;
        }
        if (slirp->stop)
            break;

        slirp_pollfds_poll(slirp->slirp, ret < 0, net_slirp_get_revents, slirp);

        /* Woken by the card: what it has queued for the network. */
        if (woken) {
            slirp->during_tx = 1;
            int packets = network_tx_popv(slirp->card, slirp->pkt_tx_v, SLIRP_PKT_BATCH);
            if (!(net_cards_conf[slirp->card->card_num].link_state & NET_LINK_DOWN)) {
                for (int i = 0; i < packets; i++)
                    net_slirp_in(slirp, slirp->pkt_tx_v[i].data, slirp->pkt_tx_v[i].len);
            }
            slirp->during_tx = 0;

            net_slirp_rx_deferred_packets(slirp);
        }
    }

    slirp_log("SLiRP: polling stopped.\n");
}
#else
/* Handle the receiving of frames. */
static void
net_slirp_thread(void *priv)
{
    net_slirp_t *slirp = (net_slirp_t *) priv;

    /* Start polling. */
    slirp_log("SLiRP: polling started.\n");

    while (1) {
        uint32_t timeout = -1;

        slirp->pfd_len = 0;
        net_slirp_add_poll(net_event_get_fd(&slirp->stop_event), SLIRP_POLL_IN, slirp);
        net_slirp_add_poll(net_event_get_fd(&slirp->tx_event), SLIRP_POLL_IN, slirp);

#    if SLIRP_CHECK_VERSION(4, 9, 0)
        slirp_pollfds_fill_socket(slirp->slirp, &timeout, net_slirp_add_poll, slirp);
#    else
        slirp_pollfds_fill(slirp->slirp, &timeout, net_slirp_add_poll, slirp);
#    endif

        int ret = poll(slirp->pfd, slirp->pfd_len, timeout);

        slirp_pollfds_poll(slirp->slirp, (ret < 0), net_slirp_get_revents, slirp);

        if (slirp->pfd[NET_EVENT_STOP].revents & POLLIN) {
            net_event_clear(&slirp->stop_event);
            break;
        }

        if (slirp->pfd[NET_EVENT_TX].revents & POLLIN) {
            net_event_clear(&slirp->tx_event);

            slirp->during_tx = 1;
            int packets = network_tx_popv(slirp->card, slirp->pkt_tx_v, SLIRP_PKT_BATCH);
            if (!(net_cards_conf[slirp->card->card_num].link_state & NET_LINK_DOWN)) {
                for (int i = 0; i < packets; i++)
                    net_slirp_in(slirp, slirp->pkt_tx_v[i].data, slirp->pkt_tx_v[i].len);
            }
            slirp->during_tx = 0;

            net_slirp_rx_deferred_packets(slirp);
        }
    }

    slirp_log("SLiRP: polling stopped.\n");
}
#endif

#ifdef _WIN32
/* 86Box-Next: the poll thread's wake-up.  Bound to loopback and connected to
   itself, so it hears only itself; nonblocking.  slirp_new() has started
   Winsock by the time this is called. */
static SOCKET
net_slirp_wake_socket(void)
{
    struct sockaddr_in sa;
    int                len = sizeof(sa);
    u_long             yes = 1;
    SOCKET             s   = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if ((bind(s, (struct sockaddr *) &sa, sizeof(sa)) != 0) ||
        (getsockname(s, (struct sockaddr *) &sa, &len) != 0) ||
        (connect(s, (struct sockaddr *) &sa, sizeof(sa)) != 0)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    ioctlsocket(s, FIONBIO, &yes);
    return s;
}
#endif

int slirp_card_num = 2;

/* Initialize SLiRP for use. */
void *
net_slirp_init(const netcard_t *card, const uint8_t *mac_addr, UNUSED(void *priv), char *netdrv_errbuf)
{
    slirp_log("SLiRP: initializing with range %d...\n", slirp_card_num);
    net_slirp_t *slirp = calloc(1, sizeof(net_slirp_t));
    memcpy(slirp->mac_addr, mac_addr, sizeof(slirp->mac_addr));
    slirp->card = (netcard_t *) card;

#ifndef _WIN32
    slirp->pfd_size = 16 * sizeof(struct pollfd);
    slirp->pfd      = calloc(1, slirp->pfd_size);
#endif

    struct in_addr net;
    struct in_addr host;
    struct in_addr dhcp;
    struct in_addr dns;

    /* Set the IP addresses to use.
       Use a configured address if set, otherwise 10.0.x.0 */
    const char *slirp_net = net_cards_conf[card->card_num].slirp_net;
    if (slirp_net[0] != '\0') {
        struct in_addr addr;
        inet_pton(AF_INET, slirp_net, &addr);
        net.s_addr = htonl(ntohl(addr.s_addr) & 0xffffff00);
        host.s_addr = htonl(ntohl(addr.s_addr) + 2);
        dhcp.s_addr = htonl(ntohl(addr.s_addr) + 15);
        dns.s_addr = htonl(ntohl(addr.s_addr) + 3);
    } else {
        net.s_addr = htonl(0x0a000000 | (slirp_card_num << 8));      /* 10.0.x.0 */
        host.s_addr = htonl(0x0a000002 | (slirp_card_num << 8));     /* 10.0.x.2 */
        dhcp.s_addr = htonl(0x0a00000f | (slirp_card_num << 8));     /* 10.0.x.15 */
        dns.s_addr = htonl(0x0a000003 | (slirp_card_num << 8));      /* 10.0.x.3 */
    }

    struct in_addr  mask       = { .s_addr = htonl(0xffffff00) };    /* 255.255.255.0 */
    struct in_addr  bind       = { .s_addr = htonl(0x00000000) };    /* 0.0.0.0 */

    const SlirpConfig slirp_config = {
#if SLIRP_CHECK_VERSION(4, 9, 0)
        .version = 6,
#else
        .version = 1,
#endif
        .restricted            = 0,
        .in_enabled            = 1,
        .vnetwork              = net,
        .vnetmask              = mask,
        .vhost                 = host,
        .in6_enabled           = 0,
        .vprefix_addr6         = { .s6_addr = { 0xfe, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } }, /* fec0:: - unused */
        .vprefix_len           = 64,
        .vhost6                = { .s6_addr = { 0xfe, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02 } }, /* fec0::2 - unused */
        .vhostname             = "86Box",
        .tftp_server_name      = NULL,
        .tftp_path             = NULL,
        .bootfile              = NULL,
        .vdhcp_start           = dhcp,
        .vnameserver           = dns,
        .vnameserver6          = { .s6_addr = { 0xfe, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x03 } }, /* fec0::3 - unused */
        .vdnssearch            = NULL,
        .vdomainname           = NULL,
        .if_mtu                = 0,
        .if_mru                = 0,
        .disable_host_loopback = 0,
        .enable_emu            = 0,
#if SLIRP_CHECK_VERSION(4, 9, 0)
        .outbound_addr         = NULL,
        .outbound_addr6        = NULL,
        .disable_dns           = 0,
        .disable_dhcp          = 0,
        .mfr_id                = 0,
        .oob_eth_addr          = { 0, 0, 0, 0, 0, 0 }
#endif
    };

    /* Initialize SLiRP. */
    slirp->slirp = slirp_new(&slirp_config, &slirp_cb, slirp);
    if (!slirp->slirp) {
        slirp_log("SLiRP: initialization failed\n");
        snprintf(netdrv_errbuf, NET_DRV_ERRBUF_SIZE, "SLiRP initialization failed");
        free(slirp);
        return NULL;
    }

    /* Set up port forwarding. */
    int  udp;
    int  i = 0;
    int  external;
    int  internal;
    char category[32];
    snprintf(category, sizeof(category), "SLiRP Port Forwarding #%d", card->card_num + 1);
    char key[20];
    while (1) {
        sprintf(key, "%d_protocol", i);
        udp = strcmp(config_get_string(category, key, "tcp"), "udp") == 0;
        sprintf(key, "%d_external", i);
        external = config_get_int(category, key, 0);
        sprintf(key, "%d_internal", i);
        internal = config_get_int(category, key, 0);
        if ((external <= 0) && (internal <= 0))
            break;
        else if (internal <= 0)
            internal = external;
        else if (external <= 0)
            external = internal;

        if (slirp_add_hostfwd(slirp->slirp, udp, bind, external, dhcp, internal) == 0)
            pclog("SLiRP: Forwarded %s port external:%d to internal:%d\n", udp ? "UDP" : "TCP", external, internal);
        else
            pclog("SLiRP: Failed to forward %s port external:%d to internal:%d\n", udp ? "UDP" : "TCP", external, internal);

        i++;
    }

    for (int i = 0; i < SLIRP_PKT_BATCH; i++) {
        slirp->pkt_tx_v[i].data = calloc(1, NET_MAX_FRAME);
    }
    slirp->pkt.data = calloc(1, NET_MAX_FRAME);
#ifdef _WIN32
    slirp->wake = net_slirp_wake_socket();
    if (slirp->wake == INVALID_SOCKET) {
        slirp_log("SLiRP: no loopback socket to wake the poll thread with\n");
        snprintf(netdrv_errbuf, NET_DRV_ERRBUF_SIZE, "SLiRP initialization failed (loopback socket)");
        slirp_cleanup(slirp->slirp);
        for (int i = 0; i < SLIRP_PKT_BATCH; i++)
            free(slirp->pkt_tx_v[i].data);
        free(slirp->pkt.data);
        free(slirp);
        return NULL;
    }
#else
    net_event_init(&slirp->rx_event);
    net_event_init(&slirp->tx_event);
    net_event_init(&slirp->stop_event);
#endif

    const char *nic_name = network_card_get_internal_name(net_cards_conf[net_card_current].device_num);
    if (!strcmp(nic_name, "modem") || !strcmp(nic_name, "plip")) {
        /* Send a gratuitous ARP here to make SLiRP work properly with SLIP/PLIP connections. */
        struct arphdr_local arphdr;
        /* ARP part. */
        arphdr.ar_hrd = htons(1);
        arphdr.ar_pro = htons(0x0800);
        arphdr.ar_hln = 6;
        arphdr.ar_pln = 4;
        arphdr.ar_op  = htons(1);
        memcpy(&arphdr.ar_sha, mac_addr, 6);
        memcpy(&arphdr.ar_tha, mac_addr, 6);
        arphdr.ar_sip = dhcp.s_addr;
        arphdr.ar_tip = dhcp.s_addr;

        /* Ethernet header part. */
        arphdr.h_proto = htons(0x0806);
        memset(arphdr.h_dest, 0xff, 6);
        memset(arphdr.h_source, 0x52, 6);
        AS_U32(arphdr.h_source[2]) = host.s_addr;
        slirp_input(slirp->slirp, (const uint8_t *) &arphdr, sizeof(struct arphdr_local));
    }

    slirp_log("SLiRP: creating thread...\n");
    slirp->poll_tid = thread_create(net_slirp_thread, slirp);

    slirp_card_num++;
    return slirp;
}

void
net_slirp_close(void *priv)
{
    if (!priv)
        return;

    net_slirp_t *slirp = (net_slirp_t *) priv;

    slirp_log("SLiRP: closing\n");
    /* Tell the polling thread to shut down. */
#ifdef _WIN32
    slirp->stop = 1;
    (void) send(slirp->wake, "s", 1, 0);
#else
    net_event_set(&slirp->stop_event);
#endif

    /* Wait for the thread to finish. */
    slirp_log("SLiRP: waiting for thread to end...\n");
    thread_wait(slirp->poll_tid);

#ifdef _WIN32
    closesocket(slirp->wake);
    free(slirp->poll_fd);
    free(slirp->poll_ev);
#else
    net_event_close(&slirp->stop_event);
    net_event_close(&slirp->tx_event);
    net_event_close(&slirp->rx_event);
#endif
    slirp_cleanup(slirp->slirp);
    for (int i = 0; i < SLIRP_PKT_BATCH; i++) {
        free(slirp->pkt_tx_v[i].data);
    }
    free(slirp->pkt.data);
    free(slirp);
}

const netdrv_t net_slirp_drv = {
    .notify_in = &net_slirp_in_available,
    .init      = &net_slirp_init,
    .close     = &net_slirp_close,
	.priv      = NULL
};
