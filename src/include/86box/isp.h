/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The virtual ISP (src/network/isp/): what a modem dials when
 *             its line is "Internet".  One session is one call: PPP on a
 *             byte stream, a private /24, and a user-mode NAT (libslirp) out
 *             to the host's Internet.  Used by the COM port modems
 *             (char_modem.c) and by isp-server.exe, which puts the same
 *             thing behind a TCP port.
 *
 *             Each session runs on a thread of its own; these calls only
 *             move bytes through locked queues and never wait on the
 *             network.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef EMU_ISP_H
#define EMU_ISP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ISP_MAX_SESSIONS 254

typedef struct isp_session isp_session_t;

typedef struct isp_settings {
    uint32_t base_net;     /* host order; session n gets base_net + n * 256, a /24 (default 10.86.0.0) */
    int      max_sessions; /* 1 to ISP_MAX_SESSIONS                                                   */
    int      require_pap;  /* ask the guest to authenticate (any name and password will do)           */
    uint32_t echo_secs;    /* LCP keepalive interval; 0 sends none                                    */
} isp_settings_t;

/* Process-wide; a session takes them when it opens. */
extern void isp_get_settings(isp_settings_t *s);
extern void isp_set_settings(const isp_settings_t *s);

typedef struct isp_session_callbacks {
    /* From the session's thread: output is waiting, or the call has ended.
       Optional, for a frontend that would rather wait than poll. */
    void (*notify)(void *opaque);
    /* Progress, one line at a time: LCP and IPCP, the guest's address,
       failures.  From the session's thread or the caller's.  Optional. */
    void (*log)(void *opaque, const char *msg);
    void *opaque;
} isp_session_callbacks_t;

/* A call comes in.  PPP waits for the guest to start it, so this can be
   opened while the modem is still dialling.  NULL, with the reason in `err`,
   when every address is taken or the thread will not start. */
extern isp_session_t *isp_session_open(const isp_session_callbacks_t *cb, char *err, size_t err_len);
/* The call is over: PPP, NAT, address and queues go, all of them. */
extern void isp_session_close(isp_session_t *s);

/* Bytes from the guest's modem; returns how many were taken (fewer when the
   queue is full: hold the rest back). */
extern size_t isp_session_write(isp_session_t *s, const uint8_t *buf, size_t len);
/* Bytes for the guest's modem; returns how many. */
extern size_t isp_session_read(isp_session_t *s, uint8_t *buf, size_t len);
/* 1 once PPP has finished and its last byte has been read: hang up. */
extern int isp_session_ended(isp_session_t *s);

extern int      isp_session_number(const isp_session_t *s);
extern uint32_t isp_session_guest_ip(const isp_session_t *s); /* host order */

#ifdef __cplusplus
}
#endif

#endif /* EMU_ISP_H */
