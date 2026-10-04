/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             A PCI USB 2.0 host controller card: an Enhanced Host Controller
 *             (EHCI 1.0) for high-speed devices with a UHCI companion for
 *             full- and low-speed ones, sharing two ports -- the arrangement
 *             of the VIA VT6202 USB 2.0 add-in cards (VIA's VT83C572 UHCI as
 *             function 0, the VT6202 EHCI, 1106:3104, as function 1).  Windows
 *             XP and 2000 (SP4) and Linux drive it with their own EHCI and
 *             UHCI drivers.
 *
 *             Port ownership is the EHCI way: until the guest's EHCI driver
 *             sets CONFIGFLAG every port belongs to the companion; after it,
 *             a port starts as EHCI's, and the driver hands a full- or
 *             low-speed device to the companion by setting PORT_OWNER (a
 *             low-speed device shows a K state, a full-speed one does not
 *             come out of reset enabled).
 *
 *             The schedules -- the periodic frame list and the asynchronous
 *             queue-head ring -- are walked once per emulated millisecond.
 *             Queue heads and qTDs are supported; isochronous iTDs and
 *             split-transaction siTDs are skipped.  A qTD is handed to the
 *             device whole (up to 20 KB) and a device that is not ready NAKs,
 *             leaving it active for the next frame.
 *
 *             Reference: Intel, "Enhanced Host Controller Interface
 *             Specification for Universal Serial Bus", revision 1.0, 2002.
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
#include <86box/mem.h>
#include <86box/dma.h>
#include <86box/pci.h>
#include <86box/timer.h>
#include <86box/plat_unused.h>
#include <86box/usb_next.h>

#ifdef ENABLE_USB_EHCI_LOG
int usb_ehci_do_log = ENABLE_USB_EHCI_LOG;

static void
ehci_log(const char *fmt, ...)
{
    va_list ap;

    if (usb_ehci_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define ehci_log(fmt, ...)
#endif

#define MMIO_SIZE      0x100
#define OPREG          0x20     /* CAPLENGTH */

/* Operational registers, relative to OPREG. */
#define R_USBCMD       0x00
#define R_USBSTS       0x04
#define R_USBINTR      0x08
#define R_FRINDEX      0x0c
#define R_CTRLDSSEG    0x10
#define R_PERIODIC     0x14
#define R_ASYNC        0x18
#define R_CONFIGFLAG   0x40
#define R_PORTSC       0x44

#define CMD_RS         0x00000001
#define CMD_HCRESET    0x00000002
#define CMD_PSE        0x00000010
#define CMD_ASE        0x00000020
#define CMD_IAAD       0x00000040

#define STS_USBINT     0x00000001
#define STS_ERRINT     0x00000002
#define STS_PCD        0x00000004
#define STS_FLR        0x00000008
#define STS_HSE        0x00000010
#define STS_IAA        0x00000020
#define STS_HALTED     0x00001000
#define STS_PSS        0x00004000
#define STS_ASS        0x00008000

#define PORT_CCS       0x00000001
#define PORT_CSC       0x00000002
#define PORT_PED       0x00000004
#define PORT_PEDC      0x00000008
#define PORT_OCC       0x00000020
#define PORT_FPR       0x00000040
#define PORT_SUSP      0x00000080
#define PORT_PR        0x00000100
#define PORT_LS_MASK   0x00000c00
#define PORT_LS_K      0x00000400
#define PORT_LS_J      0x00000800
#define PORT_PP        0x00001000
#define PORT_PO        0x00002000
#define PORT_RWC       (PORT_CSC | PORT_PEDC | PORT_OCC)

/* Link pointers. */
#define LINK_T         0x00000001
#define LINK_TYPE(l)   (((l) >> 1) & 3)
#define TYPE_ITD       0
#define TYPE_QH        1
#define TYPE_SITD      2
#define TYPE_FSTN      3

/* qTD token. */
#define TOK_PING       0x00000001
#define TOK_XACTERR    0x00000008
#define TOK_BABBLE     0x00000010
#define TOK_DBERR      0x00000020
#define TOK_HALTED     0x00000040
#define TOK_ACTIVE     0x00000080
#define TOK_PID(t)     (((t) >> 8) & 3)
#define TOK_IOC        0x00008000
#define TOK_BYTES(t)   (((t) >> 16) & 0x7fff)
#define TOK_TOGGLE     0x80000000

#define PID_OUT        0
#define PID_IN         1
#define PID_SETUP      2

#define XFER_MAX       20480

typedef struct ehci_t {
    uint8_t       pci_conf[256];
    uint8_t       slot;
    uint8_t       irq_state;
    mem_mapping_t mmio;

    uint32_t cmd, sts, intr, frindex, periodic, async, configflag;
    uint32_t portsc[USBN_PORTS];

    usbn_device_t *dev[USBN_PORTS];   /* what is plugged into each port */
    uhci_t        *uhci;              /* the companion, function 0      */

    pc_timer_t frame_timer;
    uint32_t   pending_sts;
    uint8_t    buf[XFER_MAX];
} ehci_t;

/* ------------------------------------------------------------- the IRQ --- */

static void
ehci_update_irq(ehci_t *dev)
{
    if (dev->sts & dev->intr & 0x3f)
        pci_set_irq(dev->slot, PCI_INTB, &dev->irq_state);
    else
        pci_clear_irq(dev->slot, PCI_INTB, &dev->irq_state);
}

/* --------------------------------------------------------------- ports --- */

static int
port_companion_owned(ehci_t *dev, int p)
{
    return !dev->configflag || (dev->portsc[p] & PORT_PO);
}

/* Give the device on port p to whoever owns the port now, and make EHCI's
   own view of the port agree. */
static void
ehci_route(ehci_t *dev, int p)
{
    usbn_device_t *d    = dev->dev[p];
    int            comp = port_companion_owned(dev, p);

    /* The companion is full/low speed: a high-speed device there is shown
       at full speed (usb_speed.c), as a real one would fall back -- which is
       what a guest without an EHCI driver sees. */
    if (d && !comp)
        d->fs_view = 0;
    uhci_route_port(dev->uhci, p, comp ? d : NULL);

    int was = dev->portsc[p] & PORT_CCS;
    int now = !comp && (d != NULL);
    dev->portsc[p] &= ~(PORT_CCS | PORT_LS_MASK);
    if (now) {
        dev->portsc[p] |= PORT_CCS;
        dev->portsc[p] |= (d->speed == USBN_SPEED_LOW) ? PORT_LS_K : PORT_LS_J;
    } else if (dev->portsc[p] & PORT_PED) {
        dev->portsc[p] &= ~PORT_PED;
        dev->portsc[p] |= PORT_PEDC;
    }
    if (was != now) {
        dev->portsc[p] |= PORT_CSC;
        dev->sts |= STS_PCD;
        ehci_update_irq(dev);
    }
}

static void
ehci_root_connect(void *priv, int port, usbn_device_t *d)
{
    ehci_t *dev = (ehci_t *) priv;

    dev->dev[port] = d;
    ehci_route(dev, port);
}

static void
ehci_root_disconnect(void *priv, int port)
{
    ehci_t *dev = (ehci_t *) priv;

    dev->dev[port] = NULL;
    ehci_route(dev, port);
}

static const usbn_root_ops_t ehci_root_ops = { ehci_root_connect, ehci_root_disconnect };

static usbn_device_t *
ehci_find_device(ehci_t *dev, uint8_t addr)
{
    for (int p = 0; p < USBN_PORTS; p++) {
        usbn_device_t *d = dev->dev[p];
        if (d && !port_companion_owned(dev, p) && (dev->portsc[p] & PORT_PED) && !(dev->portsc[p] & (PORT_PR | PORT_SUSP))
            && (d->addr == addr))
            return d;
    }
    return NULL;
}

/* ---------------------------------------------------------- the memory --- */

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

/* A qTD's data: five 4 KB page pointers, the first with the offset. */
static void
qtd_copy(uint32_t ptr[5], uint8_t *buf, int len, int to_guest)
{
    uint32_t off = ptr[0] & 0xfff;

    for (int i = 0; i < len;) {
        int      page  = (off + i) >> 12;
        uint32_t in_pg = (off + i) & 0xfff;
        int      n     = 0x1000 - in_pg;

        if (page > 4)
            break;
        if (n > len - i)
            n = len - i;
        uint32_t a = (ptr[page] & ~0xfff) + in_pg;
        if (to_guest)
            dma_bm_write(a, buf + i, n, 1);
        else
            dma_bm_read(a, buf + i, n, 1);
        i += n;
    }
}

/* ------------------------------------------------------- the schedules --- */

enum { Q_DONE, Q_WAIT, Q_HALT };

/* Run the qTD in a queue head's overlay. */
static int
ehci_exec(ehci_t *dev, uint32_t qh)
{
    uint32_t       epc   = rd32(qh + 4);
    uint32_t       token = rd32(qh + 0x18);
    uint32_t       qtd   = rd32(qh + 0x0c) & ~0x1f;
    uint8_t        addr  = epc & 0x7f;
    uint8_t        ep    = (epc >> 8) & 0x0f;
    int            maxp  = (epc >> 16) & 0x7ff;
    int            total = TOK_BYTES(token);
    int            pid   = TOK_PID(token);
    uint32_t       ptr[5];
    usbn_device_t *d;
    int            ret;

    for (int i = 0; i < 5; i++)
        ptr[i] = rd32(qh + 0x1c + i * 4);
    if (total > XFER_MAX)
        total = XFER_MAX;

    d = ehci_find_device(dev, addr);
    if (d == NULL) {
        token = (token & ~TOK_ACTIVE) | TOK_HALTED | TOK_XACTERR;
        wr32(qh + 0x18, token);
        wr32(qtd + 8, token);
        dev->pending_sts |= STS_ERRINT;
        return Q_HALT;
    }

    switch (pid) {
        case PID_SETUP:
            qtd_copy(ptr, dev->buf, total, 0);
            ret = d->packet(d, USB_PID_SETUP, ep, dev->buf, total);
            if (ret >= 0)
                ret = total;
            break;
        case PID_OUT:
            qtd_copy(ptr, dev->buf, total, 0);
            ret = d->packet(d, USB_PID_OUT, ep, dev->buf, total);
            if (ret >= 0)
                ret = total;
            break;
        case PID_IN:
            ret = d->packet(d, USB_PID_IN, ep, dev->buf, total);
            if (ret > total)
                ret = USBN_BABBLE;
            else if (ret > 0)
                qtd_copy(ptr, dev->buf, ret, 1);
            break;
        default:
            ret = USBN_STALL;
            break;
    }

    if (ret == USBN_NAK)
        return Q_WAIT;

    if (ret < 0) {
        token &= ~TOK_ACTIVE;
        token |= TOK_HALTED;
        if (ret == USBN_BABBLE)
            token |= TOK_BABBLE;
        else if (ret != USBN_STALL)
            token |= TOK_XACTERR;
        wr32(qh + 0x18, token);
        wr32(qtd + 8, token);
        dev->pending_sts |= STS_ERRINT;
        if (token & TOK_IOC)
            dev->pending_sts |= STS_USBINT;
        return Q_HALT;
    }

    /* The data toggle moves once per packet. */
    int packets = (maxp > 0) ? ((ret + maxp - 1) / maxp) : 1;
    if (packets == 0)
        packets = 1;
    if (packets & 1)
        token ^= TOK_TOGGLE;

    int left = TOK_BYTES(token) - ret;
    token    = (token & ~(TOK_ACTIVE | 0x7fff0000)) | ((uint32_t) left << 16);
    wr32(qh + 0x18, token);
    wr32(qtd + 8, token);
    if ((token & TOK_IOC) || ((pid == PID_IN) && left))
        dev->pending_sts |= STS_USBINT;
    return Q_DONE;
}

static void
ehci_process_qh(ehci_t *dev, uint32_t qh)
{
    for (int n = 0; n < 16; n++) {
        uint32_t token = rd32(qh + 0x18);

        if (token & TOK_ACTIVE) {
            if (ehci_exec(dev, qh) != Q_DONE)
                return;
            continue;
        }
        if (token & TOK_HALTED)
            return;

        /* Advance: after a short IN the alternate next qTD, else the next. */
        uint32_t next = rd32(qh + 0x10);
        uint32_t alt  = rd32(qh + 0x14);
        if (TOK_BYTES(token) && (TOK_PID(token) == PID_IN) && !(alt & LINK_T))
            next = alt;
        if (next & LINK_T)
            return;

        uint32_t qtd  = next & ~0x1f;
        uint32_t qtok = rd32(qtd + 8);
        if (!(qtok & TOK_ACTIVE))
            return;

        /* Load the overlay.  Unless the qTD carries its own toggle (DTC),
           the queue head's toggle is kept. */
        if (!(rd32(qh + 4) & (1u << 14)))
            qtok = (qtok & ~TOK_TOGGLE) | (token & TOK_TOGGLE);
        wr32(qh + 0x0c, qtd);
        wr32(qh + 0x10, rd32(qtd));
        wr32(qh + 0x14, rd32(qtd + 4));
        wr32(qh + 0x18, qtok);
        for (int i = 0; i < 5; i++)
            wr32(qh + 0x1c + i * 4, rd32(qtd + 0x0c + i * 4));
    }
}

static void
ehci_periodic(ehci_t *dev)
{
    uint32_t link = rd32((dev->periodic & ~0xfff) + (((dev->frindex >> 3) & 0x3ff) << 2));

    for (int n = 0; (n < 64) && !(link & LINK_T); n++) {
        uint32_t a = link & ~0x1f;

        if (LINK_TYPE(link) == TYPE_QH)
            ehci_process_qh(dev, a);
        /* iTDs, siTDs and FSTNs are passed over: their first dword is the
           next link all the same. */
        link = rd32(a);
    }
}

static void
ehci_async(ehci_t *dev)
{
    uint32_t start = dev->async & ~0x1f;
    uint32_t qh    = start;

    for (int n = 0; n < 64; n++) {
        ehci_process_qh(dev, qh);
        uint32_t link = rd32(qh);
        if ((link & LINK_T) || (LINK_TYPE(link) != TYPE_QH))
            break;
        qh = link & ~0x1f;
        if (qh == start)
            break;
    }
}

static void
ehci_frame(void *priv)
{
    ehci_t *dev = (ehci_t *) priv;

    timer_on_auto(&dev->frame_timer, 1000.0);
    usbn_apply();

    if (!(dev->cmd & CMD_RS))
        return;

    dev->pending_sts = 0;
    if (dev->cmd & CMD_PSE)
        ehci_periodic(dev);
    if (dev->cmd & CMD_ASE)
        ehci_async(dev);

    /* Async advance doorbell: the driver may now free unlinked queue heads. */
    if (dev->cmd & CMD_IAAD) {
        dev->cmd &= ~CMD_IAAD;
        dev->pending_sts |= STS_IAA;
    }

    uint32_t old  = dev->frindex;
    dev->frindex  = (dev->frindex + 8) & 0x3fff;
    if ((old ^ dev->frindex) & 0x2000)
        dev->pending_sts |= STS_FLR;   /* 1024-entry frame list rolled over */

    if (dev->pending_sts) {
        dev->sts |= dev->pending_sts;
        ehci_update_irq(dev);
    }
}

/* ----------------------------------------------------------- registers --- */

static void
ehci_reset_regs(ehci_t *dev)
{
    dev->cmd        = 0x00080000;   /* interrupt threshold: 8 microframes */
    dev->sts        = STS_HALTED;
    dev->intr       = 0;
    dev->frindex    = 0;
    dev->periodic   = 0;
    dev->async      = 0;
    dev->configflag = 0;
    for (int p = 0; p < USBN_PORTS; p++) {
        dev->portsc[p] = PORT_PP | PORT_PO;
        if (dev->dev[p]) {
            dev->dev[p]->addr = 0;
            if (dev->dev[p]->reset)
                dev->dev[p]->reset(dev->dev[p]);
        }
        ehci_route(dev, p);
    }
    ehci_update_irq(dev);
}

static uint32_t
ehci_readl(uint32_t addr, void *priv)
{
    ehci_t  *dev = (ehci_t *) priv;
    uint32_t a   = addr & (MMIO_SIZE - 1) & ~3;

    switch (a) {
        case 0x00:
            return OPREG | (0x0100 << 16);                /* CAPLENGTH, HCIVERSION 1.00 */
        case 0x04:
            return USBN_PORTS | (USBN_PORTS << 8) | (1 << 12);   /* N_PORTS, N_PCC, N_CC = 1 */
        case 0x08:
            return 0x00000010;                            /* IST 1 frame, no 64-bit, no EECP */
        case 0x0c:
            return 0;
        default:
            break;
    }
    if (a < OPREG)
        return 0;
    a -= OPREG;
    switch (a) {
        case R_USBCMD:
            return dev->cmd;
        case R_USBSTS:
            return dev->sts | (((dev->cmd & CMD_PSE) && (dev->cmd & CMD_RS)) ? STS_PSS : 0)
                | (((dev->cmd & CMD_ASE) && (dev->cmd & CMD_RS)) ? STS_ASS : 0);
        case R_USBINTR:
            return dev->intr;
        case R_FRINDEX:
            return dev->frindex;
        case R_CTRLDSSEG:
            return 0;
        case R_PERIODIC:
            return dev->periodic;
        case R_ASYNC:
            return dev->async;
        case R_CONFIGFLAG:
            return dev->configflag;
        default:
            if ((a >= R_PORTSC) && (a < R_PORTSC + 4 * USBN_PORTS))
                return dev->portsc[(a - R_PORTSC) >> 2];
            return 0;
    }
}

static void
ehci_write_portsc(ehci_t *dev, int p, uint32_t val)
{
    uint32_t old = dev->portsc[p];
    uint32_t v   = old;

    v &= ~(val & PORT_RWC);
    if (!(val & PORT_PED))
        v &= ~PORT_PED;                 /* software can disable, not enable */
    v = (v & ~(PORT_SUSP | PORT_FPR | 0x007fc000)) | (val & (PORT_SUSP | PORT_FPR | 0x007fc000));

    if ((val & PORT_PR) && !(old & PORT_PR)) {
        v |= PORT_PR;
        v &= ~PORT_PED;
    } else if (!(val & PORT_PR) && (old & PORT_PR)) {
        /* Reset over: a high-speed device comes out enabled; a full-speed
           one stays disabled, and the driver hands it to the companion. */
        v &= ~PORT_PR;
        usbn_device_t *d = dev->dev[p];
        if (d && (v & PORT_CCS)) {
            d->addr = 0;
            if (d->reset)
                d->reset(d);
            if (d->speed == USBN_SPEED_HIGH)
                v = (v | PORT_PED) & ~PORT_LS_MASK;
        }
    }

    dev->portsc[p] = (v & ~PORT_PO) | (val & PORT_PO) | PORT_PP;
    if ((old ^ dev->portsc[p]) & PORT_PO) {
        dev->portsc[p] &= ~PORT_PED;
        ehci_route(dev, p);
    }
}

static void
ehci_writel(uint32_t addr, uint32_t val, void *priv)
{
    ehci_t  *dev = (ehci_t *) priv;
    uint32_t a   = addr & (MMIO_SIZE - 1) & ~3;

    if (a < OPREG)
        return;
    a -= OPREG;
    switch (a) {
        case R_USBCMD:
            if (val & CMD_HCRESET) {
                ehci_reset_regs(dev);
                return;
            }
            dev->cmd = val & 0x00ff007f;
            if (dev->cmd & CMD_RS)
                dev->sts &= ~STS_HALTED;
            else
                dev->sts |= STS_HALTED;
            break;
        case R_USBSTS:
            dev->sts &= ~(val & 0x3f);
            ehci_update_irq(dev);
            break;
        case R_USBINTR:
            dev->intr = val & 0x3f;
            ehci_update_irq(dev);
            break;
        case R_FRINDEX:
            if (dev->sts & STS_HALTED)
                dev->frindex = val & 0x3fff;
            break;
        case R_PERIODIC:
            dev->periodic = val & ~0xfff;
            break;
        case R_ASYNC:
            dev->async = val & ~0x1f;
            break;
        case R_CONFIGFLAG:
            if ((val & 1) != dev->configflag) {
                dev->configflag = val & 1;
                for (int p = 0; p < USBN_PORTS; p++) {
                    /* Every port goes to EHCI when the driver takes over, and
                       back to the companion if it lets go. */
                    if (dev->configflag)
                        dev->portsc[p] &= ~PORT_PO;
                    else
                        dev->portsc[p] |= PORT_PO;
                    ehci_route(dev, p);
                }
            }
            break;
        default:
            if ((a >= R_PORTSC) && (a < R_PORTSC + 4 * USBN_PORTS))
                ehci_write_portsc(dev, (a - R_PORTSC) >> 2, val);
            break;
    }
}

static uint8_t
ehci_readb(uint32_t addr, void *priv)
{
    return ehci_readl(addr & ~3, priv) >> ((addr & 3) * 8);
}

static uint16_t
ehci_readw(uint32_t addr, void *priv)
{
    return ehci_readl(addr & ~3, priv) >> ((addr & 2) * 8);
}

static void
ehci_writeb(uint32_t addr, uint8_t val, void *priv)
{
    /* Byte writes to the registers are rare (CONFIGFLAG, the port bytes);
       write-1-to-clear bits are not echoed back. */
    uint32_t a     = addr & ~3;
    int      shift = (addr & 3) * 8;
    uint32_t cur   = ehci_readl(a, priv);

    if (((a - OPREG) == R_USBSTS) || (((a - OPREG) >= R_PORTSC) && ((a - OPREG) < R_PORTSC + 4 * USBN_PORTS)))
        cur &= ~(PORT_RWC | 0x3f);
    cur = (cur & ~(0xffu << shift)) | ((uint32_t) val << shift);
    ehci_writel(a, cur, priv);
}

static void
ehci_writew(uint32_t addr, uint16_t val, void *priv)
{
    uint32_t a     = addr & ~3;
    int      shift = (addr & 2) * 8;
    uint32_t cur   = ehci_readl(a, priv);

    if (((a - OPREG) == R_USBSTS) || (((a - OPREG) >= R_PORTSC) && ((a - OPREG) < R_PORTSC + 4 * USBN_PORTS)))
        cur &= ~(PORT_RWC | 0x3f);
    cur = (cur & ~(0xffffu << shift)) | ((uint32_t) val << shift);
    ehci_writel(a, cur, priv);
}

/* ------------------------------------------------------------------ PCI --- */

static void
ehci_remap(ehci_t *dev)
{
    uint32_t base = (dev->pci_conf[0x11] << 8) | (dev->pci_conf[0x12] << 16) | (dev->pci_conf[0x13] << 24);

    if ((dev->pci_conf[0x04] & 0x02) && base)
        mem_mapping_set_addr(&dev->mmio, base, MMIO_SIZE);
    else
        mem_mapping_disable(&dev->mmio);
}

static uint8_t
ehci_card_read(int func, int addr, int len, void *priv)
{
    ehci_t *dev = (ehci_t *) priv;

    if (func == 0)
        return uhci_pci_read(0, addr, len, dev->uhci);
    if (func == 1)
        return dev->pci_conf[addr & 0xff];
    return 0xff;
}

static void
ehci_card_write(int func, int addr, int len, uint8_t val, void *priv)
{
    ehci_t *dev = (ehci_t *) priv;

    if (func == 0) {
        uhci_pci_write(0, addr, len, val, dev->uhci);
        return;
    }
    if (func != 1)
        return;

    switch (addr) {
        case 0x04:
            dev->pci_conf[addr] = val & 0x16;
            ehci_remap(dev);
            break;
        case 0x05:
            dev->pci_conf[addr] = val & 0x01;
            break;
        case 0x07:
            dev->pci_conf[addr] &= ~(val & 0xf9);
            break;
        case 0x0d:
        case 0x3c:
        case 0x61:          /* FLADJ */
            dev->pci_conf[addr] = val;
            break;
        case 0x11:
        case 0x12:
        case 0x13:
            dev->pci_conf[addr] = val;   /* 256 bytes: 0x10 stays 0 */
            ehci_remap(dev);
            break;
        default:
            break;
    }
}

static void
ehci_pci_init_conf(ehci_t *dev)
{
    uint8_t *c = dev->pci_conf;

    memset(c, 0, sizeof(dev->pci_conf));
    c[0x00] = 0x06; c[0x01] = 0x11;   /* VIA */
    c[0x02] = 0x04; c[0x03] = 0x31;   /* VT6202 USB 2.0 */
    c[0x06] = 0x10; c[0x07] = 0x02;
    c[0x08] = 0x63;                   /* revision */
    c[0x09] = 0x20;                   /* prog-if: EHCI */
    c[0x0a] = 0x03;                   /* USB */
    c[0x0b] = 0x0c;                   /* serial bus controller */
    c[0x0d] = 0x16;
    c[0x0e] = 0x80;
    c[0x2c] = 0x06; c[0x2d] = 0x11;
    c[0x2e] = 0x04; c[0x2f] = 0x31;
    c[0x3d] = 0x02;                   /* INTB# (the companion has INTA#) */
    c[0x60] = 0x20;                   /* SBRN: USB 2.0 */
    c[0x61] = 0x20;                   /* FLADJ default */
}

/* --------------------------------------------------------- the device --- */

static void *
ehci_init(UNUSED(const device_t *info))
{
    ehci_t *dev = calloc(1, sizeof(ehci_t));

    ehci_pci_init_conf(dev);
    pci_add_card(PCI_ADD_NORMAL, ehci_card_read, ehci_card_write, dev, &dev->slot);
    dev->uhci = uhci_companion_create(&dev->slot);
    mem_mapping_add(&dev->mmio, 0, 0, ehci_readb, ehci_readw, ehci_readl, ehci_writeb, ehci_writew, ehci_writel,
                    NULL, MEM_MAPPING_EXTERNAL, dev);
    mem_mapping_disable(&dev->mmio);
    ehci_reset_regs(dev);
    timer_add(&dev->frame_timer, ehci_frame, dev, 0);
    timer_on_auto(&dev->frame_timer, 1000.0);

    usbn_set_root(&ehci_root_ops, dev, 1);
    usbn_restore_ports();
    return dev;
}

static void
ehci_close(void *priv)
{
    ehci_t *dev = (ehci_t *) priv;

    for (int p = 0; p < USBN_PORTS; p++)
        uhci_route_port(dev->uhci, p, NULL);
    usbn_set_root(NULL, NULL, 0);
    uhci_companion_close(dev->uhci);
    free(dev);
}

static void
ehci_reset(void *priv)
{
    ehci_t *dev = (ehci_t *) priv;

    uhci_companion_reset(dev->uhci);
    ehci_reset_regs(dev);
}

const device_t usb_ehci_via_device = {
    .name          = "VIA VT6202 USB 2.0 Host Controller (EHCI + UHCI, PCI)",
    .internal_name = "usb_ehci_via",
    .flags         = DEVICE_PCI,
    .local         = 0,
    .init          = ehci_init,
    .close         = ehci_close,
    .reset         = ehci_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};
