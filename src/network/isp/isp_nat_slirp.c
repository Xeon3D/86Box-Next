/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The virtual ISP's libslirp adapter.  See isp_nat_slirp.h.
 *
 *             Unlike net_slirp.c, which runs libslirp's timers on the
 *             emulated clock and hands it Ethernet frames from a network
 *             card, this keeps the time of the real world -- the far end of
 *             the modem is the real Internet -- and runs the timers on the
 *             thread that polls, so that one thread makes every call.
 *
 *             And it waits with select() (poll() elsewhere), not with
 *             WSAEventSelect as net_slirp.c does.  Every WSAEventSelect call
 *             clears the socket's record of what has happened, and libslirp
 *             wants its sockets registered afresh on every pass: an
 *             FD_CONNECT recorded between one pass's look and the next
 *             registration is lost, and a connect only completes once --
 *             the guest's outbound TCP then never gets its SYN-ACK.
 *             select() reports a state, not an event, and loses nothing.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifdef _WIN32
/* A guest with a browser open has dozens of connections; Winsock's default
   select() set holds 64. */
#    define FD_SETSIZE 1024
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <winsock2.h>
#    include <windows.h>
#    include <ws2tcpip.h>
#else
#    include <arpa/inet.h>
#    include <poll.h>
#endif
#define _SSIZE_T_DEFINED
#include <slirp/libslirp.h>
#include "isp_plat.h"
#include "isp_nat_slirp.h"

#define ETH_HLEN    14
#define ETH_P_IP    0x0800
#define ETH_P_ARP   0x0806
#define ARP_LEN     28
#define FRAME_MAX   (ETH_HLEN + 65535)

typedef struct isp_nat_timer {
    struct isp_nat_timer *next;
    SlirpTimerCb          cb;
    void                 *cb_opaque;
    int                   armed;
    int64_t               expire_ms;
} isp_nat_timer_t;

struct isp_nat {
    Slirp           *slirp;
    isp_nat_config_t cfg;
    uint8_t          guest_mac[6];
    uint8_t          slirp_mac[6];
    isp_nat_timer_t *timers;
    uint8_t         *frame; /* one Ethernet frame, built on the way in */
    /* What libslirp asked to have watched this pass, by the index add_poll
       returned. */
    size_t poll_n;
    size_t poll_cap;
#ifdef _WIN32
    slirp_os_socket *poll_fd;
    int             *poll_ev;
    fd_set           rd;
    fd_set           wr;
    fd_set           ex;
#else
    struct pollfd *pfd; /* [0] is the wake-up */
#endif
};

static void
put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t) (v >> 8);
    p[1] = (uint8_t) v;
}

static void
put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

static uint32_t
get32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

/* An ARP packet from the guest's made-up MAC.  op 1 with the guest's address
   as sender and target is a gratuitous one: libslirp files the guest's MAC
   from it (arp_table_add) without answering. */
static void
nat_send_arp(isp_nat_t *nat, int op, const uint8_t *dst_mac, const uint8_t *tha, uint32_t tip)
{
    uint8_t f[ETH_HLEN + ARP_LEN];

    memcpy(&f[0], dst_mac, 6);
    memcpy(&f[6], nat->guest_mac, 6);
    put16(&f[12], ETH_P_ARP);
    put16(&f[14], 1);          /* Ethernet         */
    put16(&f[16], ETH_P_IP);   /* IPv4             */
    f[18] = 6;
    f[19] = 4;
    put16(&f[20], (uint16_t) op);
    memcpy(&f[22], nat->guest_mac, 6);
    put32(&f[28], nat->cfg.guest);
    memcpy(&f[32], tha, 6);
    put32(&f[38], tip);
    slirp_input(nat->slirp, f, sizeof(f));
}

/* ------------------------------------------------------ libslirp's calls */

static slirp_ssize_t
nat_send_packet(const void *buf, size_t len, void *opaque)
{
    isp_nat_t     *nat = (isp_nat_t *) opaque;
    const uint8_t *f   = (const uint8_t *) buf;
    uint16_t       type;

    if (len < ETH_HLEN)
        return (slirp_ssize_t) len;
    type = (uint16_t) ((f[12] << 8) | f[13]);

    if (type == ETH_P_IP) {
        /* The packet for the guest.  This only queues it: see isp.c. */
        nat->cfg.deliver(nat->cfg.opaque, f + ETH_HLEN, len - ETH_HLEN);
    } else if ((type == ETH_P_ARP) && (len >= (ETH_HLEN + ARP_LEN))) {
        const uint8_t *a = f + ETH_HLEN;

        /* libslirp asks who has the guest's address: the guest does. */
        if ((a[6] == 0) && (a[7] == 1) && (get32(&a[24]) == nat->cfg.guest))
            nat_send_arp(nat, 2, &a[8], &a[8], get32(&a[14]));
    }
    /* Anything else (there is no IPv6 here) stays on this side. */
    return (slirp_ssize_t) len;
}

static void
nat_guest_error(const char *msg, void *opaque)
{
    (void) msg;
    (void) opaque;
}

static int64_t
nat_clock_get_ns(void *opaque)
{
    (void) opaque;
    return (int64_t) isp_now_ns();
}

static void *
nat_timer_new(SlirpTimerCb cb, void *cb_opaque, void *opaque)
{
    isp_nat_t       *nat = (isp_nat_t *) opaque;
    isp_nat_timer_t *t   = (isp_nat_timer_t *) calloc(1, sizeof(isp_nat_timer_t));

    if (t == NULL)
        return NULL;
    t->cb        = cb;
    t->cb_opaque = cb_opaque;
    t->next      = nat->timers;
    nat->timers  = t;
    return t;
}

static void
nat_timer_free(void *timer, void *opaque)
{
    isp_nat_t        *nat = (isp_nat_t *) opaque;
    isp_nat_timer_t **pp  = &nat->timers;

    while (*pp != NULL) {
        if (*pp == timer) {
            *pp = ((isp_nat_timer_t *) timer)->next;
            free(timer);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void
nat_timer_mod(void *timer, int64_t expire_ms, void *opaque)
{
    isp_nat_timer_t *t = (isp_nat_timer_t *) timer;

    (void) opaque;
    t->expire_ms = expire_ms; /* in clock_get_ns()'s time base, milliseconds */
    t->armed     = 1;
}

static void
nat_register_poll_socket(slirp_os_socket fd, void *opaque)
{
    (void) fd;
    (void) opaque;
}

static void
nat_unregister_poll_socket(slirp_os_socket fd, void *opaque)
{
    (void) fd;
    (void) opaque;
}

static void
nat_notify(void *opaque)
{
    (void) opaque; /* the loop polls again anyway */
}

static const SlirpCb nat_cb = {
    .send_packet            = nat_send_packet,
    .guest_error            = nat_guest_error,
    .clock_get_ns           = nat_clock_get_ns,
    .timer_new              = nat_timer_new,
    .timer_free             = nat_timer_free,
    .timer_mod              = nat_timer_mod,
    .register_poll_socket   = nat_register_poll_socket,
    .unregister_poll_socket = nat_unregister_poll_socket,
    .notify                 = nat_notify
};

/* --------------------------------------------------------------- the API */

isp_nat_t *
isp_nat_new(const isp_nat_config_t *cfg, char *err, size_t err_len)
{
    isp_nat_t     *nat = (isp_nat_t *) calloc(1, sizeof(isp_nat_t));
    struct in_addr net;
    struct in_addr mask;
    struct in_addr host;
    struct in_addr dhcp;
    struct in_addr dns;
    uint32_t       mtu = cfg->mtu;
    static const uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t zero[6]      = { 0 };

    if ((nat == NULL) || ((nat->frame = (uint8_t *) malloc(FRAME_MAX)) == NULL)) {
        snprintf(err, err_len, "out of memory");
        free(nat);
        return NULL;
    }
    nat->cfg = *cfg;

    /* Locally administered, and one per session. */
    nat->guest_mac[0] = 0x52;
    nat->guest_mac[1] = 0x54;
    put32(&nat->guest_mac[2], cfg->guest);
    /* libslirp's own frames come from 52:55 and its address; ours go there. */
    nat->slirp_mac[0] = 0x52;
    nat->slirp_mac[1] = 0x55;
    put32(&nat->slirp_mac[2], cfg->gateway);

    net.s_addr  = htonl(cfg->net);
    mask.s_addr = htonl(0xffffff00);
    host.s_addr = htonl(cfg->gateway);
    dhcp.s_addr = htonl(cfg->guest);
    dns.s_addr  = htonl(cfg->dns);
    if (mtu > 1500)
        mtu = 1500;
    if (mtu < 576)
        mtu = 576; /* every IPv4 host takes 576 (RFC 791); below it libslirp's MSS goes silly */

    const SlirpConfig sc = {
        .version               = 6,
        .restricted            = 0,
        .in_enabled            = true,
        .vnetwork              = net,
        .vnetmask              = mask,
        .vhost                 = host,
        .in6_enabled           = false,
        .vhostname             = "86Box-Next-ISP",
        .vdhcp_start           = dhcp,
        .vnameserver           = dns,
        .if_mtu                = mtu,
        .if_mru                = 1500,
        .disable_host_loopback = false,
        .enable_emu            = false,
        .disable_dhcp          = true /* PPP gave the guest its address */
    };

    nat->slirp = slirp_new(&sc, &nat_cb, nat);
    if (nat->slirp == NULL) {
        snprintf(err, err_len, "libslirp would not start");
        free(nat->frame);
        free(nat);
        return NULL;
    }

    /* Tell it the guest's MAC now, so that its first packet for the guest is
       not held up behind an ARP request. */
    nat_send_arp(nat, 1, broadcast, zero, cfg->guest);
    return nat;
}

void
isp_nat_free(isp_nat_t *nat)
{
    if (nat == NULL)
        return;
    slirp_cleanup(nat->slirp);
    /* slirp_cleanup frees its timers through timer_free; anything left is ours. */
    while (nat->timers != NULL) {
        isp_nat_timer_t *t = nat->timers;

        nat->timers = t->next;
        free(t);
    }
#ifdef _WIN32
    free(nat->poll_fd);
    free(nat->poll_ev);
#else
    free(nat->pfd);
#endif
    free(nat->frame);
    free(nat);
}

void
isp_nat_input(isp_nat_t *nat, const uint8_t *packet, size_t len)
{
    if ((len < 20) || ((packet[0] >> 4) != 4) || (len > (FRAME_MAX - ETH_HLEN)))
        return; /* IPv4 only */
    memcpy(&nat->frame[0], nat->slirp_mac, 6);
    memcpy(&nat->frame[6], nat->guest_mac, 6);
    put16(&nat->frame[12], ETH_P_IP);
    memcpy(&nat->frame[ETH_HLEN], packet, len);
    slirp_input(nat->slirp, nat->frame, (int) (len + ETH_HLEN));
}

static void
nat_run_timers(isp_nat_t *nat, uint32_t *timeout_ms)
{
    const int64_t now = (int64_t) isp_now_ms();
    int           again;

    /* A callback may add, free or re-arm timers: start over after each. */
    do {
        again = 0;
        for (isp_nat_timer_t *t = nat->timers; t != NULL; t = t->next) {
            if (t->armed && (t->expire_ms <= now)) {
                t->armed = 0;
                t->cb(t->cb_opaque);
                again = 1;
                break;
            }
        }
    } while (again);

    if (timeout_ms != NULL) {
        for (isp_nat_timer_t *t = nat->timers; t != NULL; t = t->next) {
            if (t->armed) {
                const int64_t d = t->expire_ms - now;

                if (d < (int64_t) *timeout_ms)
                    *timeout_ms = (d < 0) ? 0 : (uint32_t) d;
            }
        }
    }
}

/* Room for one more socket in this pass's list; its index, or -1. */
static int
nat_poll_slot(isp_nat_t *nat)
{
    if (nat->poll_n >= nat->poll_cap) {
        const size_t cap = nat->poll_cap + 16;

#ifdef _WIN32
        slirp_os_socket *fds;
        int             *evs;

        if ((fds = realloc(nat->poll_fd, cap * sizeof(slirp_os_socket))) == NULL)
            return -1;
        nat->poll_fd = fds;
        if ((evs = realloc(nat->poll_ev, cap * sizeof(int))) == NULL)
            return -1;
        nat->poll_ev = evs;
#else
        struct pollfd *p;

        if ((p = realloc(nat->pfd, cap * sizeof(struct pollfd))) == NULL)
            return -1;
        nat->pfd = p;
#endif
        nat->poll_cap = cap;
    }
    return (int) nat->poll_n++;
}

#ifdef _WIN32
static int
nat_add_poll(slirp_os_socket fd, int events, void *opaque)
{
    isp_nat_t *nat = (isp_nat_t *) opaque;
    int        idx;

    if ((nat->rd.fd_count >= FD_SETSIZE) || (nat->wr.fd_count >= FD_SETSIZE) ||
        (nat->ex.fd_count >= FD_SETSIZE))
        return -1; /* full: not watched this pass */
    if ((idx = nat_poll_slot(nat)) < 0)
        return -1;
    nat->poll_fd[idx] = fd;
    nat->poll_ev[idx] = events;

    if (events & SLIRP_POLL_IN)
        FD_SET(fd, &nat->rd);
    /* A connect that fails shows in the exception set, one that succeeds in
       the write set. */
    if (events & SLIRP_POLL_OUT) {
        FD_SET(fd, &nat->wr);
        FD_SET(fd, &nat->ex);
    }
    if (events & SLIRP_POLL_PRI)
        FD_SET(fd, &nat->ex);
    return idx;
}

static int
nat_get_revents(int idx, void *opaque)
{
    isp_nat_t            *nat = (isp_nat_t *) opaque;
    slirp_os_socket       fd;
    int                   ev;
    int                   ret = 0;

    if ((idx < 0) || ((size_t) idx >= nat->poll_n))
        return 0;
    fd = nat->poll_fd[idx];
    ev = nat->poll_ev[idx];
    if (FD_ISSET(fd, &nat->rd))
        ret |= SLIRP_POLL_IN; /* data, a connection to accept, or the far end closing */
    if (FD_ISSET(fd, &nat->wr))
        ret |= SLIRP_POLL_OUT;
    if (FD_ISSET(fd, &nat->ex)) {
        if (ev & SLIRP_POLL_PRI)
            ret |= SLIRP_POLL_PRI;
        if (ev & SLIRP_POLL_OUT)
            ret |= SLIRP_POLL_ERR;
    }
    return ret;
}

void
isp_nat_run(isp_nat_t *nat, isp_wake_t *wake, uint32_t timeout_ms)
{
    const SOCKET   ws = (SOCKET) isp_wake_socket(wake);
    struct timeval tv;
    int            ret;

    FD_ZERO(&nat->rd);
    FD_ZERO(&nat->wr);
    FD_ZERO(&nat->ex);
    FD_SET(ws, &nat->rd);
    nat->poll_n = 0;

    nat_run_timers(nat, &timeout_ms);
    slirp_pollfds_fill_socket(nat->slirp, &timeout_ms, nat_add_poll, nat);

    tv.tv_sec  = (long) (timeout_ms / 1000);
    tv.tv_usec = (long) ((timeout_ms % 1000) * 1000);
    ret        = select(0, &nat->rd, &nat->wr, &nat->ex, &tv);
    if (ret < 0) {
        /* Nothing is known to be ready: tell libslirp so, with empty sets. */
        FD_ZERO(&nat->rd);
        FD_ZERO(&nat->wr);
        FD_ZERO(&nat->ex);
    } else if (FD_ISSET(ws, &nat->rd))
        isp_wake_clear(wake);

    slirp_pollfds_poll(nat->slirp, ret < 0, nat_get_revents, nat);
    nat_run_timers(nat, NULL);
}
#else
static int
nat_add_poll(slirp_os_socket fd, int events, void *opaque)
{
    isp_nat_t *nat = (isp_nat_t *) opaque;
    short      ev  = 0;
    int        idx;

    if ((idx = nat_poll_slot(nat)) < 0)
        return -1;
    if (events & SLIRP_POLL_IN)
        ev |= POLLIN;
    if (events & SLIRP_POLL_OUT)
        ev |= POLLOUT;
    if (events & SLIRP_POLL_PRI)
        ev |= POLLPRI;
    nat->pfd[idx].fd      = fd;
    nat->pfd[idx].events  = ev;
    nat->pfd[idx].revents = 0;
    return idx;
}

static int
nat_get_revents(int idx, void *opaque)
{
    isp_nat_t *nat = (isp_nat_t *) opaque;
    int        ret = 0;
    short      ev;

    if ((idx < 1) || ((size_t) idx >= nat->poll_n))
        return 0;
    ev = nat->pfd[idx].revents;
    if (ev & POLLIN)
        ret |= SLIRP_POLL_IN;
    if (ev & POLLOUT)
        ret |= SLIRP_POLL_OUT;
    if (ev & POLLPRI)
        ret |= SLIRP_POLL_PRI;
    if (ev & POLLERR)
        ret |= SLIRP_POLL_ERR;
    if (ev & POLLHUP)
        ret |= SLIRP_POLL_HUP;
    return ret;
}

void
isp_nat_run(isp_nat_t *nat, isp_wake_t *wake, uint32_t timeout_ms)
{
    int ret;

    nat->poll_n = 0;
    if (nat_poll_slot(nat) != 0)
        return;
    nat->pfd[0].fd      = isp_wake_fd(wake);
    nat->pfd[0].events  = POLLIN;
    nat->pfd[0].revents = 0;

    nat_run_timers(nat, &timeout_ms);
    slirp_pollfds_fill_socket(nat->slirp, &timeout_ms, nat_add_poll, nat);

    ret = poll(nat->pfd, (nfds_t) nat->poll_n, (int) timeout_ms);
    if ((ret > 0) && (nat->pfd[0].revents & POLLIN))
        isp_wake_clear(wake);

    slirp_pollfds_poll(nat->slirp, ret < 0, nat_get_revents, nat);
    nat_run_timers(nat, NULL);
}
#endif
