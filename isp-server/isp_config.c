/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             isp-server's settings as words.  See isp_config.h.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "isp.h"
#include "isp_config.h"

static const char *const auth_keys[] = { "none", "any", "accounts" };
static const char *const mppe_keys[] = { "off", "allowed", "required" };

const char *
isp_auth_key(int auth)
{
    return ((auth >= 0) && (auth <= 2)) ? auth_keys[auth] : "none";
}

int
isp_auth_parse(const char *s)
{
    for (int i = 0; i < 3; i++)
        if (!strcmp(s, auth_keys[i]))
            return i;
    return -1;
}

const char *
isp_mppe_key(int mppe)
{
    return ((mppe >= 0) && (mppe <= 2)) ? mppe_keys[mppe] : "off";
}

int
isp_mppe_parse(const char *s)
{
    for (int i = 0; i < 3; i++)
        if (!strcmp(s, mppe_keys[i]))
            return i;
    return -1;
}

static void
append(char *out, size_t len, const char *word)
{
    const size_t used = strlen(out);

    snprintf(out + used, len - used, "%s%s", used ? " " : "", word);
}

/* Each word of `s` (spaces or commas between) through `fn`; -1 at the
   first it does not know. */
static int
each_word(const char *s, int (*fn)(const char *word, uint32_t *bits), uint32_t *bits)
{
    uint32_t got = 0;

    while (*s) {
        char         w[32];
        const size_t n = strcspn(s, " ,;\t");

        if (n == 0) {
            s++;
            continue;
        }
        if (n >= sizeof(w))
            return -1;
        memcpy(w, s, n);
        w[n] = '\0';
        if (fn(w, &got) < 0)
            return -1;
        s += n;
    }
    *bits = got;
    return 0;
}

void
isp_methods_format(uint32_t bits, char *out, size_t len)
{
    out[0] = '\0';
    for (int i = 0; i < PPP_AP_COUNT; i++)
        if (bits & (1u << i))
            append(out, len, ppp_ap_key(i));
}

static int
method_word(const char *w, uint32_t *bits)
{
    const int ap = ppp_ap_from_key(w);

    if (ap < 0)
        return -1;
    *bits |= 1u << ap;
    return 0;
}

int
isp_methods_parse(const char *s, uint32_t *bits)
{
    return each_word(s, method_word, bits);
}

void
isp_comp_format(uint32_t bits, char *out, size_t len)
{
    out[0] = '\0';
    for (int i = 0; i < PPP_COMP_COUNT; i++)
        if (bits & (1u << i))
            append(out, len, ppp_comp_key(i));
}

static int
comp_word(const char *w, uint32_t *bits)
{
    for (int i = 0; i < PPP_COMP_COUNT; i++)
        if (!strcmp(w, ppp_comp_key(i))) {
            *bits |= 1u << i;
            return 0;
        }
    return -1;
}

int
isp_comp_parse(const char *s, uint32_t *bits)
{
    return each_word(s, comp_word, bits);
}

void
isp_strengths_format(uint32_t mppx, char *out, size_t len)
{
    out[0] = '\0';
    if (mppx & MPPX_L)
        append(out, len, "40");
    if (mppx & MPPX_M)
        append(out, len, "56");
    if (mppx & MPPX_S)
        append(out, len, "128");
}

static int
strength_word(const char *w, uint32_t *bits)
{
    if (!strcmp(w, "40"))
        *bits |= MPPX_L;
    else if (!strcmp(w, "56"))
        *bits |= MPPX_M;
    else if (!strcmp(w, "128"))
        *bits |= MPPX_S;
    else
        return -1;
    return 0;
}

int
isp_strengths_parse(const char *s, uint32_t *mppx)
{
    return each_word(s, strength_word, mppx);
}

int
isp_ip_parse(const char *s, uint32_t *ip)
{
    unsigned a, b, c, d;
    char     extra;

    while (*s == ' ')
        s++;
    if (*s == '\0') {
        *ip = 0;
        return 0;
    }
    if ((sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) || (a > 255) || (b > 255) || (c > 255) || (d > 255))
        return -1;
    *ip = (a << 24) | (b << 16) | (c << 8) | d;
    return 0;
}

void
isp_ip_format(uint32_t ip, char *out, size_t len)
{
    if (ip == 0)
        snprintf(out, len, "%s", "");
    else
        snprintf(out, len, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
}

int
isp_wins_parse(const char *s, uint32_t wins[2])
{
    uint32_t got[2] = { 0, 0 };
    int      n      = 0;

    while (*s) {
        char         w[32];
        const size_t k = strcspn(s, " ,;\t");

        if (k == 0) {
            s++;
            continue;
        }
        if ((k >= sizeof(w)) || (n == 2))
            return -1;
        memcpy(w, s, k);
        w[k] = '\0';
        if (isp_ip_parse(w, &got[n++]) < 0)
            return -1;
        s += k;
    }
    wins[0] = got[0];
    wins[1] = got[1];
    return 0;
}

void
isp_wins_format(const uint32_t wins[2], char *out, size_t len)
{
    char a[16], b[16];

    isp_ip_format(wins[0], a, sizeof(a));
    isp_ip_format(wins[1], b, sizeof(b));
    snprintf(out, len, "%s%s%s", a, (a[0] && b[0]) ? " " : "", b);
}

void
isp_settings_describe(const isp_settings_t *st, char *out, size_t len)
{
    char methods[160];
    char comp[64];
    char strengths[16];
    char wins[40];

    isp_methods_format(st->auth_protos, methods, sizeof(methods));
    isp_comp_format(st->compression, comp, sizeof(comp));
    isp_strengths_format(st->mppe_bits, strengths, sizeof(strengths));
    isp_wins_format(st->wins, wins, sizeof(wins));
    snprintf(out, len,
             "%u.%u.0.0, up to %d calls; authentication: %s%s%s%s; MPPE %s%s%s%s; compression: %s; WINS: %s; "
             "Multilink %s; keepalive %u s; guest LAN %s; %s",
             st->base_net >> 24, (st->base_net >> 16) & 0xff, st->max_sessions,
             (st->auth == PPP_AUTH_NONE) ? "none" : (st->auth == PPP_AUTH_ANY) ? "anyone" : "accounts",
             (st->auth != PPP_AUTH_NONE) ? " (" : "", (st->auth != PPP_AUTH_NONE) ? methods : "",
             (st->auth != PPP_AUTH_NONE) ? ")" : "", isp_mppe_key(st->mppe), (st->mppe != PPP_MPPE_OFF) ? " (" : "",
             (st->mppe != PPP_MPPE_OFF) ? strengths : "",
             (st->mppe != PPP_MPPE_OFF) ? ((st->mppe_bits & MPPX_H) ? ", stateless too)" : ")") : "",
             comp[0] ? comp : "none", wins[0] ? wins : "none", st->multilink ? "on" : "off", st->echo_secs,
             st->guest_lan ? "on" : "off", st->throttle ? "held to modem speed" : "line speed");
}

void
isp_pct_encode(const char *in, char *out, size_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t            n     = 0;

    for (; *in && (n + 4 < len); in++) {
        const unsigned char c = (unsigned char) *in;

        if (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || (c == '-') ||
            (c == '.') || (c == '_') || (c == '~'))
            out[n++] = (char) c;
        else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = '\0';
}

static int
hexv(char c)
{
    if ((c >= '0') && (c <= '9'))
        return c - '0';
    if ((c >= 'a') && (c <= 'f'))
        return c - 'a' + 10;
    if ((c >= 'A') && (c <= 'F'))
        return c - 'A' + 10;
    return -1;
}

void
isp_pct_decode(const char *in, char *out, size_t len)
{
    size_t n = 0;

    for (; *in && (n + 1 < len); in++) {
        if ((in[0] == '%') && (hexv(in[1]) >= 0) && (hexv(in[2]) >= 0)) {
            out[n++] = (char) ((hexv(in[1]) << 4) | hexv(in[2]));
            in += 2;
        } else
            out[n++] = *in;
    }
    out[n] = '\0';
}
