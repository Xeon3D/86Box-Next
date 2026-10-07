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
    CHAR_MODEM_LINE_DEAD  = 0, /* not connected                                  */
    CHAR_MODEM_LINE_TCP   = 1, /* dialling reaches a TCP/IP host                 */
    CHAR_MODEM_LINE_PHONE = 2  /* isp-server's telephone network: a number, other
                                  modems' numbers, and the ISP on any other one.
                                  (For a day 2 was an ISP built into the
                                  emulator; this still reaches the ISP.)       */
};

/* Where isp-server (isp-server/) listens by default: its exchange, and
   the plain TCP line to its ISP ("Dial the ISP"). */
#define CHAR_MODEM_ISP_HOST "127.0.0.1"
#define CHAR_MODEM_ISP_PORT 2323
#define CHAR_MODEM_EXCHANGE "127.0.0.1:2323"

enum {
    CHAR_MODEM_ABSENT = -1,
    CHAR_MODEM_IDLE   = 0, /* on hook                      */
    CHAR_MODEM_CALLING,    /* dialling, ringing, training  */
    CHAR_MODEM_ONLINE,     /* carrier up                   */
    CHAR_MODEM_VOICE,      /* off hook in voice mode       */
    CHAR_MODEM_HANDSET     /* the handset's call           */
};

/* 86Box-Next: the handset -- the telephone beside the modem, which is the
   host's speaker and microphone. */
enum {
    CHAR_MODEM_HANDSET_HANGUP = 0,
    CHAR_MODEM_HANDSET_PICKUP,     /* answers a call ringing, or a dial tone */
    CHAR_MODEM_HANDSET_DIAL        /* off hook if it is not, then the number */
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
/* The telephone network: the number asked for ("" lets the exchange choose)
   and the exchange's address, as set; and the number the exchange gave the
   modem, "" while it is not registered (is isp-server running?).  Returns
   1 when registered, 0 when not, -1 with no modem on that port. */
extern int         char_modem_get_phone(int com, char *want, size_t want_len, char *exchange, size_t ex_len,
                                        char *number, size_t num_len);
/* Saved like the line; registers again with the new number or exchange. */
extern void        char_modem_set_phone(int com, const char *want, const char *exchange);
/* The handset (CHAR_MODEM_HANDSET_*), applied on the emulation thread. */
extern void        char_modem_handset(int com, int action, const char *number);
/* Bit 0: the handset is off hook; bit 1: a call is ringing; bit 2: the modem
   can talk (a Rockwell voice modem on the telephone network).  -1 with no
   modem on that port. */
extern int         char_modem_handset_state(int com);

#ifdef __cplusplus
}
#endif

#endif /* EMU_CHAR_MODEM_H */
