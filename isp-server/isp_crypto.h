/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The hashes and ciphers the ISP's authentication, encryption
 *             and logins need: MD4 and DES for MS-CHAP, MD5 and SHA-1/2/3
 *             for CHAP, RC4 for MPPE, HMAC-SHA-256 and PBKDF2 for the
 *             status page's passwords, and the system's random numbers.
 *             Small portable implementations, no library: isp-server
 *             links statically and has nothing else that needs them.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef ISP_CRYPTO_H
#define ISP_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define MD4_LEN    16
#define MD5_LEN    16
#define SHA1_LEN   20
#define SHA256_LEN 32
#define SHA384_LEN 48
#define SHA512_LEN 64
#define HASH_MAX   64

/* The hash functions CHAP can be negotiated with, and the rest. */
enum {
    HASH_MD4 = 0,
    HASH_MD5,
    HASH_SHA1,
    HASH_SHA256,
    HASH_SHA384,
    HASH_SHA512,
    HASH_SHA3_256,
    HASH_SHA3_384,
    HASH_SHA3_512,
    HASH_COUNT
};

typedef struct hash_ctx {
    int      alg;
    uint64_t len;     /* bytes so far */
    size_t   fill;    /* bytes in buf */
    uint8_t  buf[200];
    union {
        uint32_t s32[8];
        uint64_t s64[25];
    } st;
} hash_ctx_t;

extern size_t hash_len(int alg);
extern const char *hash_name(int alg);
extern void   hash_init(hash_ctx_t *c, int alg);
extern void   hash_update(hash_ctx_t *c, const void *data, size_t len);
extern void   hash_final(hash_ctx_t *c, uint8_t *out); /* hash_len(alg) bytes */
extern void   hash(int alg, const void *data, size_t len, uint8_t *out);

/* DES, one block, ECB: `key7` is 56 bits without parity, as MS-CHAP keeps
   them. */
extern void des_encrypt7(const uint8_t key7[7], const uint8_t in[8], uint8_t out[8]);

typedef struct rc4 {
    uint8_t s[256];
    uint8_t i, j;
} rc4_t;

extern void rc4_init(rc4_t *r, const uint8_t *key, size_t len);
extern void rc4_crypt(rc4_t *r, const uint8_t *in, uint8_t *out, size_t len); /* in == out is fine */

extern void hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[SHA256_LEN]);
extern void pbkdf2_sha256(const char *password, const uint8_t *salt, size_t slen, uint32_t rounds, uint8_t *out,
                          size_t olen);

/* From the system's generator; 0 if it would not give any. */
extern int crypto_random(uint8_t *out, size_t len);

/* Compares in time that does not depend on where they differ. */
extern int crypto_equal(const void *a, const void *b, size_t len);
extern void crypto_wipe(void *p, size_t len);

/* Lowercase hex, NUL-terminated (2 * len + 1 bytes); and back, 0 on a bad
   digit or length. */
extern void hex_encode(const uint8_t *in, size_t len, char *out);
extern int  hex_decode(const char *in, uint8_t *out, size_t len);

#endif /* ISP_CRYPTO_H */
