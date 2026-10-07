/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             isp-server's status and control page: what a request asks for
 *             and what goes back.  No sockets here (isp_srv.c has them),
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
    const char *method;      /* "GET", "POST"                                */
    const char *path;        /* "/api/status", query string removed          */
    const char *host;        /* the Host header, "" if none                  */
    int         from_page;   /* the X-ISP-Request header was present         */
    const char *body;        /* form-encoded, "" if none                     */
    int         http_port;   /* the port the page is served on               */
    const char *cookie;      /* the Cookie header, NULL or "" if none        */
    int         loopback;    /* it came from this machine                    */
    const char *query;       /* after the '?', NULL or "" if none            */
    int         https;       /* a proxy in front says the browser used HTTPS */
} isp_web_request_t;

typedef struct isp_web_response {
    int         status;      /* 200, 400, 403, 404, 409, 429                 */
    const char *type;        /* the Content-Type                             */
    char       *body;        /* malloc()ed; the caller frees it              */
    size_t      len;
    char        set_cookie[200]; /* a Set-Cookie header's value, "" if none  */
} isp_web_response_t;

extern void isp_web_handle(const isp_web_request_t *req, isp_web_response_t *resp);

/* The page's phone (a WebSocket upgrade, which isp_phone.c takes over):
   1 if this request may have it -- an admin's, from the page's own
   Origin. */
extern int isp_web_phone_allowed(const isp_web_request_t *req, const char *origin);

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
    int      voice;      /* ISP_PCALL_FROM_VOICE | ISP_PCALL_TO_VOICE */
} isp_srv_pcall_t;

#define ISP_PCALL_FROM_VOICE 1 /* the caller dialled a voice call      */
#define ISP_PCALL_TO_VOICE   2 /* the callee picked up as a voice call */

/* One line of the log, as the page's Log tab shows it. */
typedef struct isp_log_line {
    uint32_t n;          /* from 1 up */
    char     time[20];   /* "2026-10-07 18:03:12" */
    char     text[600];
} isp_log_line_t;

/* What the server provides the page with (isp_srv.c; the tests have their
   own). */
extern void isp_srv_get_forwards(int number, char *spec, size_t len); /* "" if none */
extern int  isp_srv_set_forwards(int number, const char *spec);       /* saves, applies to a call up */
extern void isp_srv_settings_changed(void);                           /* saves */
extern void isp_srv_config_changed(const char *what);                 /* logs it, saves */
extern void isp_srv_endpoint(char *listen, size_t len, int *port);    /* where modems dial */
extern int  isp_srv_lines(isp_srv_line_t *out, int max);
extern int  isp_srv_phone_calls(isp_srv_pcall_t *out, int max);
extern int  isp_srv_hangup_phone(int id);                             /* 0: no such call */
extern void isp_srv_get_phone(int *unknown_to_isp, char *isp_numbers, size_t len);
extern void isp_srv_set_phone(int unknown_to_isp, const char *isp_numbers); /* saves */
/* The log's lines after number `after`, oldest first; returns how many. */
extern int  isp_srv_log_lines(uint32_t after, isp_log_line_t *out, int max);

#endif /* ISP_WEB_H */
