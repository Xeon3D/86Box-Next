/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             What CCP negotiates (RFC 1962): the compressors and
 *             decompressors, one per direction -- Microsoft's MPPC
 *             (RFC 2118) and MPPE encryption (RFC 3078), which share one
 *             option and one header, Deflate (RFC 1979), BSD-Compress
 *             (RFC 1977) and Predictor-1 (RFC 1978).  No CCP here: the
 *             option negotiation is ppp_session.c's.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef PPP_COMP_H
#define PPP_COMP_H

#include <stddef.h>
#include <stdint.h>

#define PPP_PROTO_COMP   0x00fd /* a compressed (or encrypted) datagram */
#define PPP_PROTO_CCP    0x80fd
#define PPP_PROTO_LCOMP  0x00fb /* the same, per member link of a bundle */
#define PPP_PROTO_LCCP   0x80fb

/* CCP option types. */
#define CCP_OPT_PRED1         1
#define CCP_OPT_MPPX          18 /* MPPC and MPPE */
#define CCP_OPT_BSD           21
#define CCP_OPT_DEFLATE_DRAFT 24
#define CCP_OPT_DEFLATE       26

/* Option 18's bits. */
#define MPPX_C 0x00000001u /* MPPC                 */
#define MPPX_D 0x00000010u /* obsolete             */
#define MPPX_L 0x00000020u /* 40-bit MPPE          */
#define MPPX_S 0x00000040u /* 128-bit MPPE         */
#define MPPX_M 0x00000080u /* 56-bit MPPE          */
#define MPPX_H 0x01000000u /* stateless            */
#define MPPX_STRENGTHS (MPPX_L | MPPX_S | MPPX_M)

/* The compressions, as a set (isp_settings_t.compression). */
enum {
    PPP_COMP_MPPC = 0,
    PPP_COMP_DEFLATE,
    PPP_COMP_BSD,
    PPP_COMP_PRED1,
    PPP_COMP_COUNT
};
#define PPP_COMP_ALL ((1u << PPP_COMP_COUNT) - 1)

extern const char *ppp_comp_key(int c);  /* "deflate" */
extern const char *ppp_comp_name(int c); /* "Deflate" */

typedef struct ppp_comp_params {
    int      type;      /* CCP_OPT_* */
    uint32_t mppx;      /* option 18: MPPX_C, one strength, MPPX_H */
    int      bits;      /* BSD-Compress code size, Deflate window (9..15) */
    uint8_t  key[16];   /* MPPE: this direction's start key */
} ppp_comp_params_t;

typedef struct ppp_comp ppp_comp_t;

/* NULL if the method is unknown or out of memory.  `mru`: the largest
   packet this direction carries. */
extern ppp_comp_t *ppp_comp_new(const ppp_comp_params_t *p, int compressor, size_t mru);
extern void        ppp_comp_free(ppp_comp_t *c);
extern const char *ppp_comp_describe(const ppp_comp_t *c); /* "MPPE 128-bit stateless, MPPC" */
extern int         ppp_comp_encrypts(const ppp_comp_t *c);

/* Does this method compress `proto`?  (The rest go as they are.) */
extern int ppp_comp_applies(const ppp_comp_t *c, uint16_t proto);

/* Compresses one packet.  Returns the length of the 0x00FD datagram written
   to `out`, or 0: send the packet as it is (it would grow; the history has
   been told). */
extern size_t ppp_comp_compress(ppp_comp_t *c, uint16_t proto, const uint8_t *data, size_t len, uint8_t *out,
                                size_t outmax);

enum {
    COMP_DROP  = -1, /* discard it, quietly (waiting for a resynchronization)  */
    COMP_RESET = -2, /* discard it and send a CCP Reset-Request                  */
    COMP_FAIL  = -3  /* discard it; CCP has to be negotiated again (Predictor) */
};

/* Decompresses a 0x00FD datagram: the original protocol and its data.
   Returns the data's length, or COMP_*. */
extern int ppp_comp_decompress(ppp_comp_t *c, const uint8_t *in, size_t len, uint16_t *proto, uint8_t *out,
                               size_t outmax);

/* A packet the peer sent uncompressed: into the decompressor's history
   (Deflate and BSD-Compress keep it in step that way). */
extern void ppp_comp_incomp(ppp_comp_t *c, uint16_t proto, const uint8_t *data, size_t len);

/* CCP Reset-Request received (compressor) or Reset-Ack received
   (decompressor). */
extern void ppp_comp_reset(ppp_comp_t *c);

/* Packets each way through it, for the log. */
extern void ppp_comp_stats(const ppp_comp_t *c, uint64_t *in_bytes, uint64_t *out_bytes);

#endif /* PPP_COMP_H */
