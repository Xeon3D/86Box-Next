/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Internal modem cards, one per machine (Settings > Other
 *             peripherals), and the one there is:
 *
 *             Diamond SupraExpress 56i Sp: the internal sister of the COM
 *             port SupraExpress 56e PRO -- the same Rockwell RCV56ACF/SP
 *             controller, so the same modem engine (char_modem_attach()) on a
 *             16550 of its own (serial_init_detached()), on a 16-bit ISA card.
 *             Plug and Play as ISA PnP SUP2171, the ID Diamond's SUPIV90.INF
 *             and MDMISUPV.INF install as "SupraExpress 56i Sp Intl" (Linux's
 *             8250_pnp knows it too): eight ports at 3E8h, 2E8h, 3F8h, 2F8h or
 *             anywhere from 100h to 3F8h, IRQ 3, 4, 5, 7, 9, 10, 11, 12 or 15,
 *             and nothing until the BIOS or the OS configures it.  Or, with
 *             Plug and Play off (the jumper the real card has), at a COM
 *             port's address and the IRQ it is set to from power-on, for DOS,
 *             Windows 3.x, OS/2 and NT 4.  Voice and speakerphone, as the
 *             56e's (Rockwell's #CLS=8 set).
 *
 *             Diamond's "56i PRO" (SUP2380/2390/2560) is not this card: it is
 *             a PCI Rockwell HCF soft modem.
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
#include <86box/io.h>
#include <86box/pic.h>
#include <86box/timer.h>
#include <86box/char.h>
#include <86box/serial.h>
#include <86box/isapnp.h>
#include <86box/char_modem.h>
#include <86box/modem_card.h>
#include <86box/plat_unused.h>

int modem_card_type = MODEM_CARD_NONE;

typedef struct {
    serial_t *uart;
    void     *modem; /* the modem engine's, on the UART */
    void     *pnp;   /* the ISA PnP card; NULL with Plug and Play off */

    uint16_t base;   /* 0: not decoded */
    uint8_t  irq;    /* 0: none        */
    int      intr;   /* the UART's interrupt request */
    uint8_t  irq_state;
} supra56i_t;

/* The modem: the 56e PRO's answers, as the internal part's. */
static const char_modem_model_t supra56i_model = {
    .name       = "Diamond SupraExpress 56i Sp",
    .ident_at   = 3, .ident = "SupraExpress 56i Sp",
    .fmw_at     = 7, .fmw   = "V1.100-V90_1M_DLS",
    .country_at = 5,
    .info       = {
        [0] = "1794",
        [1] = "168",
        [2] = "OK",
        [4] = "Diamond Multimedia SupraExpress 56i Sp",
        [6] = "RCVDL56ACF/SP Rev 1.100"
    },
    .voice      = 1
};

static uint8_t supra56i_pnp_rom[] = {
    /* SUP2171, serial 0, dummy checksum (filled in by isapnp_add_card) */
    0x4e, 0xb0, 0x21, 0x71, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* PnP version 1.0, vendor version 1.0 */
    0x0a, 0x10, 0x10,
    /* ANSI identifier */
    0x82, 0x13, 0x00, 'S', 'u', 'p', 'r', 'a', 'E', 'x', 'p', 'r', 'e', 's', 's',
    ' ', '5', '6', 'i', ' ', 'S', 'p',

    /* Logical device SUP2171 */
    0x15, 0x4e, 0xb0, 0x21, 0x71, 0x00,
    /* IRQ 3, 4, 5, 7, 9, 10, 11, 12, 15 */
    0x22, 0xb8, 0x9e,
    /* Start dependent functions, preferred: COM3's ports */
    0x31, 0x00,
    0x47, 0x01, 0xe8, 0x03, 0xe8, 0x03, 0x08, 0x08,
    /* acceptable: COM4's, COM1's, COM2's */
    0x30,
    0x47, 0x01, 0xe8, 0x02, 0xe8, 0x02, 0x08, 0x08,
    0x30,
    0x47, 0x01, 0xf8, 0x03, 0xf8, 0x03, 0x08, 0x08,
    0x30,
    0x47, 0x01, 0xf8, 0x02, 0xf8, 0x02, 0x08, 0x08,
    /* sub-optimal: anywhere from 100h to 3F8h */
    0x31, 0x02,
    0x47, 0x01, 0x00, 0x01, 0xf8, 0x03, 0x08, 0x08,
    /* End dependent functions */
    0x38,

    /* End tag, dummy checksum (filled in by isapnp_add_card) */
    0x79, 0x00
};

static uint8_t
supra56i_read(uint16_t port, void *priv)
{
    const supra56i_t *dev = (supra56i_t *) priv;

    return serial_read(port & 7, dev->uart);
}

static void
supra56i_write(uint16_t port, uint8_t val, void *priv)
{
    const supra56i_t *dev = (supra56i_t *) priv;

    serial_write(port & 7, val, dev->uart);
}

/* The UART's interrupt on the card's IRQ line, as serial_do_irq() puts a COM
   port's on the PIC. */
static void
supra56i_irq_update(supra56i_t *dev)
{
    const int set = dev->intr && dev->base && dev->irq;

    if (dev->irq && (set || dev->irq_state))
        picint_common(1 << dev->irq, PIC_IRQ_LEVEL, set, &dev->irq_state);
}

static void
supra56i_uart_irq(void *priv, int level)
{
    supra56i_t *dev = (supra56i_t *) priv;

    dev->intr = level;
    supra56i_irq_update(dev);
}

/* Where the card decodes: base 0 is nowhere. */
static void
supra56i_map(supra56i_t *dev, uint16_t base, uint8_t irq)
{
    if (dev->irq && dev->irq_state)
        picint_common(1 << dev->irq, PIC_IRQ_LEVEL, 0, &dev->irq_state);
    if (dev->base)
        io_removehandler(dev->base, 8, supra56i_read, NULL, NULL, supra56i_write, NULL, NULL, dev);

    dev->base = base;
    dev->irq  = (irq < 16) ? irq : 0;
    if (dev->base)
        io_sethandler(dev->base, 8, supra56i_read, NULL, NULL, supra56i_write, NULL, NULL, dev);
    supra56i_irq_update(dev);
}

static void
supra56i_pnp_config_changed(uint8_t ld, isapnp_device_config_t *config, void *priv)
{
    supra56i_t *dev = (supra56i_t *) priv;

    if (ld != 0)
        return;
    if (config->activate && (config->io[0].base != ISAPNP_IO_DISABLED))
        supra56i_map(dev, config->io[0].base, config->irq[0].irq);
    else
        supra56i_map(dev, 0, 0);
}

static void
supra56i_close(void *priv)
{
    supra56i_t *dev = (supra56i_t *) priv;

    supra56i_map(dev, 0, 0);
    char_modem_detach(dev->modem);
    serial_close_detached(dev->uart);
    free(dev);
}

static void *
supra56i_init(UNUSED(const device_t *info))
{
    static const uint16_t bases[] = { 0, 0x3f8, 0x2f8, 0x3e8, 0x2e8 };
    supra56i_t           *dev     = (supra56i_t *) calloc(1, sizeof(supra56i_t));
    int                   mode    = device_get_config_int("mode");

    dev->uart  = serial_init_detached(SERIAL_16550, supra56i_uart_irq, dev);
    dev->modem = char_modem_attach(&dev->uart->char_port, &supra56i_model, "Internal (ISA)");
    if (dev->uart->char_port.chardev.control)
        dev->uart->char_port.chardev.control((dev->uart->mctrl & 0x03) | (dev->uart->lcr & 0x40),
                                             dev->uart->char_port.chardev.priv);

    if ((mode < 0) || (mode >= (int) (sizeof(bases) / sizeof(bases[0]))))
        mode = 0;
    if (mode == 0)
        dev->pnp = isapnp_add_card(supra56i_pnp_rom, sizeof(supra56i_pnp_rom),
                                   supra56i_pnp_config_changed, NULL, NULL, NULL, dev);
    else
        supra56i_map(dev, bases[mode], device_get_config_int("irq"));

    return dev;
}

// clang-format off
static const device_config_t supra56i_config[] = {
    {
        .name           = "mode",
        .description    = "Address",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "Plug and Play",     .value = 0 },
            { .description = "COM1 (3F8h)",       .value = 1 },
            { .description = "COM2 (2F8h)",       .value = 2 },
            { .description = "COM3 (3E8h)",       .value = 3 },
            { .description = "COM4 (2E8h)",       .value = 4 },
            { .description = ""                              }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "irq",
        .description    = "IRQ (without Plug and Play)",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 3,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "IRQ 3",  .value =  3 },
            { .description = "IRQ 4",  .value =  4 },
            { .description = "IRQ 5",  .value =  5 },
            { .description = "IRQ 7",  .value =  7 },
            { .description = "IRQ 9",  .value =  9 },
            { .description = "IRQ 10", .value = 10 },
            { .description = "IRQ 11", .value = 11 },
            { .description = "IRQ 12", .value = 12 },
            { .description = "IRQ 15", .value = 15 },
            { .description = ""                    }
        },
        .bios           = { { 0 } }
    },
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
};
// clang-format on

const device_t supra56i_device = {
    .name          = "Diamond SupraExpress 56i Sp",
    .internal_name = "supra56i",
    .flags         = DEVICE_ISA16,
    .local         = 0,
    .init          = supra56i_init,
    .close         = supra56i_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = supra56i_config
};

/* ------------------------------------------------------------ the selector */

static const struct {
    const device_t *dev;
} cards[] = {
    // clang-format off
    [MODEM_CARD_NONE]     = { &device_none     },
    [MODEM_CARD_SUPRA56I] = { &supra56i_device },
    { NULL }
    // clang-format on
};

void
modem_card_reset(void)
{
    if ((modem_card_type <= MODEM_CARD_NONE) || (modem_card_type >= MODEM_CARD_COUNT))
        return;

    device_add(cards[modem_card_type].dev);
}

const char *
modem_card_get_internal_name(int card)
{
    return device_get_internal_name(cards[card].dev);
}

int
modem_card_get_from_internal_name(const char *str)
{
    for (int c = 0; cards[c].dev != NULL; c++) {
        if (!strcmp(cards[c].dev->internal_name, str))
            return c;
    }

    return MODEM_CARD_NONE;
}

const device_t *
modem_card_get_device(int card)
{
    return cards[card].dev;
}

int
modem_card_has_config(int card)
{
    if (cards[card].dev == NULL)
        return 0;

    return (cards[card].dev->config ? 1 : 0);
}
