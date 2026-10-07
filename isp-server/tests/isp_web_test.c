/*
 * 86Box-Next: isp-server's status page (isp-server/isp_web.c), its
 * requests handled directly, with the ISP core underneath.  No test
 * framework; non-zero on failure.
 *
 * With no users: the page as it always was for a browser on this machine
 * (status, hang-up, forwards, settings, the exchange), and what it refuses
 * -- a Host it was not asked for by (DNS rebinding), a change without
 * X-ISP-Request (another site's form), bad values -- and, from another
 * machine, nothing but the setup form, which wants the token.  Then the
 * users: the super admin made, logins, a viewer who looks and does not
 * touch, an admin who keeps the dial-in accounts but not the users,
 * wrong passwords slowed down, logging out.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#    include <winsock2.h>
#else
#    include <unistd.h>
#endif
#include "isp.h"
#include "isp_users.h"
#include "isp_web.h"

static int  failures;
static char forwards[ISP_MAX_SESSIONS + 1][256];
static int  saves;

/* What isp_srv.c would provide. */
void
isp_srv_get_forwards(int number, char *spec, size_t len)
{
    snprintf(spec, len, "%s", ((number >= 1) && (number <= ISP_MAX_SESSIONS)) ? forwards[number] : "");
}

int
isp_srv_set_forwards(int number, const char *spec)
{
    isp_forward_t fwd[ISP_MAX_FORWARDS];

    snprintf(forwards[number], sizeof(forwards[number]), "%s", spec);
    isp_set_call_forwards(number, fwd, isp_forwards_parse(spec, fwd, ISP_MAX_FORWARDS));
    saves++;
    return 1;
}

void
isp_srv_settings_changed(void)
{
    saves++;
}

void
isp_srv_config_changed(const char *what)
{
    (void) what;
    saves++;
}

void
isp_srv_endpoint(char *listen, size_t len, int *port)
{
    snprintf(listen, len, "127.0.0.1");
    *port = 2323;
}

int
isp_srv_log_lines(uint32_t after, isp_log_line_t *out, int max)
{
    int n = 0;

    for (uint32_t i = after + 1; (i <= 3) && (n < max); i++, n++) {
        out[n].n = i;
        snprintf(out[n].time, sizeof(out[n].time), "2026-10-07 12:00:0%u", i);
        snprintf(out[n].text, sizeof(out[n].text), "line %u \"quoted\"", i);
    }
    return n;
}

/* An exchange with two lines and a call between them. */
static int  phone_unknown = 1;
static char phone_numbers[128];
static int  phone_hung_up;

int
isp_srv_lines(isp_srv_line_t *out, int max)
{
    if (max < 2)
        return 0;
    memset(out, 0, 2 * sizeof(*out));
    snprintf(out[0].number, sizeof(out[0].number), "5550101");
    snprintf(out[0].label, sizeof(out[0].label), "Win98 (COM2)");
    out[0].state = ISP_LINE_BUSY;
    snprintf(out[1].number, sizeof(out[1].number), "5550102");
    snprintf(out[1].label, sizeof(out[1].label), "WfW \"3.11\"");
    out[1].state = ISP_LINE_BUSY;
    return 2;
}

int
isp_srv_phone_calls(isp_srv_pcall_t *out, int max)
{
    if ((max < 1) || phone_hung_up)
        return 0;
    memset(out, 0, sizeof(*out));
    out->id = 7;
    snprintf(out->from, sizeof(out->from), "5550101");
    snprintf(out->to, sizeof(out->to), "5550102");
    out->state      = ISP_PCALL_ACTIVE;
    out->from_bytes = 1234;
    return 1;
}

int
isp_srv_hangup_phone(int id)
{
    if ((id != 7) || phone_hung_up)
        return 0;
    phone_hung_up = 1;
    return 1;
}

void
isp_srv_get_phone(int *unknown, char *numbers, size_t len)
{
    *unknown = phone_unknown;
    snprintf(numbers, len, "%s", phone_numbers);
}

void
isp_srv_set_phone(int unknown, const char *numbers)
{
    phone_unknown = unknown;
    snprintf(phone_numbers, sizeof(phone_numbers), "%s", numbers);
    saves++;
}

static void
check(const char *what, int ok)
{
    if (!ok)
        failures++;
    printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
}

/* Who is asking: from where, with what cookie. */
typedef struct {
    int  loopback;
    char cookie[200];
} client_t;

static client_t here   = { 1, "" };
static client_t remote = { 0, "" };

/* One request; the response's status, its body in `body`; a Set-Cookie is
   taken up by the client, as a browser would. */
static int
ask_as(client_t *c, const char *method, const char *path, const char *host, int from_page, const char *form,
       char *body, size_t len)
{
    isp_web_request_t  req;
    isp_web_response_t resp;
    int                status;
    char               p[128];
    char              *q;

    snprintf(p, sizeof(p), "%s", path);
    q = strchr(p, '?');
    if (q != NULL)
        *q++ = '\0';
    memset(&req, 0, sizeof(req));
    req.method    = method;
    req.path      = p;
    req.host      = host;
    req.from_page = from_page;
    req.body      = form;
    req.http_port = 2324;
    req.cookie    = c->cookie;
    req.loopback  = c->loopback;
    req.query     = q ? q : "";
    isp_web_handle(&req, &resp);
    status = resp.status;
    if (body != NULL)
        snprintf(body, len, "%s", resp.body ? resp.body : "");
    if (resp.set_cookie[0]) {
        const size_t n = strcspn(resp.set_cookie, ";");

        snprintf(c->cookie, sizeof(c->cookie), "%.*s", (int) n, resp.set_cookie);
        if (strstr(resp.set_cookie, "Max-Age=0"))
            c->cookie[0] = '\0';
    }
    free(resp.body);
    return status;
}

static int
ask(const char *method, const char *path, const char *host, int from_page, const char *form, char *body, size_t len)
{
    return ask_as(&here, method, path, host, from_page, form, body, len);
}

int
main(void)
{
    static char    body[65536];
    isp_settings_t st;
    isp_session_t *s;
    char           err[128];

#ifdef _WIN32
    WSADATA wsa;

    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    printf("== isp-server's status page, no users: this machine ==\n");

    check("the page, asked for as 127.0.0.1",
          (ask("GET", "/", "127.0.0.1:2324", 0, "", body, sizeof(body)) == 200) && strstr(body, "86Box-Next ISP") &&
          strstr(body, "/api/status"));
    check("...or as localhost", ask("GET", "/", "localhost:2324", 0, "", NULL, 0) == 200);
    check("refused by any other name (DNS rebinding)", ask("GET", "/api/status", "evil.example:2324", 0, "", NULL, 0) == 403);
    check("...or the wrong port", ask("GET", "/", "127.0.0.1:80", 0, "", NULL, 0) == 403);
    check("an unknown page: 404", ask("GET", "/nope", "127.0.0.1:2324", 0, "", NULL, 0) == 404);
    check("the session: no users yet, this machine may do everything",
          (ask("GET", "/api/session", "127.0.0.1:2324", 0, "", body, sizeof(body)) == 200) &&
          strstr(body, "\"setup\":true") && strstr(body, "\"role\":\"local\""));

    s = isp_session_open(NULL, err, sizeof(err));
    isp_session_set_label(s, "127.0.0.1:50000 \"quoted\"");
    check("status lists a call",
          (ask("GET", "/api/status", "127.0.0.1:2324", 0, "", body, sizeof(body)) == 200) &&
          strstr(body, "\"number\":1") && strstr(body, "\"ip\":\"10.86.1.15\"") &&
          strstr(body, "\"state\":\"waiting for PPP\"") && strstr(body, "\"listen\":\"127.0.0.1\""));
    check("...its label escaped for JSON", strstr(body, "\"label\":\"127.0.0.1:50000 \\\"quoted\\\"\"") != NULL);
    check("...and the new settings and call fields",
          strstr(body, "\"auth\":\"none\"") && strstr(body, "\"methods\":\"pap chap-md5 mschap mschap2") &&
          strstr(body, "\"mppe\":\"allowed\"") && strstr(body, "\"compression\":\"mppc deflate bsd predictor1\"") &&
          strstr(body, "\"multilink\":1") && strstr(body, "\"encrypted\":0,\"bundle\":0,\"links\":1"));
    check("the log, after line 1",
          (ask("GET", "/api/log?after=1", "127.0.0.1:2324", 0, "", body, sizeof(body)) == 200) &&
          strstr(body, "{\"n\":2,") && !strstr(body, "{\"n\":1,") && strstr(body, "\"next\":3") &&
          strstr(body, "line 3 \\\"quoted\\\""));

    /* Changes need the page's own header. */
    check("a change without X-ISP-Request: refused",
          ask("POST", "/api/hangup", "127.0.0.1:2324", 0, "n=1", NULL, 0) == 403);

    check("forwards: tcp and udp on every interface",
          ask("POST", "/api/forwards", "127.0.0.1:2324", 1, "n=1&spec=tcp%3A2121%3A21+udp%3A*%3A5000%3A5000", NULL, 0) == 200);
    check("...kept by number and shown", !strcmp(forwards[1], "tcp:2121:21 udp:*:5000:5000") &&
                                             (ask("GET", "/api/status", "127.0.0.1:2324", 0, "", body, sizeof(body)) == 200) &&
                                             strstr(body, "\"spec\":\"tcp:2121:21 udp:*:5000:5000\""));
    check("a typo in a forward: 400, nothing changed",
          (ask("POST", "/api/forwards", "127.0.0.1:2324", 1, "n=1&spec=tcp%3A2121%3A21+ftp%3A1%3A2", NULL, 0) == 400) &&
          !strcmp(forwards[1], "tcp:2121:21 udp:*:5000:5000"));
    check("forwards for no call number: 400", ask("POST", "/api/forwards", "127.0.0.1:2324", 1, "n=0&spec=", NULL, 0) == 400);
    check("removing them all", (ask("POST", "/api/forwards", "127.0.0.1:2324", 1, "n=1&spec=", NULL, 0) == 200) &&
                                   (forwards[1][0] == '\0'));

    check("settings: guest LAN off, throttle at 28800, PAP, keepalive 30",
          ask("POST", "/api/settings", "127.0.0.1:2324", 1, "lan=0&throttle=1&rate=28800&pap=1&echo=30&net=10.99.0.0&max=8",
              NULL, 0) == 200);
    isp_get_settings(&st);
    check("...in effect (the old switch: anyone, by name and password)",
          !st.guest_lan && st.throttle && (st.default_rate == 28800) && (st.auth == PPP_AUTH_ANY) &&
          (st.echo_secs == 30) && (st.base_net == 0x0a630000) && (st.max_sessions == 8));
    check("a range that is not a /16: 400", ask("POST", "/api/settings", "127.0.0.1:2324", 1, "net=10.99.1.0", NULL, 0) == 400);
    check("a flag that is not 0 or 1: 400", ask("POST", "/api/settings", "127.0.0.1:2324", 1, "lan=2", NULL, 0) == 400);
    check("...and nothing changed by them", (isp_get_settings(&st), (st.base_net == 0x0a630000) && !st.guest_lan));
    check("accounts only, MS-CHAP-2 and PAP, MPPE 128 required, stateful, Deflate, WINS, no Multilink",
          ask("POST", "/api/settings", "127.0.0.1:2324", 1,
              "auth=accounts&methods=mschap2+pap&mppe=required&strengths=128&stateless=0&compression=deflate"
              "&wins=10.0.0.5+10.0.0.6&multilink=0",
              NULL, 0) == 200);
    isp_get_settings(&st);
    check("...in effect", (st.auth == PPP_AUTH_ACCOUNTS) &&
                              (st.auth_protos == ((1u << PPP_AP_MSCHAP2) | (1u << PPP_AP_PAP))) &&
                              (st.mppe == PPP_MPPE_REQUIRED) && ((st.mppe_bits & (MPPX_STRENGTHS | MPPX_H)) == MPPX_S) &&
                              (st.compression == (1u << PPP_COMP_DEFLATE)) && (st.wins[0] == 0x0a000005) &&
                              (st.wins[1] == 0x0a000006) && !st.multilink);
    check("encryption required without MS-CHAP: 400",
          ask("POST", "/api/settings", "127.0.0.1:2324", 1, "methods=pap+chap-md5", NULL, 0) == 400);
    check("an unknown method: 400", ask("POST", "/api/settings", "127.0.0.1:2324", 1, "methods=kerberos", NULL, 0) == 400);
    check("a WINS server that is no address: 400",
          ask("POST", "/api/settings", "127.0.0.1:2324", 1, "wins=wins.example", NULL, 0) == 400);
    check("...nothing changed by them", (isp_get_settings(&st), (st.auth_protos == ((1u << PPP_AP_MSCHAP2) | (1u << PPP_AP_PAP)))));
    check("back to no authentication and no encryption",
          ask("POST", "/api/settings", "127.0.0.1:2324", 1, "auth=none&mppe=off", NULL, 0) == 200);

    check("hang up a call that is not there: 404", ask("POST", "/api/hangup", "127.0.0.1:2324", 1, "n=9", NULL, 0) == 404);
    check("hang up call 1", ask("POST", "/api/hangup", "127.0.0.1:2324", 1, "n=1", NULL, 0) == 200);
    {
        int ended = 0;

        for (int i = 0; (i < 200) && !ended; i++) {
            ended = isp_session_ended(s);
#ifdef _WIN32
            Sleep(10);
#else
            usleep(10000);
#endif
        }
        check("...and it ends (no PPP yet: at once)", ended);
    }
    isp_session_close(s);

    /* The telephone exchange. */
    {
        const int ok = (ask("GET", "/api/status", "127.0.0.1:2324", 0, "", body, sizeof(body)) == 200) &&
                       strstr(body, "\"lines\":[{\"number\":\"5550101\",\"label\":\"Win98 (COM2)\",\"state\":\"in a call\"") &&
                       strstr(body, "\"label\":\"WfW \\\"3.11\\\"\"") &&
                       strstr(body, "\"phone_calls\":[{\"id\":7,\"from\":\"5550101\"") &&
                       strstr(body, "\"from_bytes\":1234") && strstr(body, "\"phone\":{\"unknown_to_isp\":1");

        check("status: the exchange's lines and the call between them", ok);
        if (!ok)
            printf("      %s\n", strstr(body, "\"phone\"") ? strstr(body, "\"phone\"") : body);
    }
    check("hang up the modem-to-modem call", ask("POST", "/api/hangup", "127.0.0.1:2324", 1, "call=7", NULL, 0) == 200);
    check("...once", ask("POST", "/api/hangup", "127.0.0.1:2324", 1, "call=7", NULL, 0) == 404);
    check("exchange: other numbers unknown, the ISP on 0191 and 555-1234",
          (ask("POST", "/api/phone", "127.0.0.1:2324", 1, "unknown_to_isp=0&isp_numbers=0191%2C+555-1234", NULL, 0) == 200) &&
          !phone_unknown && !strcmp(phone_numbers, "0191, 555-1234"));
    check("ISP numbers with letters in: 400",
          ask("POST", "/api/phone", "127.0.0.1:2324", 1, "isp_numbers=0800-FLOWERS", NULL, 0) == 400);
    check("...and the exchange needs the page's header too",
          ask("POST", "/api/phone", "127.0.0.1:2324", 0, "unknown_to_isp=1", NULL, 0) == 403);
    check("every change was saved, and only those", saves == 6);

    /* Dial-in accounts. */
    check("an account for alice", ask("POST", "/api/accounts", "127.0.0.1:2324", 1, "op=add&user=alice&password=s3cret", NULL, 0) == 200);
    check("...and one for DOMAIN\\bob", ask("POST", "/api/accounts", "127.0.0.1:2324", 1, "op=add&user=DOMAIN%5Cbob&password=pw", NULL, 0) == 200);
    check("...not twice", ask("POST", "/api/accounts", "127.0.0.1:2324", 1, "op=add&user=alice&password=x", NULL, 0) == 409);
    check("...nor with no password", ask("POST", "/api/accounts", "127.0.0.1:2324", 1, "op=add&user=carol", NULL, 0) == 400);
    check("listed by name, the passwords never shown",
          (ask("GET", "/api/accounts", "127.0.0.1:2324", 0, "", body, sizeof(body)) == 200) &&
          strstr(body, "\"user\":\"alice\"") && strstr(body, "\"user\":\"DOMAIN\\\\bob\"") && !strstr(body, "s3cret"));
    {
        char pw[64];

        check("the core finds alice, whatever the case of her name",
              isp_account_password("ALICE", pw, sizeof(pw)) && !strcmp(pw, "s3cret"));
        check("a new password for alice",
              (ask("POST", "/api/accounts", "127.0.0.1:2324", 1, "op=password&user=alice&password=n3w", NULL, 0) == 200) &&
              isp_account_password("alice", pw, sizeof(pw)) && !strcmp(pw, "n3w"));
        check("bob removed", (ask("POST", "/api/accounts", "127.0.0.1:2324", 1, "op=delete&user=DOMAIN%5Cbob", NULL, 0) == 200) &&
                                 !isp_account_password("DOMAIN\\bob", pw, sizeof(pw)));
    }

    printf("\n== no users: another machine ==\n");
    check("the page itself, to show the setup form", ask_as(&remote, "GET", "/", "isp.example:2324", 0, "", NULL, 0) == 200);
    check("its session: setup needed, nobody",
          (ask_as(&remote, "GET", "/api/session", "isp.example:2324", 0, "", body, sizeof(body)) == 200) &&
          strstr(body, "\"setup\":true") && strstr(body, "\"role\":\"none\"") && strstr(body, "\"local\":false"));
    check("nothing else: status 403", ask_as(&remote, "GET", "/api/status", "isp.example:2324", 0, "", NULL, 0) == 403);
    check("...nor changes", ask_as(&remote, "POST", "/api/settings", "isp.example:2324", 1, "lan=1", NULL, 0) == 403);
    check("setup without the token: 403",
          ask_as(&remote, "POST", "/api/setup", "isp.example:2324", 1, "name=root&password=rootpass", NULL, 0) == 403);
    check("...or with a wrong one",
          ask_as(&remote, "POST", "/api/setup", "isp.example:2324", 1, "name=root&password=rootpass&token=0123", NULL, 0) == 403);
    {
        char form[160];

        snprintf(form, sizeof(form), "name=root&password=short&token=%s", isp_users_setup_token());
        check("the right token, but too short a password: 400",
              ask_as(&remote, "POST", "/api/setup", "isp.example:2324", 1, form, NULL, 0) == 400);
        snprintf(form, sizeof(form), "name=root&password=rootpass&token=%s", isp_users_setup_token());
        check("the right token: the super admin made, and logged in",
              (ask_as(&remote, "POST", "/api/setup", "isp.example:2324", 1, form, NULL, 0) == 200) &&
              !strncmp(remote.cookie, "isp_session=", 12) && (isp_users_count() == 1));
    }
    check("...the token is spent", isp_users_setup_token()[0] == '\0');
    check("...and setup is over", ask_as(&remote, "POST", "/api/setup", "isp.example:2324", 1, "name=x&password=xxxxxxxx", NULL, 0) == 409);

    printf("\n== users ==\n");
    check("the super admin sees the status, by any host name now",
          ask_as(&remote, "GET", "/api/status", "isp.example:2324", 0, "", NULL, 0) == 200);
    check("...this machine without a login no longer does",
          ask("GET", "/api/status", "127.0.0.1:2324", 0, "", NULL, 0) == 403);
    check("the session says who",
          (ask_as(&remote, "GET", "/api/session", "isp.example:2324", 0, "", body, sizeof(body)) == 200) &&
          strstr(body, "\"role\":\"super\"") && strstr(body, "\"user\":\"root\"") && strstr(body, "\"setup\":false"));
    check("adds a viewer", ask_as(&remote, "POST", "/api/users", "isp.example:2324", 1,
                                  "op=add&name=val&password=viewpass&role=viewer", NULL, 0) == 200);
    check("...and an admin", ask_as(&remote, "POST", "/api/users", "isp.example:2324", 1,
                                    "op=add&name=adam&password=adminpass&role=admin", NULL, 0) == 200);
    check("...not a second super admin", ask_as(&remote, "POST", "/api/users", "isp.example:2324", 1,
                                                "op=add&name=eve&password=evepass1&role=super", NULL, 0) == 400);
    check("...nor removes itself", ask_as(&remote, "POST", "/api/users", "isp.example:2324", 1, "op=delete&name=root", NULL, 0) == 400);
    check("the users, listed",
          (ask_as(&remote, "GET", "/api/users", "isp.example:2324", 0, "", body, sizeof(body)) == 200) &&
          strstr(body, "{\"name\":\"root\",\"role\":\"super\"}") && strstr(body, "{\"name\":\"val\",\"role\":\"viewer\"}") &&
          strstr(body, "{\"name\":\"adam\",\"role\":\"admin\"}") && !strstr(body, "pass"));
    {
        client_t viewer = { 0, "" };
        client_t admin  = { 0, "" };

        check("the viewer logs in", (ask_as(&viewer, "POST", "/api/login", "isp.example:2324", 1, "name=val&password=viewpass", NULL, 0) == 200) &&
                                        viewer.cookie[0]);
        check("...sees the status and the log", (ask_as(&viewer, "GET", "/api/status", "isp.example:2324", 0, "", NULL, 0) == 200) &&
                                                    (ask_as(&viewer, "GET", "/api/log", "isp.example:2324", 0, "", NULL, 0) == 200));
        check("...changes nothing", (ask_as(&viewer, "POST", "/api/settings", "isp.example:2324", 1, "lan=1", NULL, 0) == 403) &&
                                        (ask_as(&viewer, "POST", "/api/hangup", "isp.example:2324", 1, "n=1", NULL, 0) == 403));
        check("...nor sees the accounts or the users",
              (ask_as(&viewer, "GET", "/api/accounts", "isp.example:2324", 0, "", NULL, 0) == 403) &&
              (ask_as(&viewer, "GET", "/api/users", "isp.example:2324", 0, "", NULL, 0) == 403));
        check("...but changes its own password",
              ask_as(&viewer, "POST", "/api/password", "isp.example:2324", 1, "current=viewpass&password=viewpass2", NULL, 0) == 200);
        check("...and still is logged in", ask_as(&viewer, "GET", "/api/status", "isp.example:2324", 0, "", NULL, 0) == 200);

        check("the admin logs in", ask_as(&admin, "POST", "/api/login", "isp.example:2324", 1, "name=adam&password=adminpass", NULL, 0) == 200);
        check("...changes settings and keeps accounts",
              (ask_as(&admin, "POST", "/api/settings", "isp.example:2324", 1, "lan=1", NULL, 0) == 200) &&
              (ask_as(&admin, "POST", "/api/accounts", "isp.example:2324", 1, "op=add&user=dave&password=d", NULL, 0) == 200));
        check("...but not the users", ask_as(&admin, "POST", "/api/users", "isp.example:2324", 1, "op=delete&name=val", NULL, 0) == 403);

        check("the super admin makes the viewer an admin",
              ask_as(&remote, "POST", "/api/users", "isp.example:2324", 1, "op=role&name=val&role=admin", NULL, 0) == 200);
        check("...whose next request may change things",
              ask_as(&viewer, "POST", "/api/settings", "isp.example:2324", 1, "lan=1", NULL, 0) == 200);
        check("the super admin removes the admin", ask_as(&remote, "POST", "/api/users", "isp.example:2324", 1, "op=delete&name=adam", NULL, 0) == 200);
        check("...whose login ends at once", ask_as(&admin, "GET", "/api/status", "isp.example:2324", 0, "", NULL, 0) == 403);

        check("logging out", (ask_as(&viewer, "POST", "/api/logout", "isp.example:2324", 1, "", NULL, 0) == 200) && !viewer.cookie[0]);
        check("...the cookie gone, nothing to see", ask_as(&viewer, "GET", "/api/status", "isp.example:2324", 0, "", NULL, 0) == 403);
        {
            client_t forged = { 0, "isp_session=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" };

            check("a made-up token: nothing", ask_as(&forged, "GET", "/api/status", "isp.example:2324", 0, "", NULL, 0) == 403);
        }
        {
            int codes[6];

            for (int i = 0; i < 6; i++)
                codes[i] = ask_as(&viewer, "POST", "/api/login", "isp.example:2324", 1, "name=val&password=guess", NULL, 0);
            /* (The setup attempts with a wrong token counted too.) */
            check("wrong passwords: 403, then 429 for everyone",
                  (codes[0] == 403) && (codes[5] == 429) &&
                  (ask_as(&viewer, "POST", "/api/login", "isp.example:2324", 1, "name=val&password=viewpass2", NULL, 0) == 429));
        }
    }
    check("users are kept as hashes", (isp_users_count() == 2));
    {
        isp_user_info_t list[8];
        const int       n = isp_users_list(list, 8);

        check("...the super admin first", (n == 2) && !strcmp(list[0].name, "root") && (list[0].role == ISP_ROLE_SUPER) &&
                                              !strcmp(list[1].name, "val") && (list[1].role == ISP_ROLE_ADMIN));
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed", failures,
           (failures == 1) ? "" : "s");
    return failures ? 1 : 0;
}
