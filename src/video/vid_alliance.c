/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Alliance Semiconductor ProMotion emulation.
 *
 *          The ProMotion family (6410, 6412, 6422, and the AT24/AT3D/AT25
 *          that followed) is a VGA core with extended sequencer and CRTC
 *          registers, plus a block of "extended" registers (drawing engine,
 *          cursor, clocks, video window) that the chip maps either into the
 *          A0000h window (SR1B = 1) or at the end of the linear aperture
 *          (SR1B bit 2), and on the 6412 and later also reaches through an
 *          I/O window (SR1D index, base in SR1E/SR1F).
 *
 *          Register layout from Alliance's ProMotion Windows 95 display
 *          driver source (STB Horizon 64 Video, physical.h / physical.inc),
 *          the X.org apm driver (MIT) and the behaviour of the
 *          PhoenixVIEW "Alliance Semiconductor PM6410" video BIOS.
 *
 * Authors: Marcos Alves, <xeon4d@gmail.com>
 *
 *          Copyright 2026 Marcos Alves.
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
#include <86box/pci.h>
#include <86box/rom.h>
#include <86box/plat.h>
#include <86box/timer.h>
#include <86box/video.h>
#include <86box/vid_svga.h>
#include <86box/vid_svga_render.h>
#include <86box/plat_unused.h>
#include <86box/debug_cmd.h>

#define BIOS_PM6410_PCPARTNER "roms/video/alliance/pm6410/PCPartner-6410.BIN"

enum {
    PM_6410 = 0, /* "GXX", PCI 1142:3210 */
    PM_6422,     /* "GXS", PCI 1142:6422 */
};

/* Extended register block (offsets as the drivers use them). */
#define XR_CLIP_CTRL   0x030
#define XR_BLT_CTRL    0x040
#define XR_MEM_CONFIG  0x0c4
#define XR_DAC_CTRL    0x0c6
#define XR_PIN_IFACE   0x0ca
#define XR_SCRATCH     0x0f0
#define XR_CURS_CTRL   0x140
#define XR_STATUS      0x1fc
#define XR_SIZE        0x800

static video_timings_t timing_pm_pci = { .type = VIDEO_PCI, .write_b = 2, .write_w = 2, .write_l = 4, .read_b = 10, .read_w = 10, .read_l = 20 };

typedef struct pm_t {
    svga_t svga;
    rom_t  bios_rom;

    mem_mapping_t lfb_mapping; /* PCI BAR 0 */
    mem_mapping_t xr_mapping;  /* extended registers in the A0000h window */

    int      chip;
    void    *ramdac;
    uint32_t vram_size;
    uint32_t vram_mask;
    uint32_t lfb_base;
    uint32_t lfb_size;

    uint8_t pci_regs[256];
    uint8_t pci_slot;

    uint8_t sr[0x40]; /* SR08 and up */
    uint8_t cr[0x40]; /* CR19 and up */
    uint8_t xr[XR_SIZE];
} pm_t;

static int pm_do_log = 1;

static void
pm_log(const char *fmt, ...)
{
    va_list ap;

    if (pm_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}

static void pm_recalc_mappings(pm_t *pm);

/* ------------------------------------------------------------------------- */
/* Extended registers                                                        */
/* ------------------------------------------------------------------------- */

static void
pm_update_banks(pm_t *pm)
{
    svga_t  *svga = &pm->svga;
    uint32_t bank = (pm->xr[0xc0] | (pm->xr[0xc1] << 8)) << 12; /* 4 KB units */

    svga->write_bank = bank & pm->vram_mask;
    svga->read_bank  = svga->write_bank;
    /* A banked chain-4 window addresses video memory a byte at a time. */
    svga->packed_chain4 = !!bank;
}

/* The bank applies to the 32 KB windows at B0000h/B8000h too (the BIOS sizes
   memory through B8000h); the SVGA core only adds it for A0000h. */
static uint32_t
pm_translate_address(uint32_t addr, void *priv)
{
    const svga_t *svga = (svga_t *) priv;
    const pm_t   *pm   = (pm_t *) svga->priv;

    if ((svga->gdcreg[6] & 0x08) && svga->chain4 && svga->packed_chain4)
        addr += svga->write_bank;
    /* XRC4 bit 0 selects the 2 MB organisation, where address bit 16
       picks the second row of DRAM: absent on a 1 MB board (this is how
       the BIOS sizes memory). */
    if ((pm->xr[XR_MEM_CONFIG] & 0x01) && (pm->vram_size <= 0x100000) && (addr & 0x10000))
        return svga->vram_max;
    return addr;
}

static uint8_t
pm_xr_read(pm_t *pm, uint32_t addr)
{
    uint8_t ret;

    addr &= (XR_SIZE - 1);
    switch (addr) {
        case XR_STATUS:
            ret = 0x08; /* FIFO empty: eight free entries, engine idle */
            break;
        case XR_STATUS + 1:
        case XR_STATUS + 2:
        case XR_STATUS + 3:
            ret = 0x00;
            break;
        default:
            ret = pm->xr[addr];
            break;
    }
    if (pm_do_log >= 2 || (pm_do_log && addr != XR_STATUS && addr < 0x30))
        pm_log("PM: XR R %03X = %02X\n", addr, ret);
    return ret;
}

static void
pm_xr_write(pm_t *pm, uint32_t addr, uint8_t val)
{
    addr &= (XR_SIZE - 1);
    if (pm_do_log)
        pm_log("PM: XR W %03X = %02X\n", addr, val);
    pm->xr[addr] = val;

    switch (addr) {
        case 0x80:
            svga_recalctimings(&pm->svga);
            break;
        case 0xc0:
        case 0xc1:
            pm_update_banks(pm);
            break;
        default:
            break;
    }
}

static uint8_t
pm_xr_readb(uint32_t addr, void *priv)
{
    return pm_xr_read((pm_t *) priv, addr);
}

static uint16_t
pm_xr_readw(uint32_t addr, void *priv)
{
    pm_t *pm = (pm_t *) priv;

    return pm_xr_read(pm, addr) | (pm_xr_read(pm, addr + 1) << 8);
}

static uint32_t
pm_xr_readl(uint32_t addr, void *priv)
{
    return pm_xr_readw(addr, priv) | (pm_xr_readw(addr + 2, priv) << 16);
}

static void
pm_xr_writeb(uint32_t addr, uint8_t val, void *priv)
{
    pm_xr_write((pm_t *) priv, addr, val);
}

static void
pm_xr_writew(uint32_t addr, uint16_t val, void *priv)
{
    pm_t *pm = (pm_t *) priv;

    pm_xr_write(pm, addr, val & 0xff);
    pm_xr_write(pm, addr + 1, val >> 8);
}

static void
pm_xr_writel(uint32_t addr, uint32_t val, void *priv)
{
    pm_xr_writew(addr, val & 0xffff, priv);
    pm_xr_writew(addr + 2, val >> 16, priv);
}

/* ------------------------------------------------------------------------- */
/* Linear aperture                                                           */
/* ------------------------------------------------------------------------- */

static int
pm_lfb_is_xr(pm_t *pm, uint32_t addr)
{
    /* SR1B bit 2: the register block occupies the last 2 KB of the
       aperture (the 30 KB below it is the blit port). */
    return (pm->sr[0x1b - 8] & 0x04) && ((addr & (pm->lfb_size - 1)) >= (pm->lfb_size - XR_SIZE));
}

static uint8_t
pm_lfb_readb(uint32_t addr, void *priv)
{
    pm_t   *pm   = (pm_t *) priv;
    svga_t *svga = &pm->svga;

    addr -= pm->lfb_base;
    if (pm_lfb_is_xr(pm, addr))
        return pm_xr_read(pm, addr);
    addr &= svga->decode_mask;
    if (addr >= pm->vram_size)
        return 0xff;
    return svga->vram[addr];
}

static uint16_t
pm_lfb_readw(uint32_t addr, void *priv)
{
    return pm_lfb_readb(addr, priv) | (pm_lfb_readb(addr + 1, priv) << 8);
}

static uint32_t
pm_lfb_readl(uint32_t addr, void *priv)
{
    return pm_lfb_readw(addr, priv) | (pm_lfb_readw(addr + 2, priv) << 16);
}

static void
pm_lfb_writeb(uint32_t addr, uint8_t val, void *priv)
{
    pm_t   *pm   = (pm_t *) priv;
    svga_t *svga = &pm->svga;

    addr -= pm->lfb_base;
    if (pm_lfb_is_xr(pm, addr)) {
        pm_xr_write(pm, addr, val);
        return;
    }
    addr &= svga->decode_mask;
    if (addr >= pm->vram_size)
        return;
    svga->vram[addr]                   = val;
    svga->changedvram[addr >> 12] = svga->monitor->mon_changeframecount;
}

static void
pm_lfb_writew(uint32_t addr, uint16_t val, void *priv)
{
    pm_lfb_writeb(addr, val & 0xff, priv);
    pm_lfb_writeb(addr + 1, val >> 8, priv);
}

static void
pm_lfb_writel(uint32_t addr, uint32_t val, void *priv)
{
    pm_lfb_writew(addr, val & 0xffff, priv);
    pm_lfb_writew(addr + 2, val >> 16, priv);
}

static void
pm_recalc_mappings(pm_t *pm)
{
    svga_t *svga = &pm->svga;

    if (!(pm->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_MEM)) {
        mem_mapping_disable(&svga->mapping);
        mem_mapping_disable(&pm->lfb_mapping);
        mem_mapping_disable(&pm->xr_mapping);
        return;
    }

    switch (svga->gdcreg[6] & 0x0c) {
        case 0x0: /* 128k at A0000 */
            mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x20000);
            svga->banked_mask = 0xffff;
            break;
        case 0x4: /* 64k at A0000 */
            mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x10000);
            svga->banked_mask = 0xffff;
            break;
        case 0x8: /* 32k at B0000 */
            mem_mapping_set_addr(&svga->mapping, 0xb0000, 0x08000);
            svga->banked_mask = 0x7fff;
            break;
        case 0xC: /* 32k at B8000 */
            mem_mapping_set_addr(&svga->mapping, 0xb8000, 0x08000);
            svga->banked_mask = 0x7fff;
            break;
        default:
            break;
    }

    /* SR1C bit 3 turns VGA memory decoding off. */
    if (pm->sr[0x1c - 8] & 0x08)
        mem_mapping_disable(&svga->mapping);

    /* SR1B = 1 maps the register block over A0000h. */
    if ((pm->sr[0x1b - 8] & 0x07) == 0x01)
        mem_mapping_set_addr(&pm->xr_mapping, 0xa0000, 0x10000);
    else
        mem_mapping_disable(&pm->xr_mapping);

    pm->lfb_base = (pm->pci_regs[0x13] << 24) | (pm->pci_regs[0x12] << 16);
    pm->lfb_base &= ~(pm->lfb_size - 1);
    if (pm->lfb_base)
        mem_mapping_set_addr(&pm->lfb_mapping, pm->lfb_base, pm->lfb_size);
    else
        mem_mapping_disable(&pm->lfb_mapping);
}

/* ------------------------------------------------------------------------- */
/* VGA ports                                                                 */
/* ------------------------------------------------------------------------- */

/* The BIOS tells the chips apart by which extended sequencer registers hold
   a value: SR1D (the 6412 and later), SR23 (a 6410 revision 3). The first
   6410s implement SR10-SR1C only. */
static int
pm_sr_present(pm_t *pm, uint8_t idx)
{
    if (idx >= 0x40)
        return 0;
    if (pm->chip == PM_6410)
        return (idx >= 0x10) && (idx <= 0x1c);
    return 1;
}

static void
pm_out(uint16_t addr, uint8_t val, void *priv)
{
    pm_t   *pm   = (pm_t *) priv;
    svga_t *svga = &pm->svga;
    uint8_t old;

    if (((addr & 0xfff0) == 0x3d0 || (addr & 0xfff0) == 0x3b0) && !(svga->miscout & 1))
        addr ^= 0x60;

    switch (addr) {
        case 0x3c5:
            if (svga->seqaddr >= 0x08) {
                const uint8_t idx = svga->seqaddr & 0x3f;

                if (!pm_sr_present(pm, svga->seqaddr))
                    return;
                pm_log("PM: SR%02X = %02X\n", svga->seqaddr, val);
                old             = pm->sr[idx - 8];
                pm->sr[idx - 8] = val;
                switch (idx) {
                    case 0x1b:
                    case 0x1c:
                        if (old != val) {
                            pm_recalc_mappings(pm);
                            svga_recalctimings(svga);
                        }
                        break;
                    default:
                        break;
                }
                return;
            }
            break;

        case 0x3c6:
        case 0x3c7:
        case 0x3c8:
        case 0x3c9:
            if (pm->ramdac) {
                att498_ramdac_out(addr, 0, val, pm->ramdac, svga);
                return;
            }
            break;

        case 0x3cf:
            if (svga->gdcaddr == 6) {
                old             = svga->gdcreg[6];
                svga_out(addr, val, svga);
                if ((old ^ val) & 0x0c)
                    pm_recalc_mappings(pm);
                return;
            }
            if (svga->gdcaddr > 8) {
                pm_log("PM: GR%02X = %02X\n", svga->gdcaddr, val);
                return;
            }
            break;

        case 0x3d4:
            svga->crtcreg = val & 0x3f;
            return;
        case 0x3d5:
            if (svga->crtcreg >= 0x19) {
                pm_log("PM: CR%02X = %02X\n", svga->crtcreg, val);
                old                          = pm->cr[svga->crtcreg];
                pm->cr[svga->crtcreg]        = val;
                if (old != val) {
                    svga->fullchange = svga->monitor->mon_changeframecount;
                    svga_recalctimings(svga);
                }
                return;
            }
            if ((svga->crtcreg < 7) && (svga->crtc[0x11] & 0x80))
                return;
            if ((svga->crtcreg == 7) && (svga->crtc[0x11] & 0x80))
                val = (svga->crtc[7] & ~0x10) | (val & 0x10);
            old                       = svga->crtc[svga->crtcreg];
            svga->crtc[svga->crtcreg] = val;
            if (old != val) {
                if ((svga->crtcreg < 0x0e) || (svga->crtcreg > 0x10)) {
                    if ((svga->crtcreg == 0x0c) || (svga->crtcreg == 0x0d)) {
                        svga->fullchange    = 3;
                        svga->memaddr_latch = ((svga->crtc[0xc] << 8) | svga->crtc[0xd]) + ((svga->crtc[8] & 0x60) >> 5);
                    } else {
                        svga->fullchange = svga->monitor->mon_changeframecount;
                        svga_recalctimings(svga);
                    }
                }
            }
            return;

        default:
            break;
    }

    svga_out(addr, val, svga);
}

static uint8_t
pm_in(uint16_t addr, void *priv)
{
    pm_t   *pm   = (pm_t *) priv;
    svga_t *svga = &pm->svga;
    uint8_t ret;

    if (((addr & 0xfff0) == 0x3d0 || (addr & 0xfff0) == 0x3b0) && !(svga->miscout & 1))
        addr ^= 0x60;

    switch (addr) {
        case 0x3c5:
            if (svga->seqaddr >= 0x08) {
                /* The first 6410s read SR20-SR2F from the scratch
                   registers XRF0-XRFF (the BIOS keeps the memory size and
                   DAC type there); writes do not reach them this way. */
                if ((pm->chip == PM_6410) && (svga->seqaddr >= 0x20) && (svga->seqaddr < 0x30))
                    return pm->xr[XR_SCRATCH + svga->seqaddr - 0x20];
                if (!pm_sr_present(pm, svga->seqaddr))
                    return 0xff;
                ret = pm->sr[(svga->seqaddr & 0x3f) - 8];
                pm_log("PM: SR%02X R %02X\n", svga->seqaddr, ret);
                return ret;
            }
            break;

        case 0x3c6:
        case 0x3c7:
        case 0x3c8:
        case 0x3c9:
            if (pm->ramdac)
                return att498_ramdac_in(addr, 0, pm->ramdac, svga);
            break;

        case 0x3cf:
            if (svga->gdcaddr > 8) {
                pm_log("PM: GR%02X R\n", svga->gdcaddr);
                return 0xff;
            }
            break;

        case 0x3d4:
            return svga->crtcreg;
        case 0x3d5:
            if (svga->crtcreg >= 0x19) {
                ret = pm->cr[svga->crtcreg];
                pm_log("PM: CR%02X R %02X\n", svga->crtcreg, ret);
                return ret;
            }
            return svga->crtc[svga->crtcreg];

        default:
            break;
    }

    return svga_in(addr, svga);
}

/* ------------------------------------------------------------------------- */
/* Display                                                                   */
/* ------------------------------------------------------------------------- */

/* XR80 bits 3:0, the pixel format of the extended (packed) modes. */
static int
pm_ext_bpp(pm_t *pm)
{
    switch (pm->xr[0x80] & 0x0f) {
        case 0x02:
            return 8;
        case 0x0c:
            return 15;
        case 0x0d:
            return 16;
        case 0x0e:
            return 24;
        case 0x0f:
            return 32;
        default:
            return 0;
    }
}

static void
pm_recalctimings(svga_t *svga)
{
    pm_t *pm  = (pm_t *) svga->priv;
    int   bpp = pm_ext_bpp(pm);

    svga->force_old_addr = 0;
    if (!bpp)
        return;

    /* Extended modes: start address in dwords (CR1C bits 3:0 are bits
       19:16), offset in 8-byte units (CR1C bits 7:4 are bits 11:8). */
    svga->force_old_addr = 1;
    svga->memaddr_latch  = (((pm->cr[0x1c] & 0x0f) << 16) | (svga->crtc[0x0c] << 8) | svga->crtc[0x0d]) << 2;
    svga->rowoffset      = svga->crtc[0x13] | ((pm->cr[0x1c] & 0xf0) << 4);
    svga->lowres         = 0;

    /* An external RAMDAC (the 6410's) assembles wider pixels itself. */
    if (pm->ramdac && (bpp == 8) && (svga->bpp > 8))
        bpp = svga->bpp;

    if ((svga->miscout & 0x0c) == 0x0c) {
        /* TODO: the programmable/external clock; until it is known, run
           the mode at 60 Hz. */
        const double hz = (double) (svga->htotal * 8) * (double) svga->vtotal * 60.0;

        if (hz > 0.0)
            svga->clock = (cpuclock * (double) (1ULL << 32)) / hz;
    }

    switch (bpp) {
        case 15:
            svga->render = svga_render_15bpp_highres;
            break;
        case 16:
            svga->render = svga_render_16bpp_highres;
            break;
        case 24:
            svga->render = svga_render_24bpp_highres;
            break;
        case 32:
            svga->render = svga_render_32bpp_highres;
            break;
        default:
            svga->map8   = svga->pallook;
            svga->render = svga_render_8bpp_highres;
            break;
    }
}

/* ------------------------------------------------------------------------- */
/* PCI                                                                       */
/* ------------------------------------------------------------------------- */

static uint8_t
pm_pci_read(UNUSED(int func), int addr, UNUSED(int len), void *priv)
{
    pm_t *pm = (pm_t *) priv;

    if (func)
        return 0xff;
    return pm->pci_regs[addr];
}

static void
pm_pci_write(UNUSED(int func), int addr, UNUSED(int len), uint8_t val, void *priv)
{
    pm_t *pm = (pm_t *) priv;

    if (func)
        return;

    switch (addr) {
        case PCI_REG_COMMAND:
            pm->pci_regs[addr] = val & 0x23;
            pm_recalc_mappings(pm);
            break;
        case 0x07:
            pm->pci_regs[addr] &= ~(val & 0xf8);
            break;
        case 0x0c:
        case 0x0d:
            pm->pci_regs[addr] = val;
            break;

        case 0x12:
        case 0x13:
            pm->pci_regs[addr] = val;
            if (addr == 0x12)
                pm->pci_regs[addr] &= ~((pm->lfb_size - 1) >> 16);
            pm_recalc_mappings(pm);
            break;

        case 0x30:
        case 0x32:
        case 0x33:
            pm->pci_regs[addr] = (addr == 0x30) ? (val & 0x01) : val;
            if (pm->pci_regs[0x30] & 0x01) {
                uint32_t biosaddr = (pm->pci_regs[0x32] << 16) | (pm->pci_regs[0x33] << 24);
                mem_mapping_set_addr(&pm->bios_rom.mapping, biosaddr, 0x8000);
            } else
                mem_mapping_disable(&pm->bios_rom.mapping);
            break;

        case 0x3c:
            pm->pci_regs[addr] = val;
            break;

        default:
            break;
    }
}

static void
pm_init_pci_regs(pm_t *pm)
{
    memset(pm->pci_regs, 0, sizeof(pm->pci_regs));

    pm->pci_regs[0x00] = 0x42; /* Alliance Semiconductor */
    pm->pci_regs[0x01] = 0x11;
    if (pm->chip == PM_6422) {
        pm->pci_regs[0x02] = 0x22;
        pm->pci_regs[0x03] = 0x64;
    } else {
        pm->pci_regs[0x02] = 0x10;
        pm->pci_regs[0x03] = 0x32;
    }
    pm->pci_regs[0x06] = 0x00;
    pm->pci_regs[0x07] = 0x02; /* DEVSEL medium */
    pm->pci_regs[0x08] = 0x00; /* revision */
    pm->pci_regs[0x0b] = 0x03; /* VGA compatible display controller */
    pm->pci_regs[0x10] = 0x00; /* memory, 32-bit, not prefetchable */
    pm->pci_regs[0x32] = 0x0c;
    pm->pci_regs[0x3d] = PCI_INTA;
}

/* ------------------------------------------------------------------------- */
/* Debug hook ("dev pm ..." in the BOX86NEXT_DEBUG_CMD file)                 */
/* ------------------------------------------------------------------------- */

static pm_t *pm_dbg;

static void
pm_debug_hook(const char *args)
{
    pm_t   *pm = pm_dbg;
    svga_t *svga;
    char    line[512];
    int     n;

    if (!pm)
        return;
    svga = &pm->svga;

    if (!strncmp(args, "pm", 2))
        args += 2;
    while (*args == ' ')
        args++;

    if (!strncmp(args, "log ", 4)) {
        pm_do_log = atoi(args + 4);
        pclog("PM: log %i\n", pm_do_log);
        return;
    }

    n = 0;
    for (int i = 0x08; i < 0x30; i++)
        n += sprintf(line + n, "%02X ", pm->sr[i - 8]);
    pclog("PM: SR08.. %s\n", line);
    n = 0;
    for (int i = 0x19; i < 0x20; i++)
        n += sprintf(line + n, "%02X ", pm->cr[i]);
    pclog("PM: CR19.. %s\n", line);
    n = 0;
    for (int i = 0; i < 0x19; i++)
        n += sprintf(line + n, "%02X ", svga->crtc[i]);
    pclog("PM: crtc %s\n", line);
    pclog("PM: misc %02X seq %02X %02X %02X %02X %02X gdc5/6 %02X %02X ar10 %02X\n",
          svga->miscout, svga->seqregs[0], svga->seqregs[1], svga->seqregs[2], svga->seqregs[3],
          svga->seqregs[4], svga->gdcreg[5], svga->gdcreg[6], svga->attrregs[0x10]);
    pclog("PM: hdisp %i htotal %i dispend %i vtotal %i rowoff %i ma %06X bpp %i lfb %08X\n",
          svga->hdisp, svga->htotal, svga->dispend, svga->vtotal, svga->rowoffset, svga->memaddr_latch,
          svga->bpp, pm->lfb_base);
    for (int row = 0; row < 0x200; row += 0x20) {
        n = 0;
        for (int i = row; i < row + 0x20; i++)
            n += sprintf(line + n, "%02X", pm->xr[i]);
        pclog("PM: XR%03X %s\n", row, line);
    }
}

/* ------------------------------------------------------------------------- */
/* Device                                                                    */
/* ------------------------------------------------------------------------- */

static void
pm_reset_state(pm_t *pm)
{
    memset(pm->sr, 0, sizeof(pm->sr));
    memset(pm->cr, 0, sizeof(pm->cr));
    memset(pm->xr, 0, sizeof(pm->xr));
    pm->xr[XR_PIN_IFACE] = 0x01; /* PCI bus */
    pm_update_banks(pm);
    /* The BIOS sizes memory before its first mode set and needs writes to
       land: start with all bits of the bit mask enabled. */
    pm->svga.gdcreg[8] = 0xff;
}

static void *
pm_init(const device_t *info)
{
    pm_t   *pm   = calloc(1, sizeof(pm_t));
    svga_t *svga = &pm->svga;

    pm->chip      = info->local;
    pm->vram_size = device_get_config_int("memory") << 20;
    pm->vram_mask = pm->vram_size - 1;
    pm->lfb_size  = 0x400000;

    rom_init(&pm->bios_rom, BIOS_PM6410_PCPARTNER, 0xc0000, 0x8000, 0x7fff, 0, MEM_MAPPING_EXTERNAL);
    mem_mapping_disable(&pm->bios_rom.mapping);

    video_inform(VIDEO_FLAG_TYPE_SPECIAL, &timing_pm_pci);

    svga_init(info, svga, pm, pm->vram_size,
              pm_recalctimings,
              pm_in, pm_out,
              NULL,
              NULL);
    svga->decode_mask       = pm->lfb_size - 1;

    /* The 6410 drives an external RAMDAC: an AT&T 20C498, which the BIOS
       knows as its 16-bit DAC (type 81h). */
    if (pm->chip == PM_6410)
        pm->ramdac = device_add(&att498_ramdac_device);
    svga->vram_display_mask = pm->vram_mask;
    svga->translate_address = pm_translate_address;

    mem_mapping_add(&pm->lfb_mapping, 0, 0,
                    pm_lfb_readb, pm_lfb_readw, pm_lfb_readl,
                    pm_lfb_writeb, pm_lfb_writew, pm_lfb_writel,
                    NULL, MEM_MAPPING_EXTERNAL, pm);
    mem_mapping_add(&pm->xr_mapping, 0, 0,
                    pm_xr_readb, pm_xr_readw, pm_xr_readl,
                    pm_xr_writeb, pm_xr_writew, pm_xr_writel,
                    NULL, MEM_MAPPING_EXTERNAL, pm);
    mem_mapping_disable(&pm->lfb_mapping);
    mem_mapping_disable(&pm->xr_mapping);

    pm_init_pci_regs(pm);
    pm_reset_state(pm);

    io_sethandler(0x03c0, 0x0020, pm_in, NULL, NULL, pm_out, NULL, NULL, pm);
    pm->pci_regs[PCI_REG_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEM;
    pm_recalc_mappings(pm);

    pci_add_card(PCI_ADD_NORMAL, pm_pci_read, pm_pci_write, pm, &pm->pci_slot);

    pm_dbg = pm;
    debug_cmd_set_device_hook(pm_debug_hook);

    return pm;
}

static void
pm_reset(void *priv)
{
    pm_t *pm = (pm_t *) priv;

    pm_init_pci_regs(pm);
    pm_reset_state(pm);
    pm->pci_regs[PCI_REG_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEM;
    pm_recalc_mappings(pm);
    mem_mapping_disable(&pm->bios_rom.mapping);
    svga_recalctimings(&pm->svga);
}

static int
pm6410_available(void)
{
    return rom_present(BIOS_PM6410_PCPARTNER);
}

static void
pm_close(void *priv)
{
    pm_t *pm = (pm_t *) priv;

    if (pm_dbg == pm) {
        debug_cmd_set_device_hook(NULL);
        pm_dbg = NULL;
    }
    svga_close(&pm->svga);
    free(pm);
}

static void
pm_speed_changed(void *priv)
{
    pm_t *pm = (pm_t *) priv;

    svga_recalctimings(&pm->svga);
}

static void
pm_force_redraw(void *priv)
{
    pm_t *pm = (pm_t *) priv;

    pm->svga.fullchange = pm->svga.monitor->mon_changeframecount;
}

static const device_config_t pm6410_config[] = {
    {
        .name           = "memory",
        .description    = "Memory size",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 2,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "1 MB", .value = 1 },
            { .description = "2 MB", .value = 2 },
            { .description = ""                 }
        },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
};

const device_t pm6410_pci_device = {
    .name          = "Alliance ProMotion 6410 PCI",
    .internal_name = "pm6410_pci",
    .flags         = DEVICE_PCI,
    .local         = PM_6410,
    .init          = pm_init,
    .close         = pm_close,
    .reset         = pm_reset,
    .available     = pm6410_available,
    .speed_changed = pm_speed_changed,
    .force_redraw  = pm_force_redraw,
    .config        = pm6410_config
};
