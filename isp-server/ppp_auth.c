/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             CHAP digests, MS-CHAP and MS-CHAP-2 responses, and MPPE's
 *             keys.  See ppp_auth.h.  The functions follow the pseudocode
 *             of RFC 2433, RFC 2759 and RFC 3079 step by step, and the
 *             tests check them against those RFCs' worked examples.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "isp_crypto.h"
#include "ppp_auth.h"

static const struct {
    const char *name;
    const char *key;
    int         chap;
    int         hash;
} aps[PPP_AP_COUNT] = {
    [PPP_AP_PAP]           = { "PAP", "pap", 0, -1 },
    [PPP_AP_CHAP_MD5]      = { "CHAP-MD5", "chap-md5", CHAP_MD5, HASH_MD5 },
    [PPP_AP_MSCHAP1]       = { "MS-CHAP", "mschap", CHAP_MSCHAP1, -1 },
    [PPP_AP_MSCHAP2]       = { "MS-CHAP-2", "mschap2", CHAP_MSCHAP2, -1 },
    [PPP_AP_CHAP_SHA1]     = { "CHAP-SHA1", "chap-sha1", CHAP_SHA1, HASH_SHA1 },
    [PPP_AP_CHAP_SHA256]   = { "CHAP-SHA256", "chap-sha256", CHAP_SHA256, HASH_SHA256 },
    [PPP_AP_CHAP_SHA384]   = { "CHAP-SHA384", "chap-sha384", CHAP_SHA384, HASH_SHA384 },
    [PPP_AP_CHAP_SHA512]   = { "CHAP-SHA512", "chap-sha512", CHAP_SHA512, HASH_SHA512 },
    [PPP_AP_CHAP_SHA3_256] = { "CHAP-SHA3-256", "chap-sha3-256", CHAP_SHA3_256, HASH_SHA3_256 },
    [PPP_AP_CHAP_SHA3_384] = { "CHAP-SHA3-384", "chap-sha3-384", CHAP_SHA3_384, HASH_SHA3_384 },
    [PPP_AP_CHAP_SHA3_512] = { "CHAP-SHA3-512", "chap-sha3-512", CHAP_SHA3_512, HASH_SHA3_512 },
};

const char *
ppp_ap_name(int ap)
{
    return ((ap >= 0) && (ap < PPP_AP_COUNT)) ? aps[ap].name : "?";
}

const char *
ppp_ap_key(int ap)
{
    return ((ap >= 0) && (ap < PPP_AP_COUNT)) ? aps[ap].key : "?";
}

int
ppp_ap_from_key(const char *key)
{
    for (int i = 0; i < PPP_AP_COUNT; i++)
        if (!strcmp(aps[i].key, key))
            return i;
    return -1;
}

int
ppp_ap_chap_alg(int ap)
{
    return ((ap >= 0) && (ap < PPP_AP_COUNT)) ? aps[ap].chap : 0;
}

int
ppp_ap_from_chap(int alg)
{
    for (int i = 1; i < PPP_AP_COUNT; i++)
        if (aps[i].chap == alg)
            return i;
    return -1;
}

size_t
chap_digest(int alg, uint8_t id, const char *secret, const uint8_t *challenge, size_t clen, uint8_t *out)
{
    const int  ap = ppp_ap_from_chap(alg);
    hash_ctx_t c;

    if ((ap < 0) || (aps[ap].hash < 0))
        return 0;
    hash_init(&c, aps[ap].hash);
    hash_update(&c, &id, 1);
    hash_update(&c, secret, strlen(secret));
    hash_update(&c, challenge, clen);
    hash_final(&c, out);
    return hash_len(aps[ap].hash);
}

/* ------------------------------------------------------------ MS-CHAP */

/* UTF-8 to UTF-16LE, as Windows hashes passwords; at most 256 characters
   (RFC 2759).  Returns the byte length. */
static size_t
utf16le(const char *s, uint8_t *out, size_t max_units)
{
    const uint8_t *p = (const uint8_t *) s;
    size_t         n = 0;

    while (*p && (n < max_units)) {
        uint32_t cp;

        if (*p < 0x80)
            cp = *p++;
        else if (((*p & 0xe0) == 0xc0) && ((p[1] & 0xc0) == 0x80)) {
            cp = ((uint32_t) (p[0] & 0x1f) << 6) | (p[1] & 0x3f);
            p += 2;
        } else if (((*p & 0xf0) == 0xe0) && ((p[1] & 0xc0) == 0x80) && ((p[2] & 0xc0) == 0x80)) {
            cp = ((uint32_t) (p[0] & 0x0f) << 12) | ((uint32_t) (p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            p += 3;
        } else if (((*p & 0xf8) == 0xf0) && ((p[1] & 0xc0) == 0x80) && ((p[2] & 0xc0) == 0x80) &&
                   ((p[3] & 0xc0) == 0x80)) {
            cp = ((uint32_t) (p[0] & 0x07) << 18) | ((uint32_t) (p[1] & 0x3f) << 12) |
                 ((uint32_t) (p[2] & 0x3f) << 6) | (p[3] & 0x3f);
            p += 4;
        } else {
            cp = *p++; /* not UTF-8: take the byte as Latin-1 */
        }
        if ((cp >= 0x10000) && (n + 1 < max_units)) {
            const uint32_t v  = cp - 0x10000;
            const uint16_t hi = (uint16_t) (0xd800 | (v >> 10));
            const uint16_t lo = (uint16_t) (0xdc00 | (v & 0x3ff));

            out[2 * n]     = (uint8_t) hi;
            out[2 * n + 1] = (uint8_t) (hi >> 8);
            n++;
            out[2 * n]     = (uint8_t) lo;
            out[2 * n + 1] = (uint8_t) (lo >> 8);
            n++;
        } else if (cp < 0x10000) {
            out[2 * n]     = (uint8_t) cp;
            out[2 * n + 1] = (uint8_t) (cp >> 8);
            n++;
        }
    }
    return 2 * n;
}

void
nt_password_hash(const char *password, uint8_t out[16])
{
    uint8_t      u[512];
    const size_t n = utf16le(password, u, 256);

    hash(HASH_MD4, u, n, out);
    crypto_wipe(u, sizeof(u));
}

void
lm_password_hash(const char *password, uint8_t out[16])
{
    static const uint8_t std_text[8] = { 'K', 'G', 'S', '!', '@', '#', '$', '%' };
    uint8_t              up[14];

    memset(up, 0, sizeof(up));
    for (size_t i = 0; (i < 14) && password[i]; i++) {
        const char c = password[i];

        up[i] = (uint8_t) (((c >= 'a') && (c <= 'z')) ? (c - 'a' + 'A') : c);
    }
    des_encrypt7(up, std_text, out);
    des_encrypt7(up + 7, std_text, out + 8);
    crypto_wipe(up, sizeof(up));
}

void
challenge_response(const uint8_t challenge[8], const uint8_t pwhash[16], uint8_t out[24])
{
    uint8_t z[21];

    memset(z, 0, sizeof(z));
    memcpy(z, pwhash, 16);
    des_encrypt7(z, challenge, out);
    des_encrypt7(z + 7, challenge, out + 8);
    des_encrypt7(z + 14, challenge, out + 16);
    crypto_wipe(z, sizeof(z));
}

const char *
mschap_user_part(const char *name)
{
    const char *bs = strrchr(name, '\\');

    return bs ? (bs + 1) : name;
}

void
mschap2_challenge_hash(const uint8_t peer[16], const uint8_t auth[16], const char *user, uint8_t out[8])
{
    hash_ctx_t  c;
    uint8_t     d[SHA1_LEN];
    const char *u = mschap_user_part(user);

    hash_init(&c, HASH_SHA1);
    hash_update(&c, peer, 16);
    hash_update(&c, auth, 16);
    hash_update(&c, u, strlen(u));
    hash_final(&c, d);
    memcpy(out, d, 8);
}

void
mschap2_nt_response(const uint8_t auth[16], const uint8_t peer[16], const char *user, const char *password,
                    uint8_t out[24])
{
    uint8_t ch[8];
    uint8_t ph[16];

    mschap2_challenge_hash(peer, auth, user, ch);
    nt_password_hash(password, ph);
    challenge_response(ch, ph, out);
    crypto_wipe(ph, sizeof(ph));
}

void
mschap2_authenticator_response(const char *password, const uint8_t nt_response[24], const uint8_t peer[16],
                               const uint8_t auth[16], const char *user, char out[43])
{
    static const char magic1[] = "Magic server to client signing constant";
    static const char magic2[] = "Pad to make it do more than one iteration";
    static const char digits[] = "0123456789ABCDEF";
    uint8_t           ph[16];
    uint8_t           phh[16];
    uint8_t           d[SHA1_LEN];
    uint8_t           ch[8];
    hash_ctx_t        c;

    nt_password_hash(password, ph);
    hash(HASH_MD4, ph, 16, phh);

    hash_init(&c, HASH_SHA1);
    hash_update(&c, phh, 16);
    hash_update(&c, nt_response, 24);
    hash_update(&c, magic1, sizeof(magic1) - 1);
    hash_final(&c, d);

    mschap2_challenge_hash(peer, auth, user, ch);
    hash_init(&c, HASH_SHA1);
    hash_update(&c, d, sizeof(d));
    hash_update(&c, ch, 8);
    hash_update(&c, magic2, sizeof(magic2) - 1);
    hash_final(&c, d);

    out[0] = 'S';
    out[1] = '=';
    for (int i = 0; i < SHA1_LEN; i++) {
        out[2 + 2 * i]     = digits[d[i] >> 4];
        out[2 + 2 * i + 1] = digits[d[i] & 15];
    }
    out[42] = '\0';
    crypto_wipe(ph, sizeof(ph));
    crypto_wipe(phh, sizeof(phh));
}

/* ------------------------------------------------------------- MPPE keys */

static const uint8_t sha_pad1[40] = { 0 };
static const uint8_t sha_pad2[40] = {
    0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2,
    0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2,
    0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2
};

void
mppe_new_key_from_sha(const uint8_t *start, const uint8_t *session, size_t len, uint8_t *out)
{
    hash_ctx_t c;
    uint8_t    d[SHA1_LEN];

    hash_init(&c, HASH_SHA1);
    hash_update(&c, start, len);
    hash_update(&c, sha_pad1, 40);
    hash_update(&c, session, len);
    hash_update(&c, sha_pad2, 40);
    hash_final(&c, d);
    memcpy(out, d, len);
}

void
mppe_mschap1_start_key(const char *password, const uint8_t challenge[8], int bits, uint8_t send[16],
                       uint8_t recv[16])
{
    uint8_t key[16];

    memset(key, 0, sizeof(key));
    if (bits == 128) {
        /* Get_Start_Key(): SHA-1 of the NT hash's hash, twice, and the
           challenge. */
        uint8_t    ph[16];
        uint8_t    phh[16];
        uint8_t    d[SHA1_LEN];
        hash_ctx_t c;

        nt_password_hash(password, ph);
        hash(HASH_MD4, ph, 16, phh);
        hash_init(&c, HASH_SHA1);
        hash_update(&c, phh, 16);
        hash_update(&c, phh, 16);
        hash_update(&c, challenge, 8);
        hash_final(&c, d);
        memcpy(key, d, 16);
        crypto_wipe(ph, sizeof(ph));
        crypto_wipe(phh, sizeof(phh));
    } else {
        /* 40 and 56 bits: the first half of the LAN Manager hash. */
        uint8_t lm[16];

        lm_password_hash(password, lm);
        memcpy(key, lm, 8);
        crypto_wipe(lm, sizeof(lm));
    }
    memcpy(send, key, 16);
    memcpy(recv, key, 16);
    crypto_wipe(key, sizeof(key));
}

void
mppe_mschap2_start_keys(const char *password, const uint8_t nt_response[24], int is_server, uint8_t send[16],
                        uint8_t recv[16])
{
    static const char magic1[] = "This is the MPPE Master Key";
    static const char magic2[] = "On the client side, this is the send key; on the server side, it is the receive key.";
    static const char magic3[] = "On the client side, this is the receive key; on the server side, it is the send key.";
    uint8_t           ph[16];
    uint8_t           phh[16];
    uint8_t           master[16];
    uint8_t           d[SHA1_LEN];
    hash_ctx_t        c;

    nt_password_hash(password, ph);
    hash(HASH_MD4, ph, 16, phh);
    hash_init(&c, HASH_SHA1);
    hash_update(&c, phh, 16);
    hash_update(&c, nt_response, 24);
    hash_update(&c, magic1, sizeof(magic1) - 1);
    hash_final(&c, d);
    memcpy(master, d, 16);

    for (int dir = 0; dir < 2; dir++) {
        const int   is_send = (dir == 0);
        const char *s       = (is_send == is_server) ? magic3 : magic2;

        hash_init(&c, HASH_SHA1);
        hash_update(&c, master, 16);
        hash_update(&c, sha_pad1, 40);
        hash_update(&c, s, 84);
        hash_update(&c, sha_pad2, 40);
        hash_final(&c, d);
        memcpy(is_send ? send : recv, d, 16);
    }
    crypto_wipe(ph, sizeof(ph));
    crypto_wipe(phh, sizeof(phh));
    crypto_wipe(master, sizeof(master));
}
