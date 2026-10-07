/*
 * 86Box-Next: a scripted PPP client for the virtual ISP's tests.  See
 * ppp_client.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "isp_crypto.h"
#include "isp_plat.h"
#include "ppp_auth.h"
#include "ppp_framing.h"
#include "ppp_session.h"
#include "ppp_client.h"

#define LCP_ACCM     2
#define LCP_AUTH     3
#define LCP_MAGIC    5
#define LCP_PFC      7
#define LCP_ACFC     8
#define LCP_CALLBACK 13

#define IPCP_COMP  2
#define IPCP_ADDR  3
#define IPCP_DNS1  129
#define IPCP_NBNS1 130
#define IPCP_DNS2  131
#define IPCP_NBNS2 132

uint32_t
ppp_client_ms(void)
{
    return (uint32_t) isp_now_ms();
}

const char *
ppp_client_ip_str(uint32_t ip)
{
    static char buf[4][16];
    static int  n;
    char       *b = buf[n++ & 3];

    snprintf(b, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
    return b;
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
put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t) (v >> 8);
    p[1] = (uint8_t) v;
}

static void
line_write(ppp_client_t *c, const uint8_t *buf, size_t len)
{
    const uint32_t until = ppp_client_ms() + 5000;

    while (len > 0) {
        const size_t n = c->write(c->opaque, buf, len);

        buf += n;
        len -= n;
        if (len > 0) {
            if ((int32_t) (ppp_client_ms() - until) >= 0) {
                printf("    client: the line will not take %u more bytes\n", (unsigned) len);
                return;
            }
            c->idle(c->opaque);
        }
    }
}

void
ppp_client_send(ppp_client_t *c, uint16_t proto, const uint8_t *info, size_t len)
{
    uint8_t      enc[PPP_ENCODED_MAX(2048)];
    const size_t n = ppp_encode(PPP_ACCM_ALL, proto, info, len, enc, sizeof(enc));

    line_write(c, enc, n);
}

static void
send_ctl(ppp_client_t *c, uint16_t proto, int code, int id, const uint8_t *data, size_t len)
{
    uint8_t pkt[512];

    pkt[0] = (uint8_t) code;
    pkt[1] = (uint8_t) id;
    put16(&pkt[2], (uint16_t) (len + 4));
    memcpy(&pkt[4], data, len);
    ppp_client_send(c, proto, pkt, len + 4);
}

static int
was_rejected(const uint8_t *list, int n, int type)
{
    for (int i = 0; i < n; i++)
        if (list[i] == type)
            return 1;
    return 0;
}

static size_t
add_opt(uint8_t *p, int type, const uint8_t *data, size_t len)
{
    p[0] = (uint8_t) type;
    p[1] = (uint8_t) (len + 2);
    memcpy(&p[2], data, len);
    return len + 2;
}

static size_t
add_opt32(uint8_t *p, int type, uint32_t v)
{
    uint8_t b[4];

    put32(b, v);
    return add_opt(p, type, b, 4);
}

/* What Windows asks for: no escaping, a magic number, both compressions and
   Microsoft's callback.  Whatever the ISP rejects is left out next time. */
static void
send_lcp_req(ppp_client_t *c)
{
    uint8_t o[64];
    size_t  n = 0;

    if (!was_rejected(c->lcp_rejected, c->lcp_rejected_n, LCP_ACCM))
        n += add_opt32(&o[n], LCP_ACCM, 0);
    if (!was_rejected(c->lcp_rejected, c->lcp_rejected_n, LCP_MAGIC))
        n += add_opt32(&o[n], LCP_MAGIC, c->magic);
    if (!was_rejected(c->lcp_rejected, c->lcp_rejected_n, LCP_PFC))
        n += add_opt(&o[n], LCP_PFC, NULL, 0);
    if (!was_rejected(c->lcp_rejected, c->lcp_rejected_n, LCP_ACFC))
        n += add_opt(&o[n], LCP_ACFC, NULL, 0);
    if (!was_rejected(c->lcp_rejected, c->lcp_rejected_n, LCP_CALLBACK)) {
        const uint8_t cb = 6;

        n += add_opt(&o[n], LCP_CALLBACK, &cb, 1);
    }
    c->id++;
    send_ctl(c, PPP_PROTO_LCP, PPP_CONF_REQ, c->id, o, n);
    c->last_req_ms = ppp_client_ms();
}

/* Windows 98 again: an address of 0.0.0.0, VJ compression, DNS and WINS. */
static void
send_ipcp_req(ppp_client_t *c)
{
    uint8_t o[64];
    size_t  n = 0;

    n += add_opt32(&o[n], IPCP_ADDR, c->my_ip);
    if (!was_rejected(c->ipcp_rejected, c->ipcp_rejected_n, IPCP_COMP)) {
        const uint8_t vj[4] = { 0x00, 0x2d, 0x0f, 0x01 };

        n += add_opt(&o[n], IPCP_COMP, vj, 4);
    }
    if (!was_rejected(c->ipcp_rejected, c->ipcp_rejected_n, IPCP_DNS1))
        n += add_opt32(&o[n], IPCP_DNS1, c->dns);
    if (!was_rejected(c->ipcp_rejected, c->ipcp_rejected_n, IPCP_NBNS1))
        n += add_opt32(&o[n], IPCP_NBNS1, 0);
    if (!was_rejected(c->ipcp_rejected, c->ipcp_rejected_n, IPCP_DNS2))
        n += add_opt32(&o[n], IPCP_DNS2, c->dns2);
    if (!was_rejected(c->ipcp_rejected, c->ipcp_rejected_n, IPCP_NBNS2))
        n += add_opt32(&o[n], IPCP_NBNS2, 0);
    c->id++;
    send_ctl(c, PPP_PROTO_IPCP, PPP_CONF_REQ, c->id, o, n);
    c->last_req_ms = ppp_client_ms();
}

static void
send_pap(ppp_client_t *c)
{
    uint8_t     p[128];
    const char *u = c->user ? c->user : "anyone";
    const char *w = c->password ? c->password : "anything";
    size_t      n = 0;

    p[n++] = (uint8_t) strlen(u);
    memcpy(&p[n], u, strlen(u));
    n += strlen(u);
    p[n++] = (uint8_t) strlen(w);
    memcpy(&p[n], w, strlen(w));
    n += strlen(w);
    c->id++;
    send_ctl(c, PPP_PROTO_PAP, 1, c->id, p, n);
    c->last_req_ms = ppp_client_ms();
}

static void
lcp_maybe_open(ppp_client_t *c)
{
    if (c->lcp_opened || !c->lcp_ack_sent || !c->lcp_ack_rcvd)
        return;
    c->lcp_opened = 1;
    /* What it asked for: from now on the ISP sends control characters raw. */
    if (!was_rejected(c->lcp_rejected, c->lcp_rejected_n, LCP_ACCM))
        c->rx.accm = 0;
    if (c->verbose)
        printf("    client: LCP open%s\n", c->isp_wants_pap ? ", ISP asks for PAP" : "");
    if (c->isp_wants_pap)
        send_pap(c);
    else if (!c->isp_chap_alg)
        send_ipcp_req(c);
    /* (CHAP: the ISP's challenge comes next) */
}

/* The answer to a CHAP challenge, by whichever method the ISP asked for. */
static void
answer_chap(ppp_client_t *c, const uint8_t *p, size_t n)
{
    const char *u = c->user ? c->user : "anyone";
    const char *w = c->password ? c->password : "anything";
    uint8_t     pkt[300];
    uint8_t     value[64];
    size_t      vlen = 0;
    size_t      clen;
    size_t      len;

    if ((n < 5) || ((size_t) get16(&p[2]) > n))
        return;
    clen = p[4];
    if ((5 + clen) > n)
        return;
    if (c->isp_chap_alg == CHAP_MSCHAP1) {
        uint8_t ph[16];

        memset(value, 0, 49);
        nt_password_hash(w, ph);
        if (clen == 8)
            challenge_response(&p[5], ph, &value[24]);
        value[48] = 1; /* the NT response */
        vlen      = 49;
    } else if (c->isp_chap_alg == CHAP_MSCHAP2) {
        memset(value, 0, 49);
        crypto_random(value, 16); /* the peer challenge */
        if (clen == 16) {
            mschap2_nt_response(&p[5], value, u, w, &value[24]);
            mschap2_authenticator_response(w, &value[24], value, &p[5], u, c->chap_expect);
        }
        vlen = 49;
    } else
        vlen = chap_digest(c->isp_chap_alg, p[1], w, &p[5], clen, value);

    pkt[0] = 2;
    pkt[1] = p[1];
    pkt[4] = (uint8_t) vlen;
    memcpy(&pkt[5], value, vlen);
    memcpy(&pkt[5 + vlen], u, strlen(u));
    len    = 5 + vlen + strlen(u);
    pkt[2] = (uint8_t) (len >> 8);
    pkt[3] = (uint8_t) len;
    ppp_client_send(c, PPP_PROTO_CHAP, pkt, len);
    c->last_req_ms = ppp_client_ms();
}

static void
handle_chap(ppp_client_t *c, const uint8_t *p, size_t n)
{
    if (n < 4)
        return;
    switch (p[0]) {
        case 1:
            if (!c->chap_ok)
                answer_chap(c, p, n);
            break;
        case 3:
            if (c->chap_ok)
                break;
            if (c->isp_chap_alg == CHAP_MSCHAP2) {
                const size_t len = get16(&p[2]);

                /* The ISP has to prove it knows the password too. */
                if ((len < 4 + 42) || memcmp(&p[4], c->chap_expect, 42)) {
                    c->auth_failed = 1;
                    break;
                }
            }
            c->chap_ok = 1;
            send_ipcp_req(c);
            break;
        case 4:
            c->auth_failed = 1;
            break;
        default:
            break;
    }
}

static void
handle_lcp(ppp_client_t *c, const uint8_t *p, size_t n)
{
    size_t len;

    if (n < 4)
        return;
    len = get16(&p[2]);
    if ((len < 4) || (len > n))
        return;

    switch (p[0]) {
        case PPP_CONF_REQ: {
            const uint8_t *o   = &p[4];
            const uint8_t *end = p + len;

            memcpy(c->isp_lcp_req, &p[4], len - 4);
            c->isp_lcp_req_len = len - 4;
            c->isp_wants_pap   = 0;
            c->isp_chap_alg    = 0;
            while ((end - o) >= 2 && o[1] >= 2 && o[1] <= (end - o)) {
                if ((o[0] == LCP_AUTH) && (o[1] == 4) && (get16(&o[2]) == PPP_PROTO_PAP))
                    c->isp_wants_pap = 1;
                if ((o[0] == LCP_AUTH) && (o[1] == 5) && (get16(&o[2]) == PPP_PROTO_CHAP))
                    c->isp_chap_alg = o[4];
                o += o[1];
            }
            send_ctl(c, PPP_PROTO_LCP, PPP_CONF_ACK, p[1], &p[4], len - 4);
            c->lcp_ack_sent = 1;
            lcp_maybe_open(c);
            break;
        }
        case PPP_CONF_ACK:
            if (p[1] == c->id) {
                c->lcp_ack_rcvd = 1;
                lcp_maybe_open(c);
            }
            break;
        case PPP_CONF_REJ:
        case PPP_CONF_NAK:
            if (p[1] == c->id) {
                const uint8_t *o   = &p[4];
                const uint8_t *end = p + len;

                while ((end - o) >= 2 && o[1] >= 2 && o[1] <= (end - o)) {
                    if ((p[0] == PPP_CONF_REJ) && (c->lcp_rejected_n < 16))
                        c->lcp_rejected[c->lcp_rejected_n++] = o[0];
                    o += o[1];
                }
                c->lcp_round++;
                send_lcp_req(c);
            }
            break;
        case PPP_TERM_REQ:
            send_ctl(c, PPP_PROTO_LCP, PPP_TERM_ACK, p[1], NULL, 0);
            c->terminated = 1;
            break;
        case PPP_TERM_ACK:
            c->terminated = 1;
            break;
        case PPP_PROT_REJ:
            c->prot_rejects++;
            break;
        case PPP_ECHO_REQ: {
            uint8_t rep[256];
            size_t  dl = len - 4;

            if (dl > sizeof(rep))
                dl = sizeof(rep);
            memcpy(rep, &p[4], dl);
            if (dl >= 4)
                put32(rep, c->magic);
            send_ctl(c, PPP_PROTO_LCP, PPP_ECHO_REP, p[1], rep, dl);
            break;
        }
        case PPP_ECHO_REP:
            c->echo_replies++;
            break;
        default:
            break;
    }
}

static void
ipcp_maybe_open(ppp_client_t *c)
{
    if (!c->ipcp_opened && c->ipcp_ack_sent && c->ipcp_ack_rcvd) {
        c->ipcp_opened = 1;
        if (c->verbose)
            printf("    client: IPCP open: %s, DNS %s, ISP %s\n", ppp_client_ip_str(c->my_ip),
                   ppp_client_ip_str(c->dns), ppp_client_ip_str(c->isp_ip));
    }
}

static void
handle_ipcp(ppp_client_t *c, const uint8_t *p, size_t n)
{
    size_t         len;
    const uint8_t *o;
    const uint8_t *end;

    if (n < 4)
        return;
    len = get16(&p[2]);
    if ((len < 4) || (len > n))
        return;
    o   = &p[4];
    end = p + len;

    switch (p[0]) {
        case PPP_CONF_REQ:
            while ((end - o) >= 2 && o[1] >= 2 && o[1] <= (end - o)) {
                if ((o[0] == IPCP_ADDR) && (o[1] == 6))
                    c->isp_ip = get32(&o[2]);
                o += o[1];
            }
            send_ctl(c, PPP_PROTO_IPCP, PPP_CONF_ACK, p[1], &p[4], len - 4);
            c->ipcp_ack_sent = 1;
            ipcp_maybe_open(c);
            break;
        case PPP_CONF_ACK:
            if (p[1] == c->id) {
                c->ipcp_ack_rcvd = 1;
                ipcp_maybe_open(c);
            }
            break;
        case PPP_CONF_NAK:
        case PPP_CONF_REJ:
            if (p[1] != c->id)
                break;
            while ((end - o) >= 2 && o[1] >= 2 && o[1] <= (end - o)) {
                if (p[0] == PPP_CONF_REJ) {
                    if (c->ipcp_rejected_n < 16)
                        c->ipcp_rejected[c->ipcp_rejected_n++] = o[0];
                } else if (o[1] == 6) {
                    if (o[0] == IPCP_ADDR)
                        c->my_ip = get32(&o[2]);
                    else if (o[0] == IPCP_DNS1)
                        c->dns = get32(&o[2]);
                    else if (o[0] == IPCP_DNS2)
                        c->dns2 = get32(&o[2]);
                }
                o += o[1];
            }
            c->ipcp_round++;
            send_ipcp_req(c);
            break;
        default:
            break;
    }
}

static void
handle_frame(void *opaque, const uint8_t *f, size_t n)
{
    ppp_client_t *c = (ppp_client_t *) opaque;
    uint16_t      proto;

    if ((n >= 2) && (f[0] == 0xff) && (f[1] == 0x03)) {
        f += 2;
        n -= 2;
    }
    if (n < 2)
        return;
    proto = get16(f);
    f += 2;
    n -= 2;

    switch (proto) {
        case PPP_PROTO_LCP:
            handle_lcp(c, f, n);
            break;
        case PPP_PROTO_PAP:
            if ((n >= 4) && (f[0] == 2) && !c->pap_acked) {
                c->pap_acked = 1;
                send_ipcp_req(c);
            } else if ((n >= 4) && (f[0] == 3))
                c->auth_failed = 1;
            break;
        case PPP_PROTO_CHAP:
            handle_chap(c, f, n);
            break;
        case PPP_PROTO_IPCP:
            handle_ipcp(c, f, n);
            break;
        case PPP_PROTO_IP:
            if ((c->verbose > 2) && (n >= 24))
                printf("    client: IP in: proto %u, %s:%u -> %s:%u, %u bytes\n", f[9],
                       ppp_client_ip_str(get32(&f[12])), get16(&f[20]), ppp_client_ip_str(get32(&f[16])),
                       get16(&f[22]), (unsigned) n);
            if (c->ip_n == PPPC_MAX_IP) {
                memmove(c->ip[0], c->ip[1], sizeof(c->ip[0]) * (PPPC_MAX_IP - 1));
                memmove(&c->ip_len[0], &c->ip_len[1], sizeof(c->ip_len[0]) * (PPPC_MAX_IP - 1));
                c->ip_n--;
            }
            if (n <= sizeof(c->ip[0])) {
                memcpy(c->ip[c->ip_n], f, n);
                c->ip_len[c->ip_n++] = n;
            }
            break;
        default:
            break;
    }
}

void
ppp_client_init(ppp_client_t *c)
{
    memset(&c->rx, 0, sizeof(*c) - offsetof(ppp_client_t, rx));
    ppp_rx_init(&c->rx);
    c->magic = 0x1d2c3b4a ^ isp_random();
    c->id    = (uint8_t) c->magic;
}

void
ppp_client_poll(ppp_client_t *c)
{
    uint8_t buf[4096];
    size_t  n;

    while ((n = c->read(c->opaque, buf, sizeof(buf))) > 0)
        ppp_rx_feed(&c->rx, buf, n, handle_frame, c);
}

int
ppp_client_connect(ppp_client_t *c, uint32_t timeout_ms)
{
    const uint32_t until = ppp_client_ms() + timeout_ms;

    send_lcp_req(c);
    while (!c->ipcp_opened) {
        if ((int32_t) (ppp_client_ms() - until) >= 0)
            return 0;
        ppp_client_poll(c);
        if (c->terminated || c->auth_failed)
            return 0;
        /* A request that went unanswered goes again. */
        if ((ppp_client_ms() - c->last_req_ms) >= 1000) {
            if (!c->lcp_ack_rcvd)
                send_lcp_req(c);
            else if (c->lcp_opened && c->isp_wants_pap && !c->pap_acked)
                send_pap(c);
            else if (c->lcp_opened && c->isp_chap_alg && !c->chap_ok)
                c->last_req_ms = ppp_client_ms(); /* the ISP challenges again itself */
            else if (c->lcp_opened && !c->ipcp_ack_rcvd)
                send_ipcp_req(c);
            else
                c->last_req_ms = ppp_client_ms();
        }
        c->idle(c->opaque);
    }
    return 1;
}

int
ppp_client_terminate(ppp_client_t *c, uint32_t timeout_ms)
{
    const uint32_t until = ppp_client_ms() + timeout_ms;
    static const char why[] = "User request";

    c->terminated = 0;
    c->id++;
    send_ctl(c, PPP_PROTO_LCP, PPP_TERM_REQ, c->id, (const uint8_t *) why, sizeof(why) - 1);
    while (!c->terminated && ((int32_t) (ppp_client_ms() - until) < 0)) {
        ppp_client_poll(c);
        c->idle(c->opaque);
    }
    return c->terminated;
}

/* ------------------------------------------------------------------- IPv4 */

static uint32_t
csum_add(uint32_t sum, const uint8_t *p, size_t len)
{
    for (size_t i = 0; (i + 1) < len; i += 2)
        sum += get16(&p[i]);
    if (len & 1)
        sum += (uint32_t) p[len - 1] << 8;
    return sum;
}

static uint16_t
csum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t) ~sum;
}

static size_t
ip_header(ppp_client_t *c, uint8_t *p, int proto, uint32_t dst, size_t payload)
{
    memset(p, 0, 20);
    p[0] = 0x45;
    put16(&p[2], (uint16_t) (20 + payload));
    put16(&p[4], c->ip_id++);
    p[8] = 64;
    p[9] = (uint8_t) proto;
    put32(&p[12], c->my_ip);
    put32(&p[16], dst);
    put16(&p[10], csum_fold(csum_add(0, p, 20)));
    return 20;
}

void
ppp_client_send_ip(ppp_client_t *c, const uint8_t *pkt, size_t len)
{
    ppp_client_send(c, PPP_PROTO_IP, pkt, len);
}

void
ppp_client_send_udp(ppp_client_t *c, uint32_t dst, uint16_t sport, uint16_t dport,
                    const uint8_t *data, size_t len)
{
    uint8_t pkt[1600];
    size_t  h = ip_header(c, pkt, 17, dst, 8 + len);

    put16(&pkt[h], sport);
    put16(&pkt[h + 2], dport);
    put16(&pkt[h + 4], (uint16_t) (8 + len));
    put16(&pkt[h + 6], 0);
    memcpy(&pkt[h + 8], data, len);
    ppp_client_send_ip(c, pkt, h + 8 + len);
}

/* Takes the oldest received packet of `proto` to `dport` out of the list. */
static int
take_packet(ppp_client_t *c, int proto, uint16_t dport, uint8_t *out, size_t *len)
{
    for (int i = 0; i < c->ip_n; i++) {
        const uint8_t *p   = c->ip[i];
        const size_t   ihl = (size_t) (p[0] & 0x0f) * 4;

        if ((c->ip_len[i] < (ihl + 8)) || (p[9] != proto) || (get16(&p[ihl + 2]) != dport))
            continue;
        *len = c->ip_len[i];
        memcpy(out, p, *len);
        memmove(&c->ip[i], &c->ip[i + 1], sizeof(c->ip[0]) * (size_t) (c->ip_n - i - 1));
        memmove(&c->ip_len[i], &c->ip_len[i + 1], sizeof(c->ip_len[0]) * (size_t) (c->ip_n - i - 1));
        c->ip_n--;
        return 1;
    }
    return 0;
}

int
ppp_client_recv_udp(ppp_client_t *c, uint16_t sport, uint8_t *data, size_t size,
                    uint32_t *src, uint16_t *src_port, uint32_t timeout_ms)
{
    const uint32_t until = ppp_client_ms() + timeout_ms;
    uint8_t        pkt[1600];
    size_t         len;

    for (;;) {
        ppp_client_poll(c);
        if (take_packet(c, 17, sport, pkt, &len)) {
            const size_t ihl = (size_t) (pkt[0] & 0x0f) * 4;
            size_t       dl  = get16(&pkt[ihl + 4]) - 8;

            if (dl > size)
                dl = size;
            memcpy(data, &pkt[ihl + 8], dl);
            if (src)
                *src = get32(&pkt[12]);
            if (src_port)
                *src_port = get16(&pkt[ihl]);
            return (int) dl;
        }
        if ((int32_t) (ppp_client_ms() - until) >= 0)
            return -1;
        c->idle(c->opaque);
    }
}

uint32_t
ppp_client_resolve(ppp_client_t *c, const char *name, uint32_t timeout_ms)
{
    uint8_t     q[300];
    uint8_t     r[1500];
    size_t      n = 12;
    const char *s = name;
    int         len;
    size_t      i;

    memset(q, 0, 12);
    put16(&q[0], 0x86b0);
    put16(&q[2], 0x0100); /* recursion desired */
    put16(&q[4], 1);
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t      l   = dot ? (size_t) (dot - s) : strlen(s);

        q[n++] = (uint8_t) l;
        memcpy(&q[n], s, l);
        n += l;
        s += l + (dot ? 1 : 0);
    }
    q[n++] = 0;
    put16(&q[n], 1); /* A  */
    put16(&q[n + 2], 1); /* IN */
    n += 4;

    ppp_client_send_udp(c, c->dns, 40053, 53, q, n);
    len = ppp_client_recv_udp(c, 40053, r, sizeof(r), NULL, NULL, timeout_ms);
    if (c->verbose)
        printf("    client: DNS answer of %d bytes, flags %04X, %u answers\n", len,
               (len >= 12) ? get16(&r[2]) : 0, (len >= 12) ? get16(&r[6]) : 0);
    if ((len < 12) || (get16(&r[0]) != 0x86b0))
        return 0;

    /* Past the question, then the first A record. */
    i = 12;
    while ((i < (size_t) len) && r[i])
        i += (size_t) r[i] + 1;
    i += 5;
    for (int a = 0; (a < get16(&r[6])) && ((i + 12) <= (size_t) len); a++) {
        uint16_t type;
        uint16_t rdlen;

        if ((r[i] & 0xc0) == 0xc0)
            i += 2;
        else {
            while ((i < (size_t) len) && r[i])
                i += (size_t) r[i] + 1;
            i++;
        }
        type  = get16(&r[i]);
        rdlen = get16(&r[i + 8]);
        i += 10;
        if ((type == 1) && (rdlen == 4) && ((i + 4) <= (size_t) len))
            return get32(&r[i]);
        i += rdlen;
    }
    return 0;
}

/* -------------------------------------------------------------------- TCP */

static void
tcp_send(ppp_client_t *c, uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
         int flags, const uint8_t *data, size_t len)
{
    uint8_t  pkt[1600];
    uint8_t  pseudo[12];
    size_t   h    = ip_header(c, pkt, 6, dst, 20 + ((flags & 0x02) ? 4 : 0) + len);
    size_t   thl  = 20 + ((flags & 0x02) ? 4 : 0);
    uint8_t *t    = &pkt[h];
    uint32_t sum;

    memset(t, 0, thl);
    put16(&t[0], sport);
    put16(&t[2], dport);
    put32(&t[4], seq);
    put32(&t[8], ack);
    t[12] = (uint8_t) ((thl / 4) << 4);
    t[13] = (uint8_t) flags;
    put16(&t[14], 32768); /* window */
    if (flags & 0x02) {
        t[20] = 2; /* MSS */
        t[21] = 4;
        put16(&t[22], 1400);
    }
    memcpy(&t[thl], data, len);

    put32(&pseudo[0], c->my_ip);
    put32(&pseudo[4], dst);
    pseudo[8] = 0;
    pseudo[9] = 6;
    put16(&pseudo[10], (uint16_t) (thl + len));
    sum = csum_add(0, pseudo, 12);
    sum = csum_add(sum, t, thl + len);
    put16(&t[16], csum_fold(sum));
    ppp_client_send_ip(c, pkt, h + thl + len);
}

int
ppp_client_tcp_fetch(ppp_client_t *c, uint32_t dst, uint16_t dport, const char *req,
                     uint8_t *resp, size_t size, uint32_t timeout_ms)
{
    const uint32_t until  = ppp_client_ms() + timeout_ms;
    const uint16_t sport  = (uint16_t) (40000 + (isp_random() % 20000));
    const uint32_t iss    = isp_random();
    uint32_t       snd    = iss + 1;
    uint32_t       rcv    = 0;
    int            state  = 0; /* 0 SYN sent, 1 established, 2 done */
    size_t         got    = 0;
    uint32_t       sent_at;
    uint8_t        pkt[1600];
    size_t         len;

    tcp_send(c, dst, sport, dport, iss, 0, 0x02, NULL, 0);
    sent_at = ppp_client_ms();

    while (state != 2) {
        if ((int32_t) (ppp_client_ms() - until) >= 0)
            return -1;
        ppp_client_poll(c);
        while (take_packet(c, 6, sport, pkt, &len)) {
            const size_t   ihl   = (size_t) (pkt[0] & 0x0f) * 4;
            const uint8_t *t     = &pkt[ihl];
            const size_t   thl   = (size_t) (t[12] >> 4) * 4;
            const int      flags = t[13];
            const uint32_t seq   = get32(&t[4]);
            const size_t   dl    = get16(&pkt[2]) - ihl - thl;

            if (c->verbose > 1)
                printf("    client: TCP in: flags %02X seq %u, %u bytes (expecting %u)\n", flags,
                       (unsigned) seq, (unsigned) dl, (unsigned) rcv);
            if (flags & 0x04) {
                printf("    client: TCP reset by %s:%u\n", ppp_client_ip_str(dst), dport);
                return -1;
            }
            if ((state == 0) && (flags & 0x12) == 0x12) {
                rcv   = seq + 1;
                state = 1;
                tcp_send(c, dst, sport, dport, snd, rcv, 0x10, NULL, 0);
                tcp_send(c, dst, sport, dport, snd, rcv, 0x18, (const uint8_t *) req, strlen(req));
                snd += (uint32_t) strlen(req);
                continue;
            }
            if (state != 1)
                continue;
            if ((seq == rcv) && (dl > 0)) {
                size_t take = dl;

                if (take > (size - got))
                    take = size - got;
                memcpy(&resp[got], &t[thl], take);
                got += take;
                rcv += (uint32_t) dl;
            }
            if ((flags & 0x01) && (seq + dl == rcv)) {
                rcv++;
                tcp_send(c, dst, sport, dport, snd, rcv, 0x11, NULL, 0);
                state = 2;
                break;
            }
            tcp_send(c, dst, sport, dport, snd, rcv, 0x10, NULL, 0);
            if (got >= size) {
                tcp_send(c, dst, sport, dport, snd, rcv, 0x14, NULL, 0); /* RST */
                state = 2;
                break;
            }
        }
        if ((state == 0) && ((ppp_client_ms() - sent_at) > 2000)) {
            tcp_send(c, dst, sport, dport, iss, 0, 0x02, NULL, 0);
            sent_at = ppp_client_ms();
        }
        c->idle(c->opaque);
    }
    return (int) got;
}
