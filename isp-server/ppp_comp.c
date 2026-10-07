/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             CCP's compressors and decompressors.  See ppp_comp.h.
 *
 *             MPPC and MPPE share option 18 and one header, |A|B|C|D| and
 *             a 12-bit coherency count: compressed (C) with an 8 KB sliding
 *             history (B: moved to the front, A: flushed), then encrypted
 *             (D) with RC4 under keys that change every packet (stateless)
 *             or every 256 (stateful).  Deflate is zlib's raw deflate, a
 *             sync flush per packet, its 00 00 FF FF taken off and put
 *             back.  BSD-Compress follows RFC 1977's appendix code, which
 *             is its definition: both ends must clear the dictionary at the
 *             same moments, so the arithmetic is kept exactly.  Predictor-1
 *             is RFC 1978's guess table.
 *
 *             BSD-Compress: derived from RFC 1977's appendix, itself from
 *             the 4.3BSD compress command: Copyright (c) 1985, 1986 The
 *             Regents of the University of California; contributed to
 *             Berkeley by James A. Woods, from original work by Spencer
 *             Thomas and Joseph Orost.  Used under the BSD license (its
 *             advertising clause rescinded by the University in 1999).
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "isp_crypto.h"
#include "ppp_auth.h"
#include "ppp_comp.h"
#include "ppp_framing.h"

#define MPPC_HIST      8192
#define MPPC_HASH_BITS 12
#define MPPC_MAX_MATCH 8191
#define CCOUNT_MASK    0x0fff

#define HDR_A 0x80 /* flushed            */
#define HDR_B 0x40 /* at the front       */
#define HDR_C 0x20 /* compressed         */
#define HDR_D 0x10 /* encrypted          */

/* BSD-Compress. */
#define BSD_MIN_BITS   9
#define BSD_MAX_BITS   15
#define BSD_CLEAR      256
#define BSD_FIRST      257
#define BSD_LAST       255
#define BSD_MAXCODE(b) ((1u << (b)) - 1)
#define BSD_BADCODEM1  BSD_MAXCODE(BSD_MAX_BITS)
#define BSD_HASH(prefix, suffix, hshift) ((((uint32_t) (suffix)) << (hshift)) ^ (uint32_t) (prefix))
#define BSD_KEY(prefix, suffix)          ((((uint32_t) (suffix)) << 16) + (uint32_t) (prefix))
#define BSD_CHECK_GAP       10000
#define BSD_RATIO_SCALE_LOG 8
#define BSD_RATIO_SCALE     (1u << BSD_RATIO_SCALE_LOG)
#define BSD_RATIO_MAX       (0x7fffffffu >> BSD_RATIO_SCALE_LOG)

typedef struct {
    uint32_t fcode;  /* suffix << 16 | prefix */
    uint16_t codem1; /* code - 1 */
    uint16_t cptr;   /* code -> hash slot */
} bsd_dict_t;

typedef struct {
    uint32_t    hsize;
    uint32_t    hshift;
    uint32_t    n_bits;
    uint16_t    seqno;
    uint32_t    maxmaxcode;
    uint32_t    max_ent;
    uint32_t    in_count;
    uint32_t    bytes_out;
    uint32_t    ratio;
    uint32_t    checkpoint;
    uint16_t   *lens; /* decompressor only */
    bsd_dict_t *dict;
} bsd_db_t;

struct ppp_comp {
    int      type;
    int      compressor;
    size_t   mru;
    uint64_t in_bytes;
    uint64_t out_bytes;
    char     desc[64];

    /* Option 18. */
    int      mppc;
    int      mppe;
    int      stateless;
    size_t   keylen;   /* 8 or 16 */
    int      keybits;  /* 40, 56, 128 */
    uint8_t  start_key[16];
    uint8_t  session_key[16];
    rc4_t    rc4;
    uint16_t ccount;
    int      flush;     /* compressor: A on the next packet (after a raw MPPC packet) */
    int      reset_req; /* compressor: a Reset-Request came; A and a new key next */
    int      discard;  /* decompressor: lost a packet; dropping until A */
    uint8_t *hist;
    size_t   hpos;
    uint16_t *htab;
    uint8_t  *work;

    /* Deflate. */
    z_stream z;
    int      z_ready;
    int      window;
    uint16_t seq;
    int      need_reset; /* decompressor: waiting for a Reset-Ack */

    /* BSD-Compress. */
    bsd_db_t bsd;

    /* Predictor-1. */
    uint8_t *guess;
    uint16_t phash;
    uint8_t *squeezed;
};

static const char *const comp_keys[PPP_COMP_COUNT]  = { "mppc", "deflate", "bsd", "predictor1" };
static const char *const comp_names[PPP_COMP_COUNT] = { "MPPC", "Deflate", "BSD-Compress", "Predictor-1" };

const char *
ppp_comp_key(int c)
{
    return ((c >= 0) && (c < PPP_COMP_COUNT)) ? comp_keys[c] : "?";
}

const char *
ppp_comp_name(int c)
{
    return ((c >= 0) && (c < PPP_COMP_COUNT)) ? comp_names[c] : "?";
}

/* The protocol field first: one byte if it fits (RFC 1661's PFC form). */
static size_t
put_proto(uint8_t *out, uint16_t proto, int may_compress)
{
    if (may_compress && (proto < 0x100)) {
        out[0] = (uint8_t) proto;
        return 1;
    }
    out[0] = (uint8_t) (proto >> 8);
    out[1] = (uint8_t) proto;
    return 2;
}

static int
get_proto(const uint8_t *in, size_t len, uint16_t *proto)
{
    if (len < 1)
        return 0;
    if (in[0] & 1) {
        *proto = in[0];
        return 1;
    }
    if ((len < 2) || !(in[1] & 1))
        return 0;
    *proto = (uint16_t) ((in[0] << 8) | in[1]);
    return 2;
}

/* ------------------------------------------------------------------ bits */

typedef struct {
    uint8_t *buf;
    size_t   max;
    size_t   len;
    uint32_t acc;
    int      n;
    int      overflow;
} bitw_t;

static void
bw_put(bitw_t *w, uint32_t v, int bits)
{
    while (bits > 0) {
        const int take = (bits > 8) ? 8 : bits;

        bits -= take;
        w->acc = (w->acc << take) | ((v >> bits) & ((1u << take) - 1));
        w->n += take;
        while (w->n >= 8) {
            w->n -= 8;
            if (w->len < w->max)
                w->buf[w->len++] = (uint8_t) (w->acc >> w->n);
            else
                w->overflow = 1;
        }
    }
}

static void
bw_flush(bitw_t *w)
{
    if (w->n > 0)
        bw_put(w, 0, 8 - w->n);
}

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos; /* in bits */
} bitr_t;

static size_t
br_left(const bitr_t *r)
{
    return (r->len * 8) - r->pos;
}

static uint32_t
br_get(bitr_t *r, int bits)
{
    uint32_t v = 0;

    for (int i = 0; i < bits; i++) {
        const size_t p = r->pos++;

        v = (v << 1) | ((r->buf[p >> 3] >> (7 - (p & 7))) & 1);
    }
    return v;
}

/* ------------------------------------------------------------------ MPPC */

static void
mppc_reset(ppp_comp_t *c)
{
    memset(c->hist, 0, MPPC_HIST);
    if (c->htab != NULL)
        memset(c->htab, 0, sizeof(uint16_t) << MPPC_HASH_BITS);
    c->hpos = 0;
}

static unsigned
mppc_hash(const uint8_t *p)
{
    return (((unsigned) p[0] << 7) ^ ((unsigned) p[1] << 3) ^ p[2] ^ ((unsigned) p[0] >> 2)) &
           ((1u << MPPC_HASH_BITS) - 1);
}

static void
mppc_put_copy(bitw_t *w, size_t off, size_t len)
{
    if (off < 64)
        bw_put(w, 0x3c0 | (uint32_t) off, 10); /* 1111 + 6 bits */
    else if (off < 320)
        bw_put(w, 0xe00 | (uint32_t) (off - 64), 12); /* 1110 + 8 bits */
    else
        bw_put(w, 0xc000 | (uint32_t) (off - 320), 16); /* 110 + 13 bits */

    if (len == 3)
        bw_put(w, 0, 1);
    else {
        int n = 1; /* len < 1 << (n + 2) */

        while (len >= (1u << (n + 2)))
            n++;
        /* n ones, a zero, then n + 1 bits of the length */
        bw_put(w, ((1u << n) - 1) << 1, n + 1);
        bw_put(w, (uint32_t) len & ((1u << (n + 1)) - 1), n + 1);
    }
}

/* Compresses hist[hpos..hpos+len), which is already in place.  Returns the
   bytes written, or 0 if they would not be fewer than `len`. */
static size_t
mppc_compress_run(ppp_comp_t *c, size_t start, size_t len, uint8_t *out, size_t outmax)
{
    bitw_t       w   = { out, (outmax < len) ? outmax : len, 0, 0, 0, 0 };
    const size_t end = start + len;
    size_t       i   = start;

    while ((i < end) && !w.overflow) {
        if ((end - i) >= 3) {
            const unsigned h    = mppc_hash(&c->hist[i]);
            const size_t   cand = c->htab[h] ? (size_t) (c->htab[h] - 1) : (size_t) -1;

            c->htab[h] = (uint16_t) (i + 1);
            if ((cand < i) && ((i - cand) <= MPPC_MAX_MATCH) && !memcmp(&c->hist[cand], &c->hist[i], 3)) {
                size_t m = 3;

                while (((i + m) < end) && (m < MPPC_MAX_MATCH) && (c->hist[cand + m] == c->hist[i + m]))
                    m++;
                mppc_put_copy(&w, i - cand, m);
                for (size_t k = 1; (k < m) && ((i + k + 3) <= end); k++)
                    c->htab[mppc_hash(&c->hist[i + k])] = (uint16_t) (i + k + 1);
                i += m;
                continue;
            }
        }
        if (c->hist[i] < 0x80)
            bw_put(&w, c->hist[i], 8);
        else
            bw_put(&w, 0x100 | (c->hist[i] & 0x7f), 9); /* 10 + 7 bits */
        i++;
    }
    bw_flush(&w);
    if (w.overflow || (w.len >= len))
        return 0;
    return w.len;
}

/* Decompresses into hist at hpos; the output is hist[start..hpos). */
static int
mppc_decompress_run(ppp_comp_t *c, const uint8_t *in, size_t len, size_t *start)
{
    bitr_t r = { in, len, 0 };

    *start = c->hpos;
    while (br_left(&r) >= 8) {
        size_t off;
        size_t mlen;

        if (br_get(&r, 1) == 0) { /* 0 + 7 bits: a literal below 0x80 */
            if (c->hpos >= MPPC_HIST)
                return 0;
            c->hist[c->hpos++] = (uint8_t) br_get(&r, 7);
            continue;
        }
        if (br_get(&r, 1) == 0) { /* 10 + 7 bits: a literal from 0x80 */
            if ((br_left(&r) < 7) || (c->hpos >= MPPC_HIST))
                return 0;
            c->hist[c->hpos++] = (uint8_t) (0x80 | br_get(&r, 7));
            continue;
        }
        /* 11: a copy, the offset first. */
        if (br_left(&r) < 1)
            return 0;
        if (br_get(&r, 1) == 0) { /* 110 + 13 bits */
            if (br_left(&r) < 13)
                return 0;
            off = br_get(&r, 13) + 320;
        } else {
            if (br_left(&r) < 1)
                return 0;
            if (br_get(&r, 1) == 0) { /* 1110 + 8 bits */
                if (br_left(&r) < 8)
                    return 0;
                off = br_get(&r, 8) + 64;
            } else { /* 1111 + 6 bits */
                if (br_left(&r) < 6)
                    return 0;
                off = br_get(&r, 6);
            }
        }
        {
            int n = 0;

            while ((n < 12) && (br_left(&r) > 0) && br_get(&r, 1))
                n++;
            if (n == 0)
                mlen = 3;
            else {
                if ((n > 11) || (br_left(&r) < (size_t) (n + 1)))
                    return 0;
                mlen = ((size_t) 1 << (n + 1)) | br_get(&r, n + 1);
            }
        }
        if ((off == 0) || (off > c->hpos) || ((c->hpos + mlen) > MPPC_HIST))
            return 0;
        for (size_t k = 0; k < mlen; k++, c->hpos++)
            c->hist[c->hpos] = c->hist[c->hpos - off];
    }
    return 1;
}

/* ------------------------------------------------------------------ MPPE */

static void
mppe_reduce(ppp_comp_t *c, uint8_t *key)
{
    if (c->keybits == 40) {
        key[0] = 0xd1;
        key[1] = 0x26;
        key[2] = 0x9e;
    } else if (c->keybits == 56)
        key[0] = 0xd1;
}

static void
mppe_initial_key(ppp_comp_t *c)
{
    mppe_new_key_from_sha(c->start_key, c->start_key, c->keylen, c->session_key);
    mppe_reduce(c, c->session_key);
    rc4_init(&c->rc4, c->session_key, c->keylen);
}

/* RFC 3078 7.3. */
static void
mppe_key_change(ppp_comp_t *c)
{
    uint8_t interim[16];
    rc4_t   r;

    mppe_new_key_from_sha(c->start_key, c->session_key, c->keylen, interim);
    rc4_init(&r, interim, c->keylen);
    rc4_crypt(&r, interim, c->session_key, c->keylen);
    mppe_reduce(c, c->session_key);
    rc4_init(&c->rc4, c->session_key, c->keylen);
    crypto_wipe(interim, sizeof(interim));
    crypto_wipe(&r, sizeof(r));
}

static size_t
mppx_compress(ppp_comp_t *c, uint16_t proto, const uint8_t *data, size_t len, uint8_t *out, size_t outmax)
{
    size_t   plen  = len + 2;
    uint8_t   flags = 0;
    uint8_t  *payload;
    int       flushed;
    int       rekey;
    int       flag;

    if (outmax < (plen + 2))
        return 0;
    c->ccount = (uint16_t) ((c->ccount + 1) & CCOUNT_MASK);
    flag      = (c->ccount & 0xff) == 0xff;
    /* FLUSHED: stateless (every packet); a "flag" packet, whose key
       changes; the first after a Reset-Request; the first after a raw MPPC
       packet.  The key changes on the first three only (as Linux's MPPE,
       which works with Windows', has it), so that a receiver that lost
       packets can count them: flags by the coherency count, the reset by
       having asked for it.  An MPPC flush only starts the RC4 tables over
       from the current key, as RFC 3078 says of FLUSHED. */
    rekey    = c->stateless || (c->mppe && flag) || c->reset_req;
    flushed  = rekey || c->flush;
    c->flush = c->reset_req = 0;
    payload  = out + 2;
    if (flushed)
        flags |= HDR_A;

    if (c->mppc) {
        size_t start;
        size_t n;

        if (flushed)
            mppc_reset(c);
        if ((c->hpos + plen) > MPPC_HIST) {
            c->hpos = 0;
            flags |= HDR_B;
        }
        start              = c->hpos;
        c->hist[start]     = (uint8_t) (proto >> 8);
        c->hist[start + 1] = (uint8_t) proto;
        memcpy(&c->hist[start + 2], data, len);
        n       = mppc_compress_run(c, start, plen, payload, outmax - 2);
        c->hpos = start + plen;
        if (n > 0) {
            flags |= HDR_C;
            plen = n;
        } else {
            /* It would grow: as it is, and the history starts over with the
               next packet. */
            memcpy(payload, &c->hist[start], plen);
            flags &= (uint8_t) ~HDR_B;
            c->flush = 1;
        }
    } else {
        payload[0] = (uint8_t) (proto >> 8);
        payload[1] = (uint8_t) proto;
        memcpy(payload + 2, data, len);
    }

    if (c->mppe) {
        if (rekey)
            mppe_key_change(c);
        else if (flushed)
            rc4_init(&c->rc4, c->session_key, c->keylen);
        flags |= HDR_D;
        rc4_crypt(&c->rc4, payload, payload, plen);
    }
    out[0] = (uint8_t) (flags | ((c->ccount >> 8) & 0x0f));
    out[1] = (uint8_t) c->ccount;
    return plen + 2;
}

static int
mppx_decompress(ppp_comp_t *c, const uint8_t *in, size_t len, uint16_t *proto, uint8_t *out, size_t outmax)
{
    uint8_t        flags;
    uint16_t       ccount;
    const uint8_t *data;
    size_t         dlen;
    size_t         hdr;

    if (len < 3)
        return COMP_DROP;
    flags  = in[0] & 0xf0;
    ccount = (uint16_t) (((in[0] & 0x0f) << 8) | in[1]);
    dlen   = len - 2;
    if ((dlen > c->mru + 2) || (dlen > outmax))
        return COMP_DROP;
    memcpy(c->work, in + 2, dlen);
    data = c->work;

    if (c->mppe) {
        if (!(flags & HDR_D))
            return COMP_DROP; /* not encrypted: never let it through */
        if (c->stateless) {
            int n = (ccount - c->ccount) & CCOUNT_MASK;

            if (n == 0)
                return COMP_DROP; /* the same count again */
            while (n-- > 0)
                mppe_key_change(c);
            c->ccount = ccount;
        } else {
            /* A flag packet always says FLUSHED. */
            if (((ccount & 0xff) == 0xff) && !(flags & HDR_A))
                return COMP_DROP;
            if (!c->discard && (ccount != ((c->ccount + 1) & CCOUNT_MASK)) && !(flags & HDR_A)) {
                c->discard = 1;
                return COMP_RESET;
            }
            {
                const int flag    = (ccount & 0xff) == 0xff;
                int       answer  = 0; /* the first FLUSHED after our Reset-Request */

                if (c->discard || (ccount != ((c->ccount + 1) & CCOUNT_MASK))) {
                    /* After a loss: a FLUSHED packet is where both ends meet
                       again (and one right after the gap needs no reset). */
                    if (!(flags & HDR_A))
                        return COMP_DROP;
                    /* A key change for every flag packet missed. */
                    while ((ccount & ~0xff) != (c->ccount & ~0xff)) {
                        mppe_key_change(c);
                        c->ccount = (uint16_t) ((c->ccount + 256) & CCOUNT_MASK);
                    }
                    answer     = c->discard;
                    c->discard = 0;
                }
                c->ccount = ccount;
                /* A new key on a flag packet and on the answer to a reset;
                   on any other FLUSHED (an MPPC flush) the tables start over
                   from the key there is. */
                if (flag || answer)
                    mppe_key_change(c);
                else if (flags & HDR_A)
                    rc4_init(&c->rc4, c->session_key, c->keylen);
            }
        }
        rc4_crypt(&c->rc4, c->work, c->work, dlen);
    } else if (!c->stateless) {
        /* MPPC alone: the count keeps the histories together. */
        if (c->discard && !(flags & HDR_A))
            return COMP_DROP;
        if (!(flags & HDR_A) && (ccount != ((c->ccount + 1) & CCOUNT_MASK))) {
            c->discard = 1;
            c->ccount  = ccount;
            return COMP_RESET;
        }
        c->discard = 0;
        c->ccount  = ccount;
    }

    if (c->mppc) {
        if (flags & HDR_A)
            mppc_reset(c);
        if (flags & HDR_B)
            c->hpos = 0;
        if (flags & HDR_C) {
            size_t start;

            if (!mppc_decompress_run(c, data, dlen, &start)) {
                c->discard = 1;
                return COMP_RESET;
            }
            dlen = c->hpos - start;
            if (dlen > outmax)
                return COMP_DROP;
            memcpy(out, &c->hist[start], dlen);
            data = out;
        }
    }
    if (data != out)
        memcpy(out, data, dlen);
    hdr = (size_t) get_proto(out, dlen, proto);
    if (hdr == 0)
        return COMP_DROP;
    memmove(out, out + hdr, dlen - hdr);
    return (int) (dlen - hdr);
}

/* --------------------------------------------------------------- Deflate */

static size_t
deflate_compress(ppp_comp_t *c, uint16_t proto, const uint8_t *data, size_t len, uint8_t *out, size_t outmax)
{
    uint8_t      hdr[2];
    const size_t hl = put_proto(hdr, proto, 1);
    size_t       olen;

    if (outmax < 8)
        return 0;
    out[0] = (uint8_t) (c->seq >> 8);
    out[1] = (uint8_t) c->seq;
    c->seq++;

    c->z.next_out  = out + 2;
    c->z.avail_out = (uInt) (outmax - 2);
    c->z.next_in   = hdr;
    c->z.avail_in  = (uInt) hl;
    if (deflate(&c->z, Z_NO_FLUSH) != Z_OK)
        goto broken;
    c->z.next_in  = (Bytef *) (uintptr_t) data;
    c->z.avail_in = (uInt) len;
    if ((deflate(&c->z, Z_SYNC_FLUSH) != Z_OK) || (c->z.avail_out == 0))
        goto broken;
    olen = (size_t) (c->z.next_out - (out + 2));
    if ((olen >= 4) && !memcmp(out + 2 + olen - 4, "\x00\x00\xff\xff", 4))
        olen -= 4;
    /* The history has it either way; bigger goes as it is. */
    if ((olen + 2) >= (len + hl))
        return 0;
    return olen + 2;

broken:
    /* Out of room: whatever is half out cannot be taken back, so start
       over.  The peer finds the sequence broken and asks for a reset. */
    deflateReset(&c->z);
    return 0;
}

static int
deflate_decompress(ppp_comp_t *c, const uint8_t *in, size_t len, uint16_t *proto, uint8_t *out, size_t outmax)
{
    static const uint8_t tail[4] = { 0x00, 0x00, 0xff, 0xff };
    uint16_t             seq;
    int                  r;
    size_t               n;
    size_t               hdr;

    if (c->need_reset)
        return COMP_RESET;
    if (len < 3)
        return COMP_DROP;
    seq = (uint16_t) ((in[0] << 8) | in[1]);
    if (seq != c->seq) {
        c->need_reset = 1;
        return COMP_RESET;
    }
    c->seq++;

    c->z.next_out  = out;
    c->z.avail_out = (uInt) outmax;
    c->z.next_in   = (Bytef *) (uintptr_t) (in + 2);
    c->z.avail_in  = (uInt) (len - 2);
    r              = inflate(&c->z, Z_SYNC_FLUSH);
    /* The sync flush's empty stored block, which the sender took off --
       unless it did not. */
    if (((r == Z_OK) || (r == Z_BUF_ERROR)) && !((len >= 6) && !memcmp(in + len - 4, tail, 4))) {
        c->z.next_in  = (Bytef *) (uintptr_t) tail;
        c->z.avail_in = 4;
        r             = inflate(&c->z, Z_SYNC_FLUSH);
    }
    if (((r != Z_OK) && (r != Z_BUF_ERROR)) || (c->z.avail_in != 0)) {
        c->need_reset = 1;
        return COMP_RESET;
    }
    n   = outmax - c->z.avail_out;
    hdr = (size_t) get_proto(out, n, proto);
    if (hdr == 0)
        return COMP_DROP;
    memmove(out, out + hdr, n - hdr);
    return (int) (n - hdr);
}

/* An uncompressed packet into the history, as a stored block. */
static void
deflate_incomp(ppp_comp_t *c, uint16_t proto, const uint8_t *data, size_t len)
{
    uint8_t      blk[5 + 2];
    const size_t hl  = put_proto(blk + 5, proto, 1);
    const size_t all = hl + len;
    uint8_t      sink[2048];

    if (c->need_reset || (all > 0xffff))
        return;
    c->seq++;
    blk[0] = 0x00; /* not final, stored */
    blk[1] = (uint8_t) all;
    blk[2] = (uint8_t) (all >> 8);
    blk[3] = (uint8_t) ~all;
    blk[4] = (uint8_t) (~all >> 8);

    c->z.next_in  = blk;
    c->z.avail_in = (uInt) (5 + hl);
    do {
        c->z.next_out  = sink;
        c->z.avail_out = sizeof(sink);
        inflate(&c->z, Z_SYNC_FLUSH);
    } while (c->z.avail_in > 0 && c->z.avail_out == 0);
    c->z.next_in  = (Bytef *) (uintptr_t) data;
    c->z.avail_in = (uInt) len;
    do {
        c->z.next_out  = sink;
        c->z.avail_out = sizeof(sink);
        if (inflate(&c->z, Z_SYNC_FLUSH) < 0) {
            c->need_reset = 1;
            return;
        }
    } while (c->z.avail_in > 0);
}

/* ---------------------------------------------------------- BSD-Compress */

static void
bsd_clear(bsd_db_t *db)
{
    db->max_ent    = BSD_FIRST - 1;
    db->n_bits     = BSD_MIN_BITS;
    db->ratio      = 0;
    db->bytes_out  = 0;
    db->in_count   = 0;
    db->checkpoint = BSD_CHECK_GAP;
}

/* 1 when the dictionary was cleared (and the compressor sends CLEAR). */
static int
bsd_check(bsd_db_t *db)
{
    if (db->in_count >= db->checkpoint) {
        if ((db->in_count >= BSD_RATIO_MAX) || (db->bytes_out >= BSD_RATIO_MAX)) {
            db->in_count -= db->in_count / 4;
            db->bytes_out -= db->bytes_out / 4;
        }
        db->checkpoint = db->in_count + BSD_CHECK_GAP;
        if (db->max_ent >= db->maxmaxcode) {
            uint32_t new_ratio = db->in_count << BSD_RATIO_SCALE_LOG;

            if (db->bytes_out != 0)
                new_ratio /= db->bytes_out;
            if ((new_ratio < db->ratio) || (new_ratio < 1 * BSD_RATIO_SCALE)) {
                bsd_clear(db);
                return 1;
            }
            db->ratio = new_ratio;
        }
    }
    return 0;
}

static int
bsd_init(bsd_db_t *db, int bits, int decomp)
{
    switch (bits) {
        case 9:
        case 10:
        case 11:
        case 12:
            db->hsize  = 5003;
            db->hshift = 4;
            break;
        case 13:
            db->hsize  = 9001;
            db->hshift = 5;
            break;
        case 14:
            db->hsize  = 18013;
            db->hshift = 6;
            break;
        case 15:
            db->hsize  = 35023;
            db->hshift = 7;
            break;
        default:
            return 0;
    }
    db->maxmaxcode = BSD_MAXCODE(bits);
    db->dict       = (bsd_dict_t *) calloc(db->hsize, sizeof(bsd_dict_t));
    if (db->dict == NULL)
        return 0;
    if (decomp) {
        db->lens = (uint16_t *) calloc(db->maxmaxcode + 1, sizeof(uint16_t));
        if (db->lens == NULL)
            return 0;
        for (int i = 0; i <= BSD_LAST; i++)
            db->lens[i] = 1;
    }
    for (uint32_t i = 0; i < db->hsize; i++) {
        db->dict[i].codem1 = BSD_BADCODEM1;
        db->dict[i].cptr   = 0;
    }
    db->seqno = 0;
    bsd_clear(db);
    return 1;
}

static void
bsd_reset(bsd_db_t *db)
{
    db->seqno = 0;
    bsd_clear(db);
}

/* The dictionary's new entry for (ent, c) at hash slot hval. */
static void
bsd_add(bsd_db_t *db, bsd_dict_t *dictp, uint32_t hval, uint32_t fcode, uint32_t *n_bits, int decomp_lens_from)
{
    bsd_dict_t *dictp2;

    if (db->max_ent >= BSD_MAXCODE(*n_bits))
        db->n_bits = ++(*n_bits);
    dictp2 = &db->dict[db->max_ent + 1];
    if (db->dict[dictp2->cptr].codem1 == db->max_ent)
        db->dict[dictp2->cptr].codem1 = BSD_BADCODEM1;
    dictp2->cptr   = (uint16_t) hval;
    dictp->codem1  = (uint16_t) db->max_ent;
    dictp->fcode   = fcode;
    db->max_ent++;
    if ((decomp_lens_from >= 0) && (db->lens != NULL))
        db->lens[db->max_ent] = (uint16_t) (db->lens[decomp_lens_from] + 1);
}

/* Finds (ent, c); returns its code, or -1 with *hval at the free slot. */
static int
bsd_find(bsd_db_t *db, uint32_t ent, uint8_t c, uint32_t *hvalp, uint32_t *fcodep)
{
    const uint32_t fcode = BSD_KEY(ent, c);
    uint32_t       hval  = BSD_HASH(ent, c, db->hshift);
    bsd_dict_t    *dictp = &db->dict[hval];

    *fcodep = fcode;
    if (dictp->codem1 < db->max_ent) {
        if (dictp->fcode == fcode) {
            *hvalp = hval;
            return dictp->codem1 + 1;
        }
        {
            const uint32_t disp = (hval == 0) ? 1 : hval;

            for (;;) {
                hval += disp;
                if (hval >= db->hsize)
                    hval -= db->hsize;
                dictp = &db->dict[hval];
                if (dictp->codem1 >= db->max_ent)
                    break;
                if (dictp->fcode == fcode) {
                    *hvalp = hval;
                    return dictp->codem1 + 1;
                }
            }
        }
    }
    *hvalp = hval;
    return -1;
}

static size_t
bsd_compress(ppp_comp_t *c, uint16_t proto, const uint8_t *data, size_t len, uint8_t *out, size_t outmax)
{
    bsd_db_t *db     = &c->bsd;
    uint32_t  n_bits = db->n_bits;
    uint32_t  bitno  = 32;
    uint32_t  accum  = 0;
    uint32_t  ent    = proto;
    size_t    wpos   = 2;
    int       overflow = 0;

#define BSD_OUTPUT(code)                                \
    do {                                                \
        bitno -= n_bits;                                \
        accum |= ((uint32_t) (code) << bitno);          \
        do {                                            \
            if (wpos < outmax)                          \
                out[wpos++] = (uint8_t) (accum >> 24);  \
            else                                        \
                overflow = 1;                           \
            accum <<= 8;                                \
            bitno += 8;                                 \
        } while (bitno <= 24);                          \
    } while (0)

    if ((proto < 0x21) || (proto > 0xf9) || (outmax < 4))
        return 0;
    db->in_count++;
    out[0] = (uint8_t) (db->seqno >> 8);
    out[1] = (uint8_t) db->seqno;
    db->seqno++;
    db->in_count += (uint32_t) len;

    for (size_t i = 0; i < len; i++) {
        const uint8_t ch = data[i];
        uint32_t      hval;
        uint32_t      fcode;
        const int     code = bsd_find(db, ent, ch, &hval, &fcode);

        if (code >= 0) {
            ent = (uint32_t) code;
            continue;
        }
        BSD_OUTPUT(ent);
        if (db->max_ent < db->maxmaxcode)
            bsd_add(db, &db->dict[hval], hval, fcode, &n_bits, -1);
        ent = ch;
    }
    BSD_OUTPUT(ent);
    db->bytes_out += (uint32_t) ((wpos - 2) + (32 - bitno + 7) / 8);
    if (bsd_check(db))
        BSD_OUTPUT(BSD_CLEAR); /* at the width before the clear */
    if (bitno != 32) {
        if (wpos < outmax)
            out[wpos++] = (uint8_t) ((accum | (0xffu << (bitno - 8))) >> 24);
        else
            overflow = 1;
    }
    if ((db->max_ent >= BSD_MAXCODE(db->n_bits)) && (db->max_ent < db->maxmaxcode))
        db->n_bits++;
#undef BSD_OUTPUT

    /* Bigger than it was: as it is (the dictionary is in step all the same,
       the peer pretends to compress it too). */
    if (overflow || (wpos >= (len + 1)))
        return 0;
    return wpos;
}

static void
bsd_incomp(ppp_comp_t *c, uint16_t proto, const uint8_t *data, size_t len)
{
    bsd_db_t *db     = &c->bsd;
    uint32_t  n_bits = db->n_bits;
    uint32_t  bitno  = 7;
    uint32_t  ent    = proto;

    if ((proto < 0x21) || (proto > 0xf9))
        return;
    db->in_count++;
    db->seqno++;
    db->in_count += (uint32_t) len;
    for (size_t i = 0; i < len; i++) {
        const uint8_t ch = data[i];
        uint32_t      hval;
        uint32_t      fcode;
        const int     code = bsd_find(db, ent, ch, &hval, &fcode);

        if (code >= 0) {
            ent = (uint32_t) code;
            continue;
        }
        bitno += n_bits;
        if (db->max_ent < db->maxmaxcode)
            bsd_add(db, &db->dict[hval], hval, fcode, &n_bits, (int) ent);
        ent = ch;
    }
    bitno += n_bits;
    db->bytes_out += bitno / 8;
    (void) bsd_check(db);
    if ((db->max_ent >= BSD_MAXCODE(db->n_bits)) && (db->max_ent < db->maxmaxcode))
        db->n_bits++;
}

static int
bsd_decompress(ppp_comp_t *c, const uint8_t *in, size_t len, uint16_t *proto, uint8_t *out, size_t outmax)
{
    bsd_db_t *db       = &c->bsd;
    uint32_t  max_ent  = db->max_ent;
    uint32_t  accum    = 0;
    uint32_t  bitno    = 32;
    uint32_t  n_bits   = db->n_bits;
    uint32_t  tgtbitno = 32 - n_bits;
    uint32_t  oldcode  = BSD_CLEAR;
    uint32_t  finchar  = 0;
    size_t    wpos     = 0;
    size_t    rpos;
    uint16_t  seq;
    int       cleared = 0;

    if (c->need_reset)
        return COMP_RESET;
    if (len < 3)
        return COMP_DROP;
    seq = (uint16_t) ((in[0] << 8) | in[1]);
    if (seq != db->seqno) {
        c->need_reset = 1;
        return COMP_RESET;
    }
    db->seqno++;
    rpos = 2;
    db->bytes_out += (uint32_t) (len - 2);

    while (rpos < len) {
        uint32_t incode;
        size_t   i;
        size_t   p;

        bitno -= 8;
        accum |= (uint32_t) in[rpos++] << bitno;
        if (tgtbitno < bitno)
            continue;
        incode = accum >> tgtbitno;
        accum <<= n_bits;
        bitno += n_bits;

        if (incode == BSD_CLEAR) {
            /* Only at the very end of a packet. */
            if (rpos != len) {
                c->need_reset = 1;
                return COMP_RESET;
            }
            bsd_clear(db);
            max_ent = db->max_ent;
            cleared = 1;
            break;
        }

        if (incode > max_ent) {
            /* KwKwK. */
            if ((incode > (max_ent + 2)) || (incode > db->maxmaxcode) || (oldcode == BSD_CLEAR)) {
                c->need_reset = 1;
                return COMP_RESET;
            }
            i = db->lens[oldcode];
            if ((wpos + i + 1) > outmax) {
                c->need_reset = 1;
                return COMP_RESET;
            }
            p           = (wpos += i);
            out[wpos++] = (uint8_t) finchar;
            finchar     = oldcode;
        } else {
            i = db->lens[finchar = incode];
            if ((wpos + i) > outmax) {
                c->need_reset = 1;
                return COMP_RESET;
            }
            p = (wpos += i);
        }
        while (finchar > BSD_LAST) {
            const bsd_dict_t *dictp = &db->dict[db->dict[finchar].cptr];

            out[--p] = (uint8_t) (dictp->fcode >> 16);
            finchar  = dictp->fcode & 0xffff;
        }
        out[--p] = (uint8_t) finchar;

        if ((oldcode != BSD_CLEAR) && (max_ent < db->maxmaxcode)) {
            const uint32_t fcode = BSD_KEY(oldcode, finchar);
            uint32_t       hval  = BSD_HASH(oldcode, finchar, db->hshift);
            bsd_dict_t    *dictp = &db->dict[hval];
            bsd_dict_t    *dictp2;

            if (dictp->codem1 < max_ent) {
                const uint32_t disp = (hval == 0) ? 1 : hval;

                do {
                    hval += disp;
                    if (hval >= db->hsize)
                        hval -= db->hsize;
                    dictp = &db->dict[hval];
                } while (dictp->codem1 < max_ent);
            }
            dictp2 = &db->dict[max_ent + 1];
            if (db->dict[dictp2->cptr].codem1 == max_ent)
                db->dict[dictp2->cptr].codem1 = BSD_BADCODEM1;
            dictp2->cptr  = (uint16_t) hval;
            dictp->codem1 = (uint16_t) max_ent;
            dictp->fcode  = fcode;
            db->max_ent   = ++max_ent;
            db->lens[max_ent] = (uint16_t) (db->lens[oldcode] + 1);
            if ((max_ent >= BSD_MAXCODE(n_bits)) && (max_ent < db->maxmaxcode)) {
                db->n_bits = ++n_bits;
                tgtbitno   = 32 - n_bits;
            }
        }
        oldcode = incode;
    }
    /* A packet that ends in CLEAR counts nothing towards the new
       dictionary, as the compressor counted it before clearing. */
    if (!cleared)
        db->in_count += (uint32_t) wpos;
    (void) bsd_check(db);

    /* The protocol is the first byte. */
    if (wpos < 1)
        return COMP_DROP;
    *proto = out[0];
    memmove(out, out + 1, wpos - 1);
    return (int) (wpos - 1);
}

/* ------------------------------------------------------------ Predictor-1 */

#define PRED_HASH(c, x) ((c)->phash = (uint16_t) (((c)->phash << 4) ^ (x)))

static size_t
pred1_squeeze(ppp_comp_t *c, const uint8_t *src, size_t len, uint8_t *dst, size_t dmax)
{
    size_t d = 0;

    while (len > 0) {
        size_t  flagpos = d++;
        uint8_t flags   = 0;

        if (d > dmax)
            return (size_t) -1;
        for (int bit = 1, i = 0; (i < 8) && len; i++, bit <<= 1) {
            if (c->guess[c->phash] == *src)
                flags |= (uint8_t) bit;
            else {
                c->guess[c->phash] = *src;
                if (d >= dmax)
                    return (size_t) -1;
                dst[d++] = *src;
            }
            PRED_HASH(c, *src++);
            len--;
        }
        dst[flagpos] = flags;
    }
    return d;
}

static size_t
pred1_compress(ppp_comp_t *c, uint16_t proto, const uint8_t *data, size_t len, uint8_t *out, size_t outmax)
{
    const size_t orglen = len + 2;
    uint8_t      lenb[2];
    uint16_t     fcs;
    size_t       n;

    if ((orglen > (c->mru + 2)) || (orglen > 0x7fff) || (outmax < (orglen + 4)))
        return 0;
    c->work[0] = (uint8_t) (proto >> 8);
    c->work[1] = (uint8_t) proto;
    memcpy(c->work + 2, data, len);
    lenb[0] = (uint8_t) ((orglen >> 8) & 0x7f);
    lenb[1] = (uint8_t) orglen;
    fcs     = ppp_fcs16(PPP_FCS_INIT, lenb, 2);
    fcs     = (uint16_t) ~ppp_fcs16(fcs, c->work, orglen);

    /* Squeezed whole, so that the table sees every byte either way. */
    n = pred1_squeeze(c, c->work, orglen, c->squeezed, orglen + (orglen / 8) + 2);
    if (n < orglen) {
        out[0] = (uint8_t) (0x80 | lenb[0]);
        out[1] = lenb[1];
        memcpy(out + 2, c->squeezed, n);
    } else {
        /* Sent as it is; the table has seen it all the same. */
        out[0] = lenb[0];
        out[1] = lenb[1];
        memcpy(out + 2, c->work, orglen);
        n = orglen;
    }
    out[2 + n]     = (uint8_t) fcs;
    out[2 + n + 1] = (uint8_t) (fcs >> 8);
    return n + 4;
}

static int
pred1_decompress(ppp_comp_t *c, const uint8_t *in, size_t len, uint16_t *proto, uint8_t *out, size_t outmax)
{
    size_t         want;
    size_t         got = 0;
    const uint8_t *src;
    size_t         slen;
    uint8_t        lenb[2];
    uint16_t       fcs;
    size_t         hdr;

    if (len < 4)
        return COMP_DROP;
    want = ((size_t) (in[0] & 0x7f) << 8) | in[1];
    if (want > outmax)
        return COMP_FAIL;
    src  = in + 2;
    slen = len - 4;
    if (in[0] & 0x80) {
        while ((got < want) && (slen > 0)) {
            const uint8_t flags = *src++;

            slen--;
            for (int i = 0, bit = 1; (i < 8) && (got < want); i++, bit <<= 1) {
                if (flags & bit)
                    out[got] = c->guess[c->phash];
                else {
                    if (slen == 0)
                        return COMP_FAIL;
                    c->guess[c->phash] = *src;
                    out[got]           = *src++;
                    slen--;
                }
                PRED_HASH(c, out[got]);
                got++;
            }
        }
        if ((got != want) || (slen != 0))
            return COMP_FAIL;
    } else {
        if (slen != want)
            return COMP_FAIL;
        for (size_t i = 0; i < want; i++) {
            out[i] = c->guess[c->phash] = src[i];
            PRED_HASH(c, src[i]);
        }
        got = want;
    }
    lenb[0] = in[0] & 0x7f;
    lenb[1] = in[1];
    fcs     = ppp_fcs16(PPP_FCS_INIT, lenb, 2);
    fcs     = (uint16_t) ~ppp_fcs16(fcs, out, got);
    if ((in[len - 2] != (uint8_t) fcs) || (in[len - 1] != (uint8_t) (fcs >> 8)))
        return COMP_FAIL;
    hdr = (size_t) get_proto(out, got, proto);
    if (hdr == 0)
        return COMP_DROP;
    memmove(out, out + hdr, got - hdr);
    return (int) (got - hdr);
}

/* ------------------------------------------------------------------ the API */

ppp_comp_t *
ppp_comp_new(const ppp_comp_params_t *p, int compressor, size_t mru)
{
    ppp_comp_t *c = (ppp_comp_t *) calloc(1, sizeof(ppp_comp_t));

    if (c == NULL)
        return NULL;
    c->type       = p->type;
    c->compressor = compressor;
    c->mru        = mru ? mru : 1500;
    c->work       = (uint8_t *) malloc(c->mru + 64);
    if (c->work == NULL)
        goto fail;

    switch (p->type) {
        case CCP_OPT_MPPX:
            c->mppc      = !!(p->mppx & MPPX_C);
            c->mppe      = !!(p->mppx & MPPX_STRENGTHS);
            c->stateless = !!(p->mppx & MPPX_H);
            c->ccount    = CCOUNT_MASK; /* the first packet is 0 */
            if (!c->mppc && !c->mppe)
                goto fail;
            if (c->mppc) {
                c->hist = (uint8_t *) calloc(1, MPPC_HIST);
                if (c->hist == NULL)
                    goto fail;
                if (compressor && ((c->htab = (uint16_t *) calloc(1u << MPPC_HASH_BITS, sizeof(uint16_t))) == NULL))
                    goto fail;
                if (c->mru > (MPPC_HIST - 2))
                    c->mru = MPPC_HIST - 2;
            }
            if (c->mppe) {
                c->keybits = (p->mppx & MPPX_S) ? 128 : (p->mppx & MPPX_M) ? 56 : 40;
                c->keylen  = (c->keybits == 128) ? 16 : 8;
                memcpy(c->start_key, p->key, 16);
                mppe_initial_key(c);
            }
            snprintf(c->desc, sizeof(c->desc), "%s%s%s%s", c->mppe ? "MPPE " : "",
                     c->mppe ? ((c->keybits == 128) ? "128-bit" : (c->keybits == 56) ? "56-bit" : "40-bit") : "",
                     (c->mppe && c->mppc) ? " + MPPC" : c->mppc ? "MPPC" : "",
                     c->stateless ? ", stateless" : (c->mppe ? ", stateful" : ""));
            break;

        case CCP_OPT_DEFLATE:
        case CCP_OPT_DEFLATE_DRAFT:
            c->window = p->bits ? p->bits : 15;
            if ((c->window < 9) || (c->window > 15))
                goto fail;
            if (compressor) {
                if (deflateInit2(&c->z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -c->window, 8, Z_DEFAULT_STRATEGY) != Z_OK)
                    goto fail;
            } else if (inflateInit2(&c->z, -15) != Z_OK)
                goto fail;
            c->z_ready = 1;
            snprintf(c->desc, sizeof(c->desc), "Deflate, %d-bit window", c->window);
            break;

        case CCP_OPT_BSD:
            if (!bsd_init(&c->bsd, p->bits, !compressor))
                goto fail;
            snprintf(c->desc, sizeof(c->desc), "BSD-Compress, %d bits", p->bits);
            break;

        case CCP_OPT_PRED1:
            c->guess    = (uint8_t *) calloc(1, 65536);
            c->squeezed = (uint8_t *) malloc(c->mru + (c->mru / 8) + 16);
            if ((c->guess == NULL) || (c->squeezed == NULL))
                goto fail;
            snprintf(c->desc, sizeof(c->desc), "Predictor-1");
            break;

        default:
            goto fail;
    }
    return c;

fail:
    ppp_comp_free(c);
    return NULL;
}

void
ppp_comp_free(ppp_comp_t *c)
{
    if (c == NULL)
        return;
    if (c->z_ready) {
        if (c->compressor)
            deflateEnd(&c->z);
        else
            inflateEnd(&c->z);
    }
    free(c->work);
    free(c->hist);
    free(c->htab);
    free(c->bsd.dict);
    free(c->bsd.lens);
    free(c->guess);
    free(c->squeezed);
    crypto_wipe(c, sizeof(*c));
    free(c);
}

const char *
ppp_comp_describe(const ppp_comp_t *c)
{
    return c ? c->desc : "none";
}

int
ppp_comp_encrypts(const ppp_comp_t *c)
{
    return (c != NULL) && c->mppe;
}

int
ppp_comp_applies(const ppp_comp_t *c, uint16_t proto)
{
    if (c == NULL)
        return 0;
    if (c->type == CCP_OPT_MPPX)
        return (proto >= 0x0021) && (proto <= 0x00fa);
    if (c->type == CCP_OPT_BSD)
        return (proto >= 0x21) && (proto <= 0xf9);
    return (proto <= 0x3fff) && (proto != PPP_PROTO_COMP) && (proto != PPP_PROTO_LCOMP);
}

size_t
ppp_comp_compress(ppp_comp_t *c, uint16_t proto, const uint8_t *data, size_t len, uint8_t *out, size_t outmax)
{
    size_t n = 0;

    if (!ppp_comp_applies(c, proto))
        return 0;
    switch (c->type) {
        case CCP_OPT_MPPX:
            n = mppx_compress(c, proto, data, len, out, outmax);
            break;
        case CCP_OPT_DEFLATE:
        case CCP_OPT_DEFLATE_DRAFT:
            n = deflate_compress(c, proto, data, len, out, outmax);
            break;
        case CCP_OPT_BSD:
            n = bsd_compress(c, proto, data, len, out, outmax);
            break;
        case CCP_OPT_PRED1:
            n = pred1_compress(c, proto, data, len, out, outmax);
            break;
        default:
            break;
    }
    c->in_bytes += len + 2;
    c->out_bytes += n ? n : (len + 2);
    return n;
}

int
ppp_comp_decompress(ppp_comp_t *c, const uint8_t *in, size_t len, uint16_t *proto, uint8_t *out, size_t outmax)
{
    int r;

    switch (c->type) {
        case CCP_OPT_MPPX:
            r = mppx_decompress(c, in, len, proto, out, outmax);
            break;
        case CCP_OPT_DEFLATE:
        case CCP_OPT_DEFLATE_DRAFT:
            r = deflate_decompress(c, in, len, proto, out, outmax);
            break;
        case CCP_OPT_BSD:
            r = bsd_decompress(c, in, len, proto, out, outmax);
            break;
        case CCP_OPT_PRED1:
            r = pred1_decompress(c, in, len, proto, out, outmax);
            break;
        default:
            r = COMP_DROP;
            break;
    }
    if (r >= 0) {
        c->in_bytes += len;
        c->out_bytes += (uint64_t) r + 2;
    }
    return r;
}

void
ppp_comp_incomp(ppp_comp_t *c, uint16_t proto, const uint8_t *data, size_t len)
{
    if ((c == NULL) || !ppp_comp_applies(c, proto))
        return;
    if ((c->type == CCP_OPT_DEFLATE) || (c->type == CCP_OPT_DEFLATE_DRAFT))
        deflate_incomp(c, proto, data, len);
    else if (c->type == CCP_OPT_BSD)
        bsd_incomp(c, proto, data, len);
}

void
ppp_comp_reset(ppp_comp_t *c)
{
    if (c == NULL)
        return;
    switch (c->type) {
        case CCP_OPT_MPPX:
            /* Compressor: flush on the next packet.  Decompressor: the FLUSHED
               bit does it, nothing here. */
            if (c->compressor)
                c->reset_req = 1;
            break;
        case CCP_OPT_DEFLATE:
        case CCP_OPT_DEFLATE_DRAFT:
            if (c->compressor)
                deflateReset(&c->z);
            else
                inflateReset(&c->z);
            c->seq        = 0;
            c->need_reset = 0;
            break;
        case CCP_OPT_BSD:
            bsd_reset(&c->bsd);
            c->need_reset = 0;
            break;
        case CCP_OPT_PRED1:
            memset(c->guess, 0, 65536);
            c->phash = 0;
            break;
        default:
            break;
    }
}

void
ppp_comp_stats(const ppp_comp_t *c, uint64_t *in_bytes, uint64_t *out_bytes)
{
    *in_bytes  = c ? c->in_bytes : 0;
    *out_bytes = c ? c->out_bytes : 0;
}
