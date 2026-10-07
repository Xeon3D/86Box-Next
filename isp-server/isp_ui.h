/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             isp-server's face.  On Windows a log window that minimizes to
 *             the notification area (isp_ui_win.c); elsewhere the terminal
 *             (isp_ui_term.c).  isp_server.c drives either.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef ISP_UI_H
#define ISP_UI_H

/* Text for the user before there is a window: the usage, a bad option. */
extern void isp_ui_message(int error, const char *text);

/* The log: isp_srv_config_t.log.  Any thread, before isp_ui_open() too. */
extern void isp_ui_log(const char *line);

/* Shows the program, in the notification area only if minimized.  0 on
   success. */
extern int isp_ui_open(int minimized);

/* The status page's address, and opening it in the browser. */
extern void isp_ui_set_page(const char *url, int open_it);

/* Until the user quits.  If the server could not start (running 0), the
   log is put in front, so that the reason can be read, and kept until it is
   closed. */
extern void isp_ui_run(int running);

/* After the server has stopped. */
extern void isp_ui_close(void);

#endif /* ISP_UI_H */
