/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             PPP in HDLC-like framing (RFC 1662): flag delimited, 7Dh
 *             escaped, FCS-16 checked frames over an asynchronous byte
 *             stream.  No sockets, no clock.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef PPP_FRAMING_H
#define PPP_FRAMING_H

#include <stddef.h>
#include <stdint.h>

#define PPP_FLAG        0x7e
#define PPP_ESCAPE      0x7d
#define PPP_TRANS       0x20
#define PPP_FCS_INIT    0xffff
#define PPP_FCS_GOOD    0xf0b8
#define PPP_ACCM_ALL    0xffffffffu /* the default ACCM: every control character escaped */

/* The largest frame, address to FCS, the decoder accepts: a 1500-byte MRU's
   worth of information and its headers, with room to spare for a peer that
   pads. */
#define PPP_MAX_FRAME   2048
/* An encoded frame: everything escaped, both flags. */
#define PPP_ENCODED_MAX(info_len) ((((info_len) + 6) * 2) + 2)

typedef void (*ppp_frame_cb)(void *opaque, const uint8_t *frame, size_t len);

typedef struct ppp_rx {
    uint8_t  buf[PPP_MAX_FRAME];
    size_t   len;
    int      escaped;
    int      hunting;   /* discarding until the next flag              */
    uint32_t accm;      /* control characters a sender's DCE may insert */

    /* What was thrown away, for the log. */
    uint32_t frames;
    uint32_t bad_fcs;
    uint32_t too_long;
    uint32_t runts;
    uint32_t aborts;
} ppp_rx_t;

extern uint16_t ppp_fcs16(uint16_t fcs, const uint8_t *data, size_t len);

extern void ppp_rx_init(ppp_rx_t *rx);
/* Feed bytes split anywhere; each good frame (address field to the end of the
   information, FCS removed) is handed to `cb` as soon as its closing flag
   arrives. */
extern void ppp_rx_feed(ppp_rx_t *rx, const uint8_t *data, size_t len, ppp_frame_cb cb, void *opaque);

/* Encode one frame with the uncompressed address, control and protocol fields:
   flag, FF 03, protocol, info, FCS, flag, with every byte the ACCM names
   escaped.  Returns the length written, or 0 if `out` is too small. */
extern size_t ppp_encode(uint32_t accm, uint16_t protocol, const uint8_t *info, size_t len,
                         uint8_t *out, size_t out_size);

#endif /* PPP_FRAMING_H */
