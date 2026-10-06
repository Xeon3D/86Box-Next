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
#include <stdint.h>

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

/* The telephone exchange, as the page shows it. */
enum {
    ISP_LINE_IDLE = 0,
    ISP_LINE_RINGING,
    ISP_LINE_BUSY      /* in a call: to another modem, or to the ISP */
};

typedef struct isp_srv_line {
    char     number[24];
    char     label[64];
    int      state;      /* ISP_LINE_* */
    uint32_t seconds;    /* plugged in this long */
} isp_srv_line_t;

enum {
    ISP_PCALL_RINGING = 0,
    ISP_PCALL_ACTIVE
};

/* A call from one modem to another. */
typedef struct isp_srv_pcall {
    int      id;
    char     from[24];
    char     from_label[64];
    char     to[24];
    char     to_label[64];
    int      state;      /* ISP_PCALL_* */
    uint32_t seconds;
    uint64_t from_bytes; /* caller to callee */
    uint64_t to_bytes;
} isp_srv_pcall_t;

/* What the server provides the page with (isp_srv.c; the tests have their
   own). */
extern void isp_srv_get_forwards(int number, char *spec, size_t len); /* "" if none */
extern int  isp_srv_set_forwards(int number, const char *spec);       /* saves, applies to a call up */
extern void isp_srv_settings_changed(void);                           /* saves */
extern void isp_srv_endpoint(char *listen, size_t len, int *port);    /* where modems dial */
extern int  isp_srv_lines(isp_srv_line_t *out, int max);
extern int  isp_srv_phone_calls(isp_srv_pcall_t *out, int max);
extern int  isp_srv_hangup_phone(int id);                             /* 0: no such call */
extern void isp_srv_get_phone(int *unknown_to_isp, char *isp_numbers, size_t len);
extern void isp_srv_set_phone(int unknown_to_isp, const char *isp_numbers); /* saves */

#endif /* ISP_WEB_H */
