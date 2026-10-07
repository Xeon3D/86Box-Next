/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          86Box-Next: the modems (src/char/char_modem.c).  A modem is either
 *          a COM port's device (Settings > Ports) or carried by another device
 *          on a UART of its own -- a PC Card's, say -- which opens it with
 *          char_modem_attach().  The modem engine knows nothing of what
 *          carries it but the label it is given.
 *
 *          As the status bar sees them, `com` is a slot: 0 to SERIAL_MAX - 1
 *          the COM ports, then the carried modems, each in the first slot
 *          free when it was attached; char_modem_slot_label() names it
 *          ("COM1", or the label its device gave it).
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

/* What a modem answers to ATI0..ATI9, and what it can do. */
typedef struct char_modem_model_t {
    const char *name;       /* the part, as the status bar names it         */
    int         ident_at;   /* which ATIn carries the identification string */
    const char *ident;      /* ...and its text                              */
    int         fmw_at;     /* which ATIn carries the firmware version      */
    const char *fmw;        /* ...and its text                              */
    int         country_at; /* which ATIn reports the country code          */
    const char *info[10];   /* the rest of ATI0..ATI9; NULL: says nothing   */
    int         voice;      /* a voice modem: Rockwell's #CLS=8 set, V.253's
                               +FCLASS=8, and the handset beside it.  0: data
                               and fax only, voice commands are ERROR       */
} char_modem_model_t;

/* A modem another device carries on a UART of its own (serial_init_detached())
   rather than one on a COM port: the modem engine on port, that UART's char
   port, answering as model (kept by the caller).  Call it from the device's
   init, in its device context: the modem's settings are the device's -- its
   config has CHAR_MODEM_CONFIG_LINE, a "connect_rate" selection and
   CHAR_MODEM_CONFIG_SPEAKER -- and a line changed from the status bar is
   saved there.  label names it in the status bar and on the exchange ("PC
   Card A").  char_modem_detach() closes it.  NULL if it could not be made. */
#ifdef EMU_CHAR_H
extern void *char_modem_attach(char_port_t *port, const char_modem_model_t *model, const char *label);
#endif
extern void  char_modem_detach(void *modem);

extern int         char_modem_slots(void);
extern void        char_modem_slot_label(int com, char *buf, size_t len); /* "COM1", or its device's label */
extern int         char_modem_present(int com);
extern const char *char_modem_name(int com);
extern int         char_modem_get_state(int com);
/* Returns the line (CHAR_MODEM_LINE_*), or -1 with no modem on that port. */
extern int         char_modem_get_line(int com, char *host, size_t host_len, int *port);
/* Takes effect at once, without a reset: a call in progress ends with NO
   CARRIER.  Also saved in the modem's configuration section (a carried
   modem's: its device's). */
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
   can talk (a voice modem on the telephone network).  -1 with no modem on
   that port. */
extern int         char_modem_handset_state(int com);

#ifdef EMU_DEVICE_H
/* A modem's settings, for the config of a device that carries one: the
   telephone line, then (after the device's own "connect_rate") the speaker.
   A COM port modem's config is made of the same, so the two are set alike. */
// clang-format off
#define CHAR_MODEM_CONFIG_LINE                                                          \
    {                                                                                   \
        .name           = "line",                                                       \
        .description    = "Telephone line",                                             \
        .type           = CONFIG_SELECTION,                                             \
        .default_string = NULL,                                                         \
        .default_int    = CHAR_MODEM_LINE_DEAD,                                         \
        .file_filter    = NULL,                                                         \
        .spinner        = { 0 },                                                        \
        .selection      = {                                                             \
            { .description = "Not connected",                  .value = CHAR_MODEM_LINE_DEAD  }, \
            { .description = "Dial out to a TCP/IP host",      .value = CHAR_MODEM_LINE_TCP   }, \
            { .description = "Telephone network (isp-server)", .value = CHAR_MODEM_LINE_PHONE }, \
            { .description = ""                                                         }        \
        },                                                                              \
        .bios           = { { 0 } }                                                     \
    },                                                                                  \
    {                                                                                   \
        .name           = "phone_number",                                               \
        .description    = "Phone number (blank: the exchange gives one)",               \
        .type           = CONFIG_STRING,                                                \
        .default_string = "",                                                           \
        .default_int    = 0,                                                            \
        .file_filter    = NULL,                                                         \
        .spinner        = { 0 },                                                        \
        .selection      = { { 0 } },                                                    \
        .bios           = { { 0 } }                                                     \
    },                                                                                  \
    {                                                                                   \
        .name           = "exchange",                                                   \
        .description    = "Telephone exchange (isp-server)",                            \
        .type           = CONFIG_STRING,                                                \
        .default_string = CHAR_MODEM_EXCHANGE,                                          \
        .default_int    = 0,                                                            \
        .file_filter    = NULL,                                                         \
        .spinner        = { 0 },                                                        \
        .selection      = { { 0 } },                                                    \
        .bios           = { { 0 } }                                                     \
    },                                                                                  \
    {                                                                                   \
        .name           = "host",                                                       \
        .description    = "Host",                                                       \
        .type           = CONFIG_STRING,                                                \
        .default_string = "",                                                           \
        .default_int    = 0,                                                            \
        .file_filter    = NULL,                                                         \
        .spinner        = { 0 },                                                        \
        .selection      = { { 0 } },                                                    \
        .bios           = { { 0 } }                                                     \
    },                                                                                  \
    {                                                                                   \
        .name           = "host_port",                                                  \
        .description    = "Port",                                                       \
        .type           = CONFIG_SPINNER,                                               \
        .default_string = NULL,                                                         \
        .default_int    = 23,                                                           \
        .file_filter    = NULL,                                                         \
        .spinner        = { .min = 1, .max = 32767 },                                   \
        .selection      = { { 0 } },                                                    \
        .bios           = { { 0 } }                                                     \
    }

#define CHAR_MODEM_CONFIG_SPEAKER                                                       \
    {                                                                                   \
        .name           = "speaker",                                                    \
        .description    = "Speaker",                                                    \
        .type           = CONFIG_BINARY,                                                \
        .default_string = NULL,                                                         \
        .default_int    = 1,                                                            \
        .file_filter    = NULL,                                                         \
        .spinner        = { 0 },                                                        \
        .selection      = { { 0 } },                                                    \
        .bios           = { { 0 } }                                                     \
    }
// clang-format on
/* host_port stops at 32767 rather than 65535: device_config_spinner_t is
   int16_t, so a wider maximum wraps negative and the box then takes no value
   at all.  Upstream's own modem stops there too. */
#endif

#ifdef __cplusplus
}
#endif

#endif /* EMU_CHAR_MODEM_H */
