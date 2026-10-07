/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             isp-server's settings as words: what isp-server.ini, the
 *             command line and the status page say for the authentication,
 *             encryption and compression choices, and back.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef ISP_CONFIG_H
#define ISP_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include "isp.h"

extern const char *isp_auth_key(int auth);         /* "none", "any", "accounts" */
extern int         isp_auth_parse(const char *s);  /* -1 if none of those */
extern const char *isp_mppe_key(int mppe);         /* "off", "allowed", "required" */
extern int         isp_mppe_parse(const char *s);

/* Space-separated lists of ppp_ap_key() / ppp_comp_key() / "40 56 128";
   parsing returns -1 (and sets nothing) on a word it does not know. */
extern void isp_methods_format(uint32_t bits, char *out, size_t len);
extern int  isp_methods_parse(const char *s, uint32_t *bits);
extern void isp_comp_format(uint32_t bits, char *out, size_t len);
extern int  isp_comp_parse(const char *s, uint32_t *bits);
extern void isp_strengths_format(uint32_t mppx, char *out, size_t len);
extern int  isp_strengths_parse(const char *s, uint32_t *mppx); /* MPPX_L/M/S */

/* "a.b.c.d" (host order); 0.0.0.0 and "" are 0.  -1 if not an address. */
extern int  isp_ip_parse(const char *s, uint32_t *ip);
extern void isp_ip_format(uint32_t ip, char *out, size_t len); /* "" for 0 */
/* "a.b.c.d a.b.c.d": the WINS pair. */
extern int  isp_wins_parse(const char *s, uint32_t wins[2]);
extern void isp_wins_format(const uint32_t wins[2], char *out, size_t len);

/* One line for the log. */
extern void isp_settings_describe(const isp_settings_t *st, char *out, size_t len);

/* %XX for anything but letters, digits and -._~ (names and passwords in
   the .ini); and back. */
extern void isp_pct_encode(const char *in, char *out, size_t len);
extern void isp_pct_decode(const char *in, char *out, size_t len);

#endif /* ISP_CONFIG_H */
