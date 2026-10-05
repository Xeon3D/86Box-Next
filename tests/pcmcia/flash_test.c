/*
 * 86Box-Next  The Intel Series 2 flash card (src/pcmcia/flash_card.c)
 *             under what M-Systems TrueFFS 3.2's Intel MTD (TRUEFFS.PDR,
 *             Windows 9x) does to it, step by step as the driver does it:
 *
 *               - flIntelIdentify: Read Identifier at byte 0, 1, 2 ... until
 *                 the second identifier byte shows up, which gives the JEDEC
 *                 ID (89A2h, the 28F008SA) and the interleave (2);
 *               - flIntelSize: chip 0 left in Read Identifier, the next pair
 *                 of chips probed every chipSize until one does not answer
 *                 -- the number of pairs, so the size;
 *               - write: Program Setup and the data a word at a time, status
 *                 polled, Vpp/program/erase errors checked, Read Array, the
 *                 data compared;
 *               - erase: Erase Setup and Confirm on both chips of a 128 KB
 *                 unit, the same checks;
 *
 *             plus the card's own rules: no programming or erasing without
 *             12 V on Vpp, programming only clears bits, the write-protect
 *             switch (the card still identifies, nothing is written), and
 *             other flash drivers' probes (CFI's Query) leaving the array
 *             alone.
 *
 *             No test framework; returns non-zero on failure.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/nvr.h>
#include <86box/pcmcia.h>
#include <86box/plat_unused.h>

static int checks, failures;
#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            failures++;                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

extern const device_t pccard_flash_device;

/* ---- the platform ---------------------------------------------------- */

static int             cfg_size, cfg_wp;
static const pccard_t *card;
static int             vpp[2];   /* the socket's Vpp1 and Vpp2, tenths of a volt */

int
device_get_config_int(const char *name)
{
    return !strcmp(name, "size") ? cfg_size : cfg_wp;
}

int
device_get_instance(void)
{
    return 1;
}

FILE *
nvr_fopen(UNUSED(char *str), UNUSED(char *mode))
{
    return NULL;   /* a new, erased card */
}

void
pcmcia_insert(UNUSED(int socket), const pccard_t *c)
{
    card = c;
}

int
pcmcia_socket_vpp(UNUSED(int socket), int pin)
{
    return vpp[pin ? 1 : 0];
}

static uint8_t
rd(uint32_t a)
{
    return card->common_read(a, card->priv);
}

static void
wr(uint32_t a, uint8_t v)
{
    card->common_write(a, v, card->priv);
}

static uint16_t
rdw(uint32_t a)
{
    return rd(a) | (rd(a + 1) << 8);
}

static void
wrw(uint32_t a, uint16_t v)
{
    wr(a, v & 0xff);
    wr(a + 1, v >> 8);
}

/* ---- TrueFFS's Intel MTD (TRUEFFS.PDR obj1 0x4806, 0x499a, 0x73e6,
        0x75cf) ---------------------------------------------------------- */

typedef struct {
    uint32_t type;
    int      interleave;
    uint32_t chip_size; /* of an interleaved group */
    int      chips;     /* groups */
} mtd_t;

/* flIntelIdentify (0x4806), the Intel case: reset with FFh. */
static void
intel_identify(mtd_t *m)
{
    uint8_t vendor = 0, first = 0;
    int     inlv;

    m->type = 0;
    for (inlv = 0; inlv < 0x3f; inlv++) {
        wr(inlv, 0xff);
        wr(inlv, 0xff);
        if (inlv == 0)
            first = rd(0);
        wr(inlv, 0x90);
        if (inlv == 0)
            vendor = rd(0);
        else if ((rd(inlv) != vendor) || (rd(0) != first)) {
            m->type = (vendor << 8) | rd(inlv);
            wr(inlv, 0xff);
            break;
        }
        wr(inlv, 0xff);
    }
    if (inlv & (inlv - 1))
        m->type = 0;
    else
        m->interleave = inlv;
}

/* flIntelSize (0x499a): returns 0 (flOK) or 8 (unknown media). */
static int
intel_size(mtd_t *m)
{
    wr(0, 0x90);   /* chip 0 stays in Read Identifier: a wraparound shows */
    for (m->chips = 0; m->chips < 0x1000; m->chips++) {
        const uint32_t base = m->chips * m->chip_size;
        if ((m->chips > 0) && ((uint32_t) ((rd(base) << 8) | rd(base + m->interleave)) == m->type))
            break;
        int i;
        for (i = m->chips ? 0 : 1; i < m->interleave; i++) {
            wr(base + i, 0x90);
            if ((uint32_t) ((rd(base + i) << 8) | rd(base + i + m->interleave)) != m->type)
                break;
            wr(base + i, 0xff);
        }
        if (i < m->interleave)
            break;
    }
    wr(0, 0xff);
    return m->chips ? 0 : 8;
}

/* The identification as the Intel MTD's constructor (0x7120) finishes it:
   the 28F008SA is 1 MB a chip, the erase unit 64 KB a chip, Clear Status to
   every chip of the first group. */
static int
intel_mount(mtd_t *m)
{
    intel_identify(m);
    if (m->type != 0x89a2)
        return 8;
    m->chip_size = (1 << 20) * m->interleave;
    if (intel_size(m))
        return 8;
    for (int i = 0; i < m->interleave; i++)
        wr(i, 0x50);
    return 0;
}

/* The write (0x73e6) for interleave 2: words, Program Setup to both chips,
   status polled; then each chip's status checked for 38h, Read Array, and
   the data compared.  0 or 11 (write fault). */
static int
intel_write(const mtd_t *m, uint32_t addr, const uint8_t *buf, int len)
{
    int ret = 0;

    for (int i = 0; i < len; i += 2) {
        int tries = 0xffff;
        wrw(addr + i, 0x4040);
        wrw(addr + i, buf[i] | (buf[i + 1] << 8));
        while (((rdw(addr + i) & 0x8080) != 0x8080) && --tries)
            ;
    }
    for (int i = 0; i < m->interleave; i++) {
        if (rd(addr + i) & 0x38) {
            ret = 11;
            wr(addr + i, 0x50);
        }
        wr(addr + i, 0xff);
    }
    for (int i = 0; i < len; i++)
        if (rd(addr + i) != buf[i])
            ret = 11;
    return ret;
}

/* The erase (0x75cf) of one unit: Erase Setup and Confirm to each chip,
   Read Status polled, 38h checked, Clear Status, Read Array. */
static int
intel_erase(const mtd_t *m, int unit)
{
    const uint32_t base = unit * (65536u * m->interleave);
    int            ret  = 0;

    for (int i = 0; i < m->interleave; i++) {
        wr(base + i, 0x20);
        wr(base + i, 0xd0);
    }
    for (int i = 0; i < m->interleave; i++) {
        wr(base + i, 0x70);
        while (!(rd(base + i) & 0x80))
            ;
    }
    for (int i = 0; i < m->interleave; i++) {
        wr(base + i, 0x70);
        if (rd(base + i) & 0x38)
            ret = 11;
        wr(base + i, 0x50);
        wr(base + i, 0xff);
    }
    return ret;
}

/* ---- the tests ------------------------------------------------------- */

static void *
insert(int mb, int wp)
{
    cfg_size = mb;
    cfg_wp   = wp;
    card     = NULL;
    return pccard_flash_device.init(&pccard_flash_device);
}

static void
test_identify(void)
{
    static const int sizes[] = { 2, 4, 10, 20 };

    for (int s = 0; s < 4; s++) {
        void *dev = insert(sizes[s], 0);
        mtd_t m   = { 0 };

        vpp[0] = vpp[1] = 50;
        CHECK(intel_mount(&m) == 0, "%d MB: the Intel MTD takes the card", sizes[s]);
        CHECK(m.type == 0x89a2, "%d MB: JEDEC ID 89A2h, the 28F008SA (%04X)", sizes[s], m.type);
        CHECK(m.interleave == 2, "%d MB: two chips side by side (interleave %d)", sizes[s], m.interleave);
        CHECK(m.chips * m.chip_size == (uint32_t) sizes[s] << 20, "%d MB: the size found is %u bytes (%d x %u)", sizes[s],
              m.chips * m.chip_size, m.chips, m.chip_size);
        CHECK(rd(0) == 0xff && rd(1) == 0xff && rd(((uint32_t) sizes[s] << 20) - 1) == 0xff,
              "%d MB: back to reading the (erased) array", sizes[s]);
        CHECK(rd((uint32_t) sizes[s] << 20) == 0xff, "%d MB: nothing past the end", sizes[s]);
        CHECK(card->write_protect && !card->write_protect(card->priv), "%d MB: the WP pin says writable", sizes[s]);
        pccard_flash_device.close(dev);
    }
}

static void
test_program_erase(void)
{
    void   *dev = insert(4, 0);
    mtd_t   m   = { 0 };
    uint8_t data[512], back[512];

    intel_mount(&m);
    for (int i = 0; i < 512; i++)
        data[i] = (uint8_t) (i * 7 + 3);

    /* Without 12 V the chips refuse, and say why. */
    vpp[0] = vpp[1] = 50;
    CHECK(intel_write(&m, 0x20000, data, 16) == 11, "no Vpp: the write fails");
    wr(0x20000, 0x70);
    wr(0x20001, 0x70);
    CHECK(rd(0x20000) == 0x80 && rd(0x20001) == 0x80, "no Vpp: Clear Status left the chips ready, %02X %02X", rd(0x20000),
          rd(0x20001));
    wr(0x20000, 0xff);
    wr(0x20001, 0xff);
    CHECK(rd(0x20000) == 0xff && rd(0x20003) == 0xff, "no Vpp: nothing programmed");
    wr(0x20000, 0x40);
    wr(0x20000, 0x00);
    CHECK(rd(0x20000) == 0x98, "no Vpp: status 98h, ready, Vpp low, program failed (%02X)", rd(0x20000));
    wr(0x20000, 0x50);
    wr(0x20000, 0xff);

    vpp[0] = vpp[1] = 120;
    CHECK(intel_write(&m, 0x20000, data, 512) == 0, "12 V: 512 bytes written and read back");
    for (int i = 0; i < 512; i++)
        back[i] = rd(0x20000 + i);
    CHECK(!memcmp(back, data, 512), "12 V: the array holds them");

    /* Programming clears bits only: FFh over data changes nothing. */
    memset(back, 0xff, 16);
    intel_write(&m, 0x20000, back, 16);
    CHECK(!memcmp((uint8_t[]) { rd(0x20000), rd(0x20001), rd(0x20002) }, data, 3), "programming FFh leaves the data");

    /* Only one Vpp at 12 V: only that chip's bytes are written. */
    vpp[0] = 120;
    vpp[1] = 50;
    CHECK(intel_write(&m, 0x60000, data, 4) == 11, "12 V on Vpp1 only: the odd chip fails");
    CHECK(rd(0x60000) == data[0] && rd(0x60001) == 0xff, "12 V on Vpp1 only: even bytes written, odd ones not");
    wr(0x60001, 0x50);
    wr(0x60001, 0xff);
    vpp[1] = 120;

    /* Erase unit 1 (128 KB from 20000h): erased; units 0 and 3 untouched. */
    intel_write(&m, 0x00000, data, 4);
    intel_write(&m, 0x5fffc, data, 4);
    CHECK(intel_erase(&m, 1) == 0, "erase of unit 1 succeeds");
    CHECK(rd(0x20000) == 0xff && rd(0x20001) == 0xff && rd(0x3ffff) == 0xff, "unit 1 erased");
    CHECK(rd(0x00000) == data[0] && rd(0x5fffc) == data[0] && rd(0x5ffff) == data[3] && rd(0x60000) == data[0],
          "the other units kept their data");

    /* The second pair of chips (2 MB up) is chips of its own. */
    CHECK(intel_write(&m, 0x200000, data, 8) == 0, "a write to the second pair");
    CHECK(intel_erase(&m, 0) == 0 && rd(0x200000) == data[0] && rd(0x200007) == data[7],
          "erasing the first pair's unit 0 leaves the second pair's");

    /* Erase Setup and anything but Confirm: a command sequence error. */
    wr(0x40000, 0x20);
    wr(0x40000, 0xff);
    CHECK((rd(0x40000) & 0x30) == 0x30, "Erase Setup without Confirm: status 30h set (%02X)", rd(0x40000));
    wr(0x40000, 0x50);
    wr(0x40000, 0xff);

    /* A CFI Query (TrueFFS's CFI MTD probes before the Intel one) is not a
       28F008SA command: the array reads on, nothing changes. */
    wr(0x200000 + 0xaa, 0x98);
    CHECK(rd(0x200000) == data[0] && rd(0x200000 + 0x20) != 'Q', "CFI Query: no QRY, the array reads on");

    /* Power-off resets the chips to Read Array. */
    wr(0x200000, 0x90);
    card->reset(card->priv);
    CHECK(rd(0x200000) == data[0], "after a reset the array reads again");
    pccard_flash_device.close(dev);
}

static void
test_write_protect(void)
{
    void *dev = insert(2, 1);
    mtd_t m   = { 0 };

    vpp[0] = vpp[1] = 120;
    uint8_t data[4] = { 0x12, 0x34, 0x56, 0x78 };

    vpp[0] = vpp[1] = 120;
    CHECK(card->write_protect && card->write_protect(card->priv), "switch on: the WP pin says protected");
    CHECK(intel_mount(&m) == 0 && m.chips * m.chip_size == 2 << 20, "switch on: identified, the right size");
    CHECK(intel_write(&m, 0x100, data, 4) == 11, "switch on: the write fails");
    wr(0x100, 0xff);
    CHECK(rd(0x100) == 0xff && rd(0x103) == 0xff, "switch on: nothing written");
    CHECK(intel_erase(&m, 0) == 11, "switch on: the erase fails");
    pccard_flash_device.close(dev);
}

int
main(void)
{
    test_identify();
    test_program_erase();
    test_write_protect();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
