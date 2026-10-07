/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The arithmetic of PPP authentication: CHAP's digest with MD5
 *             (RFC 1994) or SHA-1/2/3 (IANA's CHAP algorithms 6 to 12),
 *             Microsoft's MS-CHAP (RFC 2433) and MS-CHAP-2 (RFC 2759), and
 *             the MPPE keys they lead to (RFC 3079).  Pure functions; the
 *             protocol is ppp_session.c's.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef PPP_AUTH_H
#define PPP_AUTH_H

#include <stddef.h>
#include <stdint.h>

/* The Algorithm byte of LCP's Authentication-Protocol option for CHAP. */
#define CHAP_MD5      0x05
#define CHAP_SHA1     0x06
#define CHAP_SHA256   0x07
#define CHAP_SHA3_256 0x08
#define CHAP_SHA384   0x09
#define CHAP_SHA3_384 0x0a
#define CHAP_SHA512   0x0b
#define CHAP_SHA3_512 0x0c
#define CHAP_MSCHAP1  0x80
#define CHAP_MSCHAP2  0x81

/* The authentication methods, as a set (isp_settings_t.auth_protos). */
enum {
    PPP_AP_PAP = 0,
    PPP_AP_CHAP_MD5,
    PPP_AP_MSCHAP1,
    PPP_AP_MSCHAP2,
    PPP_AP_CHAP_SHA1,
    PPP_AP_CHAP_SHA256,
    PPP_AP_CHAP_SHA384,
    PPP_AP_CHAP_SHA512,
    PPP_AP_CHAP_SHA3_256,
    PPP_AP_CHAP_SHA3_384,
    PPP_AP_CHAP_SHA3_512,
    PPP_AP_COUNT
};
#define PPP_AP_ALL ((1u << PPP_AP_COUNT) - 1)

extern const char *ppp_ap_name(int ap);   /* "MS-CHAP-2" */
extern const char *ppp_ap_key(int ap);    /* "mschap2", for the .ini and the page */
extern int         ppp_ap_from_key(const char *key);
extern int         ppp_ap_chap_alg(int ap);   /* CHAP_*, 0 for PAP */
extern int         ppp_ap_from_chap(int alg); /* PPP_AP_*, -1 if unknown */

/* CHAP with a hash: H(Identifier || secret || challenge).  Returns its
   length, 0 for an algorithm that is not one of these. */
extern size_t chap_digest(int alg, uint8_t id, const char *secret, const uint8_t *challenge, size_t clen,
                          uint8_t *out);

/* Microsoft's hashes of a password (UTF-8 here; NT hashes UTF-16LE). */
extern void nt_password_hash(const char *password, uint8_t out[16]);
extern void lm_password_hash(const char *password, uint8_t out[16]);
extern void challenge_response(const uint8_t challenge[8], const uint8_t pwhash[16], uint8_t out[24]);

/* MS-CHAP-2. */
extern void mschap2_challenge_hash(const uint8_t peer[16], const uint8_t auth[16], const char *user, uint8_t out[8]);
extern void mschap2_nt_response(const uint8_t auth[16], const uint8_t peer[16], const char *user, const char *password,
                                uint8_t out[24]);
/* "S=" and 40 uppercase hex digits. */
extern void mschap2_authenticator_response(const char *password, const uint8_t nt_response[24], const uint8_t peer[16],
                                           const uint8_t auth[16], const char *user, char out[43]);
/* The name without a "DOMAIN\" in front: what MS-CHAP-2 hashes. */
extern const char *mschap_user_part(const char *name);

/* MPPE's keys (RFC 3079).  `len` is 8 for 40- and 56-bit keys, 16 for
   128-bit.  MS-CHAP: one master key both ways; MS-CHAP-2: one each way, the
   server's view here. */
extern void mppe_mschap1_start_key(const char *password, const uint8_t challenge[8], int bits, uint8_t send[16],
                                   uint8_t recv[16]);
extern void mppe_mschap2_start_keys(const char *password, const uint8_t nt_response[24], int is_server, uint8_t send[16],
                                    uint8_t recv[16]);
/* RFC 3078's GetNewKeyFromSHA. */
extern void mppe_new_key_from_sha(const uint8_t *start, const uint8_t *session, size_t len, uint8_t *out);

#endif /* PPP_AUTH_H */
