/*
 * 86Box-Next: modems calling each other over isp-server's telephone network.
 * Two real modems (src/char/char_modem.c) with real sockets
 * (win_netsocket.c / unix_netsocket.c), the real exchange (isp_srv.c)
 * started in-process; the guests are this test, typing AT commands into the
 * modems and reading what comes back, on the real clock.  No test framework;
 * non-zero on failure.
 *
 *   both modems register: one gets the number it asked for, one is given one
 *   a dial rings the other: RING, RI, caller ID (AT#CID=1); ATA; CONNECT and
 *   DCD on both; bytes both ways, exactly; +++ ATH, and the other hears NO
 *   CARRIER
 *   auto-answer (S0=1), BUSY (dialling oneself), the ISP on any other
 *   number (PPP up through the modem), and the exchange going away (the
 *   number goes, and comes back with it).
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <winsock2.h>
#    include <windows.h>
#else
#    include <unistd.h>
#endif
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/char.h>
#include <86box/plat_netsocket.h>
#include <86box/thread.h>
#include <86box/modem_sound.h>
#include <86box/char_modem.h>
#include "isp.h"
#include "isp_plat.h"
#include "isp_srv.h"
#include "ppp_client.h"

/* ------------------------------------------------------------ 86Box stubs */

char vm_name[1024] = "Phone test";

static char_port_t  ports[4];
static int          next_port;
static char_port_t *last_port;

char_port_t *
char_attach(uint32_t flags, size_t (*read)(uint8_t *, size_t, void *), size_t (*write)(uint8_t *, size_t, void *),
            uint32_t (*status)(void *), void (*control)(uint32_t, void *), void (*port_config)(void *), void *priv)
{
    char_port_t *p = &ports[next_port++ % 4];

    memset(p, 0, sizeof(*p));
    p->chardev.flags       = flags;
    p->chardev.read        = read;
    p->chardev.write       = write;
    p->chardev.status      = status;
    p->chardev.control     = control;
    p->chardev.port_config = port_config;
    p->chardev.priv        = priv;
    p->attached            = 1;
    last_port              = p;
    return p;
}

void  char_update_status(char_port_t *p) { (void) p; }
void *char_log_open(char_port_t *p, char *n) { (void) p; (void) n; return NULL; }
void  log_close(void *p) { (void) p; }
void  log_out(void *p, const char *f, va_list a) { (void) p; (void) f; (void) a; }
void  pclog(const char *f, ...) { (void) f; }

mutex_t *thread_create_mutex(void) { return (mutex_t *) isp_mutex_new(); }
int      thread_wait_mutex(mutex_t *m) { isp_mutex_lock((isp_mutex_t *) m); return 1; }
int      thread_release_mutex(mutex_t *m) { isp_mutex_unlock((isp_mutex_t *) m); return 1; }

/* The configuration of the modem being made. */
static int  cfg_instance;
static char cfg_number[24];
static char cfg_exchange[64];

int             device_get_instance(void) { return cfg_instance; }
void            device_context_inst(const device_t *d, int inst) { (void) d; (void) inst; }
void            device_context_restore(void) { }
const device_t *device_context_get_device(void) { return NULL; }
void            device_set_config_int(const char *name, int val) { (void) name; (void) val; }
void            device_set_config_string(const char *name, const char *val) { (void) name; (void) val; }

int
device_get_config_int(const char *name)
{
    if (!strcmp(name, "line"))
        return CHAR_MODEM_LINE_PHONE;
    if (!strcmp(name, "host_port"))
        return 23;
    if (!strcmp(name, "connect_rate"))
        return 57600;
    return 0;
}

const char *
device_get_config_string(const char *name)
{
    if (!strcmp(name, "phone_number"))
        return cfg_number;
    if (!strcmp(name, "exchange"))
        return cfg_exchange;
    return "";
}

/* The real clock, and a short call: 100 ms to dial, 100 to ring, 300 to
   train. */
uint32_t plat_get_ticks(void) { return (uint32_t) isp_now_ms(); }

static int busy_tone; /* the busy tone was played */

modem_sound_t *modem_sound_init(int enabled) { (void) enabled; return NULL; }
void     modem_sound_close(modem_sound_t *s) { (void) s; }
void     modem_sound_event(modem_sound_t *s, int t, const char *n, int a) { (void) s; (void) n; (void) a; busy_tone |= (t == MODEM_SOUND_BUSY); }
void     modem_sound_speaker(modem_sound_t *s, int m, int l) { (void) s; (void) m; (void) l; }
void     modem_sound_country(modem_sound_t *s, int uk, int v90) { (void) s; (void) uk; (void) v90; }
uint32_t modem_sound_dial_ms(const char *n, int s8, int p) { (void) n; (void) s8; (void) p; return 100; }
uint32_t modem_sound_ring_ms(void) { return 100; }
uint32_t modem_sound_handshake_ms(int v90) { (void) v90; return 300; }

/* The handset's ear and mouth: what the speaker plays, and a microphone the
   test speaks into. */
static int16_t heard[64000];
static size_t  heard_n;
static double  mic_hz;     /* a tone the microphone hears; 0: silence */
static double  mic_ph;
void   modem_sound_voice(modem_sound_t *s, const int16_t *v, size_t n)
{
    (void) s;
    for (size_t i = 0; (i < n) && (heard_n < (sizeof(heard) / sizeof(heard[0]))); i++)
        heard[heard_n++] = v[i];
}
int    snd_mic_open(void) { return 0; }
void   snd_mic_close(void) { }
size_t snd_mic_read(int16_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = (int16_t) ((mic_hz > 0.0) ? 8000.0 * sin(mic_ph) : 0.0);
        mic_ph += 2.0 * 3.14159265358979 * mic_hz / 8000.0;
    }
    return n;
}

/* --------------------------------------------------------------- the rig */

extern const device_t char_modem_supra_com_device;
extern const device_t char_modem_elsa_com_device;

typedef struct {
    const char  *name;
    int          slot;
    char_port_t *port;
    void        *dev;
    char         heard[16384]; /* everything the modem has said since the last forget() */
    size_t       len;
} modem_t_;

static modem_t_ A = { "COM2", 1 }, B = { "COM3", 2 };
static int      failures;

static void
check(const char *what, int ok)
{
    if (!ok)
        failures++;
    printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
    fflush(stdout);
}

static void
sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

/* The UARTs' receive timers: each modem is read until it has nothing. */
static void
pump(int ms)
{
    const uint32_t until = ppp_client_ms() + (uint32_t) ms;

    do {
        modem_t_ *m[2] = { &A, &B };

        for (int k = 0; k < 2; k++) {
            for (int i = 0; i < 64; i++) {
                uint8_t c;

                if (m[k]->port->chardev.read(&c, 1, m[k]->dev) != 1)
                    continue;
                if (m[k]->len < (sizeof(m[k]->heard) - 1))
                    m[k]->heard[m[k]->len++] = (char) c;
                m[k]->heard[m[k]->len] = '\0';
            }
        }
        sleep_ms(1);
    } while ((int32_t) (ppp_client_ms() - until) < 0);
}

static void
forget(modem_t_ *m)
{
    m->len      = 0;
    m->heard[0] = '\0';
}

/* Pumps until `m` has said `what`, at most `ms`. */
static int
hears(modem_t_ *m, const char *what, int ms)
{
    const uint32_t until = ppp_client_ms() + (uint32_t) ms;

    while (strstr(m->heard, what) == NULL) {
        if ((int32_t) (ppp_client_ms() - until) >= 0)
            return 0;
        pump(5);
    }
    return 1;
}

static void
type(modem_t_ *m, const char *s)
{
    for (; *s; s++) {
        uint8_t c = (uint8_t) *s;

        m->port->chardev.write(&c, 1, m->dev);
    }
}

static void
at(modem_t_ *m, const char *cmd)
{
    forget(m);
    type(m, cmd);
    type(m, "\r");
    pump(50);
}

static int
dcd(modem_t_ *m)
{
    return !!(m->port->chardev.status(m->dev) & CHAR_COM_DCD);
}

static int
registered(modem_t_ *m, char *number, size_t len)
{
    return char_modem_get_phone(m->slot, NULL, 0, NULL, 0, number, len) == 1;
}

static int
wait_registered(modem_t_ *m, char *number, size_t len, int ms)
{
    const uint32_t until = ppp_client_ms() + (uint32_t) ms;

    while (!registered(m, number, len)) {
        if ((int32_t) (ppp_client_ms() - until) >= 0)
            return 0;
        pump(10);
    }
    return 1;
}

/* Exactly `len` bytes from one modem's DTE to the other's. */
static int
carry(modem_t_ *from, modem_t_ *to, size_t len)
{
    uint8_t       *sent = (uint8_t *) malloc(len);
    uint8_t       *got  = (uint8_t *) malloc(len);
    size_t         s = 0, g = 0;
    const uint32_t until = ppp_client_ms() + 20000;
    int            ok;

    for (size_t i = 0; i < len; i++)
        sent[i] = (uint8_t) ((i * 7) ^ (i >> 8));
    while ((g < len) && ((int32_t) (ppp_client_ms() - until) < 0)) {
        /* A UART's worth at a time, while CTS says so. */
        for (int i = 0; (i < 64) && (s < len) && (from->port->chardev.status(from->dev) & CHAR_COM_CTS); i++)
            from->port->chardev.write(&sent[s++], 1, from->dev);
        for (int i = 0; i < 256; i++) {
            uint8_t c;

            if (to->port->chardev.read(&c, 1, to->dev) != 1)
                break;
            if (g < len)
                got[g++] = c;
        }
        {
            uint8_t c;

            (void) from->port->chardev.read(&c, 0, from->dev); /* the sender's poll */
        }
        sleep_ms(1);
    }
    ok = (g == len) && !memcmp(sent, got, len);
    free(sent);
    free(got);
    return ok;
}

/* The PPP client through modem A's UART. */
static size_t
uart_write(void *o, const uint8_t *b, size_t n)
{
    modem_t_ *m = (modem_t_ *) o;

    for (size_t i = 0; i < n; i++)
        m->port->chardev.write((uint8_t *) &b[i], 1, m->dev);
    return n;
}

static size_t
uart_read(void *o, uint8_t *b, size_t n)
{
    modem_t_ *m   = (modem_t_ *) o;
    size_t    got = 0;

    while ((got < n) && (m->port->chardev.read(&b[got], 1, m->dev) == 1))
        got++;
    return got;
}

static void
uart_idle(void *o)
{
    (void) o;
    sleep_ms(1);
}

static void
quiet_log(const char *line)
{
    if (getenv("PHONE_TEST_VERBOSE"))
        printf("      | %s\n", line);
}

int
main(void)
{
    isp_srv_config_t cfg;
    char             na[24] = "", nb[24] = "";
    int              ri = 0;

#ifdef _WIN32
    WSADATA wsa;

    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.listen, sizeof(cfg.listen), "127.0.0.1");
    cfg.http_port = -1;
    cfg.log       = quiet_log;
    isp_srv_load(&cfg);
    if (isp_srv_start(&cfg) != 0)
        return 1;
    snprintf(cfg_exchange, sizeof(cfg_exchange), "127.0.0.1:%d", cfg.port);
    printf("== two modems on isp-server's telephone network (exchange on %s) ==\n", cfg_exchange);

    cfg_instance = 2;
    snprintf(cfg_number, sizeof(cfg_number), "555-0101");
    A.dev  = char_modem_supra_com_device.init(&char_modem_supra_com_device);
    A.port = last_port;
    cfg_instance = 3;
    cfg_number[0] = '\0';
    B.dev  = char_modem_elsa_com_device.init(&char_modem_elsa_com_device);
    B.port = last_port;

    check("COM2 registers the number it asks for",
          wait_registered(&A, na, sizeof(na), 5000) && !strcmp(na, "5550101"));
    check("COM3, asking for none, is given one", wait_registered(&B, nb, sizeof(nb), 5000) && !strcmp(nb, "5550102"));

    at(&A, "ATE0");
    at(&B, "ATE0");
    at(&B, "AT#CID=1");
    check("AT#CID=1 is accepted", strstr(B.heard, "OK") != NULL);

    /* COM2 calls COM3. */
    forget(&B);
    at(&A, "ATDT555-0102");
    check("COM3 rings", hears(&B, "RING", 3000));
    ri = !!(B.port->chardev.status(B.dev) & CHAR_COM_RI);
    check("...with RI on", ri);
    check("...and caller ID: COM2's number", hears(&B, "NMBR = 5550101", 1000));
    check("COM2 is still calling, no CONNECT", (char_modem_get_state(A.slot) == CHAR_MODEM_CALLING) &&
                                                   (strstr(A.heard, "CONNECT") == NULL));
    at(&B, "ATA");
    check("COM3 answers: CONNECT", hears(&B, "CONNECT", 3000));
    check("COM2: CONNECT", hears(&A, "CONNECT", 3000));
    pump(300);
    check("DCD on both", dcd(&A) && dcd(&B));

    check("8 KB from COM2 to COM3, exactly", carry(&A, &B, 8192));
    check("8 KB from COM3 to COM2, exactly", carry(&B, &A, 8192));

    /* COM2 hangs up: +++ (a short guard time), ATH. */
    pump(10);
    {
        forget(&A);
        sleep_ms(1100);
        type(&A, "+++");
        sleep_ms(1100);
        pump(100);
        check("COM2: +++ gives command mode", strstr(A.heard, "OK") != NULL);
        forget(&B);
        at(&A, "ATH");
        check("COM3 hears NO CARRIER", hears(&B, "NO CARRIER", 3000));
        check("...and DCD drops on both", !dcd(&A) && !dcd(&B));
    }

    /* Auto-answer: COM2 answers on the first ring. */
    at(&A, "ATS0=1");
    forget(&A);
    forget(&B);
    at(&B, "ATDT5550101");
    check("S0=1: COM2 answers its first ring by itself", hears(&A, "RING", 3000) && hears(&A, "CONNECT", 3000));
    check("...COM3 connects", hears(&B, "CONNECT", 3000));
    pump(300);
    check("8 KB across, exactly", carry(&B, &A, 8192));
    pump(10);
    sleep_ms(1100);
    type(&B, "+++");
    sleep_ms(1100);
    pump(100);
    forget(&A);
    at(&B, "ATH");
    check("COM3 hangs up: COM2 hears NO CARRIER", hears(&A, "NO CARRIER", 3000));
    at(&A, "ATS0=0");

    /* Dialling oneself. */
    forget(&B);
    at(&B, "ATDT5550102");
    check("COM3 dials its own number: BUSY", hears(&B, "BUSY", 6000));
    check("...with the busy tone", busy_tone);

    /* Any other number: the ISP. */
    forget(&A);
    at(&A, "ATDT0191");
    check("COM2 dials 0191: the ISP answers, CONNECT", hears(&A, "CONNECT", 3000));
    pump(300);
    {
        ppp_client_t c;

        memset(&c, 0, sizeof(c));
        c.write  = uart_write;
        c.read   = uart_read;
        c.idle   = uart_idle;
        c.opaque = &A;
        ppp_client_init(&c);
        check("...and PPP comes up through the modem", ppp_client_connect(&c, 10000));
        check("...on the ISP's address", (c.my_ip >> 16) == 0x0a56);
    }
    char_modem_set_line(A.slot, CHAR_MODEM_LINE_DEAD, "", 23);
    pump(100);

    /* The exchange goes away, and comes back. */
    isp_srv_stop();
    {
        const uint32_t until = ppp_client_ms() + 5000;

        while (registered(&B, nb, sizeof(nb)) && ((int32_t) (ppp_client_ms() - until) < 0))
            pump(20);
        check("isp-server stops: COM3 has no number", !registered(&B, nb, sizeof(nb)));
    }
    cfg.port = atoi(strchr(cfg_exchange, ':') + 1);
    if (isp_srv_start(&cfg) == 0)
        check("...and gets it back when it returns", wait_registered(&B, nb, sizeof(nb), 8000) && !strcmp(nb, "5550101"));

    char_modem_supra_com_device.close(A.dev);
    char_modem_elsa_com_device.close(B.dev);
    isp_srv_stop();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed", failures,
           (failures == 1) ? "" : "s");
    return failures ? 1 : 0;
}
