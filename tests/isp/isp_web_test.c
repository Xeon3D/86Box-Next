/*
 * 86Box-Next: isp-server's status page (src/network/isp/isp_web.c), its
 * requests handled directly, with the ISP core underneath.  No test
 * framework; non-zero on failure.
 *
 * The page, the status JSON with a call in it, hang-up, forwards and
 * settings, and what it refuses: a Host it was not asked for by (DNS
 * rebinding), a change without X-ISP-Request (another site's form), bad
 * values.
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
#include "isp_web.h"

static int  failures;
static char forwards[ISP_MAX_SESSIONS + 1][256];
static int  saves;

/* What isp_server.c would provide. */
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
isp_srv_endpoint(char *listen, size_t len, int *port)
{
    snprintf(listen, len, "127.0.0.1");
    *port = 2323;
}

static void
check(const char *what, int ok)
{
    if (!ok)
        failures++;
    printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
}

/* One request; the response's status, its body in `body`. */
static int
ask(const char *method, const char *path, const char *host, int from_page, const char *form, char *body, size_t len)
{
    isp_web_request_t  req = { method, path, host, from_page, form, 2324 };
    isp_web_response_t resp;
    int                status;

    isp_web_handle(&req, &resp);
    status = resp.status;
    if (body != NULL)
        snprintf(body, len, "%s", resp.body ? resp.body : "");
    free(resp.body);
    return status;
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
    printf("== isp-server's status page ==\n");

    check("the page, asked for as 127.0.0.1",
          (ask("GET", "/", "127.0.0.1:2324", 0, "", body, sizeof(body)) == 200) && strstr(body, "86Box-Next ISP") &&
          strstr(body, "/api/status"));
    check("...or as localhost", ask("GET", "/", "localhost:2324", 0, "", NULL, 0) == 200);
    check("refused by any other name (DNS rebinding)", ask("GET", "/api/status", "evil.example:2324", 0, "", NULL, 0) == 403);
    check("...or the wrong port", ask("GET", "/", "127.0.0.1:80", 0, "", NULL, 0) == 403);
    check("an unknown page: 404", ask("GET", "/nope", "127.0.0.1:2324", 0, "", NULL, 0) == 404);

    s = isp_session_open(NULL, err, sizeof(err));
    isp_session_set_label(s, "127.0.0.1:50000 \"quoted\"");
    check("status lists a call",
          (ask("GET", "/api/status", "127.0.0.1:2324", 0, "", body, sizeof(body)) == 200) &&
          strstr(body, "\"number\":1") && strstr(body, "\"ip\":\"10.86.1.15\"") &&
          strstr(body, "\"state\":\"waiting for PPP\"") && strstr(body, "\"listen\":\"127.0.0.1\""));
    check("...its label escaped for JSON", strstr(body, "\"label\":\"127.0.0.1:50000 \\\"quoted\\\"\"") != NULL);

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
    check("...in effect", !st.guest_lan && st.throttle && (st.default_rate == 28800) && st.require_pap &&
                              (st.echo_secs == 30) && (st.base_net == 0x0a630000) && (st.max_sessions == 8));
    check("a range that is not a /16: 400", ask("POST", "/api/settings", "127.0.0.1:2324", 1, "net=10.99.1.0", NULL, 0) == 400);
    check("a flag that is not 0 or 1: 400", ask("POST", "/api/settings", "127.0.0.1:2324", 1, "lan=2", NULL, 0) == 400);
    check("...and nothing changed by them", (isp_get_settings(&st), (st.base_net == 0x0a630000) && !st.guest_lan));

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
    check("every change was saved, and only those", saves == 3);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed", failures,
           (failures == 1) ? "" : "s");
    return failures ? 1 : 0;
}
