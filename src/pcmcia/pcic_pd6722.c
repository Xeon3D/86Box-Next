/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The PC Card controller: a Cirrus Logic CL-PD6722 (Intel
 *             82365SL compatible) on ISA at 0x3E0, two sockets.  From
 *             MegaPPBox, where it is the PC Card slots of the Merit MAXX I/O
 *             board; here it is also a card of its own (Settings > PCMCIA),
 *             with the cards in pcmcia.c's list in its sockets.
 *
 * ---------------------------------------------------------------------------
 *
 * On the MAXX: its I/O cards carry a "PCMCIA connector" (90003007, board
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
 * The cards are pcmcia.c's (the TRENDnet TE100-PC16 network card,
 * net_te100pc16.c, for a start); a socket may also be empty.  What a driver
 * *identifies* the chip by is emulated:
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
 * and, for a socket holding a card, what drives it:
 *
 *   0x01  status: card detect, battery good, power, ready
 *   0x02  power: VCC on (bit 4) and outputs enabled (bit 7) power the card
 *   0x03  bits 3:0 the IRQ the card's IREQ is steered to, bit 5 I/O card,
 *         bit 6 clear = card held in RESET
 *   0x06  window enables: memory windows 0-4 (bits 0-4), I/O 0-1 (bits 6-7)
 *   0x08  I/O windows: start and stop, 16 bits each (0x08-0x0F)
 *   0x10  memory windows, eight registers apart: start, stop (4 KB units,
 *         ISA's 16 MB) and the offset to the card address, whose bit 14
 *         selects attribute memory and bit 15 write-protects the window
 *
 * Everything else a driver writes is kept and read back.
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
#include <86box/mem.h>
#include <86box/pic.h>
#include <86box/pcmcia.h>
#include <86box/plat_unused.h>

#define PCIC_BASE     0x3e0
#define PCIC_SOCKETS  2
#define PCIC_MEMWIN   5
#define PCIC_IOWIN    2

#define REG_IDENT     0x00
#define REG_STATUS    0x01
#define REG_POWER     0x02
#define REG_INTCTL    0x03
#define REG_CSC       0x04
#define REG_CSCINT    0x05
#define REG_WINEN     0x06
#define REG_IO_BASE   0x08 /* four registers per I/O window            */
#define REG_MEM_BASE  0x10 /* eight registers per memory window        */
#define REG_MISC1     0x16 /* 82365 "general control"                  */
#define REG_CHIP_INFO 0x1f
#define REG_EXT_INDEX 0x2e
#define REG_EXT_DATA  0x2f

#define STATUS_BVD    0x03 /* BVD1/BVD2: battery good (STSCHG/SPKR idle) */
#define STATUS_CD     0x0c /* CD1/CD2: card fully inserted               */
#define STATUS_READY  0x20
#define STATUS_POWER  0x40

#define POWER_VCC     0x10
#define POWER_OUT     0x80

#define INTCTL_IRQ    0x0f
#define INTCTL_IOCARD 0x20
#define INTCTL_NRESET 0x40

#define MISC1_SW_IRQ  0x20

#define PD6722_IDENT  0x83
#define PD6722_INFO   0x20 /* dual socket, revision 0 */

struct pcic_t;

typedef struct {
    struct pcic_t *pcic;
    int            socket;
    int            win;
} pcic_win_t;

typedef struct {
    const pccard_t *card;
    int             live;    /* powered, outputs on and out of RESET */
    int             ireq;    /* the card's interrupt request line    */
    int             irq_out; /* the ISA IRQ it is raising, or 0      */
    uint16_t        io_base[PCIC_IOWIN];
    uint32_t        io_len[PCIC_IOWIN]; /* 0 = no handler installed  */
    mem_mapping_t   mem[PCIC_MEMWIN];
    pcic_win_t      mem_win[PCIC_MEMWIN];
} pcic_socket_t;

typedef struct pcic_t {
    uint8_t       index;
    uint8_t       reg[PCIC_SOCKETS][0x40];
    uint8_t       ext[PCIC_SOCKETS][0x40];
    uint8_t       info_toggle;
    int           irq_raised; /* the IRQ the software interrupt is holding, or 0 */
    pcic_socket_t sock[PCIC_SOCKETS];
} pcic_t;

/* One controller per machine; cards plugged in before it exists wait here. */
static pcic_t         *pcic_inst;
static const pccard_t *pcic_pending[PCIC_SOCKETS];

#ifdef ENABLE_PCIC_LOG
int pcic_do_log = ENABLE_PCIC_LOG;

static void
pcic_log(const char *fmt, ...)
{
    va_list ap;

    if (pcic_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define pcic_log(fmt, ...)
#endif

static void
pcic_irq_clear(pcic_t *dev)
{
    if (dev->irq_raised) {
        picintc(1 << dev->irq_raised);
        dev->irq_raised = 0;
    }
}

/* The card's IREQ goes out on the IRQ in 0x03 once the card is an I/O card. */
static void
pcic_card_irq_update(pcic_t *dev, int s)
{
    pcic_socket_t *sock = &dev->sock[s];
    const uint8_t  ctl  = dev->reg[s][REG_INTCTL];
    int            irq  = 0;

    if (sock->card && sock->live && (ctl & INTCTL_IOCARD) && ((ctl & INTCTL_IRQ) > 2))
        irq = ctl & INTCTL_IRQ;

    if (sock->irq_out && ((sock->irq_out != irq) || !sock->ireq)) {
        picintc(1 << sock->irq_out);
        sock->irq_out = 0;
    }
    if (irq && sock->ireq) {
        sock->irq_out = irq;
        picint(1 << irq);
    }
}

/* The card is live when powered, its outputs enabled and RESET released;
   going live or dropping out of it resets the card. */
static void
pcic_card_power_update(pcic_t *dev, int s)
{
    pcic_socket_t *sock = &dev->sock[s];
    const int      live = sock->card && ((dev->reg[s][REG_POWER] & (POWER_VCC | POWER_OUT)) == (POWER_VCC | POWER_OUT)) &&
                         (dev->reg[s][REG_INTCTL] & INTCTL_NRESET);

    if (live != sock->live) {
        pcic_log("PCIC: socket %c card %s\n", 'A' + s, live ? "live" : "off / in reset");
        sock->live = live;
        if (sock->card && sock->card->reset)
            sock->card->reset(sock->card->priv);
    }
    pcic_card_irq_update(dev, s);
}

/* --- I/O windows ---------------------------------------------------------- */

static pcic_socket_t *
pcic_io_socket(void *priv)
{
    pcic_socket_t *sock = (pcic_socket_t *) priv;
    const int      s    = (sock == &pcic_inst->sock[1]);

    if (!sock->card || !sock->live || !(pcic_inst->reg[s][REG_INTCTL] & INTCTL_IOCARD))
        return NULL;
    return sock;
}

static uint8_t
pcic_io_readb(uint16_t port, void *priv)
{
    const pcic_socket_t *sock = pcic_io_socket(priv);

    return sock ? sock->card->io_read(port, sock->card->priv) : 0xff;
}

static uint16_t
pcic_io_readw(uint16_t port, void *priv)
{
    const pcic_socket_t *sock = pcic_io_socket(priv);

    if (!sock)
        return 0xffff;
    if (sock->card->io_readw)
        return sock->card->io_readw(port, sock->card->priv);
    return sock->card->io_read(port, sock->card->priv) | (sock->card->io_read(port + 1, sock->card->priv) << 8);
}

static void
pcic_io_writeb(uint16_t port, uint8_t val, void *priv)
{
    const pcic_socket_t *sock = pcic_io_socket(priv);

    if (sock)
        sock->card->io_write(port, val, sock->card->priv);
}

static void
pcic_io_writew(uint16_t port, uint16_t val, void *priv)
{
    const pcic_socket_t *sock = pcic_io_socket(priv);

    if (!sock)
        return;
    if (sock->card->io_writew)
        sock->card->io_writew(port, val, sock->card->priv);
    else {
        sock->card->io_write(port, val & 0xff, sock->card->priv);
        sock->card->io_write(port + 1, val >> 8, sock->card->priv);
    }
}

static void
pcic_io_remap(pcic_t *dev, int s, int w)
{
    pcic_socket_t *sock  = &dev->sock[s];
    const uint8_t *r     = &dev->reg[s][REG_IO_BASE + (w << 2)];
    const uint16_t start = r[0] | (r[1] << 8);
    const uint16_t stop  = r[2] | (r[3] << 8);
    const int      on    = sock->card && (dev->reg[s][REG_WINEN] & (0x40 << w)) && (stop >= start);

    if (sock->io_len[w]) {
        io_removehandler(sock->io_base[w], sock->io_len[w],
                         pcic_io_readb, pcic_io_readw, NULL, pcic_io_writeb, pcic_io_writew, NULL, sock);
        sock->io_len[w] = 0;
    }
    if (on) {
        sock->io_base[w] = start;
        sock->io_len[w]  = stop - start + 1;
        io_sethandler(sock->io_base[w], sock->io_len[w],
                      pcic_io_readb, pcic_io_readw, NULL, pcic_io_writeb, pcic_io_writew, NULL, sock);
        pcic_log("PCIC: socket %c I/O window %d = %04X-%04X\n", 'A' + s, w, start, stop);
    }
}

/* --- Memory windows ------------------------------------------------------- */

/* The card address behind a host address: 1 for attribute memory, 0 for
   common memory, -1 when the card is not answering. */
static int
pcic_mem_card_addr(const pcic_win_t *win, uint32_t addr, uint32_t *caddr)
{
    const pcic_t  *dev = win->pcic;
    const uint8_t *r   = &dev->reg[win->socket][REG_MEM_BASE + (win->win << 3)];
    const uint32_t off = (r[4] | ((r[5] & 0x3f) << 8)) << 12;

    if (!dev->sock[win->socket].card || !dev->sock[win->socket].live)
        return -1;
    *caddr = (addr + off) & 0x3ffffff;
    return !!(r[5] & 0x40);
}

static uint8_t
pcic_mem_readb(uint32_t addr, void *priv)
{
    const pcic_win_t *win = (pcic_win_t *) priv;
    const pccard_t   *card;
    uint32_t          caddr;

    /* Only attribute memory: no card here has common memory. */
    if (pcic_mem_card_addr(win, addr, &caddr) != 1)
        return 0xff;
    card = win->pcic->sock[win->socket].card;
    return card->attr_read(caddr, card->priv);
}

static uint16_t
pcic_mem_readw(uint32_t addr, void *priv)
{
    return pcic_mem_readb(addr, priv) | (pcic_mem_readb(addr + 1, priv) << 8);
}

static void
pcic_mem_writeb(uint32_t addr, uint8_t val, void *priv)
{
    const pcic_win_t *win = (pcic_win_t *) priv;
    const uint8_t    *r   = &win->pcic->reg[win->socket][REG_MEM_BASE + (win->win << 3)];
    const pccard_t   *card;
    uint32_t          caddr;

    if (r[5] & 0x80) /* write-protected window */
        return;
    if (pcic_mem_card_addr(win, addr, &caddr) != 1)
        return;
    card = win->pcic->sock[win->socket].card;
    card->attr_write(caddr, val, card->priv);
}

static void
pcic_mem_writew(uint32_t addr, uint16_t val, void *priv)
{
    pcic_mem_writeb(addr, val & 0xff, priv);
    pcic_mem_writeb(addr + 1, val >> 8, priv);
}

static void
pcic_mem_remap(pcic_t *dev, int s, int w)
{
    pcic_socket_t *sock  = &dev->sock[s];
    const uint8_t *r     = &dev->reg[s][REG_MEM_BASE + (w << 3)];
    const uint32_t start = (r[0] | ((r[1] & 0x0f) << 8)) << 12;
    const uint32_t stop  = ((r[2] | ((r[3] & 0x0f) << 8)) << 12) | 0xfff;

    if (sock->card && (dev->reg[s][REG_WINEN] & (1 << w)) && (stop > start)) {
        mem_mapping_set_addr(&sock->mem[w], start, stop - start + 1);
        pcic_log("PCIC: socket %c memory window %d = %06X-%06X, %s memory\n", 'A' + s, w, start, stop,
                       (r[5] & 0x40) ? "attribute" : "common");
    } else
        mem_mapping_disable(&sock->mem[w]);
}

static void
pcic_remap_all(pcic_t *dev, int s)
{
    for (int w = 0; w < PCIC_IOWIN; w++)
        pcic_io_remap(dev, s, w);
    for (int w = 0; w < PCIC_MEMWIN; w++)
        pcic_mem_remap(dev, s, w);
}

/* --- Registers ------------------------------------------------------------ */

static uint8_t
pcic_reg_read(pcic_t *dev, int s, int r)
{
    const pcic_socket_t *sock = &dev->sock[s];
    uint8_t              ret;

    switch (r) {
        case REG_IDENT:
            return PD6722_IDENT;

        case REG_STATUS:
            /* No card: both card-detect bits clear, no power, not ready. */
            if (!sock->card)
                return 0x00;
            ret = STATUS_CD | STATUS_BVD;
            if (dev->reg[s][REG_POWER] & POWER_VCC)
                ret |= STATUS_POWER;
            if (sock->live)
                ret |= STATUS_READY;
            return ret;

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
                    pcic_log("PCIC: socket %c software interrupt on IRQ %d\n", 'A' + s, irq);
                }
            }
            dev->reg[s][r] = val;
            break;

        case REG_CSCINT:
            dev->reg[s][r] = val;
            if (!(val >> 4))
                pcic_irq_clear(dev);
            break;

        case REG_POWER:
        case REG_INTCTL:
            dev->reg[s][r] = val;
            pcic_card_power_update(dev, s);
            break;

        case REG_WINEN:
            dev->reg[s][r] = val;
            pcic_remap_all(dev, s);
            break;

        default:
            dev->reg[s][r] = val;
            if ((r >= REG_IO_BASE) && (r < REG_MEM_BASE))
                pcic_io_remap(dev, s, (r - REG_IO_BASE) >> 2);
            else if ((r >= REG_MEM_BASE) && (r < (REG_MEM_BASE + (PCIC_MEMWIN << 3))) && (((r - REG_MEM_BASE) & 7) < 6))
                pcic_mem_remap(dev, s, (r - REG_MEM_BASE) >> 3);
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

/* --- Cards ---------------------------------------------------------------- */

void
pcmcia_insert(int socket, const pccard_t *card)
{
    pcic_t *dev = pcic_inst;

    if ((socket < 0) || (socket >= PCIC_SOCKETS))
        return;

    pcic_pending[socket] = card;
    if (dev) {
        dev->sock[socket].card = card;
        dev->sock[socket].live = 0;
        dev->sock[socket].ireq = 0;
        pcic_remap_all(dev, socket);
        pcic_card_power_update(dev, socket);
    }
}

void
pcmcia_card_irq(int socket, int level)
{
    pcic_t *dev = pcic_inst;

    if (!dev || (socket < 0) || (socket >= PCIC_SOCKETS))
        return;

    dev->sock[socket].ireq = !!level;
    pcic_card_irq_update(dev, socket);
}

/* --- Device --------------------------------------------------------------- */

static void
pcic_reset(void *priv)
{
    pcic_t *dev = (pcic_t *) priv;

    pcic_irq_clear(dev);
    memset(dev->reg, 0, sizeof(dev->reg));
    memset(dev->ext, 0, sizeof(dev->ext));
    dev->index       = 0;
    dev->info_toggle = 0;

    /* Power off, RESET asserted, every window closed. */
    for (int s = 0; s < PCIC_SOCKETS; s++) {
        dev->sock[s].ireq = 0;
        pcic_remap_all(dev, s);
        pcic_card_power_update(dev, s);
    }
}

static void *
pcic_init(UNUSED(const device_t *info))
{
    pcic_t *dev = (pcic_t *) calloc(1, sizeof(pcic_t));

    for (int s = 0; s < PCIC_SOCKETS; s++) {
        dev->sock[s].card = pcic_pending[s];
        for (int w = 0; w < PCIC_MEMWIN; w++) {
            dev->sock[s].mem_win[w] = (pcic_win_t) { .pcic = dev, .socket = s, .win = w };
            mem_mapping_add(&dev->sock[s].mem[w], 0, 0,
                            pcic_mem_readb, pcic_mem_readw, NULL,
                            pcic_mem_writeb, pcic_mem_writew, NULL,
                            NULL, MEM_MAPPING_EXTERNAL, &dev->sock[s].mem_win[w]);
            mem_mapping_disable(&dev->sock[s].mem[w]);
        }
        if (dev->sock[s].card)
            pcic_log("PCIC: socket %c holds a card\n", 'A' + s);
    }

    io_sethandler(PCIC_BASE, 2, pcic_read, NULL, NULL, pcic_write, NULL, NULL, dev);
    pcic_inst = dev;
    return dev;
}

static void
pcic_close(void *priv)
{
    pcic_t *dev = (pcic_t *) priv;

    pcic_irq_clear(dev);
    for (int s = 0; s < PCIC_SOCKETS; s++) {
        if (dev->sock[s].irq_out)
            picintc(1 << dev->sock[s].irq_out);
        /* Close the windows before the card goes away. */
        dev->sock[s].card = NULL;
        pcic_remap_all(dev, s);
    }
    pcic_inst = NULL;
    free(dev);
}

static const device_t pcic_pd6722_device = {
    .name          = "Cirrus Logic CL-PD6722 PC Card controller",
    .internal_name = "pcic_pd6722",
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

/* One controller per machine, whoever asks for it first: the PCMCIA settings
   or an I/O board with PC Card slots. */
void
pcmcia_controller_add(void)
{
    if (pcic_inst == NULL)
        device_add(&pcic_pd6722_device);
}
