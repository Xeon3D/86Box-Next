/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             isp-server's status and control page.  See isp_web.h.
 *
 *               GET  /               the page (isp_web_page.html)
 *               GET  /api/session    who is asking, and whether there are
 *                                    users yet
 *               POST /api/login      name, password (sets the cookie)
 *               POST /api/logout
 *               POST /api/setup      name, password[, token]: the first
 *                                    user, the super admin
 *               GET  /api/status     settings, calls, forwards, the
 *                                    exchange's lines and calls, as JSON
 *               GET  /api/log        ?after=N: the log's newer lines
 *               GET  /api/accounts   the accounts guests dial in with
 *               POST /api/accounts   op=add|delete|password, user, password
 *               GET  /api/users      the page's users (super admin)
 *               POST /api/users      op=add|delete|role|password, name,
 *                                    password, role (super admin)
 *               POST /api/password   current, password: one's own
 *               POST /api/hangup     n=<ISP call> or call=<modem-to-modem call>
 *               POST /api/forwards   n=<call>&spec=tcp:2121:21 udp:*:5000:5000
 *               POST /api/settings   lan, throttle, rate, echo, net, max,
 *                                    auth, methods, mppe, strengths,
 *                                    stateless, compression, wins, multilink
 *               POST /api/phone      unknown_to_isp, isp_numbers
 *
 *             Who may do what: viewers look (status, log); admins change
 *             (calls, forwards, settings, accounts, the exchange, the
 *             phone); the super admin also keeps the users.  With no users
 *             at all, whoever is on this machine may do everything, as long
 *             as the browser asked for the page by a loopback name (no DNS
 *             rebinding); anyone else only sees the setup form, which wants
 *             the token isp-server put in its log.  Changes only come from a
 *             request carrying X-ISP-Request, which another site's page
 *             cannot send here without a CORS preflight this never grants,
 *             and the login cookie is SameSite=Strict besides.
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
#include "isp_config.h"
#include "isp_users.h"
#include "isp_web.h"
#include "isp_web_page.h" /* generated: isp_web_page[] */

#define WEB_MAX_CALLS 64
#define LOG_BATCH     500
#define COOKIE_NAME   "isp_session"

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} sbuf_t;

/* Who is asking. */
typedef struct {
    int  role; /* ISP_ROLE_* */
    char user[64];
    char token[ISP_TOKEN_CHARS + 1];
} who_t;

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
sb_str(sbuf_t *b, const char *s)
{
    sb_add(b, s, strlen(s));
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

/* "key": "value", JSON. */
static void
sb_kv(sbuf_t *b, const char *key, const char *val, int comma)
{
    sb_printf(b, "%s\"%s\":", comma ? "," : "", key);
    sb_json(b, val);
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

/* The value of `key` in a form-encoded body (or query string), decoded; 0
   if absent. */
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
reply_ok(isp_web_response_t *resp)
{
    reply_json(resp, 200, "{\"ok\":true}");
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

/* ------------------------------------------------------------ who is it */

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

/* The login cookie's value, or "". */
static void
cookie_token(const isp_web_request_t *req, char *out, size_t len)
{
    const char  *c    = req->cookie;
    const size_t klen = strlen(COOKIE_NAME);

    out[0] = '\0';
    while ((c != NULL) && *c) {
        while ((*c == ' ') || (*c == ';'))
            c++;
        if (!strncmp(c, COOKIE_NAME, klen) && (c[klen] == '=')) {
            const char  *v = c + klen + 1;
            const size_t n = strcspn(v, ";");

            if (n < len) {
                memcpy(out, v, n);
                out[n] = '\0';
            }
            return;
        }
        c = strchr(c, ';');
    }
}

static void
whois(const isp_web_request_t *req, who_t *w)
{
    memset(w, 0, sizeof(*w));
    w->role = ISP_ROLE_NONE;
    if (isp_users_count() == 0) {
        /* Nobody to log in as: this machine's browser may do everything. */
        if (req->loopback && host_ok(req)) {
            w->role = ISP_ROLE_LOCAL;
            snprintf(w->user, sizeof(w->user), "%s", "");
        }
        return;
    }
    cookie_token(req, w->token, sizeof(w->token));
    if (!isp_login_check(w->token, w->user, sizeof(w->user), &w->role))
        w->role = ISP_ROLE_NONE;
}

static int
may_view(const who_t *w)
{
    return w->role >= ISP_ROLE_VIEWER;
}

static int
may_change(const who_t *w)
{
    return (w->role == ISP_ROLE_ADMIN) || (w->role == ISP_ROLE_SUPER) || (w->role == ISP_ROLE_LOCAL);
}

static int
may_keep_users(const who_t *w)
{
    return (w->role == ISP_ROLE_SUPER) || (w->role == ISP_ROLE_LOCAL);
}

static void
set_cookie(const isp_web_request_t *req, isp_web_response_t *resp, const char *token)
{
    if (token == NULL)
        snprintf(resp->set_cookie, sizeof(resp->set_cookie), "%s=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0%s",
                 COOKIE_NAME, req->https ? "; Secure" : "");
    else
        snprintf(resp->set_cookie, sizeof(resp->set_cookie),
                 "%s=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=604800%s", COOKIE_NAME, token,
                 req->https ? "; Secure" : "");
}

int
isp_web_phone_allowed(const isp_web_request_t *req, const char *origin)
{
    who_t w;
    char  own[2][160];

    whois(req, &w);
    if (!may_change(&w))
        return 0;
    /* The page's own Origin: not another site's. */
    snprintf(own[0], sizeof(own[0]), "http://%s", req->host);
    snprintf(own[1], sizeof(own[1]), "https://%s", req->host);
    return !strcmp(origin, own[0]) || !strcmp(origin, own[1]);
}

/* ---------------------------------------------------------- the session */

static void
api_session(const isp_web_request_t *req, const who_t *w, isp_web_response_t *resp)
{
    sbuf_t    b     = { 0 };
    const int setup = (isp_users_count() == 0);

    sb_printf(&b, "{\"setup\":%s,\"local\":%s,\"role\":\"%s\"", setup ? "true" : "false",
              (req->loopback && host_ok(req)) ? "true" : "false", isp_role_key(w->role));
    sb_kv(&b, "user", w->user, 1);
    sb_add(&b, "}", 1);
    reply(resp, 200, "application/json", &b);
}

static void
api_login(const isp_web_request_t *req, isp_web_response_t *resp)
{
    char name[64], password[256];
    char token[ISP_TOKEN_CHARS + 1];
    int  role;

    if (isp_login_throttled()) {
        reply_error(resp, 429, "too many wrong passwords: wait half a minute");
        return;
    }
    if (!form_get(req->body, "name", name, sizeof(name)) || !form_get(req->body, "password", password, sizeof(password))) {
        reply_error(resp, 400, "a name and a password, please");
        return;
    }
    if (!isp_users_verify(name, password, &role)) {
        isp_login_failed();
        memset(password, 0, sizeof(password));
        reply_error(resp, 403, "wrong name or password");
        return;
    }
    memset(password, 0, sizeof(password));
    if (!isp_login_start(name, token)) {
        reply_error(resp, 500, "could not log in");
        return;
    }
    set_cookie(req, resp, token);
    memset(token, 0, sizeof(token));
    reply_ok(resp);
}

static void
api_logout(const isp_web_request_t *req, const who_t *w, isp_web_response_t *resp)
{
    isp_login_end(w->token);
    set_cookie(req, resp, NULL);
    reply_ok(resp);
}

static void
api_setup(const isp_web_request_t *req, isp_web_response_t *resp)
{
    char        name[64], password[256], token[64];
    char        err[128];
    char        login[ISP_TOKEN_CHARS + 1];
    const char *want = isp_users_setup_token();

    if (isp_users_count() > 0) {
        reply_error(resp, 409, "there are users already: log in");
        return;
    }
    if (!(req->loopback && host_ok(req))) {
        /* From elsewhere: the token from the log proves it is the owner. */
        if (!form_get(req->body, "token", token, sizeof(token)) || (want[0] == '\0') || strcmp(token, want)) {
            isp_login_failed();
            reply_error(resp, 403, "the setup token is wrong (it is in isp-server's log)");
            return;
        }
    }
    if (!form_get(req->body, "name", name, sizeof(name)) || !form_get(req->body, "password", password, sizeof(password))) {
        reply_error(resp, 400, "a name and a password, please");
        return;
    }
    if (!isp_users_add(name, password, ISP_ROLE_SUPER, err, sizeof(err))) {
        memset(password, 0, sizeof(password));
        reply_error(resp, 400, err);
        return;
    }
    memset(password, 0, sizeof(password));
    isp_srv_config_changed("the first user, the super admin, was made: logins are needed from now on");
    if (isp_login_start(name, login))
        set_cookie(req, resp, login);
    memset(login, 0, sizeof(login));
    reply_ok(resp);
}

static void
api_password(const isp_web_request_t *req, const who_t *w, isp_web_response_t *resp)
{
    char current[256], password[256], err[128];
    int  role;

    if ((w->role < ISP_ROLE_VIEWER) || (w->role == ISP_ROLE_LOCAL)) {
        reply_error(resp, 403, "log in first");
        return;
    }
    if (!form_get(req->body, "current", current, sizeof(current)) ||
        !form_get(req->body, "password", password, sizeof(password))) {
        reply_error(resp, 400, "the current password and a new one, please");
        return;
    }
    if (!isp_users_verify(w->user, current, &role)) {
        isp_login_failed();
        reply_error(resp, 403, "the current password is wrong");
    } else if (!isp_users_set_password(w->user, password, err, sizeof(err)))
        reply_error(resp, 400, err);
    else {
        char token[ISP_TOKEN_CHARS + 1];

        /* Every login of this user ended; this one starts again. */
        if (isp_login_start(w->user, token))
            set_cookie(req, resp, token);
        memset(token, 0, sizeof(token));
        isp_srv_config_changed("a user changed their password");
        reply_ok(resp);
    }
    memset(current, 0, sizeof(current));
    memset(password, 0, sizeof(password));
}

static void
api_users(const isp_web_request_t *req, const who_t *w, isp_web_response_t *resp)
{
    char op[16], name[64], password[256], role[16], err[128];
    char what[160];
    int  ok;

    if (!strcmp(req->method, "GET")) {
        static isp_user_info_t list[ISP_MAX_USERS];
        sbuf_t                 b = { 0 };
        const int              n = isp_users_list(list, ISP_MAX_USERS);

        sb_str(&b, "{\"users\":[");
        for (int i = 0; i < n; i++) {
            sb_str(&b, i ? ",{" : "{");
            sb_kv(&b, "name", list[i].name, 0);
            sb_kv(&b, "role", isp_role_key(list[i].role), 1);
            sb_str(&b, "}");
        }
        sb_str(&b, "]}");
        reply(resp, 200, "application/json", &b);
        return;
    }
    if (!form_get(req->body, "op", op, sizeof(op)) || !form_get(req->body, "name", name, sizeof(name))) {
        reply_error(resp, 400, "which user, and what?");
        return;
    }
    if (!strcmp(op, "add")) {
        const int r = form_get(req->body, "role", role, sizeof(role)) ? isp_role_parse(role) : ISP_ROLE_NONE;

        if ((r != ISP_ROLE_ADMIN) && (r != ISP_ROLE_VIEWER)) {
            reply_error(resp, 400, "a new user is an admin or a viewer");
            return;
        }
        if (!form_get(req->body, "password", password, sizeof(password)))
            password[0] = '\0';
        ok = isp_users_add(name, password, r, err, sizeof(err));
        snprintf(what, sizeof(what), "user \"%s\" added, as %s", name, (r == ISP_ROLE_ADMIN) ? "an admin" : "a viewer");
    } else if (!strcmp(op, "delete")) {
        ok = !strcmp(name, w->user) ? 0 : isp_users_remove(name, err, sizeof(err));
        if (!strcmp(name, w->user))
            snprintf(err, sizeof(err), "one does not remove oneself");
        snprintf(what, sizeof(what), "user \"%s\" removed", name);
    } else if (!strcmp(op, "role")) {
        const int r = form_get(req->body, "role", role, sizeof(role)) ? isp_role_parse(role) : ISP_ROLE_NONE;

        ok = isp_users_set_role(name, r, err, sizeof(err));
        snprintf(what, sizeof(what), "user \"%s\" is now %s", name, (r == ISP_ROLE_ADMIN) ? "an admin" : "a viewer");
    } else if (!strcmp(op, "password")) {
        if (!form_get(req->body, "password", password, sizeof(password)))
            password[0] = '\0';
        ok = isp_users_set_password(name, password, err, sizeof(err));
        snprintf(what, sizeof(what), "user \"%s\" has a new password", name);
    } else {
        reply_error(resp, 400, "op is add, delete, role or password");
        return;
    }
    memset(password, 0, sizeof(password));
    if (!ok) {
        reply_error(resp, 400, err);
        return;
    }
    isp_srv_config_changed(what);
    reply_ok(resp);
}

/* ------------------------------------------------------------ accounts */

static int
account_name_ok(const char *s)
{
    const size_t n = strlen(s);

    if ((n < 1) || (n >= sizeof(((isp_account_t *) 0)->user)))
        return 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char) s[i] < 0x21) || ((unsigned char) s[i] == 0x7f))
            return 0;
    return 1;
}

static void
api_accounts(const isp_web_request_t *req, isp_web_response_t *resp)
{
    static isp_account_t list[ISP_MAX_ACCOUNTS];
    char                 op[16], user[64], password[256], what[160];
    int                  n   = isp_get_accounts(list, ISP_MAX_ACCOUNTS);
    int                  at  = -1;

    if (!strcmp(req->method, "GET")) {
        sbuf_t b = { 0 };

        /* Names only: the passwords are never shown. */
        sb_str(&b, "{\"accounts\":[");
        for (int i = 0; i < n; i++) {
            sb_str(&b, i ? ",{" : "{");
            sb_kv(&b, "user", list[i].user, 0);
            sb_str(&b, "}");
        }
        sb_str(&b, "]}");
        reply(resp, 200, "application/json", &b);
        memset(list, 0, sizeof(list));
        return;
    }
    if (!form_get(req->body, "op", op, sizeof(op)) || !form_get(req->body, "user", user, sizeof(user)) ||
        !account_name_ok(user)) {
        memset(list, 0, sizeof(list));
        reply_error(resp, 400, "a name is 1 to 63 characters, no spaces");
        return;
    }
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].user, user))
            at = i;
    if (!form_get(req->body, "password", password, sizeof(password)))
        password[0] = '\0';

    if (!strcmp(op, "delete")) {
        if (at < 0) {
            memset(list, 0, sizeof(list));
            reply_error(resp, 404, "no such account");
            return;
        }
        memmove(&list[at], &list[at + 1], (size_t) (n - at - 1) * sizeof(list[0]));
        n--;
        snprintf(what, sizeof(what), "account \"%s\" removed", user);
    } else if (!strcmp(op, "add") || !strcmp(op, "password")) {
        if ((password[0] == '\0') || (strlen(password) >= sizeof(list[0].password))) {
            memset(list, 0, sizeof(list));
            reply_error(resp, 400, "a password is 1 to 127 characters");
            return;
        }
        if (!strcmp(op, "add")) {
            if (at >= 0) {
                memset(list, 0, sizeof(list));
                reply_error(resp, 409, "there is an account of that name already");
                return;
            }
            if (n >= ISP_MAX_ACCOUNTS) {
                memset(list, 0, sizeof(list));
                reply_error(resp, 400, "too many accounts");
                return;
            }
            at = n++;
            snprintf(list[at].user, sizeof(list[at].user), "%s", user);
            snprintf(what, sizeof(what), "account \"%s\" added", user);
        } else {
            if (at < 0) {
                memset(list, 0, sizeof(list));
                reply_error(resp, 404, "no such account");
                return;
            }
            snprintf(what, sizeof(what), "account \"%s\" has a new password", user);
        }
        snprintf(list[at].password, sizeof(list[at].password), "%s", password);
    } else {
        memset(list, 0, sizeof(list));
        reply_error(resp, 400, "op is add, delete or password");
        return;
    }
    isp_set_accounts(list, n);
    memset(list, 0, sizeof(list));
    memset(password, 0, sizeof(password));
    isp_srv_config_changed(what);
    reply_ok(resp);
}

/* ------------------------------------------------------------------ log */

static void
api_log(const isp_web_request_t *req, isp_web_response_t *resp)
{
    isp_log_line_t *lines = (isp_log_line_t *) malloc(LOG_BATCH * sizeof(isp_log_line_t));
    sbuf_t          b     = { 0 };
    char            after[24];
    uint32_t        from  = 0;
    int             n;

    if (lines == NULL) {
        reply_error(resp, 500, "out of memory");
        return;
    }
    if ((req->query != NULL) && form_get(req->query, "after", after, sizeof(after)))
        from = (uint32_t) strtoul(after, NULL, 10);
    n = isp_srv_log_lines(from, lines, LOG_BATCH);
    sb_str(&b, "{\"lines\":[");
    for (int i = 0; i < n; i++) {
        sb_printf(&b, "%s{\"n\":%u", i ? "," : "", lines[i].n);
        sb_kv(&b, "t", lines[i].time, 1);
        sb_kv(&b, "text", lines[i].text, 1);
        sb_str(&b, "}");
    }
    sb_printf(&b, "],\"next\":%u}", n ? lines[n - 1].n : from);
    free(lines);
    reply(resp, 200, "application/json", &b);
}

/* --------------------------------------------------------------- status */

static void
api_status(isp_web_response_t *resp)
{
    static isp_call_info_t calls[WEB_MAX_CALLS];
    isp_settings_t         st;
    sbuf_t                 b = { 0 };
    char                   listen[64];
    char                   ip[16];
    char                   words[200];
    int                    port = 0;
    int                    n;
    int                    first;

    isp_get_settings(&st);
    isp_srv_endpoint(listen, sizeof(listen), &port);
    n = isp_list_calls(calls, WEB_MAX_CALLS);

    sb_add(&b, "{\"server\":{\"listen\":", 20);
    sb_json(&b, listen);
    sb_printf(&b, ",\"port\":%d},", port);
    sb_printf(&b, "\"settings\":{\"net\":\"%s\",\"max\":%d,\"pap\":%d,\"echo\":%u,\"lan\":%d,\"throttle\":%d,\"rate\":%u",
              ip_str(st.base_net, ip), st.max_sessions, st.auth != PPP_AUTH_NONE, st.echo_secs, st.guest_lan,
              st.throttle, st.default_rate);
    sb_kv(&b, "auth", isp_auth_key(st.auth), 1);
    isp_methods_format(st.auth_protos, words, sizeof(words));
    sb_kv(&b, "methods", words, 1);
    sb_kv(&b, "mppe", isp_mppe_key(st.mppe), 1);
    isp_strengths_format(st.mppe_bits, words, sizeof(words));
    sb_kv(&b, "strengths", words, 1);
    sb_printf(&b, ",\"stateless\":%d", !!(st.mppe_bits & MPPX_H));
    isp_comp_format(st.compression, words, sizeof(words));
    sb_kv(&b, "compression", words, 1);
    isp_wins_format(st.wins, words, sizeof(words));
    sb_kv(&b, "wins", words, 1);
    sb_printf(&b, ",\"multilink\":%d},", st.multilink);

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
        sb_kv(&b, "auth", c->auth, 1);
        sb_kv(&b, "ccp", c->ccp, 1);
        sb_printf(&b, ",\"encrypted\":%d,\"bundle\":%d,\"links\":%d", c->encrypted, c->bundle, c->links);
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
            sb_printf(&b, ",\"state\":\"%s\",\"seconds\":%u,\"from_bytes\":%llu,\"to_bytes\":%llu,"
                          "\"from_voice\":%d,\"to_voice\":%d}",
                      (p->state == ISP_PCALL_ACTIVE) ? "connected" : "ringing", p->seconds,
                      (unsigned long long) p->from_bytes, (unsigned long long) p->to_bytes,
                      !!(p->voice & ISP_PCALL_FROM_VOICE), !!(p->voice & ISP_PCALL_TO_VOICE));
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
        reply_ok(resp);
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
    reply_ok(resp);
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
    reply_ok(resp);
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
    reply_ok(resp);
}

static void
api_settings(const char *body, isp_web_response_t *resp)
{
    isp_settings_t st;
    char           word[200];
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
    FLAG("multilink", multilink)
#undef FLAG
    /* The old switch: on asks for a name and password, any will do. */
    if ((r = form_int(body, "pap", 0, 1, &v)) < 0) {
        reply_error(resp, 400, "pap is 0 or 1");
        return;
    } else if (r && !form_get(body, "auth", word, sizeof(word)))
        st.auth = v ? ((st.auth == PPP_AUTH_NONE) ? PPP_AUTH_ANY : st.auth) : PPP_AUTH_NONE;
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
    if (form_get(body, "net", word, sizeof(word))) {
        unsigned a, b, c, d;
        char     extra;

        if ((sscanf(word, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) || (a > 255) || (b > 255) || c || d) {
            reply_error(resp, 400, "the address range is a /16: A.B.0.0");
            return;
        }
        st.base_net = (a << 24) | (b << 16);
    }
    if (form_get(body, "auth", word, sizeof(word))) {
        if ((st.auth = isp_auth_parse(word)) < 0) {
            reply_error(resp, 400, "auth is none, any or accounts");
            return;
        }
    }
    if (form_get(body, "methods", word, sizeof(word)) && (isp_methods_parse(word, &st.auth_protos) < 0)) {
        reply_error(resp, 400, "methods are pap, chap-md5, mschap, mschap2, chap-sha1, chap-sha256, chap-sha384, "
                               "chap-sha512, chap-sha3-256, chap-sha3-384, chap-sha3-512");
        return;
    }
    if (form_get(body, "mppe", word, sizeof(word))) {
        if ((st.mppe = isp_mppe_parse(word)) < 0) {
            reply_error(resp, 400, "mppe is off, allowed or required");
            return;
        }
    }
    if (form_get(body, "strengths", word, sizeof(word))) {
        uint32_t s;

        if (isp_strengths_parse(word, &s) < 0) {
            reply_error(resp, 400, "MPPE strengths are 40, 56 and 128");
            return;
        }
        st.mppe_bits = (st.mppe_bits & ~MPPX_STRENGTHS) | s;
    }
    if ((r = form_int(body, "stateless", 0, 1, &v)) < 0) {
        reply_error(resp, 400, "stateless is 0 or 1");
        return;
    } else if (r)
        st.mppe_bits = v ? (st.mppe_bits | MPPX_H) : (st.mppe_bits & ~MPPX_H);
    if (form_get(body, "compression", word, sizeof(word)) && (isp_comp_parse(word, &st.compression) < 0)) {
        reply_error(resp, 400, "compressions are mppc, deflate, bsd and predictor1");
        return;
    }
    if (form_get(body, "wins", word, sizeof(word)) && (isp_wins_parse(word, st.wins) < 0)) {
        reply_error(resp, 400, "WINS is up to two addresses, a.b.c.d");
        return;
    }

    /* What cannot work together. */
    if ((st.auth != PPP_AUTH_NONE) && (st.auth_protos == 0)) {
        reply_error(resp, 400, "authentication needs at least one method");
        return;
    }
    if (st.mppe == PPP_MPPE_REQUIRED) {
        if ((st.auth != PPP_AUTH_ACCOUNTS) ||
            !(st.auth_protos & ((1u << PPP_AP_MSCHAP1) | (1u << PPP_AP_MSCHAP2)))) {
            reply_error(resp, 400, "encryption required needs accounts and MS-CHAP or MS-CHAP-2");
            return;
        }
        if (!(st.mppe_bits & MPPX_STRENGTHS)) {
            reply_error(resp, 400, "encryption required needs at least one strength");
            return;
        }
    }
    isp_set_settings(&st);
    isp_srv_settings_changed();
    reply_ok(resp);
}

/* -------------------------------------------------------------- the API */

void
isp_web_handle(const isp_web_request_t *req, isp_web_response_t *resp)
{
    const char *body = req->body ? req->body : "";
    who_t       w;
    int         setup;

    memset(resp, 0, sizeof(*resp));
    whois(req, &w);
    setup = (isp_users_count() == 0);

    /* With no users, only a browser on this machine that asked for a
       loopback name gets anything but the setup form. */
    if (setup && req->loopback && !host_ok(req)) {
        reply_error(resp, 403, "ask for this page as 127.0.0.1 or localhost");
        return;
    }

    if (!strcmp(req->method, "GET")) {
        if (!strcmp(req->path, "/") || !strcmp(req->path, "/index.html")) {
            sbuf_t b = { 0 };

            sb_add(&b, isp_web_page, strlen(isp_web_page));
            reply(resp, 200, "text/html; charset=utf-8", &b);
        } else if (!strcmp(req->path, "/api/session"))
            api_session(req, &w, resp);
        else if (!strncmp(req->path, "/api/", 5) && !may_view(&w))
            reply_error(resp, 403, setup ? "make the first user first" : "log in first");
        else if (!strcmp(req->path, "/api/status"))
            api_status(resp);
        else if (!strcmp(req->path, "/api/log"))
            api_log(req, resp);
        else if (!strcmp(req->path, "/api/accounts"))
            may_change(&w) ? api_accounts(req, resp) : reply_error(resp, 403, "for admins");
        else if (!strcmp(req->path, "/api/users"))
            may_keep_users(&w) ? api_users(req, &w, resp) : reply_error(resp, 403, "for the super admin");
        else
            reply_error(resp, 404, "no such page");
        return;
    }

    if (!strcmp(req->method, "POST")) {
        if (!req->from_page)
            reply_error(resp, 403, "changes come from the ISP's own page");
        else if (!strcmp(req->path, "/api/login"))
            api_login(req, resp);
        else if (!strcmp(req->path, "/api/setup"))
            api_setup(req, resp);
        else if (!strcmp(req->path, "/api/logout"))
            api_logout(req, &w, resp);
        else if (!strcmp(req->path, "/api/password"))
            api_password(req, &w, resp);
        else if (!may_view(&w))
            reply_error(resp, 403, setup ? "make the first user first" : "log in first");
        else if (!strcmp(req->path, "/api/users"))
            may_keep_users(&w) ? api_users(req, &w, resp) : reply_error(resp, 403, "for the super admin");
        else if (!may_change(&w))
            reply_error(resp, 403, "viewers look; admins change things");
        else if (!strcmp(req->path, "/api/hangup"))
            api_hangup(body, resp);
        else if (!strcmp(req->path, "/api/forwards"))
            api_forwards(body, resp);
        else if (!strcmp(req->path, "/api/settings"))
            api_settings(body, resp);
        else if (!strcmp(req->path, "/api/phone"))
            api_phone(body, resp);
        else if (!strcmp(req->path, "/api/accounts"))
            api_accounts(req, resp);
        else
            reply_error(resp, 404, "no such action");
        return;
    }

    reply_error(resp, 405, "GET or POST");
}
