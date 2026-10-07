/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Multilink PPP's fragments (RFC 1990): reassembly of what the
 *             guest sends over all the links of a bundle, and the ISP's
 *             packets cut up over them.  Sequence numbers, B and E bits,
 *             loss detection by the per-link minimum (M); no sockets, no
 *             threads.  ppp_session.c owns one of each per bundle.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef PPP_MP_H
#define PPP_MP_H

#include <stddef.h>
#include <stdint.h>

#define PPP_PROTO_MP  0x003d
#define MP_MAX_LINKS  8   /* links in one bundle      */
#define MP_RX_MAX     96  /* fragments held at most   */

#define MP_B 0x80 /* the first fragment of a packet */
#define MP_E 0x40 /* the last                        */

typedef struct mp_frag {
    uint32_t seq;
    uint8_t  flags;
    uint16_t len;
    uint8_t *data;
} mp_frag_t;

typedef struct mp_rx {
    int       short_seq; /* 12-bit sequence numbers (we asked for them) */
    uint32_t  mask;
    size_t    mrru;
    int       started;
    uint32_t  expected;
    uint32_t  last[MP_MAX_LINKS];
    uint8_t   seen[MP_MAX_LINKS];
    int       n;
    mp_frag_t f[MP_RX_MAX];
    uint32_t  packets;
    uint32_t  lost;      /* fragments given up on */
} mp_rx_t;

typedef void (*mp_deliver_cb)(void *opaque, const uint8_t *pkt, size_t len);

extern void mp_rx_init(mp_rx_t *rx, int short_seq, size_t mrru);
extern void mp_rx_free(mp_rx_t *rx);
/* A link left the bundle: it no longer holds M back. */
extern void mp_rx_link_gone(mp_rx_t *rx, int link);
/* One MP frame's information field (the MP header onward), from `link`;
   `deliver` gets every packet it completes: protocol field and data. */
extern void mp_rx_input(mp_rx_t *rx, int link, const uint8_t *p, size_t len, mp_deliver_cb deliver, void *opaque);

typedef struct mp_tx {
    int      short_seq; /* the peer asked for 12-bit numbers */
    uint32_t seq;
    int      rr;
} mp_tx_t;

typedef void (*mp_emit_cb)(void *opaque, int link, const uint8_t *frag, size_t len);

extern void mp_tx_init(mp_tx_t *tx, int short_seq);
/* A packet (protocol field and data) over `nlinks` links: whole on the next
   link in turn if it is small, else a fragment on each.  `emit` gets the MP
   frame's information field. */
extern void mp_tx_send(mp_tx_t *tx, const uint8_t *pkt, size_t len, int nlinks, mp_emit_cb emit, void *opaque);

#endif /* PPP_MP_H */
