/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Multilink PPP's fragments.  See ppp_mp.h.
 *
 *             Reassembly keeps the fragments in sequence order from the one
 *             expected next.  A packet is delivered once a fragment with B
 *             at `expected` is followed, without a gap, by one with E.  The
 *             fragment at `expected` is given up on when every link of the
 *             bundle has sent something later (RFC 1990 4.1: M has passed
 *             it), and then everything up to the next B goes with it.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ppp_mp.h"

#define SMALL_PACKET 256 /* whole on one link below this */

/* How far `a` is after `b`, in sequence space; "before" is the top half. */
static uint32_t
seq_diff(const mp_rx_t *rx, uint32_t a, uint32_t b)
{
    return (a - b) & rx->mask;
}

static int
seq_before(const mp_rx_t *rx, uint32_t a, uint32_t b)
{
    return seq_diff(rx, b, a) != 0 && seq_diff(rx, b, a) < ((rx->mask + 1) / 2);
}

void
mp_rx_init(mp_rx_t *rx, int short_seq, size_t mrru)
{
    memset(rx, 0, sizeof(*rx));
    rx->short_seq = short_seq;
    rx->mask      = short_seq ? 0x0fff : 0x00ffffff;
    rx->mrru      = mrru ? mrru : 1500;
}

static void
drop_front(mp_rx_t *rx, int k)
{
    for (int i = 0; i < k; i++)
        free(rx->f[i].data);
    memmove(&rx->f[0], &rx->f[k], (size_t) (rx->n - k) * sizeof(mp_frag_t));
    rx->n -= k;
}

void
mp_rx_free(mp_rx_t *rx)
{
    drop_front(rx, rx->n);
}

void
mp_rx_link_gone(mp_rx_t *rx, int link)
{
    if ((link >= 0) && (link < MP_MAX_LINKS))
        rx->seen[link] = 0;
}

/* Has every link sent something after `seq`? */
static int
m_passed(const mp_rx_t *rx, uint32_t seq)
{
    int any = 0;

    for (int i = 0; i < MP_MAX_LINKS; i++) {
        if (!rx->seen[i])
            continue;
        any = 1;
        if (!seq_before(rx, seq, rx->last[i]))
            return 0;
    }
    return any;
}

/* Gives up on the packet at the front: up to the next fragment with B. */
static void
skip_to_next_b(mp_rx_t *rx)
{
    int k = 1;

    while ((k < rx->n) && !(rx->f[k].flags & MP_B))
        k++;
    rx->lost += (uint32_t) k;
    if (k < rx->n) {
        rx->expected = rx->f[k].seq;
        drop_front(rx, k);
    } else {
        rx->expected = (rx->f[rx->n - 1].seq + 1) & rx->mask;
        drop_front(rx, rx->n);
    }
}

static void
process(mp_rx_t *rx, mp_deliver_cb deliver, void *opaque)
{
    while (rx->n > 0) {
        const mp_frag_t *head = &rx->f[0];
        int              k;
        int              end = -1;

        if (head->seq != rx->expected) {
            /* A gap: lost once every link has gone past it. */
            if (!m_passed(rx, rx->expected))
                return;
            rx->lost++;
            if (head->flags & MP_B)
                rx->expected = head->seq;
            else
                skip_to_next_b(rx);
            continue;
        }
        if (!(head->flags & MP_B)) {
            /* The middle of a packet whose beginning is gone. */
            skip_to_next_b(rx);
            continue;
        }
        for (k = 0; k < rx->n; k++) {
            if (rx->f[k].seq != ((rx->expected + (uint32_t) k) & rx->mask))
                break;
            if ((k > 0) && (rx->f[k].flags & MP_B))
                break; /* a new packet before this one ended */
            if (rx->f[k].flags & MP_E) {
                end = k;
                break;
            }
        }
        if (end < 0) {
            if ((k < rx->n) && (rx->f[k].seq == ((rx->expected + (uint32_t) k) & rx->mask))) {
                /* B again before E: the end was lost. */
                rx->lost += (uint32_t) k;
                rx->expected = rx->f[k].seq;
                drop_front(rx, k);
                continue;
            }
            /* Waiting for the rest, unless every link has gone past it. */
            if ((k < rx->n) && m_passed(rx, (rx->expected + (uint32_t) k) & rx->mask)) {
                rx->lost += (uint32_t) k;
                rx->expected = (rx->expected + (uint32_t) k) & rx->mask;
                drop_front(rx, k);
                continue;
            }
            return;
        }
        {
            size_t total = 0;

            for (int i = 0; i <= end; i++)
                total += rx->f[i].len;
            if ((total >= 1) && (total <= (rx->mrru + 2))) {
                uint8_t *pkt = (uint8_t *) malloc(total);

                if (pkt != NULL) {
                    size_t off = 0;

                    for (int i = 0; i <= end; i++) {
                        memcpy(pkt + off, rx->f[i].data, rx->f[i].len);
                        off += rx->f[i].len;
                    }
                    rx->packets++;
                    deliver(opaque, pkt, total);
                    free(pkt);
                }
            } else
                rx->lost += (uint32_t) end + 1;
            rx->expected = (rx->f[end].seq + 1) & rx->mask;
            drop_front(rx, end + 1);
        }
    }
}

void
mp_rx_input(mp_rx_t *rx, int link, const uint8_t *p, size_t len, mp_deliver_cb deliver, void *opaque)
{
    uint8_t  flags;
    uint32_t seq;
    size_t   hl = rx->short_seq ? 2 : 4;
    int      at;

    if (len < hl)
        return;
    flags = p[0] & (MP_B | MP_E);
    if (rx->short_seq)
        seq = ((uint32_t) (p[0] & 0x0f) << 8) | p[1];
    else
        seq = ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
    p += hl;
    len -= hl;
    if (len > 0xffff)
        return;

    if (!rx->started) {
        rx->started  = 1;
        rx->expected = seq;
    }
    if ((link >= 0) && (link < MP_MAX_LINKS)) {
        if (!rx->seen[link] || seq_before(rx, rx->last[link], seq))
            rx->last[link] = seq;
        rx->seen[link] = 1;
    }
    if (seq_before(rx, seq, rx->expected)) {
        rx->lost++; /* too late */
        process(rx, deliver, opaque);
        return;
    }

    /* In order of distance from `expected`; a repeat is dropped. */
    for (at = 0; at < rx->n; at++) {
        if (rx->f[at].seq == seq)
            return;
        if (seq_diff(rx, rx->f[at].seq, rx->expected) > seq_diff(rx, seq, rx->expected))
            break;
    }
    if (rx->n == MP_RX_MAX) {
        /* Full: the oldest packet goes. */
        skip_to_next_b(rx);
        mp_rx_input(rx, -1, p - hl, len + hl, deliver, opaque);
        return;
    }
    memmove(&rx->f[at + 1], &rx->f[at], (size_t) (rx->n - at) * sizeof(mp_frag_t));
    rx->f[at].seq   = seq;
    rx->f[at].flags = flags;
    rx->f[at].len   = (uint16_t) len;
    rx->f[at].data  = (uint8_t *) malloc(len ? len : 1);
    if (rx->f[at].data == NULL) {
        memmove(&rx->f[at], &rx->f[at + 1], (size_t) (rx->n - at) * sizeof(mp_frag_t));
        return;
    }
    if (len > 0)
        memcpy(rx->f[at].data, p, len);
    rx->n++;
    process(rx, deliver, opaque);
}

void
mp_tx_init(mp_tx_t *tx, int short_seq)
{
    memset(tx, 0, sizeof(*tx));
    tx->short_seq = short_seq;
}

static void
emit_frag(mp_tx_t *tx, int link, uint8_t flags, const uint8_t *data, size_t len, mp_emit_cb emit, void *opaque)
{
    uint8_t frag[4 + 2048];
    size_t  hl;

    if (len > 2048)
        return;
    if (tx->short_seq) {
        frag[0] = (uint8_t) (flags | ((tx->seq >> 8) & 0x0f));
        frag[1] = (uint8_t) tx->seq;
        hl      = 2;
        tx->seq = (tx->seq + 1) & 0x0fff;
    } else {
        frag[0] = flags;
        frag[1] = (uint8_t) (tx->seq >> 16);
        frag[2] = (uint8_t) (tx->seq >> 8);
        frag[3] = (uint8_t) tx->seq;
        hl      = 4;
        tx->seq = (tx->seq + 1) & 0x00ffffff;
    }
    memcpy(frag + hl, data, len);
    emit(opaque, link, frag, hl + len);
}

void
mp_tx_send(mp_tx_t *tx, const uint8_t *pkt, size_t len, int nlinks, mp_emit_cb emit, void *opaque)
{
    if (nlinks < 1)
        nlinks = 1;
    if ((len < SMALL_PACKET) || (nlinks == 1)) {
        emit_frag(tx, tx->rr % nlinks, MP_B | MP_E, pkt, len, emit, opaque);
        tx->rr = (tx->rr + 1) % nlinks;
        return;
    }
    {
        const size_t per = (len + (size_t) nlinks - 1) / (size_t) nlinks;
        size_t       off = 0;

        for (int i = 0; (i < nlinks) && (off < len); i++) {
            const size_t n     = ((len - off) < per) ? (len - off) : per;
            uint8_t      flags = 0;

            if (off == 0)
                flags |= MP_B;
            if ((off + n) == len)
                flags |= MP_E;
            emit_frag(tx, (tx->rr + i) % nlinks, flags, pkt + off, n, emit, opaque);
            off += n;
        }
        tx->rr = (tx->rr + 1) % nlinks;
    }
}
