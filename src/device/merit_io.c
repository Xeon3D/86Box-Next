/*
 * 86Box-Next: Merit Megatouch I/O board (CRT-500 "Zeus" / Millennium) with its
 * Dallas DS1991 MultiKey iButton security key.  Ported from MegaPPBox
 * (github.com/Xeon3D/MegaPPBox), where it is part of the cabinet; here it is
 * an optional ISA card, one of the arcade I/O boards (io_board.c).
 *
 * Port map (base 0x228, from the game binaries and MAME merit/mtouchxl.cpp):
 *   0x228-0x229  R: inputs (idle 0xFF)        W: outputs (lamps/meters), latched
 *   0x22A        R: DIP bank, bit 7 = 1 normal (0 runs the CD-cleaning procedure)
 *   0x22B        R: DIP bank
 *   0x22C        R: bit 0 Setup, bit 1 Calibrate (active low), bits 2-5 coins
 *                   (active high), bits 6-7 high -> idle 0xC3.   W: outputs
 *   0x22D        1-Wire key line. Read: bit 5 = line level. Write: the drive bit
 *                pulls the line low when set -- bit 6 on the XL (Zeus) card,
 *                bit 5 on the MAXX variant.
 *   0x22F        W: bank register (bits 0-4) for the 64 KB window at 0xD0000:
 *                banks 0-15 = the 1 MB U12 image (SA3014; bank 0 starts with the
 *                option ROM that hooks INT 19h and boots the ROM-DOS ROM disk from
 *                bank 1 on, which then runs the CD), banks 16-23 = AM29F040 flash
 *                (512 KB, kept in nvr/), banks 24-31 = open bus (MAME mtouchxl.cpp)
 *   0x224-0x227  XL only: the board's CS4231A codec (index, data, status, PIO),
 *                IRQ 5, DMA 1, no WSS configuration register (MAME mtouchxl). The
 *                games' Miles DIG.INI says IO_ADDR 220h: base + 4 is this codec.
 *
 * The DS1991 is a time-based 1-Wire slave working in emulated time (TSC), so
 * the guest's self-calibrated bit-banging sees real slot timing. Its contents
 * come from a keyflasher "full" dump (264 bytes: 3 x [8 ID, 8 password, 48 data],
 * 64 scratchpad, 8 ROM ID stored family-last). Passwords are checked exactly as
 * the part does; a wrong password returns the part's deterministic bad-password
 * pattern (tables from keyflasher's PasswordValidator, loaded from the ROM set).
 * Writes the guest makes to the key are kept in nvr/, never in the dump.
 *
 * The earlier XL generation (XL, Super 5000, 6000, Gold CDs) has a DS1205
 * MultiKey instead: a 3-wire part on the same port (write bit 6 = RST, bit 4 =
 * CLK, bit 5 = DQ; DQ reads back on bit 5), fitted by loading a 192-byte
 * MultiKey dump (3 x [8 ID, 8 security match, 48 data], as MAME's "multikey").
 * Its protocol follows MAME's ds1205.cpp (smf, Carl; BSD-3-Clause).  Those
 * boards also carry 32 KB of battery-backed RAM (a DS1235) at 0xC8000, kept in
 * nvr/ and optionally seeded from a dump (MAME mtouchxl: u12-nvram-ds1235).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/io.h>
#include <86box/timer.h>
#include <86box/dma.h>
#include <86box/pic.h>
#include <86box/sound.h>
#include <86box/snd_ad1848.h>
#include <86box/nvr.h>
#include <86box/mem.h>
#include <86box/rom.h>
#include <86box/plat.h>
#include <86box/plat_unused.h>
#include <86box/merit_io.h>
#include <86box/snd_speaker.h>
#include <86box/path.h>
#include <86box/ini.h>
#include <86box/config.h>
#include "cpu.h"

#ifdef ENABLE_MERIT_IO_LOG
int merit_io_do_log = ENABLE_MERIT_IO_LOG;
#else
int merit_io_do_log = 0;
#endif

static void
mio_log(const char *fmt, ...)
{
    va_list ap;
    char    buf[256];
    if (!merit_io_do_log)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pclog("%s", buf);
}

/* 1-Wire slave timing, microseconds. */
#define OW_RESET_US      350.0 /* low at least this long = reset pulse */
#define OW_PRESENCE_WAIT 30.0  /* release -> presence pulse start */
#define OW_PRESENCE_LEN  120.0
#define OW_BIT_THRESH_US 30.0  /* master low shorter than this = write 1 */
#define OW_READ0_HOLD_US 45.0  /* slave holds line low for a 0 bit */

enum {
    OW_IDLE = 0, /* not addressed; wait for reset */
    OW_ROM,      /* expecting ROM command */
    OW_MATCH,    /* receiving 8 ROM bytes for Match ROM */
    OW_SEARCH,   /* Search ROM triplets */
    OW_FUNC,     /* expecting memory function command */
    OW_ADDR,     /* receiving address byte */
    OW_NADDR,    /* receiving complemented address byte */
    OW_TX_ID,    /* sending the subkey ID */
    OW_RX_PW,    /* receiving a password */
    OW_TX_DATA,  /* sending data bytes */
    OW_RX_DATA,  /* receiving data bytes */
    OW_RX_PWD3,  /* Write Password: old ID, new ID, new password (24 bytes) */
    OW_RX_COPY,  /* Copy Scratchpad: block code + password (16 bytes) */
    OW_DEAD      /* ignore everything until reset */
};

typedef struct merit_io_t {
    const device_t *info;

    /* card */
    int      has_codec;
    int      apricot;    /* 1 = wanted, 2 = in place at apricot_at, -1 = no room */
    uint32_t apricot_at;
    ad1848_t codec;
    uint8_t out[4]; /* 0x228, 0x229, 0x22C, 0x22D shadow */
    uint8_t bank;
    uint8_t in_22c;

    /* banked ROM / flash window at 0xD0000 */
    mem_mapping_t win;
    uint8_t      *u12;        /* 1 MB board ROM image, NULL = no window */
    uint8_t      *flash;      /* AM29F040, 512 KB */
    int           flash_cyc;  /* command cycle within an unlock sequence */
    int           flash_mode; /* 0 read, 1 program next byte, 2 erase armed, 3 autoselect */
    int           flash_dirty;
    uint8_t dip_a, dip_b;
    uint8_t drive_mask;

    /* key contents */
    int     key_present;
    uint8_t subkey[3][64];
    uint8_t scratch[64];
    uint8_t rom[8]; /* wire order: family first */
    char    nvr_name[64];
    char    key_file[512]; /* the fitted dump, "" = none */
    int     dirty;

    /* bad-password tables (optional) */
    uint8_t *const_blk; /* 256 x 48 */
    uint8_t *signed_blk; /* 128 x 48 */

    /* line */
    int    master_low;
    int    slot_rx; /* the current slot (since the falling edge) is a master write */

    /* diagnostics: how the master samples the bits we send */
    int    txd_active, txd_bit, txd_seen;
    int    txd_bits, txd_errs, txd_unread;
    double txd_dmin, txd_dmax;
    int    rxl_hist[16]; /* master write-slot low times, 5 us buckets (diagnostics) */
    double fall_us;
    double slave_low_until;
    double presence_start, presence_end;

    /* protocol */
    int     state;
    int     tx_mode; /* 1 while the slave is sending bits */
    uint8_t rx_byte, rx_bits;
    uint8_t tx_byte, tx_bits;
    uint8_t cmd, addr, key;
    uint8_t buf[24];
    int     cnt;
    int     pos;
    int     pw_ok;
    int     search_bit, search_phase;

    /* DS1205 MultiKey (3-wire) */
    int     ds1205; /* the fitted dump is a 192-byte MultiKey */
    int     ds_rst, ds_clk, ds_dqw, ds_dqr, ds_state, ds_bit;
    uint8_t ds_cmd[3], ds_cmp[8];
    uint32_t ds_rand;

    /* battery-backed RAM at 0xC8000 */
    mem_mapping_t nv_map;
    uint8_t      *nv;
    int           nv_dirty;
} merit_io_t;

/* ---- cabinet controls ------------------------------------------------- */

extern const device_t merit_pcic_device;

static merit_io_t      *mio_inst;
static volatile uint32_t line_until[MERIT_LINES]; /* host ms; 0 = released */

/* CMOS shutdown byte (register 0x0F), watched through a write-only handler on
   0x70/0x71 next to the RTC's own.  A CPU reset with it at 0 is a reboot; a
   non-zero code is the BIOS or a DOS extender leaving protected mode. */
static uint8_t cmos_index;
static uint8_t cmos_shutdown;

static void
mio_cmos_write(uint16_t port, uint8_t val, UNUSED(void *priv))
{
    if (port == 0x70)
        cmos_index = val & 0x7f;
    else if (cmos_index == 0x0f)
        cmos_shutdown = val;
}

/* The MAXX board's BIOS (ASUS TX97, Award) hangs in its warm-boot path when a
   Linux release reboots itself (first boot, new hardware), and the cabinet's
   own board did not.  Such a reboot pulls the board's RESET line instead of
   the CPU's alone: chipset, devices and CPU reset, the RTC runs on.  Called
   from softresetx86(). */
int
merit_io_reboot_as_hard_reset(void)
{
    if (!mio_inst || (mio_inst->drive_mask != 0x20) || cmos_shutdown)
        return 0;
    mio_log("[KEY] guest reboot -> board reset\n");
    board_reset_pending = 1; /* at the next frame: not from inside the CPU loop */
    return 1;
}

void
merit_io_pulse(int line, int ms)
{
    if ((line < 0) || (line >= MERIT_LINES) || !mio_inst)
        return;
    if (ms <= 0)
        ms = (line <= MERIT_LINE_COIN4) ? 100 : 500;
    line_until[line] = plat_get_ticks() + (uint32_t) ms;
}

int
merit_io_present(void)
{
    return mio_inst != NULL;
}

static uint8_t
inputs_22c(merit_io_t *dev)
{
    uint32_t now = plat_get_ticks();
    uint8_t  v   = dev->in_22c;

    for (int l = 0; l < MERIT_LINES; l++) {
        uint32_t u = line_until[l];
        if (!u || ((int32_t) (u - now) <= 0))
            continue;
        if (l <= MERIT_LINE_COIN4)
            v |= (uint8_t) (0x04 << l);           /* coins: active high */
        else
            v &= (uint8_t) ~(1 << (l - MERIT_LINE_SETUP)); /* buttons: active low */
    }
    return v;
}

static double
now_us(void)
{
#ifdef USE_DYNAREC
    if (cpu_use_dynarec)
        update_tsc();
#endif
    return (double) tsc / ((double) TIMER_USEC / 4294967296.0);
}

/* ---- key persistence ---------------------------------------------------- */

/* Load a 264-byte dump. With `must_match`, only if its ROM ID is the fitted key's
   (guest writes kept in nvr/ must not be applied to a different key). */
static int
key_load_data(merit_io_t *dev, const uint8_t *d, size_t n, int must_match)
{
    if ((n == 192) && !must_match) {
        /* DS1205 MultiKey: same subkey layout, no scratchpad kept, no ROM ID. */
        for (int k = 0; k < 3; k++)
            memcpy(dev->subkey[k], d + 64 * k, 64);
        memset(dev->scratch, 0, sizeof(dev->scratch));
        memset(dev->rom, 0, sizeof(dev->rom));
        dev->ds1205 = 1;
        return 1;
    }
    if (n != 264)
        return 0;
    dev->ds1205 = 0;
    if (must_match)
        for (int i = 0; i < 8; i++)
            if (dev->rom[i] != d[256 + 7 - i])
                return 0;
    for (int k = 0; k < 3; k++)
        memcpy(dev->subkey[k], d + 64 * k, 64);
    memcpy(dev->scratch, d + 192, 64);
    for (int i = 0; i < 8; i++)
        dev->rom[i] = d[256 + 7 - i];
    return 1;
}

static int
key_load_file(merit_io_t *dev, const char *fn, int must_match)
{
    uint8_t d[264];
    FILE   *f = must_match ? nvr_fopen(fn, "rb") : plat_fopen(fn, "rb");
    if (!f)
        return 0;
    size_t n = fread(d, 1, sizeof(d), f);
    fclose(f);
    return key_load_data(dev, d, n, must_match);
}

/* The user's own dump file. */
static int
key_load_ref(merit_io_t *dev, const char *ref)
{
    return key_load_file(dev, ref, 0);
}

static void
key_save(merit_io_t *dev)
{
    uint8_t d[264];
    if (!dev->key_present || !dev->dirty || dev->ds1205)
        return;
    for (int k = 0; k < 3; k++)
        memcpy(d + 64 * k, dev->subkey[k], 64);
    memcpy(d + 192, dev->scratch, 64);
    for (int i = 0; i < 8; i++)
        d[256 + 7 - i] = dev->rom[i];
    FILE *f = nvr_fopen(dev->nvr_name, "wb");
    if (f) {
        fwrite(d, 1, sizeof(d), f);
        fclose(f);
        dev->dirty = 0;
    }
}

/* Fit the key from dump `fn` (NULL or "" = no key). The guest's writes to a key
   live in nvr/megatouch_ds1991_<ROM ID>.bin, one file per key, and are applied
   over the dump; the dump itself is never modified. The pre-per-key file
   (megatouch_ds1991.bin) is still honoured when its ROM ID matches. */
static void
key_fit(merit_io_t *dev, const char *fn)
{
    key_save(dev);
    dev->key_present = 0;
    dev->dirty       = 0;
    dev->state       = OW_IDLE;
    dev->tx_mode     = 0;
    dev->slave_low_until = dev->presence_start = dev->presence_end = 0.0;
    snprintf(dev->key_file, sizeof(dev->key_file), "%s", fn ? fn : "");
    /* A relative dump path is the rig's (keys\...): resolve it against the VM
       folder, not the process's working directory, which file dialogs move. */
    char full[1024 + 512];
    if (fn && fn[0] && !path_abs((char *) fn)) {
        path_append_filename(full, usr_path, fn);
        fn = full;
    }
    dev->ds1205   = 0;
    dev->ds_state = 0;
    dev->ds_dqr   = -1;
    if (!fn || !fn[0] || !key_load_ref(dev, fn)) {
        if (fn && fn[0])
            pclog("Merit I/O: key dump %s not loadable (need 264 or 192 bytes)\n", fn);
        pclog("Merit I/O: no key fitted\n");
        return;
    }
    dev->key_present = 1;
    if (dev->ds1205) {
        pclog("Merit I/O: DS1205 MultiKey %.8s fitted (%s)\n", (char *) dev->subkey[0], fn);
        return;
    }
    snprintf(dev->nvr_name, sizeof(dev->nvr_name), "megatouch_ds1991_%02X%02X%02X%02X%02X%02X%02X%02X.bin",
             dev->rom[7], dev->rom[6], dev->rom[5], dev->rom[4], dev->rom[3], dev->rom[2], dev->rom[1], dev->rom[0]);
    if (key_load_file(dev, dev->nvr_name, 1) || key_load_file(dev, "megatouch_ds1991.bin", 1))
        pclog("Merit I/O: guest writes to this key restored from nvr/\n");
    pclog("Merit I/O: DS1991 %02X%02X%02X%02X%02X%02X%02X%02X fitted (%s)\n", dev->rom[7], dev->rom[6],
          dev->rom[5], dev->rom[4], dev->rom[3], dev->rom[2], dev->rom[1], dev->rom[0], fn);
}

/* Key swaps from the UI: the request is picked up by the emulator thread at the
   board's next key-line access (like pulling and fitting a real iButton). */
static char         key_req[512];
static volatile int key_req_pending;
static char         key_ui[512]; /* what the UI shows: the requested / fitted dump */

static void
key_apply_request(merit_io_t *dev)
{
    char fn[512];
    if (!key_req_pending)
        return;
    memcpy(fn, key_req, sizeof(fn));
    key_req_pending = 0;
    key_fit(dev, fn);
}

void
merit_io_set_key(const char *fn)
{
    if (!mio_inst)
        return;
    key_req_pending = 0;
    snprintf(key_req, sizeof(key_req), "%s", fn ? fn : "");
    snprintf(key_ui, sizeof(key_ui), "%s", key_req);
    key_req_pending = 1;
    /* The board's key_file setting follows, so the swap survives a restart. */
    config_set_string(mio_inst->info->name, "key_file", key_req);
    config_save();
}

const char *
merit_io_key_file(void)
{
    return mio_inst ? key_ui : NULL;
}

static uint8_t *
load_table(const char *fn, size_t len)
{
    FILE *f = rom_fopen(fn, "rb");
    if (!f)
        return NULL;
    uint8_t *p = malloc(len);
    if (fread(p, 1, len, f) != len) {
        free(p);
        p = NULL;
    }
    fclose(f);
    return p;
}

/* Byte `off` (0..47) of what the part returns for a wrong password. */
static uint8_t
bad_pw_byte(merit_io_t *dev, const uint8_t *pw, int off)
{
    uint8_t p[8];
    int     signed_off = -1;
    uint8_t r          = 0;
    if (!dev->const_blk)
        return 0xff;
    memcpy(p, pw, 8);
    if (p[7] >= 128) {
        signed_off = p[7] - 128;
        p[7]       = 127;
    }
    for (int i = 0; i < 8; i++)
        if (off - i >= 0)
            r ^= dev->const_blk[48 * p[i] + off - i];
    if ((signed_off >= 0) && dev->signed_blk)
        r ^= dev->signed_blk[48 * signed_off + off];
    return r;
}

/* ---- 1-Wire byte layer -------------------------------------------------- */

static void
ow_tx(merit_io_t *dev, uint8_t b)
{
    dev->tx_mode = 1;
    dev->tx_byte = b;
    dev->tx_bits = 0;
}

static void
ow_rx(merit_io_t *dev)
{
    dev->tx_mode = 0;
    dev->rx_byte = 0;
    dev->rx_bits = 0;
}

static void ow_next_tx(merit_io_t *dev);

static void
ow_got_byte(merit_io_t *dev, uint8_t b)
{
    switch (dev->state) {
        case OW_ROM:
            mio_log("[KEY] ROM cmd %02X\n", b);
            if (b == 0x33) { /* Read ROM */
                dev->pos   = 0;
                dev->state = OW_TX_DATA;
                dev->cmd   = 0x33;
                ow_tx(dev, dev->rom[0]);
                return;
            } else if (b == 0x55) {
                dev->cnt   = 0;
                dev->pw_ok = 1;
                dev->state = OW_MATCH;
            } else if (b == 0xcc) {
                dev->state = OW_FUNC;
            } else if (b == 0xf0) {
                dev->state        = OW_SEARCH;
                dev->search_bit   = 0;
                dev->search_phase = 0;
                ow_tx(dev, 0); /* bit-level, handled in ow_slot */
                return;
            } else
                dev->state = OW_DEAD;
            break;

        case OW_MATCH:
            if (b != dev->rom[dev->cnt])
                dev->pw_ok = 0;
            if (++dev->cnt == 8)
                dev->state = dev->pw_ok ? OW_FUNC : OW_DEAD;
            break;

        case OW_FUNC:
            dev->cmd = b;
            mio_log("[KEY] function %02X\n", b);
            if ((b == 0x66) || (b == 0x99) || (b == 0x5a) || (b == 0x96) || (b == 0x69) || (b == 0x3c))
                dev->state = OW_ADDR;
            else
                dev->state = OW_DEAD;
            break;

        case OW_ADDR:
            dev->addr  = b;
            dev->state = OW_NADDR;
            break;

        case OW_NADDR:
            if ((uint8_t) ~b != dev->addr) {
                mio_log("[KEY] address complement mismatch %02X/%02X\n", dev->addr, b);
                dev->state = OW_DEAD;
                break;
            }
            dev->key = dev->addr >> 6;
            dev->pos = dev->addr & 0x3f;
            mio_log("[KEY] cmd %02X key %d addr %02X\n", dev->cmd, dev->key, dev->pos);
            switch (dev->cmd) {
                case 0x66: /* Read SubKey */
                case 0x99: /* Write SubKey */
                case 0x5a: /* Write Password */
                    if (dev->key > 2) {
                        dev->state = OW_DEAD;
                        break;
                    }
                    dev->cnt   = 0;
                    dev->state = OW_TX_ID;
                    ow_tx(dev, dev->subkey[dev->key][0]);
                    return;
                case 0x96: /* Write Scratchpad */
                    dev->state = OW_RX_DATA;
                    break;
                case 0x69: /* Read Scratchpad */
                    dev->state = OW_TX_DATA;
                    ow_next_tx(dev);
                    return;
                case 0x3c: /* Copy Scratchpad */
                    dev->cnt   = 0;
                    dev->state = OW_RX_COPY;
                    break;
            }
            break;

        case OW_RX_PW:
            dev->buf[dev->cnt++] = b;
            if (dev->cnt == 8) {
                dev->pw_ok = !memcmp(dev->buf, dev->subkey[dev->key] + 8, 8);
                mio_log("[KEY] subkey %d password %s (got %02X%02X%02X%02X%02X%02X%02X%02X)\n", dev->key,
                        dev->pw_ok ? "OK" : "WRONG", dev->buf[0], dev->buf[1], dev->buf[2], dev->buf[3],
                        dev->buf[4], dev->buf[5], dev->buf[6], dev->buf[7]);
                if (dev->cmd == 0x66) {
                    if (dev->pos < 0x10)
                        dev->pos = 0x10;
                    dev->state = OW_TX_DATA;
                    ow_next_tx(dev);
                    return;
                }
                dev->state = OW_RX_DATA; /* 0x99 */
                if (dev->pos < 0x10)
                    dev->pos = 0x10;
            }
            break;

        case OW_RX_DATA:
            if (dev->pos > 0x3f)
                break;
            if (dev->cmd == 0x96) {
                dev->scratch[dev->pos++] = b;
                dev->dirty               = 1;
            } else if ((dev->cmd == 0x99) && dev->pw_ok) {
                dev->subkey[dev->key][dev->pos++] = b;
                dev->dirty                        = 1;
            }
            break;

        case OW_RX_PWD3:
            dev->buf[dev->cnt++] = b;
            if (dev->cnt == 24) {
                if (!memcmp(dev->buf, dev->subkey[dev->key], 8)) {
                    memcpy(dev->subkey[dev->key], dev->buf + 8, 16);
                    memset(dev->subkey[dev->key] + 16, 0, 48);
                    dev->dirty = 1;
                    mio_log("[KEY] subkey %d password rewritten\n", dev->key);
                }
                dev->state = OW_DEAD;
            }
            break;

        case OW_RX_COPY:
            dev->buf[dev->cnt++] = b;
            if (dev->cnt == 16) {
                /* Block code selects the scratchpad range; the all-64-bytes code
                   (56 56 7F 51 57 5D ..) is the one software uses. Copy the data
                   area (0x10-0x3F) when the password matches. */
                if (!memcmp(dev->buf + 8, dev->subkey[dev->key] + 8, 8)) {
                    memcpy(dev->subkey[dev->key] + 0x10, dev->scratch + 0x10, 48);
                    dev->dirty = 1;
                    mio_log("[KEY] scratchpad copied to subkey %d\n", dev->key);
                }
                dev->state = OW_DEAD;
            }
            break;

        default:
            break;
    }
    ow_rx(dev);
}

/* Called when a byte has been fully sent; decide what comes next. */
static void
ow_next_tx(merit_io_t *dev)
{
    switch (dev->state) {
        case OW_TX_ID:
            if (++dev->cnt < 8) {
                ow_tx(dev, dev->subkey[dev->key][dev->cnt]);
                return;
            }
            dev->cnt = 0;
            if (dev->cmd == 0x5a)
                dev->state = OW_RX_PWD3;
            else
                dev->state = OW_RX_PW;
            ow_rx(dev);
            return;

        case OW_TX_DATA:
            if (dev->cmd == 0x33) {
                if (++dev->pos < 8) {
                    ow_tx(dev, dev->rom[dev->pos]);
                    return;
                }
                dev->state = OW_FUNC;
                ow_rx(dev);
                return;
            }
            if (dev->pos > 0x3f) {
                ow_tx(dev, 0xff);
                return;
            }
            if (dev->cmd == 0x69)
                ow_tx(dev, dev->scratch[dev->pos]);
            else if (dev->pw_ok)
                ow_tx(dev, dev->subkey[dev->key][dev->pos]);
            else
                ow_tx(dev, bad_pw_byte(dev, dev->buf, dev->pos - 0x10));
            dev->pos++;
            return;

        default:
            ow_rx(dev);
            return;
    }
}

/* One time slot, reported at the master's falling edge (TX) or rising edge (RX). */
static void
txd_close(merit_io_t *dev)
{
    if (!dev->txd_active)
        return;
    dev->txd_bits++;
    if (dev->txd_seen < 0)
        dev->txd_unread++;
    else if (dev->txd_seen != dev->txd_bit)
        dev->txd_errs++;
    dev->txd_active = 0;
}

static void
txd_report(merit_io_t *dev)
{
    txd_close(dev);
    if (dev->txd_bits)
        mio_log("[KEY] session: %d bits sent, %d sampled wrong, %d not sampled, sample delay %.1f-%.1f us\n",
                dev->txd_bits, dev->txd_errs, dev->txd_unread, dev->txd_dmin, dev->txd_dmax);
    {
        char h[16 * 8] = "";
        int  n         = 0;
        for (int i = 0; i < 16; i++) {
            if (dev->rxl_hist[i])
                n += snprintf(h + n, sizeof(h) - n, " %d:%d", i * 5, dev->rxl_hist[i]);
            dev->rxl_hist[i] = 0;
        }
        if (h[0])
            mio_log("[KEY] master write slots, low time (us:count):%s\n", h);
    }
    dev->txd_bits = dev->txd_errs = dev->txd_unread = 0;
    dev->txd_dmin = 1e9;
    dev->txd_dmax = 0;
}

static void
ow_slot_tx(merit_io_t *dev, double t)
{
    int bit;

    if (dev->state == OW_SEARCH) {
        int rb = (dev->rom[dev->search_bit >> 3] >> (dev->search_bit & 7)) & 1;
        if (dev->search_phase == 0)
            bit = rb;
        else
            bit = !rb;
        dev->slave_low_until = bit ? 0 : 1e30;
        if (++dev->search_phase == 2) {
            dev->tx_mode = 0; /* next slot is the master's direction bit */
        }
        return;
    }

    bit = (dev->tx_byte >> dev->tx_bits) & 1;
    /* A 0 is held until the master's next falling edge, not for a fixed time:
       the MAXX master samples late in the slot (its delay loops scale with the
       CPU), and the next slot -- or the reset -- starts with the master pulling
       the line low itself, so holding until then can never block it. */
    dev->slave_low_until = bit ? 0 : 1e30;
    txd_close(dev);
    dev->txd_active = 1;
    dev->txd_bit    = bit;
    dev->txd_seen   = -1;
    if (++dev->tx_bits == 8) {
        if (dev->state == OW_TX_ID || dev->state == OW_TX_DATA)
            ow_next_tx(dev);
        else
            ow_rx(dev);
    }
}

static void
ow_slot_rx(merit_io_t *dev, int bit)
{
    if (dev->state == OW_SEARCH) {
        int rb = (dev->rom[dev->search_bit >> 3] >> (dev->search_bit & 7)) & 1;
        if (bit != rb) {
            dev->state = OW_DEAD;
            return;
        }
        dev->search_phase = 0;
        dev->tx_mode      = 1;
        if (++dev->search_bit == 64) {
            dev->state = OW_FUNC;
            ow_rx(dev);
        }
        return;
    }
    if (dev->state == OW_IDLE || dev->state == OW_DEAD)
        return;
    dev->rx_byte |= bit << dev->rx_bits;
    if (++dev->rx_bits == 8)
        ow_got_byte(dev, dev->rx_byte);
}

static void
ow_line_write(merit_io_t *dev, int drive_low)
{
    double t = now_us();

    if (drive_low && !dev->master_low) {
        dev->master_low = 1;
        dev->fall_us    = t;
        /* The slot type is fixed at its falling edge: a slot that carries the
           slave's last TX bit must not also be counted as an RX bit when the
           master releases the line to end it. */
        dev->slot_rx = !dev->tx_mode;
        dev->slave_low_until = 0; /* any held 0 bit ends when the master starts a new slot */
        if (dev->key_present && dev->tx_mode && (dev->state != OW_IDLE) && (dev->state != OW_DEAD))
            ow_slot_tx(dev, t);
    } else if (!drive_low && dev->master_low) {
        double dur      = t - dev->fall_us;
        dev->master_low = 0;
        if (!dev->key_present)
            return;
        if (dur >= OW_RESET_US) {
            if (merit_io_do_log)
                txd_report(dev);
            key_save(dev);
            dev->presence_start  = t + OW_PRESENCE_WAIT;
            dev->presence_end    = t + OW_PRESENCE_WAIT + OW_PRESENCE_LEN;
            dev->slave_low_until = 0;
            dev->state           = OW_ROM;
            ow_rx(dev);
        } else if (dev->slot_rx) {
            int b = (int) (dur / 5.0);
            dev->rxl_hist[(b > 15) ? 15 : b]++;
            ow_slot_rx(dev, dur < OW_BIT_THRESH_US);
        }
    }
}

static int
ow_line_level(merit_io_t *dev)
{
    double t;
    if (dev->master_low)
        return 0;
    if (!dev->key_present)
        return 1;
    t = now_us();
    if ((t >= dev->presence_start) && (t < dev->presence_end))
        return 0;
    if (t < dev->slave_low_until)
        return 0;
    return 1;
}

/* ---- DS1205 MultiKey (3-wire), after MAME's ds1205.cpp ------------------ */

enum {
    DS_STOP = 0,
    DS_PROTOCOL,
    DS_READ_ID,
    DS_WRITE_ID,
    DS_WRITE_COMPARE,
    DS_WRITE_MATCH,
    DS_READ_DATA,
    DS_READ_SCRATCH,
    DS_GARBLED
};

static void
ds_new_state(merit_io_t *dev, int state)
{
    dev->ds_state = state;
    dev->ds_bit   = 0;
}

/* Master to part: sampled on the rising clock edge. */
static void
ds_writebit(merit_io_t *dev, uint8_t *buf)
{
    if (dev->ds_clk) {
        int mask = 1 << (dev->ds_bit & 7);
        if (dev->ds_dqw)
            buf[dev->ds_bit >> 3] |= mask;
        else
            buf[dev->ds_bit >> 3] &= ~mask;
        dev->ds_bit++;
    }
}

/* Part to master: driven while the clock is low, advanced on the rising edge. */
static void
ds_readbit(merit_io_t *dev, const uint8_t *buf)
{
    if (!dev->ds_clk)
        dev->ds_dqr = (buf[dev->ds_bit >> 3] >> (dev->ds_bit & 7)) & 1;
    else
        dev->ds_bit++;
}

static void
ds_write_rst(merit_io_t *dev, int state)
{
    if (dev->ds_rst == state)
        return;
    dev->ds_rst = state;
    if (state)
        ds_new_state(dev, DS_PROTOCOL);
    else {
        ds_new_state(dev, DS_STOP);
        dev->ds_dqr = -1;
    }
}

static void
ds_write_clk(merit_io_t *dev, int state)
{
    int page;

    if (dev->ds_clk == state)
        return;
    dev->ds_clk = state;
    if (state)
        dev->ds_dqr = -1;
    page = dev->ds_cmd[1] >> 6;
    if (page > 2)
        page = 2;

    switch (dev->ds_state) {
        case DS_PROTOCOL:
            ds_writebit(dev, dev->ds_cmd);
            if (dev->ds_bit == 24) {
                uint8_t c = dev->ds_cmd[0], a = dev->ds_cmd[1];
                int     ok = (a == (uint8_t) ~dev->ds_cmd[2]);
                mio_log("[KEY] DS1205 command %02X %02X %02X\n", c, a, dev->ds_cmd[2]);
                if (ok && (c == 0x69) && ((a & 0xc0) == 0xc0))
                    ds_new_state(dev, DS_READ_SCRATCH);
                else if (ok && (c == 0x66) && ((a & 0xc0) != 0xc0) && ((a & 0x3f) >= 0x10))
                    ds_new_state(dev, DS_READ_ID);
                else if (ok && (c == 0x5a) && ((a & 0xc0) != 0xc0) && !(a & 0x3f))
                    ds_new_state(dev, DS_READ_ID);
                else
                    ds_new_state(dev, DS_STOP);
            }
            break;
        case DS_READ_ID:
            ds_readbit(dev, dev->subkey[page]);
            if (dev->ds_bit == 64)
                ds_new_state(dev, DS_WRITE_COMPARE);
            break;
        case DS_WRITE_COMPARE:
            ds_writebit(dev, dev->ds_cmp);
            if (dev->ds_bit == 64) {
                int match = !memcmp(dev->ds_cmp, dev->subkey[page] + 8, 8);
                mio_log("[KEY] DS1205 subkey %d password %s\n", page, match ? "OK" : "WRONG");
                if (!match)
                    ds_new_state(dev, DS_GARBLED);
                else if (dev->ds_cmd[0] == 0x66)
                    ds_new_state(dev, DS_READ_DATA);
                else if (dev->ds_cmd[0] == 0x69)
                    ds_new_state(dev, DS_READ_SCRATCH);
                else
                    ds_new_state(dev, DS_WRITE_ID);
            }
            break;
        case DS_READ_DATA:
            ds_readbit(dev, dev->subkey[page] + 16);
            if (dev->ds_bit == 384)
                ds_new_state(dev, DS_STOP);
            break;
        case DS_READ_SCRATCH:
            ds_readbit(dev, dev->scratch);
            if (dev->ds_bit == 512)
                ds_new_state(dev, DS_STOP);
            break;
        case DS_WRITE_ID:
            ds_writebit(dev, dev->subkey[page]);
            if (dev->ds_bit == 64)
                ds_new_state(dev, DS_WRITE_MATCH);
            break;
        case DS_WRITE_MATCH:
            ds_writebit(dev, dev->subkey[page] + 8);
            if (dev->ds_bit == 64)
                ds_new_state(dev, DS_STOP);
            break;
        case DS_GARBLED:
            if (!dev->ds_clk && (dev->ds_cmd[0] == 0x66)) {
                dev->ds_rand = dev->ds_rand * 1103515245u + 12345u; /* the part answers noise */
                dev->ds_dqr  = (dev->ds_rand >> 16) & 1;
                dev->ds_bit++;
            } else if (dev->ds_clk && (dev->ds_cmd[0] == 0x99))
                dev->ds_bit++;
            if (dev->ds_bit == 64)
                ds_new_state(dev, DS_STOP);
            break;
        default:
            break;
    }
}

/* ---- battery-backed RAM at 0xC8000 (DS1235, 32 KB) ---------------------- */

#define NV_SIZE 0x8000

static uint8_t
nv_read(uint32_t addr, void *priv)
{
    return ((merit_io_t *) priv)->nv[addr & (NV_SIZE - 1)];
}

static void
nv_write(uint32_t addr, uint8_t val, void *priv)
{
    merit_io_t *dev = (merit_io_t *) priv;
    if (dev->nv[addr & (NV_SIZE - 1)] != val) {
        dev->nv[addr & (NV_SIZE - 1)] = val;
        dev->nv_dirty                 = 1;
    }
}

static void
nv_init(merit_io_t *dev, const char *seed)
{
    FILE *f;

    dev->nv = calloc(1, NV_SIZE);
    f       = nvr_fopen("megatouch_ds1235.bin", "rb");
    if (!f && seed && seed[0])
        f = rom_fopen(seed, "rb");
    if (f) {
        if (fread(dev->nv, 1, NV_SIZE, f) != NV_SIZE)
            pclog("Merit I/O: battery RAM image is short\n");
        fclose(f);
    }
    mem_mapping_add(&dev->nv_map, 0xc8000, NV_SIZE, nv_read, NULL, NULL, nv_write, NULL, NULL,
                    NULL, MEM_MAPPING_EXTERNAL, dev);
    pclog("Merit I/O: 32 KB battery RAM at C8000%s%s\n", seed && seed[0] ? ", seed " : "", seed ? seed : "");
}

static void
nv_save(merit_io_t *dev)
{
    FILE *f;

    if (!dev->nv || !dev->nv_dirty)
        return;
    f = nvr_fopen("megatouch_ds1235.bin", "wb");
    if (f) {
        fwrite(dev->nv, 1, NV_SIZE, f);
        fclose(f);
    }
    dev->nv_dirty = 0;
}

/* ---- banked ROM / flash window at 0xD0000 ------------------------------ */

#define FLASH_SIZE 0x80000

static uint8_t
win_read(uint32_t addr, void *priv)
{
    merit_io_t *dev  = (merit_io_t *) priv;
    uint32_t    off  = addr & 0xffff;
    uint8_t     bank = dev->bank & 0x1f;

    if (bank < 16)
        return dev->u12[(bank << 16) | off];
    if (bank < 24) {
        if (dev->flash_mode == 3) /* autoselect: AMD, Am29F040 */
            return (off & 1) ? 0xa4 : 0x01;
        return dev->flash[((bank - 16) << 16) | off];
    }
    return 0xff;
}

/* AM29F040 command set: unlock AA@5555 55@2AAA, then A0 program, 90 autoselect,
   80 + unlock + 10 (chip) / 30 (64 KB sector) erase, F0 reset. Operations
   complete at once, so data polling reads the final data. */
static void
win_write(uint32_t addr, uint8_t val, void *priv)
{
    merit_io_t *dev  = (merit_io_t *) priv;
    uint8_t     bank = dev->bank & 0x1f;
    uint32_t    fa   = ((uint32_t) (bank - 16) << 16) | (addr & 0xffff);
    uint32_t    ca   = fa & 0x7fff;

    if ((bank < 16) || (bank >= 24))
        return;
    if (dev->flash_mode == 1) {
        dev->flash[fa] &= val; /* programming only clears bits */
        dev->flash_mode  = 0;
        dev->flash_dirty = 1;
        return;
    }
    if (val == 0xf0) {
        dev->flash_mode = dev->flash_cyc = 0;
        return;
    }
    switch (dev->flash_cyc) {
        case 0:
        case 3:
            dev->flash_cyc = ((ca == 0x5555) && (val == 0xaa)) ? dev->flash_cyc + 1 : 0;
            break;
        case 1:
        case 4:
            dev->flash_cyc = ((ca == 0x2aaa) && (val == 0x55)) ? dev->flash_cyc + 1 : 0;
            break;
        case 2:
            dev->flash_cyc = 0;
            if (ca != 0x5555)
                break;
            if (val == 0xa0)
                dev->flash_mode = 1;
            else if (val == 0x90)
                dev->flash_mode = 3;
            else if (val == 0x80)
                dev->flash_cyc = 3;
            break;
        case 5:
            dev->flash_cyc = 0;
            if ((val == 0x10) && (ca == 0x5555))
                memset(dev->flash, 0xff, FLASH_SIZE);
            else if (val == 0x30)
                memset(&dev->flash[fa & ~0xffff], 0xff, 0x10000);
            else
                break;
            dev->flash_mode  = 0;
            dev->flash_dirty = 1;
            mio_log("Merit I/O: flash erase %s\n", (val == 0x10) ? "chip" : "sector");
            break;
    }
}

static void
win_init(merit_io_t *dev, const char *fn)
{
    FILE *f = rom_fopen(fn, "rb");

    if (!f) {
        pclog("Merit I/O: board ROM %s not found; no ROM window\n", fn);
        return;
    }
    dev->u12 = malloc(0x100000);
    memset(dev->u12, 0xff, 0x100000);
    if (fread(dev->u12, 1, 0x100000, f) < 0x800)
        pclog("Merit I/O: board ROM %s is short\n", fn);
    fclose(f);

    dev->flash = malloc(FLASH_SIZE);
    memset(dev->flash, 0xff, FLASH_SIZE);
    f = nvr_fopen("megatouch_flash.bin", "rb");
    if (f) {
        if (fread(dev->flash, 1, FLASH_SIZE, f) != FLASH_SIZE)
            pclog("Merit I/O: megatouch_flash.bin is short\n");
        fclose(f);
    }

    mem_mapping_add(&dev->win, 0xd0000, 0x10000, win_read, NULL, NULL, win_write, NULL, NULL,
                    NULL, MEM_MAPPING_EXTERNAL, dev);
    pclog("Merit I/O: board ROM %s at D0000 (bank 0x22F)\n", fn);
}

static void
win_save(merit_io_t *dev)
{
    FILE *f;

    if (!dev->flash || !dev->flash_dirty)
        return;
    f = nvr_fopen("megatouch_flash.bin", "wb");
    if (f) {
        fwrite(dev->flash, 1, FLASH_SIZE, f);
        fclose(f);
    }
    dev->flash_dirty = 0;
}

/* ---- BIOS identity ------------------------------------------------------ */

/* Which cabinet board Emerald's C:\MTOOLS\CMOS\WBOARD.EXE sees.  It looks for
   an RTL8139 on the PCI bus, then for strings in the BIOS segment (F000:0000
   to FEFF): "4.51G", "4.04", "-NOUSB", "Apricot" (types 1-4) and "REV: MERIT"
   (5, Unicorn).  STARTTCP.BAT runs the TE-16 drivers for types 1-4, and the
   RTL8139 ODI driver for everything else.  The original MAXX's Mitsubishi board
   says "@(#)Apricot BIOS Version 10.83"; other BIOSes say none of them.  So
   when asked (the board's "apricot" option, for a TE-16 network card), the
   Apricot identity is written into a blank stretch of the shadowed F000
   segment -- after POST, at the first access to the board, and again should a
   reset have copied the BIOS back over it.  From MegaPPBox's megatouch.c. */
static const char mio_apricot_id[] = "@(#)Apricot BIOS";

static void
merit_board_ident(merit_io_t *dev)
{
    const uint32_t len  = sizeof(mio_apricot_id) - 1;
    uint32_t       run  = 0;
    uint32_t       best = 0;
    uint8_t        prev = 0x55;

    if (dev->apricot <= 0)
        return;
    if ((dev->apricot == 2) && !memcmp(&ram[dev->apricot_at], mio_apricot_id, len))
        return;

    /* The middle of the longest stretch of padding (0x00 or 0xFF, at least
       256 bytes). */
    dev->apricot_at = 0;
    for (uint32_t a = 0xf0000; a < 0xfff00; a++) {
        const uint8_t b = mem_readb_phys(a);

        run  = ((b == prev) && ((b == 0x00) || (b == 0xff))) ? (run + 1) : 1;
        prev = b;
        if ((run >= 256) && (run > best)) {
            best            = run;
            dev->apricot_at = (a - (run / 2)) & ~0x0f;
        }
    }
    if (dev->apricot_at) {
        memcpy(&ram[dev->apricot_at], mio_apricot_id, len);
        for (uint32_t i = 0; i < len; i++)
            if (mem_readb_phys(dev->apricot_at + i) != (uint8_t) mio_apricot_id[i])
                dev->apricot_at = 0;
    }
    if (dev->apricot_at) {
        dev->apricot = 2;
        pclog("Merit I/O: BIOS segment says \"%s\" at %05X\n", mio_apricot_id, dev->apricot_at);
    } else {
        dev->apricot = -1;
        pclog("Merit I/O: no room in the BIOS segment for the Apricot identity\n");
    }
}

/* ---- I/O ---------------------------------------------------------------- */

static uint8_t
mio_read(uint16_t port, void *priv)
{
    merit_io_t *dev = (merit_io_t *) priv;
    uint8_t     ret = 0xff;

    merit_board_ident(dev);

    /* An 8-bit ISA cycle, about 1 us. The Linux MAXX games time the key's
       1-Wire slots by counting reads of 0x22D (a reset is 600 of them), so
       without the bus cost the reset is too short for the part to see. */
    cycles -= ISA_CYCLES(8);

    switch (port) {
        case 0x22a:
            ret = dev->dip_a;
            break;
        case 0x22b:
            ret = dev->dip_b;
            break;
        case 0x22c:
            ret = inputs_22c(dev);
            break;
        case 0x22d:
            key_apply_request(dev);
            if (dev->key_present && dev->ds1205) {
                ret = (dev->ds_dqr > 0) ? 0xff : 0xdf;
                break;
            }
            ret = ow_line_level(dev) ? 0xff : 0xdf;
            if (dev->txd_active && !dev->master_low && (dev->txd_seen < 0)) {
                double d = now_us() - dev->fall_us;
                dev->txd_seen = (ret & 0x20) ? 1 : 0; /* first read after release = the sample */
                if (d < dev->txd_dmin) dev->txd_dmin = d;
                if (d > dev->txd_dmax) dev->txd_dmax = d;
            }
            break;
        default:
            break;
    }
    return ret;
}

static void
mio_write(uint16_t port, uint8_t val, void *priv)
{
    merit_io_t *dev = (merit_io_t *) priv;

    cycles -= ISA_CYCLES(8);
    merit_board_ident(dev);

    switch (port) {
        case 0x228:
            dev->out[0] = val;
            break;
        case 0x229:
            dev->out[1] = val;
            break;
        case 0x22c:
            dev->out[2] = val;
            break;
        case 0x22d:
            key_apply_request(dev);
            dev->out[3] = val;
            if (dev->key_present && dev->ds1205) {
                ds_write_rst(dev, !!(val & 0x40));
                ds_write_clk(dev, !!(val & 0x10));
                dev->ds_dqw = !!(val & 0x20);
                break;
            }
            ow_line_write(dev, !!(val & dev->drive_mask));
            break;
        case 0x22f:
            dev->bank = val;
            break;
        default:
            break;
    }
}

static void
mio_get_buffer(int32_t *buffer, uint16_t len, void *priv)
{
    merit_io_t *dev = (merit_io_t *) priv;

    ad1848_update(&dev->codec);
    for (int c = 0; c < len * 2; c++)
        buffer[c] += dev->codec.buffer[c] / 2;
    dev->codec.pos = 0;
}

static void
mio_speed_changed(void *priv)
{
    merit_io_t *dev = (merit_io_t *) priv;

    if (dev->has_codec)
        ad1848_speed_changed(&dev->codec);
}

/* Power-on / hard reset: bank 0, so the BIOS option-ROM scan finds U12's 55 AA. */
static void
mio_reset(void *priv)
{
    merit_io_t *dev = (merit_io_t *) priv;

    dev->bank       = 0;
    dev->flash_mode = dev->flash_cyc = 0;
    win_save(dev);
    nv_save(dev);
}

static void *
merit_io_init(const device_t *info)
{
    merit_io_t *dev = calloc(1, sizeof(merit_io_t));
    const char *fn;

    /* .local: 0 = the XL (Zeus) card, 1 = the MAXX (Millennium) card. */
    dev->info       = info;
    dev->drive_mask = info->local ? 0x20 : 0x40;
    dev->apricot    = device_get_config_int("apricot") ? 1 : 0;
    dev->dip_a      = device_get_config_hex16("dip_a");
    dev->dip_b      = device_get_config_hex16("dip_b");
    dev->in_22c     = 0xc3;
    dev->state      = OW_IDLE;
    dev->txd_dmin   = 1e9;

    fn = device_get_config_string("key_file");
    key_req_pending = 0;
    key_fit(dev, fn);
    snprintf(key_ui, sizeof(key_ui), "%s", dev->key_file);

    dev->const_blk  = load_table("roms/megatouch/ds1991_constant.bin", 256 * 48);
    dev->signed_blk = load_table("roms/megatouch/ds1991_signed.bin", 128 * 48);
    if (!dev->const_blk)
        pclog("Merit I/O: bad-password tables missing; wrong passwords will read 0xFF\n");

    io_sethandler(0x0228, 0x0008, mio_read, NULL, NULL, mio_write, NULL, NULL, dev);
    cmos_index = cmos_shutdown = 0;
    io_sethandler(0x0070, 0x0002, NULL, NULL, NULL, mio_cmos_write, NULL, NULL, dev);

    fn = device_get_config_string("board_rom");
    if (fn && fn[0])
        win_init(dev, fn);

    if (device_get_config_int("battery_ram"))
        nv_init(dev, device_get_config_string("battery_ram_seed"));

    /* The MicroTouch DOS driver clicks the PC speaker on every touch; the
       cabinets had no speaker wired to it, so by default it is silent. */
    if (!device_get_config_int("pc_speaker"))
        speaker_mute = 1;
    memset((void *) line_until, 0, sizeof(line_until));
    mio_inst = dev;

    /* The XL (Zeus) board carries the CS4231A codec. */
    if (dev->drive_mask == 0x40) {
        dev->has_codec = 1;
        ad1848_init(&dev->codec, AD1848_TYPE_CS4231);
        ad1848_setirq(&dev->codec, 5);
        ad1848_setdma(&dev->codec, 1);
        io_sethandler(0x0224, 0x0004, ad1848_read, NULL, NULL, ad1848_write, NULL, NULL, &dev->codec);
        sound_add_handler(mio_get_buffer, dev);
        ad1848_speed_changed(&dev->codec);
    }

    /* The MAXX cards carry the PC Card slots (merit_pcic.c). */
    if (dev->drive_mask == 0x20)
        device_add(&merit_pcic_device);
    return dev;
}

static void
merit_io_close(void *priv)
{
    merit_io_t *dev = (merit_io_t *) priv;
    key_save(dev);
    win_save(dev);
    nv_save(dev);
    mio_inst = NULL;
    free(dev->nv);
    free(dev->u12);
    free(dev->flash);
    free(dev->const_blk);
    free(dev->signed_blk);
    free(dev);
}

static const device_config_t merit_io_config[] = {
  // clang-format off
    {
        .name           = "apricot",
        .description    = "Report the Apricot BIOS identity (MAXX with TE-16 network card)",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "pc_speaker",
        .description    = "PC speaker audible (touch clicks)",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "key_file",
        .description    = "Security key dump (DS1991 264 bytes, DS1205 192 bytes)",
        .type           = CONFIG_FNAME,
        .default_string = "",
        .default_int    = 0,
        .file_filter    = "Key dumps (*.bin *.key)|*.bin;*.key|All files (*)|*",
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "board_rom",
        .description    = "Board ROM (U12, 1 MB; empty = none)",
        .type           = CONFIG_FNAME,
        .default_string = "",
        .default_int    = 0,
        .file_filter    = "ROM images (*.u12 *.bin)|*.u12;*.bin",
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "battery_ram",
        .description    = "32 KB battery RAM at C8000 (early XL boards)",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "battery_ram_seed",
        .description    = "Battery RAM contents when nvr/ has none (empty = zeros)",
        .type           = CONFIG_FNAME,
        .default_string = "",
        .default_int    = 0,
        .file_filter    = "RAM images (*)|*",
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "dip_a",
        .description    = "DIP 0x22A",
        .type           = CONFIG_HEX16,
        .default_string = NULL,
        .default_int    = 0xff,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { .description = "FF", .value = 0xff }, { .description = "" } },
        .bios           = { { 0 } }
    },
    {
        .name           = "dip_b",
        .description    = "DIP 0x22B",
        .type           = CONFIG_HEX16,
        .default_string = NULL,
        .default_int    = 0xff,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { .description = "FF", .value = 0xff }, { .description = "" } },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
  // clang-format on
};

const device_t merit_io_xl_device = {
    .name          = "Merit Megatouch XL I/O Board (CRT-500 Zeus)",
    .internal_name = "merit_io",
    .flags         = DEVICE_ISA,
    .local         = 0,
    .init          = merit_io_init,
    .close         = merit_io_close,
    .reset         = mio_reset,
    .available     = NULL,
    .speed_changed = mio_speed_changed,
    .force_redraw  = NULL,
    .config        = merit_io_config
};

const device_t merit_io_maxx_device = {
    .name          = "Merit Megatouch MAXX I/O Board (Millennium)",
    .internal_name = "merit_io_maxx",
    .flags         = DEVICE_ISA,
    .local         = 1,
    .init          = merit_io_init,
    .close         = merit_io_close,
    .reset         = mio_reset,
    .available     = NULL,
    .speed_changed = mio_speed_changed,
    .force_redraw  = NULL,
    .config        = merit_io_config
};
