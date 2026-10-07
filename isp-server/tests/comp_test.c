/*
 * 86Box-Next: CCP's compressors and decompressors (isp-server/ppp_comp.c),
 * each paired with itself: MPPC, MPPE at each strength, stateful and
 * stateless, the two together, Deflate, BSD-Compress at several code sizes
 * and Predictor-1.  Packets of text, of noise (sent as they are, the
 * receiver kept in step) and of repetition; then a lost packet, which the
 * decompressor must notice and recover from.  No test framework; non-zero
 * on failure.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ppp_comp.h"

#define MRU 1500

static int      failures;
static uint32_t rng = 12345;

static uint32_t
rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static size_t
make_packet(int i, uint8_t *p)
{
    static const char text[] = "GET /index.html HTTP/1.0\r\nHost: www.example.com\r\nUser-Agent: Mozilla/4.0 "
                               "(compatible; MSIE 5.0; Windows 98)\r\nAccept: */*\r\n\r\n";
    size_t n = 40 + (rnd() % 1400);

    switch (i % 4) {
        case 0: /* text, much like the last */
            for (size_t k = 0; k < n; k++)
                p[k] = (uint8_t) text[(k + (size_t) i) % (sizeof(text) - 1)];
            break;
        case 1: /* noise */
            for (size_t k = 0; k < n; k++)
                p[k] = (uint8_t) rnd();
            break;
        case 2: /* one byte, over and over */
            memset(p, 0x41 + (i % 7), n);
            break;
        default: /* an IP header's worth of sameness, then text */
            for (size_t k = 0; k < n; k++)
                p[k] = (uint8_t) ((k < 20) ? (0x45 + (int) k) : (text[k % 40] ^ (i & 3)));
            break;
    }
    return n;
}

typedef struct {
    ppp_comp_t *tx;
    ppp_comp_t *rx;
} pair_t;

/* One packet through: 1 if it came out right. */
static int
send_one(pair_t *pr, uint16_t proto, const uint8_t *p, size_t n, int lose, int *reset_needed)
{
    uint8_t  wire[MRU * 2 + 64];
    uint8_t  back[MRU + 64];
    uint16_t got_proto = 0;
    size_t   w         = ppp_comp_compress(pr->tx, proto, p, n, wire, sizeof(wire));
    int      r;

    if (w == 0) {
        /* Sent as it is. */
        if (lose)
            return 1;
        ppp_comp_incomp(pr->rx, proto, p, n);
        return 2;
    }
    if (lose)
        return 1;
    r = ppp_comp_decompress(pr->rx, wire, w, &got_proto, back, sizeof(back));
    if ((r == COMP_RESET) || (r == COMP_FAIL)) {
        *reset_needed = 1;
        return 1;
    }
    if (r == COMP_DROP)
        return 1;
    if ((got_proto != proto) || ((size_t) r != n) || memcmp(back, p, n))
        return 0;
    return 2; /* delivered */
}

static void
run(const char *name, const ppp_comp_params_t *prm, int lossy)
{
    pair_t  pr;
    uint8_t pkt[MRU];
    int     ok = 1, delivered = 0, recovered = 0, resets = 0;

    pr.tx = ppp_comp_new(prm, 1, MRU);
    pr.rx = ppp_comp_new(prm, 0, MRU);
    if ((pr.tx == NULL) || (pr.rx == NULL)) {
        printf("FAIL %s: would not start\n", name);
        failures++;
        ppp_comp_free(pr.tx);
        ppp_comp_free(pr.rx);
        return;
    }
    for (int i = 0; (i < 3000) && ok; i++) {
        const size_t n          = make_packet(i, pkt);
        const int    lose       = lossy && ((i % 397) == 200);
        int          need_reset = 0;
        const int    r          = send_one(&pr, (i % 9) ? 0x0021 : 0x002b, pkt, n, lose, &need_reset);

        if (r == 0) {
            printf("FAIL %s: packet %d came out wrong\n", name, i);
            ok = 0;
            break;
        }
        if (r == 2)
            delivered++;
        if (need_reset) {
            /* What CCP does: the peer's compressor resets (Reset-Request),
               then ours hears the Reset-Ack. */
            resets++;
            ppp_comp_reset(pr.tx);
            ppp_comp_reset(pr.rx);
        } else if (lose)
            recovered++;
    }
    {
        uint64_t in, out;

        ppp_comp_stats(pr.tx, &in, &out);
        if (ok && (delivered < 1500)) {
            printf("FAIL %s: only %d packets came through\n", name, delivered);
            ok = 0;
        }
        if (ok && lossy && (resets == 0) && !(prm->mppx & MPPX_H) && (prm->type != CCP_OPT_PRED1)) {
            printf("FAIL %s: a lost packet went unnoticed\n", name);
            ok = 0;
        }
        if (ok)
            printf("ok   %-40s %s; %d delivered, %d resets; %llu -> %llu bytes\n", name, ppp_comp_describe(pr.tx),
                   delivered, resets, (unsigned long long) in, (unsigned long long) out);
    }
    if (!ok)
        failures++;
    (void) recovered;
    ppp_comp_free(pr.tx);
    ppp_comp_free(pr.rx);
}

int
main(void)
{
    ppp_comp_params_t p;

    for (int lossy = 0; lossy < 2; lossy++) {
        const char *sfx = lossy ? " (lossy)" : "";
        char        name[64];

        memset(&p, 0, sizeof(p));
        p.type = CCP_OPT_MPPX;
        p.mppx = MPPX_C;
        snprintf(name, sizeof(name), "MPPC%s", sfx);
        run(name, &p, lossy);
        p.mppx = MPPX_C | MPPX_H;
        snprintf(name, sizeof(name), "MPPC stateless%s", sfx);
        run(name, &p, lossy);

        for (int k = 0; k < 16; k++)
            p.key[k] = (uint8_t) (k * 7 + 3);
        {
            static const uint32_t strengths[3] = { MPPX_L, MPPX_M, MPPX_S };

            for (int s = 0; s < 3; s++)
                for (int h = 0; h < 2; h++)
                    for (int mppc = 0; mppc < 2; mppc++) {
                        p.mppx = strengths[s] | (h ? MPPX_H : 0) | (mppc ? MPPX_C : 0);
                        snprintf(name, sizeof(name), "MPPE %s%s%s%s", (s == 0) ? "40" : (s == 1) ? "56" : "128",
                                 h ? " stateless" : "", mppc ? " +MPPC" : "", sfx);
                        run(name, &p, lossy);
                    }
        }

        memset(&p, 0, sizeof(p));
        p.type = CCP_OPT_DEFLATE;
        for (int w = 9; w <= 15; w += 3) {
            p.bits = w;
            snprintf(name, sizeof(name), "Deflate %d%s", w, sfx);
            run(name, &p, lossy);
        }
        p.type = CCP_OPT_BSD;
        for (int b = 9; b <= 15; b += 2) {
            p.bits = b;
            snprintf(name, sizeof(name), "BSD-Compress %d%s", b, sfx);
            run(name, &p, lossy);
        }
        p.type = CCP_OPT_PRED1;
        p.bits = 0;
        snprintf(name, sizeof(name), "Predictor-1%s", sfx);
        run(name, &p, lossy);
    }

    /* MPPE never lets a packet through that does not say it is encrypted. */
    {
        uint8_t     in[8] = { 0x00, 0x05, 0x00, 0x21, 0x45, 0, 0, 0 };
        uint8_t     out[64];
        uint16_t    proto;
        ppp_comp_t *rx;

        memset(&p, 0, sizeof(p));
        p.type = CCP_OPT_MPPX;
        p.mppx = MPPX_S | MPPX_H;
        rx     = ppp_comp_new(&p, 0, MRU);
        if (ppp_comp_decompress(rx, in, sizeof(in), &proto, out, sizeof(out)) >= 0) {
            printf("FAIL MPPE took an unencrypted packet\n");
            failures++;
        } else
            printf("ok   MPPE refuses an unencrypted packet\n");
        ppp_comp_free(rx);
    }

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "all checks passed", failures);
    return failures ? 1 : 0;
}
