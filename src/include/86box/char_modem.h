/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          86Box-Next: the modems (src/char/char_modem.c), as the status bar
 *          sees them.  `com` is a slot: 0 to SERIAL_MAX - 1 the COM ports,
 *          then one per PC Card socket (a PC Card's own modem, such as the
 *          3C562D's); char_modem_slot_label() names it.
 *
 *          Released under the GNU General Public License version 2 or
 *          later.  See COPYING for more information.
 */
#ifndef EMU_CHAR_MODEM_H
#define EMU_CHAR_MODEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Saved in the configuration as these numbers: never renumber. */
enum {
    CHAR_MODEM_LINE_DEAD = 0, /* not connected                                */
    CHAR_MODEM_LINE_TCP  = 1, /* dialling reaches a TCP/IP host               */
    CHAR_MODEM_LINE_ISP  = 2  /* dialling reaches the built-in ISP (86box/isp.h) */
};

enum {
    CHAR_MODEM_ABSENT = -1,
    CHAR_MODEM_IDLE   = 0, /* on hook                      */
    CHAR_MODEM_CALLING,    /* dialling, ringing, training  */
    CHAR_MODEM_ONLINE      /* carrier up                   */
};

extern int         char_modem_slots(void);
extern void        char_modem_slot_label(int com, char *buf, size_t len); /* "COM1", "PC Card A" */
extern int         char_modem_present(int com);
extern const char *char_modem_name(int com);
extern int         char_modem_get_state(int com);
/* Returns the line (CHAR_MODEM_LINE_*), or -1 with no modem on that port. */
extern int         char_modem_get_line(int com, char *host, size_t host_len, int *port);
/* Takes effect at once, without a reset: a call in progress ends with NO
   CARRIER.  Also saved in the modem's configuration section (a PC Card
   modem's: the card's). */
extern void        char_modem_set_line(int com, int line, const char *host, int port);

#ifdef __cplusplus
}
#endif

#endif /* EMU_CHAR_MODEM_H */
