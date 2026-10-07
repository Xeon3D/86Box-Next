/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             One PPP link, the ISP's end.  See ppp_session.h.
 *
 *             The option negotiation automaton is RFC 1661's, states,
 *             events and actions as section 4.1 tabulates them, run for LCP,
 *             IPCP and CCP.  LCP is passive: the ISP is reached through a
 *             modem that is still dialling, ringing and training long after
 *             the line exists, so it says nothing, and starts no timer,
 *             until the guest's first Configure-Request.
 *
 *             What the ISP asks of the guest: a magic number, an ACCM of
 *             zero so that only flag and escape need escaping, and -- if
 *             so configured -- authentication, the strongest method allowed
 *             first, the guest's own choice taken when it names an allowed
 *             one.  What it grants: MRU, ACCM, magic number, and Multilink's
 *             three options when Multilink is on.  It rejects the rest
 *             (authenticating itself, link quality, compression of the
 *             headers, callback).  IPCP gives the guest its address, DNS and
 *             WINS servers and rejects Van Jacobson compression.  CCP offers
 *             one method at a time and takes the first of the guest's it
 *             can do; MPPE comes with MS-CHAP's keys and excludes the other
 *             compressions (MPPC rides along in its option).
 *
 *             Multilink: a link that negotiated MRRU asks link_ready()
 *             whether it joins a bundle; if so it keeps only LCP and
 *             authentication and hands every other packet to the bundle's
 *             owner, whose IPCP and CCP serve all its links.  The owner
 *             reassembles MP fragments from all of them and sends whole
 *             packets in turn, or fragments across them.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "isp_crypto.h"
#include "ppp_framing.h"
#include "ppp_session.h"

#define RESTART_MS     3000  /* RFC 1661 Restart timer           */
#define MAX_CONFIGURE  10    /* Max-Configure                    */
#define MAX_TERMINATE  2     /* Max-Terminate                    */
#define MAX_FAILURE    5     /* Max-Failure: naks before rejects */
#define AUTH_TIMEOUT_MS 30000 /* for the guest to authenticate   */
#define AUTH_TRIES     3     /* wrong passwords before hanging up */
#define CHAP_RETRY_MS  3000
#define CHAP_SENDS     10
#define RESET_RETRY_MS 1000  /* CCP Reset-Requests at most this often */
#define BUNDLE_MRRU    1500
#define ISP_NAME       "isp"

/* LCP options. */
#define LCP_MRU     1
#define LCP_ACCM    2
#define LCP_AUTH    3
#define LCP_QUALITY 4
#define LCP_MAGIC   5
#define LCP_PFC     7
#define LCP_ACFC    8
#define LCP_MRRU    17
#define LCP_SSNHF   18
#define LCP_ED      19

/* IPCP options. */
#define IPCP_ADDRS    1 /* the old IP-Addresses, RFC 1172 */
#define IPCP_COMP     2
#define IPCP_ADDR     3
#define IPCP_DNS1     129
#define IPCP_NBNS1    130
#define IPCP_DNS2     131
#define IPCP_NBNS2    132

/* CHAP codes. */
#define CHAP_CHALLENGE 1
#define CHAP_RESPONSE  2
#define CHAP_SUCCESS   3
#define CHAP_FAILURE   4

/* CCP's offers, in order. */
enum {
    OFFER_MPPX = 0,
    OFFER_DEFLATE,
    OFFER_BSD,
    OFFER_PRED1,
    OFFER_NONE
};

/* Authentication methods, the strongest first. */
static const int ap_order[] = {
    PPP_AP_MSCHAP2,      PPP_AP_CHAP_SHA512, PPP_AP_CHAP_SHA3_512, PPP_AP_CHAP_SHA384,
    PPP_AP_CHAP_SHA3_384, PPP_AP_CHAP_SHA256, PPP_AP_CHAP_SHA3_256, PPP_AP_CHAP_SHA1,
    PPP_AP_MSCHAP1,      PPP_AP_CHAP_MD5,    PPP_AP_PAP
};

static const char *const fsm_state_names[] = {
    "Initial", "Starting", "Closed", "Stopped", "Closing",
    "Stopping", "Req-Sent", "Ack-Rcvd", "Ack-Sent", "Opened"
};

static void bundle_send(ppp_t *ppp, uint16_t proto, const uint8_t *info, size_t len);

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

static uint16_t
get16(const uint8_t *p)
{
    return (uint16_t) ((p[0] << 8) | p[1]);
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

/* Printable, for names that end up in the log. */
static void
printable(const uint8_t *in, size_t len, char *out, size_t size)
{
    size_t n = 0;

    for (size_t i = 0; (i < len) && (n < (size - 1)); i++)
        out[n++] = ((in[i] >= 0x20) && (in[i] < 0x7f)) ? (char) in[i] : '?';
    out[n] = '\0';
}

/* A close asked for while a handler is running; done once it returns. */
static void
close_later(ppp_t *ppp, const char *fmt, ...)
{
    va_list ap;

    if (ppp->close_why[0] != '\0')
        return;
    va_start(ap, fmt);
    vsnprintf(ppp->close_why, sizeof(ppp->close_why), fmt, ap);
    va_end(ap);
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

    if (f->bundle) {
        bundle_send(ppp, f->protocol, pkt, len + 4);
        return;
    }
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

/* Start the negotiation over from Opened (CCP, when Predictor loses its
   way). */
static void
fsm_restart(ppp_t *ppp, ppp_fsm_t *f)
{
    if (f->state != FSM_OPENED)
        return;
    f->ops->down(ppp);
    f->ops->reset(ppp);
    fsm_sconfreq(ppp, f);
    fsm_new_state(ppp, f, FSM_REQSENT);
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
                if (f == &ppp->lcp)
                    snprintf(ppp->why, sizeof(ppp->why), "%s: no answer to %d Configure-Requests", f->name,
                             MAX_CONFIGURE);
                plog(ppp, "%s: no answer to %d Configure-Requests", f->name, MAX_CONFIGURE);
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

    f->resend = 0;
    code      = f->ops->got_req(ppp, opt, len, reply, &reply_len, f->naks >= MAX_FAILURE);
    if (f->resend) {
        /* What the peer asked for changes what we ask for (Multilink). */
        f->resend = 0;
        fsm_sconfreq(ppp, f);
        if (f->state == FSM_ACKRCVD)
            fsm_new_state(ppp, f, FSM_REQSENT);
    }
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

    printable(data, len, why, sizeof(why));
    switch (f->state) {
        case FSM_ACKRCVD:
        case FSM_ACKSENT:
            fsm_new_state(ppp, f, FSM_REQSENT);
            break;
        case FSM_OPENED:
            if (f == &ppp->lcp)
                snprintf(ppp->why, sizeof(ppp->why), "%s: the guest ended the link%s%s%s", f->name,
                         why[0] ? " (\"" : "", why, why[0] ? "\")" : "");
            plog(ppp, "%s: the guest ended it%s%s%s", f->name, why[0] ? " (\"" : "", why, why[0] ? "\")" : "");
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
    if (f == &ppp->lcp)
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

static void
opt_nak(opt_sort_t *s, int reject_naks, int type, const uint8_t *data, size_t len, const uint8_t *val, size_t vlen)
{
    if (reject_naks)
        opt_reject(s, type, data, len);
    else if ((s->naks.len + vlen + 2) <= sizeof(s->nak))
        opt_add(&s->naks, type, val, vlen);
}

/* A nak, unless naks have stopped working, in which case the peer's own
   value goes back as a reject. */
static void
opt_nak32(opt_sort_t *s, int reject_naks, int type, const uint8_t *data, size_t len, uint32_t val)
{
    uint8_t b[4];

    put32(b, val);
    opt_nak(s, reject_naks, type, data, len, b, 4);
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

/* -------------------------------------------------- authentication: which */

/* May the guest use this method under this configuration? */
static int
ap_usable(const ppp_t *ppp, int ap)
{
    if ((ap < 0) || (ap >= PPP_AP_COUNT) || !(ppp->cfg.auth_protos & (1u << ap)))
        return 0;
    /* MS-CHAP-2 proves the ISP knows the password too: only with accounts. */
    if ((ap == PPP_AP_MSCHAP2) && (ppp->cfg.auth != PPP_AUTH_ACCOUNTS))
        return 0;
    /* Encryption needs MS-CHAP's keys. */
    if ((ppp->cfg.mppe == PPP_MPPE_REQUIRED) && (ap != PPP_AP_MSCHAP1) && (ap != PPP_AP_MSCHAP2))
        return 0;
    return 1;
}

/* The next method to ask for, not yet turned down; -1 if none is left. */
static int
next_ap(const ppp_t *ppp)
{
    for (size_t i = 0; i < sizeof(ap_order) / sizeof(ap_order[0]); i++)
        if (ap_usable(ppp, ap_order[i]) && !(ppp->tried_aps & (1u << ap_order[i])))
            return ap_order[i];
    return -1;
}

static size_t
auth_option(int ap, uint8_t *out)
{
    if (ap == PPP_AP_PAP) {
        out[0] = PPP_PROTO_PAP >> 8;
        out[1] = PPP_PROTO_PAP & 0xff;
        return 2;
    }
    out[0] = PPP_PROTO_CHAP >> 8;
    out[1] = PPP_PROTO_CHAP & 0xff;
    out[2] = (uint8_t) ppp_ap_chap_alg(ap);
    return 3;
}

/* -------------------------------------------------------------------- LCP */

static void ipcp_lower_up(ppp_t *ppp);
static void link_ready(ppp_t *ppp);
static void chap_send_challenge(ppp_t *ppp);

static void
lcp_reset(ppp_t *ppp)
{
    ppp->want_magic    = 1;
    ppp->want_accm     = 1;
    ppp->want_accm_val = 0;
    ppp->tried_aps     = 0;
    ppp->auth_refused  = 0;
    ppp->want_ap       = (ppp->cfg.auth != PPP_AUTH_NONE) ? next_ap(ppp) : -1;
    ppp->want_mrru     = 0;
    ppp->peer_mru      = PPP_DEFAULT_MRU;
    ppp->peer_accm     = PPP_ACCM_ALL;
    ppp->peer_magic    = 0;
    ppp->peer_mrru     = 0;
    ppp->peer_ssn      = 0;
    ppp->peer_ed_len   = 0;
    next_magic(ppp);
}

static size_t
lcp_build_req(ppp_t *ppp, uint8_t *out)
{
    opt_out_t o = { out, 0 };

    if (ppp->want_accm)
        opt_add32(&o, LCP_ACCM, ppp->want_accm_val);
    if (ppp->want_ap >= 0) {
        uint8_t a[3];

        opt_add(&o, LCP_AUTH, a, auth_option(ppp->want_ap, a));
    }
    if (ppp->want_magic)
        opt_add32(&o, LCP_MAGIC, ppp->magic);
    if (ppp->want_mrru) {
        const uint8_t m[2] = { BUNDLE_MRRU >> 8, BUNDLE_MRRU & 0xff };

        opt_add(&o, LCP_MRRU, m, 2);
    }
    return o.len;
}

/* The guest will not do what we asked: the next method, or none. */
static void
lcp_auth_turned_down(ppp_t *ppp, int suggested)
{
    if (ppp->want_ap < 0)
        return;
    ppp->tried_aps |= 1u << ppp->want_ap;
    if (ap_usable(ppp, suggested) && !(ppp->tried_aps & (1u << suggested)))
        ppp->want_ap = suggested;
    else
        ppp->want_ap = next_ap(ppp);
    if (ppp->want_ap >= 0)
        plog(ppp, "LCP: the guest wants another method; asking for %s", ppp_ap_name(ppp->want_ap));
    else if (ppp->cfg.auth == PPP_AUTH_ANY)
        plog(ppp, "LCP: the guest will not authenticate as asked; continuing without");
    else
        ppp->auth_refused = 1;
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
            case LCP_AUTH: {
                int suggested = -1;

                if ((olen == 2) && (get16(data) == PPP_PROTO_PAP))
                    suggested = PPP_AP_PAP;
                else if ((olen == 3) && (get16(data) == PPP_PROTO_CHAP))
                    suggested = ppp_ap_from_chap(data[2]);
                lcp_auth_turned_down(ppp, suggested);
                break;
            }
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
                if (ppp->want_ap >= 0) {
                    if (ppp->cfg.auth == PPP_AUTH_ANY)
                        plog(ppp, "LCP: the guest refuses to authenticate; continuing without");
                    else
                        ppp->auth_refused = 1;
                }
                ppp->want_ap = -1;
                break;
            case LCP_MAGIC:
                ppp->want_magic = 0;
                break;
            case LCP_MRRU:
                ppp->want_mrru = 0;
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

    ppp->st_mrru   = 0;
    ppp->st_ssn    = 0;
    ppp->st_ed_len = 0;
    opt_sort_init(&s);
    while ((r = opt_next(&p, end, &type, &data, &olen)) > 0) {
        switch (type) {
            case LCP_MRU:
                if (olen != 2)
                    opt_reject(&s, type, data, olen);
                else if (get16(data) < PPP_MIN_MRU) {
                    const uint8_t min[2] = { PPP_MIN_MRU >> 8, PPP_MIN_MRU & 0xff };

                    opt_nak(&s, reject_naks, type, data, olen, min, 2);
                } else
                    mru = get16(data);
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
            case LCP_MRRU:
                if (!ppp->cfg.multilink || (olen != 2))
                    opt_reject(&s, type, data, olen);
                else {
                    ppp->st_mrru = get16(data);
                    if (ppp->st_mrru < PPP_MIN_MRU)
                        ppp->st_mrru = PPP_MIN_MRU;
                    /* It does Multilink: so do we, and say so. */
                    if (!ppp->want_mrru) {
                        ppp->want_mrru  = 1;
                        ppp->lcp.resend = 1;
                    }
                }
                break;
            case LCP_SSNHF:
                if (!ppp->cfg.multilink || (olen != 0))
                    opt_reject(&s, type, data, olen);
                else
                    ppp->st_ssn = 1;
                break;
            case LCP_ED:
                if (!ppp->cfg.multilink || (olen < 1) || (olen > 21))
                    opt_reject(&s, type, data, olen);
                else {
                    memcpy(ppp->st_ed, data, olen);
                    ppp->st_ed_len = (int) olen;
                }
                break;
            case LCP_AUTH:    /* we will not authenticate ourselves to the guest */
            case LCP_QUALITY: /* no link quality monitoring                    */
            case LCP_PFC:     /* no protocol field compression                 */
            case LCP_ACFC:    /* no address and control field compression      */
            default:          /* callback and anything else                    */
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
        ppp->peer_mru    = mru;
        ppp->peer_accm   = accm;
        ppp->peer_magic  = magic;
        ppp->peer_mrru   = ppp->st_mrru;
        ppp->peer_ssn    = ppp->st_ssn;
        ppp->peer_ed_len = ppp->st_ed_len;
        memcpy(ppp->peer_ed, ppp->st_ed, sizeof(ppp->peer_ed));
    }
    return r;
}

static void
lcp_up(ppp_t *ppp)
{
    ppp->tx_accm      = ppp->peer_accm;
    ppp->rx_accm      = ppp->want_accm ? ppp->want_accm_val : PPP_ACCM_ALL;
    ppp->was_open     = 1;
    ppp->why[0]       = '\0';
    ppp->echo_pending = 0;
    ppp->echo_at      = ppp->now + ppp->cfg.echo_interval_ms;
    ppp->mp           = ppp->cfg.multilink && (ppp->peer_mrru != 0) && ppp->want_mrru;
    ppp->auth_ap      = ppp->want_ap;
    ppp->authenticated = 0;
    ppp->auth_tries   = 0;
    plog(ppp, "LCP: up; guest MRU %u, ACCM %08X%s; %s%s", ppp->peer_mru, ppp->peer_accm,
         ppp->mp ? ", Multilink" : "", (ppp->auth_ap >= 0) ? "authenticating with " : "no authentication",
         (ppp->auth_ap >= 0) ? ppp_ap_name(ppp->auth_ap) : "");

    if (ppp->auth_refused) {
        close_later(ppp, "the guest will not authenticate with any method allowed");
        return;
    }
    if (ppp->auth_ap >= 0) {
        ppp->phase         = PPP_PHASE_AUTHENTICATE;
        ppp->auth_deadline = ppp->now + AUTH_TIMEOUT_MS;
        if (ppp->auth_ap != PPP_AP_PAP) {
            ppp->chap_sends = 0;
            chap_send_challenge(ppp);
        }
    } else
        link_ready(ppp);
}

static void
lcp_down(ppp_t *ppp)
{
    plog(ppp, "LCP: down");
    fsm_lower_down(ppp, &ppp->ccp);
    fsm_lower_down(ppp, &ppp->ipcp);
    ppp->phase         = PPP_PHASE_TERMINATE;
    ppp->tx_accm       = PPP_ACCM_ALL;
    ppp->rx_accm       = PPP_ACCM_ALL;
    ppp->authenticated = 0;
    ppp->chap_timer    = 0;
    ppp->key_kind      = 0;
    crypto_wipe(ppp->key_send, sizeof(ppp->key_send));
    crypto_wipe(ppp->key_recv, sizeof(ppp->key_recv));
    crypto_wipe(ppp->key_lm, sizeof(ppp->key_lm));
    if (ppp->mp_ready) {
        mp_rx_free(&ppp->mprx);
        ppp->mp_ready = 0;
    }
    ppp->mp = 0;
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
                const uint16_t proto = get16(data);

                plog(ppp, "LCP: the guest rejected protocol %04X", proto);
                if ((proto == PPP_PROTO_IPCP) || (proto == PPP_PROTO_IP)) {
                    /* Without IP there is nothing this ISP can do for it. */
                    close_later(ppp, "the guest rejected IP");
                } else if (proto == PPP_PROTO_CCP) {
                    fsm_lower_down(ppp, &ppp->ccp);
                    if (ppp->cfg.mppe == PPP_MPPE_REQUIRED)
                        close_later(ppp, "the guest will not encrypt (it does not do CCP)");
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

/* ------------------------------------------------------- authentication */

/* The account's password: the full name, then without "DOMAIN\". */
static int
get_secret(ppp_t *ppp, const char *user, char *secret, size_t len)
{
    if (ppp->cb->get_secret == NULL)
        return 0;
    if (ppp->cb->get_secret(ppp->opaque, user, secret, len))
        return 1;
    if (strcmp(mschap_user_part(user), user))
        return ppp->cb->get_secret(ppp->opaque, mschap_user_part(user), secret, len);
    return 0;
}

static void
auth_success(ppp_t *ppp, const char *user)
{
    snprintf(ppp->peer_user, sizeof(ppp->peer_user), "%.63s", user);
    if (ppp->authenticated)
        return;
    ppp->authenticated = 1;
    ppp->chap_timer    = 0;
    plog(ppp, "%s: \"%s\" let in%s", ppp_ap_name(ppp->auth_ap), user,
         ppp->key_kind ? " (encryption keys made)" : "");
    if (ppp->phase == PPP_PHASE_AUTHENTICATE)
        link_ready(ppp);
}

static void
auth_failure(ppp_t *ppp, const char *user, const char *why)
{
    plog(ppp, "%s: \"%s\" refused: %s", ppp_ap_name(ppp->auth_ap), user, why);
    snprintf(ppp->peer_user, sizeof(ppp->peer_user), "%.63s", user);
    if (++ppp->auth_tries >= AUTH_TRIES || (ppp->auth_ap != PPP_AP_PAP)) {
        snprintf(ppp->why, sizeof(ppp->why), "%s: \"%.40s\" refused", ppp_ap_name(ppp->auth_ap), user);
        close_later(ppp, "%s", ppp->why);
    }
}

static void
pap_input(ppp_t *ppp, const uint8_t *pkt, size_t len)
{
    static const char ok_msg[]  = "Welcome";
    static const char bad_msg[] = "Access denied";
    uint8_t           rep[4 + 1 + 32];
    size_t            plen;
    size_t            ulen;
    size_t            wlen;
    char              user[64];
    char              pass[256];
    char              secret[256];
    int               ok;
    const char       *msg;

    if ((len < 4) || (pkt[0] != 1) || (ppp->auth_ap != PPP_AP_PAP))
        return; /* only Authenticate-Request comes this way, and only if PAP was agreed */
    plen = get16(&pkt[2]);
    if ((plen < 6) || (plen > len))
        return;
    ulen = pkt[4];
    if ((5 + ulen + 1) > plen)
        return;
    wlen = pkt[5 + ulen];
    if ((6 + ulen + wlen) > plen)
        return;

    /* The name goes in the log; the password never does. */
    printable(&pkt[5], ulen, user, sizeof(user));
    memcpy(pass, &pkt[6 + ulen], wlen);
    pass[wlen] = '\0';

    if (ppp->cfg.auth == PPP_AUTH_ANY)
        ok = 1;
    else
        ok = get_secret(ppp, user, secret, sizeof(secret)) && (strlen(secret) == wlen) &&
             crypto_equal(secret, pass, wlen);
    crypto_wipe(pass, sizeof(pass));
    crypto_wipe(secret, sizeof(secret));

    msg    = ok ? ok_msg : bad_msg;
    rep[0] = ok ? 2 : 3; /* Authenticate-Ack or -Nak */
    rep[1] = pkt[1];
    rep[2] = 0;
    rep[3] = (uint8_t) (5 + strlen(msg));
    rep[4] = (uint8_t) strlen(msg);
    memcpy(&rep[5], msg, strlen(msg));
    ppp->cb->send(ppp->opaque, PPP_PROTO_PAP, rep, 5 + strlen(msg), ppp->tx_accm);

    if (ok)
        auth_success(ppp, user);
    else
        auth_failure(ppp, user, "wrong name or password");
}

static void
chap_send_challenge(ppp_t *ppp)
{
    uint8_t      pkt[4 + 1 + 16 + sizeof(ISP_NAME)];
    const size_t nlen = sizeof(ISP_NAME) - 1;

    if (ppp->chap_sends == 0) {
        ppp->chap_id++;
        ppp->chap_clen = (ppp->auth_ap == PPP_AP_MSCHAP1) ? 8 : 16;
        if (!crypto_random(ppp->chap_challenge, ppp->chap_clen)) {
            for (size_t i = 0; i < ppp->chap_clen; i++)
                ppp->chap_challenge[i] = (uint8_t) (next_magic(ppp) >> 13);
        }
    }
    pkt[0] = CHAP_CHALLENGE;
    pkt[1] = ppp->chap_id;
    pkt[2] = 0;
    pkt[3] = (uint8_t) (5 + ppp->chap_clen + nlen);
    pkt[4] = (uint8_t) ppp->chap_clen;
    memcpy(&pkt[5], ppp->chap_challenge, ppp->chap_clen);
    memcpy(&pkt[5 + ppp->chap_clen], ISP_NAME, nlen);
    ppp->cb->send(ppp->opaque, PPP_PROTO_CHAP, pkt, 5 + ppp->chap_clen + nlen, ppp->tx_accm);
    ppp->chap_sends++;
    ppp->chap_timer = 1;
    ppp->chap_at    = ppp->now + CHAP_RETRY_MS;
}

static void
chap_reply(ppp_t *ppp, int code, uint8_t id, const char *msg)
{
    uint8_t      pkt[4 + 128];
    const size_t n = strlen(msg) < 128 ? strlen(msg) : 128;

    pkt[0] = (uint8_t) code;
    pkt[1] = id;
    pkt[2] = 0;
    pkt[3] = (uint8_t) (4 + n);
    memcpy(&pkt[4], msg, n);
    ppp->cb->send(ppp->opaque, PPP_PROTO_CHAP, pkt, 4 + n, ppp->tx_accm);
}

/* A Response: checked against the account, the keys made if MS-CHAP. */
static void
chap_response(ppp_t *ppp, uint8_t id, const uint8_t *value, size_t vlen, const char *name)
{
    char      secret[256];
    const int have = get_secret(ppp, name, secret, sizeof(secret));
    int       ok   = 0;
    char      success[96];

    snprintf(success, sizeof(success), "Welcome");
    if (!have) {
        /* Nobody's: let in only where anyone may come, and MS-CHAP-2 never
           (it would want proof that we know the password). */
        ok = (ppp->cfg.auth == PPP_AUTH_ANY) && (ppp->auth_ap != PPP_AP_MSCHAP2);
    } else if (ppp->auth_ap == PPP_AP_MSCHAP1) {
        uint8_t ph[16];
        uint8_t want[24];

        if (vlen == 49) {
            if (value[48] & 1) {
                nt_password_hash(secret, ph);
                challenge_response(ppp->chap_challenge, ph, want);
                ok = crypto_equal(want, &value[24], 24);
            } else {
                lm_password_hash(secret, ph);
                challenge_response(ppp->chap_challenge, ph, want);
                ok = crypto_equal(want, &value[0], 24);
            }
        }
        if (ok) {
            mppe_mschap1_start_key(secret, ppp->chap_challenge, 128, ppp->key_send, ppp->key_recv);
            mppe_mschap1_start_key(secret, ppp->chap_challenge, 40, ppp->key_lm, ph);
            ppp->key_kind = 1;
        }
        crypto_wipe(ph, sizeof(ph));
    } else if (ppp->auth_ap == PPP_AP_MSCHAP2) {
        uint8_t want[24];

        if ((vlen == 49) && (ppp->chap_clen == 16)) {
            mschap2_nt_response(ppp->chap_challenge, &value[0], name, secret, want);
            ok = crypto_equal(want, &value[24], 24);
        }
        if (ok) {
            char auth[43];

            mschap2_authenticator_response(secret, &value[24], &value[0], ppp->chap_challenge, name, auth);
            snprintf(success, sizeof(success), "%s M=Welcome", auth);
            mppe_mschap2_start_keys(secret, &value[24], 1, ppp->key_send, ppp->key_recv);
            ppp->key_kind = 2;
        }
    } else {
        uint8_t      want[HASH_MAX];
        const size_t n = chap_digest(ppp_ap_chap_alg(ppp->auth_ap), id, secret, ppp->chap_challenge, ppp->chap_clen,
                                     want);

        ok = (n > 0) && (vlen == n) && crypto_equal(want, value, n);
    }
    crypto_wipe(secret, sizeof(secret));

    if (ok) {
        snprintf(ppp->chap_success, sizeof(ppp->chap_success), "%s", success);
        ppp->chap_last_resp_id = id;
        chap_reply(ppp, CHAP_SUCCESS, id, success);
        auth_success(ppp, name);
        return;
    }
    if (ppp->auth_ap == PPP_AP_MSCHAP2) {
        char msg[96];
        char hex[33];

        hex_encode(ppp->chap_challenge, 16, hex);
        for (char *h = hex; *h; h++)
            if ((*h >= 'a') && (*h <= 'f'))
                *h = (char) (*h - 'a' + 'A');
        snprintf(msg, sizeof(msg), "E=691 R=0 C=%s V=3 M=Access denied", hex);
        chap_reply(ppp, CHAP_FAILURE, id, msg);
    } else if (ppp->auth_ap == PPP_AP_MSCHAP1)
        chap_reply(ppp, CHAP_FAILURE, id, "E=691 R=0 V=2");
    else
        chap_reply(ppp, CHAP_FAILURE, id, "Access denied");
    auth_failure(ppp, name, have ? "wrong password" : "no such account");
}

static void
chap_input(ppp_t *ppp, const uint8_t *pkt, size_t len)
{
    size_t plen;
    size_t vlen;
    char   name[128];

    if ((len < 4) || (pkt[0] != CHAP_RESPONSE) || (ppp->auth_ap < 0) || (ppp->auth_ap == PPP_AP_PAP))
        return;
    plen = get16(&pkt[2]);
    if ((plen < 5) || (plen > len))
        return;
    vlen = pkt[4];
    if ((5 + vlen) > plen)
        return;
    printable(&pkt[5 + vlen], plen - 5 - vlen, name, sizeof(name));

    if (ppp->authenticated) {
        /* Our Success was lost: say it again. */
        if (pkt[1] == ppp->chap_last_resp_id)
            chap_reply(ppp, CHAP_SUCCESS, pkt[1], ppp->chap_success);
        return;
    }
    if ((ppp->phase != PPP_PHASE_AUTHENTICATE) || (pkt[1] != ppp->chap_id))
        return; /* not an answer to our challenge */
    ppp->chap_timer = 0;
    chap_response(ppp, pkt[1], &pkt[5], vlen, name);
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
            case IPCP_DNS2:
            case IPCP_NBNS1:
            case IPCP_NBNS2: {
                const int      second = (type == IPCP_DNS2) || (type == IPCP_NBNS2);
                const uint32_t val    = ((type == IPCP_DNS1) || (type == IPCP_DNS2)) ? ppp->cfg.dns[second]
                                                                                      : ppp->cfg.wins[second];

                if ((olen != 4) || (val == 0))
                    opt_reject(&s, type, data, olen);
                else if (get32(data) != val)
                    opt_nak32(&s, reject_naks, type, data, olen, val);
                break;
            }
            case IPCP_COMP:   /* Van Jacobson: not implemented */
            case IPCP_ADDRS:  /* the deprecated pair          */
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
    char wins[24];

    fmt_ip(peer, sizeof(peer), ppp->cfg.peer_ip);
    fmt_ip(local, sizeof(local), ppp->local_ip);
    fmt_ip(dns, sizeof(dns), ppp->cfg.dns[0]);
    wins[0] = '\0';
    if (ppp->cfg.wins[0]) {
        char w[16];

        fmt_ip(w, sizeof(w), ppp->cfg.wins[0]);
        snprintf(wins, sizeof(wins), ", WINS %s", w);
    }
    plog(ppp, "IPCP: up; guest %s, gateway %s, DNS %s%s", peer, local, dns, wins);
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
    if (ppp->lcp.state == FSM_OPENED)
        close_later(ppp, "IPCP failed");
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

/* -------------------------------------------------------------------- CCP */

/* The MPPE strengths the keys allow and the configuration permits. */
static uint32_t
mppe_strengths(const ppp_t *ppp)
{
    if ((ppp->cfg.mppe == PPP_MPPE_OFF) || (ppp->key_kind == 0))
        return 0;
    return ppp->cfg.mppe_bits & MPPX_STRENGTHS;
}

static uint32_t
strongest(uint32_t bits)
{
    if (bits & MPPX_S)
        return MPPX_S;
    if (bits & MPPX_M)
        return MPPX_M;
    if (bits & MPPX_L)
        return MPPX_L;
    return 0;
}

static int
ccp_enabled(const ppp_t *ppp)
{
    return (ppp->cfg.compression != 0) || (mppe_strengths(ppp) != 0) || (ppp->cfg.mppe == PPP_MPPE_REQUIRED);
}

static int
comp_on(const ppp_t *ppp, int c)
{
    /* With MPPE required, nothing but MPPE (and MPPC with it). */
    if ((ppp->cfg.mppe == PPP_MPPE_REQUIRED) && (c != PPP_COMP_MPPC))
        return 0;
    return !!(ppp->cfg.compression & (1u << c));
}

/* Our offer, from the first that is on. */
static void
ccp_next_offer(ppp_t *ppp)
{
    while (ppp->ccp_offer < OFFER_NONE) {
        switch (ppp->ccp_offer) {
            case OFFER_MPPX:
                ppp->want_mppx = 0;
                if (mppe_strengths(ppp)) {
                    ppp->want_mppx = mppe_strengths(ppp);
                    if (ppp->cfg.mppe_bits & MPPX_H)
                        ppp->want_mppx |= MPPX_H;
                }
                if (comp_on(ppp, PPP_COMP_MPPC))
                    ppp->want_mppx |= MPPX_C;
                if (ppp->want_mppx)
                    return;
                break;
            case OFFER_DEFLATE:
                ppp->want_deflate = 15;
                if (comp_on(ppp, PPP_COMP_DEFLATE))
                    return;
                break;
            case OFFER_BSD:
                ppp->want_bsd = 15;
                if (comp_on(ppp, PPP_COMP_BSD))
                    return;
                break;
            case OFFER_PRED1:
                if (comp_on(ppp, PPP_COMP_PRED1))
                    return;
                break;
            default:
                break;
        }
        ppp->ccp_offer++;
    }
}

static void
ccp_reset(ppp_t *ppp)
{
    ppp->ccp_offer = OFFER_MPPX;
    ppp->tx_set    = 0;
    ccp_next_offer(ppp);
}

static size_t
ccp_build_req(ppp_t *ppp, uint8_t *out)
{
    opt_out_t o = { out, 0 };

    switch (ppp->ccp_offer) {
        case OFFER_MPPX:
            opt_add32(&o, CCP_OPT_MPPX, ppp->want_mppx);
            break;
        case OFFER_DEFLATE: {
            const uint8_t d[2] = { (uint8_t) (((ppp->want_deflate - 8) << 4) | 8), 0 };

            opt_add(&o, CCP_OPT_DEFLATE, d, 2);
            break;
        }
        case OFFER_BSD: {
            const uint8_t b = (uint8_t) ((1 << 5) | ppp->want_bsd);

            opt_add(&o, CCP_OPT_BSD, &b, 1);
            break;
        }
        case OFFER_PRED1:
            opt_add(&o, CCP_OPT_PRED1, NULL, 0);
            break;
        default:
            break;
    }
    return o.len;
}

static void
ccp_offer_refused(ppp_t *ppp)
{
    if ((ppp->ccp_offer == OFFER_MPPX) && (ppp->cfg.mppe == PPP_MPPE_REQUIRED)) {
        close_later(ppp, "the guest will not encrypt");
        return;
    }
    ppp->ccp_offer++;
    ccp_next_offer(ppp);
}

static void
ccp_got_nak(ppp_t *ppp, const uint8_t *opt, size_t len)
{
    const uint8_t *p   = opt;
    const uint8_t *end = opt + len;
    const uint8_t *data;
    size_t         olen;
    int            type;

    while (opt_next(&p, end, &type, &data, &olen) > 0) {
        if ((type == CCP_OPT_MPPX) && (ppp->ccp_offer == OFFER_MPPX) && (olen == 4)) {
            const uint32_t allowed = mppe_strengths(ppp) | (ppp->cfg.mppe_bits & MPPX_H) |
                                     (comp_on(ppp, PPP_COMP_MPPC) ? MPPX_C : 0);
            uint32_t       want    = get32(data) & allowed;

            want = (want & ~MPPX_STRENGTHS) | strongest(want);
            if (!(want & (MPPX_C | MPPX_STRENGTHS)) ||
                ((ppp->cfg.mppe == PPP_MPPE_REQUIRED) && !(want & MPPX_STRENGTHS)))
                ccp_offer_refused(ppp);
            else
                ppp->want_mppx = want;
        } else if (((type == CCP_OPT_DEFLATE) || (type == CCP_OPT_DEFLATE_DRAFT)) &&
                   (ppp->ccp_offer == OFFER_DEFLATE) && (olen == 2)) {
            const int w = (data[0] >> 4) + 8;

            if ((w >= 9) && (w <= 15) && ((data[0] & 0x0f) == 8))
                ppp->want_deflate = w;
            else
                ccp_offer_refused(ppp);
        } else if ((type == CCP_OPT_BSD) && (ppp->ccp_offer == OFFER_BSD) && (olen == 1)) {
            const int b = data[0] & 0x1f;

            if (((data[0] >> 5) == 1) && (b >= 9) && (b <= 15))
                ppp->want_bsd = b;
            else
                ccp_offer_refused(ppp);
        }
    }
}

static void
ccp_got_rej(ppp_t *ppp, const uint8_t *opt, size_t len)
{
    (void) opt;
    if (len > 0)
        ccp_offer_refused(ppp);
}

/* The peer's request: what it can decompress, our compressor.  The first
   method we can do is taken; the rest are turned down. */
static int
ccp_got_req(ppp_t *ppp, const uint8_t *opt, size_t len, uint8_t *reply, size_t *reply_len, int reject_naks)
{
    const uint8_t *p   = opt;
    const uint8_t *end = opt + len;
    const uint8_t *data;
    size_t         olen;
    int            type;
    int            r;
    int            chosen = 0;
    int            saw_mppx = 0;
    opt_sort_t     s;

    opt_sort_init(&s);
    memset(&ppp->st_tx, 0, sizeof(ppp->st_tx));
    ppp->st_tx_set = 0;
    while ((r = opt_next(&p, end, &type, &data, &olen)) > 0) {
        if (chosen) {
            opt_reject(&s, type, data, olen);
            continue;
        }
        switch (type) {
            case CCP_OPT_MPPX: {
                const uint32_t b        = (olen == 4) ? get32(data) : 0;
                const uint32_t possible = mppe_strengths(ppp);
                uint32_t       want     = 0;

                saw_mppx = 1;
                if (olen != 4) {
                    opt_reject(&s, type, data, olen);
                    break;
                }
                if ((b & MPPX_C) && comp_on(ppp, PPP_COMP_MPPC))
                    want |= MPPX_C;
                if (b & possible)
                    want |= strongest(b & possible);
                else if ((ppp->cfg.mppe == PPP_MPPE_REQUIRED) && possible)
                    want |= strongest(possible); /* ask it to encrypt */
                if ((b & MPPX_H) && (want & (MPPX_C | MPPX_STRENGTHS)) && (ppp->cfg.mppe_bits & MPPX_H))
                    want |= MPPX_H;
                if (!(want & (MPPX_C | MPPX_STRENGTHS)) ||
                    ((ppp->cfg.mppe == PPP_MPPE_REQUIRED) && !(want & MPPX_STRENGTHS)))
                    opt_reject(&s, type, data, olen);
                else if (want == b) {
                    chosen               = 1;
                    ppp->st_tx.type      = CCP_OPT_MPPX;
                    ppp->st_tx.mppx      = want;
                    ppp->st_tx_set       = 1;
                } else {
                    opt_nak32(&s, reject_naks, type, data, olen, want);
                    chosen = 1;
                }
                break;
            }
            case CCP_OPT_DEFLATE:
            case CCP_OPT_DEFLATE_DRAFT: {
                const int w = (olen == 2) ? (data[0] >> 4) + 8 : 0;

                if (!comp_on(ppp, PPP_COMP_DEFLATE) || (olen != 2) || ((data[0] & 0x0f) != 8) || data[1] || (w < 9))
                    opt_reject(&s, type, data, olen);
                else if (w > 15) {
                    const uint8_t d[2] = { (15 - 8) << 4 | 8, 0 };

                    opt_nak(&s, reject_naks, type, data, olen, d, 2);
                    chosen = 1;
                } else {
                    chosen          = 1;
                    ppp->st_tx.type = type;
                    ppp->st_tx.bits = w;
                    ppp->st_tx_set  = 1;
                }
                break;
            }
            case CCP_OPT_BSD: {
                const int b = (olen == 1) ? (data[0] & 0x1f) : 0;

                if (!comp_on(ppp, PPP_COMP_BSD) || (olen != 1) || ((data[0] >> 5) != 1) || (b < 9))
                    opt_reject(&s, type, data, olen);
                else if (b > 15) {
                    const uint8_t v = (1 << 5) | 15;

                    opt_nak(&s, reject_naks, type, data, olen, &v, 1);
                    chosen = 1;
                } else {
                    chosen          = 1;
                    ppp->st_tx.type = CCP_OPT_BSD;
                    ppp->st_tx.bits = b;
                    ppp->st_tx_set  = 1;
                }
                break;
            }
            case CCP_OPT_PRED1:
                if (!comp_on(ppp, PPP_COMP_PRED1) || (olen != 0))
                    opt_reject(&s, type, data, olen);
                else {
                    chosen          = 1;
                    ppp->st_tx.type = CCP_OPT_PRED1;
                    ppp->st_tx_set  = 1;
                }
                break;
            default:
                opt_reject(&s, type, data, olen);
                break;
        }
    }
    if (r < 0)
        opt_reject(&s, p[0], p + 2, (size_t) (end - p) >= 2 ? (size_t) (end - p) - 2 : 0);

    /* MPPE required and not asked for: ask for it. */
    if ((ppp->cfg.mppe == PPP_MPPE_REQUIRED) && !saw_mppx && mppe_strengths(ppp) && !reject_naks) {
        uint32_t want = strongest(mppe_strengths(ppp));

        if (ppp->cfg.mppe_bits & MPPX_H)
            want |= MPPX_H;
        opt_add32(&s.naks, CCP_OPT_MPPX, want);
    }

    r = opt_sort_reply(&s, opt, len, reply, reply_len);
    if (r == PPP_CONF_ACK) {
        ppp->tx_params = ppp->st_tx;
        ppp->tx_set    = ppp->st_tx_set;
    }
    return r;
}

static void
comp_key(const ppp_t *ppp, uint32_t mppx, int send, uint8_t *key)
{
    memset(key, 0, 16);
    if (!(mppx & MPPX_STRENGTHS))
        return;
    if ((ppp->key_kind == 1) && !(mppx & MPPX_S))
        memcpy(key, ppp->key_lm, 16); /* MS-CHAP's 40 and 56 bits: the LAN Manager hash */
    else
        memcpy(key, send ? ppp->key_send : ppp->key_recv, 16);
}

static void
ccp_up(ppp_t *ppp)
{
    ppp_comp_params_t rx;
    const size_t      mru = ppp->mp ? BUNDLE_MRRU : PPP_DEFAULT_MRU;
    char              d[160];

    ppp_comp_free(ppp->comp_tx);
    ppp_comp_free(ppp->comp_rx);
    ppp->comp_tx = ppp->comp_rx = NULL;

    if (ppp->tx_set) {
        comp_key(ppp, ppp->tx_params.mppx, 1, ppp->tx_params.key);
        ppp->comp_tx = ppp_comp_new(&ppp->tx_params, 1, ppp->peer_mrru ? ppp->peer_mrru : ppp->peer_mru);
    }
    memset(&rx, 0, sizeof(rx));
    switch (ppp->ccp_offer) {
        case OFFER_MPPX:
            rx.type = CCP_OPT_MPPX;
            rx.mppx = ppp->want_mppx;
            break;
        case OFFER_DEFLATE:
            rx.type = CCP_OPT_DEFLATE;
            rx.bits = ppp->want_deflate;
            break;
        case OFFER_BSD:
            rx.type = CCP_OPT_BSD;
            rx.bits = ppp->want_bsd;
            break;
        case OFFER_PRED1:
            rx.type = CCP_OPT_PRED1;
            break;
        default:
            break;
    }
    if (rx.type != 0) {
        comp_key(ppp, rx.mppx, 0, rx.key);
        ppp->comp_rx = ppp_comp_new(&rx, 0, mru + 64);
    }
    crypto_wipe(&rx, sizeof(rx));
    crypto_wipe(ppp->tx_params.key, sizeof(ppp->tx_params.key));
    ppp->reset_sent = 0;

    if ((ppp->cfg.mppe == PPP_MPPE_REQUIRED) && !(ppp_comp_encrypts(ppp->comp_tx) && ppp_comp_encrypts(ppp->comp_rx))) {
        close_later(ppp, "the guest will not encrypt both ways");
        return;
    }
    ppp_ccp_describe(ppp, d, sizeof(d));
    plog(ppp, "CCP: up; %s", d[0] ? d : "no compression either way");
}

static void
ccp_down(ppp_t *ppp)
{
    ppp_comp_free(ppp->comp_tx);
    ppp_comp_free(ppp->comp_rx);
    ppp->comp_tx = ppp->comp_rx = NULL;
    plog(ppp, "CCP: down");
}

static void
ccp_finished(ppp_t *ppp)
{
    if ((ppp->cfg.mppe == PPP_MPPE_REQUIRED) && (ppp->lcp.state == FSM_OPENED))
        close_later(ppp, "CCP failed: no encryption");
}

static int
ccp_extcode(ppp_t *ppp, int code, int id, const uint8_t *data, size_t len)
{
    (void) data;
    (void) len;
    switch (code) {
        case CCP_RESET_REQ:
            if (ppp->ccp.state == FSM_OPENED) {
                ppp_comp_reset(ppp->comp_tx);
                fsm_send(ppp, &ppp->ccp, CCP_RESET_ACK, id, NULL, 0);
            }
            return 1;
        case CCP_RESET_ACK:
            if ((ppp->ccp.state == FSM_OPENED) && ppp->reset_sent && ((uint8_t) id == ppp->reset_id)) {
                ppp_comp_reset(ppp->comp_rx);
                ppp->reset_sent = 0;
            }
            return 1;
        default:
            return 0;
    }
}

static const ppp_fsm_ops_t ccp_ops = {
    .reset     = ccp_reset,
    .build_req = ccp_build_req,
    .got_nak   = ccp_got_nak,
    .got_rej   = ccp_got_rej,
    .got_req   = ccp_got_req,
    .up        = ccp_up,
    .down      = ccp_down,
    .finished  = ccp_finished,
    .extcode   = ccp_extcode
};

/* Our decompressor lost its place: ask the guest's compressor to reset. */
static void
ccp_ask_reset(ppp_t *ppp)
{
    if (ppp->reset_sent && !timer_due(ppp->now, ppp->reset_at + RESET_RETRY_MS))
        return;
    if (!ppp->reset_sent)
        ppp->reset_id = ppp->ccp.id++;
    ppp->reset_sent = 1;
    ppp->reset_at   = ppp->now;
    fsm_send(ppp, &ppp->ccp, CCP_RESET_REQ, ppp->reset_id, NULL, 0);
}

/* ------------------------------------------------------- the network layer */

/* Authentication done (or none asked for). */
static void
link_ready(ppp_t *ppp)
{
    ppp->phase = PPP_PHASE_NETWORK;
    if (ppp->mp && (ppp->cb->link_ready != NULL) && ppp->cb->link_ready(ppp->opaque)) {
        ppp->member = 1;
        plog(ppp, "Multilink: this link joined a bundle");
        return;
    }
    ppp->member = 0;
    ppp->links  = 1;
    if (ppp->mp) {
        mp_rx_init(&ppp->mprx, 0, BUNDLE_MRRU);
        mp_tx_init(&ppp->mptx, ppp->peer_ssn);
        ppp->mp_ready = 1;
    }
    ipcp_lower_up(ppp);
    if (ccp_enabled(ppp)) {
        ppp->ccp.passive = (ppp->cfg.mppe != PPP_MPPE_REQUIRED);
        fsm_lower_up(ppp, &ppp->ccp);
    }
}

static int
link_count(uint32_t links)
{
    int n = 0;

    for (; links; links &= links - 1)
        n++;
    return n;
}

/* MP fragment `i` of the bundle's active links. */
static void
mp_emit(void *opaque, int i, const uint8_t *frag, size_t len)
{
    ppp_t *ppp  = (ppp_t *) opaque;
    int    slot = 0;

    for (int s = 0, k = 0; s < 32; s++) {
        if (!(ppp->links & (1u << s)))
            continue;
        if (k++ == i) {
            slot = s;
            break;
        }
    }
    if (slot == 0)
        ppp->cb->send(ppp->opaque, PPP_PROTO_MP, frag, len, ppp->tx_accm);
    else if (ppp->cb->link_send != NULL)
        ppp->cb->link_send(ppp->opaque, slot, PPP_PROTO_MP, frag, len);
}

/* A packet for the bundle: compressed (and encrypted), then on this link,
   or in MP fragments over all of them. */
static void
bundle_send(ppp_t *ppp, uint16_t proto, const uint8_t *info, size_t len)
{
    uint8_t cbuf[4096];

    if ((ppp->comp_tx != NULL) && (ppp->ccp.state == FSM_OPENED) && ppp_comp_applies(ppp->comp_tx, proto)) {
        const size_t n = ppp_comp_compress(ppp->comp_tx, proto, info, len, cbuf, sizeof(cbuf));

        if (n > 0) {
            proto = PPP_PROTO_COMP;
            info  = cbuf;
            len   = n;
        }
    }
    if (ppp->mp_ready && (link_count(ppp->links) > 1)) {
        uint8_t pkt[4096 + 2];

        if (len > (sizeof(pkt) - 2))
            return;
        pkt[0] = (uint8_t) (proto >> 8);
        pkt[1] = (uint8_t) proto;
        memcpy(pkt + 2, info, len);
        mp_tx_send(&ppp->mptx, pkt, len + 2, link_count(ppp->links), mp_emit, ppp);
        return;
    }
    ppp->cb->send(ppp->opaque, proto, info, len, ppp->tx_accm);
}

static void
protocol_reject(ppp_t *ppp, uint16_t proto, const uint8_t *p, size_t n)
{
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
}

static int
mppe_gate(const ppp_t *ppp)
{
    /* Encryption agreed, or required: nothing goes in the clear. */
    return (ppp->cfg.mppe == PPP_MPPE_REQUIRED) || ppp_comp_encrypts(ppp->comp_rx);
}

static void
deliver_ip(ppp_t *ppp, const uint8_t *p, size_t n)
{
    if (ppp->ipcp.state != FSM_OPENED)
        return;
    ppp->ip_in++;
    if (ppp->cb->ip_input != NULL)
        ppp->cb->ip_input(ppp->opaque, p, n);
}

static void bundle_dispatch(ppp_t *ppp, uint16_t proto, const uint8_t *p, size_t n);

static void
mp_deliver(void *opaque, const uint8_t *pkt, size_t len)
{
    ppp_t   *ppp = (ppp_t *) opaque;
    uint16_t proto;

    if ((len >= 2) && (pkt[0] == 0xff) && (pkt[1] == 0x03)) {
        pkt += 2;
        len -= 2;
    }
    if (len < 1)
        return;
    if (pkt[0] & 1) {
        proto = pkt[0];
        pkt++;
        len--;
    } else {
        if ((len < 2) || !(pkt[1] & 1))
            return;
        proto = get16(pkt);
        pkt += 2;
        len -= 2;
    }
    bundle_dispatch(ppp, proto, pkt, len);
}

/* A packet of the bundle, from any of its links. */
static void
bundle_dispatch(ppp_t *ppp, uint16_t proto, const uint8_t *p, size_t n)
{
    switch (proto) {
        case PPP_PROTO_IPCP:
            fsm_input(ppp, &ppp->ipcp, p, n);
            break;
        case PPP_PROTO_CCP:
            if (ccp_enabled(ppp))
                fsm_input(ppp, &ppp->ccp, p, n);
            else
                protocol_reject(ppp, proto, p, n);
            break;
        case PPP_PROTO_COMP:
            if ((ppp->comp_rx != NULL) && (ppp->ccp.state == FSM_OPENED)) {
                uint8_t  out[4096];
                uint16_t inner = 0;
                const int r    = ppp_comp_decompress(ppp->comp_rx, p, n, &inner, out, sizeof(out));

                if (r >= 0) {
                    if (inner == PPP_PROTO_IP)
                        deliver_ip(ppp, out, (size_t) r);
                } else if (r == COMP_RESET)
                    ccp_ask_reset(ppp);
                else if (r == COMP_FAIL) {
                    plog(ppp, "CCP: %s lost its place; negotiating again", ppp_comp_describe(ppp->comp_rx));
                    fsm_restart(ppp, &ppp->ccp);
                }
            }
            break;
        case PPP_PROTO_IP:
            if (mppe_gate(ppp)) {
                ppp->ip_dropped++;
                break;
            }
            ppp_comp_incomp(ppp->comp_rx, proto, p, n);
            deliver_ip(ppp, p, n);
            break;
        case PPP_PROTO_MP:
            if (ppp->mp_ready)
                mp_rx_input(&ppp->mprx, 0, p, n, mp_deliver, ppp);
            else
                protocol_reject(ppp, proto, p, n);
            break;
        case PPP_PROTO_LCP:
            break; /* not allowed on the bundle: ignored */
        default:
            /* IPXCP, NBFCP, IPV6CP, per-link CCP and the rest. */
            protocol_reject(ppp, proto, p, n);
            break;
    }
}

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
    ppp->auth_ap  = -1;
    ppp->chap_id  = (uint8_t) (cfg->magic_seed >> 16);
    if (ppp->cfg.echo_fails <= 0)
        ppp->cfg.echo_fails = 4;
    if (ppp->cfg.auth_protos == 0)
        ppp->cfg.auth_protos = 1u << PPP_AP_PAP;

    ppp->lcp.name     = "LCP";
    ppp->lcp.protocol = PPP_PROTO_LCP;
    ppp->lcp.ops      = &lcp_ops;
    ppp->lcp.passive  = 1;
    ppp->lcp.id       = (uint8_t) cfg->magic_seed;

    ppp->ipcp.name     = "IPCP";
    ppp->ipcp.protocol = PPP_PROTO_IPCP;
    ppp->ipcp.ops      = &ipcp_ops;
    ppp->ipcp.bundle   = 1;
    ppp->ipcp.id       = (uint8_t) (cfg->magic_seed >> 8);

    ppp->ccp.name     = "CCP";
    ppp->ccp.protocol = PPP_PROTO_CCP;
    ppp->ccp.ops      = &ccp_ops;
    ppp->ccp.bundle   = 1;
    ppp->ccp.passive  = 1;
    ppp->ccp.id       = (uint8_t) (cfg->magic_seed >> 24);

    lcp_reset(ppp);
    ipcp_reset(ppp);
    fsm_open(ppp, &ppp->lcp);
    fsm_open(ppp, &ppp->ipcp);
    fsm_open(ppp, &ppp->ccp);
}

void
ppp_free(ppp_t *ppp)
{
    ppp_comp_free(ppp->comp_tx);
    ppp_comp_free(ppp->comp_rx);
    ppp->comp_tx = ppp->comp_rx = NULL;
    if (ppp->mp_ready)
        mp_rx_free(&ppp->mprx);
    ppp->mp_ready = 0;
    crypto_wipe(ppp->key_send, sizeof(ppp->key_send));
    crypto_wipe(ppp->key_recv, sizeof(ppp->key_recv));
    crypto_wipe(ppp->key_lm, sizeof(ppp->key_lm));
}

void
ppp_lower_up(ppp_t *ppp, uint32_t now)
{
    ppp->now   = now;
    ppp->phase = PPP_PHASE_ESTABLISH;
    fsm_lower_up(ppp, &ppp->lcp);
}

/* A close a handler asked for, now that it has returned. */
static void
do_pending_close(ppp_t *ppp)
{
    if (ppp->close_why[0] == '\0')
        return;
    if ((ppp->why[0] == '\0') || strcmp(ppp->why, ppp->close_why))
        snprintf(ppp->why, sizeof(ppp->why), "%s", ppp->close_why);
    plog(ppp, "%s", ppp->why);
    ppp->close_why[0] = '\0';
    fsm_close(ppp, &ppp->lcp);
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
        proto = get16(p);
        p += 2;
        n -= 2;
    }

    if (proto == PPP_PROTO_LCP)
        fsm_input(ppp, &ppp->lcp, p, n);
    else if (ppp->lcp.state != FSM_OPENED)
        ; /* RFC 1661 3.4: only LCP before the link is open */
    else if (proto == PPP_PROTO_PAP)
        pap_input(ppp, p, n);
    else if (proto == PPP_PROTO_CHAP)
        chap_input(ppp, p, n);
    else if (ppp->phase != PPP_PHASE_NETWORK)
        ; /* the network layer waits for authentication */
    else if (ppp->member) {
        /* The bundle's: its owner has its IPCP, CCP and reassembly. */
        if (ppp->cb->to_bundle != NULL)
            ppp->cb->to_bundle(ppp->opaque, proto, p, n);
    } else
        bundle_dispatch(ppp, proto, p, n);

    do_pending_close(ppp);
}

void
ppp_bundle_input(ppp_t *ppp, int link, uint16_t proto, const uint8_t *data, size_t len, uint32_t now)
{
    ppp->now = now;
    if ((ppp->phase != PPP_PHASE_NETWORK) || ppp->member)
        return;
    if (proto == PPP_PROTO_MP) {
        if (ppp->mp_ready)
            mp_rx_input(&ppp->mprx, link, data, len, mp_deliver, ppp);
    } else
        bundle_dispatch(ppp, proto, data, len);
    do_pending_close(ppp);
}

void
ppp_bundle_links(ppp_t *ppp, uint32_t links)
{
    if (ppp->mp_ready)
        for (int s = 0; s < MP_MAX_LINKS; s++)
            if ((ppp->links & (1u << s)) && !(links & (1u << s)))
                mp_rx_link_gone(&ppp->mprx, s);
    ppp->links = links | 1u;
}

void
ppp_send_ip(ppp_t *ppp, const uint8_t *packet, size_t len)
{
    const uint32_t mru = (ppp->mp_ready && ppp->peer_mrru) ? ppp->peer_mrru : ppp->peer_mru;

    if ((ppp->ipcp.state != FSM_OPENED) || (len > mru)) {
        ppp->ip_dropped++;
        return;
    }
    /* Encryption required: nothing until CCP has it running. */
    if ((ppp->cfg.mppe == PPP_MPPE_REQUIRED) &&
        ((ppp->ccp.state != FSM_OPENED) || !ppp_comp_encrypts(ppp->comp_tx))) {
        ppp->ip_dropped++;
        return;
    }
    ppp->ip_out++;
    bundle_send(ppp, PPP_PROTO_IP, packet, len);
}

void
ppp_tick(ppp_t *ppp, uint32_t now)
{
    ppp_fsm_t *fsms[3] = { &ppp->lcp, &ppp->ipcp, &ppp->ccp };

    ppp->now = now;

    for (int i = 0; i < 3; i++) {
        if (fsms[i]->timer_on && timer_due(now, fsms[i]->timer_at)) {
            fsms[i]->timer_on = 0;
            fsm_timeout(ppp, fsms[i]);
        }
    }

    if (ppp->phase == PPP_PHASE_AUTHENTICATE) {
        if (timer_due(now, ppp->auth_deadline)) {
            snprintf(ppp->why, sizeof(ppp->why), "%s: the guest did not authenticate in %d s",
                     ppp_ap_name(ppp->auth_ap), AUTH_TIMEOUT_MS / 1000);
            close_later(ppp, "%s", ppp->why);
        } else if (ppp->chap_timer && timer_due(now, ppp->chap_at)) {
            if (ppp->chap_sends < CHAP_SENDS)
                chap_send_challenge(ppp);
            else
                ppp->chap_timer = 0;
        }
    }

    if (ppp->cfg.echo_interval_ms && (ppp->lcp.state == FSM_OPENED) && timer_due(now, ppp->echo_at)) {
        if (ppp->echo_pending >= ppp->cfg.echo_fails) {
            snprintf(ppp->why, sizeof(ppp->why), "LCP: %d echo requests unanswered", ppp->echo_pending);
            close_later(ppp, "%s", ppp->why);
        } else {
            uint8_t m[4];

            put32(m, ppp->want_magic ? ppp->magic : 0);
            fsm_send(ppp, &ppp->lcp, PPP_ECHO_REQ, ppp->lcp.id++, m, 4);
            ppp->echo_pending++;
            ppp->echo_at = now + ppp->cfg.echo_interval_ms;
        }
    }
    do_pending_close(ppp);
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
    if (ppp->ccp.timer_on)
        min_due(&best, now, ppp->ccp.timer_at);
    if (ppp->phase == PPP_PHASE_AUTHENTICATE) {
        min_due(&best, now, ppp->auth_deadline);
        if (ppp->chap_timer)
            min_due(&best, now, ppp->chap_at);
    }
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

const char *
ppp_auth_name(const ppp_t *ppp)
{
    return (ppp->authenticated && (ppp->auth_ap >= 0)) ? ppp_ap_name(ppp->auth_ap) : "";
}

void
ppp_ccp_describe(const ppp_t *ppp, char *buf, size_t len)
{
    const char *tx = ppp->comp_tx ? ppp_comp_describe(ppp->comp_tx) : NULL;
    const char *rx = ppp->comp_rx ? ppp_comp_describe(ppp->comp_rx) : NULL;

    if ((tx == NULL) && (rx == NULL))
        snprintf(buf, len, "%s", "");
    else if ((tx != NULL) && (rx != NULL) && !strcmp(tx, rx))
        snprintf(buf, len, "%s", tx);
    else
        snprintf(buf, len, "to the guest: %s; from the guest: %s", tx ? tx : "none", rx ? rx : "none");
}

int
ppp_encrypted(const ppp_t *ppp)
{
    return ppp_comp_encrypts(ppp->comp_tx) && ppp_comp_encrypts(ppp->comp_rx);
}
