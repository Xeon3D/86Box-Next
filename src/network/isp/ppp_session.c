/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             One PPP link, the ISP's end.  See ppp_session.h.
 *
 *             The option negotiation automaton is RFC 1661's, states,
 *             events and actions as section 4.1 tabulates them, run for LCP
 *             and for IPCP.  LCP is passive: the ISP is reached through a
 *             modem that is still dialling, ringing and training long after
 *             the line exists, so it says nothing, and starts no timer,
 *             until the guest's first Configure-Request.
 *
 *             What the ISP asks of the guest: a magic number, and an ACCM of
 *             zero so that only flag and escape need escaping.  No
 *             authentication unless configured, because the guest may enter
 *             any name and password; with it, PAP, and any pair is let in.
 *             What it grants: MRU, ACCM, magic number.  It rejects what it
 *             does not implement -- the compressions, authenticating itself,
 *             link quality, callback, multilink -- rather than acknowledge
 *             them.  IPCP gives the guest its address and DNS server and
 *             rejects Van Jacobson compression and NBNS.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ppp_framing.h"
#include "ppp_session.h"

#define RESTART_MS     3000  /* RFC 1661 Restart timer           */
#define MAX_CONFIGURE  10    /* Max-Configure                    */
#define MAX_TERMINATE  2     /* Max-Terminate                    */
#define MAX_FAILURE    5     /* Max-Failure: naks before rejects */
#define PAP_TIMEOUT_MS 30000 /* for the guest to authenticate    */

/* LCP options. */
#define LCP_MRU     1
#define LCP_ACCM    2
#define LCP_AUTH    3
#define LCP_QUALITY 4
#define LCP_MAGIC   5
#define LCP_PFC     7
#define LCP_ACFC    8

/* IPCP options. */
#define IPCP_ADDRS    1 /* the old IP-Addresses, RFC 1172 */
#define IPCP_COMP     2
#define IPCP_ADDR     3
#define IPCP_DNS1     129
#define IPCP_NBNS1    130
#define IPCP_DNS2     131
#define IPCP_NBNS2    132

static const char *const fsm_state_names[] = {
    "Initial", "Starting", "Closed", "Stopped", "Closing",
    "Stopping", "Req-Sent", "Ack-Rcvd", "Ack-Sent", "Opened"
};

static void
plog(ppp_t *ppp, const char *fmt, ...)
{
    char    buf[256];
    va_list ap;

    if ((ppp->cb == NULL) || (ppp->cb->log == NULL))
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ppp->cb->log(ppp->opaque, buf);
}

static uint32_t
get32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static void
put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

static void
fmt_ip(char *buf, size_t len, uint32_t ip)
{
    snprintf(buf, len, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
}

static uint32_t
next_magic(ppp_t *ppp)
{
    /* Unique per link, not secret; never zero. */
    do
        ppp->magic = (ppp->magic * 1664525u) + 1013904223u;
    while (ppp->magic == 0);
    return ppp->magic;
}

static int
timer_due(uint32_t now, uint32_t at)
{
    return (int32_t) (now - at) >= 0;
}

/* -------------------------------------------------------------- automaton */

static void fsm_sconfreq(ppp_t *ppp, ppp_fsm_t *f);

static void
fsm_send(ppp_t *ppp, ppp_fsm_t *f, int code, int id, const uint8_t *data, size_t len)
{
    uint8_t  pkt[PPP_DEFAULT_MRU];
    uint32_t mru = ppp->peer_mru ? ppp->peer_mru : PPP_DEFAULT_MRU;

    if (mru > sizeof(pkt))
        mru = sizeof(pkt);
    if (len > (mru - 4))
        len = mru - 4; /* a Code-Reject quoting a long packet is cut short */
    pkt[0] = (uint8_t) code;
    pkt[1] = (uint8_t) id;
    pkt[2] = (uint8_t) ((len + 4) >> 8);
    pkt[3] = (uint8_t) (len + 4);
    if (len > 0)
        memcpy(&pkt[4], data, len);

    /* RFC 1662 7.1: LCP goes with the default ACCM whatever was negotiated --
       a peer may not have taken the new one up yet. */
    ppp->cb->send(ppp->opaque, f->protocol, pkt, len + 4,
                  (f->protocol == PPP_PROTO_LCP) ? PPP_ACCM_ALL : ppp->tx_accm);
}

static void
fsm_timer(ppp_t *ppp, ppp_fsm_t *f)
{
    f->timer_on = 1;
    f->timer_at = ppp->now + RESTART_MS;
}

static void
fsm_new_state(ppp_t *ppp, ppp_fsm_t *f, int state)
{
    if (f->state != state)
        plog(ppp, "%s: %s -> %s", f->name, fsm_state_names[f->state], fsm_state_names[state]);
    f->state = state;
}

static void
fsm_send_termreq(ppp_t *ppp, ppp_fsm_t *f)
{
    const size_t n = strlen(ppp->why);

    f->req_id = f->id++;
    fsm_send(ppp, f, PPP_TERM_REQ, f->req_id, (const uint8_t *) ppp->why, n);
    f->restart--;
    fsm_timer(ppp, f);
}

/* A new Configure-Request: a new identifier, the options rebuilt, the
   restart counter full (pppd's fsm_sconfreq does the same). */
static void
fsm_sconfreq(ppp_t *ppp, ppp_fsm_t *f)
{
    f->req_id  = f->id++;
    f->req_len = f->ops->build_req(ppp, f->req);
    f->restart = MAX_CONFIGURE;
    fsm_send(ppp, f, PPP_CONF_REQ, f->req_id, f->req, f->req_len);
    f->restart--;
    fsm_timer(ppp, f);
}

static void
fsm_resend(ppp_t *ppp, ppp_fsm_t *f)
{
    fsm_send(ppp, f, PPP_CONF_REQ, f->req_id, f->req, f->req_len);
    f->restart--;
    fsm_timer(ppp, f);
}

static void
fsm_lower_up(ppp_t *ppp, ppp_fsm_t *f)
{
    switch (f->state) {
        case FSM_INITIAL:
            fsm_new_state(ppp, f, FSM_CLOSED);
            break;
        case FSM_STARTING:
            if (f->passive)
                fsm_new_state(ppp, f, FSM_STOPPED);
            else {
                f->naks = 0;
                f->ops->reset(ppp);
                fsm_sconfreq(ppp, f);
                fsm_new_state(ppp, f, FSM_REQSENT);
            }
            break;
        default:
            break;
    }
}

static void
fsm_lower_down(ppp_t *ppp, ppp_fsm_t *f)
{
    switch (f->state) {
        case FSM_CLOSED:
            fsm_new_state(ppp, f, FSM_INITIAL);
            break;
        case FSM_STOPPED:
            fsm_new_state(ppp, f, FSM_STARTING);
            break;
        case FSM_CLOSING:
            f->timer_on = 0;
            fsm_new_state(ppp, f, FSM_INITIAL);
            break;
        case FSM_STOPPING:
        case FSM_REQSENT:
        case FSM_ACKRCVD:
        case FSM_ACKSENT:
            f->timer_on = 0;
            fsm_new_state(ppp, f, FSM_STARTING);
            break;
        case FSM_OPENED:
            fsm_new_state(ppp, f, FSM_STARTING);
            f->ops->down(ppp);
            break;
        default:
            break;
    }
}

static void
fsm_open(ppp_t *ppp, ppp_fsm_t *f)
{
    switch (f->state) {
        case FSM_INITIAL:
            fsm_new_state(ppp, f, FSM_STARTING);
            break;
        case FSM_CLOSED:
            if (f->passive)
                fsm_new_state(ppp, f, FSM_STOPPED);
            else {
                f->ops->reset(ppp);
                fsm_sconfreq(ppp, f);
                fsm_new_state(ppp, f, FSM_REQSENT);
            }
            break;
        case FSM_CLOSING:
            fsm_new_state(ppp, f, FSM_STOPPING);
            break;
        default:
            break;
    }
}

static void
fsm_close(ppp_t *ppp, ppp_fsm_t *f)
{
    switch (f->state) {
        case FSM_STARTING:
            fsm_new_state(ppp, f, FSM_INITIAL);
            break;
        case FSM_STOPPED:
            fsm_new_state(ppp, f, FSM_CLOSED);
            break;
        case FSM_STOPPING:
            fsm_new_state(ppp, f, FSM_CLOSING);
            break;
        case FSM_REQSENT:
        case FSM_ACKRCVD:
        case FSM_ACKSENT:
        case FSM_OPENED: {
            const int was_opened = (f->state == FSM_OPENED);

            fsm_new_state(ppp, f, FSM_CLOSING);
            if (was_opened)
                f->ops->down(ppp);
            f->restart = MAX_TERMINATE;
            fsm_send_termreq(ppp, f);
            break;
        }
        default:
            break;
    }
}

static void
fsm_timeout(ppp_t *ppp, ppp_fsm_t *f)
{
    switch (f->state) {
        case FSM_CLOSING:
        case FSM_STOPPING:
            if (f->restart <= 0) {
                fsm_new_state(ppp, f, (f->state == FSM_CLOSING) ? FSM_CLOSED : FSM_STOPPED);
                f->ops->finished(ppp);
            } else
                fsm_send_termreq(ppp, f);
            break;

        case FSM_REQSENT:
        case FSM_ACKRCVD:
        case FSM_ACKSENT:
            if (f->restart <= 0) {
                snprintf(ppp->why, sizeof(ppp->why), "%s: no answer to %d Configure-Requests",
                         f->name, MAX_CONFIGURE);
                plog(ppp, "%s", ppp->why);
                fsm_new_state(ppp, f, FSM_STOPPED);
                f->ops->finished(ppp);
            } else {
                fsm_resend(ppp, f);
                if (f->state == FSM_ACKRCVD)
                    fsm_new_state(ppp, f, FSM_REQSENT);
            }
            break;

        default:
            break;
    }
}

static void
fsm_rconfreq(ppp_t *ppp, ppp_fsm_t *f, int id, const uint8_t *opt, size_t len)
{
    uint8_t reply[PPP_DEFAULT_MRU];
    size_t  reply_len = 0;
    int     code;

    switch (f->state) {
        case FSM_CLOSED:
            fsm_send(ppp, f, PPP_TERM_ACK, id, NULL, 0);
            return;
        case FSM_CLOSING:
        case FSM_STOPPING:
            return;
        case FSM_OPENED:
            /* The peer starts over: so do we. */
            f->ops->down(ppp);
            fsm_sconfreq(ppp, f);
            fsm_new_state(ppp, f, FSM_REQSENT);
            break;
        case FSM_STOPPED:
            /* The passive end wakes up here. */
            f->naks = 0;
            f->ops->reset(ppp);
            fsm_sconfreq(ppp, f);
            fsm_new_state(ppp, f, FSM_REQSENT);
            break;
        default:
            break;
    }

    code = f->ops->got_req(ppp, opt, len, reply, &reply_len, f->naks >= MAX_FAILURE);
    fsm_send(ppp, f, code, id, reply, reply_len);

    if (code == PPP_CONF_ACK) {
        f->naks = 0;
        if (f->state == FSM_ACKRCVD) {
            f->timer_on = 0;
            fsm_new_state(ppp, f, FSM_OPENED);
            f->ops->up(ppp);
        } else
            fsm_new_state(ppp, f, FSM_ACKSENT);
    } else {
        if (code == PPP_CONF_NAK)
            f->naks++;
        if (f->state != FSM_ACKRCVD)
            fsm_new_state(ppp, f, FSM_REQSENT);
    }
}

static void
fsm_rconfack(ppp_t *ppp, ppp_fsm_t *f, int id, const uint8_t *opt, size_t len)
{
    /* An Ack repeats our request exactly, or it is not one. */
    if ((id != f->req_id) || (len != f->req_len) || ((len > 0) && memcmp(opt, f->req, len))) {
        plog(ppp, "%s: discarded a Configure-Ack that does not match our request", f->name);
        return;
    }

    switch (f->state) {
        case FSM_CLOSED:
        case FSM_STOPPED:
            fsm_send(ppp, f, PPP_TERM_ACK, id, NULL, 0);
            break;
        case FSM_REQSENT:
            f->restart = MAX_CONFIGURE;
            fsm_new_state(ppp, f, FSM_ACKRCVD);
            break;
        case FSM_ACKRCVD:
            fsm_sconfreq(ppp, f);
            fsm_new_state(ppp, f, FSM_REQSENT);
            break;
        case FSM_ACKSENT:
            f->timer_on = 0;
            f->restart  = MAX_CONFIGURE;
            fsm_new_state(ppp, f, FSM_OPENED);
            f->ops->up(ppp);
            break;
        case FSM_OPENED:
            f->ops->down(ppp);
            fsm_sconfreq(ppp, f);
            fsm_new_state(ppp, f, FSM_REQSENT);
            break;
        default:
            break;
    }
}

static void
fsm_rconfnakrej(ppp_t *ppp, ppp_fsm_t *f, int code, int id, const uint8_t *opt, size_t len)
{
    if (id != f->req_id)
        return;

    switch (f->state) {
        case FSM_CLOSED:
        case FSM_STOPPED:
            fsm_send(ppp, f, PPP_TERM_ACK, id, NULL, 0);
            return;
        case FSM_OPENED:
            f->ops->down(ppp);
            fsm_new_state(ppp, f, FSM_REQSENT);
            break;
        case FSM_ACKRCVD:
            fsm_new_state(ppp, f, FSM_REQSENT);
            break;
        case FSM_REQSENT:
        case FSM_ACKSENT:
            break;
        default:
            return;
    }

    if (code == PPP_CONF_NAK)
        f->ops->got_nak(ppp, opt, len);
    else
        f->ops->got_rej(ppp, opt, len);
    fsm_sconfreq(ppp, f);
}

static void
fsm_rtermreq(ppp_t *ppp, ppp_fsm_t *f, int id, const uint8_t *data, size_t len)
{
    char why[64];
    size_t n = 0;

    for (size_t i = 0; (i < len) && (n < (sizeof(why) - 1)); i++)
        why[n++] = ((data[i] >= 0x20) && (data[i] < 0x7f)) ? (char) data[i] : '.';
    why[n] = '\0';

    switch (f->state) {
        case FSM_ACKRCVD:
        case FSM_ACKSENT:
            fsm_new_state(ppp, f, FSM_REQSENT);
            break;
        case FSM_OPENED:
            snprintf(ppp->why, sizeof(ppp->why), "%s: the guest ended the link%s%s%s", f->name,
                     n ? " (\"" : "", why, n ? "\")" : "");
            plog(ppp, "%s", ppp->why);
            fsm_new_state(ppp, f, FSM_STOPPING);
            f->ops->down(ppp);
            /* Zero the restart counter, and wait one period for the peer to
               see our ack before calling it finished. */
            f->restart = 0;
            fsm_timer(ppp, f);
            break;
        default:
            break;
    }
    fsm_send(ppp, f, PPP_TERM_ACK, id, NULL, 0);
}

static void
fsm_rtermack(ppp_t *ppp, ppp_fsm_t *f)
{
    switch (f->state) {
        case FSM_CLOSING:
            f->timer_on = 0;
            fsm_new_state(ppp, f, FSM_CLOSED);
            f->ops->finished(ppp);
            break;
        case FSM_STOPPING:
            f->timer_on = 0;
            fsm_new_state(ppp, f, FSM_STOPPED);
            f->ops->finished(ppp);
            break;
        case FSM_ACKRCVD:
            fsm_new_state(ppp, f, FSM_REQSENT);
            break;
        case FSM_OPENED:
            f->ops->down(ppp);
            fsm_sconfreq(ppp, f);
            fsm_new_state(ppp, f, FSM_REQSENT);
            break;
        default:
            break;
    }
}

static void
fsm_rcoderej(ppp_t *ppp, ppp_fsm_t *f, const uint8_t *data, size_t len)
{
    if (len < 1)
        return;
    plog(ppp, "%s: the guest rejected code %u", f->name, data[0]);
    if ((data[0] < PPP_CONF_REQ) || (data[0] > PPP_CODE_REJ))
        return; /* RXJ+: an extension it does not do; carry on */

    /* RXJ-: it cannot speak the protocol at all. */
    snprintf(ppp->why, sizeof(ppp->why), "%s: the guest rejected code %u", f->name, data[0]);
    f->timer_on = 0;
    switch (f->state) {
        case FSM_CLOSED:
        case FSM_CLOSING:
            fsm_new_state(ppp, f, FSM_CLOSED);
            f->ops->finished(ppp);
            break;
        case FSM_OPENED:
            fsm_new_state(ppp, f, FSM_STOPPED);
            f->ops->down(ppp);
            f->ops->finished(ppp);
            break;
        default:
            fsm_new_state(ppp, f, FSM_STOPPED);
            f->ops->finished(ppp);
            break;
    }
}

static void
fsm_input(ppp_t *ppp, ppp_fsm_t *f, const uint8_t *pkt, size_t len)
{
    size_t plen;
    int    code;
    int    id;

    if (len < 4)
        return;
    code = pkt[0];
    id   = pkt[1];
    plen = ((size_t) pkt[2] << 8) | pkt[3];
    if ((plen < 4) || (plen > len))
        return; /* malformed: silently discarded */
    len = plen - 4; /* anything past the length field is padding */
    pkt += 4;

    if ((f->state == FSM_INITIAL) || (f->state == FSM_STARTING))
        return; /* the layer below is not up: not for us yet */

    switch (code) {
        case PPP_CONF_REQ:
            fsm_rconfreq(ppp, f, id, pkt, len);
            break;
        case PPP_CONF_ACK:
            fsm_rconfack(ppp, f, id, pkt, len);
            break;
        case PPP_CONF_NAK:
        case PPP_CONF_REJ:
            fsm_rconfnakrej(ppp, f, code, id, pkt, len);
            break;
        case PPP_TERM_REQ:
            fsm_rtermreq(ppp, f, id, pkt, len);
            break;
        case PPP_TERM_ACK:
            fsm_rtermack(ppp, f);
            break;
        case PPP_CODE_REJ:
            fsm_rcoderej(ppp, f, pkt, len);
            break;
        default:
            if ((f->ops->extcode == NULL) || !f->ops->extcode(ppp, code, id, pkt, len)) {
                /* RUC: send the whole packet back, header and all. */
                fsm_send(ppp, f, PPP_CODE_REJ, f->id++, pkt - 4, len + 4);
            }
            break;
    }
}

/* ------------------------------------------------------ option building */

typedef struct {
    uint8_t *buf;
    size_t   len;
} opt_out_t;

static void
opt_add(opt_out_t *o, int type, const uint8_t *data, size_t len)
{
    o->buf[o->len++] = (uint8_t) type;
    o->buf[o->len++] = (uint8_t) (len + 2);
    if (len > 0)
        memcpy(&o->buf[o->len], data, len);
    o->len += len;
}

static void
opt_add32(opt_out_t *o, int type, uint32_t val)
{
    uint8_t b[4];

    put32(b, val);
    opt_add(o, type, b, 4);
}

/* Walks a list of options; returns 0 at the end, -1 if malformed. */
static int
opt_next(const uint8_t **p, const uint8_t *end, int *type, const uint8_t **data, size_t *olen)
{
    if (*p >= end)
        return 0;
    if (((end - *p) < 2) || ((*p)[1] < 2) || ((*p)[1] > (end - *p)))
        return -1;
    *type = (*p)[0];
    *data = *p + 2;
    *olen = (size_t) (*p)[1] - 2;
    *p += (*p)[1];
    return 1;
}

/* Sorts each option of a request into ack, nak or reject, and makes the
   reply RFC 1661 asks for: the rejects if there are any, else the naks if
   there are any, else every option, acknowledged. */
typedef struct {
    uint8_t   nak[PPP_DEFAULT_MRU];
    uint8_t   rej[PPP_DEFAULT_MRU];
    opt_out_t naks;
    opt_out_t rejs;
} opt_sort_t;

static void
opt_sort_init(opt_sort_t *s)
{
    s->naks.buf = s->nak;
    s->naks.len = 0;
    s->rejs.buf = s->rej;
    s->rejs.len = 0;
}

static void
opt_reject(opt_sort_t *s, int type, const uint8_t *data, size_t len)
{
    if ((s->rejs.len + len + 2) <= sizeof(s->rej))
        opt_add(&s->rejs, type, data, len);
}

/* A nak, unless naks have stopped working, in which case the peer's own
   value goes back as a reject. */
static void
opt_nak32(opt_sort_t *s, int reject_naks, int type, const uint8_t *data, size_t len, uint32_t val)
{
    if (reject_naks)
        opt_reject(s, type, data, len);
    else if ((s->naks.len + 6) <= sizeof(s->nak))
        opt_add32(&s->naks, type, val);
}

static int
opt_sort_reply(opt_sort_t *s, const uint8_t *opt, size_t len, uint8_t *reply, size_t *reply_len)
{
    if (s->rejs.len > 0) {
        memcpy(reply, s->rej, s->rejs.len);
        *reply_len = s->rejs.len;
        return PPP_CONF_REJ;
    }
    if (s->naks.len > 0) {
        memcpy(reply, s->nak, s->naks.len);
        *reply_len = s->naks.len;
        return PPP_CONF_NAK;
    }
    memcpy(reply, opt, len);
    *reply_len = len;
    return PPP_CONF_ACK;
}

/* -------------------------------------------------------------------- LCP */

static void ipcp_lower_up(ppp_t *ppp);

static void
lcp_reset(ppp_t *ppp)
{
    ppp->want_magic    = 1;
    ppp->want_accm     = 1;
    ppp->want_accm_val = 0;
    ppp->want_pap      = ppp->cfg.require_pap;
    ppp->peer_mru      = PPP_DEFAULT_MRU;
    ppp->peer_accm     = PPP_ACCM_ALL;
    ppp->peer_magic    = 0;
    next_magic(ppp);
}

static size_t
lcp_build_req(ppp_t *ppp, uint8_t *out)
{
    opt_out_t o = { out, 0 };

    if (ppp->want_accm)
        opt_add32(&o, LCP_ACCM, ppp->want_accm_val);
    if (ppp->want_pap) {
        const uint8_t pap[2] = { PPP_PROTO_PAP >> 8, PPP_PROTO_PAP & 0xff };

        opt_add(&o, LCP_AUTH, pap, 2);
    }
    if (ppp->want_magic)
        opt_add32(&o, LCP_MAGIC, ppp->magic);
    return o.len;
}

static void
lcp_got_nak(ppp_t *ppp, const uint8_t *opt, size_t len)
{
    const uint8_t *p   = opt;
    const uint8_t *end = opt + len;
    const uint8_t *data;
    size_t         olen;
    int            type;

    while (opt_next(&p, end, &type, &data, &olen) > 0) {
        switch (type) {
            case LCP_ACCM:
                if (olen == 4)
                    ppp->want_accm_val = get32(data);
                break;
            case LCP_AUTH:
                /* It wants another protocol (CHAP, most likely).  This ISP
                   lets anyone in, so it stops asking rather than refuse. */
                if (ppp->want_pap) {
                    plog(ppp, "LCP: the guest will not do PAP; continuing without authentication");
                    ppp->want_pap = 0;
                }
                break;
            case LCP_MAGIC:
                next_magic(ppp);
                break;
            default:
                break;
        }
    }
}

static void
lcp_got_rej(ppp_t *ppp, const uint8_t *opt, size_t len)
{
    const uint8_t *p   = opt;
    const uint8_t *end = opt + len;
    const uint8_t *data;
    size_t         olen;
    int            type;

    while (opt_next(&p, end, &type, &data, &olen) > 0) {
        switch (type) {
            case LCP_ACCM:
                ppp->want_accm = 0;
                break;
            case LCP_AUTH:
                if (ppp->want_pap)
                    plog(ppp, "LCP: the guest refuses to authenticate; continuing without");
                ppp->want_pap = 0;
                break;
            case LCP_MAGIC:
                ppp->want_magic = 0;
                break;
            default:
                break;
        }
    }
}

static int
lcp_got_req(ppp_t *ppp, const uint8_t *opt, size_t len, uint8_t *reply, size_t *reply_len, int reject_naks)
{
    const uint8_t *p   = opt;
    const uint8_t *end = opt + len;
    const uint8_t *data;
    size_t         olen;
    int            type;
    int            r;
    opt_sort_t     s;
    uint32_t       mru   = PPP_DEFAULT_MRU;
    uint32_t       accm  = PPP_ACCM_ALL;
    uint32_t       magic = 0;

    opt_sort_init(&s);
    while ((r = opt_next(&p, end, &type, &data, &olen)) > 0) {
        switch (type) {
            case LCP_MRU:
                if (olen != 2)
                    opt_reject(&s, type, data, olen);
                else if ((((uint32_t) data[0] << 8) | data[1]) < PPP_MIN_MRU) {
                    const uint8_t min[2] = { PPP_MIN_MRU >> 8, PPP_MIN_MRU & 0xff };

                    if (reject_naks)
                        opt_reject(&s, type, data, olen);
                    else
                        opt_add(&s.naks, LCP_MRU, min, 2);
                } else
                    mru = ((uint32_t) data[0] << 8) | data[1];
                break;
            case LCP_ACCM:
                if (olen != 4)
                    opt_reject(&s, type, data, olen);
                else
                    accm = get32(data);
                break;
            case LCP_MAGIC:
                if (olen != 4)
                    opt_reject(&s, type, data, olen);
                else if (ppp->want_magic && (get32(data) == ppp->magic) && (get32(data) != 0)) {
                    /* Our own number: a looped-back line, or a coincidence. */
                    plog(ppp, "LCP: the guest's magic number is ours; asking for another");
                    opt_nak32(&s, reject_naks, type, data, olen, next_magic(ppp) ^ 0x5a5a5a5au);
                } else
                    magic = get32(data);
                break;
            case LCP_AUTH:    /* we will not authenticate ourselves to the guest */
            case LCP_QUALITY: /* no link quality monitoring                    */
            case LCP_PFC:     /* no protocol field compression                 */
            case LCP_ACFC:    /* no address and control field compression      */
            default:          /* callback, multilink and anything else         */
                opt_reject(&s, type, data, olen);
                break;
        }
    }
    if (r < 0) {
        /* A malformed option list: reject what is left of it. */
        opt_reject(&s, p[0], p + 2, (size_t) (end - p) >= 2 ? (size_t) (end - p) - 2 : 0);
    }

    r = opt_sort_reply(&s, opt, len, reply, reply_len);
    if (r == PPP_CONF_ACK) {
        ppp->peer_mru   = mru;
        ppp->peer_accm  = accm;
        ppp->peer_magic = magic;
    }
    return r;
}

static void
lcp_up(ppp_t *ppp)
{
    ppp->tx_accm  = ppp->peer_accm;
    ppp->rx_accm  = ppp->want_accm ? ppp->want_accm_val : PPP_ACCM_ALL;
    ppp->was_open = 1;
    ppp->why[0]   = '\0';
    ppp->echo_pending = 0;
    ppp->echo_at      = ppp->now + ppp->cfg.echo_interval_ms;
    plog(ppp, "LCP: up; guest MRU %u, ACCM %08X; %s", ppp->peer_mru, ppp->peer_accm,
         ppp->want_pap ? "waiting for PAP" : "no authentication");

    if (ppp->want_pap) {
        ppp->phase         = PPP_PHASE_AUTHENTICATE;
        ppp->authenticated = 0;
        ppp->auth_deadline = ppp->now + PAP_TIMEOUT_MS;
    } else {
        ppp->phase = PPP_PHASE_NETWORK;
        ipcp_lower_up(ppp);
    }
}

static void
lcp_down(ppp_t *ppp)
{
    plog(ppp, "LCP: down");
    fsm_lower_down(ppp, &ppp->ipcp);
    ppp->phase         = PPP_PHASE_TERMINATE;
    ppp->tx_accm       = PPP_ACCM_ALL;
    ppp->rx_accm       = PPP_ACCM_ALL;
    ppp->authenticated = 0;
}

static void
lcp_finished(ppp_t *ppp)
{
    ppp->phase = PPP_PHASE_DEAD;
    if (!ppp->finish_reported) {
        ppp->finish_reported = 1;
        if (ppp->why[0] == '\0')
            snprintf(ppp->why, sizeof(ppp->why), "LCP: link terminated");
        if (ppp->cb->finished != NULL)
            ppp->cb->finished(ppp->opaque, ppp->why);
    }
}

static int
lcp_extcode(ppp_t *ppp, int code, int id, const uint8_t *data, size_t len)
{
    switch (code) {
        case PPP_PROT_REJ:
            if (len >= 2) {
                const uint16_t proto = (uint16_t) ((data[0] << 8) | data[1]);

                plog(ppp, "LCP: the guest rejected protocol %04X", proto);
                if ((proto == PPP_PROTO_IPCP) || (proto == PPP_PROTO_IP)) {
                    /* Without IP there is nothing this ISP can do for it. */
                    snprintf(ppp->why, sizeof(ppp->why), "the guest rejected IP");
                    fsm_close(ppp, &ppp->lcp);
                }
            }
            return 1;

        case PPP_ECHO_REQ:
            if (ppp->lcp.state == FSM_OPENED) {
                uint8_t rep[PPP_DEFAULT_MRU];
                size_t  n = (len > (sizeof(rep))) ? sizeof(rep) : len;

                if (n < 4)
                    n = 4;
                memset(rep, 0, 4);
                if (len > 4)
                    memcpy(&rep[4], &data[4], n - 4);
                put32(rep, ppp->want_magic ? ppp->magic : 0);
                fsm_send(ppp, &ppp->lcp, PPP_ECHO_REP, id, rep, n);
            }
            return 1;

        case PPP_ECHO_REP:
            ppp->echo_pending = 0;
            return 1;

        case PPP_DISC_REQ:
            return 1;

        default:
            return 0;
    }
}

static const ppp_fsm_ops_t lcp_ops = {
    .reset     = lcp_reset,
    .build_req = lcp_build_req,
    .got_nak   = lcp_got_nak,
    .got_rej   = lcp_got_rej,
    .got_req   = lcp_got_req,
    .up        = lcp_up,
    .down      = lcp_down,
    .finished  = lcp_finished,
    .extcode   = lcp_extcode
};

/* -------------------------------------------------------------------- PAP */

static void
pap_input(ppp_t *ppp, const uint8_t *pkt, size_t len)
{
    static const char msg[] = "Welcome";
    uint8_t           rep[4 + 1 + sizeof(msg)];
    size_t            plen;
    size_t            ulen;
    size_t            wlen;
    char              user[64];

    if ((len < 4) || (pkt[0] != 1))
        return; /* only Authenticate-Request comes this way */
    plen = ((size_t) pkt[2] << 8) | pkt[3];
    if ((plen < 6) || (plen > len))
        return;
    ulen = pkt[4];
    if ((5 + ulen + 1) > plen)
        return;
    wlen = pkt[5 + ulen];
    if ((6 + ulen + wlen) > plen)
        return;

    /* The name goes in the log; the password never does. */
    {
        size_t n = 0;

        for (size_t i = 0; (i < ulen) && (n < (sizeof(user) - 1)); i++)
            user[n++] = ((pkt[5 + i] >= 0x20) && (pkt[5 + i] < 0x7f)) ? (char) pkt[5 + i] : '?';
        user[n] = '\0';
    }

    rep[0] = 2; /* Authenticate-Ack, whatever the pair */
    rep[1] = pkt[1];
    rep[2] = 0;
    rep[3] = (uint8_t) (5 + sizeof(msg) - 1);
    rep[4] = (uint8_t) (sizeof(msg) - 1);
    memcpy(&rep[5], msg, sizeof(msg) - 1);
    ppp->cb->send(ppp->opaque, PPP_PROTO_PAP, rep, 5 + sizeof(msg) - 1, ppp->tx_accm);

    if (!ppp->authenticated)
        plog(ppp, "PAP: \"%s\" let in", user);
    if (ppp->phase == PPP_PHASE_AUTHENTICATE) {
        ppp->authenticated = 1;
        ppp->phase         = PPP_PHASE_NETWORK;
        ipcp_lower_up(ppp);
    }
}

/* ------------------------------------------------------------------- IPCP */

static void
ipcp_lower_up(ppp_t *ppp)
{
    fsm_lower_up(ppp, &ppp->ipcp);
}

static void
ipcp_reset(ppp_t *ppp)
{
    ppp->want_addr = 1;
    ppp->local_ip  = ppp->cfg.local_ip;
}

static size_t
ipcp_build_req(ppp_t *ppp, uint8_t *out)
{
    opt_out_t o = { out, 0 };

    if (ppp->want_addr)
        opt_add32(&o, IPCP_ADDR, ppp->local_ip);
    return o.len;
}

static void
ipcp_got_nak(ppp_t *ppp, const uint8_t *opt, size_t len)
{
    const uint8_t *p   = opt;
    const uint8_t *end = opt + len;
    const uint8_t *data;
    size_t         olen;
    int            type;

    while (opt_next(&p, end, &type, &data, &olen) > 0) {
        /* Our end of a point-to-point link can be any address; if the guest
           insists on one, it can have it. */
        if ((type == IPCP_ADDR) && (olen == 4) && (get32(data) != 0))
            ppp->local_ip = get32(data);
    }
}

static void
ipcp_got_rej(ppp_t *ppp, const uint8_t *opt, size_t len)
{
    const uint8_t *p   = opt;
    const uint8_t *end = opt + len;
    const uint8_t *data;
    size_t         olen;
    int            type;

    while (opt_next(&p, end, &type, &data, &olen) > 0) {
        if (type == IPCP_ADDR)
            ppp->want_addr = 0;
    }
}

static int
ipcp_got_req(ppp_t *ppp, const uint8_t *opt, size_t len, uint8_t *reply, size_t *reply_len, int reject_naks)
{
    const uint8_t *p   = opt;
    const uint8_t *end = opt + len;
    const uint8_t *data;
    size_t         olen;
    int            type;
    int            r;
    int            got_addr = 0;
    opt_sort_t     s;

    opt_sort_init(&s);
    while ((r = opt_next(&p, end, &type, &data, &olen)) > 0) {
        switch (type) {
            case IPCP_ADDR:
                got_addr = 1;
                /* 0.0.0.0 asks to be given one; anything else is not its to
                   choose.  Either way it gets the one this session owns. */
                if (olen != 4)
                    opt_reject(&s, type, data, olen);
                else if (get32(data) != ppp->cfg.peer_ip)
                    opt_nak32(&s, reject_naks, type, data, olen, ppp->cfg.peer_ip);
                break;
            case IPCP_DNS1:
            case IPCP_DNS2: {
                const uint32_t dns = ppp->cfg.dns[(type == IPCP_DNS1) ? 0 : 1];

                if ((olen != 4) || (dns == 0))
                    opt_reject(&s, type, data, olen);
                else if (get32(data) != dns)
                    opt_nak32(&s, reject_naks, type, data, olen, dns);
                break;
            }
            case IPCP_COMP:   /* Van Jacobson: not implemented */
            case IPCP_ADDRS:  /* the deprecated pair          */
            case IPCP_NBNS1:  /* no WINS                      */
            case IPCP_NBNS2:
            default:
                opt_reject(&s, type, data, olen);
                break;
        }
    }
    if (r < 0)
        opt_reject(&s, p[0], p + 2, (size_t) (end - p) >= 2 ? (size_t) (end - p) - 2 : 0);

    /* A guest that does not ask for an address is told one (RFC 1332 3.3). */
    if (!got_addr && (s.rejs.len == 0) && !reject_naks)
        opt_add32(&s.naks, IPCP_ADDR, ppp->cfg.peer_ip);

    return opt_sort_reply(&s, opt, len, reply, reply_len);
}

static void
ipcp_up(ppp_t *ppp)
{
    char peer[16];
    char local[16];
    char dns[16];

    fmt_ip(peer, sizeof(peer), ppp->cfg.peer_ip);
    fmt_ip(local, sizeof(local), ppp->local_ip);
    fmt_ip(dns, sizeof(dns), ppp->cfg.dns[0]);
    plog(ppp, "IPCP: up; guest %s, gateway %s, DNS %s", peer, local, dns);
    if (ppp->cb->ip_up != NULL)
        ppp->cb->ip_up(ppp->opaque);
}

static void
ipcp_down(ppp_t *ppp)
{
    plog(ppp, "IPCP: down (%u packets in, %u out, %u dropped)", ppp->ip_in, ppp->ip_out, ppp->ip_dropped);
    if (ppp->cb->ip_down != NULL)
        ppp->cb->ip_down(ppp->opaque);
}

static void
ipcp_finished(ppp_t *ppp)
{
    /* No IP: nothing left for the link to carry. */
    if (ppp->lcp.state == FSM_OPENED) {
        if (ppp->why[0] == '\0')
            snprintf(ppp->why, sizeof(ppp->why), "IPCP failed");
        fsm_close(ppp, &ppp->lcp);
    }
}

static const ppp_fsm_ops_t ipcp_ops = {
    .reset     = ipcp_reset,
    .build_req = ipcp_build_req,
    .got_nak   = ipcp_got_nak,
    .got_rej   = ipcp_got_rej,
    .got_req   = ipcp_got_req,
    .up        = ipcp_up,
    .down      = ipcp_down,
    .finished  = ipcp_finished,
    .extcode   = NULL
};

/* ------------------------------------------------------------------ link */

void
ppp_init(ppp_t *ppp, const ppp_config_t *cfg, const ppp_callbacks_t *cb, void *opaque, uint32_t now)
{
    memset(ppp, 0, sizeof(ppp_t));
    ppp->cfg      = *cfg;
    ppp->cb       = cb;
    ppp->opaque   = opaque;
    ppp->now      = now;
    ppp->magic    = cfg->magic_seed;
    ppp->tx_accm  = PPP_ACCM_ALL;
    ppp->rx_accm  = PPP_ACCM_ALL;
    ppp->peer_mru = PPP_DEFAULT_MRU;
    if (ppp->cfg.echo_fails <= 0)
        ppp->cfg.echo_fails = 4;

    ppp->lcp.name     = "LCP";
    ppp->lcp.protocol = PPP_PROTO_LCP;
    ppp->lcp.ops      = &lcp_ops;
    ppp->lcp.passive  = 1;
    ppp->lcp.id       = (uint8_t) cfg->magic_seed;

    ppp->ipcp.name     = "IPCP";
    ppp->ipcp.protocol = PPP_PROTO_IPCP;
    ppp->ipcp.ops      = &ipcp_ops;
    ppp->ipcp.id       = (uint8_t) (cfg->magic_seed >> 8);

    lcp_reset(ppp);
    ipcp_reset(ppp);
    fsm_open(ppp, &ppp->lcp);
    fsm_open(ppp, &ppp->ipcp);
}

void
ppp_lower_up(ppp_t *ppp, uint32_t now)
{
    ppp->now   = now;
    ppp->phase = PPP_PHASE_ESTABLISH;
    fsm_lower_up(ppp, &ppp->lcp);
}

void
ppp_input(ppp_t *ppp, const uint8_t *frame, size_t len, uint32_t now)
{
    const uint8_t *p = frame;
    size_t         n = len;
    uint16_t       proto;

    ppp->now = now;

    /* The address and control fields may be compressed away and the protocol
       field cut to one byte: neither is asked for, both are understood. */
    if ((n >= 2) && (p[0] == 0xff) && (p[1] == 0x03)) {
        p += 2;
        n -= 2;
    }
    if (n < 1)
        return;
    if (p[0] & 1) {
        proto = p[0];
        p++;
        n--;
    } else {
        if ((n < 2) || !(p[1] & 1))
            return; /* not a protocol number */
        proto = (uint16_t) ((p[0] << 8) | p[1]);
        p += 2;
        n -= 2;
    }

    if (proto == PPP_PROTO_LCP) {
        fsm_input(ppp, &ppp->lcp, p, n);
        return;
    }
    if (ppp->lcp.state != FSM_OPENED)
        return; /* RFC 1661 3.4: only LCP before the link is open */

    switch (proto) {
        case PPP_PROTO_PAP:
            pap_input(ppp, p, n);
            break;
        case PPP_PROTO_IPCP:
            if (ppp->phase == PPP_PHASE_NETWORK)
                fsm_input(ppp, &ppp->ipcp, p, n);
            break;
        case PPP_PROTO_IP:
            if (ppp->ipcp.state == FSM_OPENED) {
                ppp->ip_in++;
                if (ppp->cb->ip_input != NULL)
                    ppp->cb->ip_input(ppp->opaque, p, n);
            }
            break;
        default: {
            /* CCP, IPXCP, NBFCP, IPV6CP and the rest: a Protocol-Reject makes
               the guest stop trying. */
            uint8_t rej[PPP_DEFAULT_MRU];
            size_t  q = n;

            if (q > (sizeof(rej) - 2))
                q = sizeof(rej) - 2;
            rej[0] = (uint8_t) (proto >> 8);
            rej[1] = (uint8_t) proto;
            memcpy(&rej[2], p, q);
            ppp->prot_rejects++;
            plog(ppp, "LCP: rejecting protocol %04X", proto);
            fsm_send(ppp, &ppp->lcp, PPP_PROT_REJ, ppp->lcp.id++, rej, q + 2);
            break;
        }
    }
}

void
ppp_send_ip(ppp_t *ppp, const uint8_t *packet, size_t len)
{
    if ((ppp->ipcp.state != FSM_OPENED) || (len > ppp->peer_mru)) {
        ppp->ip_dropped++;
        return;
    }
    ppp->ip_out++;
    ppp->cb->send(ppp->opaque, PPP_PROTO_IP, packet, len, ppp->tx_accm);
}

void
ppp_tick(ppp_t *ppp, uint32_t now)
{
    ppp->now = now;

    if (ppp->lcp.timer_on && timer_due(now, ppp->lcp.timer_at)) {
        ppp->lcp.timer_on = 0;
        fsm_timeout(ppp, &ppp->lcp);
    }
    if (ppp->ipcp.timer_on && timer_due(now, ppp->ipcp.timer_at)) {
        ppp->ipcp.timer_on = 0;
        fsm_timeout(ppp, &ppp->ipcp);
    }

    if ((ppp->phase == PPP_PHASE_AUTHENTICATE) && timer_due(now, ppp->auth_deadline)) {
        snprintf(ppp->why, sizeof(ppp->why), "PAP: the guest did not authenticate in %d s",
                 PAP_TIMEOUT_MS / 1000);
        plog(ppp, "%s", ppp->why);
        fsm_close(ppp, &ppp->lcp);
    }

    if (ppp->cfg.echo_interval_ms && (ppp->lcp.state == FSM_OPENED) && timer_due(now, ppp->echo_at)) {
        if (ppp->echo_pending >= ppp->cfg.echo_fails) {
            snprintf(ppp->why, sizeof(ppp->why), "LCP: %d echo requests unanswered", ppp->echo_pending);
            plog(ppp, "%s", ppp->why);
            fsm_close(ppp, &ppp->lcp);
        } else {
            uint8_t m[4];

            put32(m, ppp->want_magic ? ppp->magic : 0);
            fsm_send(ppp, &ppp->lcp, PPP_ECHO_REQ, ppp->lcp.id++, m, 4);
            ppp->echo_pending++;
            ppp->echo_at = now + ppp->cfg.echo_interval_ms;
        }
    }
}

static void
min_due(uint32_t *best, uint32_t now, uint32_t at)
{
    const uint32_t d = timer_due(now, at) ? 0 : (at - now);

    if (d < *best)
        *best = d;
}

uint32_t
ppp_next_timeout(const ppp_t *ppp, uint32_t now)
{
    uint32_t best = UINT32_MAX;

    if (ppp->lcp.timer_on)
        min_due(&best, now, ppp->lcp.timer_at);
    if (ppp->ipcp.timer_on)
        min_due(&best, now, ppp->ipcp.timer_at);
    if (ppp->phase == PPP_PHASE_AUTHENTICATE)
        min_due(&best, now, ppp->auth_deadline);
    if (ppp->cfg.echo_interval_ms && (ppp->lcp.state == FSM_OPENED))
        min_due(&best, now, ppp->echo_at);
    return best;
}

void
ppp_close(ppp_t *ppp, const char *why, uint32_t now)
{
    ppp->now = now;
    snprintf(ppp->why, sizeof(ppp->why), "%s", why);
    fsm_close(ppp, &ppp->lcp);
}

int
ppp_ip_open(const ppp_t *ppp)
{
    return ppp->ipcp.state == FSM_OPENED;
}
