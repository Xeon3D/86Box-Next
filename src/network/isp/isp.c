/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The virtual ISP's sessions.  See 86box/isp.h.
 *
 *             A session owns, for the length of one call: an address block,
 *             a PPP link, a libslirp instance (while IPCP is open), a thread
 *             to run them, and the two queues the frontend talks through.
 *             Its thread is the only one that touches the PPP link or
 *             libslirp; the frontend's thread only takes the lock long
 *             enough to copy bytes.
 *
 *             Frames for the guest are queued whole or not at all, so a full
 *             queue costs a packet that TCP or the guest will send again,
 *             never a torn frame.
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
#include <86box/isp.h>
#include "isp_plat.h"
#include "ppp_framing.h"
#include "ppp_session.h"
#include "isp_nat_slirp.h"

#define ISP_IN_SIZE   (64 * 1024)  /* from the guest: a few seconds of a modem */
#define ISP_OUT_SIZE  (256 * 1024) /* to the guest: room for its TCP windows   */
#define ISP_CHUNK     4096
#define ISP_MAX_WAIT  250          /* ms; the loop looks around at least this often */

typedef struct {
    uint8_t *buf;
    size_t   size;
    size_t   head;
    size_t   tail;
} ring_t;

struct isp_session {
    int                     number;
    uint32_t                net;
    uint32_t                gateway;
    uint32_t                dns;
    uint32_t                guest;
    isp_session_callbacks_t cb;
    isp_settings_t          settings;

    /* Shared with the frontend, under `lock`. */
    isp_mutex_t *lock;
    ring_t       in;
    ring_t       out;
    int          stop;
    int          ended;

    isp_wake_t   *wake;
    isp_thread_t *thread;

    /* The session's thread's alone. */
    uint64_t   t0;
    ppp_rx_t   rx;
    ppp_t      ppp;
    isp_nat_t *nat;
    uint8_t    enc[PPP_ENCODED_MAX(PPP_MAX_FRAME)];
    uint32_t   out_drops;
};

static isp_settings_t isp_settings = {
    .base_net     = 0x0a560000, /* 10.86.0.0 */
    .max_sessions = ISP_MAX_SESSIONS,
    .require_pap  = 0,
    .echo_secs    = 0
};
static uint8_t isp_used[ISP_MAX_SESSIONS + 1];

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
    }
}

static void
isp_ip_down(void *opaque)
{
    isp_session_t *s = (isp_session_t *) opaque;

    isp_nat_free(s->nat);
    s->nat = NULL;
}

static void
isp_ip_input(void *opaque, const uint8_t *packet, size_t len)
{
    isp_session_t *s = (isp_session_t *) opaque;

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

static void
isp_worker(void *arg)
{
    isp_session_t *s = (isp_session_t *) arg;
    uint8_t        buf[ISP_CHUNK];

    ppp_lower_up(&s->ppp, now32(s));

    for (;;) {
        uint32_t timeout;
        int      stop;
        size_t   n;

        /* Everything the guest has sent. */
        do {
            isp_mutex_lock(s->lock);
            stop = s->stop;
            n    = ring_get(&s->in, buf, sizeof(buf));
            isp_mutex_unlock(s->lock);
            if (n > 0)
                ppp_rx_feed(&s->rx, buf, n, isp_frame, s);
        } while ((n == sizeof(buf)) && !stop);
        if (stop)
            break;

        ppp_tick(&s->ppp, now32(s));
        timeout = ppp_next_timeout(&s->ppp, now32(s));
        if (timeout > ISP_MAX_WAIT)
            timeout = ISP_MAX_WAIT;

        /* Until the guest sends something, libslirp has something, or a
           timer is due. */
        if (s->nat != NULL)
            isp_nat_run(s->nat, s->wake, timeout);
        else
            isp_wake_wait(s->wake, timeout);
    }

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
isp_free_number(int number)
{
    isp_global_lock();
    if ((number >= 1) && (number <= ISP_MAX_SESSIONS))
        isp_used[number] = 0;
    isp_global_unlock();
}

static void
isp_session_free(isp_session_t *s)
{
    isp_wake_free(s->wake);
    isp_mutex_free(s->lock);
    free(s->in.buf);
    free(s->out.buf);
    isp_free_number(s->number);
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
    s->net     = s->settings.base_net + ((uint32_t) s->number << 8);
    s->gateway = s->net + 2;
    s->dns     = s->net + 3;
    s->guest   = s->net + 15;

    s->lock = isp_mutex_new();
    s->wake = isp_wake_new();
    if ((s->lock == NULL) || (s->wake == NULL) ||
        !ring_init(&s->in, ISP_IN_SIZE) || !ring_init(&s->out, ISP_OUT_SIZE)) {
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
    was_empty = (ring_used(&s->in) == 0);
    n         = ring_put(&s->in, buf, len);
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
    n = ring_get(&s->out, buf, len);
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
