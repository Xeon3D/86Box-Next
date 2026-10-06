/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             isp-server: the virtual ISP and the telephone exchange the
 *             emulator's modems dial (isp_srv.c), and its command line.
 *
 *             A modem on the "Telephone network" line gets a number and can
 *             dial other modems' numbers; any other number reaches the ISP:
 *             PPP, an address and DNS for the guest, and a NAT out to the
 *             host's Internet.  A modem on a plain TCP line to 127.0.0.1,
 *             port 2323 (the modem menu's "Dial the ISP") reaches the ISP
 *             directly.
 *
 *             Its window is a web page, http://127.0.0.1:2324/, opened in
 *             the browser at start (isp_web.c).  Settings and forwards are
 *             kept in isp-server.ini next to the program.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#    include <shellapi.h>
#else
#    include <unistd.h>
#endif
#include "isp.h"
#include "isp_plat.h"
#include "isp_srv.h"

#define MAX_CALLS 64

static volatile int stop_requested;

#ifdef _WIN32
static BOOL WINAPI
on_ctrl(DWORD type)
{
    (void) type;
    stop_requested = 1;
    return TRUE;
}
#else
static void
on_signal(int sig)
{
    (void) sig;
    stop_requested = 1;
}
#endif

static void
default_ini(char *path, size_t len)
{
#ifdef _WIN32
    char  exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *slash;

    if ((n > 0) && (n < sizeof(exe)) && ((slash = strrchr(exe, '\\')) != NULL)) {
        slash[1] = '\0';
        snprintf(path, len, "%sisp-server.ini", exe);
        return;
    }
#endif
    snprintf(path, len, "isp-server.ini");
}

static void
open_page(const char *url)
{
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
    char cmd[256];

#    ifdef __APPLE__
    snprintf(cmd, sizeof(cmd), "open '%s' >/dev/null 2>&1 &", url);
#    else
    snprintf(cmd, sizeof(cmd), "xdg-open '%s' >/dev/null 2>&1 &", url);
#    endif
    (void) !system(cmd);
#endif
}

static void
usage(void)
{
    printf("isp-server: the 86Box-Next virtual ISP and telephone exchange.\n"
           "\n"
           "Set a modem's line to \"Telephone network (isp-server)\": it gets a number,\n"
           "other modems can dial it, and any other number reaches the ISP.  Or point a\n"
           "modem's TCP line at 127.0.0.1, port 2323 (\"Dial the ISP\") for the ISP alone.\n"
           "Any name and password are accepted.  The status page opens in the browser.\n"
           "\n"
           "  --config FILE   settings and forwards (default: isp-server.ini by the program)\n"
           "  --listen ADDR   address modems connect to (default 127.0.0.1)\n"
           "  --port N        TCP port for modems (default 2323)\n"
           "  --http-port N   the status page, always on 127.0.0.1 (default 2324; 0: none)\n"
           "  --no-open       do not open the status page in the browser\n"
           "  --net A.B.0.0   the /16 the per-call /24s come from (default 10.86.0.0)\n"
           "  --max N         concurrent ISP calls (default 16, at most %d)\n"
           "  --pap           ask the guest for a name and password (any will do)\n"
           "  --echo SECS     LCP keepalive interval (default 0: none)\n"
           "  --no-lan        ISP calls cannot reach each other\n"
           "  --throttle BPS  hold every ISP call to this modem speed\n"
           "  --forward N:SPEC  e.g. 1:tcp:2121:21 -- host port 2121 to port 21 of call 1\n"
           "  --quiet         log calls, addresses and failures only\n"
           "\n"
           "Command-line settings override the file for this run.\n",
           MAX_CALLS);
}

int
main(int argc, char **argv)
{
    isp_srv_config_t cfg;
    isp_settings_t   st;
    int              open_it = 1;

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.listen, sizeof(cfg.listen), "127.0.0.1");
    cfg.port      = 2323;
    cfg.http_port = 2324;
    default_ini(cfg.ini, sizeof(cfg.ini));

    /* The file first, so that the command line can override it. */
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--config"))
            snprintf(cfg.ini, sizeof(cfg.ini), "%s", argv[i + 1]);
    isp_get_settings(&st);
    st.max_sessions = 16;
    isp_set_settings(&st);
    isp_srv_load(&cfg);
    isp_get_settings(&st);

    for (int i = 1; i < argc; i++) {
        const char *a    = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(a, "--config") && next)
            i++;
        else if (!strcmp(a, "--listen") && next) {
            snprintf(cfg.listen, sizeof(cfg.listen), "%s", next);
            i++;
        } else if (!strcmp(a, "--port") && next) {
            cfg.port = atoi(next);
            i++;
        } else if (!strcmp(a, "--http-port") && next) {
            cfg.http_port = atoi(next);
            if (cfg.http_port == 0)
                cfg.http_port = -1;
            i++;
        } else if (!strcmp(a, "--no-open"))
            open_it = 0;
        else if (!strcmp(a, "--net") && next) {
            unsigned b[4];

            if ((sscanf(next, "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) != 4) || (b[0] > 255) || (b[1] > 255)) {
                fprintf(stderr, "--net wants an address like 10.86.0.0\n");
                return 2;
            }
            st.base_net = (b[0] << 24) | (b[1] << 16);
            i++;
        } else if (!strcmp(a, "--max") && next) {
            st.max_sessions = atoi(next);
            i++;
        } else if (!strcmp(a, "--pap"))
            st.require_pap = 1;
        else if (!strcmp(a, "--echo") && next) {
            st.echo_secs = (uint32_t) atoi(next);
            i++;
        } else if (!strcmp(a, "--no-lan"))
            st.guest_lan = 0;
        else if (!strcmp(a, "--throttle") && next) {
            st.throttle     = 1;
            st.default_rate = (uint32_t) atoi(next);
            i++;
        } else if (!strcmp(a, "--forward") && next) {
            const int     n     = atoi(next);
            const char   *colon = strchr(next, ':');
            isp_forward_t fwd[ISP_MAX_FORWARDS];

            if ((n < 1) || (n > ISP_MAX_SESSIONS) || (colon == NULL) ||
                (isp_forwards_parse(colon + 1, fwd, ISP_MAX_FORWARDS) != 1)) {
                fprintf(stderr, "--forward wants CALL:tcp|udp:[*:]HOSTPORT:GUESTPORT, e.g. 1:tcp:2121:21\n");
                return 2;
            }
            isp_srv_add_forward(n, colon + 1);
            i++;
        } else if (!strcmp(a, "--quiet"))
            cfg.quiet = 1;
        else {
            usage();
            return (!strcmp(a, "--help") || !strcmp(a, "-h") || !strcmp(a, "/?")) ? 0 : 2;
        }
    }
    if ((cfg.port < 1) || (cfg.port > 65535) || (cfg.http_port > 65535) || (st.max_sessions < 1) ||
        (st.max_sessions > MAX_CALLS)) {
        usage();
        return 2;
    }
    isp_set_settings(&st);

#ifdef _WIN32
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    SetConsoleTitleA("86Box-Next ISP");
#else
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
#endif

    if (isp_srv_start(&cfg) != 0)
        return 1;
    if (cfg.http_port > 0) {
        char url[64];

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", cfg.http_port);
        printf("status page: %s\n", url);
        fflush(stdout);
        if (open_it)
            open_page(url);
    }

    while (!stop_requested)
#ifdef _WIN32
        Sleep(250);
#else
        usleep(250000);
#endif

    isp_srv_stop();
    return 0;
}
