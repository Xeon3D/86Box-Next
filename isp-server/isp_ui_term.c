/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             isp-server in a terminal (all but Windows): the log on
 *             stdout, timestamped; Ctrl+C or SIGTERM stops it.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "isp_ui.h"

static volatile sig_atomic_t stop_requested;

static void
on_signal(int sig)
{
    (void) sig;
    stop_requested = 1;
}

void
isp_ui_message(int error, const char *text)
{
    fputs(text, error ? stderr : stdout);
}

void
isp_ui_log(const char *line)
{
    char       stamp[32];
    time_t     t  = time(NULL);
    struct tm *tm = localtime(&t);

    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm);
    printf("%s  %s\n", stamp, line);
    fflush(stdout);
}

int
isp_ui_open(int minimized)
{
    (void) minimized;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

void
isp_ui_set_page(const char *url, int open_it)
{
    char cmd[256];

    printf("status page: %s\n", url);
    fflush(stdout);
    if (!open_it)
        return;
#ifdef __APPLE__
    snprintf(cmd, sizeof(cmd), "open '%s' >/dev/null 2>&1 &", url);
#else
    snprintf(cmd, sizeof(cmd), "xdg-open '%s' >/dev/null 2>&1 &", url);
#endif
    (void) !system(cmd);
}

void
isp_ui_run(int running)
{
    while (running && !stop_requested)
        usleep(250000);
}

void
isp_ui_close(void)
{
}
