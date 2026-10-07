/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Hashes, DES, RC4, HMAC, PBKDF2 and random numbers.  See
 *             isp_crypto.h.  Written from the published specifications:
 *             RFC 1320 (MD4), RFC 1321 (MD5), FIPS 180-4 (SHA-1, SHA-2),
 *             FIPS 202 (SHA-3), FIPS 46-3 (DES), RFC 2104 (HMAC) and
 *             RFC 8018 (PBKDF2); RC4 as everyone has it.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#    include <bcrypt.h>
#else
#    include <errno.h>
#    include <fcntl.h>
#    include <unistd.h>
#    if defined(__linux__)
#        include <sys/random.h>
#    endif
#endif
#include "isp_crypto.h"

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROL64(x, n) (((x) << (n)) | ((x) >> (64 - (n))))
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static uint32_t
le32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint32_t
be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static uint64_t
be64(const uint8_t *p)
{
    return ((uint64_t) be32(p) << 32) | be32(p + 4);
}

static uint64_t
le64(const uint8_t *p)
{
    return (uint64_t) le32(p) | ((uint64_t) le32(p + 4) << 32);
}

/* ------------------------------------------------------------------ MD4 */

static void
md4_block(uint32_t *h, const uint8_t *p)
{
    static const int r2[16] = { 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15 };
    static const int r3[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
    static const int s1[4]  = { 3, 7, 11, 19 };
    static const int s2[4]  = { 3, 5, 9, 13 };
    static const int s3[4]  = { 3, 9, 11, 15 };
    uint32_t         x[16];
    uint32_t         a = h[0], b = h[1], c = h[2], d = h[3], t;

    for (int i = 0; i < 16; i++)
        x[i] = le32(p + 4 * i);
    for (int i = 0; i < 16; i++) {
        t = a + ((b & c) | (~b & d)) + x[i];
        t = ROL32(t, s1[i & 3]);
        a = d; d = c; c = b; b = t;
    }
    for (int i = 0; i < 16; i++) {
        t = a + ((b & c) | (b & d) | (c & d)) + x[r2[i]] + 0x5a827999u;
        t = ROL32(t, s2[i & 3]);
        a = d; d = c; c = b; b = t;
    }
    for (int i = 0; i < 16; i++) {
        t = a + (b ^ c ^ d) + x[r3[i]] + 0x6ed9eba1u;
        t = ROL32(t, s3[i & 3]);
        a = d; d = c; c = b; b = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

/* ------------------------------------------------------------------ MD5 */

static void
md5_block(uint32_t *h, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };
    static const int s[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
    uint32_t         x[16];
    uint32_t         a = h[0], b = h[1], c = h[2], d = h[3];

    for (int i = 0; i < 16; i++)
        x[i] = le32(p + 4 * i);
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int      g;

        switch (i >> 4) {
            case 0:
                f = (b & c) | (~b & d);
                g = i;
                break;
            case 1:
                f = (b & d) | (c & ~d);
                g = (5 * i + 1) & 15;
                break;
            case 2:
                f = b ^ c ^ d;
                g = (3 * i + 5) & 15;
                break;
            default:
                f = c ^ (b | ~d);
                g = (7 * i) & 15;
                break;
        }
        f = a + f + k[i] + x[g];
        a = d;
        d = c;
        c = b;
        b = b + ROL32(f, s[((i >> 4) << 2) | (i & 3)]);
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

/* ---------------------------------------------------------------- SHA-1 */

static void
sha1_block(uint32_t *h, const uint8_t *p)
{
    uint32_t w[80];
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

    for (int i = 0; i < 16; i++)
        w[i] = be32(p + 4 * i);
    for (int i = 16; i < 80; i++)
        w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k, t;

        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        t = ROL32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL32(b, 30);
        b = a;
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

/* -------------------------------------------------------------- SHA-256 */

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void
sha256_block(uint32_t *h, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];

    for (int i = 0; i < 16; i++)
        w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        const uint32_t s1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = hh + s1 + ch + sha256_k[i] + w[i];
        const uint32_t s0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + mj;

        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* -------------------------------------------------------------- SHA-512 */

static const uint64_t sha512_k[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull
};

static void
sha512_block(uint64_t *h, const uint8_t *p)
{
    uint64_t w[80];
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];

    for (int i = 0; i < 16; i++)
        w[i] = be64(p + 8 * i);
    for (int i = 16; i < 80; i++) {
        const uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 80; i++) {
        const uint64_t s1 = ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41);
        const uint64_t ch = (e & f) ^ (~e & g);
        const uint64_t t1 = hh + s1 + ch + sha512_k[i] + w[i];
        const uint64_t s0 = ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39);
        const uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
        const uint64_t t2 = s0 + mj;

        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* ---------------------------------------------------------------- SHA-3 */

static void
keccakf(uint64_t *st)
{
    static const uint64_t rc[24] = {
        0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808aull, 0x8000000080008000ull,
        0x000000000000808bull, 0x0000000080000001ull, 0x8000000080008081ull, 0x8000000000008009ull,
        0x000000000000008aull, 0x0000000000000088ull, 0x0000000080008009ull, 0x000000008000000aull,
        0x000000008000808bull, 0x800000000000008bull, 0x8000000000008089ull, 0x8000000000008003ull,
        0x8000000000008002ull, 0x8000000000000080ull, 0x000000000000800aull, 0x800000008000000aull,
        0x8000000080008081ull, 0x8000000000008080ull, 0x0000000080000001ull, 0x8000000080008008ull
    };
    static const int rotc[24] = { 1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44 };
    static const int piln[24] = { 10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1 };
    uint64_t         bc[5];
    uint64_t         t;

    for (int r = 0; r < 24; r++) {
        for (int i = 0; i < 5; i++)
            bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
        for (int i = 0; i < 5; i++) {
            t = bc[(i + 4) % 5] ^ ROL64(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5)
                st[j + i] ^= t;
        }
        t = st[1];
        for (int i = 0; i < 24; i++) {
            const int j = piln[i];

            bc[0] = st[j];
            st[j] = ROL64(t, rotc[i]);
            t     = bc[0];
        }
        for (int j = 0; j < 25; j += 5) {
            for (int i = 0; i < 5; i++)
                bc[i] = st[j + i];
            for (int i = 0; i < 5; i++)
                st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
        }
        st[0] ^= rc[r];
    }
}

/* ------------------------------------------------------------ the hashes */

static size_t
block_size(int alg)
{
    switch (alg) {
        case HASH_SHA384:
        case HASH_SHA512:
            return 128;
        case HASH_SHA3_256:
            return 136;
        case HASH_SHA3_384:
            return 104;
        case HASH_SHA3_512:
            return 72;
        default:
            return 64;
    }
}

static int
is_sha3(int alg)
{
    return (alg == HASH_SHA3_256) || (alg == HASH_SHA3_384) || (alg == HASH_SHA3_512);
}

size_t
hash_len(int alg)
{
    switch (alg) {
        case HASH_MD4:
        case HASH_MD5:
            return 16;
        case HASH_SHA1:
            return 20;
        case HASH_SHA256:
        case HASH_SHA3_256:
            return 32;
        case HASH_SHA384:
        case HASH_SHA3_384:
            return 48;
        case HASH_SHA512:
        case HASH_SHA3_512:
            return 64;
        default:
            return 0;
    }
}

const char *
hash_name(int alg)
{
    static const char *const names[HASH_COUNT] = { "MD4", "MD5", "SHA-1", "SHA-256", "SHA-384",
                                                   "SHA-512", "SHA3-256", "SHA3-384", "SHA3-512" };

    return ((alg >= 0) && (alg < HASH_COUNT)) ? names[alg] : "?";
}

void
hash_init(hash_ctx_t *c, int alg)
{
    static const uint32_t md_iv[5]     = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    static const uint32_t sha256_iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    static const uint64_t sha512_iv[8] = { 0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull,
                                           0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
                                           0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull };
    static const uint64_t sha384_iv[8] = { 0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull,
                                           0x152fecd8f70e5939ull, 0x67332667ffc00b31ull, 0x8eb44a8768581511ull,
                                           0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull };

    memset(c, 0, sizeof(*c));
    c->alg = alg;
    switch (alg) {
        case HASH_MD4:
        case HASH_MD5:
        case HASH_SHA1:
            memcpy(c->st.s32, md_iv, sizeof(md_iv));
            break;
        case HASH_SHA256:
            memcpy(c->st.s32, sha256_iv, sizeof(sha256_iv));
            break;
        case HASH_SHA384:
            memcpy(c->st.s64, sha384_iv, sizeof(sha384_iv));
            break;
        case HASH_SHA512:
            memcpy(c->st.s64, sha512_iv, sizeof(sha512_iv));
            break;
        default:
            break; /* SHA-3 starts from zeroes */
    }
}

static void
compress_block(hash_ctx_t *c, const uint8_t *p)
{
    switch (c->alg) {
        case HASH_MD4:
            md4_block(c->st.s32, p);
            break;
        case HASH_MD5:
            md5_block(c->st.s32, p);
            break;
        case HASH_SHA1:
            sha1_block(c->st.s32, p);
            break;
        case HASH_SHA256:
            sha256_block(c->st.s32, p);
            break;
        case HASH_SHA384:
        case HASH_SHA512:
            sha512_block(c->st.s64, p);
            break;
        default: {
            const size_t rate = block_size(c->alg);

            for (size_t i = 0; i < rate / 8; i++)
                c->st.s64[i] ^= le64(p + 8 * i);
            keccakf(c->st.s64);
            break;
        }
    }
}

void
hash_update(hash_ctx_t *c, const void *data, size_t len)
{
    const uint8_t *p  = (const uint8_t *) data;
    const size_t   bs = block_size(c->alg);

    c->len += len;
    while (len > 0) {
        size_t n = bs - c->fill;

        if (n > len)
            n = len;
        memcpy(c->buf + c->fill, p, n);
        c->fill += n;
        p += n;
        len -= n;
        if (c->fill == bs) {
            compress_block(c, c->buf);
            c->fill = 0;
        }
    }
}

void
hash_final(hash_ctx_t *c, uint8_t *out)
{
    const size_t bs = block_size(c->alg);

    if (is_sha3(c->alg)) {
        memset(c->buf + c->fill, 0, bs - c->fill);
        c->buf[c->fill] ^= 0x06;
        c->buf[bs - 1] ^= 0x80;
        compress_block(c, c->buf);
        for (size_t i = 0; i < hash_len(c->alg); i++)
            out[i] = (uint8_t) (c->st.s64[i / 8] >> (8 * (i % 8)));
        crypto_wipe(c, sizeof(*c));
        return;
    }

    {
        const size_t   lenbytes = (bs == 128) ? 16 : 8;
        const uint64_t bits     = c->len * 8;

        c->buf[c->fill++] = 0x80;
        if (c->fill > (bs - lenbytes)) {
            memset(c->buf + c->fill, 0, bs - c->fill);
            compress_block(c, c->buf);
            c->fill = 0;
        }
        memset(c->buf + c->fill, 0, bs - c->fill);
        if ((c->alg == HASH_MD4) || (c->alg == HASH_MD5)) {
            for (int i = 0; i < 8; i++)
                c->buf[bs - 8 + i] = (uint8_t) (bits >> (8 * i));
        } else {
            for (int i = 0; i < 8; i++)
                c->buf[bs - 1 - i] = (uint8_t) (bits >> (8 * i));
        }
        compress_block(c, c->buf);
    }

    switch (c->alg) {
        case HASH_MD4:
        case HASH_MD5:
            for (int i = 0; i < 16; i++)
                out[i] = (uint8_t) (c->st.s32[i / 4] >> (8 * (i % 4)));
            break;
        case HASH_SHA1:
        case HASH_SHA256:
            for (size_t i = 0; i < hash_len(c->alg); i++)
                out[i] = (uint8_t) (c->st.s32[i / 4] >> (24 - 8 * (i % 4)));
            break;
        default:
            for (size_t i = 0; i < hash_len(c->alg); i++)
                out[i] = (uint8_t) (c->st.s64[i / 8] >> (56 - 8 * (i % 8)));
            break;
    }
    crypto_wipe(c, sizeof(*c));
}

void
hash(int alg, const void *data, size_t len, uint8_t *out)
{
    hash_ctx_t c;

    hash_init(&c, alg);
    hash_update(&c, data, len);
    hash_final(&c, out);
}

/* ------------------------------------------------------------------ DES */

static const uint8_t des_ip[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7
};
static const uint8_t des_fp[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25
};
static const uint8_t des_e[48] = {
    32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1
};
static const uint8_t des_p[32] = {
    16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
    2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25
};
static const uint8_t des_pc1[56] = {
    57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43, 35, 27, 19, 11, 3,  60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15, 7,  62, 54, 46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4
};
static const uint8_t des_pc2[48] = {
    14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,  26, 8,  16, 7,  27, 20, 13, 2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32
};
static const uint8_t des_shifts[16] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };
static const uint8_t des_s[8][64]   = {
    { 14, 4,  13, 1,  2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0,  7,  0,  15, 7,  4,  14, 2,
      13, 1,  10, 6,  12, 11, 9,  5,  3,  8,  4,  1,  14, 8,  13, 6,  2,  11, 15, 12, 9,  7,
      3,  10, 5,  0,  15, 12, 8,  2,  4,  9,  1,  7,  5,  11, 3,  14, 10, 0,  6,  13 },
    { 15, 1,  8,  14, 6,  11, 3,  4,  9,  7,  2,  13, 12, 0,  5,  10, 3,  13, 4,  7,  15, 2,
      8,  14, 12, 0,  1,  10, 6,  9,  11, 5,  0,  14, 7,  11, 10, 4,  13, 1,  5,  8,  12, 6,
      9,  3,  2,  15, 13, 8,  10, 1,  3,  15, 4,  2,  11, 6,  7,  12, 0,  5,  14, 9 },
    { 10, 0,  9,  14, 6,  3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,  13, 7,  0,  9,  3,  4,
      6,  10, 2,  8,  5,  14, 12, 11, 15, 1,  13, 6,  4,  9,  8,  15, 3,  0,  11, 1,  2,  12,
      5,  10, 14, 7,  1,  10, 13, 0,  6,  9,  8,  7,  4,  15, 14, 3,  11, 5,  2,  12 },
    { 7,  13, 14, 3,  0,  6,  9,  10, 1,  2,  8,  5,  11, 12, 4,  15, 13, 8,  11, 5,  6,  15,
      0,  3,  4,  7,  2,  12, 1,  10, 14, 9,  10, 6,  9,  0,  12, 11, 7,  13, 15, 1,  3,  14,
      5,  2,  8,  4,  3,  15, 0,  6,  10, 1,  13, 8,  9,  4,  5,  11, 12, 7,  2,  14 },
    { 2,  12, 4,  1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0,  14, 9,  14, 11, 2,  12, 4,  7,
      13, 1,  5,  0,  15, 10, 3,  9,  8,  6,  4,  2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,
      6,  3,  0,  14, 11, 8,  12, 7,  1,  14, 2,  13, 6,  15, 0,  9,  10, 4,  5,  3 },
    { 12, 1,  10, 15, 9,  2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11, 10, 15, 4,  2,  7,  12,
      9,  5,  6,  1,  13, 14, 0,  11, 3,  8,  9,  14, 15, 5,  2,  8,  12, 3,  7,  0,  4,  10,
      1,  13, 11, 6,  4,  3,  2,  12, 9,  5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13 },
    { 4,  11, 2,  14, 15, 0,  8,  13, 3,  12, 9,  7,  5,  10, 6,  1,  13, 0,  11, 7,  4,  9,
      1,  10, 14, 3,  5,  12, 2,  15, 8,  6,  1,  4,  11, 13, 12, 3,  7,  14, 10, 15, 6,  8,
      0,  5,  9,  2,  6,  11, 13, 8,  1,  4,  10, 7,  9,  5,  0,  15, 14, 2,  3,  12 },
    { 13, 2,  8,  4,  6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,  1,  15, 13, 8,  10, 3,
      7,  4,  12, 5,  6,  11, 0,  14, 9,  2,  7,  11, 4,  1,  9,  12, 14, 2,  0,  6,  10, 13,
      15, 3,  5,  8,  2,  1,  14, 7,  4,  10, 8,  13, 15, 12, 9,  0,  3,  5,  6,  11 }
};

/* Bit n (1 = most significant) of a value `width` bits wide. */
static uint64_t
permute(uint64_t in, int in_width, const uint8_t *table, int out_width)
{
    uint64_t out = 0;

    for (int i = 0; i < out_width; i++)
        out = (out << 1) | ((in >> (in_width - table[i])) & 1);
    return out;
}

static void
des_encrypt64(uint64_t key, const uint8_t in[8], uint8_t out[8])
{
    uint64_t sub[16];
    uint64_t cd = permute(key, 64, des_pc1, 56);
    uint32_t c  = (uint32_t) (cd >> 28) & 0x0fffffff;
    uint32_t d  = (uint32_t) cd & 0x0fffffff;
    uint64_t blk;
    uint32_t l, r;

    for (int i = 0; i < 16; i++) {
        for (int s = 0; s < des_shifts[i]; s++) {
            c = ((c << 1) | (c >> 27)) & 0x0fffffff;
            d = ((d << 1) | (d >> 27)) & 0x0fffffff;
        }
        sub[i] = permute(((uint64_t) c << 28) | d, 56, des_pc2, 48);
    }

    blk = be64(in);
    blk = permute(blk, 64, des_ip, 64);
    l   = (uint32_t) (blk >> 32);
    r   = (uint32_t) blk;
    for (int i = 0; i < 16; i++) {
        const uint64_t x = permute(r, 32, des_e, 48) ^ sub[i];
        uint32_t       f = 0;
        uint32_t       t;

        for (int s = 0; s < 8; s++) {
            const int six = (int) (x >> (42 - 6 * s)) & 0x3f;
            const int row = ((six & 0x20) >> 4) | (six & 1);
            const int col = (six >> 1) & 0x0f;

            f = (f << 4) | des_s[s][row * 16 + col];
        }
        f = (uint32_t) permute(f, 32, des_p, 32);
        t = r;
        r = l ^ f;
        l = t;
    }
    blk = permute(((uint64_t) r << 32) | l, 64, des_fp, 64);
    for (int i = 0; i < 8; i++)
        out[i] = (uint8_t) (blk >> (56 - 8 * i));
}

void
des_encrypt7(const uint8_t k[7], const uint8_t in[8], uint8_t out[8])
{
    uint8_t key[8];

    /* Seven bytes spread over eight, the parity bit (ignored) last. */
    key[0] = k[0] & 0xfe;
    key[1] = (uint8_t) ((k[0] << 7) | (k[1] >> 1));
    key[2] = (uint8_t) ((k[1] << 6) | (k[2] >> 2));
    key[3] = (uint8_t) ((k[2] << 5) | (k[3] >> 3));
    key[4] = (uint8_t) ((k[3] << 4) | (k[4] >> 4));
    key[5] = (uint8_t) ((k[4] << 3) | (k[5] >> 5));
    key[6] = (uint8_t) ((k[5] << 2) | (k[6] >> 6));
    key[7] = (uint8_t) (k[6] << 1);
    des_encrypt64(be64(key), in, out);
    crypto_wipe(key, sizeof(key));
}

/* ------------------------------------------------------------------ RC4 */

void
rc4_init(rc4_t *r, const uint8_t *key, size_t len)
{
    uint8_t j = 0;

    for (int i = 0; i < 256; i++)
        r->s[i] = (uint8_t) i;
    for (int i = 0; i < 256; i++) {
        const uint8_t t = r->s[i];

        j        = (uint8_t) (j + t + key[i % len]);
        r->s[i]  = r->s[j];
        r->s[j]  = t;
    }
    r->i = r->j = 0;
}

void
rc4_crypt(rc4_t *r, const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t i = r->i, j = r->j;

    for (size_t n = 0; n < len; n++) {
        uint8_t t;

        i       = (uint8_t) (i + 1);
        t       = r->s[i];
        j       = (uint8_t) (j + t);
        r->s[i] = r->s[j];
        r->s[j] = t;
        out[n]  = in[n] ^ r->s[(uint8_t) (r->s[i] + t)];
    }
    r->i = i;
    r->j = j;
}

/* --------------------------------------------------------- HMAC, PBKDF2 */

void
hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[SHA256_LEN])
{
    uint8_t    k[64];
    uint8_t    pad[64];
    uint8_t    inner[SHA256_LEN];
    hash_ctx_t c;

    memset(k, 0, sizeof(k));
    if (klen > sizeof(k))
        hash(HASH_SHA256, key, klen, k);
    else if (klen > 0)
        memcpy(k, key, klen);

    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    hash_init(&c, HASH_SHA256);
    hash_update(&c, pad, 64);
    hash_update(&c, msg, mlen);
    hash_final(&c, inner);

    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5c;
    hash_init(&c, HASH_SHA256);
    hash_update(&c, pad, 64);
    hash_update(&c, inner, sizeof(inner));
    hash_final(&c, out);
    crypto_wipe(k, sizeof(k));
    crypto_wipe(pad, sizeof(pad));
    crypto_wipe(inner, sizeof(inner));
}

void
pbkdf2_sha256(const char *password, const uint8_t *salt, size_t slen, uint32_t rounds, uint8_t *out, size_t olen)
{
    const size_t plen = strlen(password);
    uint8_t      msg[256];
    uint32_t     block = 1;

    if (slen > (sizeof(msg) - 4))
        slen = sizeof(msg) - 4;
    while (olen > 0) {
        uint8_t      u[SHA256_LEN];
        uint8_t      t[SHA256_LEN];
        const size_t n = (olen < SHA256_LEN) ? olen : SHA256_LEN;

        memcpy(msg, salt, slen);
        msg[slen]     = (uint8_t) (block >> 24);
        msg[slen + 1] = (uint8_t) (block >> 16);
        msg[slen + 2] = (uint8_t) (block >> 8);
        msg[slen + 3] = (uint8_t) block;
        hmac_sha256((const uint8_t *) password, plen, msg, slen + 4, u);
        memcpy(t, u, sizeof(t));
        for (uint32_t r = 1; r < rounds; r++) {
            hmac_sha256((const uint8_t *) password, plen, u, sizeof(u), u);
            for (int i = 0; i < SHA256_LEN; i++)
                t[i] ^= u[i];
        }
        memcpy(out, t, n);
        out += n;
        olen -= n;
        block++;
        crypto_wipe(u, sizeof(u));
        crypto_wipe(t, sizeof(t));
    }
}

/* ------------------------------------------------------------ the rest */

int
crypto_random(uint8_t *out, size_t len)
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, out, (ULONG) len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    size_t done = 0;

#    if defined(__linux__)
    while (done < len) {
        const ssize_t r = getrandom(out + done, len - done, 0);

        if (r > 0)
            done += (size_t) r;
        else if ((r < 0) && (errno != EINTR))
            break;
    }
    if (done == len)
        return 1;
#    endif
    {
        const int fd = open("/dev/urandom", O_RDONLY);

        if (fd < 0)
            return 0;
        while (done < len) {
            const ssize_t r = read(fd, out + done, len - done);

            if (r <= 0) {
                if ((r < 0) && (errno == EINTR))
                    continue;
                break;
            }
            done += (size_t) r;
        }
        close(fd);
    }
    return done == len;
#endif
}

int
crypto_equal(const void *a, const void *b, size_t len)
{
    const volatile uint8_t *x = (const volatile uint8_t *) a;
    const volatile uint8_t *y = (const volatile uint8_t *) b;
    uint8_t                 d = 0;

    for (size_t i = 0; i < len; i++)
        d |= x[i] ^ y[i];
    return d == 0;
}

void
crypto_wipe(void *p, size_t len)
{
    volatile uint8_t *v = (volatile uint8_t *) p;

    while (len--)
        *v++ = 0;
}

void
hex_encode(const uint8_t *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < len; i++) {
        out[2 * i]     = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * len] = '\0';
}

static int
hexdigit(char c)
{
    if ((c >= '0') && (c <= '9'))
        return c - '0';
    if ((c >= 'a') && (c <= 'f'))
        return c - 'a' + 10;
    if ((c >= 'A') && (c <= 'F'))
        return c - 'A' + 10;
    return -1;
}

int
hex_decode(const char *in, uint8_t *out, size_t len)
{
    if (strlen(in) != (2 * len))
        return 0;
    for (size_t i = 0; i < len; i++) {
        const int hi = hexdigit(in[2 * i]);
        const int lo = hexdigit(in[2 * i + 1]);

        if ((hi < 0) || (lo < 0))
            return 0;
        out[i] = (uint8_t) ((hi << 4) | lo);
    }
    return 1;
}
