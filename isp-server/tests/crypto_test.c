/*
 * 86Box-Next: the ISP's hashes, ciphers and authentication arithmetic
 * (isp_crypto.c, ppp_auth.c) against published test vectors: FIPS and RFC
 * examples, and the worked examples of RFC 2433, RFC 2759 and RFC 3079.
 * No test framework; non-zero on failure.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "isp_crypto.h"
#include "ppp_auth.h"

static int failures;

static void
check_hex(const char *what, const uint8_t *got, size_t len, const char *want)
{
    char hex[300];

    hex_encode(got, len, hex);
    if (strcmp(hex, want)) {
        printf("FAIL %s\n  got  %s\n  want %s\n", what, hex, want);
        failures++;
    } else
        printf("ok   %s\n", what);
}

static void
check_str(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want)) {
        printf("FAIL %s\n  got  %s\n  want %s\n", what, got, want);
        failures++;
    } else
        printf("ok   %s\n", what);
}

static void
unhex(const char *s, uint8_t *out)
{
    size_t n = 0;

    for (; *s; s++) {
        unsigned v;

        if (*s == ' ')
            continue;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t) v;
        s++;
    }
}

static void
test_hashes(void)
{
    static const struct {
        int         alg;
        const char *in;
        const char *out;
    } v[] = {
        { HASH_MD4, "abc", "a448017aaf21d8525fc10ae87aa6729d" },
        { HASH_MD4, "", "31d6cfe0d16ae931b73c59d7e0c089c0" },
        { HASH_MD5, "abc", "900150983cd24fb0d6963f7d28e17f72" },
        { HASH_MD5, "", "d41d8cd98f00b204e9800998ecf8427e" },
        { HASH_SHA1, "abc", "a9993e364706816aba3e25717850c26c9cd0d89d" },
        { HASH_SHA1, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1" },
        { HASH_SHA256, "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { HASH_SHA256, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        { HASH_SHA384, "abc",
          "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7" },
        { HASH_SHA512, "abc",
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd"
          "454d4423643ce80e2a9ac94fa54ca49f" },
        { HASH_SHA3_256, "", "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a" },
        { HASH_SHA3_256, "abc", "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532" },
        { HASH_SHA3_384, "abc",
          "ec01498288516fc926459f58e2c6ad8df9b473cb0fc08c2596da7cf0e49be4b298d88cea927ac7f539f1edf228376d25" },
        { HASH_SHA3_512, "abc",
          "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e10e116e9192af3c91a7ec57647e3934"
          "057340b4cf408d5a56592f8274eec53f0" },
    };
    uint8_t out[HASH_MAX];
    char    what[96];

    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        hash(v[i].alg, v[i].in, strlen(v[i].in), out);
        snprintf(what, sizeof(what), "%s(\"%.12s%s\")", hash_name(v[i].alg), v[i].in, strlen(v[i].in) > 12 ? "..." : "");
        check_hex(what, out, hash_len(v[i].alg), v[i].out);
    }

    /* A million 'a's, fed in uneven pieces, crosses every block boundary. */
    {
        static const char *const million[] = {
            "", "", "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0" };
        char       chunk[997];
        hash_ctx_t c;

        memset(chunk, 'a', sizeof(chunk));
        for (int alg = HASH_SHA1; alg <= HASH_SHA256; alg++) {
            size_t left = 1000000;

            hash_init(&c, alg);
            while (left > 0) {
                const size_t n = left < sizeof(chunk) ? left : sizeof(chunk);

                hash_update(&c, chunk, n);
                left -= n;
            }
            hash_final(&c, out);
            snprintf(what, sizeof(what), "%s(a million a's)", hash_name(alg));
            check_hex(what, out, hash_len(alg), million[alg]);
        }
    }
}

static void
test_des_rc4_hmac(void)
{
    uint8_t out[64];

    /* FIPS 81's classic: key 133457799BBCDFF1 is the 56 bits below with
       their parity. */
    {
        uint8_t k7[7];
        uint8_t in[8];

        unhex("12 69 5b c9 b7 b7 f8", k7);
        unhex("01 23 45 67 89 ab cd ef", in);
        des_encrypt7(k7, in, out);
        check_hex("DES(133457799BBCDFF1, 0123456789ABCDEF)", out, 8, "85e813540f0ab405");
    }

    {
        rc4_t r;

        rc4_init(&r, (const uint8_t *) "Key", 3);
        rc4_crypt(&r, (const uint8_t *) "Plaintext", out, 9);
        check_hex("RC4(Key, Plaintext)", out, 9, "bbf316e8d940af0ad3");
    }

    hmac_sha256((const uint8_t *) "Jefe", 4, (const uint8_t *) "what do ya want for nothing?", 28, out);
    check_hex("HMAC-SHA-256 (RFC 4231 case 2)", out, 32,
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    pbkdf2_sha256("password", (const uint8_t *) "salt", 4, 1, out, 32);
    check_hex("PBKDF2-SHA-256 (1 round)", out, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    pbkdf2_sha256("password", (const uint8_t *) "salt", 4, 4096, out, 32);
    check_hex("PBKDF2-SHA-256 (4096 rounds)", out, 32,
              "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
}

static void
test_mschap(void)
{
    uint8_t ph[16];
    uint8_t resp[24];
    uint8_t challenge[8];
    uint8_t auth[16];
    uint8_t peer[16];
    uint8_t ch[8];
    uint8_t nt[24];
    uint8_t send[16];
    uint8_t recv[16];
    uint8_t key[16];
    char    s[43];

    /* RFC 2433 B.2: "MyPw". */
    nt_password_hash("MyPw", ph);
    check_hex("NtPasswordHash(MyPw)", ph, 16, "fc156af7edcd6c0edde3337d427f4eac");
    unhex("10 2d b5 df 08 5d 30 41", challenge);
    challenge_response(challenge, ph, resp);
    check_hex("MS-CHAP NtChallengeResponse (RFC 2433 B.2)", resp, 24,
              "4e9d3c8f9cfd385d5bf4d3246791956ca4c351ab409a3d61");

    /* RFC 3079 2.5: the LAN Manager hash, and MS-CHAP's 128-bit start key. */
    lm_password_hash("clientPass", ph);
    check_hex("LmPasswordHash(clientPass)", ph, 16, "76a152936096d7830e2390227404afd2");
    mppe_mschap1_start_key("clientPass", challenge, 40, send, recv);
    mppe_new_key_from_sha(send, send, 8, key);
    check_hex("MPPE MS-CHAP 40-bit GetKey (RFC 3079 2.5.1)", key, 8, "d80801538cec4a08");
    mppe_mschap1_start_key("clientPass", challenge, 128, send, recv);
    /* (RFC 3079's step 3 says ...ac ca d1...; its step 4, and the session
       key that follows from it, have ...ac c1 d1....) */
    check_hex("MPPE MS-CHAP 128-bit start key (RFC 3079 2.5.3)", send, 16, "a8947850cfc0acc1d1789fb62ddcddb0");
    mppe_new_key_from_sha(send, send, 16, key);
    check_hex("MPPE MS-CHAP 128-bit session key (RFC 3079 2.5.3)", key, 16, "59d159bc09f76f1da2a86a28ffec0b1e");

    /* RFC 2759 9.2: "User", "clientPass". */
    unhex("5B 5D 7C 7D 7B 3F 2F 3E 3C 2C 60 21 32 26 26 28", auth);
    unhex("21 40 23 24 25 5E 26 2A 28 29 5F 2B 3A 33 7C 7E", peer);
    mschap2_challenge_hash(peer, auth, "User", ch);
    check_hex("MS-CHAP-2 ChallengeHash", ch, 8, "d02e4386bce91226");
    mschap2_challenge_hash(peer, auth, "DOMAIN\\User", ch);
    check_hex("MS-CHAP-2 ChallengeHash, domain dropped", ch, 8, "d02e4386bce91226");
    mschap2_nt_response(auth, peer, "User", "clientPass", nt);
    check_hex("MS-CHAP-2 NT-Response", nt, 24, "82309ecd8d708b5ea08faa3981cd83544233114a3d85d6df");
    mschap2_authenticator_response("clientPass", nt, peer, auth, "User", s);
    check_str("MS-CHAP-2 authenticator response", s, "S=407A5589115FD0D6209F510FE9C04566932CDA56");

    /* RFC 3079 3.5: the send key of its examples is the server's (the
       client receives with it). */
    mppe_mschap2_start_keys("clientPass", nt, 1, send, recv);
    check_hex("MPPE MS-CHAP-2 server send start key", send, 16, "8b7cdc149b993a1ba118cb153f56dccb");
    mppe_mschap2_start_keys("clientPass", nt, 0, key, recv);
    check_hex("MPPE MS-CHAP-2 client receive = server send", recv, 16, "8b7cdc149b993a1ba118cb153f56dccb");
    mppe_new_key_from_sha(send, send, 16, key);
    check_hex("MPPE MS-CHAP-2 128-bit session key", key, 16, "405cb2247a7956e6e211007ae27b22d4");
    {
        rc4_t   r;
        uint8_t out[12];

        rc4_init(&r, key, 16);
        rc4_crypt(&r, (const uint8_t *) "test message", out, 12);
        check_hex("MPPE 128-bit sample message", out, 12, "81848317df68846272fb5abe");
    }
    mppe_new_key_from_sha(send, send, 8, key);
    key[0] = 0xd1;
    key[1] = 0x26;
    key[2] = 0x9e;
    check_hex("MPPE MS-CHAP-2 40-bit session key", key, 8, "d1269ec49fa62e3e");
}

static void
test_chap(void)
{
    uint8_t out[HASH_MAX];
    uint8_t ch[16];
    size_t  n;

    /* RFC 1994's digest is MD5(id || secret || challenge). */
    memset(ch, 0x11, sizeof(ch));
    n = chap_digest(CHAP_MD5, 7, "secret", ch, sizeof(ch), out);
    {
        hash_ctx_t c;
        uint8_t    want[16];
        uint8_t    id = 7;

        hash_init(&c, HASH_MD5);
        hash_update(&c, &id, 1);
        hash_update(&c, "secret", 6);
        hash_update(&c, ch, sizeof(ch));
        hash_final(&c, want);
        if ((n != 16) || memcmp(out, want, 16)) {
            printf("FAIL CHAP-MD5 digest\n");
            failures++;
        } else
            printf("ok   CHAP-MD5 digest\n");
    }
    if ((chap_digest(CHAP_SHA256, 1, "x", ch, 4, out) != 32) || (chap_digest(CHAP_SHA3_512, 1, "x", ch, 4, out) != 64) ||
        (chap_digest(CHAP_MSCHAP2, 1, "x", ch, 4, out) != 0)) {
        printf("FAIL CHAP digest lengths\n");
        failures++;
    } else
        printf("ok   CHAP digest lengths\n");
    if ((ppp_ap_from_chap(CHAP_SHA384) != PPP_AP_CHAP_SHA384) || (ppp_ap_chap_alg(PPP_AP_MSCHAP2) != CHAP_MSCHAP2) ||
        (ppp_ap_from_key("chap-sha3-256") != PPP_AP_CHAP_SHA3_256)) {
        printf("FAIL method table\n");
        failures++;
    } else
        printf("ok   method table\n");
}

int
main(void)
{
    test_hashes();
    test_des_rc4_hmac();
    test_mschap();
    test_chap();
    {
        uint8_t a[32], b[32];

        if (!crypto_random(a, sizeof(a)) || !crypto_random(b, sizeof(b)) || !memcmp(a, b, sizeof(a))) {
            printf("FAIL random\n");
            failures++;
        } else
            printf("ok   random\n");
    }
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "all checks passed", failures);
    return failures ? 1 : 0;
}
