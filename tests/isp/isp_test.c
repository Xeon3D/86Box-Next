/*
 * 86Box-Next: the virtual ISP (src/network/isp/).  No test framework;
 * non-zero on failure.
 *
 *   framing   RFC 1662 against an independent bitwise FCS and the CRC-16/X-25
 *             check value; input split at every byte and joined; bad FCS,
 *             oversized, aborted and runt frames; ACCM.
 *   session   the PPP automaton driven frame by frame on a fake clock:
 *             passive start, LCP Ack/Nak/Reject, IPCP address and DNS, PAP,
 *             Protocol-Reject, echo, bounded retries, termination.
 *   isp       whole sessions, through libslirp, to a UDP socket on the host's
 *             loopback (the gateway address .2 is the host): IPCP open plus a
 *             routed packet each way, two sessions at once with their own
 *             addresses, one ended without the other, addresses reused.
 *             Offline: nothing leaves the machine.
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
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
typedef int SOCKET;
#    define closesocket close
#endif
#include "isp.h"
#include "isp_plat.h"
#include "ppp_framing.h"
#include "ppp_session.h"
#include "ppp_client.h"

static int failures;

static void
check(const char *what, int ok)
{
    if (!ok)
        failures++;
    printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
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

/* ---------------------------------------------------------------- framing */

/* RFC 1662 C.1, one bit at a time: shares nothing with the table version. */
static uint16_t
fcs_bitwise(const uint8_t *p, size_t len)
{
    uint16_t fcs = 0xffff;

    while (len--) {
        fcs ^= *p++;
        for (int i = 0; i < 8; i++)
            fcs = (fcs & 1) ? (uint16_t) ((fcs >> 1) ^ 0x8408) : (uint16_t) (fcs >> 1);
    }
    return fcs;
}

typedef struct {
    uint8_t frames[16][PPP_MAX_FRAME];
    size_t  len[16];
    int     n;
} frames_t;

static void
collect(void *opaque, const uint8_t *f, size_t len)
{
    frames_t *fr = (frames_t *) opaque;

    if (fr->n < 16) {
        memcpy(fr->frames[fr->n], f, len);
        fr->len[fr->n++] = len;
    }
}

static void
test_framing(void)
{
    static const uint8_t check_str[] = "123456789";
    /* An LCP Configure-Request as Windows 98 sends it: ACCM 0, magic, PFC,
       ACFC, callback. */
    static const uint8_t lcp[] = { 0x01, 0x01, 0x00, 0x17, 0x02, 0x06, 0x00, 0x00, 0x00, 0x00,
                                   0x05, 0x06, 0x12, 0x34, 0x56, 0x78, 0x07, 0x02, 0x08, 0x02,
                                   0x0d, 0x03, 0x06 };
    uint8_t  enc[PPP_ENCODED_MAX(2048)];
    uint8_t  head[sizeof(lcp) + 4];
    size_t   n;
    ppp_rx_t rx;
    frames_t fr;
    int      ok;

    printf("\n== framing ==\n");

    /* CRC-16/X-25's check value is the complement of the FCS of "123456789". */
    check("FCS-16 of \"123456789\" is ~0x906E (CRC-16/X-25)",
          (uint16_t) ~ppp_fcs16(PPP_FCS_INIT, check_str, 9) == 0x906e);
    check("table FCS matches the bitwise one",
          ppp_fcs16(PPP_FCS_INIT, lcp, sizeof(lcp)) == fcs_bitwise(lcp, sizeof(lcp)));

    n = ppp_encode(PPP_ACCM_ALL, 0xc021, lcp, sizeof(lcp), enc, sizeof(enc));
    ok = (n > 0) && (enc[0] == 0x7e) && (enc[n - 1] == 0x7e);
    for (size_t i = 1; ok && (i < (n - 1)); i++)
        ok = (enc[i] != 0x7e) && (enc[i] >= 0x20); /* default ACCM: no control characters */
    check("encoded with flags, nothing below 20h inside", ok);

    /* The FCS on the wire is the bitwise one, complemented, LSB first. */
    memcpy(head, (const uint8_t[]) { 0xff, 0x03, 0xc0, 0x21 }, 4);
    memcpy(head + 4, lcp, sizeof(lcp));
    {
        const uint16_t fcs = (uint16_t) ~fcs_bitwise(head, sizeof(head));
        uint8_t        dec[64];
        size_t         d = 0;

        for (size_t i = 1; i < (n - 1); i++)
            dec[d++] = (enc[i] == 0x7d) ? (uint8_t) (enc[++i] ^ 0x20) : enc[i];
        check("the FCS is the complement, low byte first",
              (d == (sizeof(head) + 2)) && (dec[d - 2] == (fcs & 0xff)) && (dec[d - 1] == (fcs >> 8)));
    }

    /* Decoding: whole, then a byte at a time, then joined with another. */
    ppp_rx_init(&rx);
    memset(&fr, 0, sizeof(fr));
    ppp_rx_feed(&rx, enc, n, collect, &fr);
    check("decodes to address, control, protocol and information",
          (fr.n == 1) && (fr.len[0] == sizeof(head)) && !memcmp(fr.frames[0], head, sizeof(head)));

    ppp_rx_init(&rx);
    memset(&fr, 0, sizeof(fr));
    for (size_t i = 0; i < n; i++)
        ppp_rx_feed(&rx, &enc[i], 1, collect, &fr);
    check("split at every byte boundary", (fr.n == 1) && !memcmp(fr.frames[0], head, sizeof(head)));

    {
        uint8_t joined[sizeof(enc) * 3];
        size_t  j = 0;

        memcpy(&joined[j], "noise", 5); /* before the first flag: hunted past */
        j += 5;
        memcpy(&joined[j], enc, n);
        j += n;
        memcpy(&joined[j], enc + 1, n - 1); /* sharing the flag */
        j += n - 1;
        ppp_rx_init(&rx);
        memset(&fr, 0, sizeof(fr));
        ppp_rx_feed(&rx, joined, j, collect, &fr);
        check("two frames in one read, one shared flag, noise before", fr.n == 2);
    }

    /* A corrupted byte fails the FCS; the next frame still decodes. */
    {
        uint8_t bad[sizeof(enc) * 2];

        memcpy(bad, enc, n);
        bad[6] ^= 0x01;
        memcpy(bad + n, enc, n);
        ppp_rx_init(&rx);
        memset(&fr, 0, sizeof(fr));
        ppp_rx_feed(&rx, bad, n * 2, collect, &fr);
        check("bad FCS discarded and counted, resynchronised", (fr.n == 1) && (rx.bad_fcs == 1));
    }

    /* Over the limit: dropped whole, then the next one decodes. */
    {
        uint8_t *big = (uint8_t *) malloc(PPP_MAX_FRAME + 64 + n);

        big[0] = 0x7e;
        memset(big + 1, 0x55, PPP_MAX_FRAME + 32);
        memcpy(big + PPP_MAX_FRAME + 33, enc, n);
        ppp_rx_init(&rx);
        memset(&fr, 0, sizeof(fr));
        ppp_rx_feed(&rx, big, PPP_MAX_FRAME + 33 + n, collect, &fr);
        check("oversized frame discarded, the next one decodes", (fr.n == 1) && (rx.too_long == 1));
        free(big);
    }

    /* 7D 7E aborts; a two-byte frame is a runt. */
    {
        const uint8_t abort_runt[] = { 0x7e, 0xff, 0x03, 0x7d, 0x7e, 0x41, 0x42, 0x7e };

        ppp_rx_init(&rx);
        memset(&fr, 0, sizeof(fr));
        ppp_rx_feed(&rx, abort_runt, sizeof(abort_runt), collect, &fr);
        check("abort sequence and runt dropped", (fr.n == 0) && (rx.aborts == 1) && (rx.runts == 1));
    }

    /* With ACCM 0, control characters go raw and only flag and escape are
       escaped. */
    {
        const uint8_t ctl[] = { 0x00, 0x11, 0x13, 0x1f, 0x7e, 0x7d, 0x41 };
        uint8_t       e2[64];
        size_t        m   = ppp_encode(0, 0x0021, ctl, sizeof(ctl), e2, sizeof(e2));
        int           esc = 0;
        int           raw = 0;

        ok = (m > 0);
        for (size_t i = 1; ok && (i < (m - 1)); i++) {
            if (e2[i] == 0x7d) {
                ok = (e2[i + 1] == 0x5e) || (e2[i + 1] == 0x5d); /* 7E or 7D, nothing else */
                esc++;
                i++;
            } else if (e2[i] < 0x20)
                raw++;
        }
        check("ACCM 0: only 7E and 7D escaped, control characters raw", ok && (esc >= 2) && (raw >= 4));

        ppp_rx_init(&rx);
        rx.accm = 0;
        memset(&fr, 0, sizeof(fr));
        ppp_rx_feed(&rx, e2, m, collect, &fr);
        check("...and decode as data under a receive ACCM of 0",
              (fr.n == 1) && (fr.len[0] == (4 + sizeof(ctl))) && !memcmp(fr.frames[0] + 4, ctl, sizeof(ctl)));
    }

    /* A receive ACCM drops what something on the line inserted (XON here). */
    {
        const uint8_t data[] = { 0x00, 0x01, 0x1f, 0x41, 0x42 };
        uint8_t       e2[64];
        uint8_t       noisy[96];
        size_t        m = ppp_encode(0, 0x0021, data, sizeof(data), e2, sizeof(e2));
        size_t        k = 0;

        for (size_t i = 0; i < m; i++) {
            noisy[k++] = e2[i];
            if ((i == 3) || (i == 7))
                noisy[k++] = 0x11;
        }
        ppp_rx_init(&rx);
        rx.accm = 0x000a0000; /* XON and XOFF */
        memset(&fr, 0, sizeof(fr));
        ppp_rx_feed(&rx, noisy, k, collect, &fr);
        check("receive ACCM 000A0000 drops inserted XONs",
              (fr.n == 1) && (fr.len[0] == (4 + sizeof(data))) && !memcmp(fr.frames[0] + 4, data, sizeof(data)));
    }
}

/* ---------------------------------------------------------------- session */

typedef struct {
    uint16_t proto[64];
    uint8_t  info[64][256];
    size_t   len[64];
    int      n;
    int      ip_up;
    int      ip_down;
    int      finished;
    char     why[128];
    uint8_t  ip[2048];
    size_t   ip_len;
} sink_t;

static void
sink_send(void *opaque, uint16_t proto, const uint8_t *info, size_t len, uint32_t accm)
{
    sink_t *s = (sink_t *) opaque;

    (void) accm;
    if (s->n < 64) {
        s->proto[s->n] = proto;
        memcpy(s->info[s->n], info, (len > 256) ? 256 : len);
        s->len[s->n++] = len;
    }
}

static void sink_ip_up(void *o) { ((sink_t *) o)->ip_up++; }
static void sink_ip_down(void *o) { ((sink_t *) o)->ip_down++; }
static void
sink_ip_input(void *o, const uint8_t *p, size_t len)
{
    sink_t *s = (sink_t *) o;

    memcpy(s->ip, p, len);
    s->ip_len = len;
}
static void
sink_finished(void *o, const char *why)
{
    sink_t *s = (sink_t *) o;

    s->finished++;
    snprintf(s->why, sizeof(s->why), "%s", why);
}
static void
sink_log(void *o, const char *msg)
{
    (void) o;
    if (getenv("ISP_TEST_VERBOSE"))
        printf("      | %s\n", msg);
}

static const ppp_callbacks_t sink_cb = { sink_send, sink_ip_up, sink_ip_down, sink_ip_input, sink_finished, sink_log };

/* One control packet in as a frame. */
static void
feed(ppp_t *ppp, uint16_t proto, int code, int id, const uint8_t *data, size_t len, uint32_t now)
{
    uint8_t f[300];

    f[0] = 0xff;
    f[1] = 0x03;
    f[2] = (uint8_t) (proto >> 8);
    f[3] = (uint8_t) proto;
    f[4] = (uint8_t) code;
    f[5] = (uint8_t) id;
    f[6] = (uint8_t) ((len + 4) >> 8);
    f[7] = (uint8_t) (len + 4);
    memcpy(&f[8], data, len);
    ppp_input(ppp, f, len + 8, now);
}

/* The last packet sent of `proto` with `code`, or -1. */
static int
find(const sink_t *s, uint16_t proto, int code)
{
    for (int i = s->n - 1; i >= 0; i--)
        if ((s->proto[i] == proto) && (s->info[i][0] == code))
            return i;
    return -1;
}

static int
has_opt(const uint8_t *pkt, int type)
{
    const size_t len = ((size_t) pkt[2] << 8) | pkt[3];

    for (size_t i = 4; (i + 1) < len; i += pkt[i + 1]) {
        if (pkt[i + 1] < 2)
            return 0;
        if (pkt[i] == type)
            return 1;
    }
    return 0;
}

static uint32_t
opt32(const uint8_t *pkt, int type)
{
    const size_t len = ((size_t) pkt[2] << 8) | pkt[3];

    for (size_t i = 4; (i + 1) < len; i += pkt[i + 1]) {
        if (pkt[i + 1] < 2)
            return 0;
        if ((pkt[i] == type) && (pkt[i + 1] == 6))
            return ((uint32_t) pkt[i + 2] << 24) | ((uint32_t) pkt[i + 3] << 16) |
                   ((uint32_t) pkt[i + 4] << 8) | pkt[i + 5];
    }
    return 0;
}

static void
session_config(ppp_config_t *pc)
{
    memset(pc, 0, sizeof(*pc));
    pc->local_ip   = 0x0a560102;
    pc->peer_ip    = 0x0a56010f;
    pc->dns[0]     = 0x0a560103;
    pc->dns[1]     = 0x0a560103;
    pc->magic_seed = 0xc0ffee;
}

/* LCP and IPCP up the way the scripted client would bring them up. */
static void
bring_up(ppp_t *ppp, sink_t *s, uint32_t *now)
{
    const uint8_t win98[] = { 0x02, 0x06, 0, 0, 0, 0, 0x05, 0x06, 0x12, 0x34, 0x56, 0x78,
                              0x07, 0x02, 0x08, 0x02, 0x0d, 0x03, 0x06 };
    const uint8_t plain[] = { 0x02, 0x06, 0, 0, 0, 0, 0x05, 0x06, 0x12, 0x34, 0x56, 0x78 };
    int           i;

    feed(ppp, 0xc021, 1, 1, win98, sizeof(win98), *now);
    feed(ppp, 0xc021, 1, 2, plain, sizeof(plain), *now);
    i = find(s, 0xc021, 1); /* the ISP's request: ack it */
    feed(ppp, 0xc021, 2, s->info[i][1], &s->info[i][4], s->len[i] - 4, *now);
}

static void
test_session(void)
{
    ppp_config_t pc;
    ppp_t        ppp;
    sink_t       s;
    uint32_t     now = 1000;
    int          i;

    printf("\n== session ==\n");
    session_config(&pc);

    /* Passive: nothing at all, however long, before the guest's first LCP. */
    memset(&s, 0, sizeof(s));
    ppp_init(&ppp, &pc, &sink_cb, &s, now);
    ppp_lower_up(&ppp, now);
    for (int t = 0; t < 120; t++) {
        now += 1000;
        ppp_tick(&ppp, now);
    }
    check("silent for two minutes before the guest's first frame", (s.n == 0) && !s.finished);
    check("...with no timer pending", ppp_next_timeout(&ppp, now) == UINT32_MAX);

    /* Windows 98's LCP request: PFC, ACFC and callback rejected, together,
       and nothing else in the reject. */
    {
        const uint8_t win98[] = { 0x02, 0x06, 0, 0, 0, 0, 0x05, 0x06, 0x12, 0x34, 0x56, 0x78,
                                  0x07, 0x02, 0x08, 0x02, 0x0d, 0x03, 0x06 };

        feed(&ppp, 0xc021, 1, 1, win98, sizeof(win98), now);
    }
    i = find(&s, 0xc021, 4);
    check("Windows' LCP request: Configure-Reject", i >= 0);
    check("...of PFC, ACFC and callback, and only those",
          (i >= 0) && has_opt(s.info[i], 7) && has_opt(s.info[i], 8) && has_opt(s.info[i], 13) &&
          !has_opt(s.info[i], 2) && !has_opt(s.info[i], 5) && (s.info[i][1] == 1));
    i = find(&s, 0xc021, 1);
    check("the ISP's own request: ACCM 0 and a magic number, no auth",
          (i >= 0) && has_opt(s.info[i], 2) && (opt32(s.info[i], 2) == 0) && has_opt(s.info[i], 5) &&
          !has_opt(s.info[i], 3));
    check("a restart timer runs once negotiation has begun", ppp_next_timeout(&ppp, now) <= 3000);

    {
        const uint8_t plain[] = { 0x02, 0x06, 0, 0, 0, 0, 0x05, 0x06, 0x12, 0x34, 0x56, 0x78 };

        feed(&ppp, 0xc021, 1, 2, plain, sizeof(plain), now);
    }
    i = find(&s, 0xc021, 2);
    check("the request without them: Configure-Ack, options echoed", (i >= 0) && (s.info[i][1] == 2) &&
                                                                         (s.len[i] == 16));
    /* A bad ack (wrong id) is ignored; the right one opens LCP. */
    i = find(&s, 0xc021, 1);
    feed(&ppp, 0xc021, 2, (s.info[i][1] + 1) & 0xff, &s.info[i][4], s.len[i] - 4, now);
    check("an ack with the wrong identifier is ignored", ppp.lcp.state != FSM_OPENED);
    feed(&ppp, 0xc021, 2, s.info[i][1], &s.info[i][4], s.len[i] - 4, now);
    check("LCP opens", ppp.lcp.state == FSM_OPENED);
    check("IPCP starts only now: its request names the gateway",
          (find(&s, 0x8021, 1) >= 0) && (opt32(s.info[find(&s, 0x8021, 1)], 3) == 0x0a560102));

    /* IPCP, the Windows way: VJ and WINS rejected... */
    {
        const uint8_t req[] = { 0x03, 0x06, 0, 0, 0, 0, 0x02, 0x06, 0x00, 0x2d, 0x0f, 0x01,
                                0x81, 0x06, 0, 0, 0, 0, 0x82, 0x06, 0, 0, 0, 0,
                                0x83, 0x06, 0, 0, 0, 0, 0x84, 0x06, 0, 0, 0, 0 };

        feed(&ppp, 0x8021, 1, 7, req, sizeof(req), now);
        i = find(&s, 0x8021, 4);
        check("IPCP: VJ compression and both NBNS rejected",
              (i >= 0) && has_opt(s.info[i], 2) && has_opt(s.info[i], 0x82) && has_opt(s.info[i], 0x84) &&
              !has_opt(s.info[i], 3) && !has_opt(s.info[i], 0x81));
    }
    /* ...then 0.0.0.0 and DNS 0 naked with the session's own... */
    {
        const uint8_t req[] = { 0x03, 0x06, 0, 0, 0, 0, 0x81, 0x06, 0, 0, 0, 0, 0x83, 0x06, 0, 0, 0, 0 };

        feed(&ppp, 0x8021, 1, 8, req, sizeof(req), now);
        i = find(&s, 0x8021, 3);
        check("0.0.0.0 naked with the assigned address", (i >= 0) && (opt32(s.info[i], 3) == 0x0a56010f));
        check("DNS naked with the session's relay",
              (i >= 0) && (opt32(s.info[i], 0x81) == 0x0a560103) && (opt32(s.info[i], 0x83) == 0x0a560103));
    }
    /* ...a different address is not the guest's to pick... */
    {
        const uint8_t req[] = { 0x03, 0x06, 192, 168, 1, 50 };

        feed(&ppp, 0x8021, 1, 9, req, sizeof(req), now);
        i = find(&s, 0x8021, 3);
        check("an address of its own choosing is naked too", (i >= 0) && (s.info[i][1] == 9) &&
                                                                (opt32(s.info[i], 3) == 0x0a56010f));
    }
    /* ...and the right values are acknowledged. */
    {
        const uint8_t req[] = { 0x03, 0x06, 10, 86, 1, 15, 0x81, 0x06, 10, 86, 1, 3, 0x83, 0x06, 10, 86, 1, 3 };

        feed(&ppp, 0x8021, 1, 10, req, sizeof(req), now);
        check("the assigned address and DNS acknowledged", (find(&s, 0x8021, 2) >= 0) &&
                                                               (s.info[find(&s, 0x8021, 2)][1] == 10));
    }
    {
        const int before = s.n;

        ppp_send_ip(&ppp, (const uint8_t *) "\x45", 1);
        check("IP does not flow before IPCP opens", s.n == before);
    }
    i = find(&s, 0x8021, 1);
    feed(&ppp, 0x8021, 2, s.info[i][1], &s.info[i][4], s.len[i] - 4, now);
    check("IPCP opens: ip_up", (ppp.ipcp.state == FSM_OPENED) && (s.ip_up == 1));

    /* IP both ways. */
    {
        uint8_t f[64] = { 0xff, 0x03, 0x00, 0x21, 0x45, 0x00, 0x00, 0x14 };

        ppp_input(&ppp, f, 24, now);
        check("an IP frame reaches ip_input", (s.ip_len == 20) && (s.ip[0] == 0x45));
        ppp_input(&ppp, f + 2, 22, now); /* address and control compressed away */
        check("...also without address and control", s.ip_len == 20);
        ppp_send_ip(&ppp, &f[4], 20);
        check("an IP packet goes out as protocol 0021", s.proto[s.n - 1] == 0x0021);
    }

    /* CCP and IPX: Protocol-Reject, quoting the protocol. */
    {
        const uint8_t ccp[] = { 0xff, 0x03, 0x80, 0xfd, 0x01, 0x01, 0x00, 0x04 };

        ppp_input(&ppp, ccp, sizeof(ccp), now);
        i = find(&s, 0xc021, 8);
        check("CCP gets a Protocol-Reject quoting 80FD",
              (i >= 0) && (s.info[i][4] == 0x80) && (s.info[i][5] == 0xfd));
    }

    /* Echo. */
    {
        const uint8_t magic[4] = { 0x12, 0x34, 0x56, 0x78 };

        feed(&ppp, 0xc021, 9, 33, magic, 4, now);
        i = find(&s, 0xc021, 10);
        check("Echo-Request answered with the ISP's magic",
              (i >= 0) && (s.info[i][1] == 33) &&
              ((((uint32_t) s.info[i][4] << 24) | ((uint32_t) s.info[i][5] << 16) |
                ((uint32_t) s.info[i][6] << 8) | s.info[i][7]) == ppp.magic));
    }

    /* The guest hangs up: Terminate-Ack, IP down, then finished. */
    feed(&ppp, 0xc021, 5, 40, (const uint8_t *) "bye", 3, now);
    i = find(&s, 0xc021, 6);
    check("Terminate-Request acknowledged", (i >= 0) && (s.info[i][1] == 40));
    check("...IP goes down at once", s.ip_down == 1);
    now += 3100;
    ppp_tick(&ppp, now);
    check("...and the link finishes after a restart period", (s.finished == 1) && strstr(s.why, "ended"));

    /* A guest that stops answering: bounded retries, then finished. */
    memset(&s, 0, sizeof(s));
    ppp_init(&ppp, &pc, &sink_cb, &s, now);
    ppp_lower_up(&ppp, now);
    {
        const uint8_t plain[] = { 0x05, 0x06, 0x12, 0x34, 0x56, 0x78 };

        feed(&ppp, 0xc021, 1, 1, plain, sizeof(plain), now);
    }
    for (int t = 0; (t < 60) && !s.finished; t++) {
        now += 1000;
        ppp_tick(&ppp, now);
    }
    {
        int reqs = 0;

        for (int k = 0; k < s.n; k++)
            reqs += (s.proto[k] == 0xc021) && (s.info[k][0] == 1);
        check("unanswered LCP: 10 requests, then finished", (reqs == 10) && (s.finished == 1));
    }

    /* PAP: asked for, any pair let in, then IPCP. */
    memset(&s, 0, sizeof(s));
    pc.require_pap = 1;
    ppp_init(&ppp, &pc, &sink_cb, &s, now);
    ppp_lower_up(&ppp, now);
    bring_up(&ppp, &s, &now);
    i = find(&s, 0xc021, 1);
    check("with PAP configured, the ISP's request asks for C023", (i >= 0) && has_opt(s.info[i], 3));
    check("...LCP open, IPCP waits for authentication",
          (ppp.lcp.state == FSM_OPENED) && (find(&s, 0x8021, 1) < 0));
    {
        const uint8_t auth[] = { 4, 'j', 'o', 'h', 'n', 6, 's', 'e', 'c', 'r', 'e', 't' };

        feed(&ppp, 0xc023, 1, 5, auth, sizeof(auth), now);
    }
    i = find(&s, 0xc023, 2);
    check("any name and password: Authenticate-Ack", (i >= 0) && (s.info[i][1] == 5));
    check("...then IPCP starts", find(&s, 0x8021, 1) >= 0);
    pc.require_pap = 0;

    /* The guest refusing PAP does not keep it out. */
    memset(&s, 0, sizeof(s));
    pc.require_pap = 1;
    ppp_init(&ppp, &pc, &sink_cb, &s, now);
    ppp_lower_up(&ppp, now);
    {
        const uint8_t plain[] = { 0x05, 0x06, 0x12, 0x34, 0x56, 0x78 };
        const uint8_t rej[]   = { 0x03, 0x04, 0xc0, 0x23 };

        feed(&ppp, 0xc021, 1, 1, plain, sizeof(plain), now);
        i = find(&s, 0xc021, 1);
        feed(&ppp, 0xc021, 4, s.info[i][1], rej, sizeof(rej), now);
        i = find(&s, 0xc021, 1);
        check("PAP rejected by the guest: the next request drops it", (i >= 0) && !has_opt(s.info[i], 3));
        feed(&ppp, 0xc021, 2, s.info[i][1], &s.info[i][4], s.len[i] - 4, now);
        check("...and the link comes up without", (ppp.lcp.state == FSM_OPENED) && (find(&s, 0x8021, 1) >= 0));
    }
    pc.require_pap = 0;
}

/* -------------------------------------------------------------------- isp */

typedef struct {
    isp_session_t *s;
} direct_t;

static size_t
direct_write(void *o, const uint8_t *b, size_t n)
{
    return isp_session_write(((direct_t *) o)->s, b, n);
}

static size_t
direct_read(void *o, uint8_t *b, size_t n)
{
    return isp_session_read(((direct_t *) o)->s, b, n);
}

static void
direct_idle(void *o)
{
    (void) o;
    sleep_ms(1);
}

static void
isp_log(void *o, const char *msg)
{
    (void) o;
    if (getenv("ISP_TEST_VERBOSE"))
        printf("      | %s\n", msg);
}

/* A UDP socket on the host's loopback: what the guest reaches at .2. */
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

/* The guest sends to the gateway; the host's socket hears it and answers;
   the answer comes back over PPP. */
static int
udp_round_trip(ppp_client_t *c, SOCKET hs, uint16_t hport, const char *msg)
{
    struct sockaddr_in from;
    socklen_t          flen = sizeof(from);
    char               buf[256];
    uint8_t            back[256];
    int                n = -1;
    const uint32_t     until = ppp_client_ms() + 3000;
    char               reply[300];

    ppp_client_send_udp(c, c->isp_ip, 5555, hport, (const uint8_t *) msg, strlen(msg));
    while ((int32_t) (ppp_client_ms() - until) < 0) {
        ppp_client_poll(c);
        n = (int) recvfrom(hs, buf, sizeof(buf) - 1, 0, (struct sockaddr *) &from, &flen);
        if (n > 0)
            break;
        sleep_ms(1);
    }
    if (n <= 0) {
        printf("    the host never heard \"%s\"\n", msg);
        return 0;
    }
    buf[n] = '\0';
    if (strcmp(buf, msg) != 0)
        return 0;
    snprintf(reply, sizeof(reply), "re: %s", msg);
    sendto(hs, reply, (int) strlen(reply), 0, (struct sockaddr *) &from, flen);
    n = ppp_client_recv_udp(c, 5555, back, sizeof(back) - 1, NULL, NULL, 3000);
    if (n <= 0) {
        printf("    the guest never heard the answer to \"%s\"\n", msg);
        return 0;
    }
    back[n] = '\0';
    return !strcmp((const char *) back, reply);
}

static void
test_isp(void)
{
    const isp_session_callbacks_t cb = { NULL, isp_log, NULL };
    char                          err[128];
    direct_t                      d1, d2;
    ppp_client_t                  c1, c2;
    uint16_t                      hport;
    SOCKET                        hs;
    int                           n1, n2;

    printf("\n== isp: sessions through libslirp ==\n");
    hs = udp_listener(&hport);

    d1.s = isp_session_open(&cb, err, sizeof(err));
    d2.s = isp_session_open(&cb, err, sizeof(err));
    check("two sessions open", (d1.s != NULL) && (d2.s != NULL));
    if ((d1.s == NULL) || (d2.s == NULL))
        return;
    n1 = isp_session_number(d1.s);
    n2 = isp_session_number(d2.s);
    check("...with different numbers and /24s", (n1 != n2) && (isp_session_guest_ip(d1.s) != isp_session_guest_ip(d2.s)));

    /* Nothing comes out of a session the guest has not spoken to. */
    {
        uint8_t b[16];

        sleep_ms(300);
        check("a fresh session says nothing", isp_session_read(d1.s, b, sizeof(b)) == 0);
    }

    c1.write = c2.write = direct_write;
    c1.read = c2.read = direct_read;
    c1.idle = c2.idle = direct_idle;
    c1.opaque = &d1;
    c2.opaque = &d2;
    c1.verbose = c2.verbose = 1;
    ppp_client_init(&c1);
    ppp_client_init(&c2);

    check("guest 1 negotiates LCP and IPCP", ppp_client_connect(&c1, 10000));
    check("guest 2 negotiates LCP and IPCP", ppp_client_connect(&c2, 10000));
    check("guest 1 got its session's address", c1.my_ip == isp_session_guest_ip(d1.s));
    check("guest 2 got its own", (c2.my_ip == isp_session_guest_ip(d2.s)) && (c2.my_ip != c1.my_ip));
    check("each was told its own DNS relay (.3)", (c1.dns == (c1.my_ip - 12)) && (c2.dns == (c2.my_ip - 12)));
    check("LCP: PFC, ACFC and callback were rejected",
          (c1.lcp_rejected_n == 3) && (c1.lcp_round == 1));
    check("IPCP: VJ and NBNS rejected, address and DNS naked",
          (c1.ipcp_rejected_n == 3) && (c1.ipcp_round == 2));

    check("guest 1: UDP to the host and back", udp_round_trip(&c1, hs, hport, "hello from guest 1"));
    check("guest 2: UDP to the host and back", udp_round_trip(&c2, hs, hport, "hello from guest 2"));

    /* An idle UDP socket must not look like a dead one: libslirp answers a
       failed recvfrom() with ICMP port unreachable, so polling a socket that
       has nothing to read would send the guest a stream of them. */
    {
        int icmp = 0;

        sleep_ms(1000);
        ppp_client_poll(&c1);
        for (int k = 0; k < c1.ip_n; k++)
            icmp += (c1.ip[k][9] == 1);
        check("no ICMP unreachables from an idle UDP socket", icmp == 0);
    }

    /* IPX from a Windows guest: rejected, and the link carries on. */
    {
        const uint8_t ipxcp[] = { 0x01, 0x05, 0x00, 0x04 };

        ppp_client_send(&c1, 0x802b, ipxcp, sizeof(ipxcp));
        sleep_ms(100);
        ppp_client_poll(&c1);
        check("IPXCP gets a Protocol-Reject", c1.prot_rejects == 1);
    }

    /* Guest 1 hangs up; guest 2 is untouched. */
    check("guest 1 terminates LCP and gets the ack", ppp_client_terminate(&c1, 3000));
    {
        const uint32_t until = ppp_client_ms() + 6000;

        while (!isp_session_ended(d1.s) && ((int32_t) (ppp_client_ms() - until) < 0)) {
            ppp_client_poll(&c1);
            sleep_ms(10);
        }
        check("...its session reports the call over", isp_session_ended(d1.s));
    }
    check("guest 2 still reaches the host", udp_round_trip(&c2, hs, hport, "still here"));
    isp_session_close(d1.s);

    /* A new call takes the freed address. */
    d1.s = isp_session_open(&cb, err, sizeof(err));
    check("a new call gets the freed session number", (d1.s != NULL) && (isp_session_number(d1.s) == n1));
    if (d1.s != NULL) {
        ppp_client_init(&c1);
        check("...and works", ppp_client_connect(&c1, 10000) && udp_round_trip(&c1, hs, hport, "again"));
        isp_session_close(d1.s); /* carrier loss mid-call: no Terminate at all */
    }
    check("guest 2 survives the other's carrier loss", udp_round_trip(&c2, hs, hport, "and again"));
    isp_session_close(d2.s);

    /* Every address in use: the next call is refused, not hung. */
    {
        isp_settings_t st, old;
        isp_session_t *a, *b;

        isp_get_settings(&old);
        st              = old;
        st.max_sessions = 1;
        isp_set_settings(&st);
        a = isp_session_open(&cb, err, sizeof(err));
        b = isp_session_open(&cb, err, sizeof(err));
        check("with every address taken, the next call is refused", (a != NULL) && (b == NULL));
        isp_session_close(a);
        isp_set_settings(&old);
    }

#ifdef _WIN32
    closesocket(hs);
#else
    close(hs);
#endif
}

/* ---------------------------------------------------------------- control */

static const isp_call_info_t *
find_call(isp_call_info_t *calls, int n, int number)
{
    for (int i = 0; i < n; i++)
        if (calls[i].number == number)
            return &calls[i];
    return NULL;
}

/* Waits for a call's forward to leave "pending"; its state. */
static int
forward_state(int number, int idx)
{
    static isp_call_info_t calls[8];
    const uint32_t         until = ppp_client_ms() + 3000;

    while ((int32_t) (ppp_client_ms() - until) < 0) {
        const int              n = isp_list_calls(calls, 8);
        const isp_call_info_t *c = find_call(calls, n, number);

        if ((c != NULL) && (c->n_forwards > idx) && (c->forward_state[idx] != ISP_FORWARD_PENDING))
            return c->forward_state[idx];
        sleep_ms(10);
    }
    return ISP_FORWARD_PENDING;
}

static void
test_control(void)
{
    const isp_session_callbacks_t cb = { NULL, isp_log, NULL };
    static isp_call_info_t        calls[8];
    isp_settings_t                old, st;
    char                          err[128];
    direct_t                      d1, d2;
    ppp_client_t                  c1, c2;
    SOCKET                        hs;
    uint16_t                      hport;
    int                           n;

    printf("\n== isp: the status page's view and controls ==\n");
    hs = udp_listener(&hport);
    isp_get_settings(&old);
    st             = old;
    st.require_pap = 1;
    isp_set_settings(&st);

    d1.s = isp_session_open(&cb, err, sizeof(err));
    d2.s = isp_session_open(&cb, err, sizeof(err));
    if ((d1.s == NULL) || (d2.s == NULL)) {
        check("two sessions open", 0);
        return;
    }
    isp_session_set_label(d1.s, "COM2 of the Win98 box");
    memset(&c1, 0, sizeof(c1));
    memset(&c2, 0, sizeof(c2));
    c1.write = c2.write = direct_write;
    c1.read = c2.read = direct_read;
    c1.idle = c2.idle = direct_idle;
    c1.opaque  = &d1;
    c2.opaque  = &d2;
    c1.user    = "alice";
    c2.user    = "bob";
    ppp_client_init(&c1);
    ppp_client_init(&c2);

    n = isp_list_calls(calls, 8);
    check("two calls listed, waiting for PPP",
          (n == 2) && (calls[0].state == ISP_CALL_WAITING) && (calls[1].state == ISP_CALL_WAITING));
    check("...with the label the frontend gave", !strcmp(calls[0].label, "COM2 of the Win98 box"));

    check("both authenticate with PAP and come up", ppp_client_connect(&c1, 10000) && ppp_client_connect(&c2, 10000));
    sleep_ms(300);
    n = isp_list_calls(calls, 8);
    {
        const isp_call_info_t *a = find_call(calls, n, isp_session_number(d1.s));
        const isp_call_info_t *b = find_call(calls, n, isp_session_number(d2.s));

        check("listed on line, with their addresses",
              a && b && (a->state == ISP_CALL_ONLINE) && (b->state == ISP_CALL_ONLINE) &&
              (a->guest_ip == c1.my_ip) && (b->guest_ip == c2.my_ip));
        check("...the names they gave PAP", a && b && !strcmp(a->user, "alice") && !strcmp(b->user, "bob"));
        check("...and the bytes each way", a && (a->bytes_from_guest > 0) && (a->bytes_to_guest > 0));
    }

    /* Guest LAN, on by default: guest 1 reaches guest 2 at its address. */
    {
        uint8_t got[64];
        int     len;

        ppp_client_send_udp(&c1, c2.my_ip, 4100, 4200, (const uint8_t *) "hi neighbour", 12);
        len = ppp_client_recv_udp(&c2, 4200, got, sizeof(got), NULL, NULL, 2000);
        check("guest LAN: one guest reaches the other", (len == 12) && !memcmp(got, "hi neighbour", 12));

        st           = old;
        st.guest_lan = 0;
        isp_set_settings(&st);
        ppp_client_send_udp(&c1, c2.my_ip, 4100, 4200, (const uint8_t *) "anyone?", 7);
        len = ppp_client_recv_udp(&c2, 4200, got, sizeof(got), NULL, NULL, 1000);
        check("...and not when it is switched off", len < 0);
        isp_set_settings(&old);
    }

    /* A UDP port forward: the host's port reaches the guest's. */
    {
        uint16_t           fport;
        SOCKET             probe = udp_listener(&fport); /* a free port, then let go */
        isp_forward_t      f     = { 1, 0, 0, 7777 };
        struct sockaddr_in to;
        uint8_t            got[64];
        int                len;

        closesocket(probe);
        f.host_port = fport;
        check("forwards set on a call by number", isp_set_call_forwards(isp_session_number(d1.s), &f, 1));
        check("...bound once the thread takes them up", forward_state(isp_session_number(d1.s), 0) == ISP_FORWARD_BOUND);
        memset(&to, 0, sizeof(to));
        to.sin_family      = AF_INET;
        to.sin_addr.s_addr = htonl(0x7f000001);
        to.sin_port        = htons(fport);
        sendto(hs, "knock knock", 11, 0, (struct sockaddr *) &to, sizeof(to));
        len = ppp_client_recv_udp(&c1, 7777, got, sizeof(got), NULL, NULL, 2000);
        check("...and the host's port reaches the guest's", (len == 11) && !memcmp(got, "knock knock", 11));

        /* The same host port for the other call: taken. */
        isp_set_call_forwards(isp_session_number(d2.s), &f, 1);
        check("the same host port on another call: failed, not hung",
              forward_state(isp_session_number(d2.s), 0) == ISP_FORWARD_FAILED);
        isp_set_call_forwards(isp_session_number(d2.s), NULL, 0);
        isp_set_call_forwards(isp_session_number(d1.s), NULL, 0);
    }

    /* Throttled to 19200 bit/s: 4000 bytes take about two seconds. */
    {
        struct sockaddr_in from;
        socklen_t          flen = sizeof(from);
        char               buf[1200];
        int                got  = 0;
        uint32_t           t0, ms;

        st              = old;
        st.throttle     = 1;
        st.default_rate = 19200;
        isp_set_settings(&st);
        ppp_client_send_udp(&c1, c1.isp_ip, 5555, hport, (const uint8_t *) "send", 4);
        for (int i = 0; (i < 3000) && (recvfrom(hs, buf, sizeof(buf), 0, (struct sockaddr *) &from, &flen) <= 0); i++)
            sleep_ms(1);
        memset(buf, 'x', sizeof(buf));
        t0 = ppp_client_ms();
        for (int i = 0; i < 4; i++)
            sendto(hs, buf, 1000, 0, (struct sockaddr *) &from, flen);
        for (int i = 0; i < 4; i++) {
            uint8_t back[1200];

            if (ppp_client_recv_udp(&c1, 5555, back, sizeof(back), NULL, NULL, 10000) == 1000)
                got++;
        }
        ms = ppp_client_ms() - t0;
        printf("    4000 bytes at 19200 bit/s: %u ms\n", ms);
        check("throttled: modem speed, not line speed", (got == 4) && (ms >= 1500) && (ms <= 5000));
        n = isp_list_calls(calls, 8);
        check("...and the status says so", find_call(calls, n, isp_session_number(d1.s)) &&
                                                (find_call(calls, n, isp_session_number(d1.s))->rate == 19200));
        isp_set_settings(&old);
    }

    /* The operator hangs up call 1: the guest is told, the call ends. */
    check("hang up a call by number", isp_hangup_call(isp_session_number(d1.s)));
    {
        const uint32_t until = ppp_client_ms() + 8000;

        while (!isp_session_ended(d1.s) && ((int32_t) (ppp_client_ms() - until) < 0)) {
            ppp_client_poll(&c1);
            sleep_ms(10);
        }
        check("...the guest got a Terminate-Request and the call ended", c1.terminated && isp_session_ended(d1.s));
    }
    check("the other call is still up", udp_round_trip(&c2, hs, hport, "still up"));
    check("no such call to hang up", !isp_hangup_call(200));

    isp_session_close(d1.s);
    isp_session_close(d2.s);
    check("closed calls leave the list", isp_list_calls(calls, 8) == 0);
    closesocket(hs);

    /* The forward notation. */
    {
        isp_forward_t f[4];
        char          back[128];
        const int     k = isp_forwards_parse("tcp:2121:21, udp:*:5000:5001 bogus tcp:0:1", f, 4);

        isp_forwards_format(f, k, back, sizeof(back));
        check("forwards parse and print back", (k == 2) && !strcmp(back, "tcp:2121:21 udp:*:5000:5001"));
    }
}

int
main(void)
{
#ifdef _WIN32
    WSADATA wsa;

    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    test_framing();
    test_session();
    test_isp();
    test_control();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed", failures,
           (failures == 1) ? "" : "s");
    return failures ? 1 : 0;
}
