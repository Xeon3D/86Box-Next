/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The status page's phone: a WebSocket from the page, which is
 *             a telephone on isp-server's exchange -- the browser's
 *             microphone and speakers, a voice end like a modem's handset.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef ISP_PHONE_H
#define ISP_PHONE_H

#include <stdint.h>

/* The number the page's phone asks the exchange for. */
#define ISP_PHONE_NUMBER "555-0100"

/* A status page connection whose request (head: its headers, NUL-ended)
   asks for GET /api/phone as a WebSocket: if the Host and Origin are the
   page's own, the socket becomes a phone of its own thread, registered on
   the exchange at exchange_port, and this returns 1 -- the socket is the
   phone's.  0: not such a request, or refused (an error has been sent);
   the caller closes the socket. */
extern int  isp_phone_accept(uintptr_t sock, const char *head, int http_port, int exchange_port);
/* Every phone hangs up and its thread ends (isp_srv_stop). */
extern void isp_phone_stop_all(void);

#endif /* ISP_PHONE_H */
