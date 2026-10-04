/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             An SRAM memory card: battery-backed static RAM, 64 KB to 4 MB,
 *             in the card's common memory, read and written byte by byte
 *             through the controller's common-memory windows.  Attribute
 *             memory holds the CIS (pccard_cis.c), which describes the RAM in
 *             its DEVICE tuple; there are no configuration registers and no
 *             I/O.  A write-protect switch, as on the real cards: it keeps
 *             writes from the RAM and shows on the card's WP pin, which the
 *             socket reports.
 *
 *             The guests: Windows 9x's Card Services with SRAMMTD.VXD,
 *             DOS card services with an SRAM driver, Linux memory_cs.  The
 *             contents survive power-off -- the battery -- in the machine's
 *             nvr folder, one file per socket.
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

typedef struct {
    pccard_t pccard;
    int      socket;
    uint32_t size;
    int      wp;          /* the write-protect switch */
    int      dirty;
    uint8_t *ram;
    uint8_t  cis[256];
    int      cis_len;
    char     file[64];
} sram_t;

static uint8_t
sram_attr_read(uint32_t addr, void *priv)
{
    const sram_t *dev = (sram_t *) priv;

    if (addr & 1)
        return 0xff;
    return ((addr >> 1) < (uint32_t) dev->cis_len) ? dev->cis[addr >> 1] : 0xff;
}

static uint8_t
sram_common_read(uint32_t addr, void *priv)
{
    const sram_t *dev = (sram_t *) priv;

    return (addr < dev->size) ? dev->ram[addr] : 0xff;
}

static void
sram_common_write(uint32_t addr, uint8_t val, void *priv)
{
    sram_t *dev = (sram_t *) priv;

    if (dev->wp || (addr >= dev->size))
        return;
    dev->ram[addr] = val;
    dev->dirty     = 1;
}

static int
sram_write_protect(void *priv)
{
    return ((sram_t *) priv)->wp;
}

static void
sram_save(sram_t *dev)
{
    FILE *fp;

    if (!dev->dirty)
        return;
    if ((fp = nvr_fopen(dev->file, "wb")) != NULL) {
        fwrite(dev->ram, 1, dev->size, fp);
        fclose(fp);
        dev->dirty = 0;
    }
}

/* Power-off or RESET: a memory card has nothing to reset, but it is a good
   time to keep what was written. */
static void
sram_reset(void *priv)
{
    sram_save((sram_t *) priv);
}

static void *
sram_init(UNUSED(const device_t *info))
{
    sram_t *dev = (sram_t *) calloc(1, sizeof(sram_t));
    FILE   *fp;

    dev->socket = device_get_instance() - 1;
    if ((dev->socket < 0) || (dev->socket >= PCMCIA_SOCKETS))
        dev->socket = 0;
    dev->size = (uint32_t) device_get_config_int("size") << 10;
    dev->wp   = device_get_config_int("write_protect");
    dev->ram  = (uint8_t *) malloc(dev->size);
    memset(dev->ram, 0xff, dev->size);   /* erased */

    snprintf(dev->file, sizeof(dev->file), "pcmcia_sram_%c.bin", 'a' + dev->socket);
    if ((fp = nvr_fopen(dev->file, "rb")) != NULL) {
        (void) !fread(dev->ram, 1, dev->size, fp);
        fclose(fp);
    }

    dev->cis_len = pccard_cis_sram(dev->cis, dev->size);
    dev->pccard  = (pccard_t) {
        .name          = "SRAM memory card",
        .attr_read     = sram_attr_read,
        .common_read   = sram_common_read,
        .common_write  = sram_common_write,
        .reset         = sram_reset,
        .write_protect = sram_write_protect,
        .priv          = dev
    };
    pcmcia_insert(dev->socket, &dev->pccard);
    return dev;
}

static void
sram_close(void *priv)
{
    sram_t *dev = (sram_t *) priv;

    pcmcia_insert(dev->socket, NULL);
    sram_save(dev);
    free(dev->ram);
    free(dev);
}

// clang-format off
static const device_config_t sram_config[] = {
    {
        .name           = "size",
        .description    = "Size",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 1024,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "64 KB",  .value = 64   },
            { .description = "128 KB", .value = 128  },
            { .description = "256 KB", .value = 256  },
            { .description = "512 KB", .value = 512  },
            { .description = "1 MB",   .value = 1024 },
            { .description = "2 MB",   .value = 2048 },
            { .description = "4 MB",   .value = 4096 },
            { .description = ""                      }
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

const device_t pccard_sram_device = {
    .name          = "SRAM memory card",
    .internal_name = "pccard_sram",
    .flags         = DEVICE_ISA,
    .local         = 0,
    .init          = sram_init,
    .close         = sram_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = sram_config
};
