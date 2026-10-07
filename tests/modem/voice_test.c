/*
 * 86Box-Next: voice calls on isp-server's telephone network.
 *
 * The codecs first (modem_voice.c): mu-law, the rate converter, and Rockwell
 * ADPCM pinned to what vgetty's rockwell.c -- which matches Rockwell's own
 * coder -- makes of the same signal.  Then two real modems
 * (src/char/char_modem.c) with real sockets, the real exchange (isp_srv.c)
 * in-process, and this test as both guests, typing what Unimodem/V types:
 *
 *   AT#CLS=8 ... ATD: VCON once dialled; the other rings, ATA: VCON
 *   AT#VTX on one, AT#VRX on the other: a tone played as 4-bit ADPCM at
 *   7200 Hz arrives as one, across the exchange
 *   AT#VTS=7: the other hears <DLE>7; "!" ends recording: <DLE><ETX> OK
 *   the handset (the host's microphone and speaker) joins a call, and makes
 *   one of its own; a voice call answered by a data modem hears its answer
 *   tone, and the modem gives up: NO CARRIER
 *   the far end hangs up: <DLE>b
 *
 * No test framework; non-zero on failure.
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
#    include <mmsystem.h>
#else
#    include <unistd.h>
#endif
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/char.h>
#include <86box/plat_netsocket.h>
#include <86box/thread.h>
#include <86box/modem_sound.h>
#include <86box/modem_voice.h>
#include <86box/char_modem.h>
#include "isp.h"
#include "isp_plat.h"
#include "isp_srv.h"
#include "isp_web.h"
#include "ppp_client.h"

#ifndef M_PI
#    define M_PI 3.14159265358979323846
#endif

#define DLE 0x10
#define ETX 0x03

/* ------------------------------------------------------------ 86Box stubs */

char vm_name[1024] = "Voice test";

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

static int  cfg_instance;
static char cfg_number[24];
static char cfg_exchange[64];

int             device_get_instance(void) { return cfg_instance; }
void            device_context_inst(const device_t *d, int inst) { (void) d; (void) inst; }
void            device_context_restore(void) { }
const device_t *device_context_get_device(void) { return NULL; }
void *char_open_unlisted(char_port_t *p, const device_t *d) { (void) p; return d->init(d); }
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

uint32_t plat_get_ticks(void) { return (uint32_t) isp_now_ms(); }

static int bells;

modem_sound_t *modem_sound_init(int enabled) { (void) enabled; return NULL; }
void     modem_sound_close(modem_sound_t *s) { (void) s; }
void     modem_sound_event(modem_sound_t *s, int t, const char *n, int a) { (void) s; (void) n; (void) a; bells += (t == MODEM_SOUND_BELL); }
void     modem_sound_speaker(modem_sound_t *s, int m, int l) { (void) s; (void) m; (void) l; }
void     modem_sound_country(modem_sound_t *s, int uk, int v90) { (void) s; (void) uk; (void) v90; }
uint32_t modem_sound_dial_ms(const char *n, int s8, int p) { (void) n; (void) s8; (void) p; return 100; }
uint32_t modem_sound_ring_ms(void) { return 100; }
uint32_t modem_sound_handshake_ms(int v90) { (void) v90; return 300; }

/* The host's speaker (both modems' handsets play into it) and microphone. */
static int16_t heard[8000 * 30];
static size_t  heard_n;
static double  mic_hz; /* a tone in front of the microphone; 0: quiet */
static double  mic_ph;

void
modem_sound_voice(modem_sound_t *s, const int16_t *v, size_t n)
{
    (void) s;
    for (size_t i = 0; (i < n) && (heard_n < (sizeof(heard) / sizeof(heard[0]))); i++)
        heard[heard_n++] = v[i];
}

int    snd_mic_open(void) { return 0; }
void   snd_mic_close(void) { }

size_t
snd_mic_read(int16_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = (int16_t) ((mic_hz > 0.0) ? 9000.0 * sin(mic_ph) : 0.0);
        mic_ph += 2.0 * M_PI * mic_hz / 8000.0;
    }
    return n;
}

/* --------------------------------------------------------------- helpers */

static int  failures;
static char last_tone[128]; /* what loudest() measured last, shown with a failure */

static void
check(const char *what, int ok)
{
    if (!ok)
        failures++;
    printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok && last_tone[0])
        printf("      (%s)\n", last_tone);
    last_tone[0] = '\0';
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

/* How much of a frequency is in a signal: Goertzel's power, normalised. */
static double
tone_level(const int16_t *s, size_t n, double rate, double hz)
{
    const double c = 2.0 * cos(2.0 * M_PI * hz / rate);
    double       q1 = 0.0, q2 = 0.0;

    if (n == 0)
        return 0.0;
    for (size_t i = 0; i < n; i++) {
        const double q0 = c * q1 - q2 + s[i];

        q2 = q1;
        q1 = q0;
    }
    return sqrt(q1 * q1 + q2 * q2 - c * q1 * q2) / (double) n;
}

/* The frequency, of a few, that is loudest -- and by how much. */
static double
loudest(const int16_t *s, size_t n, double rate, const double *hz, int k, double *margin)
{
    double best = 0.0, second = 0.0, f = 0.0;

    for (int i = 0; i < k; i++) {
        const double l = tone_level(s, n, rate, hz[i]);

        if (l > best) {
            second = best;
            best   = l;
            f      = hz[i];
        } else if (l > second)
            second = l;
    }
    if (margin != NULL)
        *margin = (second > 0.0) ? (best / second) : 1e9;
    snprintf(last_tone, sizeof(last_tone), "%zu samples, loudest %.0f Hz at %.1f, next %.1f", n, f, best, second);
    return f;
}

/* VOICE_TEST_TRACE: the loudest of hz[] in each 200 ms, and the level. */
static void
trace(const char *what, const int16_t *s, size_t n, double rate, const double *hz, int k)
{
    const size_t win = (size_t) (rate / 5);

    if (!getenv("VOICE_TEST_TRACE"))
        return;
    printf("      %s:", what);
    for (size_t i = 0; i + win <= n; i += win) {
        double       m;
        const double f = loudest(s + i, win, rate, hz, k, &m);

        printf(" %.0f(%.0f)", f, tone_level(s + i, win, rate, f));
    }
    printf("\n");
}

static uint32_t
crc32(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *) p;
    uint32_t       c = 0xffffffffu;

    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* ------------------------------------------------------------- the codecs */

/* A DTMF pair, noise, and loud stretches that reach the coder's clips. */
static void
test_signal(int16_t *s, int n)
{
    unsigned rng = 1;

    for (int i = 0; i < n; i++) {
        double v = 7000.0 * sin(2 * M_PI * 697.0 * i / 7200.0) + 7000.0 * sin(2 * M_PI * 1336.0 * i / 7200.0);

        rng = rng * 1103515245u + 12345u;
        v += (double) ((int) ((rng >> 16) & 0x7ff) - 1024);
        if ((i % 1800) < 120)
            v *= 3.0;
        if (v > 32767.0)
            v = 32767.0;
        if (v < -32768.0)
            v = -32768.0;
        s[i] = (int16_t) v;
    }
}

static void
codec_tests(void)
{
    /* What vgetty's rockwell.c makes of test_signal(7200 samples): coded
       bytes, and the samples decoded from them (as little-endian int16). */
    static const struct {
        int      bps;
        size_t   bytes;
        uint32_t enc, dec;
    } pinned[] = {
        { 2, 1800, 0xb16a751c, 0xaa9c7e3b },
        { 3, 2700, 0x08136fa4, 0xfcbc9bd4 },
        { 4, 3600, 0xae67508d, 0xe24f9129 }
    };
    static int16_t sig[7200], dec[7200];
    static uint8_t enc[7200];
    char           what[96];

    printf("== the codecs ==\n");
    test_signal(sig, 7200);
    for (int k = 0; k < 3; k++) {
        rv_adpcm_t s;
        size_t     n, d;

        rv_adpcm_init(&s, pinned[k].bps);
        n = rv_adpcm_encode(&s, sig, 7200, enc, sizeof(enc));
        n += rv_adpcm_flush(&s, enc + n, sizeof(enc) - n);
        snprintf(what, sizeof(what), "Rockwell ADPCM, %d bits: coded as Rockwell codes it", pinned[k].bps);
        check(what, (n == pinned[k].bytes) && (crc32(enc, n) == pinned[k].enc));
        rv_adpcm_init(&s, pinned[k].bps);
        d = rv_adpcm_decode(&s, enc, n, dec, 7200);
        snprintf(what, sizeof(what), "...and decoded as Rockwell decodes it");
        check(what, (d == 7200) && (crc32(dec, d * 2) == pinned[k].dec));
    }
    {
        /* A tone through 4-bit ADPCM is still that tone. */
        static const double hz[] = { 500, 1000, 1500, 2000 };
        double              margin;
        rv_adpcm_t          s;

        for (int i = 0; i < 7200; i++)
            sig[i] = (int16_t) (10000.0 * sin(2 * M_PI * 1000.0 * i / 7200.0));
        rv_adpcm_init(&s, 4);
        (void) rv_adpcm_encode(&s, sig, 7200, enc, sizeof(enc));
        rv_adpcm_init(&s, 4);
        (void) rv_adpcm_decode(&s, enc, 3600, dec, 7200);
        check("a 1 kHz tone through 4-bit ADPCM is 1 kHz", (loudest(dec + 400, 6800, 7200, hz, 4, &margin) == 1000) && (margin > 10));
    }
    {
        int worst = 0;

        for (int x = -32768; x <= 32767; x += 7) {
            const int e = abs(voice_ulaw_decode(voice_ulaw_encode((int16_t) x)) - x);

            if (e > (abs(x) / 16 + 16))
                worst = 1;
        }
        check("mu-law: every level comes back within its step", !worst && (voice_ulaw_decode(voice_ulaw_encode(0)) == 0));
        worst = 0;
        for (int x = -32768; x <= 32767; x += 7) {
            const int e = abs(voice_alaw_decode(voice_alaw_encode((int16_t) x)) - x);

            if (e > (abs(x) / 16 + 16))
                worst = 1;
        }
        check("A-law: every level comes back within its step", !worst && (voice_alaw_encode(0) == 0xd5) &&
                                                                (voice_alaw_encode(-1) == 0x55));
    }
    {
        static const double hz[] = { 300, 440, 600 };
        voice_resampler_t   r;
        static int16_t      out[9000];
        double              margin;
        size_t              n;

        for (int i = 0; i < 7200; i++)
            sig[i] = (int16_t) (10000.0 * sin(2 * M_PI * 440.0 * i / 7200.0));
        voice_resampler_init(&r, 7200, 8000);
        n = voice_resample(&r, sig, 7200, out, 9000);
        check("7200 Hz to 8000 Hz: a second is a second", (n >= 7995) && (n <= 8000));
        check("...and 440 Hz is still 440 Hz", (loudest(out, n, 8000, hz, 3, &margin) == 440) && (margin > 10));
    }
}

/* ------------------------------------------------------------- the modems */

extern const device_t char_modem_supra_com_device;

typedef struct {
    const char  *name;
    int          slot;
    char_port_t *port;
    void        *dev;
    uint8_t      raw[262144]; /* everything it has said, since forget() */
    size_t       n;
} vmodem_t;

static vmodem_t A = { "COM2", 1 }, B = { "COM3", 2 };

static void
read_modem(vmodem_t *m)
{
    for (int i = 0; i < 4096; i++) { /* a UART at 115200 bit/s and more: no backlog */
        uint8_t c;

        if (m->port->chardev.read(&c, 1, m->dev) != 1)
            break;
        if (m->n < sizeof(m->raw))
            m->raw[m->n++] = c;
    }
}

static void
pump(int ms)
{
    const uint32_t until = ppp_client_ms() + (uint32_t) ms;

    do {
        read_modem(&A);
        read_modem(&B);
        sleep_ms(1);
    } while ((int32_t) (ppp_client_ms() - until) < 0);
}

static void
forget(vmodem_t *m)
{
    m->n = 0;
}

/* Whether m has said `what` since forget() -- binary-safe. */
static int
said(const vmodem_t *m, const char *what)
{
    const size_t k = strlen(what);

    for (size_t i = 0; (i + k) <= m->n; i++)
        if (!memcmp(&m->raw[i], what, k))
            return 1;
    return 0;
}

static int
hears(vmodem_t *m, const char *what, int ms)
{
    const uint32_t until = ppp_client_ms() + (uint32_t) ms;

    while (!said(m, what)) {
        if ((int32_t) (ppp_client_ms() - until) >= 0)
            return 0;
        pump(5);
    }
    return 1;
}

static void
type(vmodem_t *m, const char *s)
{
    for (; *s; s++) {
        uint8_t c = (uint8_t) *s;

        m->port->chardev.write(&c, 1, m->dev);
    }
}

static void
at(vmodem_t *m, const char *cmd)
{
    forget(m);
    type(m, cmd);
    type(m, "\r");
    pump(60);
}

static int
ok(vmodem_t *m, const char *cmd)
{
    at(m, cmd);
    return hears(m, "OK", 1000);
}

/* #VTX: `ms` of a tone as 4-bit ADPCM at 7200 Hz, as fast as CTS allows,
   then DLE ETX. */
static void
play_tone(vmodem_t *m, double hz, int ms)
{
    const int  total = 7200 * ms / 1000;
    rv_adpcm_t s;
    int        i = 0;

    rv_adpcm_init(&s, 4);
    while (i < total) {
        if (m->port->chardev.status(m->dev) & CHAR_COM_CTS) {
            int16_t smp[2];
            uint8_t b;

            for (int k = 0; k < 2; k++, i++)
                smp[k] = (int16_t) (10000.0 * sin(2 * M_PI * hz * i / 7200.0));
            if (rv_adpcm_encode(&s, smp, 2, &b, 1) == 1) {
                if (b == DLE)
                    m->port->chardev.write(&b, 1, m->dev);
                m->port->chardev.write(&b, 1, m->dev);
            }
            if ((i % 512) == 0)
                pump(0);
        } else
            pump(2);
    }
    {
        uint8_t end[2] = { DLE, ETX };

        m->port->chardev.write(&end[0], 1, m->dev);
        m->port->chardev.write(&end[1], 1, m->dev);
    }
}

/* What #VRX sent, from just after its CONNECT: the audio decoded (4-bit
   ADPCM, 7200 Hz) and the DLE events. */
static size_t
recording(const vmodem_t *m, int16_t *out, size_t max, char *events, size_t ev_max)
{
    rv_adpcm_t s;
    size_t     start = 0, n = 0, ne = 0;

    for (size_t i = 0; (i + 9) <= m->n; i++)
        if (!memcmp(&m->raw[i], "CONNECT\r\n", 9)) {
            start = i + 9;
            break;
        }
    rv_adpcm_init(&s, 4);
    for (size_t i = start; i < m->n; i++) {
        uint8_t b = m->raw[i];

        if (b == DLE) {
            if (++i >= m->n)
                break;
            b = m->raw[i];
            if (b == ETX)
                break;
            if (b != DLE) {
                if (ne < (ev_max - 1))
                    events[ne++] = (char) b;
                continue;
            }
        }
        n += rv_adpcm_decode(&s, &b, 1, out + n, max - n);
    }
    events[ne] = '\0';
    return n;
}

/* +VTX in mu-law at 8000 Hz (+VSM=4,8000); `end` sends DLE ETX after. */
static void
play_ulaw(vmodem_t *m, double hz, int ms, int end)
{
    const int total = 8000 * ms / 1000;

    for (int i = 0; i < total;) {
        if (m->port->chardev.status(m->dev) & CHAR_COM_CTS) {
            uint8_t b = voice_ulaw_encode((int16_t) (10000.0 * sin(2 * M_PI * hz * i / 8000.0)));

            i++;
            if (b == DLE)
                m->port->chardev.write(&b, 1, m->dev);
            m->port->chardev.write(&b, 1, m->dev);
            if ((i % 512) == 0)
                pump(0);
        } else
            pump(2);
    }
    if (end) {
        uint8_t e[2] = { DLE, ETX };

        m->port->chardev.write(&e[0], 1, m->dev);
        m->port->chardev.write(&e[1], 1, m->dev);
    }
}

/* What +VRX (or +VTR) sent in mu-law, from just after its CONNECT. */
static size_t
recording_ulaw(const vmodem_t *m, int16_t *out, size_t max, char *events, size_t ev_max)
{
    size_t start = 0, n = 0, ne = 0;

    for (size_t i = 0; (i + 9) <= m->n; i++)
        if (!memcmp(&m->raw[i], "CONNECT\r\n", 9)) {
            start = i + 9;
            break;
        }
    for (size_t i = start; (i < m->n) && (n < max); i++) {
        uint8_t b = m->raw[i];

        if (b == DLE) {
            if (++i >= m->n)
                break;
            b = m->raw[i];
            if (b == ETX)
                break;
            if (b != DLE) {
                if (ne < (ev_max - 1))
                    events[ne++] = (char) b;
                continue;
            }
        }
        out[n++] = voice_ulaw_decode(b);
    }
    events[ne] = '\0';
    return n;
}

static int16_t rec[7200 * 20];

static void
quiet_log(const char *line)
{
    if (getenv("VOICE_TEST_VERBOSE"))
        printf("      | %s\n", line);
}

static int
voice_calls(void)
{
    isp_srv_config_t cfg;
    char             num[24];
    char             ev[64];
    static const double tones[] = { 500, 600, 1000, 1500, 2100 };
    double           margin;
    size_t           n;

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.listen, sizeof(cfg.listen), "127.0.0.1");
    cfg.http_port = -1;
    cfg.log       = quiet_log;
    isp_srv_load(&cfg);
    if (isp_srv_start(&cfg) != 0)
        return 1;
    snprintf(cfg_exchange, sizeof(cfg_exchange), "127.0.0.1:%d", cfg.port);
    printf("== voice calls on isp-server's telephone network (exchange on %s) ==\n", cfg_exchange);

    cfg_instance = 2;
    snprintf(cfg_number, sizeof(cfg_number), "555-0101");
    A.dev  = char_modem_supra_com_device.init(&char_modem_supra_com_device);
    A.port = last_port;
    cfg_instance = 3;
    snprintf(cfg_number, sizeof(cfg_number), "555-0102");
    B.dev  = char_modem_supra_com_device.init(&char_modem_supra_com_device);
    B.port = last_port;
    {
        const uint32_t until = ppp_client_ms() + 5000;

        while (((char_modem_get_phone(A.slot, NULL, 0, NULL, 0, num, sizeof(num)) != 1) ||
                (char_modem_get_phone(B.slot, NULL, 0, NULL, 0, num, sizeof(num)) != 1)) &&
               ((int32_t) (ppp_client_ms() - until) < 0))
            pump(10);
    }

    /* Unimodem/V's VoiceDialNumberSetup and VoiceAnswer. */
    check("COM2: AT#CLS=8 #VLS=0 #VRN=0 #VBT=1 #VSR=7200 #VBS=4", ok(&A, "ATE0#CLS=8#VLS=0#VRN=0#VBT=1#VSR=7200#VBS=4S30=60"));
    at(&A, "AT#CLS?");
    check("AT#CLS? says 8", said(&A, "8\r\n"));
    check("COM3: AT#CLS=8 #VLS=0 #VBT=1 #VSR=7200 #VBS=4 #VSB=1 #VSS=2", ok(&B, "ATE0#CLS=8#VLS=0#VBT=1#VSR=7200#VBS=4#VSB=1#VSS=2"));
    check("AT#VBS=5 is an error", (at(&B, "AT#VBS=5"), hears(&B, "ERROR", 500)));
    check("AT#VTX on hook is an error", (at(&B, "AT#VTX"), hears(&B, "ERROR", 500)));

    forget(&B);
    bells = 0;
    at(&A, "ATDT5550102");
    check("COM2 dials in voice mode: VCON once dialled", hears(&A, "VCON", 3000));
    check("COM3 rings", hears(&B, "RING", 3000));
    check("...and so does the phone beside it", bells > 0);
    at(&B, "ATA");
    check("COM3 answers in voice mode: VCON", hears(&B, "VCON", 3000));
    pump(200);
    {
        isp_srv_pcall_t pc[4];
        const int       np = isp_srv_phone_calls(pc, 4);

        check("the exchange has a voice call between them",
              (np == 1) && (pc[0].state == ISP_PCALL_ACTIVE) && (pc[0].voice == (ISP_PCALL_FROM_VOICE | ISP_PCALL_TO_VOICE)));
    }
    check("no carrier detect in voice mode", !(A.port->chardev.status(A.dev) & CHAR_COM_DCD));

    /* COM2 plays, COM3 records. */
    at(&B, "AT#VRX");
    check("COM3: AT#VRX gives CONNECT", hears(&B, "CONNECT\r\n", 1000));
    at(&A, "AT#VTX");
    check("COM2: AT#VTX gives CONNECT", hears(&A, "CONNECT\r\n", 1000));
    forget(&A);
    play_tone(&A, 1000.0, 1500);
    check("COM2 plays 1.5 s of a 1 kHz tone, DLE ETX: OK once played out", hears(&A, "OK", 3000));
    at(&A, "AT#VTS=7");
    check("AT#VTS=7: OK once the digit is played", hears(&A, "OK", 2000));
    pump(300);
    n = recording(&B, rec, sizeof(rec) / sizeof(rec[0]), ev, sizeof(ev));
    check("COM3 records it", n > 7200);
    check("...a 1 kHz tone, as ADPCM at 7200 Hz", (loudest(rec, n, 7200, tones, 5, &margin) == 1000) && (margin > 5));
    check("...and hears COM2's digit: <DLE>7", strchr(ev, '7') != NULL);
    forget(&B);
    type(&B, "!");
    pump(100);
    {
        static const uint8_t end[] = { DLE, ETX, '\r', '\n', 'O', 'K' };
        int                  found = 0;

        for (size_t i = 0; (i + sizeof(end)) <= B.n; i++)
            found |= !memcmp(&B.raw[i], end, sizeof(end));
        check("COM3 sends \"!\": recording ends, <DLE><ETX> OK", found);
    }
    check("...and the line after it still works (AT: OK)", ok(&B, "AT"));

    /* The handset picks up on COM3's call: COM2 hears its microphone, and
       its speaker hears COM2. */
    mic_hz = 600.0;
    char_modem_handset(B.slot, CHAR_MODEM_HANDSET_PICKUP, NULL);
    pump(100);
    check("COM3's handset is off hook", (char_modem_handset_state(B.slot) & 1) != 0);
    at(&A, "AT#VRX");
    pump(1200);
    n = recording(&A, rec, sizeof(rec) / sizeof(rec[0]), ev, sizeof(ev));
    check("COM2 hears COM3's handset: the host's microphone (600 Hz)",
          (n > 3600) && (loudest(rec + 1800, n - 1800, 7200, tones, 5, &margin) == 600) && (margin > 5));
    type(&A, "!");
    pump(100);
    mic_hz  = 0.0;
    heard_n = 0;
    at(&A, "AT#VTX");
    play_tone(&A, 1500.0, 1000);
    hears(&A, "OK", 3000);
    pump(200);
    check("...and the host's speaker hears COM2 (1.5 kHz)",
          (heard_n > 4000) && (loudest(heard, heard_n, 8000, tones, 5, &margin) == 1500) && (margin > 3));

    /* COM3 hangs up (the guest: ATH ends the call, the handset or not). */
    at(&A, "AT#VRX");
    pump(100);
    at(&B, "ATH");
    pump(300);
    n = recording(&A, rec, sizeof(rec) / sizeof(rec[0]), ev, sizeof(ev));
    check("COM3 hangs up: COM2, recording, hears the busy tone: <DLE>b", strchr(ev, 'b') != NULL);
    type(&A, "!");
    pump(100);
    check("COM2: ATH", ok(&A, "ATH"));
    char_modem_handset(B.slot, CHAR_MODEM_HANDSET_HANGUP, NULL);
    pump(50);

    /* The handset makes a call of its own: COM2 in data mode answers it, and
       hears no carrier; the handset hears a modem's answer tone. */
    check("COM2 back to data mode: AT#CLS=0 S7=3", ok(&A, "AT#CLS=0S7=3"));
    heard_n = 0;
    forget(&A);
    forget(&B);
    char_modem_handset(B.slot, CHAR_MODEM_HANDSET_DIAL, "5550101");
    check("COM3's handset dials COM2: it rings", hears(&A, "RING", 3000));
    check("...and COM3's own guest hears nothing of it", !said(&B, "VCON") && !said(&B, "OK"));
    at(&A, "ATA");
    pump(1500);
    check("the handset hears a modem: its answer tone (2100 Hz)",
          (heard_n > 4000) && (loudest(heard + heard_n - 4000, 4000, 8000, tones, 5, &margin) == 2100));
    check("COM2, answered by a voice, never gets a carrier: NO CARRIER", hears(&A, "NO CARRIER", 6000));
    pump(1500);
    check("...and COM3's handset is left with the busy tone",
          (heard_n > 8000) && (loudest(heard + heard_n - 8000, 8000, 8000, (const double[]) { 425, 2100 }, 2, NULL) == 425));
    char_modem_handset(B.slot, CHAR_MODEM_HANDSET_HANGUP, NULL);
    pump(100);
    check("COM3's handset on hook: the modem is idle", char_modem_get_state(B.slot) == CHAR_MODEM_IDLE);

    /* V.253 on COM2, Rockwell's set on COM3: they talk all the same. */
    printf("== V.253 (+FCLASS=8) on COM2, Rockwell's set on COM3 ==\n");
    check("COM2: AT+FCLASS=8", ok(&A, "AT+FCLASS=8"));
    at(&A, "AT+FCLASS?");
    check("AT+FCLASS? says 8", said(&A, "8\r\n"));
    at(&A, "AT+VSM=?");
    check("AT+VSM=? lists the formats by name", hears(&A, "\"ULAW\"", 500) && said(&A, "\"SIGNED PCM\",16"));
    check("AT+VSM=4,8000: mu-law", ok(&A, "AT+VSM=4,8000"));
    at(&A, "AT+VSM=77,8000");
    check("AT+VSM=77 (no such format) is an error", hears(&A, "ERROR", 500));
    check("COM3: AT#CLS=8 back in voice mode", ok(&B, "AT#CLS=8"));
    check("AT+VLS=1: COM2 off hook, OK", ok(&A, "AT+VLS=1") && (char_modem_get_state(A.slot) == CHAR_MODEM_VOICE));
    forget(&B);
    at(&A, "ATD5550102");
    check("...and dials from there: OK, not VCON", hears(&A, "OK", 3000) && !said(&A, "VCON"));
    check("COM3 rings", hears(&B, "RING", 3000));
    at(&B, "ATA");
    check("COM3 answers in Rockwell's set: VCON", hears(&B, "VCON", 3000));
    pump(200);

    at(&B, "AT#VRX");
    at(&A, "AT+VTX");
    check("AT+VTX: CONNECT", hears(&A, "CONNECT\r\n", 1000));
    forget(&A);
    play_ulaw(&A, 800.0, 1200, 1);
    check("COM2 plays 800 Hz in mu-law, DLE ETX: OK", hears(&A, "OK", 3000));
    at(&A, "AT+VTS={5,10}");
    check("AT+VTS={5,10}: OK once played", hears(&A, "OK", 2000));
    pump(300);
    n = recording(&B, rec, sizeof(rec) / sizeof(rec[0]), ev, sizeof(ev));
    trace("COM3's recording", rec, n, 7200, (const double[]) { 400, 800, 1200 }, 3);
    check("COM3 records 800 Hz, as Rockwell ADPCM",
          (n > 4000) && (loudest(rec, n, 7200, (const double[]) { 400, 800, 1200 }, 3, &margin) == 800) && (margin > 5));
    check("...and COM2's digit: <DLE>5", strchr(ev, '5') != NULL);
    type(&B, "!");
    pump(100);

    at(&A, "AT+VRX");
    check("AT+VRX: CONNECT", hears(&A, "CONNECT\r\n", 1000));
    at(&B, "AT#VTX");
    play_tone(&B, 1200.0, 1200);
    hears(&B, "OK", 3000);
    pump(300);
    n = recording_ulaw(&A, rec, sizeof(rec) / sizeof(rec[0]), ev, sizeof(ev));
    check("COM2 records COM3's 1200 Hz, in mu-law at 8000 Hz",
          (n > 6000) && (loudest(rec, n, 8000, (const double[]) { 400, 800, 1200 }, 3, &margin) == 1200) && (margin > 5));
    {
        uint8_t stop[2] = { DLE, '!' };

        forget(&A);
        A.port->chardev.write(&stop[0], 1, A.dev);
        A.port->chardev.write(&stop[1], 1, A.dev);
        pump(100);
        check("<DLE>! ends +VRX: <DLE><ETX> OK", said(&A, "\x10\x03\r\nOK"));
    }

    /* Both ways at once. */
    at(&B, "AT#VRX");
    at(&A, "AT+VTR");
    check("AT+VTR: CONNECT", hears(&A, "CONNECT\r\n", 1000));
    play_ulaw(&A, 600.0, 1000, 0);
    {
        uint8_t stop[2] = { DLE, '^' };

        A.port->chardev.write(&stop[0], 1, A.dev);
        A.port->chardev.write(&stop[1], 1, A.dev);
        pump(200);
    }
    check("+VTR: COM2 was heard and listened, <DLE>^ ends it: <DLE><ETX> OK",
          (recording_ulaw(&A, rec, sizeof(rec) / sizeof(rec[0]), ev, sizeof(ev)) > 4000) && said(&A, "\x10\x03\r\nOK"));
    n = recording(&B, rec, sizeof(rec) / sizeof(rec[0]), ev, sizeof(ev));
    trace("COM3's recording", rec, n, 7200, (const double[]) { 300, 600, 1200 }, 3);
    check("...and COM3 heard its 600 Hz", (n > 3600) && (loudest(rec, n, 7200, (const double[]) { 300, 600, 1200 }, 3, NULL) == 600));

    /* +VLS=0: on hook. */
    forget(&B);
    pump(100);
    check("AT+VLS=0 hangs COM2 up", ok(&A, "AT+VLS=0") && (char_modem_get_state(A.slot) == CHAR_MODEM_IDLE));
    pump(300);
    n = recording(&B, rec, sizeof(rec) / sizeof(rec[0]), ev, sizeof(ev));
    check("...and COM3, still recording, hears it: <DLE>b", strchr(ev, 'b') != NULL);
    type(&B, "!");
    pump(100);
    check("COM3: ATH", ok(&B, "ATH"));

    char_modem_supra_com_device.close(A.dev);
    char_modem_supra_com_device.close(B.dev);
    isp_srv_stop();
    return 0;
}

int
main(void)
{
#ifdef _WIN32
    WSADATA wsa;

    WSAStartup(MAKEWORD(2, 2), &wsa);
    timeBeginPeriod(1); /* 1 ms sleeps, not 15.6: the guests here keep real time */
#endif
    codec_tests();
    if (voice_calls() != 0)
        check("isp-server starts", 0);
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed", failures, (failures == 1) ? "" : "s");
    return failures ? 1 : 0;
}
