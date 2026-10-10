/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          External Hayes-compatible 56k modems on a COM port: a Diamond
 *          SupraExpress 56e PRO, an ELSA MicroLink 56k and (86Box-Next) a
 *          Standard Hayes-compatible one.
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
 *          stack wants.  86Box-Next: or the line is the telephone network
 *          of isp-server (isp-server/): the modem has a number, other
 *          modems can ring it (RING, caller ID, ATA or S0), it can ring
 *          them, and any other number reaches the ISP -- PPP and the host's
 *          Internet.  A call takes as long as a real one: dial tone, the
 *          digits, ringback, then V.34 / V.90 training before CONNECT, all
 *          of it heard on the speaker (modem_sound.c) unless ATM0 or the
 *          device's Speaker option silences it.
 *
 *          86Box-Next: the same engine is the modem other devices carry on a
 *          UART of their own -- a PC Card's -- opened with char_modem_attach()
 *          and answering as the model the device gives it (char_modem.h).
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
#include <time.h>
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
#include <86box/modem_voice.h>
#include <86box/snd_mic.h>
#include <86box/thread.h>
#include <86box/char_modem.h>

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
   delivers it (a TCP send can block or go short),
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
       MODEM_LINE_DEAD  = 0, /* nothing: dialling fails the way it would */
       MODEM_LINE_TCP   = 1, /* a TCP host stands in for the whole PSTN */
       /* 86Box-Next: isp-server's telephone exchange.  (For a day 2 was an
          ISP built into the emulator; the exchange still reaches the ISP
          with any number that is no modem's, so such a configuration works
          as it did.) */
       MODEM_LINE_PHONE = 2
};

/* 86Box-Next: the telephone network.  isp_srv.h has the exchange's side. */
#define EXCHANGE_GREETING "86BOX-EXCHANGE 1 "
#define RING_PERIOD_MS    6000   /* a ring every six seconds...       */
#define RING_ON_MS        2000   /* ...two of them ringing (RI on)     */
#define RING_GIVE_UP_MS   130000 /* the exchange gives up at 120 s     */
#define EXCHANGE_RETRY_MS 3000   /* isp-server not there: try again    */

enum { /* the modem's line to the exchange */
       PHONE_OFF = 0,
       PHONE_CONNECTING,
       PHONE_UP
};

enum { /* a call's connection to the exchange */
       SIG_NONE = 0,
       SIG_CONNECTING, /* the TCP connect in flight              */
       SIG_ASKED,      /* DIAL or ANSWER sent; reading the reply */
       SIG_DATA        /* CONNECT: the call's bytes from here on */
};

enum { /* line state machine */
       MODEM_ST_IDLE = 0,
       MODEM_ST_DIALING,    /* off hook, waiting out the dial  */
       MODEM_ST_CONNECTING, /* the far end is being reached    */
       MODEM_ST_ANSWERING,  /* carrier found: training, then CONNECT */
       MODEM_ST_ONLINE,
       MODEM_ST_VOICE       /* 86Box-Next: off hook in voice mode, a call or not */
};

enum { /* 86Box-Next: voice mode's streams */
       VM_NONE = 0,
       VM_TX,  /* #VTX / +VTX: the DTE's audio to the line */
       VM_RX,  /* #VRX / +VRX: the line's audio to the DTE */
       VM_TR   /* +VTR: both at once                       */
};

enum { /* 86Box-Next: which voice command set the DTE speaks */
       VSET_ROCKWELL = 0, /* #CLS=8, #VLS, #VBS...: VCON              */
       VSET_V253          /* +FCLASS=8, +VLS, +VSM...: OK (ITU V.253) */
};

enum { /* 86Box-Next: the voice audio's format on the serial port */
       VF_ADPCM = 0, /* Rockwell ADPCM, vbs bits a sample */
       VF_U8,        /* unsigned 8-bit linear             */
       VF_S8,        /* signed 8-bit linear               */
       VF_S16,       /* signed 16-bit, little-endian      */
       VF_ULAW,      /* G.711 mu-law                      */
       VF_ALAW       /* G.711 A-law                       */
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
#define MODEM_PNP_MIN_MS 50   /* 86Box-Next: DTR on, RTS off, at least this
                                 long before RTS rises: a PnP enumeration */

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
       RES_VCON, /* 86Box-Next: Rockwell's voice connection */
       RES_NONE = -1
};

static const char *modem_res_text[] = {
    "OK", "CONNECT", "RING", "NO CARRIER", "ERROR",
    "CONNECT 1200", "NO DIALTONE", "BUSY", "NO ANSWER", "VCON"
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
    MODEM_MODEL_HAYES = 2  /* 86Box-Next: none of the cabinet's thirty */
};

/* char_modem_model_t (char_modem.h): `name` is MD_NAME.CSV's, verbatim, and
   `fmw_at` MD_NAME column 7, filed as MODEMFMW.  Both are voice modems
   (Rockwell's #CLS=8 set). */
typedef char_modem_model_t modem_model_t;

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
        },
        .voice      = 1,
        /* 86Box-Next: Diamond's SUPIV92.INF, "SupraExpress 56e PRO" */
        .pnp_id     = "SUP2311", .pnp_name = "SupraExpress 56e PRO"
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
        },
        .voice      = 1
    },
    /* 86Box-Next: a modem with no make to it, for a guest to install with
       its own generic driver -- Windows' "Standard 56000 bps Modem", OS/2's
       and Linux's plain Hayes set.  Data and fax: the voice commands are
       vendor sets, and a standard modem has none.  Its answers name no part
       the cabinet knows (no SupraExpress, no MicroLink). */
    [MODEM_MODEL_HAYES] = {
        .name       = "Standard Hayes-compatible 56k Modem",
        .ident_at   = 3, .ident = "Hayes-compatible 56000 bps Modem",
        .fmw_at     = 7, .fmw   = "V1.00",
        .country_at = 5,
        .info       = {
            [0] = "56000",
            [1] = "012", /* not 255: "56000" + "255" is an INSYS Pocket 56k */
            [2] = "OK",
            [4] = "Standard Hayes-compatible V.90 Data/Fax Modem"
        },
        .voice      = 0
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
       none. */
    int                           state;
    const struct modem_line_ops  *call;
    SOCKET                        sock;
    uint32_t deadline; /* plat_get_ticks() value the current wait ends at  */
    int      said_connect; /* ANSWERING: the CONNECT line has been queued  */
    int      dtr;
    int      rts;          /* 86Box-Next: for the serial PnP enumeration   */
    int      pnp_armed;    /* DTR rose with RTS off: RTS rising enumerates */
    uint32_t pnp_dtr_at;   /* ...and when                                  */

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
       or from SERIAL_MAX on, a modem another device carries), what it is
       called there, and the device section its settings are written back to:
       its own for a COM port modem, its device's for a carried one. */
    int             slot;      /* -1: none */
    char            label[32]; /* "COM1", or what its device calls it */
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
    char pend_phone[24];
    char pend_exchange[160];

    /* 86Box-Next: the telephone network.  The modem keeps a connection to
       the exchange (`ctl`), registers its number on it and is rung on it;
       each call is a connection of its own, which opens with a request
       (`sig_req`) and the exchange's answer before the call's bytes. */
    char     phone_cfg[24];  /* the number asked for; "" lets the exchange choose */
    char     exch_host[128];
    int      exch_port;
    char     number[24];     /* the number the exchange gave; under modem_mutex */
    SOCKET   ctl;
    int      ctl_state;      /* PHONE_* */
    uint32_t ctl_at;         /* when to try again; when the attempt began */
    uint32_t ctl_polls;
    char     ctl_in[256];
    int      ctl_len;
    int      ring_id;        /* the call ringing this modem; 0: none */
    char     ring_from[24];
    uint32_t ring_next;
    uint32_t ring_on_until;  /* RI is on until then */
    uint32_t ring_started;
    int      cid;            /* AT#CID=1 / AT+VCID=1: caller ID after the first RING */
    char     sig_req[160];
    int      sig;            /* SIG_* */
    char     sig_in[64];
    int      sig_len;
    int      call_fail;      /* what a refused call reports: BUSY, NO ANSWER... */

    /* 86Box-Next: voice, in Rockwell's #CLS=8 command set -- what Windows'
       Unimodem/V sends a Rockwell part (see the voice section below). */
    int      fclass;         /* #CLS / +FCLASS: 0 data, 8 voice */
    int      vset;           /* VSET_*: the last to set voice mode */
    int      vls;            /* Rockwell's #VLS: 0/4 the line, 1 handset, 2 speaker, 3 microphone, 6 speakerphone */
    int      vls253;         /* +VLS as the DTE set it (V.253's numbering) */
    int      vfmt;           /* VF_* */
    int      vbs;            /* #VBS: 2-4 ADPCM bits a sample */
    int      vsm;            /* +VSM's compression id */
    int      vsds;           /* +VSD's sensitivity, 0-255 */
    int      vtd;            /* +VTD: a +VTS tone's length, 0.01 s */
    int      vts_unit;       /* ms in a #VTS/+VTS [f1,f2,d]'s d */
    int      vts_digit;      /* ms a #VTS/+VTS digit lasts */
    int      vsr;            /* #VSR: samples a second on the serial port */
    int      vparam[8];      /* the other #V settings, VP_* */
    int      vcall;          /* off hook in voice mode */
    int      vanswered;      /* ...having answered rather than dialled */
    int      vpeer;          /* the far end: 1 voice, 0 a modem, -1 not answered yet */
    int      vcon_said;
    int      vok_after_dial; /* ATD...; -- OK once dialled, not VCON */
    int      vline_gone;     /* the far end hung up; still off hook */
    int      vgone_said;
    int      vmode;          /* VM_*: #VTX or #VRX running */
    int      vdle;           /* a DLE in the #VTX stream */
    int      vtx_end;        /* DLE ETX came: play out, then OK */
    uint8_t  vafter[64];     /* what the DTE sent after DLE ETX, for the command line */
    int      vafter_len;
    int      vswallow;       /* after #VRX: the rest of what stopped it */
    int      vlo;            /* #VBS=16: the low byte, -1 if none */
    int      handset;        /* the host's handset is off hook */
    int      handset_call;   /* the call is the handset's: no result codes */
    int      mic_open;
    rv_adpcm_t        vcodec;    /* the DTE's audio, decoded */
    voice_resampler_t vrs;       /* ...from the serial port's rate to the line's */
    rv_adpcm_t        vcodec_rx; /* the line's, coded for the DTE */
    voice_resampler_t vrs_rx;
    int16_t  vtxq[16384];    /* the DTE's audio for the line, at 8000 Hz */
    uint32_t vtxq_head, vtxq_tail;
    int16_t  vrxq[8192];     /* the far end's audio, at 8000 Hz */
    uint32_t vrxq_head, vrxq_tail;
    uint32_t vclock;         /* when the next 20 ms of line is due */
    uint32_t vms;            /* line time, for tones */
    voice_tone_t vtone;      /* #VTS */
    char     vts[96];
    int      vts_pos;
    int      vts_busy;
    voice_tone_t vsynth;     /* a modem at the far end, as heard */
    voice_silence_t vsil;
    int      vsil_said;
    voice_deframer_t vdefr;
    char     vdtmf[16];      /* digits the far end pressed, for the DTE */
    int      vdtmf_len;
    int      pend_handset;   /* CHAR_MODEM_HANDSET_* + 1 from the UI; 0: none (modem_mutex) */
    char     pend_dial[24];
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

/* 86Box-Next: the telephone network.  A call is a connection to the
   exchange that opens with DIAL (or ANSWER, for a call ringing this modem);
   the exchange's answer comes back a line at a time -- RINGING, then
   CONNECT, BUSY, NOANSWER... -- read a byte at a time so that nothing of
   the call itself is taken with it. */
static int
modem_exchange_connect(modem_t *dev, SOCKET *s)
{
    *s = plat_netsocket_create(NET_SOCKET_TCP);
    if (!CHAR_FD_VALID(*s))
        return -1;
    if (plat_netsocket_connect(*s, dev->exch_host, (unsigned short) dev->exch_port) != 0) {
        plat_netsocket_close(*s);
        *s = (SOCKET) -1;
        return -1;
    }
    return 0;
}

/* One request, on a connection that has just come up: it goes whole. */
static int
modem_exchange_send(SOCKET s, const char *req)
{
    char      buf[256];
    int       wouldblock = 0;
    const int n          = snprintf(buf, sizeof(buf), EXCHANGE_GREETING "%s\r\n", req);

    return plat_netsocket_send(s, (const uint8_t *) buf, (unsigned int) n, &wouldblock) == n;
}

static int
modem_phone_open(modem_t *dev)
{
    if (modem_exchange_connect(dev, &dev->sock) != 0)
        return -1;
    dev->sig     = SIG_CONNECTING;
    dev->sig_len = 0;
    return 0;
}

static int
modem_phone_connected(modem_t *dev)
{
    if (dev->sig == SIG_CONNECTING) {
        const int c = plat_netsocket_connected(dev->sock);

        if (c <= 0)
            return c;
        if (!modem_exchange_send(dev->sock, dev->sig_req))
            return -1;
        dev->sig = SIG_ASKED;
    }
    while (dev->sig == SIG_ASKED) {
        uint8_t   ch;
        int       wouldblock = 0;
        const int r          = plat_netsocket_receive(dev->sock, &ch, 1, &wouldblock);

        if (r < 0)
            return wouldblock ? 0 : -1;
        if (r == 0)
            return -1; /* the exchange hung up */
        if (ch == '\r')
            continue;
        if (ch != '\n') {
            if (dev->sig_len < (int) (sizeof(dev->sig_in) - 1))
                dev->sig_in[dev->sig_len++] = (char) ch;
            continue;
        }
        dev->sig_in[dev->sig_len] = '\0';
        dev->sig_len              = 0;
        char_modem_log(dev->log, "exchange: %s\n", dev->sig_in);
        if (!strncmp(dev->sig_in, "CONNECT", 7) && ((dev->sig_in[7] == '\0') || (dev->sig_in[7] == ' '))) {
            /* What picked up at the other end: a voice call, or a modem. */
            dev->vpeer = !strcmp(dev->sig_in, "CONNECT VOICE");
            dev->sig   = SIG_DATA;
        }
        else if (strcmp(dev->sig_in, "RINGING")) {
            dev->call_fail = !strcmp(dev->sig_in, "BUSY")       ? RES_BUSY
                             : !strcmp(dev->sig_in, "NOANSWER") ? RES_NO_ANSWER
                                                                : RES_NO_CARRIER;
            return -1;
        }
    }
    return (dev->sig == SIG_DATA) ? 1 : 0;
}

static const modem_line_ops_t modem_line_phone = {
    .open      = modem_phone_open,
    .connected = modem_phone_connected,
    .send      = modem_tcp_send,
    .recv      = modem_tcp_recv,
    .close     = modem_tcp_close
};

#ifdef ENABLE_CHAR_MODEM_LOG
/* For the log, which is all that names the line. */
static const char *
modem_line_name(const modem_t *dev)
{
    switch (dev->line) {
        case MODEM_LINE_TCP:
            return dev->host;
        case MODEM_LINE_PHONE:
            return "the telephone network";
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
    dev->sig      = SIG_NONE;
    dev->vcall        = 0;
    dev->vpeer        = -1;
    dev->vline_gone   = 0;
    dev->handset_call = 0;
    dev->vmode        = VM_NONE;
    dev->vts_busy     = 0;
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

static void modem_voice_gone(modem_t *dev);

/* Hand the line what it will take of the queue, in order.  A short send
   advances by what went; a full line is tried again on the next poll. */
static void
modem_txq_flush(modem_t *dev)
{
    while ((dev->call != NULL) && ((dev->state == MODEM_ST_ONLINE) || (dev->state == MODEM_ST_VOICE)) &&
           (dev->txq_tail != dev->txq_head)) {
        const uint32_t chunk      = (dev->txq_head > dev->txq_tail) ? (dev->txq_head - dev->txq_tail)
                                                                    : (MODEM_TXQ_SIZE - dev->txq_tail);
        int            wouldblock = 0;
        const int      ret        = dev->call->send(dev, &dev->txq[dev->txq_tail], (int) chunk, &wouldblock);

        if (ret > 0)
            dev->txq_tail = (dev->txq_tail + (uint32_t) ret) % MODEM_TXQ_SIZE;
        else if ((ret == 0) || wouldblock)
            break; /* the line is full for now */
        else {
            if (dev->state == MODEM_ST_VOICE)
                modem_voice_gone(dev);
            else
                modem_carrier_lost(dev, "send failed");
            return;
        }
    }
    if (dev->state != MODEM_ST_VOICE)
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

/* ----------------------------------------------- the telephone network */

static void modem_lock(void);
static void modem_unlock(void);

static void
modem_ring_stop(modem_t *dev)
{
    dev->ring_id       = 0;
    dev->s[1]          = 0;
    dev->ring_on_until = 0;
    char_update_status(dev->port);
}

/* The connection to the exchange goes: no number, no ringing, until it is
   back. */
static void
modem_phone_unplug(modem_t *dev, uint32_t retry_at)
{
    if (CHAR_FD_VALID(dev->ctl)) {
        plat_netsocket_close(dev->ctl);
        dev->ctl = (SOCKET) -1;
    }
    dev->ctl_state = PHONE_OFF;
    dev->ctl_len   = 0;
    dev->ctl_at    = retry_at;
    modem_lock();
    dev->number[0] = '\0';
    modem_unlock();
    if (dev->ring_id)
        modem_ring_stop(dev);
}

/* One line from the exchange on the modem's own connection. */
static void
modem_phone_line(modem_t *dev, const char *line)
{
    char from[24];
    int  id;

    if (!strncmp(line, "NUMBER ", 7)) {
        modem_lock();
        snprintf(dev->number, sizeof(dev->number), "%s", line + 7);
        modem_unlock();
        char_modem_log(dev->log, "the exchange gives this line %s\n", line + 7);
    } else if (sscanf(line, "RING %d %23s", &id, from) == 2) {
        /* An off-hook modem is busy, and the exchange knows it; a second
           call while one rings is the exchange's to refuse. */
        if ((dev->state == MODEM_ST_IDLE) && (dev->ring_id == 0)) {
            dev->ring_id      = id;
            dev->ring_started = plat_get_ticks();
            dev->ring_next    = dev->ring_started;
            dev->s[1]         = 0;
            snprintf(dev->ring_from, sizeof(dev->ring_from), "%s", from);
        }
    } else if (sscanf(line, "CANCEL %d", &id) == 1) {
        if (dev->ring_id == id)
            modem_ring_stop(dev); /* the caller gave up: the ringing just stops */
    }
}

/* The modem's line to the exchange: connect, register, listen for RING.
   Polled from modem_read(), every 32nd call: often enough for a ring,
   little enough per character. */
static void
modem_phone_poll(modem_t *dev)
{
    const uint32_t now = plat_get_ticks();

    if (dev->line != MODEM_LINE_PHONE) {
        if (dev->ctl_state != PHONE_OFF)
            modem_phone_unplug(dev, now);
        return;
    }
    if ((++dev->ctl_polls & 31) != 0)
        return;

    switch (dev->ctl_state) {
        case PHONE_OFF:
            if ((int32_t) (now - dev->ctl_at) < 0)
                break;
            if (modem_exchange_connect(dev, &dev->ctl) != 0) {
                dev->ctl_at = now + EXCHANGE_RETRY_MS;
                break;
            }
            dev->ctl_state = PHONE_CONNECTING;
            dev->ctl_at    = now;
            break;

        case PHONE_CONNECTING: {
            const int c = plat_netsocket_connected(dev->ctl);

            if (c == 1) {
                /* "Windows 98 (COM2)": how the exchange's phone book lists it. */
                char req[160];
                char where[32];

                snprintf(where, sizeof(where), "%s", dev->label[0] ? dev->label : dev->model->name);
                snprintf(req, sizeof(req), "REGISTER %s %.80s (%s)", dev->phone_cfg[0] ? dev->phone_cfg : "-",
                         vm_name[0] ? vm_name : dev->model->name, where);
                for (char *q = req; *q; q++)
                    if ((*q == '\r') || (*q == '\n'))
                        *q = ' ';
                if (modem_exchange_send(dev->ctl, req))
                    dev->ctl_state = PHONE_UP;
                else
                    modem_phone_unplug(dev, now + EXCHANGE_RETRY_MS);
            } else if ((c < 0) || ((int32_t) (now - dev->ctl_at) > 5000))
                modem_phone_unplug(dev, now + EXCHANGE_RETRY_MS);
            break;
        }

        case PHONE_UP:
            for (;;) {
                uint8_t   buf[128];
                int       wouldblock = 0;
                const int r          = plat_netsocket_receive(dev->ctl, buf, sizeof(buf), &wouldblock);

                if (r > 0) {
                    for (int i = 0; i < r; i++) {
                        if (buf[i] == '\r')
                            continue;
                        if (buf[i] == '\n') {
                            dev->ctl_in[dev->ctl_len] = '\0';
                            dev->ctl_len              = 0;
                            modem_phone_line(dev, dev->ctl_in);
                        } else if (dev->ctl_len < (int) (sizeof(dev->ctl_in) - 1))
                            dev->ctl_in[dev->ctl_len++] = (char) buf[i];
                    }
                    continue;
                }
                if ((r == 0) || !wouldblock)
                    modem_phone_unplug(dev, now + EXCHANGE_RETRY_MS); /* isp-server went away */
                break;
            }
            break;

        default:
            break;
    }
}

static void modem_voice_begin(modem_t *dev, int answered);

/* Off hook for the call ringing this modem: ATA, or S0's rings counted, as
   a modem -- or as a voice call (voice mode, or the handset).  0 if there is
   nothing to answer. */
static int
modem_answer(modem_t *dev, int voice)
{
    const uint32_t now = plat_get_ticks();

    if ((dev->ring_id == 0) || (dev->state != MODEM_ST_IDLE))
        return 0;
    snprintf(dev->sig_req, sizeof(dev->sig_req), voice ? "ANSWER %d VOICE" : "ANSWER %d", dev->ring_id);
    modem_ring_stop(dev);
    dev->refused   = 0;
    dev->call_fail = RES_NO_CARRIER;
    dev->vpeer     = -1;
    if (modem_line_phone.open(dev) != 0)
        return 0;
    dev->call = &modem_line_phone;
    if (voice) {
        modem_voice_begin(dev, 1);
        return 1;
    }
    dev->state     = MODEM_ST_CONNECTING;
    dev->answer_at = now; /* the caller is there already */
    dev->deadline  = now + (dev->s[7] * 1000u);
    char_update_status(dev->port);
    return 1;
}

/* Caller ID, the Rockwell way (AT#CID=1, AT+VCID=1): between the first ring
   and the second. */
static void
modem_caller_id(modem_t *dev)
{
    const time_t     t  = time(NULL);
    const struct tm *tm = localtime(&t);
    char             buf[48];

    if (tm != NULL) {
        snprintf(buf, sizeof(buf), "DATE = %02d%02d", tm->tm_mon + 1, tm->tm_mday);
        modem_out_line(dev, buf);
        snprintf(buf, sizeof(buf), "TIME = %02d%02d", tm->tm_hour, tm->tm_min);
        modem_out_line(dev, buf);
    }
    /* "O": out of area, what a modem says for a number it was not told. */
    snprintf(buf, sizeof(buf), "NMBR = %s", strcmp(dev->ring_from, "-") ? dev->ring_from : "O");
    modem_out_line(dev, buf);
}

/* RING, every six seconds; RI with each; S1 counts them and S0 answers. */
static void
modem_phone_ring(modem_t *dev)
{
    const uint32_t now = plat_get_ticks();

    if (dev->ring_id == 0)
        return;
    if ((int32_t) (now - dev->ring_started) > RING_GIVE_UP_MS) {
        modem_ring_stop(dev);
        return;
    }
    if ((int32_t) (now - dev->ring_next) < 0)
        return;
    dev->ring_next     = now + RING_PERIOD_MS;
    dev->ring_on_until = now + RING_ON_MS;
    if (dev->s[1] < 255)
        dev->s[1]++;
    modem_result(dev, RES_RING);
    if (!dev->handset)
        modem_sound_event(dev->snd, MODEM_SOUND_BELL, NULL, 0); /* the phone beside it rings too */
    if (dev->cid && (dev->s[1] == 1))
        modem_caller_id(dev);
    char_update_status(dev->port);
    if ((dev->s[0] > 0) && (dev->s[1] >= dev->s[0]) && !modem_answer(dev, dev->fclass == 8))
        modem_result(dev, RES_NO_CARRIER);
}

/* Everything a dialled string can contain that is not a digit: the dial
   modifiers.  On a TCP line the number itself is never used -- there is one
   host; on the telephone network the exchange routes it. */
static void
modem_dial(modem_t *dev, const char *number, int voice)
{
    uint32_t       now     = plat_get_ticks();
    const uint32_t dial_ms = modem_sound_dial_ms(number, dev->s[8], dev->pulse);

    /* There is one line: the number goes nowhere, but dialling it takes the
       time it takes -- tone or pulse, digit by digit, a comma for S8 -- and
       the far end answers after its first ring. */
    char_modem_log(dev->log, "dial \"%s\"\n", number);
    dev->refused   = 0;
    dev->answer_at = now + dial_ms + modem_sound_ring_ms();
    dev->vpeer     = -1;
    if ((dev->line != MODEM_LINE_DEAD) && !dev->handset_call) {
        modem_sound_country(dev->snd, (dev->country == 16) || (dev->country == 0xB4), dev->connect_rate > 33600);
        modem_sound_event(dev->snd, MODEM_SOUND_DIAL, number, (dev->s[8] & 0xff) | (dev->pulse << 8));
    }

    dev->call_fail = RES_NO_CARRIER;
    if (dev->line != MODEM_LINE_DEAD) {
        const modem_line_ops_t *ops = &modem_line_tcp;

        if (dev->line == MODEM_LINE_PHONE) {
            char from[24];

            modem_lock();
            snprintf(from, sizeof(from), "%s", dev->number[0] ? dev->number : "-");
            modem_unlock();
            snprintf(dev->sig_req, sizeof(dev->sig_req), voice ? "DIAL %s %s VOICE" : "DIAL %s %s", from, number);
            ops = &modem_line_phone;
        }

        /* On the telephone network a bare ATD reaches nobody. */
        if (((dev->line == MODEM_LINE_PHONE) && (strpbrk(number, "0123456789*#") == NULL)) ||
            (ops->open(dev) != 0)) {
            dev->state    = MODEM_ST_DIALING;
            dev->deadline = dev->answer_at + 5000;   /* it rings, and nobody answers */
            return;
        }
        dev->call = ops;
        if (voice && (dev->line == MODEM_LINE_PHONE)) {
            /* A voice call: off hook from here, VCON once dialled (#VRN=0)
               or once answered. */
            modem_voice_begin(dev, 0);
            dev->deadline = now + dial_ms;
            return;
        }
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

static void modem_voice_call_poll(modem_t *dev, uint32_t now);

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

            if ((connected == 1) && (dev->vpeer == 1)) {
                /* A voice answered, not a modem: no carrier comes, and S7
                   runs out. */
                if ((int32_t) (now - dev->deadline) >= 0) {
                    modem_hangup(dev);
                    modem_result(dev, RES_NO_CARRIER);
                }
            } else if (connected == 1) {
                /* The far end is there; it picks up after its first ring. */
                if ((int32_t) (now - dev->answer_at) >= 0)
                    modem_answered(dev);
            } else if (connected == -1) {
                if (!dev->refused) {
                    dev->refused = 1;
                    if (dev->call_fail == RES_BUSY) {
                        /* Engaged: the busy tone, a few beats of it. */
                        modem_sound_event(dev->snd, MODEM_SOUND_BUSY, NULL, 0);
                        dev->deadline = now + 3000;
                    } else {
                        /* Nobody there: a real call rings on unanswered, then
                           gives up. */
                        dev->deadline = ((int32_t) (dev->answer_at - now) > 0 ? dev->answer_at : now) + 5000;
                    }
                }
                if ((int32_t) (now - dev->deadline) >= 0) {
                    const int res = dev->call_fail;

                    modem_hangup(dev);
                    modem_result(dev, res);
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

        case MODEM_ST_IDLE:
            modem_phone_ring(dev);
            break;

        case MODEM_ST_VOICE:
            modem_voice_call_poll(dev, now);
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------------- voice */

/* 86Box-Next: voice mode, in Rockwell's own command set (the SupraExpress is
   a Rockwell RCVDL56ACF/SP).  It is what Windows 9x's Unimodem/V sends a
   Rockwell part -- the Diamond INF's VoiceAnswer is AT#CLS=8, #VLS=0, #VBT=1,
   #VSR=7200, #VBS=4, #VSB=1, #VSS=2, S30=60, ATA; StartPlay AT#VTX,
   StartRecord AT#VRX, GenerateDigit AT#VTS=<digit> -- and what vgetty's
   Rockwell driver sends.

   Off hook in voice mode (VCON), the modem is a telephone line the DTE talks
   and listens on, a 20 ms frame at a time: #VTX takes the DTE's audio
   (Rockwell ADPCM or linear, DLE-shielded, ended by DLE ETX) and plays it to
   the far end; #VRX sends the DTE what the line brings, until the DTE sends
   anything at all.  Both report what the modem hears on the line as DLE
   codes: the far end's DTMF digits, b for the busy tone once it has hung up,
   s for silence, a or e for a modem answering or calling.

   On the telephone network the far end is another voice end -- a modem in
   voice mode, or one's handset -- and the line between them carries frames
   of 8000 Hz mu-law (modem_voice.h).  If a modem is there instead, it is
   heard (answer tone, or its calling tone) and sent nothing.

   The handset is the telephone beside the modem, which is the host: its
   speaker plays the line, its microphone talks on it (snd_mic.c). */

enum { /* vparam[] */
       VP_VSS = 0, /* silence sensitivity, 0-3                  */
       VP_VSP,     /* silence period, 0.1 s                     */
       VP_VRN,     /* ringback never came, s (0: VCON at once)  */
       VP_VRA,     /* ringback went away, 0.1 s                 */
       VP_VBT,     /* #VTS digit length, 0.1 s                  */
       VP_VSD,     /* silence deletion                          */
       VP_VGT,     /* gains, accepted                           */
       VP_VGR
};

#define DLE 0x10
#define ETX 0x03
#define CAN 0x18

#define VTXQ_HIGH 4000 /* half a second of the DTE's audio waiting: CTS off */
#define VTXQ_LOW  2000
#define VRXQ_KEEP 1600 /* more than 200 ms of the far end waiting: catch up */

static void modem_command_byte(modem_t *dev, uint8_t val);
static int  modem_arg(const char **p);

/* A voice connection: VCON in Rockwell's set, OK in V.253's. */
static int
modem_vcon(const modem_t *dev)
{
    return (dev->vset == VSET_V253) ? RES_OK : RES_VCON;
}

/* A result, unless the call is the handset's -- the DTE has no part in
   that one. */
static void
modem_vresult(modem_t *dev, int code)
{
    if (!dev->handset_call)
        modem_result(dev, code);
}

static void
modem_vdte_event(modem_t *dev, char code)
{
    if (dev->vmode != VM_NONE) {
        modem_out_byte(dev, DLE);
        modem_out_byte(dev, (uint8_t) code);
    }
}

static void
modem_voice_defaults(modem_t *dev)
{
    dev->fclass           = 0;
    dev->vset             = VSET_ROCKWELL;
    dev->vls              = 0;
    dev->vls253           = 0;
    dev->vfmt             = VF_ADPCM;
    dev->vbs              = 4;
    dev->vsm              = 1;
    dev->vsds             = 128;
    dev->vtd              = 100;
    dev->vsr              = 7200;
    dev->vparam[VP_VSS]   = 2;
    dev->vparam[VP_VSP]   = 50;
    dev->vparam[VP_VRN]   = 0;
    dev->vparam[VP_VRA]   = 50;
    dev->vparam[VP_VBT]   = 1;
    dev->vparam[VP_VSD]   = 0;
    dev->vparam[VP_VGT]   = 128;
    dev->vparam[VP_VGR]   = 128;
}

/* The host's microphone, wanted or not. */
static void
modem_voice_mic(modem_t *dev, int want)
{
    if (want && !dev->mic_open) {
        dev->mic_open = 1;
        (void) snd_mic_open();
    } else if (!want && dev->mic_open) {
        dev->mic_open = 0;
        snd_mic_close();
    }
}

/* Off hook in voice mode: a call dialled or answered (dev->call set), or
   just off hook. */
static void
modem_voice_begin(modem_t *dev, int answered)
{
    dev->state        = MODEM_ST_VOICE;
    dev->vcall        = 1;
    dev->vanswered    = answered;
    dev->vcon_said    = 0;
    dev->vok_after_dial = 0;
    dev->vline_gone   = 0;
    dev->vgone_said   = 0;
    dev->vclock       = plat_get_ticks();
    dev->vrxq_head    = 0;
    dev->vrxq_tail    = 0;
    dev->vdtmf_len    = 0;
    dev->txq_head     = 0;
    dev->txq_tail     = 0;
    memset(&dev->vdefr, 0, sizeof(dev->vdefr));
    memset(&dev->vsynth, 0, sizeof(dev->vsynth));
    voice_silence_init(&dev->vsil, dev->vparam[VP_VSS]);
    char_update_status(dev->port);
}

/* The far end has hung up, or the line failed: still off hook, hearing the
   exchange's busy tone, until ATH. */
static void
modem_voice_gone(modem_t *dev)
{
    if (dev->call != NULL) {
        dev->call->close(dev);
        dev->call = NULL;
    }
    dev->vline_gone = 1;
    dev->txq_head   = 0;
    dev->txq_tail   = 0;
    char_modem_log(dev->log, "voice: the far end hung up\n");
}

/* The call, until it is answered: VCON then (or once dialled, with #VRN=0);
   BUSY, NO ANSWER... if it never is. */
static void
modem_voice_call_poll(modem_t *dev, uint32_t now)
{
    if ((dev->call != NULL) && (dev->vpeer < 0)) {
        const int c = dev->call->connected(dev);

        if (c == 1) {
            char_modem_log(dev->log, "voice: answered by %s\n", dev->vpeer ? "a voice" : "a modem");
            if (!dev->vanswered)
                modem_sound_event(dev->snd, MODEM_SOUND_HANGUP, NULL, 0); /* the ringback stops */
            if (!dev->vcon_said) {
                dev->vcon_said = 1;
                modem_vresult(dev, dev->vok_after_dial ? RES_OK : modem_vcon(dev));
            }
        } else if (c == -1) {
            const int res = dev->call_fail;

            if (!dev->vcon_said) {
                if (res == RES_BUSY)
                    modem_sound_event(dev->snd, MODEM_SOUND_BUSY, NULL, 0);
                if (dev->handset_call) {
                    /* The handset hears it: busy, or nothing. */
                    modem_voice_gone(dev);
                    dev->vcon_said = 1;
                    return;
                }
                modem_hangup(dev);
                modem_vresult(dev, res);
                return;
            }
            modem_voice_gone(dev);
        }
    }
    if (!dev->vcon_said && !dev->vanswered && ((dev->vparam[VP_VRN] == 0) || dev->vok_after_dial) &&
        ((int32_t) (now - dev->deadline) >= 0)) {
        /* Dialled: VCON without waiting for an answer (#VRN=0), as Unimodem
           asks -- or OK for ATD...;. */
        dev->vcon_said = 1;
        modem_vresult(dev, dev->vok_after_dial ? RES_OK : modem_vcon(dev));
    }
}

/* -------------------------------------------- the DTE's audio, both ways */

static uint32_t
modem_vtxq_used(const modem_t *dev)
{
    return (dev->vtxq_head - dev->vtxq_tail) % (sizeof(dev->vtxq) / sizeof(dev->vtxq[0]));
}

static void
modem_vtx_cts(modem_t *dev)
{
    const uint32_t used = modem_vtxq_used(dev);
    const int      held = dev->cts_held;

    if (!held && (used >= VTXQ_HIGH))
        dev->cts_held = 1;
    else if (held && (used <= VTXQ_LOW))
        dev->cts_held = 0;
    if (held != dev->cts_held)
        char_update_status(dev->port);
}

static void
modem_vtxq_put(modem_t *dev, const int16_t *s, size_t n)
{
    const uint32_t size = sizeof(dev->vtxq) / sizeof(dev->vtxq[0]);

    for (size_t i = 0; i < n; i++) {
        if (((dev->vtxq_head + 1) % size) == dev->vtxq_tail)
            return; /* past CTS: lost */
        dev->vtxq[dev->vtxq_head] = s[i];
        dev->vtxq_head            = (dev->vtxq_head + 1) % size;
    }
}

/* One byte of the DTE's audio. */
static void
modem_vtx_data(modem_t *dev, uint8_t b)
{
    int16_t s[8];
    int16_t rs[32];
    size_t  n = 0;

    switch (dev->vfmt) {
        case VF_U8:
            s[n++] = (int16_t) (((int) b - 128) << 8);
            break;
        case VF_S8:
            s[n++] = (int16_t) ((int8_t) b * 256);
            break;
        case VF_S16:
            if (dev->vlo < 0)
                dev->vlo = b;
            else {
                s[n++]   = (int16_t) (uint16_t) (dev->vlo | (b << 8));
                dev->vlo = -1;
            }
            break;
        case VF_ULAW:
            s[n++] = voice_ulaw_decode(b);
            break;
        case VF_ALAW:
            s[n++] = voice_alaw_decode(b);
            break;
        default:
            n = rv_adpcm_decode(&dev->vcodec, &b, 1, s, 8);
            break;
    }
    modem_vtxq_put(dev, rs, voice_resample(&dev->vrs, s, n, rs, 32));
}

static void modem_vrx_stop(modem_t *dev);

/* #VTX's stream: audio with DLE-shielded commands.  DLE ETX ends it once
   played out; DLE CAN throws away what is waiting; in +VTR, DLE ^ ends both
   ways at once. */
static void
modem_vtx_byte(modem_t *dev, uint8_t b)
{
    if (dev->vtx_end) {
        /* After DLE ETX the DTE goes on with commands, which wait for the
           audio to finish -- Unimodem sends "<DLE><ETX>AT<CR>" in one go. */
        if (dev->vafter_len < (int) sizeof(dev->vafter))
            dev->vafter[dev->vafter_len++] = b;
        return;
    }
    if (dev->vdle) {
        dev->vdle = 0;
        switch (b) {
            case DLE:
                modem_vtx_data(dev, DLE);
                break;
            case ETX:
                dev->vtx_end = 1;
                break;
            case CAN:
            case '!':
                dev->vtxq_tail = dev->vtxq_head;
                if (b == '!')
                    dev->vtx_end = 1;
                break;
            case '^':
                if (dev->vmode == VM_TR) {
                    dev->vtxq_tail = dev->vtxq_head;
                    modem_vrx_stop(dev);
                }
                break;
            default:
                break;
        }
        return;
    }
    if (b == DLE)
        dev->vdle = 1;
    else
        modem_vtx_data(dev, b);
    modem_vtx_cts(dev);
}

static void
modem_vstream_start(modem_t *dev, int mode)
{
    rv_adpcm_init(&dev->vcodec, dev->vbs);
    rv_adpcm_init(&dev->vcodec_rx, dev->vbs);
    voice_resampler_init(&dev->vrs, (uint32_t) dev->vsr, VOICE_LINE_RATE);
    voice_resampler_init(&dev->vrs_rx, VOICE_LINE_RATE, (uint32_t) dev->vsr);
    dev->vmode      = mode;
    dev->vdle       = 0;
    dev->vtx_end    = 0;
    dev->vafter_len = 0;
    dev->vlo        = -1;
    dev->vtxq_head  = 0;
    dev->vtxq_tail  = 0;
    dev->vgone_said = 0;
    dev->vsil_said  = 0;
    voice_silence_init(&dev->vsil, dev->vparam[VP_VSS]);
    /* CONNECT, plain: there is no carrier, and no speed to report. */
    if (!dev->quiet) {
        if (dev->verbose)
            modem_out_line(dev, "CONNECT");
        else {
            modem_out_byte(dev, '1');
            modem_out_byte(dev, dev->s[3]);
        }
    }
}

/* The end of #VTX, played out: OK, and whatever the DTE sent behind it. */
static void
modem_vtx_done(modem_t *dev)
{
    uint8_t after[sizeof(dev->vafter)];
    int     n = dev->vafter_len;

    memcpy(after, dev->vafter, (size_t) n);
    dev->vmode      = VM_NONE;
    dev->vtx_end    = 0;
    dev->vafter_len = 0;
    if (dev->cts_held) {
        dev->cts_held = 0;
        char_update_status(dev->port);
    }
    modem_result(dev, RES_OK);
    for (int i = 0; i < n; i++)
        modem_command_byte(dev, after[i]);
}

/* #VRX ends on anything from the DTE (+VRX on <DLE>!), +VTR on <DLE>^: DLE
   ETX, then OK.  What else came with that byte, up to the next AT, is not a
   command. */
static void
modem_vrx_stop(modem_t *dev)
{
    uint8_t tail[2];
    size_t  n = (dev->vfmt == VF_ADPCM) ? rv_adpcm_flush(&dev->vcodec_rx, tail, sizeof(tail)) : 0;

    for (size_t i = 0; i < n; i++) {
        if (tail[i] == DLE)
            modem_out_byte(dev, DLE);
        modem_out_byte(dev, tail[i]);
    }
    modem_out_byte(dev, DLE);
    modem_out_byte(dev, ETX);
    dev->vmode    = VM_NONE;
    dev->vswallow = 1;
    if (dev->cts_held) {
        dev->cts_held = 0;
        char_update_status(dev->port);
    }
    modem_result(dev, RES_OK);
}

/* 20 ms of audio for #VRX: to the serial port's rate and format, shielded. */
static void
modem_vrx_put(modem_t *dev, const int16_t *s, size_t n)
{
    int16_t rs[VOICE_FRAME_SAMPLES * 2];
    uint8_t enc[VOICE_FRAME_SAMPLES * 4];
    size_t  m = voice_resample(&dev->vrs_rx, s, n, rs, sizeof(rs) / sizeof(rs[0]));
    size_t  k = 0;

    switch (dev->vfmt) {
        case VF_U8:
            for (size_t i = 0; i < m; i++)
                enc[k++] = (uint8_t) ((rs[i] >> 8) + 128);
            break;
        case VF_S8:
            for (size_t i = 0; i < m; i++)
                enc[k++] = (uint8_t) (int8_t) (rs[i] >> 8);
            break;
        case VF_S16:
            for (size_t i = 0; i < m; i++) {
                enc[k++] = (uint8_t) rs[i];
                enc[k++] = (uint8_t) ((uint16_t) rs[i] >> 8);
            }
            break;
        case VF_ULAW:
            for (size_t i = 0; i < m; i++)
                enc[k++] = voice_ulaw_encode(rs[i]);
            break;
        case VF_ALAW:
            for (size_t i = 0; i < m; i++)
                enc[k++] = voice_alaw_encode(rs[i]);
            break;
        default:
            k = rv_adpcm_encode(&dev->vcodec_rx, rs, m, enc, sizeof(enc));
            break;
    }
    for (size_t i = 0; i < k; i++) {
        if (enc[i] == DLE)
            modem_out_byte(dev, DLE);
        modem_out_byte(dev, enc[i]);
    }
}

/* ------------------------------------------------------------ the line */

static void
modem_vrxq_put(modem_t *dev, const int16_t *s, size_t n)
{
    const uint32_t size = sizeof(dev->vrxq) / sizeof(dev->vrxq[0]);

    for (size_t i = 0; i < n; i++) {
        dev->vrxq[dev->vrxq_head] = s[i];
        dev->vrxq_head            = (dev->vrxq_head + 1) % size;
        if (dev->vrxq_head == dev->vrxq_tail)
            dev->vrxq_tail = (dev->vrxq_tail + 1) % size;
    }
    /* The far end's clock and this one are not the same clock: keep the
       wait short rather than let it grow. */
    while (((dev->vrxq_head - dev->vrxq_tail) % size) > VRXQ_KEEP)
        dev->vrxq_tail = (dev->vrxq_tail + VOICE_FRAME_SAMPLES) % size;
}

static void
modem_voice_frame_in(int type, const uint8_t *payload, size_t len, void *priv)
{
    modem_t *dev = (modem_t *) priv;

    if (type == VOICE_FRAME_AUDIO) {
        int16_t s[VOICE_FRAME_MAX];

        for (size_t i = 0; i < len; i++)
            s[i] = voice_ulaw_decode(payload[i]);
        modem_vrxq_put(dev, s, len);
    } else if ((type == VOICE_FRAME_DTMF) && (len >= 1) && (dev->vdtmf_len < (int) sizeof(dev->vdtmf)))
        dev->vdtmf[dev->vdtmf_len++] = (char) payload[0];
}

/* Whatever the far end has sent: frames from a voice end; a modem's noise
   is only heard (modem_voice_tick), not read. */
static void
modem_voice_receive(modem_t *dev)
{
    if ((dev->call == NULL) || (dev->vpeer < 0))
        return;
    for (int round = 0; round < 8; round++) {
        uint8_t   buf[1024];
        int       wouldblock = 0;
        const int r          = dev->call->recv(dev, buf, sizeof(buf), &wouldblock);

        if (r > 0) {
            if (dev->vpeer && (voice_deframe(&dev->vdefr, buf, (size_t) r, modem_voice_frame_in, dev) != 0)) {
                modem_voice_gone(dev); /* not frames: not a voice call after all */
                return;
            }
            continue;
        }
        if ((r == 0) || !wouldblock)
            modem_voice_gone(dev);
        return;
    }
}

static void
modem_voice_send(modem_t *dev, int type, const uint8_t *payload, size_t len)
{
    uint8_t        frame[VOICE_FRAME_HDR + VOICE_FRAME_MAX];
    const size_t   n    = voice_frame(frame, type, payload, len);
    const uint32_t free = MODEM_TXQ_SIZE - 1 - modem_txq_used(dev);

    if ((dev->call == NULL) || (dev->vpeer != 1) || (n > free))
        return; /* nobody to hear it, or the line has fallen behind: dropped whole */
    for (size_t i = 0; i < n; i++) {
        dev->txq[dev->txq_head] = frame[i];
        dev->txq_head           = (dev->txq_head + 1) % MODEM_TXQ_SIZE;
    }
}

/* The exchange's tones, for the handset: dial tone, ringback, busy. */
static void
modem_handset_tones(modem_t *dev, int16_t *rx)
{
    const uint32_t ms = dev->vms;
    int            on = 0;
    voice_tone_t   t;

    if (dev->vline_gone)
        on = (ms % 1000) < 500; /* busy */
    else if (dev->call == NULL)
        on = 1;                 /* dial tone */
    else if (dev->vpeer < 0)
        on = (ms % 5000) < 1000; /* ringback */
    if (!on)
        return;
    memset(&t, 0, sizeof(t));
    voice_tone_start(&t, 425.0, 0.0, VOICE_FRAME_MS, 6000.0);
    t.ph1 = 2.0 * 3.14159265358979 * 425.0 * (double) (ms % 1000) / 1000.0;
    (void) voice_tone_mix(&t, rx, VOICE_FRAME_SAMPLES);
}

/* Up to three comma-separated numbers to a closing bracket, any of them
   left out ("[440,,10]"); returns past the bracket. */
static const char *
modem_vts_group(const char *p, char close, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    for (int i = 0; (i < 3) && (*p != '\0') && (*p != close); i++) {
        if (isdigit((unsigned char) *p))
            v[i] = modem_arg(&p);
        while ((*p != '\0') && (*p != ',') && (*p != close))
            p++;
        if (*p == ',')
            p++;
    }
    while ((*p != '\0') && (*p != close))
        p++;
    return (*p == close) ? (p + 1) : p;
}

/* #VTS / +VTS: the next digit, {digit,length} or [f1,f2,length], once the
   last has played.  Lengths are #VTS's 0.1 s or +VTS's 0.01 s. */
static void
modem_vts_next(modem_t *dev)
{
    const char *p = &dev->vts[dev->vts_pos];
    double      f1;
    double      f2;
    char        digit = 0;
    uint32_t    ms    = (uint32_t) dev->vts_digit;

    if (dev->vtone.left > 0)
        return;
    while ((*p == ',') || (*p == ' '))
        p++;
    if (*p == '\0') {
        dev->vts_busy = 0;
        modem_vresult(dev, RES_OK);
        return;
    }
    if (*p == '[') {
        int v[3];

        p = modem_vts_group(p + 1, ']', v);
        voice_tone_start(&dev->vtone, (double) v[0], (double) v[1], (uint32_t) v[2] * (uint32_t) dev->vts_unit, 8000.0);
        if ((v[0] == 0) && (v[1] == 0))
            dev->vtone.amp = 0.0; /* a pause */
    } else {
        if (*p == '{') {
            int v[3];

            digit = p[1];
            p     = modem_vts_group(p + 2, '}', v);
            if (v[0] == 0)
                v[0] = v[1]; /* "{5,10}": the length after the digit's comma */
            if (v[0] > 0)
                ms = (uint32_t) v[0] * (uint32_t) dev->vts_unit;
        } else
            digit = *p++;
        if (voice_dtmf_freqs(digit, &f1, &f2)) {
            const uint8_t d = (uint8_t) digit;

            voice_tone_start(&dev->vtone, f1, f2, (ms > 0) ? ms : 100, 9000.0);
            dev->vtone.left += VOICE_LINE_RATE / 20; /* 50 ms between */
            modem_voice_send(dev, VOICE_FRAME_DTMF, &d, 1);
        }
    }
    dev->vts_pos = (int) (p - dev->vts);
}

/* 20 ms of the line, both ways. */
static void
modem_voice_tick(modem_t *dev)
{
    const int      to_line = (dev->vls == 0) || (dev->vls == 4);
    const int      hear    = dev->handset || (dev->vls == 6); /* the host listens and talks */
    const int      local   = (dev->vls == 1) || (dev->vls == 2) || (dev->vls == 3);
    const uint32_t vsize   = sizeof(dev->vrxq) / sizeof(dev->vrxq[0]);
    int16_t        tx[VOICE_FRAME_SAMPLES];
    int16_t        rx[VOICE_FRAME_SAMPLES];
    int16_t        mic[VOICE_FRAME_SAMPLES];
    const int      txing    = (dev->vmode == VM_TX) || (dev->vmode == VM_TR);
    const int      rxing    = (dev->vmode == VM_RX) || (dev->vmode == VM_TR);
    const int      want_mic = (dev->vcall && hear) || (rxing && ((dev->vls == 1) || (dev->vls == 3)));

    memset(tx, 0, sizeof(tx));
    memset(rx, 0, sizeof(rx));
    memset(mic, 0, sizeof(mic));

    /* What the far end hears: the DTE's audio, the host's voice, tones. */
    if (txing) {
        int16_t play[VOICE_FRAME_SAMPLES];

        memset(play, 0, sizeof(play));
        for (int i = 0; (i < VOICE_FRAME_SAMPLES) && (dev->vtxq_tail != dev->vtxq_head); i++) {
            play[i]        = dev->vtxq[dev->vtxq_tail];
            dev->vtxq_tail = (dev->vtxq_tail + 1) % (sizeof(dev->vtxq) / sizeof(dev->vtxq[0]));
        }
        if (to_line)
            memcpy(tx, play, sizeof(tx));
        else if ((dev->vls == 1) || (dev->vls == 2))
            modem_sound_voice(dev->snd, play, VOICE_FRAME_SAMPLES); /* the handset's or the modem's speaker */
        modem_vtx_cts(dev);
    }
    modem_voice_mic(dev, want_mic);
    if (want_mic)
        (void) snd_mic_read(mic, VOICE_FRAME_SAMPLES);
    if (dev->vcall && hear) {
        for (int i = 0; i < VOICE_FRAME_SAMPLES; i++) {
            const int32_t v = tx[i] + mic[i];

            tx[i] = (int16_t) ((v > 32767) ? 32767 : ((v < -32768) ? -32768 : v));
        }
    }
    if (dev->vts_busy) {
        if (!voice_tone_mix(&dev->vtone, tx, VOICE_FRAME_SAMPLES))
            modem_vts_next(dev);
    }
    if (dev->vcall && (dev->call != NULL) && (dev->vpeer == 1) && !dev->vline_gone) {
        uint8_t u[VOICE_FRAME_SAMPLES];

        for (int i = 0; i < VOICE_FRAME_SAMPLES; i++)
            u[i] = voice_ulaw_encode(tx[i]);
        modem_voice_send(dev, VOICE_FRAME_AUDIO, u, sizeof(u));
    }

    /* What the line brings: the far end's audio, or a modem's tones. */
    if ((dev->call != NULL) && (dev->vpeer == 1)) {
        for (int i = 0; (i < VOICE_FRAME_SAMPLES) && (dev->vrxq_tail != dev->vrxq_head); i++) {
            rx[i]          = dev->vrxq[dev->vrxq_tail];
            dev->vrxq_tail = (dev->vrxq_tail + 1) % vsize;
        }
    } else if ((dev->call != NULL) && (dev->vpeer == 0)) {
        /* A modem: its answer tone if it answered this call, its calling
           tone (1300 Hz, 0.5 s in every 2.5) if it made it. */
        if (dev->vsynth.left == 0) {
            if (!dev->vanswered) {
                voice_tone_start(&dev->vsynth, 2100.0, 0.0, 1000, 9000.0);
                modem_vdte_event(dev, 'a');
            } else if ((dev->vms % 2500) < VOICE_FRAME_MS) {
                voice_tone_start(&dev->vsynth, 1300.0, 0.0, 500, 9000.0);
                modem_vdte_event(dev, 'e');
            }
        }
        (void) voice_tone_mix(&dev->vsynth, rx, VOICE_FRAME_SAMPLES);
    }
    if (dev->handset_call || (dev->handset && dev->vcall))
        modem_handset_tones(dev, rx);
    if (dev->vcall && hear)
        modem_sound_voice(dev->snd, rx, VOICE_FRAME_SAMPLES);

    /* And what the DTE is told. */
    if (dev->vmode != VM_NONE) {
        for (int i = 0; i < dev->vdtmf_len; i++)
            modem_vdte_event(dev, dev->vdtmf[i]);
        if (dev->vline_gone && !dev->vgone_said) {
            dev->vgone_said = 1;
            modem_vdte_event(dev, 'b');
        }
    }
    dev->vdtmf_len = 0;
    if (rxing) {
        const int16_t *src = local ? mic : rx;

        voice_silence_feed(&dev->vsil, src, VOICE_FRAME_SAMPLES);
        if ((dev->vsil.quiet_ms >= (uint32_t) dev->vparam[VP_VSP] * 100u) && (dev->vparam[VP_VSP] > 0)) {
            if (!dev->vsil_said) {
                /* Rockwell says s; V.253 tells quiet after a voice (q) from
                   no voice at all (s). */
                dev->vsil_said = 1;
                modem_vdte_event(dev, ((dev->vset == VSET_V253) && (dev->vsil.heard_ms > 0)) ? 'q' : 's');
            }
        } else if (dev->vsil.quiet_ms == 0)
            dev->vsil_said = 0;
        modem_vrx_put(dev, src, VOICE_FRAME_SAMPLES);
    }
    if ((dev->vmode == VM_TX) && dev->vtx_end && (dev->vtxq_tail == dev->vtxq_head))
        modem_vtx_done(dev);
}

/* From modem_read(): the call, the frames in, and the line's clock. */
static void
modem_voice_poll(modem_t *dev)
{
    const uint32_t now = plat_get_ticks();

    if ((dev->state != MODEM_ST_VOICE) && (dev->vmode == VM_NONE) && !dev->vts_busy) {
        modem_voice_mic(dev, 0);
        dev->vclock = now;
        return;
    }
    modem_voice_receive(dev);
    /* Paused, or the host was busy: pick up from now rather than catch up. */
    if ((int32_t) (now - dev->vclock) > 200)
        dev->vclock = now - VOICE_FRAME_MS;
    while ((int32_t) (now - dev->vclock) >= VOICE_FRAME_MS) {
        modem_voice_tick(dev);
        dev->vclock += VOICE_FRAME_MS;
        dev->vms += VOICE_FRAME_MS;
    }
    modem_txq_flush(dev);
}

/* --------------------------------------------------------------- #V... */

/* "=n", "?", "=?" for one number; -1 if malformed. */
static int
modem_vnum(modem_t *dev, const char **p, int *val, int min, int max, const char *range)
{
    char buf[48];

    if (**p == '?') {
        (*p)++;
        snprintf(buf, sizeof(buf), "%d", *val);
        modem_out_line(dev, buf);
        return 0;
    }
    if (**p != '=')
        return -1;
    (*p)++;
    if (**p == '?') {
        (*p)++;
        modem_out_line(dev, range);
        return 0;
    }
    {
        const int v = modem_arg(p);

        if ((v < min) || (v > max))
            return -1;
        *val = v;
    }
    /* Further values (#VTD=3F,3F,3F; #VSD=0,...): accepted, not used. */
    while (**p == ',') {
        (*p)++;
        while (isalnum((unsigned char) **p))
            (*p)++;
    }
    return 0;
}

/* #VTX/#VRX, +VTX/+VRX/+VTR: on the line, off hook -- or a local device, on
   hook or off. */
static int
modem_vstream_cmd(modem_t *dev, int mode)
{
    if ((dev->fclass != 8) || (((dev->vls == 0) || (dev->vls == 4)) && (dev->state != MODEM_ST_VOICE)) ||
        dev->handset_call)
        return RES_ERROR;
    modem_vstream_start(dev, mode);
    return RES_NONE;
}

/* #VTS / +VTS: digits, {digit,length}s and [f1,f2,length]s to the end of
   the line, lengths in `unit` ms; OK once played. */
static int
modem_vts_cmd(modem_t *dev, const char **p, int unit, int digit_ms)
{
    int n = 0;

    if ((dev->fclass != 8) || (**p != '='))
        return RES_ERROR;
    (*p)++;
    while ((**p != '\0') && (n < (int) (sizeof(dev->vts) - 1)))
        dev->vts[n++] = *(*p)++;
    dev->vts[n]     = '\0';
    dev->vts_pos    = 0;
    dev->vts_busy   = 1;
    dev->vts_unit   = unit;
    dev->vts_digit  = (digit_ms > 0) ? digit_ms : 100;
    dev->vtone.left = 0;
    return RES_NONE;
}

/* A #V command (or #CLS, #BDR...): RES_OK to go on with the line, an error,
   or RES_NONE for one that answers for itself. */
static int
modem_voice_command(modem_t *dev, const char *name, const char **p)
{
    static const struct {
        const char *name;
        int         idx;
        int         min, max;
        const char *range;
    } nums[] = {
        { "VSS", VP_VSS, 0, 3, "0-3" },
        { "VSP", VP_VSP, 0, 255, "0-255" },
        { "VRN", VP_VRN, 0, 255, "0-255" },
        { "VRA", VP_VRA, 0, 255, "0-255" },
        { "VBT", VP_VBT, 0, 255, "0-255" },
        { "VSD", VP_VSD, 0, 1, "0-1" },
        { "VGT", VP_VGT, 0, 255, "0-255" },
        { "VGR", VP_VGR, 0, 255, "0-255" },
        { NULL, 0, 0, 0, NULL }
    };

    /* A data and fax modem has no voice commands. */
    if (!dev->model->voice)
        return RES_ERROR;

    if (!strcmp(name, "CLS")) {
        int cls = dev->fclass;

        if (modem_vnum(dev, p, &cls, 0, 8, "0,8") != 0)
            return RES_ERROR;
        if ((cls != 0) && (cls != 8))
            return RES_ERROR;
        dev->fclass = cls;
        if (cls == 8)
            dev->vset = VSET_ROCKWELL;
        return RES_OK;
    }
    if (!strcmp(name, "VLS")) {
        int vls = dev->vls;

        if (modem_vnum(dev, p, &vls, 0, 7, "0,1,2,3,4,6") != 0)
            return RES_ERROR;
        dev->vls = vls;
        /* A local device -- handset, speaker, microphone, speakerphone -- is
           a voice connection of its own: VCON (vgetty waits for it). */
        return ((vls != 0) && (vls != 4)) ? RES_VCON : RES_OK;
    }
    if (!strcmp(name, "VBS")) {
        int vbs = dev->vbs;

        if (modem_vnum(dev, p, &vbs, 2, 16, "2-4,8,16") != 0)
            return RES_ERROR;
        if ((vbs > 4) && (vbs != 8) && (vbs != 16))
            return RES_ERROR;
        dev->vfmt = (vbs == 8) ? VF_U8 : ((vbs == 16) ? VF_S16 : VF_ADPCM);
        if (vbs <= 4)
            dev->vbs = vbs;
        return RES_OK;
    }
    if (!strcmp(name, "VSR")) {
        int vsr = dev->vsr;

        if (modem_vnum(dev, p, &vsr, 4000, 11025, "7200,8000,11025") != 0)
            return RES_ERROR;
        if ((vsr != 7200) && (vsr != 8000) && (vsr != 11025))
            return RES_ERROR;
        dev->vsr = vsr;
        return RES_OK;
    }
    if (!strcmp(name, "VTX") || !strcmp(name, "VRX"))
        return modem_vstream_cmd(dev, (name[1] == 'T') ? VM_TX : VM_RX);
    if (!strcmp(name, "VTS"))
        return modem_vts_cmd(dev, p, 100, dev->vparam[VP_VBT] * 100);
    if (!strcmp(name, "BDR") || !strcmp(name, "VTD") || !strcmp(name, "VSB") || !strcmp(name, "VSK") ||
        !strcmp(name, "TL") || !strcmp(name, "RG") || !strcmp(name, "VCI") || !strcmp(name, "VSM")) {
        /* Port speed, tone reporting, buffers, levels: accepted whole. */
        if (**p == '?') {
            (*p)++;
            modem_out_line(dev, "0");
            return RES_OK;
        }
        while ((**p != '\0') && (**p != ';') && (**p != '#') && (**p != '&') && (**p != '+'))
            (*p)++;
        return RES_OK;
    }
    for (int i = 0; nums[i].name != NULL; i++) {
        if (!strcmp(name, nums[i].name))
            return (modem_vnum(dev, p, &dev->vparam[nums[i].idx], nums[i].min, nums[i].max, nums[i].range) == 0)
                       ? RES_OK
                       : RES_ERROR;
    }
    return RES_ERROR;
}

/* ----------------------------------------------------------- +V... (V.253) */

/* 86Box-Next: the ITU's voice set, V.253 (IS-101 grown up) -- +FCLASS=8,
   +VLS, +VSM, +VTX/+VRX/+VTR, +VTS -- as vgetty's V253modem driver and
   Windows 2000's Unimodem/5 use it, on the same voice engine as Rockwell's
   set.  A voice connection is OK here rather than VCON; +VLS has its own
   numbering (1 is the line, 0 on hook); +VSM picks the format by number,
   listing names for software that looks them up (vgetty does). */

static const struct {
    int         cml;
    int         fmt;
    const char *desc; /* +VSM=? */
} v253_formats[] = {
    { 0, VF_S8, "0,\"SIGNED PCM\",8,0,(7200,8000,11025),(0),(0)" },
    { 1, VF_U8, "1,\"UNSIGNED PCM\",8,0,(7200,8000,11025),(0),(0)" },
    { 2, VF_S16, "2,\"SIGNED PCM\",16,0,(7200,8000,11025),(0),(0)" },
    { 4, VF_ULAW, "4,\"ULAW\",8,0,(8000),(0),(0)" },
    { 5, VF_ALAW, "5,\"ALAW\",8,0,(8000),(0),(0)" },
    { 128, VF_U8, "128,\"8-BIT LINEAR\",8,0,(7200,8000,11025),(0),(0)" },
    { 129, VF_S16, "129,\"16-BIT LINEAR\",16,0,(7200,8000,11025),(0),(0)" },
    { -1, 0, NULL }
};

/* +VLS's analogue source/destination primitives, as Rockwell's #VLS: the
   line, handset, speaker, microphone, speakerphone.  -1: none such. */
static const int v253_vls[14] = {
    0,  /*  0 on hook                          */
    0,  /*  1 the line                         */
    1,  /*  2 the handset                      */
    0,  /*  3 the line, with the handset       */
    2,  /*  4 the speaker                      */
    4,  /*  5 the line, with the speaker       */
    3,  /*  6 the microphone                   */
    6,  /*  7 the line, microphone and speaker */
    2,  /*  8 an external speaker              */
    4,  /*  9 the line, external speaker       */
    -1, /* 10                                  */
    3,  /* 11 an external microphone           */
    -1, /* 12                                  */
    6   /* 13 the line, external mic and speaker */
};

/* "=n", "=a,b,...", "?", "=?": the rest of one parameter, skipped. */
static void
modem_vskip(const char **p)
{
    while ((**p != '\0') && (**p != ';') && (**p != '+') && (**p != '#') && (**p != '&'))
        (*p)++;
}

static int
modem_v253_command(modem_t *dev, const char *name, const char **p)
{
    char buf[64];

    if (!dev->model->voice)
        return RES_ERROR;

    if (!strcmp(name, "FCLASS")) {
        if (**p == '?') {
            (*p)++;
            snprintf(buf, sizeof(buf), "%d", dev->fclass);
            modem_out_line(dev, buf);
            return RES_OK;
        }
        if (**p != '=')
            return RES_ERROR;
        (*p)++;
        if (**p == '?') {
            (*p)++;
            modem_out_line(dev, "0,8");
            return RES_OK;
        }
        {
            const int cls = modem_arg(p);

            while ((**p == '.') || isdigit((unsigned char) **p))
                (*p)++; /* 2.0 */
            /* Fax classes: accepted, as they always were here, and data. */
            dev->fclass = (cls == 8) ? 8 : 0;
            if (cls == 8)
                dev->vset = VSET_V253;
        }
        return RES_OK;
    }

    if (!strcmp(name, "VLS")) {
        int v = dev->vls253;

        if (modem_vnum(dev, p, &v, 0, 13, "0,1,2,3,4,5,6,7,8,9,11,13") != 0)
            return RES_ERROR;
        if ((dev->fclass != 8) || (v253_vls[v] < 0))
            return RES_ERROR;
        dev->vls253 = v;
        dev->vls    = v253_vls[v];
        if (v == 0) {
            /* On hook. */
            if ((dev->state == MODEM_ST_VOICE) && !dev->handset_call)
                modem_hangup(dev);
        } else if ((dev->vls == 0) || (dev->vls == 4) || (dev->vls == 6)) {
            /* The line: off hook -- which answers a call ringing. */
            if (dev->state == MODEM_ST_IDLE) {
                if (!modem_answer(dev, 1)) {
                    modem_voice_begin(dev, 1);
                    dev->vcon_said = 1;
                } else
                    return RES_NONE; /* OK once answered */
            }
        }
        return RES_OK;
    }

    if (!strcmp(name, "VSM")) {
        if (**p == '?') {
            (*p)++;
            snprintf(buf, sizeof(buf), "%d,%d,0,0", dev->vsm, dev->vsr);
            modem_out_line(dev, buf);
            return RES_OK;
        }
        if (**p != '=')
            return RES_ERROR;
        (*p)++;
        if (**p == '?') {
            (*p)++;
            for (int i = 0; v253_formats[i].desc != NULL; i++)
                modem_out_line(dev, v253_formats[i].desc);
            return RES_OK;
        }
        {
            const int cml = modem_arg(p);
            int       vsr = dev->vsr;
            int       fmt = -1;

            if (**p == ',') {
                (*p)++;
                if (isdigit((unsigned char) **p))
                    vsr = modem_arg(p);
            }
            for (int i = 0; v253_formats[i].desc != NULL; i++)
                if (v253_formats[i].cml == cml)
                    fmt = v253_formats[i].fmt;
            if ((fmt < 0) || ((vsr != 7200) && (vsr != 8000) && (vsr != 11025)))
                return RES_ERROR;
            dev->vsm  = cml;
            dev->vfmt = fmt;
            dev->vsr  = vsr;
            while (**p == ',') { /* silence compression: none here */
                (*p)++;
                (void) modem_arg(p);
            }
        }
        return RES_OK;
    }

    if (!strcmp(name, "VTX") || !strcmp(name, "VRX") || !strcmp(name, "VTR")) {
        if (**p == '?') {
            (*p)++;
            return RES_OK;
        }
        return modem_vstream_cmd(dev, (name[2] == 'R') ? VM_TR : ((name[1] == 'T') ? VM_TX : VM_RX));
    }
    if (!strcmp(name, "VTS"))
        return modem_vts_cmd(dev, p, 10, dev->vtd * 10);
    if (!strcmp(name, "VTD"))
        return (modem_vnum(dev, p, &dev->vtd, 0, 255, "0-255") == 0) ? RES_OK : RES_ERROR;

    if (!strcmp(name, "VSD")) {
        /* <sensitivity, 128 nominal>,<silence, 0.1 s> */
        if (**p == '?') {
            (*p)++;
            snprintf(buf, sizeof(buf), "%d,%d", dev->vsds, dev->vparam[VP_VSP]);
            modem_out_line(dev, buf);
            return RES_OK;
        }
        if (**p != '=')
            return RES_ERROR;
        (*p)++;
        if (**p == '?') {
            (*p)++;
            modem_out_line(dev, "(0-255),(0-255)");
            return RES_OK;
        }
        dev->vsds = modem_arg(p);
        if (dev->vsds > 255)
            return RES_ERROR;
        dev->vparam[VP_VSS] = dev->vsds / 64;
        if (**p == ',') {
            (*p)++;
            dev->vparam[VP_VSP] = modem_arg(p);
        }
        return RES_OK;
    }
    if (!strcmp(name, "VGT"))
        return (modem_vnum(dev, p, &dev->vparam[VP_VGT], 0, 255, "0-255") == 0) ? RES_OK : RES_ERROR;
    if (!strcmp(name, "VGR"))
        return (modem_vnum(dev, p, &dev->vparam[VP_VGR], 0, 255, "0-255") == 0) ? RES_OK : RES_ERROR;
    if (!strcmp(name, "VRN"))
        return (modem_vnum(dev, p, &dev->vparam[VP_VRN], 0, 255, "0-255") == 0) ? RES_OK : RES_ERROR;
    if (!strcmp(name, "VRA"))
        return (modem_vnum(dev, p, &dev->vparam[VP_VRA], 0, 255, "0-255") == 0) ? RES_OK : RES_ERROR;
    if (!strcmp(name, "VIP")) {
        /* The voice settings as they came. */
        const int fclass = dev->fclass;

        modem_vskip(p);
        modem_voice_defaults(dev);
        dev->fclass = fclass;
        dev->vset   = VSET_V253;
        return RES_OK;
    }
    if (!strcmp(name, "VIT") || !strcmp(name, "VNH") || !strcmp(name, "VDR") || !strcmp(name, "VEM") ||
        !strcmp(name, "VBT") || !strcmp(name, "VGM") || !strcmp(name, "VGS") || !strcmp(name, "VRL") ||
        !strcmp(name, "VPR") || !strcmp(name, "VSP")) {
        /* Timers, hang-up control, ring and event reporting, buffers, levels:
           accepted. */
        if (**p == '?') {
            (*p)++;
            modem_out_line(dev, "0");
            return RES_OK;
        }
        modem_vskip(p);
        return RES_OK;
    }
    return RES_ERROR;
}

/* The handset, from the UI, on the emulation thread.  Off hook it answers a
   call ringing, or hears the dial tone and can dial; it joins the guest's
   voice call if there is one.  Its own calls are no business of the DTE's:
   no result codes, and ATH does not end them. */
static void
modem_handset_apply(modem_t *dev, int action, const char *number)
{
    const int can_talk = dev->model->voice;

    switch (action) {
        case CHAR_MODEM_HANDSET_PICKUP:
        case CHAR_MODEM_HANDSET_DIAL:
            if (!can_talk)
                return;
            if (!dev->handset) {
                dev->handset = 1;
                if (dev->state == MODEM_ST_IDLE) {
                    dev->handset_call = 1;
                    if ((dev->ring_id == 0) || (dev->line != MODEM_LINE_PHONE) || !modem_answer(dev, 1)) {
                        modem_voice_begin(dev, 1); /* a dial tone */
                        dev->vcon_said = 1;
                    }
                    dev->handset_call = 1;
                }
                char_modem_log(dev->log, "handset off hook\n");
            }
            if ((action == CHAR_MODEM_HANDSET_DIAL) && dev->handset_call && (dev->state == MODEM_ST_VOICE) &&
                (dev->call == NULL) && !dev->vline_gone && (number != NULL) && (number[0] != '\0')) {
                char_modem_log(dev->log, "handset dials %s\n", number);
                if (dev->line == MODEM_LINE_PHONE)
                    modem_dial(dev, number, 1);
                if (dev->state != MODEM_ST_VOICE) {
                    /* Nobody to reach: off hook, hearing the busy tone. */
                    modem_hangup(dev);
                    modem_voice_begin(dev, 1);
                    dev->vcon_said = 1;
                    dev->handset_call = 1;
                } else
                    dev->vcon_said = 1;
                if ((dev->call == NULL) && (dev->line != MODEM_LINE_PHONE))
                    dev->vline_gone = 1;
            }
            break;

        case CHAR_MODEM_HANDSET_HANGUP:
            if (!dev->handset)
                return;
            dev->handset = 0;
            if (dev->handset_call)
                modem_hangup(dev);
            char_modem_log(dev->log, "handset on hook\n");
            break;

        default:
            break;
    }
}

static void
modem_apply_handset(modem_t *dev)
{
    int  action;
    char number[sizeof(dev->pend_dial)];

    if (!dev->pend_handset)
        return;
    modem_lock();
    action            = dev->pend_handset - 1;
    dev->pend_handset = 0;
    memcpy(number, dev->pend_dial, sizeof(number));
    modem_unlock();
    modem_handset_apply(dev, action, number);
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

/* 86Box-Next: the serial PnP ID string (Microsoft's Plug and Play External
   COM Device Specification 1.00), 7-bit form: '(', the revision (1.00, as
   two 6-bit halves), the EISA ID, then '\'-separated serial number (none),
   class, compatible IDs (none) and description, the checksum -- the sum of
   every character from '(' to ')' but itself, as two hex digits -- and ')'.
   Windows' serenum and Linux's probes check it. */
static int
modem_pnp_string(const modem_t *dev, char *buf, size_t size)
{
    unsigned sum = 0;
    int      n;

    if (dev->model->pnp_id == NULL)
        return 0;
    n = snprintf(buf, size, "(%c%c%s\\\\MODEM\\\\%s", 0x01, 0x24, dev->model->pnp_id,
                 dev->model->pnp_name ? dev->model->pnp_name : "");
    if ((n < 0) || ((size_t) (n + 4) > size))
        return 0;
    for (int i = 0; i < n; i++)
        sum += (uint8_t) buf[i];
    sum += ')';
    n += snprintf(buf + n, size - n, "%02X)", sum & 0xff);
    return n;
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
    } else if ((n == 9) && (dev->model->info[9] == NULL) && modem_pnp_string(dev, buf, sizeof(buf)))
        modem_out_line(dev, buf); /* 86Box-Next: Rockwell's ATI9 is the PnP ID */
    else if (dev->model->info[n] != NULL)
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
    dev->cid       = 0;
    modem_voice_defaults(dev);
    modem_sound_speaker(dev->snd, dev->spk_mode, dev->spk_level);
}

/* Caller ID on or off: "=n", "?" or "=?".  -1 if malformed. */
static int
modem_cid_command(modem_t *dev, const char **p, const char *name)
{
    char buf[24];

    if (**p == '?') {
        (*p)++;
        snprintf(buf, sizeof(buf), "%s: %d", name, dev->cid);
        modem_out_line(dev, buf);
        return 0;
    }
    if (**p != '=')
        return -1;
    (*p)++;
    if (**p == '?') {
        (*p)++;
        modem_out_line(dev, "(0-1)");
        return 0;
    }
    dev->cid = !!modem_arg(p);
    return 0;
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

            case 'A': /* answer: a call ringing on the telephone network */
                if ((dev->state == MODEM_ST_VOICE) && dev->vanswered && (dev->call != NULL) && !dev->handset_call)
                    return modem_vcon(dev); /* +VLS=1 answered it already */
                if (dev->state != MODEM_ST_IDLE)
                    return RES_ERROR;
                if (modem_answer(dev, dev->fclass == 8))
                    return RES_NONE;
                if (dev->fclass == 8) {
                    /* Nothing ringing: off hook all the same. */
                    modem_voice_begin(dev, 1);
                    dev->vcon_said = 1;
                    return modem_vcon(dev);
                }
                return RES_NO_CARRIER;

            case 'D': { /* dial */
                char number[64];
                int  n    = 0;
                int  semi = 0;

                /* On hook, or off hook in voice mode with nothing on the line
                   yet (ATH1, +VLS=1): a dial tone to dial on. */
                if ((dev->state != MODEM_ST_IDLE) &&
                    !((dev->state == MODEM_ST_VOICE) && (dev->call == NULL) && !dev->vline_gone && !dev->handset_call))
                    return (dev->handset_call ? RES_NO_DIALTONE : RES_ERROR);
                while ((*p != '\0') && (n < ((int) sizeof(number) - 1))) {
                    const char d = *p++;

                    /* The digits, and what shapes the dialling: a comma (wait S8),
                       W (wait for dial tone), T and P (tone or pulse). */
                    if (isdigit((unsigned char) d) || (d == '*') || (d == '#') || (d == ',') ||
                        (strchr("WwTtPp", d) != NULL))
                        number[n++] = d;
                    /* ';': back to command mode once dialled -- a voice call,
                       for the phone beside the modem. */
                    semi |= (d == ';');
                }
                number[n] = '\0';
                modem_dial(dev, number, (dev->fclass == 8) || semi);
                if (semi && (dev->state == MODEM_ST_VOICE))
                    dev->vok_after_dial = 1;
                return RES_NONE;
            }

            case 'E':
                dev->echo = !!modem_arg(&p);
                break;

            case 'H':
                if (modem_arg(&p) == 0) {
                    /* The handset's call is the handset's: the modem's own
                       hook going down does not end it. */
                    if (!dev->handset_call)
                        modem_hangup(dev);
                } else if ((dev->fclass == 8) && (dev->state == MODEM_ST_IDLE)) {
                    modem_voice_begin(dev, 1); /* ATH1: off hook */
                    dev->vcon_said = 1;
                    return modem_vcon(dev);
                }
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

            case '#': { /* Rockwell's own set: #CID, caller ID, and voice */
                char name[16];
                int  n = 0;

                while (isalpha((unsigned char) *p) && (n < ((int) sizeof(name) - 1)))
                    name[n++] = (char) toupper((unsigned char) *p++);
                name[n] = '\0';
                if (!strcmp(name, "CID")) {
                    if (modem_cid_command(dev, &p, "#CID") != 0)
                        return RES_ERROR;
                } else {
                    const int r = modem_voice_command(dev, name, &p);

                    if (r != RES_OK)
                        return r; /* an error, or a command that answers for itself */
                }
                break;
            }

            case '+': { /* the ITU extended set */
                char name[16];
                int  n = 0;

                while ((*p != '\0') && (*p != '=') && (*p != '?') && (*p != '+') && (*p != ';') &&
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
                } else if (!strcmp(name, "VCID")) {
                    if (modem_cid_command(dev, &p, "+VCID") != 0)
                        return RES_ERROR;
                } else if (!strcmp(name, "FCLASS") || (name[0] == 'V')) {
                    const int r = modem_v253_command(dev, name, &p);

                    if (r != RES_OK)
                        return r;
                } else if (!strcmp(name, "MS") || !strcmp(name, "IFC") || !strcmp(name, "ES")) {
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

/* The modems plugged in, by slot: the COM ports, then the modems other
   devices carry (MODEM_CARRIED of them at once), each in the first slot free.
   The UI thread reads and queues; the emulation thread opens, closes and
   applies. */
#define MODEM_CARRIED 8
#define MODEM_SLOTS   (SERIAL_MAX + MODEM_CARRIED)
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
       otherwise every dial would stall on a connect to port 0.  The
       telephone network has an address of its own. */
    if (line == MODEM_LINE_PHONE)
        dev->line = MODEM_LINE_PHONE;
    else
        dev->line = ((line == MODEM_LINE_TCP) && (dev->host[0] != '\0')) ? MODEM_LINE_TCP : MODEM_LINE_DEAD;
}

/* The number asked for and the exchange's address, "host" or "host:port";
   1 if either changed. */
static int
modem_set_phone(modem_t *dev, const char *number, const char *exchange)
{
    char host[128];
    int  port = CHAR_MODEM_ISP_PORT;
    int  changed;

    snprintf(host, sizeof(host), "%s", (exchange && exchange[0]) ? exchange : CHAR_MODEM_EXCHANGE);
    {
        char *colon = strrchr(host, ':');

        if (colon != NULL) {
            port   = atoi(colon + 1);
            *colon = '\0';
        }
    }
    if ((port < 1) || (port > 65535))
        port = CHAR_MODEM_ISP_PORT;
    changed = strcmp(dev->phone_cfg, number ? number : "") || strcmp(dev->exch_host, host) || (dev->exch_port != port);
    snprintf(dev->phone_cfg, sizeof(dev->phone_cfg), "%s", number ? number : "");
    snprintf(dev->exch_host, sizeof(dev->exch_host), "%s", host);
    dev->exch_port = port;
    return changed;
}

/* Take up a line change from the status bar.  A call in progress is on the
   old line, so it ends -- the way unplugging the phone cord ends it. */
static void
modem_apply_pending(modem_t *dev)
{
    char host[sizeof(dev->pend_host)];
    int  changed;

    char phone[sizeof(dev->pend_phone)];
    char exchange[sizeof(dev->pend_exchange)];
    int  phone_changed;

    if (!dev->pending)
        return;

    modem_lock();
    memcpy(host, dev->pend_host, sizeof(host));
    memcpy(phone, dev->pend_phone, sizeof(phone));
    memcpy(exchange, dev->pend_exchange, sizeof(exchange));
    changed = (dev->pend_line != dev->cfg_line) || (dev->pend_port != dev->host_port) ||
              strcmp(host, dev->host);
    modem_set_line(dev, dev->pend_line, host, dev->pend_port);
    dev->pending = 0;
    modem_unlock();
    phone_changed = modem_set_phone(dev, phone, exchange);

    /* A new number or exchange: register again. */
    if (phone_changed && (dev->ctl_state != PHONE_OFF))
        modem_phone_unplug(dev, plat_get_ticks());

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

/* "COM1", or the label a carried modem's device gave it. */
void
char_modem_slot_label(int com, char *buf, size_t len)
{
    modem_lock();
    if (char_modem_present(com))
        snprintf(buf, len, "%s", modems[com]->label);
    else if ((com >= 0) && (com < SERIAL_MAX))
        snprintf(buf, len, "COM%d", com + 1);
    else
        snprintf(buf, len, "%s", "");
    modem_unlock();
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

/* A change from the UI starts from the line as it is (or as already
   changed).  Under modem_mutex. */
static void
modem_pending_begin(modem_t *dev)
{
    if (dev->pending)
        return;
    dev->pend_line = dev->cfg_line;
    dev->pend_port = dev->host_port;
    snprintf(dev->pend_host, sizeof(dev->pend_host), "%s", dev->host);
    snprintf(dev->pend_phone, sizeof(dev->pend_phone), "%s", dev->phone_cfg);
    snprintf(dev->pend_exchange, sizeof(dev->pend_exchange), "%s:%d", dev->exch_host, dev->exch_port);
}

int
char_modem_get_phone(int com, char *want, size_t want_len, char *exchange, size_t ex_len, char *number,
                     size_t num_len)
{
    int ret = -1;

    modem_lock();
    if (char_modem_present(com)) {
        const modem_t *dev = modems[com];

        if (want != NULL)
            snprintf(want, want_len, "%s", dev->pending ? dev->pend_phone : dev->phone_cfg);
        if (exchange != NULL) {
            if (dev->pending)
                snprintf(exchange, ex_len, "%s", dev->pend_exchange);
            else
                snprintf(exchange, ex_len, "%s:%d", dev->exch_host, dev->exch_port);
        }
        if (number != NULL)
            snprintf(number, num_len, "%s", dev->number);
        ret = (dev->number[0] != '\0');
    }
    modem_unlock();
    return ret;
}

void
char_modem_set_phone(int com, const char *want, const char *exchange)
{
    modem_lock();
    if (char_modem_present(com)) {
        modem_t *dev = modems[com];

        modem_pending_begin(dev);
        snprintf(dev->pend_phone, sizeof(dev->pend_phone), "%s", want ? want : "");
        snprintf(dev->pend_exchange, sizeof(dev->pend_exchange), "%s",
                 (exchange && exchange[0]) ? exchange : CHAR_MODEM_EXCHANGE);
        dev->pending = 1;

        device_context_inst(dev->cfg_dev, dev->cfg_inst);
        device_set_config_string("phone_number", dev->pend_phone);
        device_set_config_string("exchange", dev->pend_exchange);
        device_context_restore();
    }
    modem_unlock();
}

void
char_modem_handset(int com, int action, const char *number)
{
    modem_lock();
    if (char_modem_present(com)) {
        modem_t *dev = modems[com];

        dev->pend_handset = action + 1;
        snprintf(dev->pend_dial, sizeof(dev->pend_dial), "%s", number ? number : "");
    }
    modem_unlock();
}

int
char_modem_handset_state(int com)
{
    int ret = -1;

    modem_lock();
    if (char_modem_present(com)) {
        const modem_t *dev = modems[com];

        ret = (dev->handset ? 1 : 0) | (dev->ring_id ? 2 : 0) |
              (((dev->line == MODEM_LINE_PHONE) && dev->model->voice) ? 4 : 0);
    }
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
            case MODEM_ST_VOICE:
                ret = modems[com]->handset_call ? CHAR_MODEM_HANDSET : CHAR_MODEM_VOICE;
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
        modem_pending_begin(dev);
        dev->pend_line = (line == CHAR_MODEM_LINE_TCP)     ? MODEM_LINE_TCP
                         : (line == CHAR_MODEM_LINE_PHONE) ? MODEM_LINE_PHONE
                                                           : MODEM_LINE_DEAD;
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
    modem_apply_handset(dev);
    modem_phone_poll(dev);
    modem_poll_line(dev);
    modem_voice_poll(dev);

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
       is what a real modem's flow control does: the peer's kernel holds
       it.  A full ring asks for nothing, and is not a hangup: only an
       actual receive can report one. */
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
        if ((dev->vmode == VM_TX) || (dev->vmode == VM_TR))
            modem_vtx_byte(dev, buf[i]);
        else if (dev->vmode == VM_RX)
            modem_vrx_stop(dev); /* any byte ends recording; it is not a command */
        else if (dev->vswallow && (toupper(buf[i]) != 'A'))
            ; /* the rest of what ended recording ("<DLE>E<CR>"), until the next AT */
        else if (dev->online)
            modem_data_byte(dev, buf[i]);
        else {
            dev->vswallow = 0;
            modem_command_byte(dev, buf[i]);
        }
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

    /* RI rings with the phone. */
    if (dev->ring_id && ((int32_t) (plat_get_ticks() - dev->ring_on_until) < 0))
        status |= CHAR_COM_RI;

    return status;
}

static void
modem_control(uint32_t flags, void *priv)
{
    modem_t  *dev  = (modem_t *) priv;
    const int dtr  = !!(flags & CHAR_COM_DTR);
    const int rts  = !!(flags & CHAR_COM_RTS);
    const int drop = dev->dtr && !dtr;

    /* 86Box-Next: serial PnP enumeration.  The host raises DTR with RTS off,
       waits (200 ms the first time, for modems), then raises RTS and reads
       for 240 ms.  A modem idle in command mode answers that with its PnP
       ID, whatever the port's speed.  A DTE opening the port raises both
       together, or within a moment of each other, and gets nothing. */
    if (dtr && !dev->dtr) {
        dev->pnp_armed  = !rts;
        dev->pnp_dtr_at = plat_get_ticks();
    } else if (!dtr)
        dev->pnp_armed = 0;
    if (dtr && rts && !dev->rts && dev->pnp_armed) {
        char      buf[96];
        const int n = modem_pnp_string(dev, buf, sizeof(buf));

        dev->pnp_armed = 0;
        if ((n > 0) && (dev->state == MODEM_ST_IDLE) && !dev->online &&
            ((plat_get_ticks() - dev->pnp_dtr_at) >= MODEM_PNP_MIN_MS)) {
            char_modem_log(dev->log, "serial PnP: sending %s\n", dev->model->pnp_id);
            for (int i = 0; i < n; i++)
                modem_out_byte(dev, (uint8_t) buf[i]);
        }
    }
    dev->rts = rts;
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
    modem_voice_mic(dev, 0);
    modem_phone_unplug(dev, 0);
    modem_sound_close(dev->snd);
    log_close(dev->log);
    free(dev);
}

/* A modem answering as model, its settings read from the current device
   context (cfg_dev, instance cfg_inst), going into slot (-1: none) under
   label. */
static modem_t *
modem_create(const modem_model_t *model, const device_t *cfg_dev, int slot, const char *label)
{
    modem_t    *dev = (modem_t *) calloc(1, sizeof(modem_t));
    const char *s;

    dev->model = model;
    dev->sock  = (SOCKET) -1;
    dev->ctl   = (SOCKET) -1;
    dev->vpeer = -1;
    dev->snd   = modem_sound_init(device_get_config_int("speaker"));
    modem_load_defaults(dev);

    dev->cfg_dev  = cfg_dev;
    dev->cfg_inst = device_get_instance();
    dev->slot     = slot;
    snprintf(dev->label, sizeof(dev->label), "%s", label);
    dev->connect_rate = device_get_config_int("connect_rate");

    s = device_get_config_string("host");
    modem_set_line(dev, device_get_config_int("line"), (s != NULL) ? s : "",
                   device_get_config_int("host_port"));
    {
        const char *number   = device_get_config_string("phone_number");
        const char *exchange = device_get_config_string("exchange");

        modem_set_phone(dev, number ? number : "", exchange ? exchange : "");
    }

    snprintf(dev->ident, sizeof(dev->ident), "%s", dev->model->ident);
    snprintf(dev->firmware, sizeof(dev->firmware), "%s", dev->model->fmw);

    dev->port = char_attach(0, modem_read, modem_write, modem_status,
                            modem_control, modem_port_config, dev);
    dev->log  = char_log_open(dev->port, "Modem");

    char_modem_log(dev->log, "init(): %s, ATI%d \"%s\", ATI%d \"%s\", line %s\n",
                   dev->model->name, dev->model->ident_at, dev->ident,
                   dev->model->fmw_at, dev->firmware, modem_line_name(dev));

    modem_lock();
    if (dev->slot >= 0)
        modems[dev->slot] = dev;
    modem_unlock();

    return dev;
}

static void *
modem_init(const device_t *info)
{
    const int inst  = device_get_instance();
    const int model = ((info->local >= 0) && (info->local < (int) (sizeof(modem_models) / sizeof(modem_models[0]))))
        ? info->local
        : MODEM_MODEL_SUPRA;
    char      label[16] = "";

    if ((inst >= 1) && (inst <= SERIAL_MAX))
        snprintf(label, sizeof(label), "COM%d", inst);
    if (modem_mutex == NULL)
        modem_mutex = thread_create_mutex();
    return modem_create(&modem_models[model], info, ((inst >= 1) && (inst <= SERIAL_MAX)) ? (inst - 1) : -1, label);
}

/* 86Box-Next: a modem another device carries, made by char_modem_attach()
   through char_open_unlisted() -- which gives char_attach() the device's
   port -- in that device's context, whose settings are the modem's. */
static const modem_model_t *attach_model;
static const char          *attach_label;

static void *
modem_carried_init(UNUSED(const device_t *info))
{
    int slot = -1;

    if (modem_mutex == NULL)
        modem_mutex = thread_create_mutex();
    modem_lock();
    for (int i = SERIAL_MAX; i < MODEM_SLOTS; i++) {
        if (modems[i] == NULL) {
            slot = i;
            break;
        }
    }
    modem_unlock();
    return modem_create(attach_model, device_context_get_device(), slot, attach_label);
}

static const device_t modem_carried_device = {
    .name          = "Modem",
    .internal_name = "modem_carried",
    .flags         = 0,
    .local         = 0,
    .init          = modem_carried_init,
    .close         = modem_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

void *
char_modem_attach(char_port_t *port, const char_modem_model_t *model, const char *label)
{
    void *priv;

    if ((port == NULL) || (model == NULL))
        return NULL;
    attach_model = model;
    attach_label = label ? label : "";
    priv         = char_open_unlisted(port, &modem_carried_device);
    attach_model = NULL;
    attach_label = NULL;
    return priv;
}

void
char_modem_detach(void *modem)
{
    if (modem != NULL)
        modem_close(modem);
}

/* The config of all three parts, and -- from the same entries -- of a device that
   carries a modem.  What differs between the parts -- which ATIn carries
   what, and the text of each -- lives in the model table. */
// clang-format off
static const device_config_t modem_config[] = {
    CHAR_MODEM_CONFIG_LINE,
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
    CHAR_MODEM_CONFIG_SPEAKER,
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

const device_t char_modem_hayes_com_device = {
    .name          = "Standard Hayes-compatible 56k Modem",
    .internal_name = "modem_hayes",
    .flags         = DEVICE_COM | DEVICE_HOTPLUG,
    .local         = MODEM_MODEL_HAYES,
    .init          = modem_init,
    .close         = modem_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = modem_config
};
