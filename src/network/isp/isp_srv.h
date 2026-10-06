/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             isp-server's server: the port modems connect to, the status
 *             page's port, the telephone exchange, and isp-server.ini.
 *             isp_server.c is only its command line; the tests start it
 *             in-process.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef ISP_SRV_H
#define ISP_SRV_H

#include <stddef.h>
#include <stdint.h>

/* The exchange's protocol, spoken by a modem on its "Telephone network" line
   (char_modem.c).  One line, LF-terminated (CR ignored), opens a connection:

     86BOX-EXCHANGE 1 REGISTER <number|-> <label>
         The modem's line, held open while it is plugged in.  The exchange
         answers NUMBER <digits> (the one asked for, or another if that is
         taken), then sends RING <call> <caller|-> when the number is dialled
         and CANCEL <call> when the caller gives up.

     86BOX-EXCHANGE 1 DIAL <caller|-> <dialled>
         One call.  The exchange answers RINGING while another modem rings,
         then CONNECT -- after which the connection carries the call's bytes
         and nothing else -- or BUSY, NOANSWER or UNKNOWN, and closes.  A
         number that is no modem's reaches the ISP (unless that is switched
         off): CONNECT, then PPP.

     86BOX-EXCHANGE 1 ANSWER <call>
         The ringing modem picks up: CONNECT and the caller's bytes, or GONE.

   A connection that does not begin with the greeting is a modem on a plain
   TCP line to the ISP: PPP from the first byte. */
#define ISP_EXCHANGE_GREETING "86BOX-EXCHANGE 1 "

typedef struct isp_srv_config {
    char listen[64];    /* where modems connect (default 127.0.0.1)          */
    int  port;          /* 2323; 0 picks one, and isp_srv_start() says which */
    int  http_port;     /* 2324; 0 picks one; -1 for no status page          */
    char ini[1024];     /* isp-server.ini; "" for none                       */
    int  quiet;
    void (*log)(const char *line); /* NULL: stdout, timestamped */
} isp_srv_config_t;

/* Loads the .ini (the command line may then override isp_set_settings()
   and isp_srv_add_forward()), binds, starts the threads.  0 on success, with
   the ports actually bound written back. */
extern int  isp_srv_load(isp_srv_config_t *cfg);
extern int  isp_srv_start(isp_srv_config_t *cfg);
extern void isp_srv_stop(void);
extern void isp_srv_add_forward(int number, const char *spec);
extern int  isp_srv_save(void);

/* Exchange settings. */
extern void isp_srv_get_phone(int *unknown_to_isp, char *isp_numbers, size_t len);
extern void isp_srv_set_phone(int unknown_to_isp, const char *isp_numbers);

/* "5550101" -> "555-0101"; anything else as it is. */
extern void isp_srv_format_number(const char *digits, char *out, size_t len);

#endif /* ISP_SRV_H */
