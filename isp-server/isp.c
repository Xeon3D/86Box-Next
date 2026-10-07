/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The virtual ISP's sessions.  See isp.h.
 *
 *             A session owns, for the length of one call: an address block,
 *             a PPP link, a libslirp instance (while IPCP is open), a thread
 *             to run them, and the queues the frontend talks through.  Its
 *             thread is the only one that touches the PPP link or libslirp;
 *             every other thread -- the frontend's, the status page's,
 *             another session's -- only takes the session's lock long enough
 *             to copy bytes in or a status snapshot out, and asks for
 *             anything else (hang up, new port forwards) by setting a flag
 *             and waking the thread.
 *
 *             Frames for the guest are queued whole or not at all, so a full
 *             queue costs a packet that TCP or the guest will send again,
 *             never a torn frame.
 *
 *             Guest LAN: a packet from one guest to another guest's address
 *             never reaches libslirp; it goes into that session's queue of
 *             packets from its neighbours, as on one real ISP, where users
 *             could reach each other.
 *
 *             Locks are taken in one order: the global lock, then a
 *             session's.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "isp.h"
#include "isp_plat.h"
#include "ppp_framing.h"
#include "ppp_session.h"
#include "isp_nat_slirp.h"

#define ISP_IN_SIZE    (64 * 1024)  /* from the guest: a few seconds of a modem */
#define ISP_OUT_SIZE   (256 * 1024) /* to the guest: room for its TCP windows   */
#define ISP_LAN_SIZE   (64 * 1024)  /* packets from the other guests            */
#define ISP_CHUNK      4096
#define ISP_MAX_WAIT   250          /* ms; the loop looks around at least this often */
#define ISP_GUEST_HOST 15           /* the guest is .15 of its /24 */

typedef struct {
    uint8_t *buf;
    size_t   size;
    size_t   head;
    size_t   tail;
} ring_t;

/* A token bucket, in thousandths of a byte. */
typedef struct {
    uint64_t tokens;
    uint64_t at_ms;
} bucket_t;

struct isp_session {
    int                     number;
    uint32_t                net;
    uint32_t                gateway;
    uint32_t                dns;
    uint32_t                guest;
    isp_session_callbacks_t cb;
    isp_settings_t          settings; /* as they were when the call came in */
    uint64_t                t0;

    /* Shared, under `lock`. */
    isp_mutex_t  *lock;
    ring_t        in;
    ring_t        out;
    ring_t        lan; /* a 2-byte length, then the packet */
    int           stop;
    int           ended;
    int           hangup_req;
    char          label[64];
    uint32_t      rate;      /* 0: the settings' default */
    int           throttle;  /* live copies of the settings */
    int           guest_lan;
    uint32_t      default_rate;
    bucket_t      up;        /* from the guest */
    bucket_t      down;      /* to the guest   */
    uint64_t      bytes_in;
    uint64_t      bytes_out;
    isp_forward_t fwd[ISP_MAX_FORWARDS];
    int           n_fwd;
    int           fwd_dirty;
    /* The thread's last word on itself, for isp_list_calls(). */
    int           st_state;
    char          st_user[64];
    uint32_t      st_bad;
    uint32_t      st_dropped;
    int           st_fwd_state[ISP_MAX_FORWARDS];

    isp_wake_t   *wake;
    isp_thread_t *thread;

    /* The session's thread's alone. */
    ppp_rx_t      rx;
    ppp_t         ppp;
    isp_nat_t    *nat;
    uint8_t       enc[PPP_ENCODED_MAX(PPP_MAX_FRAME)];
    uint8_t       work[0x10000];
    uint32_t      out_drops;
    int           hung_up;
    isp_forward_t applied[ISP_MAX_FORWARDS];
    int           applied_state[ISP_MAX_FORWARDS];
    int           n_applied;
};

static isp_settings_t isp_settings = {
    .base_net     = 0x0a560000, /* 10.86.0.0 */
    .max_sessions = ISP_MAX_SESSIONS,
    .require_pap  = 0,
    .echo_secs    = 0,
    .guest_lan    = 1,
    .throttle     = 0,
    .default_rate = 57600
};
/* Who has which number: reserved when a call opens, the pointer published
   once the session is whole, both cleared when it closes. */
static uint8_t        isp_used[ISP_MAX_SESSIONS + 1];
static isp_session_t *isp_sessions[ISP_MAX_SESSIONS + 1];

/* ------------------------------------------------------------------ rings */

static int
ring_init(ring_t *r, size_t size)
{
    r->buf  = (uint8_t *) malloc(size);
    r->size = size;
    r->head = r->tail = 0;
    return r->buf != NULL;
}

static size_t
ring_used(const ring_t *r)
{
    return (r->head + r->size - r->tail) % r->size;
}

static size_t
ring_free(const ring_t *r)
{
    return r->size - 1 - ring_used(r);
}

static size_t
ring_put(ring_t *r, const uint8_t *data, size_t len)
{
    size_t n = ring_free(r);

    if (len < n)
        n = len;
    for (size_t done = 0; done < n;) {
        size_t c = r->size - r->head;

        if (c > (n - done))
            c = n - done;
        memcpy(&r->buf[r->head], &data[done], c);
        r->head = (r->head + c) % r->size;
        done += c;
    }
    return n;
}

static size_t
ring_get(ring_t *r, uint8_t *data, size_t len)
{
    size_t n = ring_used(r);

    if (len < n)
        n = len;
    for (size_t done = 0; done < n;) {
        size_t c = r->size - r->tail;

        if (c > (n - done))
            c = n - done;
        memcpy(&data[done], &r->buf[r->tail], c);
        r->tail = (r->tail + c) % r->size;
        done += c;
    }
    return n;
}

/* --------------------------------------------------------------- logging */

static void
slog(isp_session_t *s, const char *fmt, ...)
{
    char    buf[320];
    int     n;
    va_list ap;

    if (s->cb.log == NULL)
        return;
    n = snprintf(buf, sizeof(buf), "ISP session %d: ", s->number);
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - (size_t) n, fmt, ap);
    va_end(ap);
    s->cb.log(s->cb.opaque, buf);
}

static void
fmt_ip(char *buf, size_t len, uint32_t ip)
{
    snprintf(buf, len, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
}

static uint32_t
now32(const isp_session_t *s)
{
    return (uint32_t) (isp_now_ms() - s->t0);
}

/* -------------------------------------------------------------- throttle */

/* How much of `want` the bucket allows now, at `bps` bit/s: ten bits a byte,
   as the modem's serial side counts them, and a tenth of a second's burst. */
static size_t
bucket_take(bucket_t *b, uint32_t bps, size_t want)
{
    const uint64_t now = isp_now_ms();
    const uint64_t per = bps / 10; /* thousandths of a byte per ms = bytes per s */
    uint64_t       cap = per * 100;
    size_t         avail;

    if (cap < 64000)
        cap = 64000;
    if (b->at_ms == 0)
        b->tokens = cap;
    else
        b->tokens += (now - b->at_ms) * per;
    b->at_ms = now;
    if (b->tokens > cap)
        b->tokens = cap;
    avail = (size_t) (b->tokens / 1000);
    if (want > avail)
        want = avail;
    b->tokens -= (uint64_t) want * 1000;
    return want;
}

static uint32_t
effective_rate(const isp_session_t *s)
{
    return s->rate ? s->rate : s->default_rate;
}

/* ------------------------------------------------------------- guest LAN */

/* Is `ip` a guest's address under this call's addressing?  Its session
   number if so, else 0. */
static int
guest_number(const isp_session_t *s, uint32_t ip)
{
    const int n = (int) ((ip >> 8) & 0xff);

    if (((ip & 0xffff0000u) != s->settings.base_net) || ((ip & 0xff) != ISP_GUEST_HOST) ||
        (n < 1) || (n > ISP_MAX_SESSIONS))
        return 0;
    return n;
}

/* A packet for another guest: into its session's neighbour queue.  Dropped
   when there is no such call, as by a router with no route. */
static void
lan_route(int number, const uint8_t *packet, size_t len)
{
    isp_session_t *d;
    int            wake = 0;

    if (len > 0xffff)
        return;
    isp_global_lock();
    d = isp_sessions[number];
    if (d != NULL) {
        const uint8_t hdr[2] = { (uint8_t) (len >> 8), (uint8_t) len };

        isp_mutex_lock(d->lock);
        if (ring_free(&d->lan) >= (len + 2)) {
            wake = (ring_used(&d->lan) == 0);
            ring_put(&d->lan, hdr, 2);
            ring_put(&d->lan, packet, len);
        }
        isp_mutex_unlock(d->lock);
        if (wake)
            isp_wake_set(d->wake);
    }
    isp_global_unlock();
}

/* ---------------------------------------------------------- port forwards */

static void
forwards_unapply(isp_session_t *s)
{
    for (int i = 0; i < s->n_applied; i++) {
        if ((s->nat != NULL) && (s->applied_state[i] == ISP_FORWARD_BOUND))
            isp_nat_remove_forward(s->nat, &s->applied[i]);
        s->applied_state[i] = ISP_FORWARD_PENDING;
    }
}

/* Binds the forwards in `applied`; the NAT is up. */
static void
forwards_apply(isp_session_t *s)
{
    for (int i = 0; i < s->n_applied; i++) {
        const isp_forward_t *f = &s->applied[i];

        if (isp_nat_add_forward(s->nat, f) == 0)
            s->applied_state[i] = ISP_FORWARD_BOUND;
        else {
            s->applied_state[i] = ISP_FORWARD_FAILED;
            slog(s, "port forward %s %s:%u -> guest port %u: the host port is taken",
                 f->udp ? "UDP" : "TCP", f->all_interfaces ? "*" : "127.0.0.1", f->host_port, f->guest_port);
        }
    }
}

/* ------------------------------------------------------ PPP's callbacks */

static void
isp_send(void *opaque, uint16_t protocol, const uint8_t *info, size_t len, uint32_t accm)
{
    isp_session_t *s = (isp_session_t *) opaque;
    const size_t   n = ppp_encode(accm, protocol, info, len, s->enc, sizeof(s->enc));
    int            was_empty;
    int            queued = 0;

    if (n == 0)
        return;
    isp_mutex_lock(s->lock);
    was_empty = (ring_used(&s->out) == 0);
    if (ring_free(&s->out) >= n) {
        ring_put(&s->out, s->enc, n);
        queued = 1;
    }
    isp_mutex_unlock(s->lock);

    if (!queued) {
        if (s->out_drops++ == 0)
            slog(s, "output queue full: dropping frames (the guest is not reading)");
        return;
    }
    if (was_empty && (s->cb.notify != NULL))
        s->cb.notify(s->cb.opaque);
}

static void
isp_deliver(void *opaque, const uint8_t *packet, size_t len)
{
    isp_session_t *s = (isp_session_t *) opaque;

    ppp_send_ip(&s->ppp, packet, len);
}

static void
isp_ip_up(void *opaque)
{
    isp_session_t   *s = (isp_session_t *) opaque;
    isp_nat_config_t nc;
    char             err[128];

    if (s->nat != NULL)
        return;
    memset(&nc, 0, sizeof(nc));
    nc.net     = s->net;
    nc.gateway = s->gateway;
    nc.dns     = s->dns;
    nc.guest   = s->guest;
    nc.mtu     = s->ppp.peer_mru;
    nc.deliver = isp_deliver;
    nc.opaque  = s;
    s->nat     = isp_nat_new(&nc, err, sizeof(err));
    if (s->nat == NULL) {
        slog(s, "NAT failed to start: %s", err);
        ppp_close(&s->ppp, "the ISP's NAT failed to start", now32(s));
        return;
    }
    forwards_apply(s);
}

static void
isp_ip_down(void *opaque)
{
    isp_session_t *s = (isp_session_t *) opaque;

    forwards_unapply(s);
    isp_nat_free(s->nat);
    s->nat = NULL;
}

static void
isp_ip_input(void *opaque, const uint8_t *packet, size_t len)
{
    isp_session_t *s = (isp_session_t *) opaque;
    int            lan;

    isp_mutex_lock(s->lock);
    lan = s->guest_lan;
    isp_mutex_unlock(s->lock);

    if (lan && (len >= 20)) {
        const uint32_t dst = ((uint32_t) packet[16] << 24) | ((uint32_t) packet[17] << 16) |
                             ((uint32_t) packet[18] << 8) | packet[19];
        const int      n   = guest_number(s, dst);

        if (n != 0) {
            lan_route(n, packet, len);
            return;
        }
    }
    if (s->nat != NULL)
        isp_nat_input(s->nat, packet, len);
}

static void
isp_finished(void *opaque, const char *why)
{
    isp_session_t *s = (isp_session_t *) opaque;

    slog(s, "PPP finished: %s", why);
    isp_mutex_lock(s->lock);
    s->ended = 1;
    isp_mutex_unlock(s->lock);
    if (s->cb.notify != NULL)
        s->cb.notify(s->cb.opaque);
}

static void
isp_ppp_log(void *opaque, const char *msg)
{
    slog((isp_session_t *) opaque, "%s", msg);
}

static const ppp_callbacks_t isp_ppp_cb = {
    .send     = isp_send,
    .ip_up    = isp_ip_up,
    .ip_down  = isp_ip_down,
    .ip_input = isp_ip_input,
    .finished = isp_finished,
    .log      = isp_ppp_log
};

static void
isp_frame(void *opaque, const uint8_t *frame, size_t len)
{
    isp_session_t *s = (isp_session_t *) opaque;

    ppp_input(&s->ppp, frame, len, now32(s));
    /* What the guest's line may insert changes when LCP opens or closes. */
    s->rx.accm = s->ppp.rx_accm;
}

/* -------------------------------------------------------------- the loop */

static int
call_state(const isp_session_t *s, int ended)
{
    const ppp_t *p = &s->ppp;

    if (ended)
        return ISP_CALL_ENDED;
    if ((p->phase == PPP_PHASE_TERMINATE) || (p->lcp.state == FSM_CLOSING) || (p->lcp.state == FSM_STOPPING))
        return ISP_CALL_CLOSING;
    if (p->ipcp.state == FSM_OPENED)
        return ISP_CALL_ONLINE;
    if (p->phase == PPP_PHASE_AUTHENTICATE)
        return ISP_CALL_AUTHENTICATING;
    if (!p->was_open && (p->lcp.state == FSM_STOPPED))
        return ISP_CALL_WAITING;
    return ISP_CALL_NEGOTIATING;
}

/* The operator hangs up: politely if PPP is under way, at once if not. */
static void
isp_do_hangup(isp_session_t *s)
{
    const int st = s->ppp.lcp.state;

    if (s->hung_up)
        return;
    s->hung_up = 1;
    slog(s, "hung up by the ISP");
    if ((st == FSM_REQSENT) || (st == FSM_ACKRCVD) || (st == FSM_ACKSENT) || (st == FSM_OPENED))
        ppp_close(&s->ppp, "disconnected by the ISP", now32(s));
    else if ((st != FSM_CLOSING) && (st != FSM_STOPPING))
        isp_finished(s, "disconnected by the ISP");
}

static void
isp_worker(void *arg)
{
    isp_session_t *s = (isp_session_t *) arg;

    ppp_lower_up(&s->ppp, now32(s));

    for (;;) {
        uint32_t timeout;
        int      stop;
        int      hangup;
        int      ended;
        size_t   n;

        /* Everything the guest has sent. */
        do {
            isp_mutex_lock(s->lock);
            stop = s->stop;
            n    = ring_get(&s->in, s->work, ISP_CHUNK);
            isp_mutex_unlock(s->lock);
            if (n > 0)
                ppp_rx_feed(&s->rx, s->work, n, isp_frame, s);
        } while ((n == ISP_CHUNK) && !stop);
        if (stop)
            break;

        /* Packets from the other guests, one at a time. */
        for (;;) {
            uint8_t hdr[2];
            size_t  len = 0;

            isp_mutex_lock(s->lock);
            if (ring_used(&s->lan) >= 2) {
                ring_get(&s->lan, hdr, 2);
                len = ((size_t) hdr[0] << 8) | hdr[1];
                ring_get(&s->lan, s->work, len);
            }
            isp_mutex_unlock(s->lock);
            if (len == 0)
                break;
            ppp_send_ip(&s->ppp, s->work, len);
        }

        /* What the status page has asked for. */
        isp_mutex_lock(s->lock);
        hangup        = s->hangup_req;
        s->hangup_req = 0;
        if (s->fwd_dirty) {
            s->fwd_dirty = 0;
            forwards_unapply(s);
            memcpy(s->applied, s->fwd, sizeof(s->applied));
            s->n_applied = s->n_fwd;
            for (int i = 0; i < s->n_applied; i++)
                s->applied_state[i] = ISP_FORWARD_PENDING;
            isp_mutex_unlock(s->lock);
            if (s->nat != NULL)
                forwards_apply(s);
        } else
            isp_mutex_unlock(s->lock);
        if (hangup)
            isp_do_hangup(s);

        ppp_tick(&s->ppp, now32(s));

        /* And what it will see next. */
        isp_mutex_lock(s->lock);
        ended       = s->ended;
        s->st_state = call_state(s, ended);
        snprintf(s->st_user, sizeof(s->st_user), "%s", s->ppp.peer_user);
        s->st_bad     = s->rx.bad_fcs + s->rx.too_long + s->rx.runts + s->rx.aborts;
        s->st_dropped = s->out_drops + s->ppp.ip_dropped;
        for (int i = 0; i < ISP_MAX_FORWARDS; i++)
            s->st_fwd_state[i] = (i < s->n_applied) ? s->applied_state[i] : ISP_FORWARD_PENDING;
        isp_mutex_unlock(s->lock);

        timeout = ppp_next_timeout(&s->ppp, now32(s));
        if (timeout > ISP_MAX_WAIT)
            timeout = ISP_MAX_WAIT;

        /* Until the guest or a neighbour sends something, libslirp has
           something, the status page asks, or a timer is due. */
        if (s->nat != NULL)
            isp_nat_run(s->nat, s->wake, timeout);
        else
            isp_wake_wait(s->wake, timeout);
    }

    forwards_unapply(s);
    isp_nat_free(s->nat);
    s->nat = NULL;
}

/* --------------------------------------------------------------- the API */

void
isp_get_settings(isp_settings_t *st)
{
    isp_global_lock();
    *st = isp_settings;
    isp_global_unlock();
}

void
isp_set_settings(const isp_settings_t *st)
{
    isp_global_lock();
    isp_settings = *st;
    if ((isp_settings.max_sessions < 1) || (isp_settings.max_sessions > ISP_MAX_SESSIONS))
        isp_settings.max_sessions = ISP_MAX_SESSIONS;
    isp_settings.base_net &= 0xffff0000u;
    if (isp_settings.default_rate < 300)
        isp_settings.default_rate = 300;
    /* Guest LAN and throttle take effect on the calls already up. */
    for (int i = 1; i <= ISP_MAX_SESSIONS; i++) {
        isp_session_t *s = isp_sessions[i];

        if (s == NULL)
            continue;
        isp_mutex_lock(s->lock);
        s->guest_lan    = isp_settings.guest_lan;
        s->throttle     = isp_settings.throttle;
        s->default_rate = isp_settings.default_rate;
        isp_mutex_unlock(s->lock);
    }
    isp_global_unlock();
}

static int
isp_alloc_number(isp_settings_t *st)
{
    int number = 0;

    isp_global_lock();
    *st = isp_settings;
    for (int i = 1; i <= st->max_sessions; i++) {
        if (!isp_used[i]) {
            isp_used[i] = 1;
            number      = i;
            break;
        }
    }
    isp_global_unlock();
    return number;
}

static void
isp_session_free(isp_session_t *s)
{
    /* Out of the registry first: after this no other thread can find it. */
    isp_global_lock();
    if ((s->number >= 1) && (s->number <= ISP_MAX_SESSIONS)) {
        if (isp_sessions[s->number] == s)
            isp_sessions[s->number] = NULL;
        isp_used[s->number] = 0;
    }
    isp_global_unlock();

    isp_wake_free(s->wake);
    isp_mutex_free(s->lock);
    free(s->in.buf);
    free(s->out.buf);
    free(s->lan.buf);
    free(s);
}

isp_session_t *
isp_session_open(const isp_session_callbacks_t *cb, char *err, size_t err_len)
{
    isp_session_t *s = (isp_session_t *) calloc(1, sizeof(isp_session_t));
    ppp_config_t   pc;
    char           guest[16];
    char           dns[16];

    if (s == NULL) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    if (cb != NULL)
        s->cb = *cb;

    s->number = isp_alloc_number(&s->settings);
    if (s->number == 0) {
        snprintf(err, err_len, "all %d ISP sessions are in use", s->settings.max_sessions);
        free(s);
        return NULL;
    }
    s->net          = s->settings.base_net + ((uint32_t) s->number << 8);
    s->gateway      = s->net + 2;
    s->dns          = s->net + 3;
    s->guest        = s->net + ISP_GUEST_HOST;
    s->guest_lan    = s->settings.guest_lan;
    s->throttle     = s->settings.throttle;
    s->default_rate = s->settings.default_rate;
    snprintf(s->label, sizeof(s->label), "call %d", s->number);

    s->lock = isp_mutex_new();
    s->wake = isp_wake_new();
    if ((s->lock == NULL) || (s->wake == NULL) || !ring_init(&s->in, ISP_IN_SIZE) ||
        !ring_init(&s->out, ISP_OUT_SIZE) || !ring_init(&s->lan, ISP_LAN_SIZE)) {
        snprintf(err, err_len, "out of memory, or no loopback socket to wake the session with");
        isp_session_free(s);
        return NULL;
    }

    memset(&pc, 0, sizeof(pc));
    pc.local_ip         = s->gateway;
    pc.peer_ip          = s->guest;
    pc.dns[0]           = s->dns;
    pc.dns[1]           = s->dns; /* libslirp has the one relay */
    pc.require_pap      = s->settings.require_pap;
    pc.echo_interval_ms = s->settings.echo_secs * 1000u;
    pc.echo_fails       = 4;
    pc.magic_seed       = isp_random();
    s->t0               = isp_now_ms();
    ppp_rx_init(&s->rx);
    ppp_init(&s->ppp, &pc, &isp_ppp_cb, s, 0);

    fmt_ip(guest, sizeof(guest), s->guest);
    fmt_ip(dns, sizeof(dns), s->dns);
    slog(s, "open; guest %s, DNS %s; waiting for the guest's LCP", guest, dns);

    s->thread = isp_thread_start(isp_worker, s);
    if (s->thread == NULL) {
        snprintf(err, err_len, "the session's thread would not start");
        isp_session_free(s);
        return NULL;
    }

    isp_global_lock();
    isp_sessions[s->number] = s;
    isp_global_unlock();
    return s;
}

void
isp_session_close(isp_session_t *s)
{
    if (s == NULL)
        return;

    isp_mutex_lock(s->lock);
    s->stop = 1;
    isp_mutex_unlock(s->lock);
    isp_wake_set(s->wake);
    isp_thread_join(s->thread);

    slog(s, "closed; %u good frames, %u bad FCS, %u too long, %u runts, %u aborted; %u frames to the guest dropped",
         s->rx.frames, s->rx.bad_fcs, s->rx.too_long, s->rx.runts, s->rx.aborts, s->out_drops);
    isp_session_free(s);
}

size_t
isp_session_write(isp_session_t *s, const uint8_t *buf, size_t len)
{
    size_t n;
    int    was_empty;

    if (len == 0)
        return 0;
    isp_mutex_lock(s->lock);
    if (s->throttle) {
        uint32_t up = effective_rate(s);

        if (up > 33600)
            up = 33600; /* V.90 sends at V.34 speed */
        len = bucket_take(&s->up, up, len);
    }
    was_empty = (ring_used(&s->in) == 0);
    n         = ring_put(&s->in, buf, len);
    s->bytes_in += n;
    isp_mutex_unlock(s->lock);

    /* The thread empties the queue whenever it wakes, so only the first byte
       into an empty queue needs to wake it. */
    if (was_empty && (n > 0))
        isp_wake_set(s->wake);
    return n;
}

size_t
isp_session_read(isp_session_t *s, uint8_t *buf, size_t len)
{
    size_t n;

    isp_mutex_lock(s->lock);
    if (s->throttle) {
        const size_t used = ring_used(&s->out);

        len = bucket_take(&s->down, effective_rate(s), (len < used) ? len : used);
    }
    n = ring_get(&s->out, buf, len);
    s->bytes_out += n;
    isp_mutex_unlock(s->lock);
    return n;
}

int
isp_session_ended(isp_session_t *s)
{
    int ret;

    isp_mutex_lock(s->lock);
    ret = s->ended && (ring_used(&s->out) == 0);
    isp_mutex_unlock(s->lock);
    return ret;
}

int
isp_session_number(const isp_session_t *s)
{
    return s->number;
}

uint32_t
isp_session_guest_ip(const isp_session_t *s)
{
    return s->guest;
}

void
isp_session_set_label(isp_session_t *s, const char *label)
{
    isp_mutex_lock(s->lock);
    snprintf(s->label, sizeof(s->label), "%s", label);
    isp_mutex_unlock(s->lock);
}

void
isp_session_set_rate(isp_session_t *s, uint32_t bps)
{
    isp_mutex_lock(s->lock);
    s->rate = bps;
    isp_mutex_unlock(s->lock);
}

void
isp_session_set_forwards(isp_session_t *s, const isp_forward_t *fwd, int n)
{
    if (n < 0)
        n = 0;
    if (n > ISP_MAX_FORWARDS)
        n = ISP_MAX_FORWARDS;
    isp_mutex_lock(s->lock);
    memset(s->fwd, 0, sizeof(s->fwd));
    if (n > 0)
        memcpy(s->fwd, fwd, (size_t) n * sizeof(isp_forward_t));
    s->n_fwd     = n;
    s->fwd_dirty = 1;
    isp_mutex_unlock(s->lock);
    isp_wake_set(s->wake);
}

/* ------------------------------------------------------ the status page */

int
isp_list_calls(isp_call_info_t *out, int max)
{
    const uint64_t now = isp_now_ms();
    int            n   = 0;

    isp_global_lock();
    for (int i = 1; (i <= ISP_MAX_SESSIONS) && (n < max); i++) {
        isp_session_t   *s = isp_sessions[i];
        isp_call_info_t *c = &out[n];

        if (s == NULL)
            continue;
        memset(c, 0, sizeof(*c));
        isp_mutex_lock(s->lock);
        c->number           = s->number;
        c->guest_ip         = s->guest;
        c->dns_ip           = s->dns;
        c->state            = s->st_state;
        c->seconds          = (uint32_t) ((now - s->t0) / 1000);
        c->bytes_from_guest = s->bytes_in;
        c->bytes_to_guest   = s->bytes_out;
        c->bad_frames       = s->st_bad;
        c->dropped          = s->st_dropped;
        c->rate             = s->throttle ? effective_rate(s) : 0;
        c->n_forwards       = s->n_fwd;
        snprintf(c->label, sizeof(c->label), "%s", s->label);
        snprintf(c->user, sizeof(c->user), "%s", s->st_user);
        memcpy(c->forwards, s->fwd, sizeof(c->forwards));
        /* A forward the thread has not taken up yet is still pending. */
        for (int f = 0; f < s->n_fwd; f++)
            c->forward_state[f] = s->fwd_dirty ? ISP_FORWARD_PENDING : s->st_fwd_state[f];
        isp_mutex_unlock(s->lock);
        n++;
    }
    isp_global_unlock();
    return n;
}

int
isp_hangup_call(int number)
{
    isp_session_t *s;
    int            found = 0;

    if ((number < 1) || (number > ISP_MAX_SESSIONS))
        return 0;
    isp_global_lock();
    s = isp_sessions[number];
    if (s != NULL) {
        isp_mutex_lock(s->lock);
        s->hangup_req = 1;
        isp_mutex_unlock(s->lock);
        isp_wake_set(s->wake);
        found = 1;
    }
    isp_global_unlock();
    return found;
}

int
isp_set_call_forwards(int number, const isp_forward_t *fwd, int n)
{
    isp_session_t *s;
    int            found = 0;

    if ((number < 1) || (number > ISP_MAX_SESSIONS))
        return 0;
    isp_global_lock();
    s = isp_sessions[number];
    if (s != NULL) {
        isp_session_set_forwards(s, fwd, n);
        found = 1;
    }
    isp_global_unlock();
    return found;
}

const char *
isp_call_state_name(int state)
{
    switch (state) {
        case ISP_CALL_WAITING:
            return "waiting for PPP";
        case ISP_CALL_NEGOTIATING:
            return "negotiating";
        case ISP_CALL_AUTHENTICATING:
            return "authenticating";
        case ISP_CALL_ONLINE:
            return "on line";
        case ISP_CALL_CLOSING:
            return "hanging up";
        case ISP_CALL_ENDED:
            return "ended";
        default:
            return "?";
    }
}

/* "tcp:2121:21", "udp:*:5000:5000" (* = every interface), separated by
   spaces or commas. */
int
isp_forwards_parse(const char *s, isp_forward_t *fwd, int max)
{
    int n = 0;

    while ((s != NULL) && (*s != '\0') && (n < max)) {
        char          item[64];
        size_t        len = strcspn(s, " ,;\t");
        isp_forward_t f;
        unsigned      hp;
        unsigned      gp;
        const char   *p;

        if (len == 0) {
            s++;
            continue;
        }
        if (len >= sizeof(item))
            len = sizeof(item) - 1;
        memcpy(item, s, len);
        item[len] = '\0';
        s += len;

        memset(&f, 0, sizeof(f));
        if (!strncmp(item, "udp:", 4))
            f.udp = 1;
        else if (strncmp(item, "tcp:", 4))
            continue;
        p = item + 4;
        if (!strncmp(p, "*:", 2)) {
            f.all_interfaces = 1;
            p += 2;
        }
        if ((sscanf(p, "%u:%u", &hp, &gp) != 2) || (hp < 1) || (hp > 65535) || (gp < 1) || (gp > 65535))
            continue;
        f.host_port  = (uint16_t) hp;
        f.guest_port = (uint16_t) gp;
        fwd[n++]     = f;
    }
    return n;
}

void
isp_forwards_format(const isp_forward_t *fwd, int n, char *buf, size_t len)
{
    size_t used = 0;

    if (len == 0)
        return;
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        const int w = snprintf(buf + used, len - used, "%s%s:%s%u:%u", i ? " " : "", fwd[i].udp ? "udp" : "tcp",
                               fwd[i].all_interfaces ? "*:" : "", fwd[i].host_port, fwd[i].guest_port);

        if ((w < 0) || ((size_t) w >= (len - used)))
            break;
        used += (size_t) w;
    }
}
