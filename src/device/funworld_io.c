/*
 * 86Box-Next  The funworld Photo Play / I.G.O. I/O card, ported from
 *             PeepeeBox (github.com/Xeon3D/PeepeeBox) as an optional ISA card,
 *             one of the arcade I/O boards (io_board.c).
 *
 *             The funworld I/O card.
 *
 *             An ISA card the cabinets carried, built around an NEC D71055C --
 *             an 8255-compatible PPI, so three 8-bit ports and a control
 *             register at base+3.  A ULN2003 drives what leaves the card
 *             (the coin acceptor's inhibit line, the counters, the lamps); a
 *             74HC14 conditions what arrives; a 74LS682 compares the address
 *             against an 8-way DIP switch, which is what makes the base
 *             address a setting rather than a constant.
 *
 *             Wired to it: a Coin Controls C120 validator on a 10-way IDC, and
 *             the two buttons behind the cabinet door -- one for the operator
 *             setup, one for the touchscreen calibration.
 *
 *             The C120's contract (its manual, section 4.2) is the part that
 *             constrains this: six *separate* accept lines, one per coin, each
 *             an open-collector NPN pulled **low** for 100 ms +/- 20% on a good
 *             coin.  The manual is emphatic that the host must see the line
 *             held, not merely edge-detect it -- "NOT LESS THAN 50 mS" -- so a
 *             coin here is a timer, not a flag poked and cleared.  Anything
 *             shorter is a coin the software will not count, and it would fail
 *             silently, which is the failure mode this cabinet specialises in.
 *
 *             Where it lives was not documented anywhere; the disk was asked
 *             instead.  Booting I.G.O. 7 with PEEPEEBOX_IO_PROBE=00 caught a
 *             resident program at segment 06FC writing control word **0x99** to
 *             **0x213** and then reading 0210 and 0212 and writing 0211 --
 *             which is an 8255 at base 0x210 with, decoded out of 0x99:
 *
 *                 mode 0 throughout, port A input, port B output,
 *                 port C input in both halves.
 *
 *             So the coins and the buttons arrive on A and C, and B is what
 *             drives the ULN2003 -- the acceptor's inhibit line, the counters
 *             and the lamps.
 *
 *             Which bit is which was then measured, one line at a time, against
 *             the operator setup's book-keeping page on an I.G.O. 6 rig: six
 *             coins on A6, A7 and C4..C7, four notes on C0..C3, and the two
 *             groups rest opposite ways round.  The map and the idle levels
 *             below carry the working; neither is a guess any more.
 *
 * Authors:    The HUEG PP team.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/io.h>
#include <86box/timer.h>
#include <86box/plat.h>
#include <86box/funworld_io.h>

/* The 8255's four registers. */
#define FWIO_PORT_A   0
#define FWIO_PORT_B   1
#define FWIO_PORT_C   2
#define FWIO_CTRL     3
#define FWIO_LEN      4

/* The addresses the software sweeps looking for the card, from a boot traced
   with PEEPEEBOX_IO_TRACE: 0203, 0207, 0233 ... 02F7, every base+3 in the
   range.  The sweep is over 4-byte blocks, so these are the bases. */
#define FWIO_PROBE_FIRST 0x200
#define FWIO_PROBE_LAST  0x2fc

/* How long a coin holds its line.  The C120 says 100 ms +/- 20%, and the host is
   required to want at least 50 ms of it.  One hold books one coin, confirmed on
   the rig once the idle levels were right.

   This was for a while believed to be broken -- one hold appeared to book five
   coins -- which was the wrong idle level rather than a debounce fault; see the
   idle comment below.  PEEPEEBOX_IO_MS is kept because measuring a width is
   still cheaper than arguing about one, not because anything needs it. */
static double fwio_hold_ms = 100.0;

#define FWIO_COIN_MS  fwio_hold_ms

typedef struct fwio_t {
    uint16_t   base;

    uint8_t    ctrl;               /* last control word written           */
    uint8_t    out[3];             /* what the guest last drove outward   */
    uint8_t    in[3];              /* what the card presents to the guest */

    pc_timer_t release[FWIO_IN_LINES];
    uint16_t   held;               /* bitmap of lines currently asserted  */

    pc_timer_t train;              /* a burst of pulses on one line       */
    int        train_left;
    uint8_t    train_port;
    uint8_t    train_bit;
} fwio_t;

static fwio_t *fwio_inst = NULL;

#ifdef ENABLE_FUNWORLD_IO_LOG
int funworld_io_do_log = ENABLE_FUNWORLD_IO_LOG;
#else
int funworld_io_do_log = -1;   /* -1: ask the environment on first use */
#endif

/* Also on with PEEPEEBOX_IO_TRACE, which is what is set when someone is looking
   at this card at all -- having to rebuild to see it answer is a poor trade. */
static void
fwio_log(const char *fmt, ...)
{
    va_list ap;

    if (funworld_io_do_log < 0)
        funworld_io_do_log = (getenv("PEEPEEBOX_IO_TRACE") != NULL);
    if (!funworld_io_do_log)
        return;
    va_start(ap, fmt);
    pclog_ex(fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- the card */

/* What the inputs read when nothing is happening.

   Two devices share this loom and they rest opposite ways round, which is what
   made every earlier reading of the card wrong.

   The six C120 coin lines rest **high**, and a coin pulls one **low** -- exactly
   what the validator's manual says its open-collector outputs do.  The four bill
   validator lines rest **low** and a note drives one **high**.  A0 (the setup
   button) and A1 (the CRC check) rest high, with the coins.

       port A  idle FF   A0 setup, A1 CRC, A6 A7 coins, A2..A5 nothing
       port B  output    never read
       port C  idle F0   C0..C3 the notes, C4..C7 coins

   How that was settled.  With everything rested low -- the old
   PEEPEEBOX_IO_IDLE=00 -- pulsing one coin line high booked a coin on *each of
   the other five*, while pulsing a note line booked its one note.  Six presses,
   six sets of five, and every set is the six coin channels minus one:

       A6 -> everything but 2.00       C4 -> everything but 0.10
       A7 -> everything but TOKEN 10   C5 -> everything but 0.20
                                       C6 -> everything but 0.50
                                       C7 -> everything but 1.00

   That is not six coin lines misfiring.  It is the software reading the port and
   booking every coin line it finds low -- finding five, because we were resting
   them all there, and the pressed line was the only one we had lifted.  So the
   coin a line carries is the one *missing* from its set, and the notes read
   correctly all along because they are the group we happened to be driving the
   right way up.  (Marcos, on the I.G.O. 6 rig, 2026-09-05.)

   It also disposes of the "one 100 ms hold books five coins" defect recorded in
   the research notes: never five counts of one coin, but one count on each of
   five other channels.  There is nothing wrong with the debounce.

   PEEPEEBOX_IO_IDLE=ff,ff,f0 overrides the three ports in order, for when some
   other image disagrees. */
static uint8_t fwio_idle[3] = { 0xff, 0xff, 0xf0 };

/* There was a PEEPEEBOX_IO_PHASE here, which presented an asserted line only
   while port B bit 7 was high or low.  It existed because B7 looked like a ~3 Hz
   square wave and a spare ULN2003 output squaring away like that reads as a bank
   select.  It is not a bank select and it is not a square wave: **B7 is a
   mechanical coin counter**, and what looked like a free-running wave was the
   counter being driven five times per press, back when a press booked five
   channels.  The knob is gone with the theory. */

/* PEEPEEBOX_IO_HOLD=A2,A3 -- lines pinned to their asserted state for the whole
   run, on top of whatever the walk is doing.

   The walk clears every line to idle before each click, which is right for
   finding a pulse and wrong if some line has to be *held* for the pulse to mean
   anything -- an acceptor-enabled or acceptor-present signal, the sort of thing
   a validator loom carries alongside its accept lines.  Anything like that has
   been knocked down before every click so far.

   A2..A5 look like the candidates, being the lines that have never answered
   anything -- but note that until fwio_read_env() they were never tested
   individually either, so "A2..A5 must be enabling something" is a reading of a
   walk that was ignoring its own settings.  Try the pin first; this is for if
   that comes back negative. */
static uint8_t fwio_hold_mask[3] = { 0, 0, 0 };

static uint8_t
fwio_idle_of(uint8_t port)
{
    return (port < 3) ? fwio_idle[port] : 0xff;
}

static void
fwio_idle_all(fwio_t *dev)
{
    /* Held lines read asserted, which is away from their idle -- so flip them. */
    for (uint8_t port = 0; port < 3; port++)
        dev->in[port] = fwio_idle_of(port) ^ fwio_hold_mask[port];
}

static void
fwio_reset(fwio_t *dev)
{
    dev->ctrl = 0x9b;              /* all ports input, mode 0 */
    memset(dev->out, 0x00, sizeof(dev->out));
    fwio_idle_all(dev);
    dev->held = 0;
}

static uint8_t
fwio_read(uint16_t port, void *priv)
{
    fwio_t       *dev = (fwio_t *) priv;
    const uint8_t reg = (uint8_t) (port - dev->base);
    uint8_t       ret = 0xff;

    switch (reg) {
        case FWIO_PORT_A:
        case FWIO_PORT_B:
        case FWIO_PORT_C:
            ret = dev->in[reg];
            break;

        case FWIO_CTRL:
            /* An 8255's control register is write-only; a real one leaves the
               bus floating and the host reads 0xFF.  Kept explicit because the
               software's card-detection reads exactly this address. */
            ret = 0xff;
            break;

        default:
            break;
    }

    fwio_log("FWIO: read  %04X (reg %d) = %02X\n", port, reg, ret);
    return ret;
}

/* Port B is the card's output side: the ULN2003 drives the coin acceptor's
   inhibit line, the lamps, and the **two mechanical coin counters** that leave
   on the DB15.  Which bit is which has never been known, and the guest will say
   so if asked -- book a coin and watch which bit pulses.  That is cheaper than
   tracing wire, and it works on any image rather than on the one cabinet whose
   loom is in front of us.

   B7 was excluded at first, on the ~3 Hz square wave recorded in the research
   notes -- and that was a mistake waiting to happen, because there are two
   counters and blanking a bit could hide one of them.  A plain I.G.O. 6 boot
   writes port B exactly once, so that square wave is not a property of this
   image anyway.  Every bit is logged and every bit is capped instead: a bit that
   turns out to be chatty costs one line, not a gigabyte, and nothing is hidden
   on the strength of an observation made somewhere else. */
#define FWIO_OUT_LOG_CAP 200

/* Milliseconds since the first thing this card logged.  Five pulses is a number;
   five pulses 120 ms apart and 60 ms wide is a mechanical counter being driven,
   and five pulses seconds apart is something else entirely.  Without the stamp
   those readings are indistinguishable in the log, which is how "B7 is a ~3 Hz
   square wave" got written down. */
static uint32_t
fwio_ms(void)
{
    static uint32_t base = 0;
    const uint32_t  now  = plat_get_ticks();

    if (base == 0)
        base = now ? now : 1;
    return now - base;
}

static void
fwio_log_out_b(uint8_t was, uint8_t now)
{
    static unsigned seen[8];
    const uint8_t   changed = (uint8_t) (was ^ now);

    for (uint8_t bit = 0; bit < 8; bit++) {
        if (!(changed & (1 << bit)))
            continue;

        if (seen[bit] < FWIO_OUT_LOG_CAP)
            pclog("FWIO-OUT: %6u ms  port B bit %d -> %d\n",
                  fwio_ms(), bit, (now >> bit) & 1);
        else if (seen[bit] == FWIO_OUT_LOG_CAP)
            pclog("FWIO-OUT: port B bit %d has changed %d times; not logging it "
                  "again\n", bit, FWIO_OUT_LOG_CAP);
        seen[bit]++;
    }
}

static void
fwio_write(uint16_t port, uint8_t val, void *priv)
{
    fwio_t       *dev = (fwio_t *) priv;
    const uint8_t reg = (uint8_t) (port - dev->base);

    switch (reg) {
        case FWIO_PORT_B:
            fwio_log_out_b(dev->out[reg], val);
            dev->out[reg] = val;
            break;

        case FWIO_PORT_A:
        case FWIO_PORT_C:
            dev->out[reg] = val;
            break;

        case FWIO_CTRL:
            if (val & 0x80) {
                /* Mode set: the 8255 clears its output latches.  Logged because
                   it says which ports are inputs, and a run where that differs
                   from the 0x99 we decoded is a run whose every other reading
                   has to be re-examined. */
                pclog("FWIO: control %02X -- port A %s, port B %s, "
                      "port C upper %s, lower %s\n", val,
                      (val & 0x10) ? "in" : "out",
                      (val & 0x02) ? "in" : "out",
                      (val & 0x08) ? "in" : "out",   /* upper half */
                      (val & 0x01) ? "in" : "out");  /* lower half */
                dev->ctrl = val;
                memset(dev->out, 0x00, sizeof(dev->out));
            } else {
                /* Bit set/reset on port C. */
                const uint8_t bit = (val >> 1) & 7;

                if (val & 1)
                    dev->out[FWIO_PORT_C] |= (uint8_t) (1 << bit);
                else
                    dev->out[FWIO_PORT_C] &= (uint8_t) ~(1 << bit);
            }
            break;

        default:
            break;
    }

    fwio_log("FWIO: write %04X (reg %d) = %02X\n", port, reg, val);
}

/* ------------------------------------------------------------- input lines */

/* The map, and it is measured rather than reasoned about now.

   Ten lines, each pulled once from a cleared book-keeping page on an I.G.O. 6
   rig, reading off which channel it incremented.  See the idle-level comment
   above for why the six coins had to be read as the channel each press did *not*
   book, and the four notes straight off.

       A0   operator setup button          confirmed, active low
       A1   the other door button          active low; see below
       A2   probably not connected
       A3   probably not connected
       A4   probably not connected
       A5   probably not connected
       A6   coin  2.00 EUR
       A7   coin  TOKEN 10
       C0   note  5 EUR
       C1   note  10 EUR
       C2   note  20 EUR
       C3   note  50 EUR
       C4   coin  0.10 EUR
       C5   coin  0.20 EUR
       C6   coin  0.50 EUR
       C7   coin  1.00 EUR

   A1 is very probably the touchscreen calibration button, on a count of wires
   rather than on anything seen on screen.  The card has two connectors: a DB25
   carrying the acceptor loom, which is the ten money lines above, and a DB15
   carrying two mechanical coin counters (outputs, through the ULN2003), two
   buttons -- operator setup and calibration -- and two pins that join a serial
   connector.  Ten money lines plus two buttons is twelve inputs; ports A and C
   have sixteen bits between them; A0 is the setup button.  So the calibration
   button is A1, and A2..A5 are the four spare bits with nothing on them.

   What A1 does is then context-dependent, which this software's controls already
   are -- the C key is a credit on a game's start page and the CRC check on the
   menu.  Pressed from the menu, A1 starts a CRC check.  Whether it calibrates
   from somewhere else has not been tried yet.

   Do not read "A2..A5 do nothing" as tested.  They were pulled only during runs
   that rested port A low -- which holds an active-low line asserted from
   power-on, so it can never make the transition anything is looking for -- and
   during runs where the pin was silently ignored.  With the idle levels right,
   nobody has pressed them.  The wire count says they are unconnected; that is a
   prediction, and pulling them one at a time is what would falsify it.

   One table, so that correcting it stays a one-line job. */
static const struct {
    uint8_t port;
    uint8_t bit;
} fwio_line_map[FWIO_IN_LINES] = {
    { FWIO_PORT_C, 4 }, /* coin 1 -- 0.10 EUR                  */
    { FWIO_PORT_C, 5 }, /* coin 2 -- 0.20 EUR                  */
    { FWIO_PORT_C, 6 }, /* coin 3 -- 0.50 EUR                  */
    { FWIO_PORT_C, 7 }, /* coin 4 -- 1.00 EUR                  */
    { FWIO_PORT_A, 6 }, /* coin 5 -- 2.00 EUR                  */
    { FWIO_PORT_A, 7 }, /* coin 6 -- TOKEN 10                  */
    { FWIO_PORT_A, 0 }, /* operator setup button -- confirmed  */
    { FWIO_PORT_A, 1 }, /* second door button: CRC from the menu */
    { FWIO_PORT_C, 0 }, /* note 1 -- 5 EUR                     */
    { FWIO_PORT_C, 1 }, /* note 2 -- 10 EUR                    */
    { FWIO_PORT_C, 2 }, /* note 3 -- 20 EUR                    */
    { FWIO_PORT_C, 3 }, /* note 4 -- 50 EUR                    */
};

/* For the log.  A run has to say what was pressed as well as what happened, or
   "nothing in the log" cannot be told apart from "nothing was pressed" -- which
   is exactly how the first port B run came back unreadable. */
static const char *fwio_line_names[FWIO_IN_LINES] = {
    "coin 1", "coin 2", "coin 3", "coin 4", "coin 5", "coin 6",
    "setup button", "second door button",
    "note 1", "note 2", "note 3", "note 4"
};

static void
fwio_set_bit(fwio_t *dev, uint8_t port, uint8_t bit, int asserted)
{
    const uint8_t mask = (uint8_t) (1 << bit);
    const int     high = !(fwio_idle_of(port) & mask); /* asserted is away from idle */

    if (asserted == high)
        dev->in[port] |= mask;
    else
        dev->in[port] &= (uint8_t) ~mask;
}

static void
fwio_set_line(fwio_t *dev, int line, int asserted)
{
    fwio_set_bit(dev, fwio_line_map[line].port, fwio_line_map[line].bit, asserted);
}

/* PEEPEEBOX_IO_WALK: every pulse steps to the next line of the card rather than
   using the map above -- A0..A7, then B0..B7, then C0..C7, and round again.  One
   button, clicked through, names every line in a single sitting, which beats a
   rebuild per guess.  Each step is logged and shown on screen, so there is no
   click-counting to get wrong.

   Steps the walk refuses to take.  B is an output -- control word 0x99 says so
   and the guest never reads it -- so driving it could only waste clicks.  And A1
   starts the CRC check, which takes the machine away for minutes and says
   nothing new.  That leaves A0, A2..A7 and C0..C7: fifteen clicks. */
/* Twenty-four lines, which is the whole card.  The walk used to carry on to
   four more on COM2, back when the validator was thought to be on the serial
   port; there is nothing there. */
#define FWIO_WALK_STEPS 24

static int  fwio_walk         = -1;
static int  fwio_walk_at      = 0;
static int  fwio_pin          = -1;   /* PEEPEEBOX_IO_LINE=A6: stay on one line  */
static int  fwio_only_port    = -1;   /* PEEPEEBOX_IO_LINE=C:  walk one port only */

/* PEEPEEBOX_IO_PULSES: how many pulses one click sends.

   The six-separate-lines reading comes from the C120's own manual, and it is
   certainly how a C120 behaves.  But the machine takes a 5 EUR *note* as well as
   coins, so there is a second validator on the loom, and nothing says funworld
   wired either of them one-line-per-denomination rather than counting pulses on
   a single credit line -- which is the other common arrangement and would
   explain totals that decompose into no single denomination.

   One pulse is a C120 coin.  More than one, at the same 100 ms cadence, is the
   pulse-counting reading.  A run each settles which. */
static int  fwio_pulses       = 1;

static char fwio_walk_last[48] = "";

static const uint8_t fwio_walk_skip[FWIO_WALK_STEPS] = {
    0, 1, 0, 0, 0, 0, 0, 0,   /* port A -- A1 is the CRC check, never walked */
    1, 1, 1, 1, 1, 1, 1, 1,   /* port B -- outputs: the mechanical coin counters */
    0, 0, 0, 0, 0, 0, 0, 0    /* port C */
};

static void
fwio_train_tick(UNUSED(void *priv))
{
    fwio_t *dev = fwio_inst;

    if (dev == NULL)
        return;

    dev->train_left--;
    if (dev->train_left <= 0) {
        fwio_set_bit(dev, dev->train_port, dev->train_bit, 0);
        pclog("FWIO-WALK: burst finished\n");
        return;
    }

    /* Odd counts are the gaps, even the holds -- an even number of transitions
       per pulse, so the line always finishes released. */
    fwio_set_bit(dev, dev->train_port, dev->train_bit, dev->train_left & 1);
    timer_on_auto(&dev->train, FWIO_COIN_MS * 1000.0);
}

static void
fwio_walk_release(UNUSED(void *priv))
{
    fwio_t *dev = fwio_inst;

    if (dev != NULL) {
        fwio_idle_all(dev);
        pclog("FWIO-WALK: released\n");
    }
}

static void
fwio_release(void *priv)
{
    fwio_t *dev = fwio_inst;
    const int line = (int) (intptr_t) priv;

    if (dev == NULL)
        return;

    fwio_set_line(dev, line, 0);
    dev->held &= (uint16_t) ~(1 << line);
    fwio_log("FWIO: line %d released\n", line);
}

/* The environment is read once, here, and not on the first pulse.

   It used to be read on the first pulse, behind an `if (fwio_walk < 0)` guard --
   and that guard can never be true there.  fwio_init sets fwio_walk before it
   publishes fwio_inst, and a pulse with no fwio_inst returns before reaching the
   guard, so by the time any pulse could run it fwio_walk is already 0 or 1.  The
   block was dead from the day the same variable was given a default in init:
   PEEPEEBOX_IO_LINE, _MS, _PULSES, _PHASE and _HOLD were parsed nowhere, and
   every run that set one of them quietly did the plain full walk instead.

   Which is the answer to "the coins only credit if I walk A2..A5 first": pinning
   never took, so reaching A6 always meant clicking through A2..A5 to get there.
   They are steps on the way, not an enable.

   The settings are logged, because a variable that is read and ignored looks
   exactly like a variable that is read and obeyed. */
static void
fwio_read_env(void)
{
    const char *line   = getenv("PEEPEEBOX_IO_LINE");
    const char *hold   = getenv("PEEPEEBOX_IO_HOLD");
    const char *ms     = getenv("PEEPEEBOX_IO_MS");
    const char *pulses = getenv("PEEPEEBOX_IO_PULSES");
    const char *idle   = getenv("PEEPEEBOX_IO_IDLE");

    /* "ff,ff,f0" -- one byte per port, in order, short lists leaving the rest
       at the wiring the cabinet actually has. */
    for (int i = 0; (idle != NULL) && (i < 3) && (idle[0] != 0); i++) {
        fwio_idle[i] = (uint8_t) strtoul(idle, NULL, 16);
        idle = strchr(idle, ',');
        if (idle == NULL)
            break;
        idle++;
    }

    fwio_walk = (getenv("PEEPEEBOX_IO_WALK") != NULL) || (line != NULL);

    for (const char *q = hold; (q != NULL) && (q[0] != 0) && (q[1] != 0); ) {
        const int hp = (q[0] & ~0x20) - 'A';
        const int hb = q[1] - '0';

        if ((hp >= 0) && (hp < 3) && (hb >= 0) && (hb < 8))
            fwio_hold_mask[hp] |= (uint8_t) (1 << hb);
        q = strchr(q, ',');
        if (q != NULL)
            q++;
    }

    if (ms != NULL) {
        const double v = atof(ms);

        if ((v >= 1.0) && (v <= 5000.0))
            fwio_hold_ms = v;
    }

    if (pulses != NULL) {
        const int n = atoi(pulses);

        if ((n >= 1) && (n <= 64))
            fwio_pulses = n;
    }

    /* "A6" pins one line.  "C" on its own walks that port and nothing else,
       which is how to reach port C without click one being A0 -- A0 opens the
       operator setup, and everything after it then happens in the wrong machine
       state.  That is what spoiled the first port C pass. */
    if ((line != NULL) && (line[0] != '\0')) {
        const int port = (line[0] & ~0x20) - 'A';

        if ((port >= 0) && (port < 3)) {
            if (line[1] == '\0') {
                fwio_only_port = port;
            } else {
                const int bit = line[1] - '0';

                if ((bit >= 0) && (bit < 8))
                    fwio_pin = (port * 8) + bit;
            }
        }
    }

    pclog("FWIO: idle A=%02X B=%02X C=%02X, walk %d, pin %d, only port %d, "
          "%g ms, x%d, hold A=%02X B=%02X C=%02X\n",
          fwio_idle[0], fwio_idle[1], fwio_idle[2], fwio_walk, fwio_pin,
          fwio_only_port, fwio_hold_ms, fwio_pulses,
          fwio_hold_mask[0], fwio_hold_mask[1], fwio_hold_mask[2]);
}

void
funworld_io_pulse(int line)
{
    fwio_t *dev = fwio_inst;

    if ((dev == NULL) || (line < 0) || (line >= FWIO_IN_LINES))
        return;

    /* Only the coin button is hijacked.  The setup and calibration buttons keep
       working off the map, because reaching the operator setup is how you get the
       machine into the state where the one positive result so far happened -- and
       until now turning the walk on took that away, since every button went to
       the same stepper. */
    if (fwio_walk && (line == FWIO_LINE_COIN1)) {
        uint8_t port;
        uint8_t bit;

        /* PEEPEEBOX_IO_LINE=A6 pins the walk to one line, so the button pulses
           the same thing every click.  "It credited once and never again" is a
           different fault from "it credited once because I only pressed it once",
           and stepping past the line is no way to tell them apart. */
        if (fwio_pin >= 0)
            fwio_walk_at = fwio_pin;
        else if (fwio_only_port >= 0) {
            if ((fwio_walk_at / 8) != fwio_only_port)
                fwio_walk_at = fwio_only_port * 8;
        }

        for (int guard = 0; fwio_walk_skip[fwio_walk_at] && (guard < FWIO_WALK_STEPS); guard++)
            fwio_walk_at = (fwio_walk_at + 1) % FWIO_WALK_STEPS;

        port = (uint8_t) (fwio_walk_at / 8);
        bit  = (uint8_t) (fwio_walk_at % 8);

        fwio_idle_all(dev);
        fwio_set_bit(dev, port, bit, 1);

        dev->train_left = 0;
        if (fwio_pulses > 1) {
            /* One transition already made; the tick alternates for the rest and
               always lands released. */
            dev->train_port = port;
            dev->train_bit  = bit;
            dev->train_left = (fwio_pulses * 2) - 1;
            timer_on_auto(&dev->train, FWIO_COIN_MS * 1000.0);
        }
        if (fwio_pulses > 1)
            snprintf(fwio_walk_last, sizeof(fwio_walk_last), "port %c bit %d  x%d",
                     (char) ('A' + port), bit, fwio_pulses);
        else
            snprintf(fwio_walk_last, sizeof(fwio_walk_last), "port %c bit %d  (idle %02X)",
                     (char) ('A' + port), bit, fwio_idle_of(port));
        pclog("FWIO-WALK: %s held %g ms\n", fwio_walk_last, FWIO_COIN_MS);
        timer_on_auto(&dev->release[0], FWIO_COIN_MS * 1000.0);
        fwio_walk_at = (fwio_walk_at + 1) % FWIO_WALK_STEPS;
        if ((fwio_only_port >= 0) && ((fwio_walk_at / 8) != fwio_only_port))
            fwio_walk_at = fwio_only_port * 8;
        return;
    }

    /* Coins used to be forwarded to the C120 on COM2 here, on the reading that
       the validator was on the serial port.  It is not: all ten money lines are
       on this card, measured one at a time on I.G.O. 6.  The forward is gone
       because it swallowed every coin the map would otherwise have delivered.

       A second coin while the first is still on the wire is not a thing the
       validator can do -- it holds the line for 100 ms and will not start
       another until it lets go.  Restarting the timer instead of stacking keeps
       that true. */
    fwio_set_line(dev, line, 1);
    dev->held |= (uint16_t) (1 << line);
    timer_on_auto(&dev->release[line], FWIO_COIN_MS * 1000.0);
    pclog("FWIO-IN:  %6u ms  %s -- port %c bit %d, held %g ms\n",
          fwio_ms(), fwio_line_names[line],
          (char) ('A' + fwio_line_map[line].port),
          fwio_line_map[line].bit, FWIO_COIN_MS);
}

int
funworld_io_present(void)
{
    return fwio_inst != NULL;
}

/* -------------------------------------------------------------- the device */

static void *
fwio_init(const device_t *info)
{
    fwio_t *dev = (fwio_t *) calloc(1, sizeof(fwio_t));

    if (dev == NULL)
        return NULL;

    dev->base = (uint16_t) device_get_config_hex16("base");
    fwio_read_env();               /* before the reset: it sets the hold mask */
    fwio_reset(dev);

    timer_add(&dev->train, fwio_train_tick, NULL, 0);

    for (int i = 0; i < FWIO_IN_LINES; i++)
        timer_add(&dev->release[i],
                  fwio_walk ? fwio_walk_release : fwio_release,
                  (void *) (intptr_t) i, 0);

    io_sethandler(dev->base, FWIO_LEN, fwio_read, NULL, NULL,
                  fwio_write, NULL, NULL, dev);

    fwio_inst = dev;
    funworld_io_probe_init();      /* PEEPEEBOX_IO_PROBE diagnostic, if set */
    fwio_log("FWIO: %s at %04X-%04X\n", info->name, dev->base,
             dev->base + FWIO_LEN - 1);
    return dev;
}

static void
fwio_close(void *priv)
{
    fwio_t *dev = (fwio_t *) priv;

    if (dev == NULL)
        return;

    io_removehandler(dev->base, FWIO_LEN, fwio_read, NULL, NULL,
                     fwio_write, NULL, NULL, dev);
    free(dev);
    fwio_inst = NULL;
}

static const device_config_t fwio_config[] = {
  // clang-format off
    {
        /* The card's own DIP switch, which is the only reason this is a
           setting.  The default is a guess until a disk says otherwise. */
        .name           = "base",
        .description    = "Address",
        .type           = CONFIG_HEX16,
        .default_string = NULL,
        .default_int    = 0x210,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "0x200", .value = 0x200 },
            { .description = "0x210 (what the disks use)", .value = 0x210 },
            { .description = "0x230", .value = 0x230 },
            { .description = "0x240", .value = 0x240 },
            { .description = "0x250", .value = 0x250 },
            { .description = "0x260", .value = 0x260 },
            { .description = "0x270", .value = 0x270 },
            { .description = "0x280", .value = 0x280 },
            { .description = "0x290", .value = 0x290 },
            { .description = "0x2A0", .value = 0x2a0 },
            { .description = "0x2B0", .value = 0x2b0 },
            { .description = "0x2C0", .value = 0x2c0 },
            { .description = "0x2D0", .value = 0x2d0 },
            { .description = "0x2E0", .value = 0x2e0 },
            { .description = "0x2F0", .value = 0x2f0 },
            { .description = ""                      }
        },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
  // clang-format on
};

const device_t funworld_io_device = {
    .name          = "funworld I/O card (8255)",
    .internal_name = "funworld_io",
    .flags         = DEVICE_ISA,
    .local         = 0,
    .init          = fwio_init,
    .close         = fwio_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = fwio_config
};

/* --------------------------------------------------------------- the probe */

/* Where does the card belong?  The DIP switch decided on a real cabinet, and
   the disk was set up to match, so the answer is in the image rather than in
   any document -- and the software will say it out loud if asked.  It sweeps
   the control register of every 4-byte block hunting for the card; this claims
   all of them at once and reports what it is asked and what it answered.
   Whichever block the software goes on to talk to after the sweep -- the one it
   reads base+0..2 from -- is the base address.

   PEEPEEBOX_IO_PROBE is the byte the sweep is answered with, in hex, because
   what the software will accept as "card here" is exactly the unknown.  One run
   per candidate answer, watching where the sweep stops.  This is a diagnostic,
   not part of a cabinet: it exists to be deleted once the base is known. */
static uint8_t fwio_probe_answer = 0x00;

static uint8_t
fwio_probe_read(uint16_t port, UNUSED(void *priv))
{
    pclog("FWIO-PROBE: %04X read -> %02X\n", port, fwio_probe_answer);
    return fwio_probe_answer;
}

static void
fwio_probe_write(uint16_t port, uint8_t val, UNUSED(void *priv))
{
    pclog("FWIO-PROBE: %04X written %02X\n", port, val);
}

void
funworld_io_probe_init(void)
{
    const char *env = getenv("PEEPEEBOX_IO_PROBE");

    if (env == NULL)
        return;

    fwio_probe_answer = (uint8_t) strtoul(env, NULL, 16);
    pclog("FWIO-PROBE: answering every control register %04X..%04X with %02X\n",
          FWIO_PROBE_FIRST + FWIO_CTRL, FWIO_PROBE_LAST + FWIO_CTRL,
          fwio_probe_answer);

    for (uint16_t base = FWIO_PROBE_FIRST; base <= FWIO_PROBE_LAST; base += 4) {
        /* Only the control register.  Claiming whole blocks would sit on the
           sound card and the game port and break the boot before the sweep
           ever runs. */
        io_sethandler((uint16_t) (base + FWIO_CTRL), 1,
                      fwio_probe_read, NULL, NULL,
                      fwio_probe_write, NULL, NULL, NULL);
    }
}

/* For the UI: is the walk on, and which line did the last click hold?  The
   answer belongs on screen -- counting clicks against a comment in a batch file
   is exactly the kind of bookkeeping that produces a wrong answer. */
int
funworld_io_walk_state(char *out, size_t len)
{
    if (fwio_walk <= 0)
        return 0;
    snprintf(out, len, "%s", fwio_walk_last[0] ? fwio_walk_last : "not started");
    return 1;
}
