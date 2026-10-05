/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          External Hayes-compatible 56k modems on a COM port: a Diamond
 *          SupraExpress 56e PRO and an ELSA MicroLink 56k.
 *
 *          Ported from PeepeeBox, where these are the modems the funworld
 *          Photo Play cabinets had on COM4 for fun.net.  The cabinet's own
 *          modem database (\FN_SYS\DATABASE\NETWORK\MD_NAME.CSV and
 *          MD_INFOS.CSV) identifies a part from its ATI answers:
 *
 *              ELSA MicroLink 56k             "MicroLink" and "56" in ATI6
 *              Diamond SupraExpress 56e PRO   "SupraExpress" in ATI3
 *
 *          with the firmware read from ATI3 (ELSA) or ATI7 (Supra).  So the
 *          identity a model reports is not decoration: software measures
 *          it, and the answers in the model table below are chosen so that
 *          each matches exactly one part.
 *
 *          There is no telephone network behind the modem.  By default the
 *          line is dead: the modem identifies, accepts init strings, dials,
 *          and reports the failure a real modem would (NO DIALTONE under
 *          X2/X4, or a blind dial that waits out S7 for NO CARRIER).  With a
 *          host configured, dialling any number opens a TCP connection to it
 *          and the modem becomes a transparent pipe -- what a guest PPP
 *          stack wants.  86Box-Next: or the line is "Internet", and any number
 *          reaches the built-in ISP (src/network/isp/): PPP, an address and
 *          DNS for the guest, and a NAT out to the host's Internet, one
 *          session per call.  A call takes as long as a real one: dial tone, the
 *          digits, ringback, then V.34 / V.90 training before CONNECT, all
 *          of it heard on the speaker (modem_sound.c) unless ATM0 or the
 *          device's Speaker option silences it.
 *
 * Authors: The HUEG PP team (PeepeeBox).
 *
 *          Released under the GNU General Public License version 2 or
 *          later.  See COPYING for more information.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/ini.h>
#include <86box/char.h>
#include <86box/log.h>
#include <86box/plat.h>
#include <86box/plat_netsocket.h>
#include <86box/plat_unused.h>
#include <86box/modem_sound.h>
#include <86box/thread.h>
#include <86box/char_modem.h>
#include <86box/isp.h>
#include <86box/pcmcia.h>

#ifdef ENABLE_CHAR_MODEM_LOG
int char_modem_do_log = ENABLE_CHAR_MODEM_LOG;

static void
char_modem_log(void *priv, const char *fmt, ...)
{
    va_list ap;

    if (char_modem_do_log) {
        va_start(ap, fmt);
        log_out(priv, fmt, ap);
        va_end(ap);
    }
}
#else
#    define char_modem_log(priv, fmt, ...)
#endif

#define MODEM_OUT_SIZE 4096 /* modem -> DTE ring */
#define MODEM_CMD_SIZE 256

/* DTE -> line.  The line does not always take a byte the moment the UART
   delivers it (a TCP send can block or go short; the ISP's queue can fill),
   so bytes wait here, in order, and CTS drops while too many do.  The DTE's
   driver may still have a FIFO's worth in flight when it sees CTS fall, and
   the status lines are sampled every 64 character times: the high-water mark
   leaves room for far more than that.  A DTE that sends on regardless runs
   into the end, and the call fails -- loudly, rather than by corrupting the
   PPP stream a byte at a time. */
#define MODEM_TXQ_SIZE 16384
#define MODEM_TXQ_HIGH 8192 /* CTS off       */
#define MODEM_TXQ_LOW  2048 /* CTS on again  */

enum { /* what is on the other side of the RJ11 */
       MODEM_LINE_DEAD = 0, /* nothing: dialling fails the way it would */
       MODEM_LINE_TCP  = 1, /* a TCP host stands in for the whole PSTN */
       MODEM_LINE_ISP  = 2  /* 86Box-Next: the built-in ISP answers any number */
};

enum { /* line state machine */
       MODEM_ST_IDLE = 0,
       MODEM_ST_DIALING,    /* off hook, waiting out the dial  */
       MODEM_ST_CONNECTING, /* the far end is being reached    */
       MODEM_ST_ANSWERING,  /* carrier found: training, then CONNECT */
       MODEM_ST_ONLINE
};

/* A TCP connect to localhost completes in well under a millisecond; a V.34
   handshake takes several seconds.  The gap matters: the cabinet's PPP driver
   (Klos, PPP.EXE) sends ATDT and then reads result lines until it sees its
   CONNECTSTRING, and it also watches DCD.  Hand it DCD and the CONNECT line in
   the same instant and it goes by the carrier, never reads the text, and the
   cabinet logs the call as NO RESPONSE -- PPPMENU's placeholder for "the modem
   never said".  So: wait a plausible while, say CONNECT, let the DTE drain it,
   and only then raise DCD and go on line, which is the order a real modem does
   it in and what the cabinet's own logs show (CONNECT 48000/LAPM, every night
   for a decade). */
#define MODEM_SETTLE_MS  100  /* CONNECT drained to DCD and data mode     */

/* And the rest of a real call is not instant either: dial tone, the digits,
   the far end ringing, then a V.34 / V.90 handshake -- ten to twenty seconds
   in all, which is what the cabinet's own S7=20 allows for.  The modem takes
   that long whether or not its speaker is heard; modem_sound.c plays the same
   schedule it keeps, and is where the durations come from. */

enum { /* result codes, in the numeric order every Hayes modem uses */
       RES_OK = 0,
       RES_CONNECT,
       RES_RING,
       RES_NO_CARRIER,
       RES_ERROR,
       RES_CONNECT_1200,
       RES_NO_DIALTONE,
       RES_BUSY,
       RES_NO_ANSWER,
       RES_NONE = -1
};

static const char *modem_res_text[] = {
    "OK", "CONNECT", "RING", "NO CARRIER", "ERROR",
    "CONNECT 1200", "NO DIALTONE", "BUSY", "NO ANSWER"
};

/* The two modems these cabinets are found with, as MD_NAME.CSV rows 2 and 3.

   `ident` is the string the guest matches on, and `ident_at` says which `ATIn`
   carries it -- the two parts disagree about that, which is exactly why this is
   a table and not a pair of if statements.  `fmw_at` is MD_NAME column 7, the
   command whose answer is filed as MODEMFMW.  `info` is everything else, and a
   NULL there means the modem says nothing to that query.

   Each model's answers were chosen so that **exactly one** row of the guest's
   thirty can match, whichever order it evaluates them in:

     row  3  Diamond SupraExpress 56e PRO  needs {15}         = SupraExpress@I3
     row 14  ...the same part's V.92 variant needs {15, 28}, and 28 is V.92@I3
     row  2  ELSA MicroLink 56k            needs {9, 11}      = MicroLink@I6, 56@I6
     rows 7/18/22 are MicroLink 56k variants needing 9 and 11 *and* one more --
             Internet@I6, -i@I6 + "k i"@I6, pro@I6 -- none of which appear here.

   docs/research/33-modem.md walks all thirty. */
enum {
    MODEM_MODEL_SUPRA = 0,
    MODEM_MODEL_ELSA  = 1,
    MODEM_MODEL_3C562 = 2  /* 86Box-Next: the 3Com 3C562D PC Card's modem */
};

/* 86Box-Next: in a device's local, a modem that is no COM port's -- a PC
   Card's, opened with char_open_unlisted() inside the card's device context
   (instance = socket + 1), whose settings are the card's.  The status bar
   finds it in the socket's slot of modems[]. */
#define MODEM_UNLISTED 0x100

typedef struct {
    const char *name;        /* MD_NAME.CSV, verbatim                        */
    int         ident_at;    /* which ATIn carries the identification string */
    const char *ident;       /* ...and its default text                      */
    int         fmw_at;      /* MD_NAME column 7, filed as MODEMFMW          */
    const char *fmw;         /* ...and its default text                      */
    int         country_at;  /* which ATIn reports the country code          */
    const char *info[10];    /* the rest of ATI0..ATI9                       */
} modem_model_t;

static const modem_model_t modem_models[] = {
    // clang-format off
    [MODEM_MODEL_SUPRA] = {
        .name       = "Diamond SupraExpress 56e PRO",
        .ident_at   = 3, .ident = "SupraExpress 56e PRO",
        .fmw_at     = 7, .fmw   = "V1.100-V90_2M_DLS",
        .country_at = 5,
        .info       = {
            [0] = "1794",                                    /* product code */
            [1] = "168",                                     /* ROM checksum */
            [2] = "OK",                                      /* RAM test     */
            [4] = "Diamond Multimedia SupraExpress 56e PRO",
            [6] = "RCVDL56ACF/SP Rev 1.100"                  /* the chipset  */
        }
    },
    [MODEM_MODEL_ELSA] = {
        .name       = "ELSA MicroLink 56k",
        .ident_at   = 6, .ident = "ELSA MicroLink 56k",
        .fmw_at     = 3, .fmw   = "2.274",
        .country_at = 5,
        .info       = {
            [0] = "1442",
            [1] = "168",
            [2] = "OK",
            [4] = "ELSA MicroLink 56k",
            [7] = "ELSA AG, Aachen"
        }
    },
    /* 86Box-Next: the modem function of the 3Com 3C562D/3C563D LAN+33.6
       Modem PC Card (src/pcmcia/pccard_3c562.c).  A Rockwell V.34 data
       pump; the answers are plausible ones, nothing reads them. */
    [MODEM_MODEL_3C562] = {
        .name       = "3Com 3C562D LAN+33.6 Modem",
        .ident_at   = 3, .ident = "3Com 3C562D/3C563D 33.6 Modem",
        .fmw_at     = 7, .fmw   = "V2.31",
        .country_at = 5,
        .info       = {
            [0] = "33600",
            [1] = "255",
            [2] = "OK",
            [4] = "3Com EtherLink III LAN+33.6 Modem PC Card",
            [6] = "RC336ACi"
        }
    }
    // clang-format on
};

/* S-register power-on values.  Rockwell defaults; the cabinet overwrites S7,
   S8 and S10 in its own init string and leaves the rest alone. */
static const uint8_t modem_s_defaults[100] = {
    [0] = 0,   /* rings to auto-answer on -- 0 is "never"     */
    [2] = 43,  /* escape character, '+'                        */
    [3] = 13,  /* carriage return                              */
    [4] = 10,  /* line feed                                    */
    [5] = 8,   /* backspace                                    */
    [6] = 2,   /* wait before blind dial, seconds              */
    [7] = 50,  /* wait for carrier after dial, seconds         */
    [8] = 2,   /* comma pause, seconds                         */
    [9] = 6,   /* carrier detect response time, tenths         */
    [10] = 14, /* carrier loss disconnect delay, tenths        */
    [11] = 95, /* DTMF duration, ms                            */
    [12] = 50  /* escape guard time, 20 ms units               */
};

typedef struct {
    void        *log;
    char_port_t *port;

    /* Snapshot of the device config.  device_get_config_*() only works inside
       init(); read()/write() run much later off the serial port's timers.  Same
       reasoning as char_fujinet.c. */
    const modem_model_t *model;
    int  line;
    char host[128];
    int  host_port;
    char ident[64];    /* the identification string, at model->ident_at    */
    char firmware[64]; /* what the cabinet files as MODEMFMW               */
    int  connect_rate;

    /* DTE-facing output ring. */
    uint8_t  out[MODEM_OUT_SIZE];
    uint32_t out_head;
    uint32_t out_tail;

    /* Command line being collected. */
    char     cmd[MODEM_CMD_SIZE];
    char     lastcmd[MODEM_CMD_SIZE];
    uint32_t cmdlen;

    /* Configuration the DTE has set. */
    uint8_t s[100];
    int     echo;
    int     verbose;
    int     quiet;
    int     xresult;  /* ATX -- how much of the result set is in use      */
    int     wresult;  /* ATW -- whose speed CONNECT reports               */
    int     dcd_mode; /* AT&C                                             */
    int     dsr_mode; /* AT&S                                             */
    int     dtr_mode; /* AT&D                                             */
    int     flow;     /* AT&K                                             */
    int     country;  /* AT*NC / AT+GCI                                   */
    int     online;   /* data mode rather than command mode               */

    /* Line side.  `call` is the far end of the call in progress, NULL with
       none: a TCP host (`sock`) or an ISP session (`isp`). */
    int                           state;
    const struct modem_line_ops  *call;
    SOCKET                        sock;
    isp_session_t                *isp;
    uint32_t deadline; /* plat_get_ticks() value the current wait ends at  */
    int      said_connect; /* ANSWERING: the CONNECT line has been queued  */
    int      dtr;

    /* +++ escape detection. */
    uint32_t last_data;
    int      pluses;

    /* DTE -> line queue, and whether it has CTS held off. */
    uint8_t  txq[MODEM_TXQ_SIZE];
    uint32_t txq_head;
    uint32_t txq_tail;
    int      cts_held;

    /* The speaker, and the schedule of a real call. */
    modem_sound_t *snd;
    int            spk_mode;   /* ATM: 0 off, 1 until carrier, 2 always, 3 after dialling */
    int            spk_level;  /* ATL: 0..3                                   */
    int            pulse;      /* ATP / ATT                                   */
    uint32_t       answer_at;  /* when the far end picks up                   */
    int            refused;    /* the connect failed: ring on, then give up   */

    /* Where it is plugged in, so the status bar can find it (slot: a COM port,
       or SERIAL_MAX + a PC Card socket), and the device section its settings
       are written back to: its own for a COM port modem, the card's for a PC
       Card's. */
    int             slot;      /* -1: none */
    const device_t *cfg_dev;
    int             cfg_inst;

    /* The line as configured (`line` above is what is in effect: a TCP line
       with no host is dead), and a change from the status bar waiting to be
       taken up on the emulation thread.  Both under modem_mutex. */
    int  cfg_line;
    int  pending;
    int  pend_line;
    char pend_host[128];
    int  pend_port;
} modem_t;

/* ------------------------------------------------------------------ output */

static void
modem_out_byte(modem_t *dev, uint8_t val)
{
    uint32_t next = (dev->out_head + 1) % MODEM_OUT_SIZE;

    if (next == dev->out_tail)
        return; /* ring full: a real modem would have asserted flow control */
    dev->out[dev->out_head] = val;
    dev->out_head           = next;
}

static void
modem_out_str(modem_t *dev, const char *s)
{
    while (*s != '\0')
        modem_out_byte(dev, (uint8_t) *s++);
}

/* One information line, the way a verbose modem frames it. */
static void
modem_out_line(modem_t *dev, const char *s)
{
    modem_out_byte(dev, dev->s[3]);
    modem_out_byte(dev, dev->s[4]);
    modem_out_str(dev, s);
    modem_out_byte(dev, dev->s[3]);
    modem_out_byte(dev, dev->s[4]);
}

static void
modem_result(modem_t *dev, int code)
{
    char buf[64];

    if ((code == RES_NONE) || dev->quiet)
        return;

    /* X0 collapses everything the extended set added back onto the basic four. */
    if (dev->xresult == 0) {
        switch (code) {
            case RES_NO_DIALTONE:
            case RES_BUSY:
            case RES_NO_ANSWER:
                code = RES_NO_CARRIER;
                break;
            default:
                break;
        }
    } else {
        /* Dial tone detection is only in X2 and X4; busy detection in X3 and X4. */
        if ((code == RES_NO_DIALTONE) && (dev->xresult != 2) && (dev->xresult != 4))
            code = RES_NO_CARRIER;
        if ((code == RES_BUSY) && (dev->xresult < 3))
            code = RES_NO_CARRIER;
    }

    if (code == RES_CONNECT) {
        /* X0 says CONNECT and nothing else; anything above reports a speed,
           and W2 makes that the carrier's rather than the port's. */
        if (dev->xresult == 0)
            snprintf(buf, sizeof(buf), "CONNECT");
        else if (dev->wresult == 2)
            snprintf(buf, sizeof(buf), "CONNECT %d/V90", dev->connect_rate);
        else
            snprintf(buf, sizeof(buf), "CONNECT %d", dev->connect_rate);
    } else
        snprintf(buf, sizeof(buf), "%s", modem_res_text[code]);

    if (dev->verbose)
        modem_out_line(dev, buf);
    else {
        char num[8];

        snprintf(num, sizeof(num), "%d", code);
        modem_out_str(dev, num);
        modem_out_byte(dev, dev->s[3]);
    }

    char_modem_log(dev->log, "-> %s\n", buf);
}

/* ------------------------------------------------------------------- line */

/* The far end of a call.  Whatever it is, the call itself -- dialling,
   ringing, training, CONNECT, DCD, hanging up -- is the modem's. */
typedef struct modem_line_ops {
    int (*open)(modem_t *dev);      /* 0: reaching it; -1: nobody there    */
    int (*connected)(modem_t *dev); /* 1: answered; 0: not yet; -1: failed */
    /* Bytes taken, or -1 with *wouldblock set (try again) or clear (gone). */
    int (*send)(modem_t *dev, const uint8_t *buf, int len, int *wouldblock);
    /* Bytes, 0 when the far end has hung up, or -1 as above. */
    int (*recv)(modem_t *dev, uint8_t *buf, int len, int *wouldblock);
    void (*close)(modem_t *dev);
} modem_line_ops_t;

/* A TCP host. */
static int
modem_tcp_open(modem_t *dev)
{
    dev->sock = plat_netsocket_create(NET_SOCKET_TCP);
    if (!CHAR_FD_VALID(dev->sock) ||
        (plat_netsocket_connect(dev->sock, dev->host, (unsigned short) dev->host_port) != 0)) {
        if (CHAR_FD_VALID(dev->sock)) {
            plat_netsocket_close(dev->sock);
            dev->sock = (SOCKET) -1;
        }
        return -1;
    }
    return 0;
}

static int
modem_tcp_connected(modem_t *dev)
{
    return plat_netsocket_connected(dev->sock);
}

static int
modem_tcp_send(modem_t *dev, const uint8_t *buf, int len, int *wouldblock)
{
    *wouldblock = 0;
    return plat_netsocket_send(dev->sock, buf, (unsigned int) len, wouldblock);
}

static int
modem_tcp_recv(modem_t *dev, uint8_t *buf, int len, int *wouldblock)
{
    *wouldblock = 0;
    return plat_netsocket_receive(dev->sock, buf, (unsigned int) len, wouldblock);
}

static void
modem_tcp_close(modem_t *dev)
{
    if (CHAR_FD_VALID(dev->sock)) {
        plat_netsocket_close(dev->sock);
        dev->sock = (SOCKET) -1;
    }
}

static const modem_line_ops_t modem_line_tcp = {
    .open      = modem_tcp_open,
    .connected = modem_tcp_connected,
    .send      = modem_tcp_send,
    .recv      = modem_tcp_recv,
    .close     = modem_tcp_close
};

/* 86Box-Next: the built-in ISP.  A session is there the moment it is opened;
   it says nothing until the guest starts PPP, which is after CONNECT.  Its
   log lines come from its own thread. */
static void
modem_isp_log(void *opaque, const char *msg)
{
    modem_t *dev = (modem_t *) opaque;

    (void) dev; /* the log call compiles away when logging is off */
    (void) msg;
    char_modem_log(dev->log, "%s\n", msg);
}

static int
modem_isp_open(modem_t *dev)
{
    const isp_session_callbacks_t cb = { NULL, modem_isp_log, dev };
    char                          err[128];

    dev->isp = isp_session_open(&cb, err, sizeof(err));
    if (dev->isp == NULL) {
        char_modem_log(dev->log, "ISP: %s\n", err);
        return -1;
    }
    return 0;
}

static int
modem_isp_connected(modem_t *dev)
{
    return (dev->isp != NULL) ? 1 : -1;
}

static int
modem_isp_send(modem_t *dev, const uint8_t *buf, int len, int *wouldblock)
{
    const size_t n = isp_session_write(dev->isp, buf, (size_t) len);

    *wouldblock = (n == 0);
    return (n == 0) ? -1 : (int) n;
}

static int
modem_isp_recv(modem_t *dev, uint8_t *buf, int len, int *wouldblock)
{
    const size_t n = isp_session_read(dev->isp, buf, (size_t) len);

    *wouldblock = 0;
    if (n > 0)
        return (int) n;
    if (isp_session_ended(dev->isp))
        return 0; /* PPP is over: the ISP hangs up */
    *wouldblock = 1;
    return -1;
}

static void
modem_isp_close(modem_t *dev)
{
    isp_session_close(dev->isp);
    dev->isp = NULL;
}

static const modem_line_ops_t modem_line_isp = {
    .open      = modem_isp_open,
    .connected = modem_isp_connected,
    .send      = modem_isp_send,
    .recv      = modem_isp_recv,
    .close     = modem_isp_close
};

#ifdef ENABLE_CHAR_MODEM_LOG
/* For the log, which is all that names the line. */
static const char *
modem_line_name(const modem_t *dev)
{
    switch (dev->line) {
        case MODEM_LINE_TCP:
            return dev->host;
        case MODEM_LINE_ISP:
            return "the built-in ISP";
        default:
            return "dead";
    }
}
#endif

static uint32_t
modem_txq_used(const modem_t *dev)
{
    return (dev->txq_head - dev->txq_tail + MODEM_TXQ_SIZE) % MODEM_TXQ_SIZE;
}

/* CTS, with hysteresis, from how full the queue to the line is. */
static void
modem_txq_cts(modem_t *dev)
{
    const uint32_t used = modem_txq_used(dev);

    if (!dev->cts_held && (used >= MODEM_TXQ_HIGH)) {
        dev->cts_held = 1;
        char_update_status(dev->port);
    } else if (dev->cts_held && (used <= MODEM_TXQ_LOW)) {
        dev->cts_held = 0;
        char_update_status(dev->port);
    }
}

static void
modem_hangup(modem_t *dev)
{
    if (dev->state != MODEM_ST_IDLE)
        modem_sound_event(dev->snd, MODEM_SOUND_HANGUP, NULL, 0);
    if (dev->call != NULL) {
        dev->call->close(dev);
        dev->call = NULL;
    }
    dev->state    = MODEM_ST_IDLE;
    dev->online   = 0;
    dev->pluses   = 0;
    dev->txq_head = 0;
    dev->txq_tail = 0;
    dev->cts_held = 0;
    char_update_status(dev->port);
}

/* The carrier is gone: hang up and say so. */
static void
modem_carrier_lost(modem_t *dev, const char *why)
{
    (void) why;
    char_modem_log(dev->log, "carrier lost: %s\n", why);
    modem_hangup(dev);
    modem_result(dev, RES_NO_CARRIER);
}

/* Hand the line what it will take of the queue, in order.  A short send
   advances by what went; a full line is tried again on the next poll. */
static void
modem_txq_flush(modem_t *dev)
{
    while ((dev->call != NULL) && (dev->state == MODEM_ST_ONLINE) && (dev->txq_tail != dev->txq_head)) {
        const uint32_t chunk      = (dev->txq_head > dev->txq_tail) ? (dev->txq_head - dev->txq_tail)
                                                                    : (MODEM_TXQ_SIZE - dev->txq_tail);
        int            wouldblock = 0;
        const int      ret        = dev->call->send(dev, &dev->txq[dev->txq_tail], (int) chunk, &wouldblock);

        if (ret > 0)
            dev->txq_tail = (dev->txq_tail + (uint32_t) ret) % MODEM_TXQ_SIZE;
        else if ((ret == 0) || wouldblock)
            break; /* the line is full for now */
        else {
            modem_carrier_lost(dev, "send failed");
            return;
        }
    }
    modem_txq_cts(dev);
}

/* One byte for the line, behind the ones already waiting. */
static void
modem_txq_put(modem_t *dev, uint8_t val)
{
    const uint32_t next = (dev->txq_head + 1) % MODEM_TXQ_SIZE;

    if (dev->call == NULL)
        return; /* no call: the byte goes nowhere, as down a dead line */
    if (next == dev->txq_tail) {
        /* The DTE has sent this many bytes past CTS: end the call rather
           than drop one and corrupt the stream. */
        modem_carrier_lost(dev, "the DTE ignored CTS and overran the transmit queue");
        return;
    }
    dev->txq[dev->txq_head] = val;
    dev->txq_head           = next;
}

/* The carrier is up: start the CONNECT sequence described above. */
static void
modem_answered(modem_t *dev)
{
    dev->state        = MODEM_ST_ANSWERING;
    dev->said_connect = 0;
    dev->deadline     = plat_get_ticks() + modem_sound_handshake_ms(dev->connect_rate > 33600);
    modem_sound_event(dev->snd, MODEM_SOUND_ANSWER, NULL, 0);
}

/* CONNECT has been read: raise DCD, go on line. */
static void
modem_online(modem_t *dev)
{
    dev->state  = MODEM_ST_ONLINE;
    dev->online = 1;
    char_update_status(dev->port);
}

/* Everything a dialled string can contain that is not a digit: the dial
   modifiers.  The number itself is never used -- there is one host. */
static void
modem_dial(modem_t *dev, const char *number)
{
    uint32_t now = plat_get_ticks();

    /* There is one line: the number goes nowhere, but dialling it takes the
       time it takes -- tone or pulse, digit by digit, a comma for S8 -- and
       the far end answers after its first ring. */
    char_modem_log(dev->log, "dial \"%s\"\n", number);
    dev->refused   = 0;
    dev->answer_at = now + modem_sound_dial_ms(number, dev->s[8], dev->pulse) + modem_sound_ring_ms();
    if (dev->line != MODEM_LINE_DEAD) {
        modem_sound_country(dev->snd, (dev->country == 16) || (dev->country == 0xB4), dev->connect_rate > 33600);
        modem_sound_event(dev->snd, MODEM_SOUND_DIAL, number, (dev->s[8] & 0xff) | (dev->pulse << 8));
    }

    if (dev->line != MODEM_LINE_DEAD) {
        const modem_line_ops_t *ops = (dev->line == MODEM_LINE_ISP) ? &modem_line_isp : &modem_line_tcp;

        /* The ISP answers any number, but there has to be one: a bare ATD
           rings nowhere. */
        if (((dev->line == MODEM_LINE_ISP) && (strpbrk(number, "0123456789*#") == NULL)) ||
            (ops->open(dev) != 0)) {
            dev->state    = MODEM_ST_DIALING;
            dev->deadline = dev->answer_at + 5000;   /* it rings, and nobody answers */
            return;
        }
        dev->call     = ops;
        dev->state    = MODEM_ST_CONNECTING;
        dev->deadline = now + (dev->s[7] * 1000u);
        return;
    }

    /* No line.  A modem that can hear no dial tone says so within a second or
       two; one that cannot listen for it (X0/X1/X3) dials blind and waits out
       S7 for a carrier that never comes. */
    dev->state    = MODEM_ST_DIALING;
    dev->deadline = now + (((dev->xresult == 2) || (dev->xresult == 4))
                               ? 1500u
                               : (dev->s[7] * 1000u));
}

static void
modem_poll_line(modem_t *dev)
{
    uint32_t now = plat_get_ticks();

    switch (dev->state) {
        case MODEM_ST_DIALING:
            if ((int32_t) (now - dev->deadline) < 0)
                break;
            modem_hangup(dev);
            modem_result(dev, ((dev->line == MODEM_LINE_DEAD) &&
                               ((dev->xresult == 2) || (dev->xresult == 4)))
                                  ? RES_NO_DIALTONE
                                  : RES_NO_CARRIER);
            break;

        case MODEM_ST_CONNECTING: {
            const int connected = dev->refused ? -1 : dev->call->connected(dev);

            if (connected == 1) {
                /* The far end is there; it picks up after its first ring. */
                if ((int32_t) (now - dev->answer_at) >= 0)
                    modem_answered(dev);
            } else if (connected == -1) {
                /* Nobody there: a real call rings on unanswered, then gives up. */
                if (!dev->refused) {
                    dev->refused  = 1;
                    dev->deadline = ((int32_t) (dev->answer_at - now) > 0 ? dev->answer_at : now) + 5000;
                }
                if ((int32_t) (now - dev->deadline) >= 0) {
                    modem_hangup(dev);
                    modem_result(dev, RES_NO_CARRIER);
                }
            } else if ((int32_t) (now - dev->deadline) >= 0) {
                modem_hangup(dev);
                modem_result(dev, RES_NO_CARRIER);
            }
            break;
        }

        case MODEM_ST_ANSWERING:
            if ((int32_t) (now - dev->deadline) < 0)
                break;
            if (!dev->said_connect) {
                modem_sound_event(dev->snd, MODEM_SOUND_CONNECT, NULL, 0);
                modem_result(dev, RES_CONNECT);
                dev->said_connect = 1;
                dev->deadline     = now + MODEM_SETTLE_MS;
            } else if (dev->out_tail == dev->out_head)
                modem_online(dev); /* the DTE has read the line */
            break;

        default:
            break;
    }
}

/* ---------------------------------------------------------------- commands */

/* Read a decimal argument, defaulting to 0 the way every AT command does. */
static int
modem_arg(const char **p)
{
    int val = 0;

    while (**p == ' ')
        (*p)++;
    if (!isdigit((unsigned char) **p))
        return 0;
    while (isdigit((unsigned char) **p))
        val = (val * 10) + (*(*p)++ - '0');
    return val;
}

/* The `ATIn` responses.  Every one of these is load-bearing: this is how the
   cabinet decides which of thirty modems it is looking at, so a stray substring
   would make it report the wrong one.  See the model table above. */
static void
modem_info(modem_t *dev, int n)
{
    char buf[80];

    if ((n < 0) || (n > 9))
        return;

    if (n == dev->model->ident_at)
        modem_out_line(dev, dev->ident);
    else if (n == dev->model->fmw_at)
        modem_out_line(dev, dev->firmware);
    else if (n == dev->model->country_at) {
        snprintf(buf, sizeof(buf), "Country Code: %02X", dev->country);
        modem_out_line(dev, buf);
    } else if (dev->model->info[n] != NULL)
        modem_out_line(dev, dev->model->info[n]);
}

static void
modem_load_defaults(modem_t *dev)
{
    memcpy(dev->s, modem_s_defaults, sizeof(dev->s));
    dev->echo     = 1;
    dev->verbose  = 1;
    dev->quiet    = 0;
    dev->xresult  = 4;
    dev->wresult  = 0;
    dev->dcd_mode = 1;
    dev->dsr_mode = 0;
    dev->dtr_mode = 2;
    dev->flow     = 3;
    dev->spk_mode  = 1;
    dev->spk_level = 2;
    dev->pulse     = 0;
    modem_sound_speaker(dev->snd, dev->spk_mode, dev->spk_level);
}

/* Returns the result code to report, or RES_NONE when the command has already
   answered for itself (a dial in progress, an info line). */
static int
modem_execute(modem_t *dev, const char *line)
{
    const char *p = line;

    while (*p != '\0') {
        const char c = (char) toupper((unsigned char) *p++);

        switch (c) {
            case ' ':
                break;

            case 'A': /* answer -- there is never an incoming call */
                return RES_NO_CARRIER;

            case 'D': { /* dial */
                char number[64];
                int  n = 0;

                if (dev->state != MODEM_ST_IDLE)
                    return RES_ERROR;
                while ((*p != '\0') && (n < ((int) sizeof(number) - 1))) {
                    const char d = *p++;

                    /* The digits, and what shapes the dialling: a comma (wait S8),
                       W (wait for dial tone), T and P (tone or pulse). */
                    if (isdigit((unsigned char) d) || (d == '*') || (d == '#') || (d == ',') ||
                        (strchr("WwTtPp", d) != NULL))
                        number[n++] = d;
                }
                number[n] = '\0';
                modem_dial(dev, number);
                return RES_NONE;
            }

            case 'E':
                dev->echo = !!modem_arg(&p);
                break;

            case 'H':
                if (modem_arg(&p) == 0)
                    modem_hangup(dev);
                break;

            case 'I':
                modem_info(dev, modem_arg(&p));
                break;

            case 'O': /* back to data mode */
                if (dev->state != MODEM_ST_ONLINE)
                    return RES_ERROR;
                (void) modem_arg(&p);
                dev->online = 1;
                return RES_NONE;

            case 'Q':
                dev->quiet = !!modem_arg(&p);
                break;

            case 'S': { /* S-register read or write */
                const int reg = modem_arg(&p);

                if (reg >= 100)
                    return RES_ERROR;
                while (*p == ' ')
                    p++;
                if (*p == '=') {
                    p++;
                    dev->s[reg] = (uint8_t) modem_arg(&p);
                } else if (*p == '?') {
                    char buf[8];

                    p++;
                    snprintf(buf, sizeof(buf), "%03d", dev->s[reg]);
                    modem_out_line(dev, buf);
                } else
                    return RES_ERROR;
                break;
            }

            case 'V':
                dev->verbose = !!modem_arg(&p);
                break;

            case 'W':
                dev->wresult = modem_arg(&p);
                break;

            case 'X':
                dev->xresult = modem_arg(&p);
                break;

            case 'Z': /* reset and load a stored profile */
                (void) modem_arg(&p);
                modem_hangup(dev);
                modem_load_defaults(dev);
                break;

            /* The speaker: M is when it is on, L how loud.  The cabinets send
               M1L3 -- on until the carrier, loud. */
            case 'L':
                dev->spk_level = modem_arg(&p);
                modem_sound_speaker(dev->snd, dev->spk_mode, dev->spk_level);
                break;
            case 'M':
                dev->spk_mode = modem_arg(&p);
                modem_sound_speaker(dev->snd, dev->spk_mode, dev->spk_level);
                break;

            /* Dial mode, for how long the dialling takes. */
            case 'P':
                dev->pulse = 1;
                break;
            case 'T':
                dev->pulse = 0;
                break;

            /* Modulation selection and error control: accepted, nothing to do. */
            case 'B':
            case 'N':
                (void) modem_arg(&p);
                break;

            case '&': { /* the ampersand set */
                const char a = (char) toupper((unsigned char) *p++);

                switch (a) {
                    case 'C':
                        dev->dcd_mode = modem_arg(&p);
                        char_update_status(dev->port);
                        break;
                    case 'D':
                        dev->dtr_mode = modem_arg(&p);
                        break;
                    case 'K':
                        dev->flow = modem_arg(&p);
                        break;
                    case 'S':
                        dev->dsr_mode = modem_arg(&p);
                        char_update_status(dev->port);
                        break;
                    case 'F': /* load factory profile n */
                        (void) modem_arg(&p);
                        modem_load_defaults(dev);
                        break;
                    /* Stored profiles: there is no NVRAM behind these, but the
                       cabinet writes one (`AT&F2&W`) on every country change and
                       an ERROR there would abort its init. */
                    case 'W':
                    case 'Y':
                    case 'Z':
                        (void) modem_arg(&p);
                        if (*p == '=')
                            while ((*p != '\0') && (*p != ';'))
                                p++;
                        break;
                    case 'V': /* view active profile */
                        modem_out_line(dev, dev->ident);
                        break;
                    case '\0':
                        return RES_ERROR;
                    default:
                        (void) modem_arg(&p);
                        break;
                }
                break;
            }

            case '\\': /* error control and flow control, Microcom style */
            case '%':  /* Rockwell extended parameters, e.g. AT%C3 */
            case '-':  /* -J and friends */
                if (*p == '\0')
                    return RES_ERROR;
                p++;
                (void) modem_arg(&p);
                break;

            case '*': { /* Rockwell country select: AT*NC<n> */
                const char a = (char) toupper((unsigned char) *p);

                if (a == '\0')
                    return RES_ERROR;
                p++;
                if (toupper((unsigned char) *p) == 'C')
                    p++;
                if (a == 'N')
                    dev->country = modem_arg(&p);
                else
                    (void) modem_arg(&p);
                break;
            }

            case '+': { /* the ITU extended set */
                char name[16];
                int  n = 0;

                while ((*p != '\0') && (*p != '=') && (*p != '?') &&
                       (n < ((int) sizeof(name) - 1)))
                    name[n++] = (char) toupper((unsigned char) *p++);
                name[n] = '\0';

                if (!strcmp(name, "GCI")) {
                    /* Country of installation, T.35 code.  The cabinet uses this
                       on the ELSA and Conexant parts rather than on this one. */
                    if (*p == '?') {
                        char buf[16];

                        p++;
                        snprintf(buf, sizeof(buf), "+GCI: %02X", dev->country);
                        modem_out_line(dev, buf);
                    } else if (*p == '=') {
                        char *end = NULL;

                        dev->country = (int) strtol(p + 1, &end, 16);
                        p            = end;
                    } else
                        return RES_ERROR;
                } else if (!strcmp(name, "MS") || !strcmp(name, "FCLASS") ||
                           !strcmp(name, "IFC") || !strcmp(name, "ES")) {
                    /* Modulation, fax class and flow control: accepted whole. */
                    while ((*p != '\0') && (*p != ';'))
                        p++;
                } else
                    return RES_ERROR;
                break;
            }

            default:
                return RES_ERROR;
        }
    }

    return RES_OK;
}

static void
modem_command(modem_t *dev, const char *line)
{
    int res;

    char_modem_log(dev->log, "<- AT%s\n", line);

    /* A/ re-runs the stored line, so this can be handed its own buffer -- hence
       memmove rather than strncpy. */
    {
        size_t n = strlen(line);

        if (n > (sizeof(dev->lastcmd) - 1))
            n = sizeof(dev->lastcmd) - 1;
        memmove(dev->lastcmd, line, n);
        dev->lastcmd[n] = '\0';
    }

    res = modem_execute(dev, line);
    modem_result(dev, res);
}

/* --------------------------------------------------------------- DTE input */

static void
modem_command_byte(modem_t *dev, uint8_t val)
{
    /* Backspace edits the line, and is echoed as destructive. */
    if (val == dev->s[5]) {
        if (dev->cmdlen > 0) {
            dev->cmdlen--;
            if (dev->echo) {
                modem_out_byte(dev, dev->s[5]);
                modem_out_byte(dev, ' ');
                modem_out_byte(dev, dev->s[5]);
            }
        }
        return;
    }

    if (dev->echo && (val != dev->s[4]))
        modem_out_byte(dev, val);

    if (val == dev->s[3]) {
        dev->cmd[dev->cmdlen] = '\0';

        /* A/ repeats the last line and takes no terminator; a bare CR, which is
           how the cabinet wakes the port up, is not a command at all. */
        if (dev->cmdlen >= 2) {
            if ((toupper((unsigned char) dev->cmd[0]) == 'A') &&
                (toupper((unsigned char) dev->cmd[1]) == 'T'))
                modem_command(dev, &dev->cmd[2]);
            else
                modem_result(dev, RES_ERROR);
        } else if (dev->cmdlen > 0)
            modem_result(dev, RES_ERROR);

        dev->cmdlen = 0;
        return;
    }

    if (val == dev->s[4])
        return; /* LF inside a command line is ignored */

    /* A/ has no CR: it fires the moment the slash arrives. */
    if ((dev->cmdlen == 1) && (val == '/') &&
        (toupper((unsigned char) dev->cmd[0]) == 'A')) {
        dev->cmdlen = 0;
        modem_command(dev, dev->lastcmd);
        return;
    }

    if (dev->cmdlen < (MODEM_CMD_SIZE - 1))
        dev->cmd[dev->cmdlen++] = (char) val;
}

static void
modem_data_byte(modem_t *dev, uint8_t val)
{
    const uint32_t now   = plat_get_ticks();
    const uint32_t guard = dev->s[12] * 20u;

    /* The escape sequence: three copies of S2 with a guard time either side and
       nothing else in between. */
    if ((val == dev->s[2]) && (dev->pluses < 3) &&
        ((dev->pluses > 0) || ((now - dev->last_data) >= guard))) {
        dev->pluses++;
        dev->last_data = now;
        return;
    }
    /* A fourth falls through: it and the three held back are data. */

    if (dev->pluses > 0) {
        /* Broken by other traffic -- the pluses were data after all, and go
           ahead of this byte. */
        const int n = dev->pluses;

        dev->pluses = 0;
        for (int i = 0; i < n; i++)
            modem_txq_put(dev, dev->s[2]);
        if (!dev->online)
            return; /* the call failed under them */
    }

    dev->last_data = now;
    modem_txq_put(dev, val);
}

/* ------------------------------------------------- the line, from the UI */

/* The modems plugged in, by slot: the COM ports, then the PC Card sockets.
   The UI thread reads and queues; the emulation thread opens, closes and
   applies. */
#define MODEM_SLOTS (SERIAL_MAX + PCMCIA_SOCKETS)
static modem_t *modems[MODEM_SLOTS];
static mutex_t *modem_mutex = NULL;

static void
modem_lock(void)
{
    if (modem_mutex != NULL)
        thread_wait_mutex(modem_mutex);
}

static void
modem_unlock(void)
{
    if (modem_mutex != NULL)
        thread_release_mutex(modem_mutex);
}

static void
modem_set_line(modem_t *dev, int line, const char *host, int port)
{
    dev->cfg_line  = line;
    dev->host_port = port;
    snprintf(dev->host, sizeof(dev->host), "%s", host);

    /* A host with nothing in it is a dead line however the selector is set --
       otherwise every dial would stall on a connect to port 0.  The ISP needs
       no host. */
    if (line == MODEM_LINE_ISP)
        dev->line = MODEM_LINE_ISP;
    else
        dev->line = ((line == MODEM_LINE_TCP) && (dev->host[0] != '\0')) ? MODEM_LINE_TCP : MODEM_LINE_DEAD;
}

/* Take up a line change from the status bar.  A call in progress is on the
   old line, so it ends -- the way unplugging the phone cord ends it. */
static void
modem_apply_pending(modem_t *dev)
{
    char host[sizeof(dev->pend_host)];
    int  changed;

    if (!dev->pending)
        return;

    modem_lock();
    memcpy(host, dev->pend_host, sizeof(host));
    changed = (dev->pend_line != dev->cfg_line) || (dev->pend_port != dev->host_port) ||
              strcmp(host, dev->host);
    modem_set_line(dev, dev->pend_line, host, dev->pend_port);
    dev->pending = 0;
    modem_unlock();

    char_modem_log(dev->log, "line now %s\n", modem_line_name(dev));
    if (changed && (dev->state != MODEM_ST_IDLE)) {
        modem_hangup(dev);
        modem_result(dev, RES_NO_CARRIER);
    }
}

int
char_modem_slots(void)
{
    return MODEM_SLOTS;
}

/* "COM1", or "PC Card A" for a PC Card's modem. */
void
char_modem_slot_label(int com, char *buf, size_t len)
{
    if (com < SERIAL_MAX)
        snprintf(buf, len, "COM%d", com + 1);
    else
        snprintf(buf, len, "PC Card %c", 'A' + (com - SERIAL_MAX));
}

int
char_modem_present(int com)
{
    return (com >= 0) && (com < MODEM_SLOTS) && (modems[com] != NULL);
}

const char *
char_modem_name(int com)
{
    const char *ret = NULL;

    modem_lock();
    if (char_modem_present(com))
        ret = modems[com]->model->name;
    modem_unlock();
    return ret;
}

int
char_modem_get_line(int com, char *host, size_t host_len, int *port)
{
    int line = -1;

    modem_lock();
    if (char_modem_present(com)) {
        const modem_t *dev = modems[com];

        line = dev->pending ? dev->pend_line : dev->cfg_line;
        if (host != NULL)
            snprintf(host, host_len, "%s", dev->pending ? dev->pend_host : dev->host);
        if (port != NULL)
            *port = dev->pending ? dev->pend_port : dev->host_port;
    }
    modem_unlock();
    return line;
}

int
char_modem_get_state(int com)
{
    int ret = CHAR_MODEM_ABSENT;

    modem_lock();
    if (char_modem_present(com)) {
        switch (modems[com]->state) {
            case MODEM_ST_IDLE:
                ret = CHAR_MODEM_IDLE;
                break;
            case MODEM_ST_ONLINE:
                ret = CHAR_MODEM_ONLINE;
                break;
            default:
                ret = CHAR_MODEM_CALLING;
                break;
        }
    }
    modem_unlock();
    return ret;
}

void
char_modem_set_line(int com, int line, const char *host, int port)
{
    modem_lock();
    if (char_modem_present(com)) {
        modem_t *dev = modems[com];

        if (host == NULL)
            host = "";
        if ((port < 1) || (port > 65535))
            port = 23;
        switch (line) {
            case CHAR_MODEM_LINE_TCP:
                dev->pend_line = MODEM_LINE_TCP;
                break;
            case CHAR_MODEM_LINE_ISP:
                dev->pend_line = MODEM_LINE_ISP;
                break;
            default:
                dev->pend_line = MODEM_LINE_DEAD;
                break;
        }
        dev->pend_port = port;
        snprintf(dev->pend_host, sizeof(dev->pend_host), "%s", host);
        dev->pending = 1;

        /* Into its own section, so it is there after a restart and in the
           Settings dialog. */
        device_context_inst(dev->cfg_dev, dev->cfg_inst);
        device_set_config_int("line", dev->pend_line);
        device_set_config_string("host", dev->pend_host);
        device_set_config_int("host_port", dev->pend_port);
        device_context_restore();
    }
    modem_unlock();
}

/* ------------------------------------------------------- char device hooks */

static size_t
modem_read(uint8_t *buf, size_t len, void *priv)
{
    modem_t *dev = (modem_t *) priv;
    size_t   n   = 0;

    modem_apply_pending(dev);
    modem_poll_line(dev);

    /* The escape sequence completes on silence, not on a fourth character. */
    if (dev->online && (dev->pluses >= 3) &&
        ((plat_get_ticks() - dev->last_data) >= (dev->s[12] * 20u))) {
        dev->pluses = 0;
        dev->online = 0;
        modem_result(dev, RES_OK);
    }

    /* Whatever the line would not take before. */
    modem_txq_flush(dev);

    /* Anything the carrier is delivering goes in front of the ring -- but only
       as much as the ring can hold.  The far end can push a TCP window's worth
       in a millisecond; the UART drains 5.7 KB/s.  Taking it all and dropping
       the overflow (what this did before) corrupts HDLC frames and the guest's
       TCP pays a retransmission timeout for each.  Leaving it at the far end
       is what a real modem's flow control does: the peer's kernel, or the
       ISP's queue, holds it.  A full ring asks for nothing, and is not a
       hangup: only an actual receive can report one. */
    if (dev->online && (dev->call != NULL)) {
        uint8_t   net[256];
        const int used = (int) ((dev->out_head - dev->out_tail + MODEM_OUT_SIZE) % MODEM_OUT_SIZE);
        int       room = MODEM_OUT_SIZE - 1 - used;

        if (room > (int) sizeof(net))
            room = (int) sizeof(net);
        if (room > 0) {
            int       wouldblock = 0;
            const int ret        = dev->call->recv(dev, net, room, &wouldblock);

            if (ret > 0) {
                for (int i = 0; i < ret; i++)
                    modem_out_byte(dev, net[i]);
            } else if (ret == 0)
                modem_carrier_lost(dev, "the far end hung up");
            else if (!wouldblock)
                modem_carrier_lost(dev, "receive failed");
        }
    }

    while ((n < len) && (dev->out_tail != dev->out_head)) {
        buf[n++]      = dev->out[dev->out_tail];
        dev->out_tail = (dev->out_tail + 1) % MODEM_OUT_SIZE;
    }

    return n;
}

static size_t
modem_write(uint8_t *buf, size_t len, void *priv)
{
    modem_t *dev = (modem_t *) priv;

    for (size_t i = 0; i < len; i++) {
        if (dev->online)
            modem_data_byte(dev, buf[i]);
        else
            modem_command_byte(dev, buf[i]);
    }
    /* All of it is taken: what the line cannot have yet waits in the queue,
       and CTS tells the DTE when to stop. */
    modem_txq_flush(dev);

    return len;
}

static uint32_t
modem_status(void *priv)
{
    modem_t *dev    = (modem_t *) priv;
    uint32_t status = 0;

    /* CTS is on unless the line has fallen behind: see MODEM_TXQ_HIGH. */
    if (!dev->cts_held)
        status |= CHAR_COM_CTS;

    if ((dev->dsr_mode == 0) || (dev->state != MODEM_ST_IDLE))
        status |= CHAR_COM_DSR;

    /* &C0 nails DCD high; &C1 -- the default, and what the cabinet's PPP driver
       watches -- follows the carrier. */
    if ((dev->dcd_mode == 0) || (dev->state == MODEM_ST_ONLINE))
        status |= CHAR_COM_DCD;

    return status;
}

static void
modem_control(uint32_t flags, void *priv)
{
    modem_t  *dev  = (modem_t *) priv;
    const int dtr  = !!(flags & CHAR_COM_DTR);
    const int drop = dev->dtr && !dtr;

    dev->dtr = dtr;

    if (!drop)
        return;

    switch (dev->dtr_mode) {
        case 1: /* back to command mode, carrier kept */
            dev->online = 0;
            break;
        case 2: /* hang up -- the factory default */
            if (dev->state != MODEM_ST_IDLE) {
                modem_hangup(dev);
                modem_result(dev, RES_NO_CARRIER);
            }
            break;
        case 3: /* hang up and reset */
            modem_hangup(dev);
            modem_load_defaults(dev);
            break;
        default: /* &D0: DTR is ignored */
            break;
    }
}

static void
modem_port_config(void *priv)
{
    modem_t *dev = (modem_t *) priv;

    (void) dev; /* the log call compiles away when logging is off */
    char_modem_log(dev->log, "port %u %u%c%u\n", dev->port->com.baud,
                   dev->port->com.data_bits,
                   (dev->port->com.parity & 1) ? 'P' : 'N',
                   dev->port->com.stop_bits);
}

static void
modem_close(void *priv)
{
    modem_t *dev = (modem_t *) priv;

    modem_lock();
    if ((dev->slot >= 0) && (modems[dev->slot] == dev))
        modems[dev->slot] = NULL;
    modem_unlock();

    modem_hangup(dev);
    modem_sound_close(dev->snd);
    log_close(dev->log);
    free(dev);
}

static void *
modem_init(const device_t *info)
{
    modem_t    *dev = (modem_t *) calloc(1, sizeof(modem_t));
    const char *s;

    const int model = info->local & ~MODEM_UNLISTED;

    dev->model = &modem_models[(model < (int) (sizeof(modem_models) / sizeof(modem_models[0])))
                                   ? model
                                   : MODEM_MODEL_SUPRA];
    dev->sock  = (SOCKET) -1;
    dev->snd   = modem_sound_init(device_get_config_int("speaker"));
    modem_load_defaults(dev);

    dev->cfg_inst = device_get_instance();
    if (info->local & MODEM_UNLISTED) {
        dev->cfg_dev = device_context_get_device();
        dev->slot    = ((dev->cfg_inst >= 1) && (dev->cfg_inst <= PCMCIA_SOCKETS)) ? (SERIAL_MAX + dev->cfg_inst - 1) : -1;
    } else {
        dev->cfg_dev = info;
        dev->slot    = ((dev->cfg_inst >= 1) && (dev->cfg_inst <= SERIAL_MAX)) ? (dev->cfg_inst - 1) : -1;
    }
    dev->connect_rate = device_get_config_int("connect_rate");

    s = device_get_config_string("host");
    modem_set_line(dev, device_get_config_int("line"), (s != NULL) ? s : "",
                   device_get_config_int("host_port"));

    snprintf(dev->ident, sizeof(dev->ident), "%s", dev->model->ident);
    snprintf(dev->firmware, sizeof(dev->firmware), "%s", dev->model->fmw);

    dev->port = char_attach(0, modem_read, modem_write, modem_status,
                            modem_control, modem_port_config, dev);
    dev->log  = char_log_open(dev->port, "Modem");

    char_modem_log(dev->log, "init(): %s, ATI%d \"%s\", ATI%d \"%s\", line %s\n",
                   dev->model->name, dev->model->ident_at, dev->ident,
                   dev->model->fmw_at, dev->firmware, modem_line_name(dev));

    if (modem_mutex == NULL)
        modem_mutex = thread_create_mutex();
    modem_lock();
    if (dev->slot >= 0)
        modems[dev->slot] = dev;
    modem_unlock();

    return dev;
}

/* One config array for both parts.  What differs between them -- which ATIn
   carries what, and the text of each -- lives in the model table. */
// clang-format off
static const device_config_t modem_config[] = {
    {
        .name           = "line",
        .description    = "Telephone line",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = MODEM_LINE_DEAD,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "Not connected",             .value = MODEM_LINE_DEAD },
            { .description = "Dial out to a TCP/IP host", .value = MODEM_LINE_TCP  },
            { .description = "Internet (built-in ISP)",   .value = MODEM_LINE_ISP  },
            { .description = ""                                                    }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "host",
        .description    = "Host",
        .type           = CONFIG_STRING,
        .default_string = "",
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "host_port",
        .description    = "Port",
        .type           = CONFIG_SPINNER,
        .default_string = NULL,
        .default_int    = 23,
        .file_filter    = NULL,
        .spinner        = {
            /* 32767 rather than 65535 because device_config_spinner_t is
               int16_t: a wider maximum wraps negative and the box then refuses
               to take a value at all.  Upstream's own modem stops here too. */
            .min = 1,
            .max = 32767
        },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "connect_rate",
        .description    = "Reported connection speed",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 57600,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "57600", .value = 57600 },
            { .description = "50666", .value = 50666 },
            { .description = "46666", .value = 46666 },
            { .description = "33600", .value = 33600 },
            { .description = "28800", .value = 28800 },
            { .description = "14400", .value = 14400 },
            { .description = ""                      }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "speaker",
        .description    = "Speaker",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
};
// clang-format on

const device_t char_modem_supra_com_device = {
    .name          = "Diamond SupraExpress 56e PRO",
    .internal_name = "modem_supra",
    .flags         = DEVICE_COM | DEVICE_HOTPLUG,
    .local         = MODEM_MODEL_SUPRA,
    .init          = modem_init,
    .close         = modem_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = modem_config
};

const device_t char_modem_elsa_com_device = {
    .name          = "ELSA MicroLink 56k",
    .internal_name = "modem_elsa",
    .flags         = DEVICE_COM | DEVICE_HOTPLUG,
    .local         = MODEM_MODEL_ELSA,
    .init          = modem_init,
    .close         = modem_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = modem_config
};

/* 86Box-Next: the 3C562D PC Card's modem, opened by the card on its UART with
   char_open_unlisted(): its settings are the card's (pccard_3c562.c repeats
   modem_config's entries); the status bar's modem icon lists it under its
   socket. */
const device_t char_modem_3c562_device = {
    .name          = "3Com 3C562D LAN+33.6 Modem",
    .internal_name = "modem_3c562",
    .flags         = 0,
    .local         = MODEM_MODEL_3C562 | MODEM_UNLISTED,
    .init          = modem_init,
    .close         = modem_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = modem_config
};
