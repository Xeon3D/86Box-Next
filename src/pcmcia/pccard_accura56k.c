/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Hayes Accura 56K PC Card: a V.90 data/fax modem on a 16-bit PC
 *             Card -- a 16550 that is no COM port (serial_init_detached())
 *             with the modem engine on it (char_modem_attach()), behind the
 *             standard configuration registers of a single-function I/O card:
 *
 *               200h  COR   bit 7 SRESET, bit 6 level IREQ, bits 5:0 the
 *                           configuration index -- 0 is memory-only (no I/O,
 *                           IREQ is READY), any other the I/O interface;
 *               202h  CCSR  bit 1 Intr, the UART's interrupt request (read
 *                           only); bits 5 IOis8, 3 Audio and 2 PwrDwn kept.
 *
 *             The configuration index only says which range the CIS offered
 *             (pccard_cis_accura56k(): COM1-COM4's or anywhere): the socket's
 *             I/O window brings the card only its own eight ports, and the
 *             UART decodes the low three address lines.
 *
 *             Windows 98 installs MDMHAY2.INF's "Hayes Accura 56K PC Card" for
 *             it from the CAB files (PCMCIA\Hayes-Accura_56K_PC_CARD-EBBB),
 *             with serial.vxd on the I/O range and IRQ it chose; Linux binds
 *             serial_cs by FUNCID.  Data and fax: no voice commands.
 *
 *             The settings are the card's own (device instance socket + 1):
 *             the modem's telephone line, as a COM port modem has it.
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
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/char.h>
#include <86box/serial.h>
#include <86box/char_modem.h>
#include <86box/pcmcia.h>
#include <86box/plat_unused.h>

#define COR_SRESET  0x80
#define COR_INDEX   0x3f
#define CCSR_INTR   0x02
#define CCSR_KEPT   0x2c /* IOis8, Audio, PwrDwn */

typedef struct {
    pccard_t  pccard;
    int       socket;
    uint8_t   cis[256];
    int       cis_len;
    uint32_t  cfg_base;
    uint8_t   cor;
    uint8_t   ccsr;
    int       intr;    /* the UART's interrupt request */

    serial_t *uart;
    void     *modem;   /* the modem engine's, on the UART */
} accura_t;

/* What the modem answers.  The real card's ATI answers were not found; these
   are plausible ones -- Windows installs the card by its PC Card ID and reads
   none of them. */
static const char_modem_model_t accura_modem_model = {
    .name       = "Hayes Accura 56K PC Card",
    .ident_at   = 3, .ident = "Hayes Accura 56K PC Card",
    .fmw_at     = 7, .fmw   = "V2.07",
    .country_at = 5,
    .info       = {
        [0] = "56000",
        [1] = "255",
        [2] = "OK",
        [4] = "Hayes Microcomputer Products, Inc.",
        [6] = "V.90 Data/Fax"
    },
    .voice      = 0
};

static int
accura_io_mode(const accura_t *dev)
{
    return (dev->cor & COR_INDEX) && !(dev->cor & COR_SRESET);
}

/* In memory-only mode the IREQ pin is READY, and no interrupt. */
static void
accura_irq_update(accura_t *dev)
{
    pcmcia_card_irq(dev->socket, dev->intr && accura_io_mode(dev));
}

static void
accura_uart_irq(void *priv, int level)
{
    accura_t *dev = (accura_t *) priv;

    dev->intr = level;
    accura_irq_update(dev);
}

static void accura_card_reset(void *priv);

/* Attribute memory: the CIS in the even bytes from 0, the registers at the
   configuration base, decoding A9-A0. */
static uint8_t
accura_attr_read(uint32_t addr, void *priv)
{
    const accura_t *dev = (accura_t *) priv;

    addr &= 0x3ff;
    if (addr == dev->cfg_base)
        return dev->cor;
    if (addr == (dev->cfg_base + 2))
        return dev->ccsr | (dev->intr ? CCSR_INTR : 0);
    if ((addr & 1) || (addr >= dev->cfg_base))
        return 0xff;
    return ((addr >> 1) < (uint32_t) dev->cis_len) ? dev->cis[addr >> 1] : 0xff;
}

static void
accura_attr_write(uint32_t addr, uint8_t val, void *priv)
{
    accura_t *dev = (accura_t *) priv;

    addr &= 0x3ff;
    if (addr == dev->cfg_base) {
        if (val & COR_SRESET)
            accura_card_reset(dev);
        dev->cor = val;
        accura_irq_update(dev);
    } else if (addr == (dev->cfg_base + 2))
        dev->ccsr = val & CCSR_KEPT;
}

static uint8_t
accura_io_read(uint16_t port, void *priv)
{
    const accura_t *dev = (accura_t *) priv;

    return accura_io_mode(dev) ? serial_read(port & 7, dev->uart) : 0xff;
}

static void
accura_io_write(uint16_t port, uint8_t val, void *priv)
{
    const accura_t *dev = (accura_t *) priv;

    if (accura_io_mode(dev))
        serial_write(port & 7, val, dev->uart);
}

/* Power-up, the RESET pin or SRESET: memory-only mode, the UART reset.  The
   modem's call is not the UART's business, and goes on. */
static void
accura_card_reset(void *priv)
{
    accura_t *dev = (accura_t *) priv;

    dev->cor  = 0x00;
    dev->ccsr = 0x00;
    serial_reset_detached(dev->uart);
    accura_irq_update(dev);
}

static void
accura_close(void *priv)
{
    accura_t *dev = (accura_t *) priv;

    pcmcia_insert(dev->socket, NULL);
    char_modem_detach(dev->modem);
    serial_close_detached(dev->uart);
    free(dev);
}

static void *
accura_init(UNUSED(const device_t *info))
{
    accura_t *dev = (accura_t *) calloc(1, sizeof(accura_t));
    char      label[16];

    dev->socket = device_get_instance() - 1;
    if ((dev->socket < 0) || (dev->socket >= PCMCIA_SOCKETS))
        dev->socket = 0;

    dev->cis_len = pccard_cis_accura56k(dev->cis, &dev->cfg_base);

    /* The modem: a 16550 with the modem engine on its port. */
    snprintf(label, sizeof(label), "PC Card %c", 'A' + dev->socket);
    dev->uart  = serial_init_detached(SERIAL_16550, accura_uart_irq, dev);
    dev->modem = char_modem_attach(&dev->uart->char_port, &accura_modem_model, label);
    if (dev->uart->char_port.chardev.control)
        dev->uart->char_port.chardev.control((dev->uart->mctrl & 0x03) | (dev->uart->lcr & 0x40),
                                             dev->uart->char_port.chardev.priv);
    accura_card_reset(dev);

    dev->pccard = (pccard_t) {
        .name       = "Hayes Accura 56K",
        .attr_read  = accura_attr_read,
        .attr_write = accura_attr_write,
        .io_read    = accura_io_read,
        .io_write   = accura_io_write,
        .reset      = accura_card_reset,
        .priv       = dev
    };
    pcmcia_insert(dev->socket, &dev->pccard);
    return dev;
}

/* The modem's settings: char_modem.h's, so a COM port modem and this one are
   set alike. */
static const device_config_t accura_config[] = {
    // clang-format off
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
    // clang-format on
};

const device_t pccard_accura56k_device = {
    .name          = "Hayes Accura 56K PC Card",
    .internal_name = "accura56k",
    .flags         = DEVICE_ISA,
    .local         = 0,
    .init          = accura_init,
    .close         = accura_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = accura_config
};
