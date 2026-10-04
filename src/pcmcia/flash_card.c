/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             A linear flash card: Intel Series 2 (iMC002FLSA to
 *             iMC020FLSA), 2 to 20 MB of 28F008SA flash chips, 1 MB each,
 *             two side by side on the card's 16-bit bus -- the even bytes in
 *             one chip, the odd bytes in the other -- so 2 MB a pair.  The
 *             card has no configuration registers and no I/O: attribute
 *             memory holds the CIS (pccard_cis.c), common memory is the
 *             chips, and a write-protect switch takes the programming
 *             voltage from them and shows on the WP pin -- so a protected
 *             card still answers its identifier and reads as the size it
 *             is, read-only (gating every write instead would leave the
 *             drivers unable to identify it at all).
 *
 *             The chips are what flash file systems talk to: a write is a
 *             command, not data.  Each 28F008SA reads its array, its
 *             identifier (Intel 89h, device A2h, by address bit 0) or its
 *             status register, and takes:
 *
 *               FFh read array    90h read identifier    70h read status
 *               50h clear status  40h/10h program a byte (the next write)
 *               20h + D0h erase the 64 KB block the D0h goes to
 *               B0h erase suspend, D0h resume
 *
 *             Programming only clears bits, erasing sets a block to FFh, and
 *             either needs 12 V on the chip's Vpp pin (Vpp1 for the even
 *             chips, Vpp2 for the odd) -- without it the operation fails
 *             with the status register's Vpp bit set, as on the real part.
 *             Both finish at once, so the status register is always ready.
 *
 *             The guests: M-Systems TrueFFS (Windows 9x, DOS) and Microsoft
 *             FFS2 through their Intel MTDs -- which find the chips by their
 *             identifier, the interleave by which byte of a pair answers it,
 *             and the size by where the identifier stops -- and Linux's
 *             memory_cs/ftl_cs.  The contents stay in the machine's nvr
 *             folder, one file per socket.
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
#include <86box/nvr.h>
#include <86box/pcmcia.h>
#include <86box/plat_unused.h>

#define CHIP_SIZE   (1 << 20) /* 28F008SA: 1 MB, sixteen 64 KB blocks */
#define BLOCK_SIZE  (1 << 16)
#define MAX_CHIPS   20

#define ID_MANUF    0x89 /* Intel    */
#define ID_DEVICE   0xa2 /* 28F008SA */

#define SR_READY    0x80 /* WSMS: the write state machine is ready */
#define SR_ESS      0x40 /* erase suspended                        */
#define SR_ES       0x20 /* erase failed                           */
#define SR_DWS      0x10 /* program failed                         */
#define SR_VPPS     0x08 /* Vpp was low                            */

enum {
    MODE_ARRAY,
    MODE_ID,
    MODE_STATUS,
    MODE_PROGRAM, /* 40h/10h given: the next write is the data  */
    MODE_ERASE    /* 20h given: the next write should be D0h    */
};

typedef struct {
    uint8_t mode;
    uint8_t status;
} chip_t;

typedef struct {
    pccard_t pccard;
    int      socket;
    uint32_t size;
    int      wp; /* the write-protect switch */
    int      dirty;
    uint8_t *mem;  /* the card's common memory, chips interleaved */
    chip_t   chip[MAX_CHIPS];
    uint8_t  cis[256];
    int      cis_len;
    char     file[64];
} flash_t;

#ifdef ENABLE_FLASH_CARD_LOG
int flash_card_do_log = ENABLE_FLASH_CARD_LOG;

static void
flash_log(const char *fmt, ...)
{
    va_list ap;

    if (flash_card_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define flash_log(fmt, ...)
#endif

/* The chip behind a card address, and the address within that chip. */
static chip_t *
flash_chip(flash_t *dev, uint32_t addr, uint32_t *caddr)
{
    *caddr = (addr & (2 * CHIP_SIZE - 1)) >> 1;
    return &dev->chip[((addr / (2 * CHIP_SIZE)) << 1) | (addr & 1)];
}

/* 12 V on the chip's Vpp pin -- Vpp1 for the even-byte chips, Vpp2 for the
   odd -- and the write-protect switch off. */
static int
flash_vpp_ok(const flash_t *dev, uint32_t addr)
{
    return !dev->wp && (pcmcia_socket_vpp(dev->socket, addr & 1) >= 114);
}

static uint8_t
flash_attr_read(uint32_t addr, void *priv)
{
    const flash_t *dev = (flash_t *) priv;

    if (addr & 1)
        return 0xff;
    return ((addr >> 1) < (uint32_t) dev->cis_len) ? dev->cis[addr >> 1] : 0xff;
}

static uint8_t
flash_common_read(uint32_t addr, void *priv)
{
    flash_t      *dev = (flash_t *) priv;
    const chip_t *chip;
    uint32_t      caddr;

    if (addr >= dev->size)
        return 0xff;
    chip = flash_chip(dev, addr, &caddr);
    switch (chip->mode) {
        case MODE_ID:
            return (caddr & 1) ? ID_DEVICE : ID_MANUF;
        case MODE_STATUS:
        case MODE_PROGRAM:
        case MODE_ERASE:
            return chip->status;
        default:
            return dev->mem[addr];
    }
}

/* Erase the 64 KB block of the chip at card address addr: every second
   byte of its pair's 2 MB, from the block's start. */
static void
flash_erase_block(flash_t *dev, uint32_t addr)
{
    const uint32_t pair  = addr & ~(2 * CHIP_SIZE - 1);
    const uint32_t start = ((addr & (2 * CHIP_SIZE - 1)) & ~(2 * BLOCK_SIZE - 1)) | (addr & 1);

    for (uint32_t a = 0; a < 2 * BLOCK_SIZE; a += 2)
        dev->mem[pair + start + a] = 0xff;
    dev->dirty = 1;
}

static void
flash_common_write(uint32_t addr, uint8_t val, void *priv)
{
    flash_t *dev = (flash_t *) priv;
    chip_t  *chip;
    uint32_t caddr;

    if (addr >= dev->size)
        return;
    chip = flash_chip(dev, addr, &caddr);

    switch (chip->mode) {
        case MODE_PROGRAM:
            if (flash_vpp_ok(dev, addr)) {
                dev->mem[addr] &= val;
                dev->dirty = 1;
            } else {
                flash_log("Flash card: program at %06X without Vpp\n", addr);
                chip->status |= SR_VPPS | SR_DWS;
            }
            chip->mode = MODE_STATUS;
            return;

        case MODE_ERASE:
            flash_log("Flash card: erase %02X at %06X%s\n", val, addr, flash_vpp_ok(dev, addr) ? "" : " without Vpp");
            if (val != 0xd0)
                chip->status |= SR_ES | SR_DWS;   /* a bad command sequence */
            else if (flash_vpp_ok(dev, addr))
                flash_erase_block(dev, addr);
            else
                chip->status |= SR_VPPS | SR_ES;
            chip->mode = MODE_STATUS;
            return;

        default:
            break;
    }

    if ((val != 0xff) || (chip->mode != MODE_ARRAY))
        flash_log("Flash card: chip %d command %02X at %06X\n", (int) (chip - dev->chip), val, addr);
    switch (val) {
        case 0xff:
            chip->mode = MODE_ARRAY;
            break;
        case 0x90:
            chip->mode = MODE_ID;
            break;
        case 0x70:
            chip->mode = MODE_STATUS;
            break;
        case 0x50:
            chip->status = SR_READY;
            break;
        case 0x40:
        case 0x10:
            chip->mode = MODE_PROGRAM;
            break;
        case 0x20:
            chip->mode = MODE_ERASE;
            break;
        case 0xb0: /* erase suspend, resume: an erase is over at once */
        case 0xd0:
            chip->mode = MODE_STATUS;
            break;
        default: /* not a 28F008SA command: back to the array */
            chip->mode = MODE_ARRAY;
            break;
    }
}

static int
flash_write_protect(void *priv)
{
    return ((flash_t *) priv)->wp;
}

static void
flash_save(flash_t *dev)
{
    FILE *fp;

    if (!dev->dirty)
        return;
    if ((fp = nvr_fopen(dev->file, "wb")) != NULL) {
        fwrite(dev->mem, 1, dev->size, fp);
        fclose(fp);
        dev->dirty = 0;
    }
}

/* Power-off or RESET: the chips go back to reading their array, and it is a
   good time to keep what was written. */
static void
flash_reset(void *priv)
{
    flash_t *dev = (flash_t *) priv;

    for (int c = 0; c < MAX_CHIPS; c++) {
        dev->chip[c].mode   = MODE_ARRAY;
        dev->chip[c].status = SR_READY;
    }
    flash_save(dev);
}

static void *
flash_init(UNUSED(const device_t *info))
{
    flash_t *dev = (flash_t *) calloc(1, sizeof(flash_t));
    FILE    *fp;

    dev->socket = device_get_instance() - 1;
    if ((dev->socket < 0) || (dev->socket >= PCMCIA_SOCKETS))
        dev->socket = 0;
    dev->size = (uint32_t) device_get_config_int("size") << 20;
    if ((dev->size < 2 * CHIP_SIZE) || (dev->size > MAX_CHIPS * CHIP_SIZE))
        dev->size = 4 * CHIP_SIZE;
    dev->size &= ~(2 * CHIP_SIZE - 1);   /* whole pairs of chips */
    dev->wp  = device_get_config_int("write_protect");
    dev->mem = (uint8_t *) malloc(dev->size);
    memset(dev->mem, 0xff, dev->size);   /* erased */
    for (int c = 0; c < MAX_CHIPS; c++)
        dev->chip[c].status = SR_READY;

    snprintf(dev->file, sizeof(dev->file), "pcmcia_flash_%c.bin", 'a' + dev->socket);
    if ((fp = nvr_fopen(dev->file, "rb")) != NULL) {
        (void) !fread(dev->mem, 1, dev->size, fp);
        fclose(fp);
    }

    dev->cis_len = pccard_cis_flash(dev->cis, dev->size);
    dev->pccard  = (pccard_t) {
        .name          = "Flash memory card",
        .attr_read     = flash_attr_read,
        .common_read   = flash_common_read,
        .common_write  = flash_common_write,
        .reset         = flash_reset,
        .write_protect = flash_write_protect,
        .priv          = dev
    };
    pcmcia_insert(dev->socket, &dev->pccard);
    return dev;
}

static void
flash_close(void *priv)
{
    flash_t *dev = (flash_t *) priv;

    pcmcia_insert(dev->socket, NULL);
    flash_save(dev);
    free(dev->mem);
    free(dev);
}

// clang-format off
static const device_config_t flash_config[] = {
    {
        .name           = "size",
        .description    = "Size",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 4,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "2 MB",  .value = 2  },
            { .description = "4 MB",  .value = 4  },
            { .description = "10 MB", .value = 10 },
            { .description = "20 MB", .value = 20 },
            { .description = ""                   }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "write_protect",
        .description    = "Write protected",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
};
// clang-format on

const device_t pccard_flash_device = {
    .name          = "Flash memory card (Intel Series 2)",
    .internal_name = "pccard_flash",
    .flags         = DEVICE_ISA,
    .local         = 0,
    .init          = flash_init,
    .close         = flash_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = flash_config
};
