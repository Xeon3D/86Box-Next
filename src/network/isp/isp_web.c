/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             isp-server's status and control page.  See isp_web.h.
 *
 *               GET  /               the page (isp_web_page.html)
 *               GET  /api/status     settings, calls, forwards, the
 *                                    exchange's lines and calls, as JSON
 *               POST /api/hangup     n=<ISP call> or call=<modem-to-modem call>
 *               POST /api/forwards   n=<call>&spec=tcp:2121:21 udp:*:5000:5000
 *               POST /api/settings   lan, throttle, rate, pap, echo, net, max
 *               POST /api/phone      unknown_to_isp, isp_numbers
 *
 *             The page controls who gets on the Internet through this
 *             machine, so it answers only a browser that asked for this host
 *             by its loopback name (no DNS rebinding), and changes only come
 *             from a request carrying X-ISP-Request, which another site's
 *             page cannot send here without a CORS preflight this never
 *             grants.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "isp.h"
#include "isp_web.h"
#include "isp_web_page.h" /* generated: isp_web_page[] */

#define WEB_MAX_CALLS 64

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} sbuf_t;

static void
sb_add(sbuf_t *b, const char *s, size_t n)
{
    if ((b->len + n + 1) > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        char  *p;

        while (cap < (b->len + n + 1))
            cap *= 2;
        if ((p = (char *) realloc(b->buf, cap)) == NULL)
            return;
        b->buf = p;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void
sb_printf(sbuf_t *b, const char *fmt, ...)
{
    char    tmp[512];
    int     n;
    va_list ap;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0)
        sb_add(b, tmp, ((size_t) n < sizeof(tmp)) ? (size_t) n : sizeof(tmp) - 1);
}

/* A JSON string, quotes included. */
static void
sb_json(sbuf_t *b, const char *s)
{
    sb_add(b, "\"", 1);
    for (; *s; s++) {
        const unsigned char c = (unsigned char) *s;

        if ((c == '"') || (c == '\\'))
            sb_printf(b, "\\%c", c);
        else if (c < 0x20)
            sb_printf(b, "\\u%04x", c);
        else
            sb_add(b, (const char *) &c, 1);
    }
    sb_add(b, "\"", 1);
}

static const char *
ip_str(uint32_t ip, char *buf)
{
    snprintf(buf, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
    return buf;
}

static const char *
forward_state_name(int st)
{
    switch (st) {
        case ISP_FORWARD_BOUND:
            return "bound";
        case ISP_FORWARD_FAILED:
            return "failed";
        default:
            return "pending";
    }
}

/* ------------------------------------------------------------- the form */

static int
hexval(char c)
{
    if ((c >= '0') && (c <= '9'))
        return c - '0';
    if ((c >= 'a') && (c <= 'f'))
        return c - 'a' + 10;
    if ((c >= 'A') && (c <= 'F'))
        return c - 'A' + 10;
    return -1;
}

/* The value of `key` in a form-encoded body, decoded; 0 if absent. */
static int
form_get(const char *body, const char *key, char *out, size_t len)
{
    const size_t klen = strlen(key);
    const char  *p    = body;

    while ((p != NULL) && (*p != '\0')) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : (p + strlen(p));

        if (((size_t) (end - p) > klen) && !strncmp(p, key, klen) && (p[klen] == '=')) {
            size_t n = 0;

            for (const char *q = p + klen + 1; (q < end) && (n < (len - 1)); q++) {
                if (*q == '+')
                    out[n++] = ' ';
                else if ((*q == '%') && ((end - q) >= 3) && (hexval(q[1]) >= 0) && (hexval(q[2]) >= 0)) {
                    out[n++] = (char) ((hexval(q[1]) << 4) | hexval(q[2]));
                    q += 2;
                } else
                    out[n++] = *q;
            }
            out[n] = '\0';
            return 1;
        }
        if (((size_t) (end - p) == klen) && !strncmp(p, key, klen)) {
            out[0] = '\0';
            return 1;
        }
        p = amp ? (amp + 1) : NULL;
    }
    return 0;
}

static int
form_int(const char *body, const char *key, long lo, long hi, long *val)
{
    char  buf[32];
    char *end;
    long  v;

    if (!form_get(body, key, buf, sizeof(buf)))
        return 0;
    v = strtol(buf, &end, 10);
    if ((end == buf) || (*end != '\0') || (v < lo) || (v > hi))
        return -1;
    *val = v;
    return 1;
}

/* ------------------------------------------------------------ responses */

static void
reply(isp_web_response_t *resp, int status, const char *type, sbuf_t *b)
{
    resp->status = status;
    resp->type   = type;
    resp->body   = b->buf ? b->buf : (char *) calloc(1, 1);
    resp->len    = b->len;
}

static void
reply_json(isp_web_response_t *resp, int status, const char *fmt, ...)
{
    sbuf_t  b = { 0 };
    char    tmp[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_add(&b, tmp, strlen(tmp));
    reply(resp, status, "application/json", &b);
}

static void
reply_error(isp_web_response_t *resp, int status, const char *msg)
{
    sbuf_t b = { 0 };

    sb_add(&b, "{\"error\":", 9);
    sb_json(&b, msg);
    sb_add(&b, "}", 1);
    reply(resp, status, "application/json", &b);
}

/* --------------------------------------------------------------- routes */

static void
api_status(isp_web_response_t *resp)
{
    static isp_call_info_t calls[WEB_MAX_CALLS];
    isp_settings_t         st;
    sbuf_t                 b = { 0 };
    char                   listen[64];
    char                   ip[16];
    int                    port = 0;
    int                    n;
    int                    first;

    isp_get_settings(&st);
    isp_srv_endpoint(listen, sizeof(listen), &port);
    n = isp_list_calls(calls, WEB_MAX_CALLS);

    sb_add(&b, "{\"server\":{\"listen\":", 20);
    sb_json(&b, listen);
    sb_printf(&b, ",\"port\":%d},", port);
    sb_printf(&b, "\"settings\":{\"net\":\"%s\",\"max\":%d,\"pap\":%d,\"echo\":%u,\"lan\":%d,\"throttle\":%d,\"rate\":%u},",
              ip_str(st.base_net, ip), st.max_sessions, st.require_pap, st.echo_secs, st.guest_lan, st.throttle,
              st.default_rate);

    sb_add(&b, "\"calls\":[", 9);
    for (int i = 0; i < n; i++) {
        const isp_call_info_t *c = &calls[i];

        sb_printf(&b, "%s{\"number\":%d,\"label\":", i ? "," : "", c->number);
        sb_json(&b, c->label);
        sb_printf(&b, ",\"ip\":\"%s\"", ip_str(c->guest_ip, ip));
        sb_printf(&b, ",\"dns\":\"%s\",\"state\":", ip_str(c->dns_ip, ip));
        sb_json(&b, isp_call_state_name(c->state));
        sb_printf(&b, ",\"state_id\":%d,\"user\":", c->state);
        sb_json(&b, c->user);
        sb_printf(&b, ",\"seconds\":%u,\"from_guest\":%llu,\"to_guest\":%llu,\"bad\":%u,\"dropped\":%u,\"rate\":%u,\"forwards\":[",
                  c->seconds, (unsigned long long) c->bytes_from_guest, (unsigned long long) c->bytes_to_guest,
                  c->bad_frames, c->dropped, c->rate);
        for (int f = 0; f < c->n_forwards; f++) {
            char spec[48];

            isp_forwards_format(&c->forwards[f], 1, spec, sizeof(spec));
            sb_printf(&b, "%s{\"spec\":\"%s\",\"state\":\"%s\"}", f ? "," : "", spec,
                      forward_state_name(c->forward_state[f]));
        }
        sb_add(&b, "]}", 2);
    }
    sb_add(&b, "],\"forwards\":[", 14);
    first = 1;
    for (int num = 1; num <= ISP_MAX_SESSIONS; num++) {
        char spec[512];

        isp_srv_get_forwards(num, spec, sizeof(spec));
        if (spec[0] == '\0')
            continue;
        sb_printf(&b, "%s{\"number\":%d,\"spec\":", first ? "" : ",", num);
        sb_json(&b, spec);
        sb_add(&b, "}", 1);
        first = 0;
    }

    /* The telephone exchange. */
    {
        static isp_srv_line_t  lines[WEB_MAX_CALLS];
        static isp_srv_pcall_t pcalls[WEB_MAX_CALLS];
        static const char     *line_state[] = { "idle", "ringing", "in a call" };
        char                   numbers[128];
        int                    unknown;
        const int              nl = isp_srv_lines(lines, WEB_MAX_CALLS);
        const int              np = isp_srv_phone_calls(pcalls, WEB_MAX_CALLS);

        isp_srv_get_phone(&unknown, numbers, sizeof(numbers));
        sb_printf(&b, "],\"phone\":{\"unknown_to_isp\":%d,\"isp_numbers\":", unknown);
        sb_json(&b, numbers);
        sb_add(&b, "},\"lines\":[", 11);
        for (int i = 0; i < nl; i++) {
            const char *open = i ? ",{\"number\":" : "{\"number\":";

            sb_add(&b, open, strlen(open));
            sb_json(&b, lines[i].number);
            sb_add(&b, ",\"label\":", 9);
            sb_json(&b, lines[i].label);
            sb_printf(&b, ",\"state\":\"%s\",\"seconds\":%u}", line_state[lines[i].state % 3], lines[i].seconds);
        }
        sb_add(&b, "],\"phone_calls\":[", 17);
        for (int i = 0; i < np; i++) {
            const isp_srv_pcall_t *p = &pcalls[i];

            sb_printf(&b, "%s{\"id\":%d,\"from\":", i ? "," : "", p->id);
            sb_json(&b, p->from);
            sb_add(&b, ",\"from_label\":", 14);
            sb_json(&b, p->from_label);
            sb_add(&b, ",\"to\":", 6);
            sb_json(&b, p->to);
            sb_add(&b, ",\"to_label\":", 12);
            sb_json(&b, p->to_label);
            sb_printf(&b, ",\"state\":\"%s\",\"seconds\":%u,\"from_bytes\":%llu,\"to_bytes\":%llu}",
                      (p->state == ISP_PCALL_ACTIVE) ? "connected" : "ringing", p->seconds,
                      (unsigned long long) p->from_bytes, (unsigned long long) p->to_bytes);
        }
    }
    sb_add(&b, "]}", 2);
    reply(resp, 200, "application/json", &b);
}

/* n=<ISP call number>, or call=<modem-to-modem call id>. */
static void
api_hangup(const char *body, isp_web_response_t *resp)
{
    long n;

    if (form_int(body, "call", 1, 0x7fffffff, &n) == 1) {
        if (!isp_srv_hangup_phone((int) n)) {
            reply_error(resp, 404, "there is no such call");
            return;
        }
        reply_json(resp, 200, "{\"ok\":true}");
        return;
    }
    if (form_int(body, "n", 1, ISP_MAX_SESSIONS, &n) != 1) {
        reply_error(resp, 400, "which call?");
        return;
    }
    if (!isp_hangup_call((int) n)) {
        reply_error(resp, 404, "there is no such call");
        return;
    }
    reply_json(resp, 200, "{\"ok\":true}");
}

/* The exchange: unknown_to_isp=0|1, isp_numbers=<numbers the ISP answers>. */
static void
api_phone(const char *body, isp_web_response_t *resp)
{
    char numbers[128];
    char cur[128];
    int  unknown;
    long v;
    int  r;

    isp_srv_get_phone(&unknown, cur, sizeof(cur));
    if ((r = form_int(body, "unknown_to_isp", 0, 1, &v)) < 0) {
        reply_error(resp, 400, "unknown_to_isp is 0 or 1");
        return;
    } else if (r)
        unknown = (int) v;
    if (!form_get(body, "isp_numbers", numbers, sizeof(numbers)))
        snprintf(numbers, sizeof(numbers), "%s", cur);
    if (strspn(numbers, "0123456789-*#,; ()+") != strlen(numbers)) {
        reply_error(resp, 400, "ISP numbers are digits, separated by commas or spaces");
        return;
    }
    isp_srv_set_phone(unknown, numbers);
    reply_json(resp, 200, "{\"ok\":true}");
}

static void
api_forwards(const char *body, isp_web_response_t *resp)
{
    isp_settings_t st;
    isp_forward_t  fwd[ISP_MAX_FORWARDS];
    char           spec[512];
    char           norm[512];
    long           n;
    int            items = 0;
    int            parsed;

    isp_get_settings(&st);
    if (form_int(body, "n", 1, st.max_sessions, &n) != 1) {
        reply_error(resp, 400, "call numbers run from 1 to the most calls allowed");
        return;
    }
    if (!form_get(body, "spec", spec, sizeof(spec)))
        spec[0] = '\0';

    /* Every item must parse: a typo is refused, not quietly dropped. */
    for (const char *p = spec; *p;) {
        const size_t len = strcspn(p, " ,;\t");

        if (len > 0)
            items++;
        p += len;
        if (*p)
            p++;
    }
    parsed = isp_forwards_parse(spec, fwd, ISP_MAX_FORWARDS);
    if (parsed != items) {
        reply_error(resp, 400, (items > ISP_MAX_FORWARDS) ? "too many forwards for one call"
                                                          : "a forward is protocol:host port:guest port, e.g. tcp:2121:21");
        return;
    }
    isp_forwards_format(fwd, parsed, norm, sizeof(norm));
    if (!isp_srv_set_forwards((int) n, norm)) {
        reply_error(resp, 500, "the forwards could not be saved");
        return;
    }
    reply_json(resp, 200, "{\"ok\":true}");
}

static void
api_settings(const char *body, isp_web_response_t *resp)
{
    isp_settings_t st;
    char           net[32];
    long           v;
    int            r;

    isp_get_settings(&st);
#define FLAG(key, field)                                                \
    if ((r = form_int(body, key, 0, 1, &v)) < 0) {                       \
        reply_error(resp, 400, key " is 0 or 1");                        \
        return;                                                           \
    } else if (r)                                                         \
        st.field = (int) v;
    FLAG("lan", guest_lan)
    FLAG("throttle", throttle)
    FLAG("pap", require_pap)
#undef FLAG
    if ((r = form_int(body, "rate", 300, 10000000, &v)) < 0) {
        reply_error(resp, 400, "the speed is 300 bit/s or more");
        return;
    } else if (r)
        st.default_rate = (uint32_t) v;
    if ((r = form_int(body, "echo", 0, 3600, &v)) < 0) {
        reply_error(resp, 400, "the keepalive is 0 to 3600 seconds");
        return;
    } else if (r)
        st.echo_secs = (uint32_t) v;
    if ((r = form_int(body, "max", 1, WEB_MAX_CALLS, &v)) < 0) {
        reply_error(resp, 400, "at most 64 calls");
        return;
    } else if (r)
        st.max_sessions = (int) v;
    if (form_get(body, "net", net, sizeof(net))) {
        unsigned a, b, c, d;
        char     extra;

        if ((sscanf(net, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) || (a > 255) || (b > 255) || c || d) {
            reply_error(resp, 400, "the address range is a /16: A.B.0.0");
            return;
        }
        st.base_net = (a << 24) | (b << 16);
    }
    isp_set_settings(&st);
    isp_srv_settings_changed();
    reply_json(resp, 200, "{\"ok\":true}");
}

/* -------------------------------------------------------------- the API */

static int
host_ok(const isp_web_request_t *req)
{
    char want[3][40];

    snprintf(want[0], sizeof(want[0]), "127.0.0.1:%d", req->http_port);
    snprintf(want[1], sizeof(want[1]), "localhost:%d", req->http_port);
    snprintf(want[2], sizeof(want[2]), "[::1]:%d", req->http_port);
    for (int i = 0; i < 3; i++)
        if (!strcmp(req->host, want[i]))
            return 1;
    return 0;
}

void
isp_web_handle(const isp_web_request_t *req, isp_web_response_t *resp)
{
    const char *body = req->body ? req->body : "";

    memset(resp, 0, sizeof(*resp));
    if (!host_ok(req)) {
        reply_error(resp, 403, "ask for this page as 127.0.0.1 or localhost");
        return;
    }

    if (!strcmp(req->method, "GET")) {
        if (!strcmp(req->path, "/") || !strcmp(req->path, "/index.html")) {
            sbuf_t b = { 0 };

            sb_add(&b, isp_web_page, strlen(isp_web_page));
            reply(resp, 200, "text/html; charset=utf-8", &b);
        } else if (!strcmp(req->path, "/api/status"))
            api_status(resp);
        else
            reply_error(resp, 404, "no such page");
        return;
    }

    if (!strcmp(req->method, "POST")) {
        if (!req->from_page)
            reply_error(resp, 403, "changes come from the ISP's own page");
        else if (!strcmp(req->path, "/api/hangup"))
            api_hangup(body, resp);
        else if (!strcmp(req->path, "/api/forwards"))
            api_forwards(body, resp);
        else if (!strcmp(req->path, "/api/settings"))
            api_settings(body, resp);
        else if (!strcmp(req->path, "/api/phone"))
            api_phone(body, resp);
        else
            reply_error(resp, 404, "no such action");
        return;
    }

    reply_error(resp, 405, "GET or POST");
}
