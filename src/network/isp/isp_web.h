/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             isp-server's status and control page: what a request asks for
 *             and what goes back.  No sockets here (isp_server.c has them),
 *             so the tests can drive it directly.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef ISP_WEB_H
#define ISP_WEB_H

#include <stddef.h>

typedef struct isp_web_request {
    const char *method;      /* "GET", "POST"                         */
    const char *path;        /* "/api/status", query string removed   */
    const char *host;        /* the Host header, "" if none           */
    int         from_page;   /* the X-ISP-Request header was present  */
    const char *body;        /* form-encoded, "" if none              */
    int         http_port;   /* the port the page is served on        */
} isp_web_request_t;

typedef struct isp_web_response {
    int         status;      /* 200, 400, 403, 404                    */
    const char *type;        /* the Content-Type                      */
    char       *body;        /* malloc()ed; the caller frees it       */
    size_t      len;
} isp_web_response_t;

extern void isp_web_handle(const isp_web_request_t *req, isp_web_response_t *resp);

/* What the server provides the page with (isp_server.c; the tests have
   their own). */
extern void isp_srv_get_forwards(int number, char *spec, size_t len); /* "" if none */
extern int  isp_srv_set_forwards(int number, const char *spec);       /* saves, applies to a call up */
extern void isp_srv_settings_changed(void);                           /* saves */
extern void isp_srv_endpoint(char *listen, size_t len, int *port);    /* where modems dial */

#endif /* ISP_WEB_H */
