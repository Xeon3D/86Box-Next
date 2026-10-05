/*
 * 86Box-Next: the COM port modems (src/char/char_modem.c), driven the way the
 * funworld Photo Play cabinets' FN_SYS.EXE drives the real part, with the
 * cabinet's own identification algorithm run over the answers.  Ported from
 * PeepeeBox's tools/modemtest.  No test framework; non-zero on failure.
 *
 * The tables below are `\FN_SYS\DATABASE\NETWORK\MD_NAME.CSV` and
 * `MD_INFOS.CSV`, transcribed from two images: the fourteen-row 2001 one from
 * PP2001NL-MASTERS-NSB H9751 SR1, and the thirty-row I.G.O. 6 one from
 * IGO 6 DE ND003.  A model passes only when it resolves to exactly one row in
 * both -- not when it happens to contain the right substring.
 *
 * Also: the order of events on a connect, and the line changed from the
 * status bar (char_modem_set_line) while the machine runs.
 *
 * 86Box-Next: the line's bytes, exactly -- short sends, would-block, a full
 * receive ring, CTS, EOF and errors, DTR, +++ and ATH, two modems at once --
 * and calls to the built-in ISP (the real one, src/network/isp/, with libslirp)
 * by a scripted PPP client talking through the modem as a guest's UART would,
 * routed to a UDP socket on the host's loopback.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/char.h>
#include <86box/plat_netsocket.h>
#include <86box/thread.h>
#include <86box/modem_sound.h>
#include <86box/char_modem.h>
#include <86box/isp.h>
#ifndef _WIN32
#    include <arpa/inet.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif
#include "ppp_client.h"

/* ------------------------------------------------------------ 86Box stubs */
/* One char port per modem; `tp` is the one the helpers below talk to. */
static char_port_t  ports[4];
static int          next_port;
static char_port_t *tp = &ports[0];
#define test_port (*tp)

char_port_t *
char_attach(uint32_t flags,
            size_t (*read)(uint8_t *, size_t, void *),
            size_t (*write)(uint8_t *, size_t, void *),
            uint32_t (*status)(void *),
            void (*control)(uint32_t, void *),
            void (*port_config)(void *), void *priv)
{
    tp = &ports[next_port++ % 4];
    memset(tp, 0, sizeof(*tp));
    test_port.chardev.flags       = flags;
    test_port.chardev.read        = read;
    test_port.chardev.write       = write;
    test_port.chardev.status      = status;
    test_port.chardev.control     = control;
    test_port.chardev.port_config = port_config;
    test_port.chardev.priv        = priv;
    test_port.attached            = 1;
    return &test_port;
}

void  char_update_status(char_port_t *p) { (void) p; }
void *char_log_open(char_port_t *p, char *n) { (void) p; (void) n; return NULL; }
void  log_close(void *p) { (void) p; }
void  log_out(void *p, const char *f, va_list a) { (void) p; (void) f; (void) a; }

/* One mutex is all the modem asks for; this test has one thread. */
mutex_t *thread_create_mutex(void) { return (mutex_t *) 1; }
int      thread_wait_mutex(mutex_t *m) { (void) m; return 1; }
int      thread_release_mutex(mutex_t *m) { (void) m; return 1; }

/* The device context: COM2 (instance 2), and what the status bar saves. */
static int  saved_line = -1;
static char saved_host[128];
static int  saved_port = -1;

static int fake_instance = 2;

int  device_get_instance(void) { return fake_instance; }
void device_context_inst(const device_t *d, int inst) { (void) d; (void) inst; }
void device_context_restore(void) { }
const device_t *device_context_get_device(void) { return NULL; }

void
device_set_config_int(const char *name, int val)
{
    if (!strcmp(name, "line"))
        saved_line = val;
    else if (!strcmp(name, "host_port"))
        saved_port = val;
}

void
device_set_config_string(const char *name, const char *val)
{
    if (!strcmp(name, "host"))
        snprintf(saved_host, sizeof(saved_host), "%s", val);
}

/* The speaker is silent here, and a call's parts take fixed, round times:
   dialling 1 s, the far end's ring 2 s, training 2 s. */
modem_sound_t *modem_sound_init(int enabled) { (void) enabled; return NULL; }
void     modem_sound_close(modem_sound_t *s) { (void) s; }
void     modem_sound_event(modem_sound_t *s, int t, const char *n, int a) { (void) s; (void) t; (void) n; (void) a; }
void     modem_sound_speaker(modem_sound_t *s, int m, int l) { (void) s; (void) m; (void) l; }
void     modem_sound_country(modem_sound_t *s, int uk, int v90) { (void) s; (void) uk; (void) v90; }
uint32_t modem_sound_dial_ms(const char *n, int s8, int p) { (void) n; (void) s8; (void) p; return 1000; }
uint32_t modem_sound_ring_ms(void) { return 2000; }
uint32_t modem_sound_handshake_ms(int v90) { (void) v90; return 2000; }

/* The config the device would have read out of the ini.  Blank identity strings,
   so the model table's own answers are what gets tested. */
static int fake_line = 0; /* 0 dead, 1 a TCP host that always answers */

int
device_get_config_int(const char *name)
{
    if (!strcmp(name, "line"))
        return fake_line;
    if (!strcmp(name, "host_port"))
        return 23;
    if (!strcmp(name, "connect_rate"))
        return 57600;
    return 0;
}

const char *
device_get_config_string(const char *name)
{
    if (!strcmp(name, "host") && fake_line)
        return "127.0.0.1";
    return "";
}

static uint32_t fake_ticks = 100000;

uint32_t plat_get_ticks(void) { return fake_ticks; }

/* With fake_line set the "network" is a socket that connects at once.  Each
   records what the modem sent and can be told to take only so much per call
   (a short send), to report "would block" for a number of calls, to fail,
   and to deliver bytes, an EOF or an error. */
typedef struct {
    int      used;
    int      closed;
    uint8_t  sent[65536];
    size_t   sent_len;
    unsigned send_calls;
    unsigned send_chunk; /* 0: all of it */
    unsigned send_block; /* calls left that would block */
    int      send_error;
    uint8_t  rx[65536];
    size_t   rx_len;
    size_t   rx_pos;
    int      rx_eof;
    int      rx_error;
} fake_sock_t;

static fake_sock_t fsocks[4];
static int         last_sock = -1;

static fake_sock_t *
fsock(SOCKET s)
{
    return &fsocks[(int) s - 100];
}

SOCKET
plat_netsocket_create(int t)
{
    (void) t;
    if (!fake_line)
        return (SOCKET) -1;
    for (int i = 0; i < 4; i++) {
        if (!fsocks[i].used) {
            memset(&fsocks[i], 0, sizeof(fsocks[i]));
            fsocks[i].used = 1;
            last_sock      = i;
            return (SOCKET) (100 + i);
        }
    }
    return (SOCKET) -1;
}

void
plat_netsocket_close(SOCKET s)
{
    fsock(s)->closed = 1;
    fsock(s)->used   = 0;
}

int plat_netsocket_connect(SOCKET s, const char *h, unsigned short p) { (void) s; (void) h; (void) p; return fake_line ? 0 : -1; }
int plat_netsocket_connected(SOCKET s) { (void) s; return fake_line ? 1 : -1; }

int
plat_netsocket_send(SOCKET s, const unsigned char *d, unsigned int n, int *w)
{
    fake_sock_t *f = fsock(s);

    f->send_calls++;
    *w = 0;
    if (f->send_error)
        return -1;
    if (f->send_block > 0) {
        f->send_block--;
        *w = 1;
        return -1;
    }
    if (f->send_chunk && (n > f->send_chunk))
        n = f->send_chunk;
    if (n > (sizeof(f->sent) - f->sent_len))
        n = (unsigned int) (sizeof(f->sent) - f->sent_len);
    memcpy(&f->sent[f->sent_len], d, n);
    f->sent_len += n;
    return (int) n;
}

int
plat_netsocket_receive(SOCKET s, unsigned char *d, unsigned int n, int *w)
{
    fake_sock_t *f = fsock(s);

    *w = 0;
    if (f->rx_pos < f->rx_len) {
        if (n > (f->rx_len - f->rx_pos))
            n = (unsigned int) (f->rx_len - f->rx_pos);
        memcpy(d, &f->rx[f->rx_pos], n);
        f->rx_pos += n;
        return (int) n;
    }
    if (f->rx_eof)
        return 0;
    if (f->rx_error)
        return -1;
    *w = 1;
    return -1;
}

/* ---------------------------------------------- the cabinet's own tables */

typedef struct {
    int         id;
    int         ati;  /* which ATIn to ask */
    const char *want; /* the substring to look for in the answer */
} info_token_t;

typedef struct {
    const char *name;
    int         tok[4];
} name_row_t;

/* MD_INFOS.CSV -- I.G.O. 6.  The 2001 table is ids 9..28 of this, unchanged. */
static const info_token_t infos[] = {
    {  9, 6, "MicroLink"    }, { 10, 6, "33.6"       }, { 11, 6, "56"        },
    { 12, 6, "TanGo"        }, { 13, 6, "1000"       }, { 14, 6, "ISDN"      },
    { 15, 3, "SupraExpress" }, { 16, 4, "Sportster"  }, { 17, 4, "ISDN"      },
    { 18, 6, "Internet"     }, { 19, 0, "WAVECOM"    }, { 20, 0, "1800"      },
    { 21, 0, "900"          }, { 22, 6, "SIEMENS"    }, { 23, 6, "M20"       },
    { 24, 3, "Sportster"    }, { 25, 3, "56000"      }, { 26, 3, "Voice"     },
    { 27, 3, "P2107-V90"    }, { 28, 3, "V.92"       }, { 29, 0, "Nokia"     },
    { 30, 3, "Nokia 30"     }, { 31, 3, "TP560"      }, { 32, 6, "-i"        },
    { 33, 1, "Riser"        }, { 34, 1, "Voice"      }, { 35, 6, "2a/b"      },
    { 36, 0, "1292"         }, { 37, 0, "1.2"        }, { 38, 6, "k i"       },
    { 39, 6, "pro"          }, { 40, 0, "TA+PP2"     }, { 41, 3, "Tornado"   },
    { 42, 5, "ROPER FLYING" }, { 43, 0, "TINTORETTO" }, { 44, 0, "251"       },
    { 45, 3, "ISDN adapter" }, { 46, 3, "ZyXEL"      }, { 47, 1, "SmartUSB56"},
    { 49, 0, "MULTIBAND"    }, { 50, 0, "900E"       }, { 51, 0, "56000"     },
    { 52, 1, "255"          }, { 0, 0, NULL }
};

/* MD_NAME.CSV -- I.G.O. 6 DE ND003, all thirty rows. */
static const name_row_t names_igo6[] = {
    { "ELSA MicroLink 33.6TQV",              {  9, 10, -1, -1 } },
    { "ELSA MicroLink 56k",                  {  9, 11, -1, -1 } },
    { "Diamond SupraExpress 56e PRO",        { 15, -1, -1, -1 } },
    { "ELSA TanGo 1000",                     { 12, 13, -1, -1 } },
    { "ELSA MicroLink ISDN",                 {  9, 14, -1, -1 } },
    { "U.S. Robotics Sportster ISDN TA",     { 16, 17, -1, -1 } },
    { "ELSA MicroLink 56k Internet",         {  9, 11, 18, -1 } },
    { "Wavecom 1800",                        { 19, 20, -1, -1 } },
    { "Wavecom 900",                         { 19, 21, -1, -1 } },
    { "Siemens M20",                         { 22, 23, -1, -1 } },
    { "U.S. Robotics Sportster 56000 Voice", { 24, 25, 26, -1 } },
    { "ELSA MicroLink ISDN Internet",        {  9, 14, 18, -1 } },
    { "BestMatic Conexant 56k",              { 27, 11, -1, -1 } },
    { "Diamond SupraExpress 56e PRO V.92",   { 15, 28, -1, -1 } },
    { "Nokia 30",                            { 29, 30, -1, -1 } },
    { "Topic 56k",                           { 31, -1, -1, -1 } },
    { "ELSA MicroLink 56k i",                {  9, 11, 32, 38 } },
    { "56K Voice Modem Riser",               { 33, 34, -1, -1 } },
    { "ELSA Microlink 2ab PnP",              {  9, 14, 35, -1 } },
    { "DrayTek ISDN PPP",                    { 36, 37, -1, -1 } },
    { "ELSA MicroLink 56k pro",              {  9, 11, 39, -1 } },
    { "Stollmann TA+PP2",                    { 40, -1, -1, -1 } },
    { "Tornado WebJet 128",                  { 41, -1, -1, -1 } },
    { "AsusCom ISDNLink TA",                 { 42, -1, -1, -1 } },
    { "Tintoretto ISDN",                     { 43, -1, -1, -1 } },
    { "WebJet Pocket USB",                   { 44, 45, -1, -1 } },
    { "ZyXEL Omni 56K USB",                  { 46, -1, -1, -1 } },
    { "SmartUSB56 Voice Modem",              { 47, 34, -1, -1 } },
    { "Wavecom GPRS",                        { 49, 19, 50, 20 } },
    { "INSYS Pocket 56k Modem",              { 51, 52, -1, -1 } },
    { NULL, { 0, 0, 0, 0 } }
};

/* MD_NAME.CSV -- PP2001NL-MASTERS H9751 SR1.  Fourteen rows, and row 13 is
   weaker here than in I.G.O. 6: one token rather than two. */
static const name_row_t names_2001[] = {
    { "ELSA MicroLink 33.6TQV",              {  9, 10, -1, -1 } },
    { "ELSA MicroLink 56k",                  {  9, 11, -1, -1 } },
    { "Diamond SupraExpress 56e PRO",        { 15, -1, -1, -1 } },
    { "ELSA TanGo 1000",                     { 12, 13, -1, -1 } },
    { "ELSA MicroLink ISDN",                 {  9, 14, -1, -1 } },
    { "U.S. Robotics Sportster ISDN TA",     { 16, 17, -1, -1 } },
    { "ELSA MicroLink 56k Internet",         {  9, 11, 18, -1 } },
    { "Wavecom 1800",                        { 19, 20, -1, -1 } },
    { "Wavecom 900",                         { 19, 21, -1, -1 } },
    { "Siemens M20",                         { 22, 23, -1, -1 } },
    { "U.S. Robotics Sportster 56000 Voice", { 24, 25, 26, -1 } },
    { "ELSA MicroLink ISDN Internet",        {  9, 14, 18, -1 } },
    { "BestMatic Conexant 56k",              { 27, -1, -1, -1 } },
    { "Diamond SupraExpress 56e PRO V.92",   { 15, 28, -1, -1 } },
    { NULL, { 0, 0, 0, 0 } }
};

/* ------------------------------------------------------------------ driver */
extern const device_t char_modem_supra_com_device;
extern const device_t char_modem_elsa_com_device;

static void *dev;

static void
send_str(const char *s)
{
    while (*s != '\0') {
        uint8_t c = (uint8_t) *s++;

        test_port.chardev.write(&c, 1, dev);
    }
}

/* Pull everything the modem has queued, the way the UART's receive timer does. */
static void
drain(char *out, size_t outsz)
{
    size_t n = 0;

    for (int i = 0; i < 8192; i++) {
        uint8_t b;

        if (test_port.chardev.read(&b, 1, dev) != 1)
            break;
        if (n < (outsz - 1))
            out[n++] = (char) b;
    }
    out[n] = '\0';
}

/* One AT exchange: send the line, return everything that came back. */
static const char *
at(const char *cmd)
{
    static char buf[4096];

    send_str(cmd);
    send_str("\r");
    drain(buf, sizeof(buf));
    return buf;
}

static int failures = 0;

static void
expect(const char *what, const char *got, const char *needle)
{
    const int ok = (strstr(got, needle) != NULL);

    if (!ok)
        failures++;
    printf("  %-38s %-5s wants \"%s\"\n", what, ok ? "ok" : "FAIL", needle);
    if (!ok)
        printf("      got: %s\n", got);
}

/* -------------------------------------------- the identification algorithm */

static char ati[10][256];

static void
collect_ati(void)
{
    char buf[256];

    /* Echo off first, exactly as FN_SYS does -- otherwise the command itself is
       in the answer and the matcher would be reading its own question. */
    send_str("\r\r");
    drain(buf, sizeof(buf));
    send_str("AT\r");
    drain(buf, sizeof(buf));
    send_str("ATE0\r");
    drain(buf, sizeof(buf));

    for (int n = 0; n < 10; n++) {
        char cmd[8];

        snprintf(cmd, sizeof(cmd), "ATI%d", n);
        snprintf(ati[n], sizeof(ati[n]), "%s", at(cmd));
    }
}

static int
token_matches(int id)
{
    for (int i = 0; infos[i].want != NULL; i++) {
        if (infos[i].id != id)
            continue;
        return (strstr(ati[infos[i].ati], infos[i].want) != NULL);
    }
    return 0; /* a token this table does not define cannot match */
}

/* Every row whose tokens all match, the way FN_SYS decides what it is. */
static void
identify(const char *table_name, const name_row_t *rows, const char *want)
{
    const char *hits[8];
    int         n = 0;

    for (int r = 0; rows[r].name != NULL; r++) {
        int all = 1;

        for (int t = 0; t < 4; t++) {
            if (rows[r].tok[t] < 0)
                continue;
            if (!token_matches(rows[r].tok[t])) {
                all = 0;
                break;
            }
        }
        if (all && (n < 8))
            hits[n++] = rows[r].name;
    }

    const int ok = ((n == 1) && !strcmp(hits[0], want));

    if (!ok)
        failures++;
    printf("  %-38s %-5s resolves to exactly \"%s\"\n", table_name,
           ok ? "ok" : "FAIL", want);
    if (!ok) {
        printf("      %d row(s) matched:", n);
        for (int i = 0; i < n; i++)
            printf(" [%s]", hits[i]);
        printf("\n");
    }
}

/* ------------------------------------------------------------------- suite */

static void
run_common(void)
{
    char buf[4096];

    /* The wake-up FN_SYS sends: "\r\r", "AT\r", "ATE0\r". */
    send_str("\r\r");
    drain(buf, sizeof(buf));
    if (strstr(buf, "ERROR") != NULL) {
        failures++;
        printf("  %-38s FAIL  a bare CR is not a command\n", "wake-up");
    } else
        printf("  %-38s ok    a bare CR is not a command\n", "wake-up");

    expect("AT", at("AT"), "OK");
    expect("ATE0 (echo off)", at("ATE0"), "OK");

    /* MD_INIT0.CSV: AT*NC<n>Z for the Supra's rows, AT+GCI=<hex> for the ELSA's,
       then AT&F2&W for both. */
    expect("AT*NC6Z    (MD_INIT0 DE/3)", at("AT*NC6Z"), "OK");
    expect("AT+GCI=04  (MD_INIT0 DE/2)", at("AT+GCI=04"), "OK");
    expect("AT&F2&W", at("AT&F2&W"), "OK");

    /* MD_INIT1.CSV, and the two strings real cabinets' NET.CFG files carry. */
    send_str("ATE0\r");
    drain(buf, sizeof(buf));
    expect("ate0m1l3S8=5S7=20S10=40 (H9751)",
           at("ATE0M1L3S8=5S7=20S10=40"), "OK");
    expect("...x3 (ND003)", at("ATE0M1L3S8=5S7=20S10=40X3"), "OK");

    /* MD_INIT2.CSV: every one of modem 3's 51 ISP rows.  Modem 2 has none. */
    expect("AT%C3&K3BN1W2 (MD_INIT2 */3)", at("AT%C3&K3BN1W2"), "OK");

    expect("ATS7?", at("ATS7?"), "020");
    expect("an unknown command is refused", at("ATJ9"), "ERROR");

    /* X3 is set above, so no dial tone detection: blind dial, then S7. */
    send_str("ATDT0676077111\r");
    drain(buf, sizeof(buf));
    if (strstr(buf, "NO CARRIER") != NULL) {
        failures++;
        printf("  %-38s FAIL  the dial answered too early\n", "dial");
    } else
        printf("  %-38s ok    nothing until S7 elapses\n", "dial");
    fake_ticks += 21000; /* S7 = 20 s */
    drain(buf, sizeof(buf));
    expect("dial times out at S7", buf, "NO CARRIER");

    /* X4 does listen for dial tone, and a dead line has none. */
    expect("ATX4", at("ATX4"), "OK");
    send_str("ATDT123\r");
    drain(buf, sizeof(buf));
    fake_ticks += 2000;
    drain(buf, sizeof(buf));
    expect("no dial tone under X4", buf, "NO DIALTONE");

    expect("ATV0 makes results numeric", at("ATV0\rAT"), "0\r");
    expect("ATV1", at("ATV1"), "OK");
}

static void
run_model(const device_t *device, const char *expect_name)
{
    printf("\n== %s ==\n", device->name);
    dev = device->init(device);

    collect_ati();
    for (int n = 0; n < 10; n++) {
        char trimmed[256];
        int  k = 0;

        for (const char *s = ati[n]; *s != '\0'; s++)
            if ((*s != '\r') && (*s != '\n') && (k < ((int) sizeof(trimmed) - 1)))
                trimmed[k++] = *s;
        trimmed[k] = '\0';
        if (k > 0)
            printf("  ATI%d -> %s\n", n, trimmed);
    }

    identify("the 2001 table (14 rows)", names_2001, expect_name);
    identify("the I.G.O. 6 table (30 rows)", names_igo6, expect_name);

    run_common();

    device->close(dev);
}

/* The order of events on a connect.  The cabinet's PPP driver reads result
   lines after ATDT and watches DCD; it must see the CONNECT text before the
   carrier, or it goes by the carrier alone and the cabinet files the call as
   NO RESPONSE. */
static void
run_connect(const device_t *device)
{
    char buf[4096];

    printf("\n== %s, connecting ==\n", device->name);
    fake_line = 1;
    dev       = device->init(device);

    send_str("\r\r");
    drain(buf, sizeof(buf));
    at("ATE0");
    send_str("ATDT0676077111\r");
    drain(buf, sizeof(buf));
    expect("nothing right after ATDT", strstr(buf, "CONNECT") ? "early" : "quiet", "quiet");
    expect("no DCD before CONNECT",
           (test_port.chardev.status(dev) & CHAR_COM_DCD) ? "dcd" : "none", "none");

    fake_ticks += 3100; /* dialling, then the far end's ring */
    drain(buf, sizeof(buf));
    expect("no CONNECT while training", strstr(buf, "CONNECT") ? "early" : "quiet", "quiet");
    fake_ticks += 2100; /* the training */
    drain(buf, sizeof(buf));
    expect("CONNECT after a pause", buf, "\r\nCONNECT 57600\r\n");
    expect("still no DCD while it is being read",
           (test_port.chardev.status(dev) & CHAR_COM_DCD) ? "dcd" : "none", "none");

    fake_ticks += 200;
    drain(buf, sizeof(buf));
    expect("DCD once the line has been read",
           (test_port.chardev.status(dev) & CHAR_COM_DCD) ? "dcd" : "none", "dcd");

    /* The status bar unplugs the line mid-call: the call ends. */
    expect("the modem is found on COM2", char_modem_present(1) ? "yes" : "no", "yes");
    expect("...and reports the call", (char_modem_get_state(1) == CHAR_MODEM_ONLINE) ? "online" : "not", "online");
    char_modem_set_line(1, CHAR_MODEM_LINE_DEAD, "127.0.0.1", 23);
    drain(buf, sizeof(buf));
    expect("unplugging the line ends the call", buf, "NO CARRIER");
    expect("...and DCD drops",
           (test_port.chardev.status(dev) & CHAR_COM_DCD) ? "dcd" : "none", "none");
    expect("the change is saved", (saved_line == CHAR_MODEM_LINE_DEAD) ? "dead" : "not", "dead");

    device->close(dev);
    expect("closed, it is gone from COM2", char_modem_present(1) ? "yes" : "no", "no");
    fake_line = 0;
}

/* Plugging the line in from the status bar, without a reset: a modem that
   came up dead dials a host from the next ATDT on. */
static void
run_plug_in(const device_t *device)
{
    char buf[4096];
    char host[128];
    int  port = 0;

    printf("\n== %s, line plugged in while running ==\n", device->name);
    fake_line = 0;
    dev       = device->init(device);

    expect("it starts unplugged",
           (char_modem_get_line(1, NULL, 0, NULL) == CHAR_MODEM_LINE_DEAD) ? "dead" : "not", "dead");

    fake_line = 1; /* the sockets now connect; the config still says dead */
    char_modem_set_line(1, CHAR_MODEM_LINE_TCP, "bbs.example", 2323);
    expect("the UI reads back the host at once",
           ((char_modem_get_line(1, host, sizeof(host), &port) == CHAR_MODEM_LINE_TCP) &&
            !strcmp(host, "bbs.example") && (port == 2323)) ? "yes" : "no", "yes");
    expect("the host is saved",
           ((saved_line == CHAR_MODEM_LINE_TCP) && !strcmp(saved_host, "bbs.example") &&
            (saved_port == 2323)) ? "yes" : "no", "yes");

    send_str("\r\r");
    drain(buf, sizeof(buf));
    at("ATE0");
    send_str("ATDT5551234\r");
    drain(buf, sizeof(buf));
    expect("calling", (char_modem_get_state(1) == CHAR_MODEM_CALLING) ? "calling" : "not", "calling");
    fake_ticks += 3100;
    drain(buf, sizeof(buf));
    fake_ticks += 2100;
    drain(buf, sizeof(buf));
    expect("it reaches the host", buf, "CONNECT");

    device->close(dev);
    fake_line = 0;
}

/* ------------------------------------------------- the line, byte by byte */

static int
dcd(void)
{
    return !!(test_port.chardev.status(dev) & CHAR_COM_DCD);
}

static int
cts(void)
{
    return !!(test_port.chardev.status(dev) & CHAR_COM_CTS);
}

/* Off hook to on line, the whole way: dial, ring, train, CONNECT, DCD. */
static int
call_up(const char *number)
{
    char buf[4096];
    char cmd[64];

    send_str("\r");
    drain(buf, sizeof(buf));
    at("ATE0");
    snprintf(cmd, sizeof(cmd), "ATDT%s\r", number);
    send_str(cmd);
    drain(buf, sizeof(buf));
    fake_ticks += 3100;
    drain(buf, sizeof(buf));
    fake_ticks += 2100;
    drain(buf, sizeof(buf));
    if (strstr(buf, "CONNECT") == NULL)
        return 0;
    fake_ticks += 200;
    drain(buf, sizeof(buf));
    return dcd();
}

/* Bytes the way the UART delivers them: one write each. */
static void
send_bytes(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        test_port.chardev.write((uint8_t *) &b[i], 1, dev);
}

/* A UART receive timer's worth of polls, nothing taken. */
static void
poll_line(int times)
{
    uint8_t b;

    for (int i = 0; i < times; i++)
        test_port.chardev.read(&b, 0, dev);
}

static void
run_transport(const device_t *device)
{
    static uint8_t data[9000];
    static uint8_t got[16384];
    char           buf[4096];
    fake_sock_t   *f;
    size_t         n;

    printf("\n== %s, the line's bytes ==\n", device->name);
    fake_line = 1;
    dev       = device->init(device);
    expect("a call comes up", call_up("5551234") ? "up" : "down", "up");
    f = &fsocks[last_sock];

    /* Every byte value, four times over, plus runs of the escape character
       with no guard time around them: all of it data.  The socket takes 7
       bytes a call and refuses the first 5 calls outright. */
    for (size_t i = 0; i < 1024; i++)
        data[i] = (uint8_t) i;
    memcpy(&data[300], "+++", 3);
    memcpy(&data[700], "++++", 4);
    f->send_chunk = 7;
    f->send_block = 5;
    send_bytes(data, 1024);
    poll_line(400);
    expect("short sends and would-block lose nothing",
           ((f->sent_len == 1024) && !memcmp(f->sent, data, 1024)) ? "exact" : "corrupt", "exact");
    expect("...in more than one send per byte", (f->send_calls > 150) ? "split" : "whole", "split");
    expect("...and the call is still up", dcd() ? "dcd" : "none", "dcd");

    /* The far end sends more than the modem's ring holds while the DTE reads
       nothing: the ring fills, and that is not a hangup. */
    for (size_t i = 0; i < 10000; i++)
        f->rx[i] = (uint8_t) ((i * 7) + (i >> 8));
    f->rx_len = 10000;
    f->rx_pos = 0;
    poll_line(64);
    expect("a full receive ring is not a hangup", dcd() ? "dcd" : "none", "dcd");
    for (n = 0; n < 10000;) {
        uint8_t b;

        if (test_port.chardev.read(&b, 1, dev) != 1)
            break;
        got[n++] = b;
    }
    expect("...and every byte arrives, in order",
           ((n == 10000) && !memcmp(got, f->rx, 10000)) ? "exact" : "corrupt", "exact");

    /* The line stops taking bytes: CTS drops before the queue fills, and
       comes back once it drains. */
    f->send_chunk = 0;
    f->send_block = 1000000;
    f->sent_len   = 0;
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t) (i ^ (i >> 7));
    send_bytes(data, sizeof(data));
    expect("a stalled line drops CTS", cts() ? "cts" : "held", "held");
    expect("...without dropping the call", dcd() ? "dcd" : "none", "dcd");
    f->send_block = 0;
    poll_line(4);
    expect("CTS returns when the line takes the queue", cts() ? "cts" : "held", "cts");
    expect("...and nothing was lost",
           ((f->sent_len == sizeof(data)) && !memcmp(f->sent, data, sizeof(data))) ? "exact" : "corrupt", "exact");

    /* A DTE that sends on regardless: the call fails, out loud. */
    f->send_block = 1000000;
    for (int i = 0; i < 17000; i++) {
        const uint8_t b = (uint8_t) 'x';

        test_port.chardev.write((uint8_t *) &b, 1, dev);
        if (!dcd())
            break;
    }
    drain(buf, sizeof(buf));
    expect("overrunning the queue past CTS ends the call", buf, "NO CARRIER");
    expect("...and closes the socket", f->closed ? "closed" : "open", "closed");

    /* The far end hangs up after some last bytes: they come first. */
    expect("a second call", call_up("5551234") ? "up" : "down", "up");
    f = &fsocks[last_sock];
    memcpy(f->rx, "bye", 3);
    f->rx_len = 3;
    f->rx_eof = 1;
    drain(buf, sizeof(buf));
    expect("the last bytes, then NO CARRIER", (strncmp(buf, "bye", 3) == 0) && strstr(buf, "NO CARRIER") ? "yes" : "no", "yes");
    expect("...DCD drops", dcd() ? "dcd" : "none", "none");

    /* A receive error is carrier loss too. */
    call_up("5551234");
    f           = &fsocks[last_sock];
    f->rx_error = 1;
    drain(buf, sizeof(buf));
    expect("a receive error ends the call", buf, "NO CARRIER");

    /* A send error likewise. */
    call_up("5551234");
    f             = &fsocks[last_sock];
    f->send_error = 1;
    send_bytes((const uint8_t *) "x", 1);
    drain(buf, sizeof(buf));
    expect("a send error ends the call", buf, "NO CARRIER");

    /* DTR drops (AT&D2): hang up. */
    call_up("5551234");
    f = &fsocks[last_sock];
    test_port.chardev.control(CHAR_COM_DTR, dev);
    test_port.chardev.control(0, dev);
    drain(buf, sizeof(buf));
    expect("DTR dropped: NO CARRIER", buf, "NO CARRIER");
    expect("...the socket is closed", f->closed ? "closed" : "open", "closed");

    /* +++ with guard time either side, then ATH. */
    test_port.chardev.control(CHAR_COM_DTR, dev);
    call_up("5551234");
    f = &fsocks[last_sock];
    fake_ticks += 1100;
    send_str("+++");
    fake_ticks += 1100;
    drain(buf, sizeof(buf));
    expect("+++ with guard times: command mode", buf, "OK");
    expect("...the pluses never reached the line", (f->sent_len == 0) ? "none" : "sent", "none");
    expect("ATH hangs up", at("ATH"), "OK");
    expect("...and closes the socket", f->closed ? "closed" : "open", "closed");

    /* After a guard time, pluses followed by data, or four of them, were data
       all along: each goes out exactly once. */
    call_up("5551234");
    f = &fsocks[last_sock];
    fake_ticks += 1100;
    send_str("++x");
    fake_ticks += 1100;
    send_str("++++");
    poll_line(2);
    expect("\"++x\" and \"++++\" go out as sent", ((f->sent_len == 7) && !memcmp(f->sent, "++x++++", 7)) ? "exact" : "wrong", "exact");
    at("ATZ");

    device->close(dev);
    fake_line = 0;
}

/* Two modems at once, each with its own call. */
static void
run_two(void)
{
    void        *d2, *d3;
    char_port_t *p2, *p3;
    fake_sock_t *f2, *f3;
    uint8_t      b;

    printf("\n== two modems, COM2 and COM3 ==\n");
    fake_line     = 1;
    fake_instance = 2;
    d2            = char_modem_supra_com_device.init(&char_modem_supra_com_device);
    p2            = tp;
    fake_instance = 3;
    d3            = char_modem_elsa_com_device.init(&char_modem_elsa_com_device);
    p3            = tp;
    fake_instance = 2;

    tp  = p2;
    dev = d2;
    expect("COM2 calls", call_up("111") ? "up" : "down", "up");
    f2 = &fsocks[last_sock];
    tp  = p3;
    dev = d3;
    expect("COM3 calls", call_up("222") ? "up" : "down", "up");
    f3 = &fsocks[last_sock];
    expect("two sockets", (f2 != f3) ? "two" : "one", "two");

    tp  = p2;
    dev = d2;
    send_str("for two");
    poll_line(2);
    tp  = p3;
    dev = d3;
    send_str("for three");
    poll_line(2);
    expect("each line gets its own bytes",
           ((f2->sent_len == 7) && !memcmp(f2->sent, "for two", 7) && (f3->sent_len == 9) &&
            !memcmp(f3->sent, "for three", 9)) ? "yes" : "no", "yes");

    memcpy(f2->rx, "2", 1);
    f2->rx_len = 1;
    memcpy(f3->rx, "3", 1);
    f3->rx_len = 1;
    tp  = p2;
    dev = d2;
    expect("...and reads its own", (test_port.chardev.read(&b, 1, dev) == 1) && (b == '2') ? "yes" : "no", "yes");
    tp  = p3;
    dev = d3;
    expect("...on both", (test_port.chardev.read(&b, 1, dev) == 1) && (b == '3') ? "yes" : "no", "yes");

    expect("each modem has its slot", (char_modem_present(1) && char_modem_present(2)) ? "yes" : "no", "yes");
    char_modem_elsa_com_device.close(d3);
    tp  = p2;
    dev = d2;
    expect("closing COM3 leaves COM2's call up", dcd() ? "dcd" : "none", "dcd");
    char_modem_supra_com_device.close(d2);
    fake_line = 0;
}

/* ------------------------------------------------------- the built-in ISP */

static void
sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

/* The PPP client talks through the modem as the guest's UART would. */
typedef struct {
    char_port_t *port;
    void        *dev;
} uart_t;

static size_t
uart_write(void *o, const uint8_t *b, size_t n)
{
    uart_t *u = (uart_t *) o;

    for (size_t i = 0; i < n; i++)
        u->port->chardev.write((uint8_t *) &b[i], 1, u->dev);
    return n;
}

static size_t
uart_read(void *o, uint8_t *b, size_t n)
{
    uart_t *u = (uart_t *) o;
    size_t  got = 0;

    /* As the receive timer does: one byte per call. */
    while ((got < n) && (u->port->chardev.read(&b[got], 1, u->dev) == 1))
        got++;
    return got;
}

static void
uart_idle(void *o)
{
    (void) o;
    sleep_ms(1);
}

static SOCKET
udp_listener(uint16_t *port)
{
    struct sockaddr_in sa;
    socklen_t          len = sizeof(sa);
    SOCKET             s   = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(0x7f000001);
    bind(s, (struct sockaddr *) &sa, sizeof(sa));
    getsockname(s, (struct sockaddr *) &sa, &len);
    *port = ntohs(sa.sin_port);
#ifdef _WIN32
    {
        u_long yes = 1;

        ioctlsocket(s, FIONBIO, &yes);
    }
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    return s;
}

static int
udp_round_trip(ppp_client_t *c, SOCKET hs, uint16_t hport, const char *msg)
{
    struct sockaddr_in from;
    socklen_t          flen = sizeof(from);
    char               rbuf[256];
    uint8_t            back[256];
    int                n     = -1;
    const uint32_t     until = ppp_client_ms() + 3000;

    ppp_client_send_udp(c, c->isp_ip, 7000, hport, (const uint8_t *) msg, strlen(msg));
    while ((int32_t) (ppp_client_ms() - until) < 0) {
        ppp_client_poll(c);
        n = (int) recvfrom(hs, rbuf, sizeof(rbuf), 0, (struct sockaddr *) &from, &flen);
        if (n > 0)
            break;
        sleep_ms(1);
    }
    if ((n != (int) strlen(msg)) || memcmp(rbuf, msg, (size_t) n))
        return 0;
    sendto(hs, "pong", 4, 0, (struct sockaddr *) &from, flen);
    n = ppp_client_recv_udp(c, 7000, back, sizeof(back), NULL, NULL, 3000);
    return (n == 4) && !memcmp(back, "pong", 4);
}

/* A call through the modem to the ISP, ended by the guest's own PPP: the ISP
   hangs up, and the modem says so. */
static void
run_isp(void)
{
    char         buf[4096];
    char         host[128];
    int          port = 0;
    uart_t       u2, u3;
    ppp_client_t c2, c3;
    void        *d2, *d3;
    SOCKET       hs;
    uint16_t     hport;

    printf("\n== the built-in ISP, through the modems ==\n");
    hs = udp_listener(&hport);

    fake_line     = 0; /* no TCP host anywhere: everything here is the ISP */
    fake_instance = 2;
    d2            = char_modem_supra_com_device.init(&char_modem_supra_com_device);
    u2.port       = tp;
    u2.dev        = d2;
    fake_instance = 3;
    d3            = char_modem_elsa_com_device.init(&char_modem_elsa_com_device);
    u3.port       = tp;
    u3.dev        = d3;
    fake_instance = 2;

    char_modem_set_line(1, CHAR_MODEM_LINE_ISP, "", 23);
    char_modem_set_line(2, CHAR_MODEM_LINE_ISP, "", 23);
    expect("the line reads back as the ISP",
           (char_modem_get_line(1, host, sizeof(host), &port) == CHAR_MODEM_LINE_ISP) ? "isp" : "not", "isp");
    expect("...and is saved as 2", (saved_line == 2) ? "2" : "not", "2");

    /* A bare ATD reaches nobody. */
    tp  = u2.port;
    dev = d2;
    send_str("\r");
    drain(buf, sizeof(buf));
    at("ATE0X4");
    send_str("ATD\r");
    drain(buf, sizeof(buf));
    fake_ticks += 9000;
    drain(buf, sizeof(buf));
    expect("ATD with no number: NO CARRIER", buf, "NO CARRIER");

    /* Any number: the ISP answers, with the call's whole ceremony. */
    send_str("ATDT0191 555 0000\r");
    drain(buf, sizeof(buf));
    expect("calling", (char_modem_get_state(1) == CHAR_MODEM_CALLING) ? "calling" : "not", "calling");
    fake_ticks += 3100;
    drain(buf, sizeof(buf));
    expect("no CONNECT while it rings and trains", strstr(buf, "CONNECT") ? "early" : "quiet", "quiet");
    fake_ticks += 2100;
    drain(buf, sizeof(buf));
    expect("CONNECT", buf, "CONNECT 57600");
    expect("no DCD while CONNECT is read", dcd() ? "dcd" : "none", "none");
    fake_ticks += 200;
    drain(buf, sizeof(buf));
    expect("then DCD", dcd() ? "dcd" : "none", "dcd");
    expect("the ISP says nothing until the guest starts PPP", buf[0] ? "talked" : "quiet", "quiet");

    tp  = u3.port;
    dev = d3;
    expect("COM3 dials the ISP too", call_up("0800 4711") ? "up" : "down", "up");

    memset(&c2, 0, sizeof(c2));
    memset(&c3, 0, sizeof(c3));
    c2.write = c3.write = uart_write;
    c2.read = c3.read = uart_read;
    c2.idle = c3.idle = uart_idle;
    c2.opaque = &u2;
    c3.opaque = &u3;
    ppp_client_init(&c2);
    ppp_client_init(&c3);
    expect("COM2: PPP up through the modem", ppp_client_connect(&c2, 10000) ? "up" : "down", "up");
    expect("COM3: PPP up through the modem", ppp_client_connect(&c3, 10000) ? "up" : "down", "up");
    printf("    COM2 got %s, COM3 got %s\n", ppp_client_ip_str(c2.my_ip), ppp_client_ip_str(c3.my_ip));
    expect("different addresses", (c2.my_ip && c3.my_ip && (c2.my_ip != c3.my_ip)) ? "yes" : "no", "yes");
    expect("COM2: UDP to the host and back", udp_round_trip(&c2, hs, hport, "via COM2") ? "yes" : "no", "yes");
    expect("COM3: UDP to the host and back", udp_round_trip(&c3, hs, hport, "via COM3") ? "yes" : "no", "yes");

    /* COM3's line is unplugged mid-call; COM2 carries on. */
    tp  = u3.port;
    dev = d3;
    char_modem_set_line(2, CHAR_MODEM_LINE_DEAD, "", 23);
    drain(buf, sizeof(buf));
    expect("COM3 unplugged: NO CARRIER", buf, "NO CARRIER");
    expect("COM2 still on line", udp_round_trip(&c2, hs, hport, "still COM2") ? "yes" : "no", "yes");

    /* The guest on COM2 ends PPP: the ISP hangs up on it. */
    tp  = u2.port;
    dev = d2;
    expect("COM2's guest terminates LCP", ppp_client_terminate(&c2, 3000) ? "acked" : "no", "acked");
    {
        const uint32_t until = ppp_client_ms() + 6000;

        buf[0] = '\0';
        while (dcd() && ((int32_t) (ppp_client_ms() - until) < 0)) {
            drain(buf, sizeof(buf));
            sleep_ms(10);
        }
        expect("...and the ISP hangs up: NO CARRIER", buf, "NO CARRIER");
    }

    /* Dial again: the freed address is handed out again. */
    expect("COM2 redials", call_up("0191") ? "up" : "down", "up");
    ppp_client_init(&c2);
    expect("...PPP up", ppp_client_connect(&c2, 10000) ? "up" : "down", "up");
    expect("...on the address it had", (c2.my_ip == 0x0a56010f) ? "10.86.1.15" : ppp_client_ip_str(c2.my_ip), "10.86.1.15");

    /* The emulator closes with the call up. */
    char_modem_supra_com_device.close(d2);
    char_modem_elsa_com_device.close(d3);
    {
        char           err[128];
        isp_session_t *s = isp_session_open(NULL, err, sizeof(err));

        expect("closing mid-call frees the session",
               ((s != NULL) && (isp_session_number(s) == 1)) ? "freed" : "held", "freed");
        isp_session_close(s);
    }
#ifdef _WIN32
    closesocket(hs);
#else
    close(hs);
#endif
}

int
main(void)
{
#ifdef _WIN32
    WSADATA wsa;

    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    run_model(&char_modem_elsa_com_device, "ELSA MicroLink 56k");
    run_model(&char_modem_supra_com_device, "Diamond SupraExpress 56e PRO");
    run_connect(&char_modem_supra_com_device);
    run_plug_in(&char_modem_elsa_com_device);
    run_transport(&char_modem_supra_com_device);
    run_two();
    run_isp();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed",
           failures, (failures == 1) ? "" : "s");
    return failures ? 1 : 0;
}
