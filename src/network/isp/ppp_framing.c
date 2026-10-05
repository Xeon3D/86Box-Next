/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             PPP in HDLC-like framing, RFC 1662.  See ppp_framing.h.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "ppp_framing.h"

/* RFC 1662 C.2: x**0 + x**5 + x**12 + x**16, bit-reversed, one byte at a time. */
static uint16_t fcs_table[256];
static int      fcs_ready;

static void
fcs_init(void)
{
    for (unsigned b = 0; b < 256; b++) {
        unsigned v = b;

        for (int i = 0; i < 8; i++)
            v = (v & 1) ? ((v >> 1) ^ 0x8408) : (v >> 1);
        fcs_table[b] = (uint16_t) v;
    }
    fcs_ready = 1;
}

uint16_t
ppp_fcs16(uint16_t fcs, const uint8_t *data, size_t len)
{
    if (!fcs_ready)
        fcs_init();
    while (len--)
        fcs = (uint16_t) ((fcs >> 8) ^ fcs_table[(fcs ^ *data++) & 0xff]);
    return fcs;
}

void
ppp_rx_init(ppp_rx_t *rx)
{
    memset(rx, 0, sizeof(ppp_rx_t));
    rx->hunting = 1; /* whatever precedes the first flag is not a frame */
    rx->accm    = PPP_ACCM_ALL;
    if (!fcs_ready)
        fcs_init();
}

static void
ppp_rx_end(ppp_rx_t *rx, ppp_frame_cb cb, void *opaque)
{
    if (rx->hunting) {
        /* The flag ends the hunt; what came before it is gone. */
        rx->hunting = 0;
    } else if (rx->escaped) {
        rx->aborts++; /* 7D 7E: the sender aborted the frame */
    } else if (rx->len > 0) {
        /* At the least a one-byte protocol and the FCS. */
        if (rx->len < 3)
            rx->runts++;
        else if (ppp_fcs16(PPP_FCS_INIT, rx->buf, rx->len) != PPP_FCS_GOOD)
            rx->bad_fcs++;
        else {
            rx->frames++;
            cb(opaque, rx->buf, rx->len - 2);
        }
    }
    /* Back-to-back flags (an empty frame) are only fill. */
    rx->len     = 0;
    rx->escaped = 0;
}

void
ppp_rx_feed(ppp_rx_t *rx, const uint8_t *data, size_t len, ppp_frame_cb cb, void *opaque)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];

        if (c == PPP_FLAG) {
            ppp_rx_end(rx, cb, opaque);
            continue;
        }
        if (rx->hunting)
            continue;
        /* A control character in the receive ACCM was put there by something
           between the peers (RFC 1662 7.1), never sent as data. */
        if ((c < 0x20) && (rx->accm & (1u << c)))
            continue;
        if (c == PPP_ESCAPE) {
            rx->escaped = 1;
            continue;
        }
        if (rx->escaped) {
            c ^= PPP_TRANS;
            rx->escaped = 0;
        }
        if (rx->len >= sizeof(rx->buf)) {
            /* Over the limit: drop it all and wait for the next flag. */
            rx->too_long++;
            rx->len     = 0;
            rx->hunting = 1;
            continue;
        }
        rx->buf[rx->len++] = c;
    }
}

static int
needs_escape(uint32_t accm, uint8_t c)
{
    return (c == PPP_FLAG) || (c == PPP_ESCAPE) || ((c < 0x20) && (accm & (1u << c)));
}

typedef struct {
    uint8_t *out;
    size_t   size;
    size_t   pos;
    int      overflow;
} ppp_out_t;

static void
put_raw(ppp_out_t *o, uint8_t c)
{
    if (o->pos >= o->size)
        o->overflow = 1;
    else
        o->out[o->pos++] = c;
}

static void
put(ppp_out_t *o, uint32_t accm, uint8_t c)
{
    if (needs_escape(accm, c)) {
        put_raw(o, PPP_ESCAPE);
        put_raw(o, c ^ PPP_TRANS);
    } else
        put_raw(o, c);
}

size_t
ppp_encode(uint32_t accm, uint16_t protocol, const uint8_t *info, size_t len,
           uint8_t *out, size_t out_size)
{
    ppp_out_t     o      = { out, out_size, 0, 0 };
    const uint8_t head[4] = { 0xff, 0x03, (uint8_t) (protocol >> 8), (uint8_t) protocol };
    uint16_t      fcs;

    /* The FCS covers the unescaped bytes. */
    fcs = ppp_fcs16(PPP_FCS_INIT, head, sizeof(head));
    fcs = ppp_fcs16(fcs, info, len);
    fcs ^= 0xffff;

    put_raw(&o, PPP_FLAG);
    for (size_t i = 0; i < sizeof(head); i++)
        put(&o, accm, head[i]);
    for (size_t i = 0; i < len; i++)
        put(&o, accm, info[i]);
    put(&o, accm, (uint8_t) (fcs & 0xff));
    put(&o, accm, (uint8_t) (fcs >> 8));
    put_raw(&o, PPP_FLAG);

    return o.overflow ? 0 : o.pos;
}
