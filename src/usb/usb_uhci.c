/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             A PCI USB 1.1 host controller: Universal Host Controller
 *             Interface (UHCI, Intel's design, revision 1.1), presented as
 *             the VIA VT83C572 -- the UHCI function VIA used in its
 *             southbridges and on add-in cards, which Windows 98 SE, ME, 2000
 *             and XP and Linux all drive with their built-in UHCI drivers.
 *
 *             Two root-hub ports.  The schedule (frame list, queue heads,
 *             transfer descriptors) is walked once per emulated millisecond.
 *             A device that cannot answer yet -- a passed-through host device
 *             whose transfer is still in flight -- NAKs, and the TD is simply
 *             retried on a later frame, which is exactly what UHCI does with
 *             a NAK; that keeps the host side asynchronous without the
 *             controller knowing.
 *
 *             Reference: Intel, "Universal Host Controller Interface (UHCI)
 *             Design Guide", revision 1.1, March 1996.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/dma.h>
#include <86box/pci.h>
#include <86box/timer.h>
#include <86box/thread.h>
#include <86box/plat_unused.h>
#include <86box/usb_next.h>

#ifdef ENABLE_USB_UHCI_LOG
int usb_uhci_do_log = ENABLE_USB_UHCI_LOG;

static void
uhci_log(const char *fmt, ...)
{
    va_list ap;

    if (usb_uhci_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define uhci_log(fmt, ...)
#endif

/* Registers (I/O, 32 bytes at BAR4). */
#define REG_USBCMD    0x00
#define REG_USBSTS    0x02
#define REG_USBINTR   0x04
#define REG_FRNUM     0x06
#define REG_FLBASEADD 0x08
#define REG_SOFMOD    0x0c
#define REG_PORTSC1   0x10

#define CMD_RS        0x0001
#define CMD_HCRESET   0x0002
#define CMD_GRESET    0x0004
#define CMD_EGSM      0x0008
#define CMD_FGR       0x0010
#define CMD_CF        0x0040
#define CMD_MAXP      0x0080

#define STS_USBINT    0x0001
#define STS_ERRINT    0x0002
#define STS_RD        0x0004
#define STS_HSE       0x0008
#define STS_HCPE      0x0010
#define STS_HCHALTED  0x0020

#define INTR_CRC      0x0001
#define INTR_RESUME   0x0002
#define INTR_IOC      0x0004
#define INTR_SPD      0x0008

#define PORT_CCS      0x0001
#define PORT_CSC      0x0002
#define PORT_PED      0x0004
#define PORT_PEDC     0x0008
#define PORT_LS_J     0x0010  /* line status D+ (full speed idle) */
#define PORT_LS_K     0x0020  /* line status D- (low speed idle)  */
#define PORT_RD       0x0040
#define PORT_ALWAYS1  0x0080
#define PORT_LSDA     0x0100
#define PORT_PR       0x0200
#define PORT_SUSP     0x1000
#define PORT_RWC      (PORT_CSC | PORT_PEDC)
#define PORT_RW       (PORT_PED | PORT_RD | PORT_PR | PORT_SUSP)

/* Transfer descriptor bits. */
#define LINK_T        0x00000001
#define LINK_QH       0x00000002
#define LINK_VF       0x00000004

#define TD_ACTLEN     0x000007ff
#define TD_BITSTUFF   0x00020000
#define TD_CRCTO      0x00040000
#define TD_NAK        0x00080000
#define TD_BABBLE     0x00100000
#define TD_DBUF       0x00200000
#define TD_STALLED    0x00400000
#define TD_ACTIVE     0x00800000
#define TD_IOC        0x01000000
#define TD_ISO        0x02000000
#define TD_LS         0x04000000
#define TD_CERR       0x18000000
#define TD_SPD        0x20000000

/* What processing a TD came to. */
enum {
    TD_DONE,      /* completed; a queue moves on to the next TD             */
    TD_RETRY,     /* still active (NAK, or already inactive and skipped)    */
    TD_STOP,      /* error or short packet: the queue stops here this frame */
};

typedef struct uhci_t {
    uint8_t  pci_conf[256];
    uint8_t  pci_slot;
    uint8_t *slotp;          /* the card's slot: our own, or the EHCI card's */
    int      companion;      /* function 0 of an EHCI card                   */
    uint8_t  irq_state;
    uint16_t io_base;
    int      io_mapped;

    uint16_t cmd, sts, intr, frnum;
    uint32_t flbase;
    uint8_t  sofmod;
    uint16_t portsc[USBN_PORTS];

    usbn_device_t *dev[USBN_PORTS];   /* what this controller sees on each port */

    pc_timer_t frame_timer;
    uint8_t    pending_sts;   /* status bits raised during this frame */
} uhci_t;

int usb_card_type = 0;

/* ------------------------------------------------------------- the IRQ --- */

static void
uhci_update_irq(uhci_t *dev)
{
    int level = 0;

    if ((dev->sts & STS_USBINT) && (dev->intr & (INTR_IOC | INTR_SPD)))
        level = 1;
    if ((dev->sts & STS_ERRINT) && (dev->intr & INTR_CRC))
        level = 1;
    if ((dev->sts & STS_RD) && (dev->intr & INTR_RESUME))
        level = 1;
    if (dev->sts & (STS_HSE | STS_HCPE))
        level = 1;

    if (level)
        pci_set_irq(*dev->slotp, PCI_INTA, &dev->irq_state);
    else
        pci_clear_irq(*dev->slotp, PCI_INTA, &dev->irq_state);
}

/* --------------------------------------------------------------- ports --- */

static void
uhci_port_connect(uhci_t *dev, int p, usbn_device_t *d)
{
    /* A USB 1.1 port: a high-speed device is shown at full speed. */
    d->fs_view     = (d->speed == USBN_SPEED_HIGH);
    dev->dev[p]    = d;
    dev->portsc[p] = (dev->portsc[p] & ~(PORT_LSDA | PORT_LS_J | PORT_LS_K | PORT_PED)) | PORT_CCS | PORT_CSC;
    if (d->speed == USBN_SPEED_LOW)
        dev->portsc[p] |= PORT_LSDA | PORT_LS_K;
    else
        dev->portsc[p] |= PORT_LS_J;
    if (dev->cmd & CMD_EGSM)
        dev->sts |= STS_RD;
}

static void
uhci_port_disconnect(uhci_t *dev, int p)
{
    usbn_device_t *d = dev->dev[p];

    if (d == NULL)
        return;
    dev->dev[p] = NULL;
    if (dev->portsc[p] & PORT_PED)
        dev->portsc[p] |= PORT_PEDC;
    dev->portsc[p] = (dev->portsc[p] & ~(PORT_CCS | PORT_PED | PORT_LSDA | PORT_LS_J | PORT_LS_K)) | PORT_CSC;
}

static usbn_device_t *
uhci_find_device(uhci_t *dev, uint8_t addr)
{
    for (int p = 0; p < USBN_PORTS; p++) {
        usbn_device_t *d = dev->dev[p];
        if (d && (dev->portsc[p] & PORT_PED) && !(dev->portsc[p] & (PORT_PR | PORT_SUSP)) && (d->addr == addr))
            return d;
    }
    return NULL;
}

/* -------------------------------------------------------- the schedule --- */

static uint32_t
rd32(uint32_t a)
{
    uint32_t v;
    dma_bm_read(a, (uint8_t *) &v, 4, 4);
    return v;
}

static void
wr32(uint32_t a, uint32_t v)
{
    dma_bm_write(a, (uint8_t *) &v, 4, 4);
}

static int
uhci_process_td(uhci_t *dev, uint32_t td)
{
    uint32_t       ctrl  = rd32(td + 4);
    uint32_t       token = rd32(td + 8);
    uint32_t       bufp  = rd32(td + 12);
    uint8_t        pid   = token & 0xff;
    uint8_t        addr  = (token >> 8) & 0x7f;
    uint8_t        ep    = (token >> 15) & 0x0f;
    int            maxlen = ((token >> 21) + 1) & 0x7ff;   /* 0x7ff encodes 0 */
    uint8_t        buf[1280];
    usbn_device_t *d;
    int            ret;

    /* An inactive TD at the head of a queue means the queue is stopped -- a
       short packet or an error the driver has not dealt with yet -- and the
       controller goes on to the next queue without touching it (UHCI Design
       Guide 1.1).  Only a TD completed now moves a queue's element pointer on;
       moving past one the driver left there would send the rest of an
       abandoned transfer to the device. */
    if (!(ctrl & TD_ACTIVE))
        return TD_RETRY;

    if (ctrl & TD_ISO) {
        /* Isochronous transfers are not passed through; complete them empty
           so a driver polling one does not hang. */
        ctrl &= ~(TD_ACTIVE | TD_ACTLEN);
        ctrl |= 0x7ff;
        wr32(td + 4, ctrl);
        return TD_DONE;
    }

    if (maxlen > (int) sizeof(buf))
        maxlen = sizeof(buf);

    d = uhci_find_device(dev, addr);
    if (d == NULL) {
        usbn_trace("UHCI: no device at address %d (pid %02X ep %d): timeout", addr, pid, ep);
        /* Nobody answered: a timeout.  Real hardware retries up to C_ERR
           times; the device is not going to appear, so give up at once. */
        ctrl = (ctrl & ~(TD_ACTIVE | TD_CERR)) | TD_CRCTO;
        wr32(td + 4, ctrl);
        dev->pending_sts |= STS_ERRINT;
        if (ctrl & TD_IOC)
            dev->pending_sts |= STS_USBINT;
        return TD_STOP;
    }

    if ((pid == USB_PID_OUT) || (pid == USB_PID_SETUP)) {
        if (maxlen)
            dma_bm_read(bufp, buf, maxlen, 1);
        ret = d->packet(d, pid, ep, buf, maxlen);
        if (ret >= 0)
            ret = maxlen;
    } else if (pid == USB_PID_IN) {
        ret = d->packet(d, pid, ep, buf, maxlen);
        if (ret > maxlen)
            ret = USBN_BABBLE;
        else if (ret > 0)
            dma_bm_write(bufp, buf, ret, 1);
    } else {
        /* Not a PID the controller sends: a host controller process error. */
        dev->sts |= STS_HCPE | STS_HCHALTED;
        dev->cmd &= ~CMD_RS;
        return TD_STOP;
    }

    if (ret == USBN_NAK) {
        if (!(ctrl & TD_NAK)) {
            ctrl |= TD_NAK;
            wr32(td + 4, ctrl);
        }
        return TD_RETRY;
    }

    ctrl &= ~(TD_ACTIVE | TD_NAK | TD_ACTLEN);
    if (ret < 0) {
        if (ret == USBN_STALL)
            ctrl |= TD_STALLED;
        else if (ret == USBN_BABBLE)
            ctrl |= TD_STALLED | TD_BABBLE;
        else
            ctrl |= TD_STALLED | TD_CRCTO;
        ctrl |= 0x7ff;
        ctrl &= ~TD_CERR;
        wr32(td + 4, ctrl);
        dev->pending_sts |= STS_ERRINT;
        if (ctrl & TD_IOC)
            dev->pending_sts |= STS_USBINT;
        return TD_STOP;
    }

    ctrl |= (ret - 1) & 0x7ff;      /* ActLen is n-1, 0x7ff for none */
    wr32(td + 4, ctrl);
    if (ctrl & TD_IOC)
        dev->pending_sts |= STS_USBINT;

    if ((pid == USB_PID_IN) && (ret < maxlen) && (ctrl & TD_SPD)) {
        dev->pending_sts |= STS_USBINT;
        return TD_STOP;
    }
    return TD_DONE;
}

/* A queue: run its TDs in order until one cannot complete this frame. */
static void
uhci_process_qh(uhci_t *dev, uint32_t qh, int depth)
{
    uint32_t elem = rd32(qh + 4);

    for (int n = 0; (n < 64) && !(elem & LINK_T); n++) {
        if (elem & LINK_QH) {
            if (depth < 4)
                uhci_process_qh(dev, elem & ~0xf, depth + 1);
            return;
        }

        uint32_t td  = elem & ~0xf;
        int      res = uhci_process_td(dev, td);

        if (res != TD_DONE)
            return;
        elem = rd32(td);
        wr32(qh + 4, elem);     /* the queue element pointer moves on */
    }
}

static void
uhci_frame(void *priv)
{
    uhci_t  *dev = (uhci_t *) priv;
    uint32_t link;

    timer_on_auto(&dev->frame_timer, 1000.0);
    if (!dev->companion)
        usbn_apply();   /* an EHCI card does this for its companion */

    if (!(dev->cmd & CMD_RS))
        return;

    dev->pending_sts = 0;
    link = rd32(dev->flbase + ((dev->frnum & 0x3ff) << 2));

    /* The horizontal walk.  Bounded: a schedule is a list that may loop back
       on itself (the bandwidth-reclamation loop), and a frame is only 1 ms. */
    for (int n = 0; (n < 256) && !(link & LINK_T); n++) {
        if (link & LINK_QH) {
            uint32_t qh = link & ~0xf;
            uhci_process_qh(dev, qh, 0);
            link = rd32(qh);
        } else {
            uint32_t td = link & ~0xf;
            uhci_process_td(dev, td);
            link = rd32(td);
        }
    }

    dev->frnum = (dev->frnum + 1) & 0x7ff;
    if (dev->pending_sts) {
        dev->sts |= dev->pending_sts;
        uhci_update_irq(dev);
    }
}

/* ----------------------------------------------------------- registers --- */

static void
uhci_reset_regs(uhci_t *dev)
{
    dev->cmd    = 0;
    dev->sts    = STS_HCHALTED;
    dev->intr   = 0;
    dev->frnum  = 0;
    dev->flbase = 0;
    dev->sofmod = 0x40;
    for (int p = 0; p < USBN_PORTS; p++) {
        dev->portsc[p] &= PORT_CCS | PORT_LSDA | PORT_LS_J | PORT_LS_K;
        if (dev->portsc[p] & PORT_CCS)
            dev->portsc[p] |= PORT_CSC;
        if (dev->dev[p]) {
            dev->dev[p]->addr = 0;
            if (dev->dev[p]->reset)
                dev->dev[p]->reset(dev->dev[p]);
        }
    }
    uhci_update_irq(dev);
}

static uint16_t
uhci_readw(uint16_t addr, void *priv)
{
    uhci_t  *dev = (uhci_t *) priv;
    uint16_t ret = 0xffff;

    switch (addr & 0x1e) {
        case REG_USBCMD:
            ret = dev->cmd;
            break;
        case REG_USBSTS:
            ret = dev->sts;
            break;
        case REG_USBINTR:
            ret = dev->intr;
            break;
        case REG_FRNUM:
            ret = dev->frnum;
            break;
        case REG_FLBASEADD:
            ret = dev->flbase & 0xffff;
            break;
        case REG_FLBASEADD + 2:
            ret = dev->flbase >> 16;
            break;
        case REG_SOFMOD:
            ret = dev->sofmod;
            break;
        case REG_PORTSC1:
        case REG_PORTSC1 + 2:
            ret = dev->portsc[((addr & 0x1e) - REG_PORTSC1) >> 1] | PORT_ALWAYS1;
            break;
        default:
            /* Past the last port: reads as an invalid port (bit 7 clear) so
               drivers that probe for more ports stop here. */
            ret = 0xff7f;
            break;
    }
    return ret;
}

static uint8_t
uhci_readb(uint16_t addr, void *priv)
{
    uint16_t w = uhci_readw(addr & ~1, priv);
    return (addr & 1) ? (w >> 8) : (w & 0xff);
}

static uint32_t
uhci_readl(uint16_t addr, void *priv)
{
    return uhci_readw(addr, priv) | ((uint32_t) uhci_readw(addr + 2, priv) << 16);
}

static void
uhci_writew(uint16_t addr, uint16_t val, void *priv)
{
    uhci_t *dev = (uhci_t *) priv;
    int     p;

    switch (addr & 0x1e) {
        case REG_USBCMD:
            if (val & CMD_GRESET) {
                uhci_reset_regs(dev);
                dev->cmd = val & CMD_GRESET;
                return;
            }
            if (val & CMD_HCRESET) {
                uhci_reset_regs(dev);
                return;
            }
            dev->cmd = val & 0x00ff;
            if (dev->cmd & CMD_RS)
                dev->sts &= ~STS_HCHALTED;
            else
                dev->sts |= STS_HCHALTED;
            break;
        case REG_USBSTS:
            dev->sts &= ~(val & 0x003f & ~STS_HCHALTED);
            uhci_update_irq(dev);
            break;
        case REG_USBINTR:
            dev->intr = val & 0x000f;
            uhci_update_irq(dev);
            break;
        case REG_FRNUM:
            if (dev->sts & STS_HCHALTED)
                dev->frnum = val & 0x07ff;
            break;
        case REG_FLBASEADD:
            dev->flbase = (dev->flbase & 0xffff0000) | (val & 0xf000);
            break;
        case REG_FLBASEADD + 2:
            dev->flbase = (dev->flbase & 0x0000ffff) | ((uint32_t) val << 16);
            break;
        case REG_SOFMOD:
            dev->sofmod = val & 0x7f;
            break;
        case REG_PORTSC1:
        case REG_PORTSC1 + 2:
            p = ((addr & 0x1e) - REG_PORTSC1) >> 1;
            /* Reset ends on the 1 -> 0 transition of PR: the device is reset
               and back at address 0. */
            if (!(dev->portsc[p] & PORT_PR) && (val & PORT_PR))
                usbn_trace("UHCI: port %d reset begins", p + 1);
            if ((dev->portsc[p] & PORT_PED) != (val & PORT_PED))
                usbn_trace("UHCI: port %d %s", p + 1, (val & PORT_PED) ? "enabled" : "disabled");
            if ((dev->portsc[p] & PORT_PR) && !(val & PORT_PR) && dev->dev[p]) {
                usbn_trace("UHCI: port %d reset ends", p + 1);
                dev->dev[p]->addr = 0;
                if (dev->dev[p]->reset)
                    dev->dev[p]->reset(dev->dev[p]);
            }
            dev->portsc[p] &= ~(val & PORT_RWC);
            dev->portsc[p] = (dev->portsc[p] & ~PORT_RW) | (val & PORT_RW);
            if (!(dev->portsc[p] & PORT_CCS))
                dev->portsc[p] &= ~PORT_PED;   /* nothing to enable */
            break;
        default:
            break;
    }
}

static void
uhci_writeb(uint16_t addr, uint8_t val, void *priv)
{
    /* Byte writes only make sense for SOFMOD and the halves of the 16-bit
       registers; merge them into a word write. */
    uhci_t  *dev = (uhci_t *) priv;
    uint16_t w;

    if ((addr & 0x1f) == REG_SOFMOD) {
        dev->sofmod = val & 0x7f;
        return;
    }
    w = uhci_readw(addr & ~1, priv);
    /* Write-1-to-clear bits must not be echoed back as ones. */
    if ((addr & 0x1e) == REG_USBSTS)
        w = 0;
    if ((addr & 0x1e) >= REG_PORTSC1)
        w &= ~PORT_RWC;
    if (addr & 1)
        w = (w & 0x00ff) | (val << 8);
    else
        w = (w & 0xff00) | val;
    uhci_writew(addr & ~1, w, priv);
}

static void
uhci_writel(uint16_t addr, uint32_t val, void *priv)
{
    uhci_writew(addr, val & 0xffff, priv);
    uhci_writew(addr + 2, val >> 16, priv);
}

static void
uhci_remap(uhci_t *dev)
{
    if (dev->io_mapped) {
        io_removehandler(dev->io_base, 0x20, uhci_readb, uhci_readw, uhci_readl,
                         uhci_writeb, uhci_writew, uhci_writel, dev);
        dev->io_mapped = 0;
    }
    dev->io_base = (dev->pci_conf[0x20] & 0xe0) | (dev->pci_conf[0x21] << 8);
    if ((dev->pci_conf[0x04] & 0x01) && dev->io_base) {
        io_sethandler(dev->io_base, 0x20, uhci_readb, uhci_readw, uhci_readl,
                      uhci_writeb, uhci_writew, uhci_writel, dev);
        dev->io_mapped = 1;
    }
}

/* ------------------------------------------------------------------ PCI --- */

uint8_t
uhci_pci_read(int func, int addr, UNUSED(int len), void *priv)
{
    const uhci_t *dev = (uhci_t *) priv;

    if (func > 0)
        return 0xff;
    return dev->pci_conf[addr & 0xff];
}

void
uhci_pci_write(int func, int addr, UNUSED(int len), uint8_t val, void *priv)
{
    uhci_t *dev = (uhci_t *) priv;

    if (func > 0)
        return;

    switch (addr) {
        case 0x04:
            dev->pci_conf[addr] = val & 0x17;
            uhci_remap(dev);
            break;
        case 0x05:
            dev->pci_conf[addr] = val & 0x01;
            break;
        case 0x07:
            dev->pci_conf[addr] &= ~(val & 0xf9);
            break;
        case 0x0d:
            dev->pci_conf[addr] = val;
            break;
        case 0x20:
            dev->pci_conf[addr] = (val & 0xe0) | 0x01;
            uhci_remap(dev);
            break;
        case 0x21:
            dev->pci_conf[addr] = val;
            uhci_remap(dev);
            break;
        case 0x3c:
            dev->pci_conf[addr] = val;
            break;
        case 0x40 ... 0x4f:
        case 0xc0 ... 0xc1:
            /* VIA configuration and the legacy-support register: kept. */
            dev->pci_conf[addr] = val;
            break;
        default:
            break;
    }
}

static void
uhci_pci_init_conf(uhci_t *dev)
{
    uint8_t *c = dev->pci_conf;

    memset(c, 0, sizeof(dev->pci_conf));
    c[0x00] = 0x06; c[0x01] = 0x11;   /* VIA */
    c[0x02] = 0x38; c[0x03] = 0x30;   /* VT83C572 USB */
    c[0x06] = 0x00; c[0x07] = 0x02;   /* medium DEVSEL */
    c[0x08] = 0x1a;                   /* revision */
    c[0x09] = 0x00;                   /* prog-if: UHCI */
    c[0x0a] = 0x03;                   /* USB */
    c[0x0b] = 0x0c;                   /* serial bus controller */
    c[0x0d] = 0x16;
    c[0x20] = 0x01;                   /* BAR4: I/O, 32 bytes */
    c[0x2c] = 0x06; c[0x2d] = 0x11;   /* subsystem: VIA */
    c[0x2e] = 0x38; c[0x2f] = 0x30;
    c[0x3d] = 0x01;                   /* INTA# */
    c[0x40] = 0x40;
    c[0x41] = 0x10;
    c[0x60] = 0x10;                   /* SBRN: USB 1.0 / 1.1 */
    c[0xc1] = 0x20;                   /* LEGSUP: PIRQ enable */
}

/* --------------------------------------------------------- the device --- */

static void
uhci_root_connect(void *priv, int port, usbn_device_t *d)
{
    uhci_port_connect((uhci_t *) priv, port, d);
}

static void
uhci_root_disconnect(void *priv, int port)
{
    uhci_port_disconnect((uhci_t *) priv, port);
}

static const usbn_root_ops_t uhci_root_ops = { uhci_root_connect, uhci_root_disconnect };

static uhci_t *
uhci_core_create(void)
{
    uhci_t *dev = calloc(1, sizeof(uhci_t));

    uhci_pci_init_conf(dev);
    dev->slotp = &dev->pci_slot;
    uhci_reset_regs(dev);
    timer_add(&dev->frame_timer, uhci_frame, dev, 0);
    timer_on_auto(&dev->frame_timer, 1000.0);
    return dev;
}

static void *
uhci_init(UNUSED(const device_t *info))
{
    uhci_t *dev = uhci_core_create();

    pci_add_card(PCI_ADD_NORMAL, uhci_pci_read, uhci_pci_write, dev, &dev->pci_slot);
    usbn_set_root(&uhci_root_ops, dev, 0);
    usbn_restore_ports();
    return dev;
}

static void
uhci_close(void *priv)
{
    usbn_set_root(NULL, NULL, 0);
    free(priv);
}

static void
uhci_reset(void *priv)
{
    uhci_reset_regs((uhci_t *) priv);
}

const device_t usb_uhci_via_device = {
    .name          = "VIA VT83C572 USB 1.1 Host Controller (UHCI, PCI)",
    .internal_name = "usb_uhci_via",
    .flags         = DEVICE_PCI,
    .local         = 0,
    .init          = uhci_init,
    .close         = uhci_close,
    .reset         = uhci_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

/* ------------------------------------------------- as an EHCI companion --- */

uhci_t *
uhci_companion_create(uint8_t *card_slot)
{
    uhci_t *dev = uhci_core_create();

    dev->companion      = 1;
    dev->slotp          = card_slot;
    dev->pci_conf[0x0e] = 0x80;   /* function 0 of a multi-function card */
    return dev;
}

void
uhci_companion_close(uhci_t *dev)
{
    free(dev);
}

void
uhci_companion_reset(uhci_t *dev)
{
    uhci_reset_regs(dev);
}

/* The EHCI card gives a port to us (d) or takes it back (NULL). */
void
uhci_route_port(uhci_t *dev, int port, usbn_device_t *d)
{
    if (dev->dev[port] == d)
        return;
    if (dev->dev[port])
        uhci_port_disconnect(dev, port);
    if (d)
        uhci_port_connect(dev, port, d);
}

/* -------------------------------------------------------- the card list --- */

extern const device_t usb_ehci_via_device;

static const device_t *usb_cards[] = {
    &device_none,
    &usb_uhci_via_device,
    &usb_ehci_via_device,
    NULL
};

void
usb_card_reset(void)
{
    if ((usb_card_type > 0) && (usb_card_type < usb_card_count()))
        device_add(usb_cards[usb_card_type]);
}

int
usb_card_count(void)
{
    int n = 0;
    while (usb_cards[n])
        n++;
    return n;
}

const char *
usb_card_get_internal_name(int card)
{
    return device_get_internal_name(usb_cards[card]);
}

const char *
usb_card_get_name(int card)
{
    return usb_cards[card]->name;
}

int
usb_card_get_from_internal_name(const char *s)
{
    for (int c = 0; usb_cards[c]; c++)
        if (!strcmp(usb_cards[c]->internal_name, s))
            return c;
    return 0;
}
