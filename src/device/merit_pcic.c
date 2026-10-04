/*
 * 86Box-Next  Ported from MegaPPBox (github.com/Xeon3D/MegaPPBox), 86Box
 *             stripped down to run untouched Merit Megatouch images.
 *
 *             The PC Card slots on the MAXX I/O board: a Cirrus Logic
 *             CL-PD6722 (Intel 82365SL compatible) at 0x3E0, two sockets,
 *             both empty.
 *
 * ---------------------------------------------------------------------------
 *
 * The MAXX I/O cards carry a "PCMCIA connector" (90003007, board
 * identification) -- the "PC CARD SLOTS" of the MAXX service manuals -- used
 * for the TournaMAXX / MegaNET modem, for flash cards (books export, ad
 * screens) and for updates (/etc/init.d/update.pcmcia).  The software finds
 * the controller the way PC laptops of the day did:
 *
 *   - DOS MAXX 1st loads CardSoft with SSCIRRUS.EXE, the Cirrus socket
 *     services;
 *   - the Linux releases load pcmcia_core + i82365 + ds and start cardmgr
 *     (/etc/sysconfig/pcmcia: PCIC=i82365).  Without a controller the i82365
 *     probe fails, cardmgr has nothing to manage, and the boot reports the
 *     errors; /sbin/modemdetect.sh then takes the path meant for the later
 *     Force cabinets ("Detecting Force modem") instead of the MAXX one.
 *
 * No card is emulated in the slots yet; the modem is on COM2 instead (see
 * char_modem.c).  So both sockets report no card, and everything a driver
 * writes is kept and read back.  What a driver *identifies* the chip by is
 * emulated:
 *
 *   0x00  revision, 0x83 (82365SL step B, which the PD67xx reports)
 *   0x1F  chip information: bits 7:6 read 11 then 00 alternately (the Cirrus
 *         signature; a write restarts it), bit 5 set = two sockets
 *   0x2E  extended index, read back (Linux tells a PD67xx from a VIA VT83C469
 *         by it); 0x2F extended data behind it
 *   0x16  bit 5, software interrupt: raises the status-change interrupt on
 *         the IRQ in 0x05 bits 7:4 once, as Linux's i82365 IRQ scan expects;
 *         reading the status-change register (0x04) clears it
 *
 * Index 0x00-0x3F is socket A, 0x40-0x7F socket B; there is no socket C or D,
 * and those indexes read 0xFF, which is how the drivers tell.
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
#include <86box/pic.h>
#include <86box/plat_unused.h>

#define PCIC_BASE     0x3e0
#define PCIC_SOCKETS  2

#define REG_IDENT     0x00
#define REG_STATUS    0x01
#define REG_CSC       0x04
#define REG_CSCINT    0x05
#define REG_MISC1     0x16 /* 82365 "general control"               */
#define REG_CHIP_INFO 0x1f
#define REG_EXT_INDEX 0x2e
#define REG_EXT_DATA  0x2f

#define MISC1_SW_IRQ  0x20

#define PD6722_IDENT  0x83
#define PD6722_INFO   0x20 /* dual socket, revision 0 */

typedef struct {
    uint8_t index;
    uint8_t reg[PCIC_SOCKETS][0x40];
    uint8_t ext[PCIC_SOCKETS][0x40];
    uint8_t info_toggle;
    int     irq_raised; /* the IRQ the software interrupt is holding, or 0 */
} pcic_t;

#ifdef ENABLE_MERIT_PCIC_LOG
int merit_pcic_do_log = ENABLE_MERIT_PCIC_LOG;

static void
merit_pcic_log(const char *fmt, ...)
{
    va_list ap;

    if (merit_pcic_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define merit_pcic_log(fmt, ...)
#endif

static void
pcic_irq_clear(pcic_t *dev)
{
    if (dev->irq_raised) {
        picintc(1 << dev->irq_raised);
        dev->irq_raised = 0;
    }
}

static uint8_t
pcic_reg_read(pcic_t *dev, int s, int r)
{
    uint8_t ret;

    switch (r) {
        case REG_IDENT:
            return PD6722_IDENT;

        case REG_STATUS:
            /* No card: both card-detect bits clear, no power, not ready. */
            return 0x00;

        case REG_CSC:
            ret            = dev->reg[s][REG_CSC];
            dev->reg[s][r] = 0x00;
            pcic_irq_clear(dev);
            return ret;

        case REG_CHIP_INFO:
            ret = (dev->info_toggle ? 0x00 : 0xc0) | PD6722_INFO;
            dev->info_toggle ^= 1;
            return ret;

        case REG_EXT_DATA:
            return dev->ext[s][dev->reg[s][REG_EXT_INDEX] & 0x3f];

        default:
            return dev->reg[s][r];
    }
}

static void
pcic_reg_write(pcic_t *dev, int s, int r, uint8_t val)
{
    switch (r) {
        case REG_IDENT:
        case REG_STATUS:
        case REG_CSC:
            break;

        case REG_CHIP_INFO:
            dev->info_toggle = 0;
            break;

        case REG_EXT_DATA:
            dev->ext[s][dev->reg[s][REG_EXT_INDEX] & 0x3f] = val;
            break;

        case REG_MISC1:
            /* The software interrupt fires once per write that sets it; the
               bit does not stay set, so the next probe can fire it again. */
            if (val & MISC1_SW_IRQ) {
                const int irq = dev->reg[s][REG_CSCINT] >> 4;

                val &= ~MISC1_SW_IRQ;
                if ((irq > 0) && (irq != 2)) {
                    pcic_irq_clear(dev);
                    dev->reg[s][REG_CSC] |= 0x08; /* card detect change */
                    dev->irq_raised = irq;
                    picint(1 << irq);
                    merit_pcic_log("PCIC: socket %c software interrupt on IRQ %d\n", 'A' + s, irq);
                }
            }
            dev->reg[s][r] = val;
            break;

        case REG_CSCINT:
            dev->reg[s][r] = val;
            if (!(val >> 4))
                pcic_irq_clear(dev);
            break;

        default:
            dev->reg[s][r] = val;
            break;
    }
}

static uint8_t
pcic_read(uint16_t port, void *priv)
{
    pcic_t *dev = (pcic_t *) priv;

    if (!(port & 1))
        return dev->index;
    if ((dev->index >> 6) >= PCIC_SOCKETS)
        return 0xff;
    return pcic_reg_read(dev, dev->index >> 6, dev->index & 0x3f);
}

static void
pcic_write(uint16_t port, uint8_t val, void *priv)
{
    pcic_t *dev = (pcic_t *) priv;

    if (!(port & 1)) {
        dev->index = val;
        return;
    }
    if ((dev->index >> 6) < PCIC_SOCKETS)
        pcic_reg_write(dev, dev->index >> 6, dev->index & 0x3f, val);
}

static void
pcic_reset(void *priv)
{
    pcic_t *dev = (pcic_t *) priv;

    pcic_irq_clear(dev);
    memset(dev->reg, 0, sizeof(dev->reg));
    memset(dev->ext, 0, sizeof(dev->ext));
    dev->index       = 0;
    dev->info_toggle = 0;
}

static void *
pcic_init(UNUSED(const device_t *info))
{
    pcic_t *dev = (pcic_t *) calloc(1, sizeof(pcic_t));

    io_sethandler(PCIC_BASE, 2, pcic_read, NULL, NULL, pcic_write, NULL, NULL, dev);
    return dev;
}

static void
pcic_close(void *priv)
{
    pcic_t *dev = (pcic_t *) priv;

    pcic_irq_clear(dev);
    free(dev);
}

const device_t merit_pcic_device = {
    .name          = "Cirrus Logic CL-PD6722 (Merit MAXX PC Card slots)",
    .internal_name = "merit_pcic",
    .flags         = DEVICE_ISA,
    .local         = 0,
    .init          = pcic_init,
    .close         = pcic_close,
    .reset         = pcic_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};
