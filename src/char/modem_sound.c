/*
 * 86Box-Next: what the modem's speaker plays during a call (char_modem.c).
 *
 * Ported from PeepeeBox, where it was written for the funworld Photo Play
 * cabinets' modem.
 *
 * A real data modem is audible from the moment it goes off hook until it has
 * a carrier, and the cabinets asked for exactly that: funworld's own init
 * string, in \FN_SYS\DATABASE\NETWORK\MD_INIT1.CSV, is
 *
 *     ate0m1l3S8=5S7=20S10=40
 *
 * M1 -- speaker on until the carrier, off after -- and L3, loud.  So every
 * night a cabinet in a bar dialled its ISP out loud: dial tone, the number in
 * DTMF, the ringing, then the answer tone and the hiss and warble of the two
 * modems training, and silence once PPP started.
 *
 * This file plays that, and also says how long each part takes, so the modem
 * (char_modem.c) can take exactly as long as a real one: the call takes the
 * same time whether or not anyone is listening.
 *
 * The sequence is the calling side of a V.34 / V.90 call as heard on its own
 * speaker, which hears both ends:
 *
 *   off hook   a click, then the exchange's dial tone
 *   the number DTMF, 90 ms per digit and 90 ms apart; a comma waits S8
 *              seconds and hears the outside line's dial tone meanwhile
 *   ringing    ringback, until the far end answers
 *   V.8        ANSam: 2100 Hz with 15 Hz amplitude modulation and a phase
 *              reversal every 450 ms; the caller's CM and the answerer's JM
 *              in V.21 FSK over the top of it
 *   V.34 ph.2  INFO0 in DPSK at 1200 and 2400 Hz, tones A/B, then line
 *              probing: a comb of tones every 150 Hz -- the "bong"
 *   ph. 3/4    equaliser and echo-canceller training: loud scrambled noise
 *   V.90       (56k) digital impairment learning: a gated, crackling noise
 *   CONNECT    M1 mutes the speaker
 *
 * Everything goes through a telephone band (300-3400 Hz) and a small tinny
 * speaker.  Tones follow CEPT (425 Hz) except where the country code the
 * cabinet set says UK.
 *
 * Threading: events come from the emulation thread (modem_sound_event),
 * samples are made on the sound thread (the handler).  Events go through a
 * small ring whose write index is published last.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include <86box/86box.h>
#include <86box/sound.h>
#include <86box/modem_sound.h>

#ifndef M_PI
#    define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------ timing */

#define MS_DIALTONE   800   /* off hook to the first digit            */
#define MS_DTMF_ON    90
#define MS_DTMF_OFF   90
#define MS_PULSE_MAKE 40    /* 10 pulses per second, 60/40 break/make */
#define MS_PULSE_BRK  60
#define MS_PULSE_GAP  700   /* between pulse-dialled digits           */
#define MS_WAIT_W     1500  /* 'W': wait for a second dial tone       */
#define MS_POSTDIAL   600   /* last digit to the exchange connecting  */
#define MS_FIRST_RING 2600  /* one ring and part of the pause, then answered */

/* The handshake, as the sequence below plays it. */
#define MS_HS_GAP     150
#define MS_HS_ANSAM   1300
#define MS_HS_CMJM    1300
#define MS_HS_INFO    450
#define MS_HS_TONEAB  300
#define MS_HS_L1      160
#define MS_HS_L2      550
#define MS_HS_TRAIN3  1500
#define MS_HS_TRAIN4  1300
#define MS_HS_DIL     1200  /* V.90 only */

uint32_t
modem_sound_dial_ms(const char *number, int s8, int pulse)
{
    uint32_t ms = MS_DIALTONE;

    for (const char *p = number; p && *p; p++) {
        const char c = *p;
        if ((c == 'T') || (c == 't'))
            pulse = 0;
        else if ((c == 'P') || (c == 'p'))
            pulse = 1;
        else if (c == ',')
            ms += (uint32_t) s8 * 1000u;
        else if ((c == 'W') || (c == 'w'))
            ms += MS_WAIT_W;
        else if ((c >= '0') && (c <= '9')) {
            if (pulse) {
                const int n = (c == '0') ? 10 : (c - '0');
                ms += (uint32_t) n * (MS_PULSE_MAKE + MS_PULSE_BRK) + MS_PULSE_GAP;
            } else
                ms += MS_DTMF_ON + MS_DTMF_OFF;
        } else if ((c == '*') || (c == '#') || ((c >= 'A') && (c <= 'D')))
            ms += pulse ? 0 : (MS_DTMF_ON + MS_DTMF_OFF);
    }
    return ms + MS_POSTDIAL;
}

uint32_t
modem_sound_ring_ms(void)
{
    return MS_FIRST_RING;
}

uint32_t
modem_sound_handshake_ms(int v90)
{
    return MS_HS_GAP + MS_HS_ANSAM + MS_HS_CMJM + MS_HS_INFO + MS_HS_TONEAB + MS_HS_L1 + MS_HS_L2
         + MS_HS_TRAIN3 + MS_HS_TRAIN4 + (v90 ? MS_HS_DIL : 0);
}

/* ------------------------------------------------------------ state */

enum {
    PH_IDLE = 0,
    PH_DIAL,     /* dial tone and digits, driven by the dial program   */
    PH_RING,     /* ringback until answered                            */
    PH_HANDSHAKE,
    PH_ONLINE,   /* carrier up: M1 silent, M2 hears the data          */
    PH_BUSY
};

enum { /* dial program steps */
    ST_TONE_DIAL = 0,
    ST_DTMF,
    ST_PULSE,
    ST_SILENCE
};

typedef struct {
    int      kind;
    int      digit;
    uint32_t samples;
} step_t;

#define MAX_STEPS 256
#define EVQ       16

typedef struct {
    int  type;
    int  arg;
    char number[64];
} ev_t;

struct modem_sound_t {
    /* events, emulation thread -> sound thread */
    ev_t        q[EVQ];
    atomic_uint q_write;
    unsigned    q_read;

    /* speaker settings the DTE gave (ATM, ATL) */
    atomic_int mode;   /* 0 off, 1 until carrier, 2 always, 3 on after dialling */
    atomic_int enabled; /* the device's Speaker option                         */
    int        in_use;  /* claimed by a modem (emulation thread only)          */
    atomic_int level;  /* 0..3 */
    int        uk;
    int        v90;

    /* sound thread only */
    int      rate;
    int      phase;
    uint32_t t;         /* samples into the current phase */
    step_t   steps[MAX_STEPS];
    int      nsteps, step;
    uint32_t step_t0;
    double   ph1, ph2, ph3, ph4;
    uint32_t rng;
    int      click;     /* samples of click left */
    int      fsk_bit_left, fsk_bit;
    int      fsk2_bit_left, fsk2_bit;
    int      dpsk_sym_left;
    double   dpsk_phase;
    double   comb_phase[26];
    /* filters */
    double hp_x1, hp_y1, lp1[4], lp2[4], bp[4];
};

static inline uint32_t
ms_to_samples(modem_sound_t *s, uint32_t ms)
{
    return (uint32_t) (((uint64_t) ms * (uint64_t) s->rate) / 1000u);
}

static inline double
noise(modem_sound_t *s)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    return ((double) (s->rng >> 8) / 8388608.0) - 1.0;
}

static inline double
osc(double *ph, double hz, int rate)
{
    *ph += 2.0 * M_PI * hz / rate;
    if (*ph > 2.0 * M_PI)
        *ph -= 2.0 * M_PI;
    return sin(*ph);
}

/* ------------------------------------------------------------ events */

void
modem_sound_event(modem_sound_t *s, int type, const char *number, int arg)
{
    if (s == NULL)
        return;
    const unsigned w = atomic_load(&s->q_write);
    ev_t          *e = &s->q[w % EVQ];
    e->type          = type;
    e->arg           = arg;
    snprintf(e->number, sizeof(e->number), "%s", number ? number : "");
    atomic_store(&s->q_write, w + 1);
}

void
modem_sound_speaker(modem_sound_t *s, int mode, int level)
{
    if (s == NULL)
        return;
    atomic_store(&s->mode, mode);
    atomic_store(&s->level, level);
}

void
modem_sound_country(modem_sound_t *s, int uk, int v90)
{
    if (s != NULL) {
        s->uk  = uk;
        s->v90 = v90;
    }
}

static void
build_dial(modem_sound_t *s, const char *number, int s8, int pulse)
{
    s->nsteps = 0;
#define ADD(k, d, ms)                                                   \
    do {                                                                \
        if (s->nsteps < MAX_STEPS) {                                    \
            s->steps[s->nsteps].kind    = (k);                          \
            s->steps[s->nsteps].digit   = (d);                          \
            s->steps[s->nsteps].samples = ms_to_samples(s, (ms));       \
            s->nsteps++;                                                \
        }                                                               \
    } while (0)
    ADD(ST_TONE_DIAL, 0, MS_DIALTONE);
    for (const char *p = number; *p; p++) {
        const char c = *p;
        if ((c == 'T') || (c == 't'))
            pulse = 0;
        else if ((c == 'P') || (c == 'p'))
            pulse = 1;
        else if (c == ',')
            ADD(ST_TONE_DIAL, 0, (uint32_t) s8 * 1000u);     /* the outside line answers */
        else if ((c == 'W') || (c == 'w'))
            ADD(ST_TONE_DIAL, 0, MS_WAIT_W);
        else if ((c >= '0') && (c <= '9')) {
            if (pulse) {
                const int n = (c == '0') ? 10 : (c - '0');
                for (int i = 0; i < n; i++) {
                    ADD(ST_PULSE, 1, MS_PULSE_BRK);
                    ADD(ST_SILENCE, 0, MS_PULSE_MAKE);
                }
                ADD(ST_SILENCE, 0, MS_PULSE_GAP);
            } else {
                ADD(ST_DTMF, c, MS_DTMF_ON);
                ADD(ST_SILENCE, 0, MS_DTMF_OFF);
            }
        } else if (!pulse && ((c == '*') || (c == '#') || ((c >= 'A') && (c <= 'D')))) {
            ADD(ST_DTMF, c, MS_DTMF_ON);
            ADD(ST_SILENCE, 0, MS_DTMF_OFF);
        }
    }
    ADD(ST_SILENCE, 0, MS_POSTDIAL);
#undef ADD
    s->step    = 0;
    s->step_t0 = 0;
}

static void
take_events(modem_sound_t *s)
{
    const unsigned w = atomic_load(&s->q_write);

    while (s->q_read != w) {
        const ev_t *e = &s->q[s->q_read % EVQ];
        switch (e->type) {
            case MODEM_SOUND_DIAL:
                build_dial(s, e->number, e->arg & 0xff, (e->arg >> 8) & 1);
                s->phase = PH_DIAL;
                s->t     = 0;
                s->click = s->rate / 150;
                break;
            case MODEM_SOUND_ANSWER:
                s->phase = PH_HANDSHAKE;
                s->t     = 0;
                s->click = s->rate / 300;
                for (int i = 0; i < 26; i++)
                    s->comb_phase[i] = (double) ((i * 2654435761u) % 6283u) / 1000.0;
                break;
            case MODEM_SOUND_CONNECT:
                s->phase = PH_ONLINE;
                s->t     = 0;
                break;
            case MODEM_SOUND_BUSY:
                s->phase = PH_BUSY;
                s->t     = 0;
                break;
            case MODEM_SOUND_HANGUP:
            default:
                if (s->phase != PH_IDLE)
                    s->click = s->rate / 150;
                s->phase = PH_IDLE;
                s->t     = 0;
                break;
        }
        s->q_read++;
    }
}

/* ------------------------------------------------------------ generators */

static const double dtmf_row[4] = { 697, 770, 852, 941 };
static const double dtmf_col[4] = { 1209, 1336, 1477, 1633 };

static double
dtmf(modem_sound_t *s, int c)
{
    static const char *keys = "123A456B789C*0#D";
    const char        *k    = strchr(keys, c);
    if (k == NULL)
        return 0.0;
    const int i = (int) (k - keys);
    return 0.5 * osc(&s->ph1, dtmf_row[i / 4], s->rate) + 0.5 * osc(&s->ph2, dtmf_col[i % 4], s->rate);
}

static double
dial_tone(modem_sound_t *s)
{
    if (s->uk)
        return 0.5 * osc(&s->ph1, 350, s->rate) + 0.5 * osc(&s->ph2, 450, s->rate);
    return osc(&s->ph1, 425, s->rate);
}

static double
ringback(modem_sound_t *s)
{
    const uint32_t ms = (uint32_t) (((uint64_t) s->t * 1000u) / s->rate);
    if (s->uk) {
        const uint32_t m = ms % 3000;
        const int on = (m < 400) || ((m >= 600) && (m < 1000));
        return on ? 0.5 * osc(&s->ph1, 400, s->rate) + 0.5 * osc(&s->ph2, 450, s->rate) : 0.0;
    }
    return ((ms % 5000) < 1000) ? osc(&s->ph1, 425, s->rate) : 0.0;
}

static double
busy_tone(modem_sound_t *s)
{
    const uint32_t ms = (uint32_t) (((uint64_t) s->t * 1000u) / s->rate);
    return ((ms % 1000) < 500) ? osc(&s->ph1, s->uk ? 400 : 425, s->rate) : 0.0;
}

/* V.21 FSK at 300 baud with random bits: channel 1 (the caller) 980/1180 Hz,
   channel 2 (the answerer) 1650/1850 Hz. */
static double
fsk(modem_sound_t *s, int ch2)
{
    int *left = ch2 ? &s->fsk2_bit_left : &s->fsk_bit_left;
    int *bit  = ch2 ? &s->fsk2_bit : &s->fsk_bit;
    if (--(*left) <= 0) {
        *left = s->rate / 300;
        *bit  = (noise(s) > 0.0);
    }
    const double f = ch2 ? (*bit ? 1650 : 1850) : (*bit ? 980 : 1180);
    return osc(ch2 ? &s->ph4 : &s->ph3, f, s->rate);
}

/* DPSK at 600 baud on a carrier: the phase jumps by a random multiple of 90
   degrees each symbol. */
static double
dpsk(modem_sound_t *s, double carrier, double *ph)
{
    if (--s->dpsk_sym_left <= 0) {
        s->dpsk_sym_left = s->rate / 600;
        s->dpsk_phase += (M_PI / 2.0) * (double) ((s->rng >> 13) & 3);
        (void) noise(s);
    }
    *ph += 2.0 * M_PI * carrier / s->rate;
    if (*ph > 2.0 * M_PI)
        *ph -= 2.0 * M_PI;
    return sin(*ph + s->dpsk_phase);
}

static double
handshake(modem_sound_t *s)
{
    uint32_t ms = (uint32_t) (((uint64_t) s->t * 1000u) / s->rate);
    double   v  = 0.0;

    if (ms < MS_HS_GAP)
        return 0.0;
    ms -= MS_HS_GAP;

    /* V.8 ANSam: 2100 Hz, 15 Hz AM at 20 %, phase reversal every 450 ms */
    if (ms < MS_HS_ANSAM + MS_HS_CMJM) {
        const double am  = 1.0 + 0.2 * sin(2.0 * M_PI * 15.0 * s->t / s->rate);
        const int    rev = (int) (ms / 450) & 1;
        v                = 0.8 * am * osc(&s->ph1, 2100, s->rate) * (rev ? -1.0 : 1.0);
        if (ms >= MS_HS_ANSAM) {                       /* CM, then JM over it */
            v = 0.45 * v + 0.55 * fsk(s, 0);
            if (ms >= MS_HS_ANSAM + MS_HS_CMJM / 2)
                v = 0.6 * v + 0.5 * fsk(s, 1);
        }
        return v;
    }
    ms -= MS_HS_ANSAM + MS_HS_CMJM;

    /* V.34 phase 2: INFO0 in DPSK, both directions */
    if (ms < MS_HS_INFO)
        return 0.55 * dpsk(s, 1200, &s->ph1) + 0.45 * dpsk(s, 2400, &s->ph2);
    ms -= MS_HS_INFO;

    /* tones A and B, with the 180-degree reversal near the end */
    if (ms < MS_HS_TONEAB) {
        const double sign = (ms > MS_HS_TONEAB - 40) ? -1.0 : 1.0;
        return sign * (0.5 * osc(&s->ph1, 1200, s->rate) + 0.5 * osc(&s->ph2, 2400, s->rate));
    }
    ms -= MS_HS_TONEAB;

    /* line probing L1 (louder) and L2: tones every 150 Hz from 150 to 3750,
       without 900, 1200, 1800 and 2400 */
    if (ms < MS_HS_L1 + MS_HS_L2) {
        const double gain = (ms < MS_HS_L1) ? 0.30 : 0.16;
        for (int k = 1; k <= 25; k++) {
            const int hz = 150 * k;
            if ((hz == 900) || (hz == 1200) || (hz == 1800) || (hz == 2400))
                continue;
            v += osc(&s->comb_phase[k], hz, s->rate);
        }
        return gain * v;
    }
    ms -= MS_HS_L1 + MS_HS_L2;

    /* phases 3 and 4: scrambled data, first near-white, then with the
       carrier's hump around 1800 Hz */
    if (ms < MS_HS_TRAIN3)
        return 0.9 * noise(s);
    ms -= MS_HS_TRAIN3;
    if (ms < MS_HS_TRAIN4)
        return 0.55 * noise(s) + 0.45 * noise(s) * osc(&s->ph1, 1800, s->rate);
    ms -= MS_HS_TRAIN4;

    /* V.90 digital impairment learning: noise gated in a fast pattern */
    if (s->v90 && (ms < MS_HS_DIL)) {
        const int gate = ((s->t / (s->rate / 60)) % 3) != 0;
        return gate ? 0.9 * noise(s) : 0.25 * noise(s);
    }
    return 0.0;
}

/* The telephone band and the speaker: a first-order high-pass at ~350 Hz and
   two biquad low-passes at ~3.3 kHz, then a mild peak near 2 kHz (a small
   cone in a plastic box), then soft clipping. */
static void
biquad_lp(double c[4], double fc, int rate, double q, double *b0, double *b1, double *b2, double *a1, double *a2)
{
    const double w = 2.0 * M_PI * fc / rate, cs = cos(w), al = sin(w) / (2.0 * q), a0 = 1.0 + al;
    *b0 = ((1.0 - cs) / 2.0) / a0;
    *b1 = (1.0 - cs) / a0;
    *b2 = *b0;
    *a1 = (-2.0 * cs) / a0;
    *a2 = (1.0 - al) / a0;
    (void) c;
}

static double
run_biquad(double z[4], double x, double b0, double b1, double b2, double a1, double a2)
{
    const double y = b0 * x + b1 * z[0] + b2 * z[1] - a1 * z[2] - a2 * z[3];
    z[1] = z[0];
    z[0] = x;
    z[3] = z[2];
    z[2] = y;
    return y;
}

static double
line_and_speaker(modem_sound_t *s, double x)
{
    static double lb0, lb1, lb2, la1, la2, pb0, pb1, pb2, pa1, pa2;
    static int    for_rate = 0;

    if (for_rate != s->rate) {
        biquad_lp(NULL, 3300.0, s->rate, 0.707, &lb0, &lb1, &lb2, &la1, &la2);
        /* peaking EQ, +5 dB at 2 kHz, Q 1.2 */
        const double A = pow(10.0, 5.0 / 40.0), w = 2.0 * M_PI * 2000.0 / s->rate;
        const double al = sin(w) / (2.0 * 1.2), a0 = 1.0 + al / A;
        pb0 = (1.0 + al * A) / a0;
        pb1 = (-2.0 * cos(w)) / a0;
        pb2 = (1.0 - al * A) / a0;
        pa1 = pb1;
        pa2 = (1.0 - al / A) / a0;
        for_rate = s->rate;
    }
    const double rc = 1.0 / (2.0 * M_PI * 350.0), dt = 1.0 / s->rate, k = rc / (rc + dt);
    const double hp = k * (s->hp_y1 + x - s->hp_x1);
    s->hp_x1 = x;
    s->hp_y1 = hp;
    double y = run_biquad(s->lp1, hp, lb0, lb1, lb2, la1, la2);
    y        = run_biquad(s->lp2, y, lb0, lb1, lb2, la1, la2);
    y        = run_biquad(s->bp, y, pb0, pb1, pb2, pa1, pa2);
    return tanh(1.3 * y);
}

/* ------------------------------------------------------------ the handler */

static void
modem_sound_get_buffer(int32_t *buffer, uint16_t len, void *priv)
{
    modem_sound_t *s = (modem_sound_t *) priv;

    if ((s == NULL) || (buffer == NULL) || (sound_sample_rate <= 0))
        return;
    s->rate = sound_sample_rate;
    take_events(s);

    const int    mode    = atomic_load(&s->mode);
    const int    level   = atomic_load(&s->level);
    static const double levels[4] = { 0.30, 0.30, 0.55, 0.85 };
    const int    audible = atomic_load(&s->enabled) && (mode != 0);
    const double gain    = 11000.0 * levels[(level < 0) ? 0 : ((level > 3) ? 3 : level)];

    for (uint16_t i = 0; i < len; i++) {
        double x   = 0.0;
        int    hear = 0;

        switch (s->phase) {
            case PH_DIAL: {
                hear = (mode == 1) || (mode == 2);
                if (s->step < s->nsteps) {
                    const step_t *st = &s->steps[s->step];
                    const uint32_t k = s->t - s->step_t0;
                    switch (st->kind) {
                        case ST_TONE_DIAL: x = 0.7 * dial_tone(s); break;
                        case ST_DTMF:      x = 0.8 * dtmf(s, st->digit); break;
                        case ST_PULSE:     x = (k < (uint32_t) (s->rate / 400)) ? 0.9 * noise(s) : 0.0; break;
                        default:           x = 0.0; break;
                    }
                    if (k + 1 >= st->samples) {
                        s->step++;
                        s->step_t0 = s->t + 1;
                    }
                } else {
                    s->phase = PH_RING;
                    s->t     = 0;
                }
                break;
            }
            case PH_RING:
                hear = (mode == 1) || (mode == 2) || (mode == 3);
                x    = 0.6 * ringback(s);
                break;
            case PH_BUSY:
                hear = (mode == 1) || (mode == 2) || (mode == 3);
                x    = 0.6 * busy_tone(s);
                break;
            case PH_HANDSHAKE:
                hear = (mode == 1) || (mode == 2) || (mode == 3);
                x    = handshake(s);
                break;
            case PH_ONLINE:
                hear = (mode == 2);
                x    = 0.35 * noise(s);   /* M2: the data, as a hiss */
                break;
            default:
                break;
        }
        if (s->click > 0) {
            x += 0.9 * noise(s) * ((double) s->click / (s->rate / 150.0));
            s->click--;
            hear = 1;
        }
        if (s->phase != PH_IDLE)
            x += 0.015 * noise(s);                 /* the line's own hiss */
        s->t++;

        const double y = line_and_speaker(s, hear ? x : 0.0);
        if (audible) {
            const int32_t out = (int32_t) (y * gain);
            buffer[(i << 1)] += out;
            buffer[(i << 1) + 1] += out;
        }
    }
}

/* One speaker per modem, from a small static pool.  The sound core has no
   way to take a handler back: it drops them all on a hard reset, but a modem
   is hot-pluggable and comes and goes on a soft settings change too, while
   the sound thread may still be calling its handler.  So a speaker is never
   freed -- a closed modem's slot goes quiet and is reused by the next one --
   and its handler is registered only when the sound core does not already
   have it. */
#define MODEM_SOUND_SLOTS 8

static modem_sound_t *speakers[MODEM_SOUND_SLOTS];

modem_sound_t *
modem_sound_init(int enabled)
{
    modem_sound_t *s = NULL;

    for (int i = 0; i < MODEM_SOUND_SLOTS; i++) {
        if (speakers[i] == NULL) {
            speakers[i] = (modem_sound_t *) calloc(1, sizeof(modem_sound_t));
            if (speakers[i] == NULL)
                return NULL;
            atomic_init(&speakers[i]->q_write, 0);
            speakers[i]->rng = 0x4d4f4445u + i;
        }
        if (!speakers[i]->in_use) {
            s = speakers[i];
            break;
        }
    }
    if (s == NULL)
        return NULL;

    s->in_use = 1;
    s->q_read = atomic_load(&s->q_write);   /* nothing left over from before */
    s->phase  = PH_IDLE;
    s->click  = 0;
    atomic_store(&s->mode, 1);
    atomic_store(&s->level, 2);
    atomic_store(&s->enabled, !!enabled);
    s->rate = sound_sample_rate > 0 ? sound_sample_rate : SOUND_FREQ;
    if (!sound_has_handler(modem_sound_get_buffer, s))
        sound_add_handler(modem_sound_get_buffer, s);
    return s;
}

void
modem_sound_close(modem_sound_t *s)
{
    if (s != NULL) {
        modem_sound_event(s, MODEM_SOUND_HANGUP, NULL, 0);
        s->in_use = 0;
    }
}
