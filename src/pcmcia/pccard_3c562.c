/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             3Com 3C562D/3C563D EtherLink III LAN+33.6 Modem PC Card: a
 *             multi-function card (pccard_mfc.c) with two functions --
 *
 *               0  the LAN: an EtherLink III, net_3c509b.c's 3C589 core
 *                  (threec562_lan_device), sixteen ports, its station
 *                  address in the CIS as well as its EEPROM;
 *               1  the modem: a 16550 that is no COM port
 *                  (serial_init_detached()) with char_modem.c's engine on it
 *                  as a 3Com 33.6 modem, eight ports.
 *
 *             Both share the card's IREQ.  Windows 98 has drivers for both in
 *             the box: NET3C562.INF (elpc3r.sys) for DEV0 and MDMGATEW.INF for
 *             DEV1.  The CIS is pccard_cis_3c562d().
 *
 *             The settings are the card's own (device instance socket + 1):
 *             the station address, and the modem's telephone line, as a COM
 *             port modem has them.
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
#include <86box/pcmcia.h>
#include <86box/plat_unused.h>

extern const device_t threec562_lan_device;
extern const pccard_func_t *el3_mfc_function(void *priv, pccard_mfc_t *m, int func, uint8_t mac[6]);

typedef struct {
    int           socket;
    pccard_mfc_t  mfc;
    uint8_t       cis[512];

    void         *lan;      /* net_3c509b.c's el3_t */

    serial_t     *uart;
    void         *modem;    /* char_modem.c's, on the UART */
    pccard_func_t modem_fn;
} c562_t;

/* The modem function: the UART's eight registers. */
static uint8_t
c562_modem_read(uint16_t off, void *priv)
{
    return serial_read(off, ((c562_t *) priv)->uart);
}

static void
c562_modem_write(uint16_t off, uint8_t val, void *priv)
{
    serial_write(off, val, ((c562_t *) priv)->uart);
}

static void
c562_modem_reset(void *priv)
{
    serial_reset_detached(((c562_t *) priv)->uart);
}

static void
c562_modem_irq(void *priv, int level)
{
    c562_t *dev = (c562_t *) priv;

    pccard_mfc_irq(&dev->mfc, 1, level);
}

static void
c562_close(void *priv)
{
    c562_t *dev = (c562_t *) priv;

    pcmcia_insert(dev->socket, NULL);
    if (dev->modem)
        char_modem_3c562_device.close(dev->modem);
    serial_close_detached(dev->uart);
    if (dev->lan)
        threec562_lan_device.close(dev->lan);
    free(dev);
}

static void *
c562_init(UNUSED(const device_t *info))
{
    c562_t              *dev = (c562_t *) calloc(1, sizeof(c562_t));
    const pccard_func_t *lan_fn;
    uint8_t              mac[6];
    uint32_t             lan_cfg, modem_cfg;
    int                  len;

    dev->socket = device_get_instance() - 1;
    if ((dev->socket < 0) || (dev->socket >= PCMCIA_SOCKETS))
        dev->socket = 0;

    /* The LAN, made in this card's device context: its station address and
       network link are the card's settings. */
    dev->lan = threec562_lan_device.init(&threec562_lan_device);
    if (dev->lan == NULL) {
        free(dev);
        return NULL;
    }

    lan_fn = el3_mfc_function(dev->lan, &dev->mfc, 0, mac);
    len    = pccard_cis_3c562d(dev->cis, mac, &lan_cfg, &modem_cfg);
    pccard_mfc_init(&dev->mfc, dev->socket, "3Com 3C562D LAN+Modem", dev->cis, len);
    pccard_mfc_add(&dev->mfc, lan_fn, lan_cfg, 0x23);

    /* The modem: a 16550 with the modem engine on its port. */
    dev->uart  = serial_init_detached(SERIAL_16550, c562_modem_irq, dev);
    dev->modem = char_open_unlisted(&dev->uart->char_port, &char_modem_3c562_device);
    if (dev->uart->char_port.chardev.control)
        dev->uart->char_port.chardev.control((dev->uart->mctrl & 0x03) | (dev->uart->lcr & 0x40),
                                             dev->uart->char_port.chardev.priv);
    dev->modem_fn = (pccard_func_t) {
        .io_len   = 8,
        .io_read  = c562_modem_read,
        .io_write = c562_modem_write,
        .reset    = c562_modem_reset,
        .priv     = dev
    };
    pccard_mfc_add(&dev->mfc, &dev->modem_fn, modem_cfg, 0x23);

    pcmcia_insert(dev->socket, &dev->mfc.card);
    return dev;
}

/* The station address, then the modem's settings as char_modem.c reads them
   (modem_config's entries, so a COM port modem and this one agree). */
static const device_config_t c562_config[] = {
    // clang-format off
    {
        .name           = "mac",
        .description    = "MAC Address",
        .type           = CONFIG_MAC,
        .default_string = NULL,
        .default_int    = -1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "line",
        .description    = "Modem telephone line",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "Not connected",             .value = 0 },
            { .description = "Dial out to a TCP/IP host", .value = 1 },
            { .description = "Internet (built-in ISP)",   .value = 2 },
            { .description = ""                                      }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "host",
        .description    = "Host",
        .type           = CONFIG_STRING,
        .default_string = "",
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "host_port",
        .description    = "Port",
        .type           = CONFIG_SPINNER,
        .default_string = NULL,
        .default_int    = 23,
        .file_filter    = NULL,
        .spinner        = { .min = 1, .max = 32767 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "connect_rate",
        .description    = "Reported connection speed",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 33600,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "33600", .value = 33600 },
            { .description = "28800", .value = 28800 },
            { .description = "14400", .value = 14400 },
            { .description = ""                      }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "speaker",
        .description    = "Modem speaker",
        .type           = CONFIG_BINARY,
        .default_string = NULL,
        .default_int    = 1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
    // clang-format on
};

const device_t pccard_3c562d_device = {
    .name          = "3Com EtherLink III LAN+33.6 Modem PC Card (3C562D)",
    .internal_name = "3c562d",
    .flags         = DEVICE_ISA,
    .local         = 0,
    .init          = c562_init,
    .close         = c562_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = c562_config
};
