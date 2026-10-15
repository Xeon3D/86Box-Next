/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          S3 Savage3D (86C390/86C391) emulation: the STB Nitro 3200.
 *
 *          The VGA core and the S3 extended CRTC/sequencer registers follow
 *          the ViRGE line; the 2D engine is the Burst Command Interface (BCI):
 *          command packets written to a FIFO window in the MMIO space.
 *
 *          PCI BAR 0 is a 128 MB window:
 *            0x0000000 - 0x0FFFFFF  linear frame buffer (8 MB of SGRAM)
 *            0x1000000 - 0x107FFFF  MMIO: 0x8000 + port = VGA registers,
 *                                   0x8168/0x816C global bitmap descriptor,
 *                                   0x8180-0x83FF streams processor,
 *                                   0x10000-0x2FFFF BCI FIFO,
 *                                   0x48C00 status, 0x48C40 tiled surfaces
 *            0x2000000 - 0x7FFFFFF  six 16 MB tiled apertures (8 KB stride)
 *
 *          Tiled memory (16 and 32 bpp desktops of the S3 drivers): 2 KB tiles
 *          of 16 lines of 128 bytes, laid out row-major; the tile row length
 *          comes from the tiled surface register, the primary stream stride
 *          register or the bitmap descriptor.
 *
 *          3D is not emulated.
 *
 * Authors: Marcos Alves, <xeon4d@gmail.com>
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdbool.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include <86box/io.h>
#include <86box/timer.h>
#include <86box/mem.h>
#include <86box/pci.h>
#include <86box/rom.h>
#include <86box/device.h>
#include <86box/plat.h>
#include <86box/video.h>
#include <86box/i2c.h>
#include <86box/vid_ddc.h>
#include <86box/vid_svga.h>
#include <86box/vid_svga_render.h>
#include <86box/debug_cmd.h>

#define lookup_lut(val) svga_lookup_lut_ram(svga, val)

#define ROM_STB_NITRO_3200 "roms/video/s3savage/n32_110.rom"

#define SAVAGE_VRAM_SIZE   (8 << 20)
#define SAVAGE_VRAM_MASK   (SAVAGE_VRAM_SIZE - 1)

#define BAR0_SIZE          0x08000000
#define BAR0_MMIO          0x01000000
#define BAR0_MMIO_SIZE     0x00080000
#define BAR0_APERTURE      0x02000000
#define BAR0_APERTURE_SIZE 0x06000000

/* Status word 0: bits 17-22 are the engine idle flags, bits 0-16 the FIFO count. */
#define STATUS0_IDLE 0x007e0000

#define SERIAL_PORT_SCW (1 << 0)
#define SERIAL_PORT_SDW (1 << 1)
#define SERIAL_PORT_SCR (1 << 2)
#define SERIAL_PORT_SDR (1 << 3)

/* BCI 2D command bits. */
#define BCI_CS           0x00008000 /* colors follow */
#define BCI_SRC_TRANSP   0x00000200
#define BCI_PAT_TRANSP   0x00000010
#define BCI_X_POSITIVE   0x01000000
#define BCI_Y_POSITIVE   0x02000000

enum {
    BD_NONE = -1,
    BD_GBD  = 0,
    BD_PBD  = 1,
    BD_SBD  = 2
};

enum {
    BCI_IDLE = 0,    /* expecting a command */
    BCI_PARAMS,      /* collecting the packet's parameters */
    BCI_HOSTDATA,    /* host source data of a rectangle */
    BCI_TEXT_XY,     /* text: next glyph's destination (or a new command) */
    BCI_TEXT_HEADER, /* text: glyph width/height */
    BCI_TEXT_DATA,   /* text: glyph bits */
    BCI_SETREG,      /* register writes */
    BCI_3DDATA,      /* vertices of a 3D primitive (not drawn) */
    BCI_SKIP         /* unknown command: swallow dwords until the next command */
};

/* Parameter slots in the order the drivers send them. */
enum {
    P_SRC_BD_LO,
    P_SRC_BD_HI,
    P_DST_BD_LO,
    P_DST_BD_HI,
    P_PAT_BD_LO,
    P_PAT_BD_HI,
    P_CLIP_LR,
    P_CLIP_TL,
    P_CLIP_BR,
    P_COLOR0,
    P_COLOR1,
    P_SRC_XY,
    P_DST_XY,
    P_WH,
    P_LINE_K,
    P_LINE_E,
    P_PAT0,
    P_PAT1,
    P_MAX
};

typedef struct savage_bd_t {
    uint32_t offset;
    uint32_t hi; /* stride (pixels) 15:0, bpp 23:16, tile format 25:24 */
} savage_bd_t;

typedef struct savage_t {
    svga_t svga;

    rom_t         bios_rom;
    mem_mapping_t linear_mapping;
    mem_mapping_t mmio_mapping;
    mem_mapping_t aperture_mapping;
    mem_mapping_t old_mmio_mapping;

    uint8_t  pci_regs[256];
    uint8_t  pci_slot;
    uint8_t  irq_state;
    int      is_agp;
    uint16_t device_id;
    uint32_t bar0;

    uint8_t bank;
    uint8_t ma_ext;

    int      hwc_fg_col;
    int      hwc_bg_col;
    int      hwc_col_stack_pos;
    uint8_t  serialport;
    void    *i2c;
    void    *ddc;

    struct {
        int    n, m, r;
        double freq;
    } clock;

    /* Streams processor, MMIO 0x8180-0x83ff. */
    uint32_t streams[0x80];

    /* BCI / memory port interface. */
    uint32_t write_mask;
    uint32_t read_mask;
    uint32_t bci_ctrl;     /* MM48C18 */
    uint32_t shadow_ctrl;  /* MM48C0C */
    uint32_t buf_thresh;   /* MM48C10 */
    uint32_t cob;          /* MM48C14 */
    uint32_t event_tag;
    uint32_t tiled_surf[6];
    uint32_t regs3d[256];

    /* The last BCI dwords, for the debug command file's "dev". */
    uint32_t bci_hist[256];
    uint8_t  bci_hist_state[256];
    uint32_t bci_hist_pos;

    savage_bd_t gbd, pbd, sbd;

    struct {
        int      state;
        uint32_t cmd;
        int      slots[P_MAX];
        int      nslots;
        int      slot;
        int      first_coord; /* slot a repeated packet restarts at */
        uint32_t p[P_MAX];

        int      clip_l, clip_t, clip_r, clip_b;

        /* Host data of the current rectangle or glyph. */
        int      x, y, w, h;
        int      dwords_per_line;
        int      line, dword;
        int      mono;
        uint32_t line_buf[2048];

        /* Text glyphs. */
        int      glyph_x, glyph_y, glyph_w, glyph_h;
        int      glyph_bitpacked;
        uint32_t glyph_dwords, glyph_pos;
        uint8_t  glyph_buf[8192];

        int      setreg_index, setreg_count;
        int      skip_count;
    } bci;
} savage_t;

#ifdef ENABLE_S3_SAVAGE_LOG
int s3_savage_do_log = ENABLE_S3_SAVAGE_LOG;

static void
s3_savage_log(const char *fmt, ...)
{
    va_list ap;

    if (s3_savage_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define s3_savage_log(fmt, ...)
#endif

static video_timings_t timing_savage_agp = { .type = VIDEO_AGP, .write_b = 2, .write_w = 2, .write_l = 3, .read_b = 28, .read_w = 28, .read_l = 45 };
static video_timings_t timing_savage_pci = { .type = VIDEO_PCI, .write_b = 2, .write_w = 2, .write_l = 3, .read_b = 28, .read_w = 28, .read_l = 45 };

static void    s3_savage_updatemapping(savage_t *dev);
static void    s3_savage_bci_write(savage_t *dev, uint32_t val);
static uint8_t s3_savage_in(uint16_t addr, void *priv);
static void    s3_savage_out(uint16_t addr, uint8_t val, void *priv);

/* ------------------------------------------------------------------------ */
/* Tiled memory                                                             */
/* ------------------------------------------------------------------------ */

/* Address of byte xb on line y of a tiled surface with tiles_per_row tiles. */
static inline uint32_t
savage_tiled_addr(uint32_t base, int tiles_per_row, uint32_t xb, uint32_t y)
{
    return base + ((y >> 4) * tiles_per_row + (xb >> 7)) * 2048 + ((y & 15) << 7) + (xb & 127);
}

static uint32_t
savage_aperture_addr(savage_t *dev, uint32_t addr)
{
    uint32_t surf  = addr >> 24;
    uint32_t reg   = (surf < 6) ? dev->tiled_surf[surf] : 0;
    int      tiles = (reg >> 24) & 0x3f;
    uint32_t y     = (addr >> 13) & 0x7ff;
    uint32_t xb    = addr & 0x1fff;

    if (!tiles)
        tiles = 1;

    return savage_tiled_addr(reg & 0xffffff, tiles, xb, y) & SAVAGE_VRAM_MASK;
}

static uint8_t
s3_savage_aperture_read(uint32_t addr, void *priv)
{
    savage_t *dev = (savage_t *) priv;

    return dev->svga.vram[savage_aperture_addr(dev, (addr - dev->bar0 - BAR0_APERTURE))];
}

static uint16_t
s3_savage_aperture_readw(uint32_t addr, void *priv)
{
    return s3_savage_aperture_read(addr, priv) | (s3_savage_aperture_read(addr + 1, priv) << 8);
}

static uint32_t
s3_savage_aperture_readl(uint32_t addr, void *priv)
{
    savage_t *dev = (savage_t *) priv;
    uint32_t  off = addr - dev->bar0 - BAR0_APERTURE;

    if ((off & 3) == 0)
        return *(uint32_t *) &dev->svga.vram[savage_aperture_addr(dev, off)];

    return s3_savage_aperture_readw(addr, priv) | (s3_savage_aperture_readw(addr + 2, priv) << 16);
}

static void
s3_savage_aperture_write(uint32_t addr, uint8_t val, void *priv)
{
    savage_t *dev = (savage_t *) priv;
    uint32_t  va  = savage_aperture_addr(dev, addr - dev->bar0 - BAR0_APERTURE);

    dev->svga.vram[va]                = val;
    dev->svga.changedvram[va >> 12] = changeframecount;
}

static void
s3_savage_aperture_writew(uint32_t addr, uint16_t val, void *priv)
{
    s3_savage_aperture_write(addr, val & 0xff, priv);
    s3_savage_aperture_write(addr + 1, val >> 8, priv);
}

static void
s3_savage_aperture_writel(uint32_t addr, uint32_t val, void *priv)
{
    savage_t *dev = (savage_t *) priv;
    uint32_t  off = addr - dev->bar0 - BAR0_APERTURE;

    if ((off & 3) == 0) {
        uint32_t va = savage_aperture_addr(dev, off);

        *(uint32_t *) &dev->svga.vram[va] = val;
        dev->svga.changedvram[va >> 12] = changeframecount;
    } else {
        s3_savage_aperture_writew(addr, val & 0xffff, priv);
        s3_savage_aperture_writew(addr + 2, val >> 16, priv);
    }
}

/* ------------------------------------------------------------------------ */
/* Display                                                                  */
/* ------------------------------------------------------------------------ */

static int
savage_primary_mmio(savage_t *dev)
{
    return !!(dev->svga.crtc[0x69] & 0x80);
}

static uint32_t
savage_primary_stride(savage_t *dev)
{
    return dev->streams[(0x81c8 - 0x8180) >> 2];
}

static void
savage_render_tiled(svga_t *svga, int bytespp)
{
    savage_t *dev    = (savage_t *) svga->priv;
    uint32_t  stride = savage_primary_stride(dev);
    int       tiles  = (stride >> 24) & 0x3f;
    uint32_t  base   = dev->streams[((dev->streams[(0x81cc - 0x8180) >> 2] & 1) ? (0x81c4 - 0x8180) : (0x81c0 - 0x8180)) >> 2] & 0x7ffff8;
    int       y      = svga->displine;
    uint32_t *p;

    if (((svga->displine + svga->y_add) < 0) || (svga->monitor->target_buffer == NULL) ||
        (svga->monitor->target_buffer->line[svga->displine + svga->y_add] == NULL))
        return;

    if (!tiles)
        tiles = 1;

    if (svga->interlace)
        y = (y << 1) + svga->oddeven;

    p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

    if (svga->firstline_draw == 2000)
        svga->firstline_draw = svga->displine;
    svga->lastline_draw = svga->displine;

    for (int x = 0; x <= (svga->hdisp + svga->scrollcache); x++) {
        uint32_t va = savage_tiled_addr(base, tiles, x * bytespp, y) & SAVAGE_VRAM_MASK;

        /* Past the tile row's end the next tile row would show. */
        if ((x * bytespp) >= (tiles << 7))
            p[x] = 0;
        else if (bytespp == 2)
            p[x] = svga->conv_16to32(svga, *(uint16_t *) &svga->vram[va], (svga->bpp == 15) ? 15 : 16);
        else
            p[x] = lookup_lut(*(uint32_t *) &svga->vram[va] & 0xffffff);
    }
}

static void
savage_render_tiled_16bpp(svga_t *svga)
{
    savage_render_tiled(svga, 2);
}

static void
savage_render_tiled_32bpp(svga_t *svga)
{
    savage_render_tiled(svga, 4);
}

static void
s3_savage_recalctimings(svga_t *svga)
{
    savage_t *dev = (savage_t *) svga->priv;

    svga_set_ramdac_type(svga, (svga->seqregs[0x1b] & 0x10) ? RAMDAC_8BIT : RAMDAC_6BIT);
    svga->lut_map = !!(svga->seqregs[0x1b] & 0x08);

    if (!svga->scrblank) {
        if (svga->crtc[0x43] & 0x80)
            svga->dots_per_clock = ((svga->seqregs[1] & 1) ? 16 : 18);

        if (svga->crtc[0x3a] & 0x10)
            svga->dots_per_clock = (svga->seqregs[1] & 8) ? 16 : 8;
    }

    svga->htotal = svga->crtc[0] + ((svga->crtc[0x5d] & 0x01) ? 0x100 : 0) + 5;

    svga->hdisp = svga->crtc[1] + ((svga->crtc[0x5d] & 0x02) ? 0x100 : 0);
    if (svga->crtc[1] & 0x01)
        svga->hdisp++;
    svga->hdisp_time = svga->hdisp;
    svga->hdisp *= svga->dots_per_clock;

    svga->vtotal = svga->crtc[6];
    if (svga->crtc[7] & 0x01)
        svga->vtotal |= 0x100;
    if (svga->crtc[7] & 0x20)
        svga->vtotal |= 0x200;
    if (svga->crtc[0x5e] & 0x01)
        svga->vtotal |= 0x400;
    svga->vtotal += 2;

    svga->dispend = svga->crtc[0x12];
    if (svga->crtc[7] & 0x02)
        svga->dispend |= 0x100;
    if (svga->crtc[7] & 0x40)
        svga->dispend |= 0x200;
    if (svga->crtc[0x5e] & 0x02)
        svga->dispend |= 0x400;
    svga->dispend++;

    svga->vblankstart = svga->crtc[0x15];
    if (svga->crtc[7] & 0x08)
        svga->vblankstart |= 0x100;
    if (svga->crtc[9] & 0x20)
        svga->vblankstart |= 0x200;
    if (svga->crtc[0x5e] & 0x04)
        svga->vblankstart |= 0x400;
    svga->vblankstart++;

    svga->vsyncstart = svga->crtc[0x10];
    if (svga->crtc[7] & 0x04)
        svga->vsyncstart |= 0x100;
    if (svga->crtc[7] & 0x80)
        svga->vsyncstart |= 0x200;
    if (svga->crtc[0x5e] & 0x10)
        svga->vsyncstart |= 0x400;
    svga->vsyncstart++;

    svga->split = svga->crtc[0x18];
    if (svga->crtc[7] & 0x10)
        svga->split |= 0x100;
    if (svga->crtc[9] & 0x40)
        svga->split |= 0x200;
    if (svga->crtc[0x5e] & 0x40)
        svga->split |= 0x400;
    svga->split++;

    svga->interlace = !!(svga->crtc[0x42] & 0x20);

    /* DCLK PLL: SR12 = R[1:0] N[5:0], SR13 = M[7:0], SR29 bit 2 = R[2]. */
    if (((svga->miscout >> 2) & 3) == 3) {
        dev->clock.n    = svga->seqregs[0x12] & 0x3f;
        dev->clock.r    = ((svga->seqregs[0x12] >> 6) & 3) | (svga->seqregs[0x29] & 0x04);
        dev->clock.m    = svga->seqregs[0x13] | ((svga->seqregs[0x29] & 0x08) << 5);
        dev->clock.freq = (((double) dev->clock.m + 2) / (((double) dev->clock.n + 2) * (double) (1 << dev->clock.r))) * 14318184.0;
        svga->clock     = (cpuclock * (float) (1ULL << 32)) / dev->clock.freq;
    }

    svga->hblankstart     = (((svga->crtc[0x5d] & 0x04) >> 2) << 8) + svga->crtc[2];
    svga->hblank_end_val  = (svga->crtc[3] & 0x1f) | (((svga->crtc[5] & 0x80) >> 7) << 5) |
                            (((svga->crtc[0x5d] & 0x08) >> 3) << 6);
    svga->hblank_end_mask = 0x7f;

    svga->memaddr_latch |= (dev->ma_ext << 16);
    if (svga->crtc[0x51] & 0x70)
        svga->rowoffset |= (svga->crtc[0x51] & 0x70) << 4;
    else if (svga->crtc[0x43] & 0x04)
        svga->rowoffset |= 0x100;

    svga->vram_display_mask = SAVAGE_VRAM_MASK;

    if ((svga->gdcreg[5] & 0x40) && (svga->crtc[0x3a] & 0x10)) {
        int tiled = 0;

        svga->lowres = 0;

        if (savage_primary_mmio(dev)) {
            uint32_t stride = savage_primary_stride(dev);
            uint32_t start  = dev->streams[((dev->streams[(0x81cc - 0x8180) >> 2] & 1) ? (0x81c4 - 0x8180) : (0x81c0 - 0x8180)) >> 2];

            svga->memaddr_latch = (start & 0x7ffff8) >> 2;
            if (stride & 0x1fff)
                svga->rowoffset = (stride & 0x1fff) >> 3;
            tiled = !!(stride & 0x80000000);
        }

        /* CR67 20h/40h: the CRTC counts bytes, two clocks a pixel (the BIOS
           doubles the horizontal timings, as for 640x480x16); 30h/50h: one
           clock a pixel (1600x1200x16 uses the 8 bpp timings). */
        if ((svga->bpp == 15) || (svga->bpp == 16)) {
            if (((svga->crtc[0x67] >> 4) == 0x02) || ((svga->crtc[0x67] >> 4) == 0x04)) {
                svga->hdisp >>= 1;
                svga->dots_per_clock >>= 1;
                svga->clock /= 2.0;
            }
        }

        switch (svga->bpp) {
            case 8:
                svga->render = svga_render_8bpp_highres;
                break;
            case 15:
                svga->render = tiled ? savage_render_tiled_16bpp : svga_render_15bpp_highres;
                break;
            case 16:
                svga->render = tiled ? savage_render_tiled_16bpp : svga_render_16bpp_highres;
                break;
            case 24:
                svga->render = svga_render_24bpp_highres;
                break;
            case 32:
                svga->render = tiled ? savage_render_tiled_32bpp : svga_render_32bpp_highres;
                break;

            default:
                break;
        }
    }

    svga->hoverride = 1;

    if (svga->render == svga_render_2bpp_lowres)
        svga->render = svga_render_2bpp_s3_lowres;
    else if (svga->render == svga_render_2bpp_highres)
        svga->render = svga_render_2bpp_s3_highres;
}

static void
s3_savage_hwcursor_draw(svga_t *svga, int displine)
{
    savage_t *dev    = (savage_t *) svga->priv;
    uint16_t  dat[2] = { 0, 0 };
    int       offset = svga->hwcursor_latch.x - svga->hwcursor_latch.xoff;
    uint32_t  fg;
    uint32_t  bg;

    if (svga->interlace && svga->hwcursor_oddeven)
        svga->hwcursor_latch.addr += 16;

    switch (svga->bpp) {
        case 8:
            fg = svga->pallook[dev->hwc_fg_col & 0xff];
            bg = svga->pallook[dev->hwc_bg_col & 0xff];
            break;
        case 15:
            fg = video_15to32[dev->hwc_fg_col & 0xffff];
            bg = video_15to32[dev->hwc_bg_col & 0xffff];
            break;
        case 16:
            fg = video_16to32[dev->hwc_fg_col & 0xffff];
            bg = video_16to32[dev->hwc_bg_col & 0xffff];
            break;
        default:
            fg = dev->hwc_fg_col;
            bg = dev->hwc_bg_col;
            break;
    }

    for (int x = 0; x < 64; x += 16) {
        uint32_t a = svga->hwcursor_latch.addr & SAVAGE_VRAM_MASK;

        dat[0] = (svga->vram[a] << 8) | svga->vram[(a + 1) & SAVAGE_VRAM_MASK];
        dat[1] = (svga->vram[(a + 2) & SAVAGE_VRAM_MASK] << 8) | svga->vram[(a + 3) & SAVAGE_VRAM_MASK];

        for (int xx = 0; xx < 16; xx++) {
            if (offset >= svga->hwcursor_latch.x) {
                if (svga->crtc[0x55] & 0x10) {
                    /* X11 */
                    if (dat[0] & 0x8000)
                        buffer32->line[displine][offset + svga->x_add] = (dat[1] & 0x8000) ? fg : bg;
                } else {
                    /* Windows */
                    if (!(dat[0] & 0x8000))
                        buffer32->line[displine][offset + svga->x_add] = (dat[1] & 0x8000) ? fg : bg;
                    else if (dat[1] & 0x8000)
                        buffer32->line[displine][offset + svga->x_add] ^= 0xffffff;
                }
            }

            offset++;
            dat[0] <<= 1;
            dat[1] <<= 1;
        }

        svga->hwcursor_latch.addr += 4;
    }

    if (svga->interlace && !svga->hwcursor_oddeven)
        svga->hwcursor_latch.addr += 16;
}

/* ------------------------------------------------------------------------ */
/* VGA / S3 extended registers                                              */
/* ------------------------------------------------------------------------ */

static void
s3_savage_serialport_write(savage_t *dev, uint8_t *reg, uint8_t val)
{
    *reg = val & ~(SERIAL_PORT_SCR | SERIAL_PORT_SDR);
    if (*reg & 0x10)
        i2c_gpio_set(dev->i2c, !!(*reg & SERIAL_PORT_SCW), !!(*reg & SERIAL_PORT_SDW));
}

static uint8_t
s3_savage_serialport_read(savage_t *dev, uint8_t reg)
{
    uint8_t ret = reg;

    if (reg & 0x10) {
        if (i2c_gpio_get_scl(dev->i2c) && (reg & SERIAL_PORT_SCW))
            ret |= SERIAL_PORT_SCR;
        if (i2c_gpio_get_sda(dev->i2c) && (reg & SERIAL_PORT_SDW))
            ret |= SERIAL_PORT_SDR;
    }

    return ret;
}

static void
s3_savage_update_bank(savage_t *dev)
{
    svga_t *svga = &dev->svga;

    if (svga->chain4)
        svga->write_bank = svga->read_bank = dev->bank << 16;
    else
        svga->write_bank = svga->read_bank = dev->bank << 14;
}

static void
s3_savage_out(uint16_t addr, uint8_t val, void *priv)
{
    savage_t *dev  = (savage_t *) priv;
    svga_t   *svga = &dev->svga;
    uint8_t   old;

    if (((addr & 0xfff0) == 0x3d0 || (addr & 0xfff0) == 0x3b0) && !(svga->miscout & 1))
        addr ^= 0x60;

    switch (addr) {
        case 0x3c5:
            if (svga->seqaddr >= 0x08) {
                if (svga->seqaddr == 0x08) {
                    svga->seqregs[0x08] = val & 0x0f;
                    return;
                }
                if ((svga->seqregs[0x08] & 0x0f) != 0x06)
                    return;
                svga->seqregs[svga->seqaddr] = val;
                if (svga->seqaddr == 0x0d)
                    svga->dpms = (svga->seqregs[0x0d] & 0x50) || (svga->crtc[0x56] & 0x06);
                if ((svga->seqaddr >= 0x10) || (svga->seqaddr == 0x0d))
                    svga_recalctimings(svga);
                return;
            }
            if (svga->seqaddr == 4) { /* Chain-4 - update banking */
                svga->seqregs[4] = val;
                svga->chain4     = val & 8;
                s3_savage_update_bank(dev);
            }
            break;

        case 0x3d4:
            svga->crtcreg = val;
            return;

        case 0x3d5:
            if ((svga->crtcreg < 7) && (svga->crtc[0x11] & 0x80))
                return;
            if ((svga->crtcreg == 7) && (svga->crtc[0x11] & 0x80))
                val = (svga->crtc[7] & ~0x10) | (val & 0x10);
            if ((svga->crtcreg >= 0x20) && (svga->crtcreg < 0x40) &&
                (svga->crtcreg != 0x36) && (svga->crtcreg != 0x38) &&
                (svga->crtcreg != 0x39) && ((svga->crtc[0x38] & 0xcc) != 0x48))
                return;
            if ((svga->crtcreg >= 0x40) && ((svga->crtc[0x39] & 0xe0) != 0xa0))
                return;
            if ((svga->crtcreg == 0x36) && (svga->crtc[0x39] != 0xa5))
                return;
            /* Read-only: chip ID and revision. */
            if ((svga->crtcreg >= 0x2d) && (svga->crtcreg <= 0x30))
                return;

            old                       = svga->crtc[svga->crtcreg];
            svga->crtc[svga->crtcreg] = val;

            switch (svga->crtcreg) {
                case 0x31:
                    dev->ma_ext = (dev->ma_ext & 0x7c) | ((val & 0x30) >> 4);
                    s3_savage_updatemapping(dev);
                    break;
                case 0x32:
                    s3_savage_updatemapping(dev);
                    break;
                case 0x35:
                    dev->bank = (dev->bank & 0x70) | (val & 0x0f);
                    s3_savage_update_bank(dev);
                    break;
                case 0x51:
                    dev->bank   = (dev->bank & 0x4f) | ((val & 0x0c) << 2);
                    dev->ma_ext = (dev->ma_ext & ~0x0c) | ((val & 3) << 2);
                    s3_savage_update_bank(dev);
                    break;
                case 0x69:
                    dev->ma_ext = val & 0x7f;
                    break;
                case 0x6a:
                    dev->bank = val & 0x7f;
                    s3_savage_update_bank(dev);
                    break;

                case 0x3a:
                    if (val & 0x10)
                        svga->gdcreg[5] |= 0x40; /* Horrible cheat, as on the ViRGE. */
                    break;

                case 0x45:
                    svga->hwcursor.ena = val & 1;
                    break;
                case 0x46:
                case 0x47:
                case 0x48:
                case 0x49:
                case 0x4c:
                case 0x4d:
                case 0x4e:
                case 0x4f:
                    svga->hwcursor.x    = ((svga->crtc[0x46] << 8) | svga->crtc[0x47]) & 0x7ff;
                    svga->hwcursor.y    = ((svga->crtc[0x48] << 8) | svga->crtc[0x49]) & 0x7ff;
                    svga->hwcursor.xoff = svga->crtc[0x4e] & 0x3f;
                    svga->hwcursor.yoff = svga->crtc[0x4f] & 0x3f;
                    svga->hwcursor.addr = ((((svga->crtc[0x4c] << 8) | svga->crtc[0x4d]) & 0x1fff) * 1024) +
                                          (svga->hwcursor.yoff * 16);
                    break;

                case 0x4a:
                    switch (dev->hwc_col_stack_pos) {
                        case 0:
                            dev->hwc_fg_col = (dev->hwc_fg_col & 0xffff00) | val;
                            break;
                        case 1:
                            dev->hwc_fg_col = (dev->hwc_fg_col & 0xff00ff) | (val << 8);
                            break;
                        case 2:
                            dev->hwc_fg_col = (dev->hwc_fg_col & 0x00ffff) | (val << 16);
                            break;
                        default:
                            break;
                    }
                    dev->hwc_col_stack_pos = (dev->hwc_col_stack_pos + 1) & 3;
                    break;
                case 0x4b:
                    switch (dev->hwc_col_stack_pos) {
                        case 0:
                            dev->hwc_bg_col = (dev->hwc_bg_col & 0xffff00) | val;
                            break;
                        case 1:
                            dev->hwc_bg_col = (dev->hwc_bg_col & 0xff00ff) | (val << 8);
                            break;
                        case 2:
                            dev->hwc_bg_col = (dev->hwc_bg_col & 0x00ffff) | (val << 16);
                            break;
                        default:
                            break;
                    }
                    dev->hwc_col_stack_pos = (dev->hwc_col_stack_pos + 1) & 3;
                    break;

                case 0x53:
                case 0x58:
                    s3_savage_updatemapping(dev);
                    break;

                case 0x56:
                    svga->dpms = (svga->seqregs[0x0d] & 0x50) || (svga->crtc[0x56] & 0x06);
                    old        = ~val; /* force recalc */
                    break;

                case 0x67:
                    switch (val >> 4) {
                        case 0x02:
                        case 0x03:
                            svga->bpp = 15;
                            break;
                        case 0x04:
                        case 0x05:
                            svga->bpp = 16;
                            break;
                        case 0x07:
                            svga->bpp = 24;
                            break;
                        case 0x0d:
                            svga->bpp = 32;
                            break;
                        default:
                            svga->bpp = 8;
                            break;
                    }
                    break;

                case 0xaa:
                    s3_savage_serialport_write(dev, &svga->crtc[0xaa], val);
                    break;

                default:
                    break;
            }

            if (old != val) {
                if ((svga->crtcreg < 0xe) || (svga->crtcreg > 0x10)) {
                    if ((svga->crtcreg == 0xc) || (svga->crtcreg == 0xd)) {
                        svga->fullchange    = 3;
                        svga->memaddr_latch = ((svga->crtc[0xc] << 8) | svga->crtc[0xd]) +
                                              ((svga->crtc[8] & 0x60) >> 5) + (dev->ma_ext << 16);
                    } else {
                        svga->fullchange = changeframecount;
                        svga_recalctimings(svga);
                    }
                }
            }
            break;

        default:
            break;
    }

    svga_out(addr, val, svga);
}

static uint8_t
s3_savage_in(uint16_t addr, void *priv)
{
    savage_t *dev  = (savage_t *) priv;
    svga_t   *svga = &dev->svga;
    uint8_t   ret;

    if (((addr & 0xfff0) == 0x3d0 || (addr & 0xfff0) == 0x3b0) && !(svga->miscout & 1))
        addr ^= 0x60;

    switch (addr) {
        case 0x3c1:
            if (svga->attraddr > 0x14)
                ret = 0xff;
            else
                ret = svga_in(addr, svga);
            break;

        case 0x3c2:
            /* Monitor sense: a monitor is attached. */
            if ((svga->vgapal[0].r + svga->vgapal[0].g + svga->vgapal[0].b) >= 0x4e)
                ret = 0;
            else
                ret = 0x10;
            break;

        case 0x3c5:
            if (svga->seqaddr >= 8)
                ret = svga->seqregs[svga->seqaddr];
            else if (svga->seqaddr <= 4)
                ret = svga_in(addr, svga);
            else
                ret = 0xff;
            break;

        case 0x3d4:
            ret = svga->crtcreg;
            break;

        case 0x3d5:
            switch (svga->crtcreg) {
                case 0x2d:
                    ret = dev->device_id >> 8;
                    break;
                case 0x2e:
                    ret = dev->device_id & 0xff;
                    break;
                case 0x2f:
                    ret = 0x00; /* revision */
                    break;
                case 0x30:
                    ret = (((svga->crtc[0x38] & 0xcc) == 0x48) || ((svga->crtc[0x39] & 0xe0) == 0xa0)) ? 0xe1 : 0xff;
                    break;
                case 0x31:
                    ret = (svga->crtc[0x31] & 0xcf) | ((dev->ma_ext & 3) << 4);
                    break;
                case 0x33:
                    ret = svga->crtc[0x33] | 0x04;
                    break;
                case 0x35:
                    ret = (svga->crtc[0x35] & 0xf0) | (dev->bank & 0x0f);
                    break;
                case 0x45:
                    dev->hwc_col_stack_pos = 0;
                    ret                    = svga->crtc[0x45];
                    break;
                case 0x51:
                    ret = (svga->crtc[0x51] & 0xf0) | ((dev->bank >> 2) & 0x0c) | ((dev->ma_ext >> 2) & 3);
                    break;
                case 0x69:
                    ret = (svga->crtc[0x69] & 0x80) | (dev->ma_ext & 0x7f);
                    break;
                case 0x6a:
                    ret = dev->bank;
                    break;
                case 0xaa:
                    ret = s3_savage_serialport_read(dev, svga->crtc[0xaa]);
                    break;
                default:
                    ret = svga->crtc[svga->crtcreg];
                    break;
            }
            break;

        default:
            ret = svga_in(addr, svga);
            break;
    }

    return ret;
}

/* ------------------------------------------------------------------------ */
/* Memory mapping                                                           */
/* ------------------------------------------------------------------------ */

static void
s3_savage_updatemapping(savage_t *dev)
{
    svga_t *svga = &dev->svga;

    if (!(dev->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_MEM)) {
        mem_mapping_disable(&svga->mapping);
        mem_mapping_disable(&dev->linear_mapping);
        mem_mapping_disable(&dev->mmio_mapping);
        mem_mapping_disable(&dev->aperture_mapping);
        mem_mapping_disable(&dev->old_mmio_mapping);
        return;
    }

    if (svga->crtc[0x31] & 0x08) { /* Enhanced mode mappings */
        mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x10000);
        svga->banked_mask = 0xffff;
    } else {
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
    }

    /* The PCI aperture is always decoded; CR58 bit 4 only turns the A0000 window off. */
    if (dev->bar0) {
        mem_mapping_set_addr(&dev->linear_mapping, dev->bar0, BAR0_MMIO);
        mem_mapping_set_addr(&dev->mmio_mapping, dev->bar0 + BAR0_MMIO, BAR0_MMIO_SIZE);
        mem_mapping_set_addr(&dev->aperture_mapping, dev->bar0 + BAR0_APERTURE, BAR0_APERTURE_SIZE);
    } else {
        mem_mapping_disable(&dev->linear_mapping);
        mem_mapping_disable(&dev->mmio_mapping);
        mem_mapping_disable(&dev->aperture_mapping);
    }

    if (svga->crtc[0x58] & 0x10) {
        mem_mapping_disable(&svga->mapping);
        svga->fb_only = 1;
    } else
        svga->fb_only = 0;

    if (svga->crtc[0x53] & 0x10) { /* Old MMIO */
        mem_mapping_disable(&svga->mapping);
        if (svga->crtc[0x53] & 0x20)
            mem_mapping_set_addr(&dev->old_mmio_mapping, 0xb8000, 0x8000);
        else
            mem_mapping_set_addr(&dev->old_mmio_mapping, 0xa0000, 0x10000);
    } else
        mem_mapping_disable(&dev->old_mmio_mapping);
}

/* ------------------------------------------------------------------------ */
/* MMIO                                                                     */
/* ------------------------------------------------------------------------ */

static void
s3_savage_update_shadow(savage_t *dev)
{
    uint32_t addr = dev->shadow_ctrl & 0xfffffff8;

    if (!(dev->shadow_ctrl & 1) || !addr)
        return;

    mem_writel_phys(addr, STATUS0_IDLE);
    mem_writel_phys(addr + 4, dev->event_tag);
    mem_writel_phys(addr + 8, 0);
}

static uint32_t
s3_savage_mmio_readl_reg(savage_t *dev, uint32_t addr)
{
    if ((addr >= 0x8180) && (addr < 0x8380))
        return dev->streams[(addr - 0x8180) >> 2];

    switch (addr) {
        case 0x8128:
            return dev->write_mask;
        case 0x812c:
            return dev->read_mask;
        case 0x8168:
            return dev->gbd.offset;
        case 0x816c:
            return dev->gbd.hi;

        case 0x48c00:
            return STATUS0_IDLE;
        case 0x48c04:
            return dev->event_tag;
        case 0x48c08:
            return 0;
        case 0x48c0c:
            return dev->shadow_ctrl;
        case 0x48c10:
            return dev->buf_thresh;
        case 0x48c14:
            return dev->cob;
        case 0x48c18:
            return dev->bci_ctrl;
        case 0x48c40:
        case 0x48c44:
        case 0x48c48:
        case 0x48c4c:
        case 0x48c50:
        case 0x48c54:
            return dev->tiled_surf[(addr - 0x48c40) >> 2];

        default:
            break;
    }

    return 0;
}

static void
s3_savage_mmio_writel_reg(savage_t *dev, uint32_t addr, uint32_t val)
{
    svga_t *svga = &dev->svga;

    if ((addr >= 0x8180) && (addr < 0x8380)) {
        dev->streams[(addr - 0x8180) >> 2] = val;
        svga->fullchange                   = changeframecount;
        svga_recalctimings(svga);
        return;
    }

    switch (addr) {
        case 0x8128:
            dev->write_mask = val;
            break;
        case 0x812c:
            dev->read_mask = val;
            break;
        case 0x8168:
            dev->gbd.offset = val;
            break;
        case 0x816c:
            /* The low bits are BCI control (enable, endianness, 64-bit BD). */
            dev->gbd.hi = val & ~0x0f;
            break;

        case 0x48c0c:
            dev->shadow_ctrl = val;
            s3_savage_update_shadow(dev);
            break;
        case 0x48c10:
            dev->buf_thresh = val;
            break;
        case 0x48c14:
            dev->cob = val;
            break;
        case 0x48c18:
            dev->bci_ctrl = val;
            break;
        case 0x48c40:
        case 0x48c44:
        case 0x48c48:
        case 0x48c4c:
        case 0x48c50:
        case 0x48c54:
            dev->tiled_surf[(addr - 0x48c40) >> 2] = val;
            break;

        default:
            s3_savage_log("MMIO write %05x = %08x\n", addr, val);
            break;
    }
}

static uint8_t
s3_savage_mmio_read_off(savage_t *dev, uint32_t addr)
{
    if ((addr >= 0x83b0) && (addr < 0x83e0))
        return s3_savage_in(addr & 0x3ff, dev);
    if ((addr >= 0x8000) && (addr < 0x8020)) /* PCI configuration space copy */
        return dev->pci_regs[addr & 0xff];

    return s3_savage_mmio_readl_reg(dev, addr & ~3) >> ((addr & 3) << 3);
}

static uint8_t
s3_savage_mmio_read(uint32_t addr, void *priv)
{
    savage_t *dev = (savage_t *) priv;

    return s3_savage_mmio_read_off(dev, (addr - dev->bar0 - BAR0_MMIO) & (BAR0_MMIO_SIZE - 1));
}

static uint16_t
s3_savage_mmio_readw(uint32_t addr, void *priv)
{
    return s3_savage_mmio_read(addr, priv) | (s3_savage_mmio_read(addr + 1, priv) << 8);
}

static uint32_t
s3_savage_mmio_readl_off(savage_t *dev, uint32_t addr)
{
    if ((addr >= 0x83b0) && (addr < 0x83e0))
        return s3_savage_mmio_read_off(dev, addr) | (s3_savage_mmio_read_off(dev, addr + 1) << 8) |
               (s3_savage_mmio_read_off(dev, addr + 2) << 16) | (s3_savage_mmio_read_off(dev, addr + 3) << 24);

    return s3_savage_mmio_readl_reg(dev, addr & ~3);
}

static uint32_t
s3_savage_mmio_readl(uint32_t addr, void *priv)
{
    savage_t *dev = (savage_t *) priv;

    return s3_savage_mmio_readl_off(dev, (addr - dev->bar0 - BAR0_MMIO) & (BAR0_MMIO_SIZE - 1));
}

static void
s3_savage_mmio_write_off(savage_t *dev, uint32_t addr, uint8_t val)
{
    if ((addr >= 0x83b0) && (addr < 0x83e0)) {
        s3_savage_out(addr & 0x3ff, val, dev);
        return;
    }

    if ((addr >= 0x10000) && (addr < 0x30000))
        return; /* The BCI only takes dwords. */

    {
        uint32_t reg   = addr & ~3;
        int      shift = (addr & 3) << 3;
        uint32_t cur   = s3_savage_mmio_readl_reg(dev, reg);

        s3_savage_mmio_writel_reg(dev, reg, (cur & ~(0xff << shift)) | (val << shift));
    }
}

static void
s3_savage_mmio_writel_off(savage_t *dev, uint32_t addr, uint32_t val)
{
    if ((addr >= 0x10000) && (addr < 0x30000)) {
        s3_savage_bci_write(dev, val);
        return;
    }

    if ((addr >= 0x83b0) && (addr < 0x83e0)) {
        for (int i = 0; i < 4; i++)
            s3_savage_out((addr + i) & 0x3ff, val >> (i << 3), dev);
        return;
    }

    s3_savage_mmio_writel_reg(dev, addr & ~3, val);
}

static void
s3_savage_mmio_write(uint32_t addr, uint8_t val, void *priv)
{
    savage_t *dev = (savage_t *) priv;

    s3_savage_mmio_write_off(dev, (addr - dev->bar0 - BAR0_MMIO) & (BAR0_MMIO_SIZE - 1), val);
}

static void
s3_savage_mmio_writew(uint32_t addr, uint16_t val, void *priv)
{
    savage_t *dev = (savage_t *) priv;
    uint32_t  off = (addr - dev->bar0 - BAR0_MMIO) & (BAR0_MMIO_SIZE - 1);

    if ((off >= 0x10000) && (off < 0x30000))
        return;

    s3_savage_mmio_write_off(dev, off, val & 0xff);
    s3_savage_mmio_write_off(dev, off + 1, val >> 8);
}

static void
s3_savage_mmio_writel(uint32_t addr, uint32_t val, void *priv)
{
    savage_t *dev = (savage_t *) priv;

    s3_savage_mmio_writel_off(dev, (addr - dev->bar0 - BAR0_MMIO) & (BAR0_MMIO_SIZE - 1), val);
}

/* The old MMIO window: A0000-AFFFF = MMIO 0000-FFFF, B8000-BFFFF = MMIO 8000-FFFF. */
static uint32_t
s3_savage_old_mmio_off(savage_t *dev, uint32_t addr)
{
    if (dev->svga.crtc[0x53] & 0x20)
        return 0x8000 + (addr & 0x7fff);

    return addr & 0xffff;
}

static uint8_t
s3_savage_old_mmio_read(uint32_t addr, void *priv)
{
    savage_t *dev = (savage_t *) priv;

    return s3_savage_mmio_read_off(dev, s3_savage_old_mmio_off(dev, addr));
}

static uint16_t
s3_savage_old_mmio_readw(uint32_t addr, void *priv)
{
    return s3_savage_old_mmio_read(addr, priv) | (s3_savage_old_mmio_read(addr + 1, priv) << 8);
}

static uint32_t
s3_savage_old_mmio_readl(uint32_t addr, void *priv)
{
    savage_t *dev = (savage_t *) priv;

    return s3_savage_mmio_readl_off(dev, s3_savage_old_mmio_off(dev, addr));
}

static void
s3_savage_old_mmio_write(uint32_t addr, uint8_t val, void *priv)
{
    savage_t *dev = (savage_t *) priv;

    s3_savage_mmio_write_off(dev, s3_savage_old_mmio_off(dev, addr), val);
}

static void
s3_savage_old_mmio_writew(uint32_t addr, uint16_t val, void *priv)
{
    s3_savage_old_mmio_write(addr, val & 0xff, priv);
    s3_savage_old_mmio_write(addr + 1, val >> 8, priv);
}

static void
s3_savage_old_mmio_writel(uint32_t addr, uint32_t val, void *priv)
{
    savage_t *dev = (savage_t *) priv;

    s3_savage_mmio_writel_off(dev, s3_savage_old_mmio_off(dev, addr), val);
}

/* ------------------------------------------------------------------------ */
/* BCI 2D engine                                                            */
/* ------------------------------------------------------------------------ */

static savage_bd_t *
savage_bd(savage_t *dev, int which)
{
    switch (which) {
        case BD_PBD:
            return &dev->pbd;
        case BD_SBD:
            return &dev->sbd;
        default:
            return &dev->gbd;
    }
}

static inline int
savage_bd_bytespp(const savage_bd_t *bd)
{
    switch ((bd->hi >> 16) & 0xff) {
        case 16:
        case 15:
            return 2;
        case 24:
        case 32:
            return 4;
        default:
            return 1;
    }
}

static inline uint32_t
savage_bd_addr(const savage_bd_t *bd, int x, int y, int bytespp)
{
    uint32_t stride = bd->hi & 0xffff;
    uint32_t tile   = (bd->hi >> 24) & 3;

    if (tile >= 2) {
        int tiles = (int) ((stride * bytespp + 127) >> 7);

        return savage_tiled_addr(bd->offset, tiles, x * bytespp, y) & SAVAGE_VRAM_MASK;
    }

    return (bd->offset + (y * stride + x) * bytespp) & SAVAGE_VRAM_MASK;
}

static inline uint32_t
savage_read_pixel(savage_t *dev, uint32_t addr, int bytespp)
{
    switch (bytespp) {
        case 1:
            return dev->svga.vram[addr];
        case 2:
            return *(uint16_t *) &dev->svga.vram[addr & ~1];
        default:
            return *(uint32_t *) &dev->svga.vram[addr & ~3];
    }
}

static inline void
savage_write_pixel(savage_t *dev, uint32_t addr, uint32_t val, int bytespp)
{
    switch (bytespp) {
        case 1:
            dev->svga.vram[addr] = val;
            break;
        case 2:
            *(uint16_t *) &dev->svga.vram[addr & ~1] = val;
            break;
        default:
            *(uint32_t *) &dev->svga.vram[addr & ~3] = val;
            break;
    }
    dev->svga.changedvram[addr >> 12] = changeframecount;
}

static inline uint32_t
savage_rop(uint8_t rop, uint32_t p, uint32_t s, uint32_t d)
{
    uint32_t r = 0;

    switch (rop) {
        case 0x00:
            return 0;
        case 0xcc:
            return s;
        case 0xf0:
            return p;
        case 0xaa:
            return d;
        case 0xff:
            return 0xffffffff;
        case 0x55:
            return ~d;
        case 0x66:
            return s ^ d;
        case 0x5a:
            return p ^ d;
        case 0x88:
            return s & d;
        case 0xee:
            return s | d;
        case 0x33:
            return ~s;
        default:
            break;
    }

    for (int i = 0; i < 8; i++) {
        if (rop & (1 << i))
            r |= ((i & 4) ? p : ~p) & ((i & 2) ? s : ~s) & ((i & 1) ? d : ~d);
    }

    return r;
}

static int
savage_src_type(uint32_t cmd)
{
    return (cmd >> 5) & 0x0f;
}

static int
savage_pat_type(uint32_t cmd)
{
    return cmd & 0x0f;
}

static int
savage_dst_type(uint32_t cmd)
{
    return (cmd >> 10) & 7;
}

/* Source 1 = GBD, 2 = host color, 3 = host mono, 4-7 PBD, 8-B SBD; odd above 3 = mono. */
static int
savage_src_is_mono(int src)
{
    return (src == 3) || ((src >= 4) && (src & 1));
}

static int
savage_pat_is_mono(int pat)
{
    return (pat == 3) || ((pat >= 4) && (pat & 1));
}

static int
savage_src_bd(int src)
{
    if (src == 1)
        return BD_GBD;
    if ((src >= 4) && (src <= 7))
        return BD_PBD;
    if ((src >= 8) && (src <= 0x0b))
        return BD_SBD;
    return BD_NONE;
}

static int
savage_src_bd_new(int src)
{
    return (src == 6) || (src == 7) || (src == 0x0a) || (src == 0x0b);
}

static int
savage_dst_bd(uint32_t cmd)
{
    switch (savage_dst_type(cmd)) {
        case 2:
        case 3:
            return BD_PBD;
        case 4:
        case 5:
            return BD_SBD;
        default:
            return BD_GBD;
    }
}

static int
savage_dst_bd_new(uint32_t cmd)
{
    int d = savage_dst_type(cmd);

    return (d == 1) || (d == 3) || (d == 5);
}

/* Pattern pixel (px, py) of the 8x8 pattern. */
static uint32_t
savage_pattern(savage_t *dev, int px, int py, int bytespp, int *opaque)
{
    uint32_t cmd = dev->bci.cmd;
    int      pat = savage_pat_type(cmd);

    *opaque = 1;

    if (!pat)
        return 0;

    if (pat == 3) { /* host mono: two dwords, one byte per row */
        uint8_t  row = (dev->bci.p[(py < 4) ? P_PAT0 : P_PAT1] >> ((py & 3) << 3)) & 0xff;
        int      bit = (row >> (7 - px)) & 1;

        if (!bit && (cmd & BCI_PAT_TRANSP))
            *opaque = 0;
        return bit ? dev->bci.p[P_COLOR0] : dev->bci.p[P_COLOR1];
    }

    if (pat >= 4) {
        savage_bd_t *bd = savage_bd(dev, (pat >= 8) ? BD_SBD : BD_PBD);

        if (savage_pat_is_mono(pat)) {
            uint8_t row = dev->svga.vram[(bd->offset + py) & SAVAGE_VRAM_MASK];
            int     bit = (row >> (7 - px)) & 1;

            if (!bit && (cmd & BCI_PAT_TRANSP))
                *opaque = 0;
            return bit ? dev->bci.p[P_COLOR0] : dev->bci.p[P_COLOR1];
        } else {
            uint32_t c = savage_read_pixel(dev, savage_bd_addr(bd, px, py, bytespp), bytespp);

            if ((cmd & BCI_PAT_TRANSP) && (c == (dev->bci.p[P_COLOR0] & (bytespp == 4 ? 0xffffffff : ((1u << (bytespp * 8)) - 1)))))
                *opaque = 0;
            return c;
        }
    }

    return 0;
}

static inline int
savage_clipped(savage_t *dev, int x, int y)
{
    switch ((dev->bci.cmd >> 13) & 3) {
        case 0:
            return 0;
        case 2: /* left/right only */
            return (x < dev->bci.clip_l) || (x > dev->bci.clip_r);
        default:
            return (x < dev->bci.clip_l) || (x > dev->bci.clip_r) || (y < dev->bci.clip_t) || (y > dev->bci.clip_b);
    }
}

/* Draws one destination pixel; src is the source value, src_opaque 0 for a transparent source. */
static void
savage_put(savage_t *dev, savage_bd_t *dbd, int bytespp, int x, int y, uint32_t src, int src_opaque)
{
    uint32_t mask = (bytespp == 4) ? 0xffffffff : ((1u << (bytespp * 8)) - 1);
    uint32_t addr;
    uint32_t pat;
    uint32_t d;
    uint32_t r;
    int      pat_opaque;
    uint8_t  rop = (dev->bci.cmd >> 16) & 0xff;

    if ((x < 0) || (y < 0) || savage_clipped(dev, x, y) || !src_opaque)
        return;

    pat = savage_pattern(dev, x & 7, y & 7, bytespp, &pat_opaque);
    if (!pat_opaque)
        return;

    addr = savage_bd_addr(dbd, x, y, bytespp);
    d    = savage_read_pixel(dev, addr, bytespp);
    r    = savage_rop(rop, pat, src, d);
    r    = (r & dev->write_mask) | (d & ~dev->write_mask);

    savage_write_pixel(dev, addr, r & mask, bytespp);
}

static uint32_t
savage_color_mask(int bytespp)
{
    return (bytespp == 4) ? 0xffffffff : ((1u << (bytespp * 8)) - 1);
}

/* Rectangle with a source in video memory or a solid color. */
static void
savage_rect(savage_t *dev)
{
    uint32_t     cmd     = dev->bci.cmd;
    int          src     = savage_src_type(cmd);
    savage_bd_t *dbd     = savage_bd(dev, savage_dst_bd(cmd));
    int          bytespp = savage_bd_bytespp(dbd);
    uint32_t     cmask   = savage_color_mask(bytespp);
    int          w       = dev->bci.p[P_WH] & 0xfff;
    int          h       = (dev->bci.p[P_WH] >> 16) & 0xfff;
    int          dx      = (int16_t) (dev->bci.p[P_DST_XY] & 0xffff);
    int          dy      = (int16_t) (dev->bci.p[P_DST_XY] >> 16);
    int          sx      = (int16_t) (dev->bci.p[P_SRC_XY] & 0xffff);
    int          sy      = (int16_t) (dev->bci.p[P_SRC_XY] >> 16);
    int          xdir    = (cmd & BCI_X_POSITIVE) ? 1 : -1;
    int          ydir    = (cmd & BCI_Y_POSITIVE) ? 1 : -1;
    int          sbd_n   = savage_src_bd(src);
    savage_bd_t *sbd     = (sbd_n != BD_NONE) ? savage_bd(dev, sbd_n) : NULL;
    int          sbpp    = sbd ? savage_bd_bytespp(sbd) : bytespp;

    s3_savage_log("BCI rect %08x src %08x dst %08x wh %08x\n", cmd, dev->bci.p[P_SRC_XY], dev->bci.p[P_DST_XY], dev->bci.p[P_WH]);

    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            int      x      = dx + xx * xdir;
            int      y      = dy + yy * ydir;
            uint32_t s      = dev->bci.p[P_COLOR0] & cmask;
            int      opaque = 1;

            if (sbd) {
                int ssx = sx + xx * xdir;
                int ssy = sy + yy * ydir;

                if (savage_src_is_mono(src)) {
                    uint32_t stride_bytes = ((sbd->hi & 0xffff) + 7) >> 3;
                    uint8_t  byte         = dev->svga.vram[(sbd->offset + ssy * stride_bytes + (ssx >> 3)) & SAVAGE_VRAM_MASK];
                    int      bit          = (byte >> (7 - (ssx & 7))) & 1;

                    s = (bit ? dev->bci.p[P_COLOR0] : dev->bci.p[P_COLOR1]) & cmask;
                    if (!bit && (cmd & BCI_SRC_TRANSP))
                        opaque = 0;
                } else {
                    s = savage_read_pixel(dev, savage_bd_addr(sbd, ssx, ssy, sbpp), sbpp) & cmask;
                    if ((cmd & BCI_SRC_TRANSP) && (s == (dev->bci.p[P_COLOR0] & cmask)))
                        opaque = 0;
                }
            }

            savage_put(dev, dbd, bytespp, x, y, s, opaque);
        }
    }
}

/* One line of host data has arrived. */
static void
savage_host_line(savage_t *dev)
{
    uint32_t     cmd     = dev->bci.cmd;
    savage_bd_t *dbd     = savage_bd(dev, savage_dst_bd(cmd));
    int          bytespp = savage_bd_bytespp(dbd);
    uint32_t     cmask   = savage_color_mask(bytespp);
    int          y       = dev->bci.y + ((cmd & BCI_Y_POSITIVE) ? dev->bci.line : -dev->bci.line);
    const uint8_t *buf   = (const uint8_t *) dev->bci.line_buf;

    for (int xx = 0; xx < dev->bci.w; xx++) {
        uint32_t s;
        int      opaque = 1;

        if (dev->bci.mono) {
            int bit = (buf[xx >> 3] >> (7 - (xx & 7))) & 1;

            s = (bit ? dev->bci.p[P_COLOR0] : dev->bci.p[P_COLOR1]) & cmask;
            if (!bit && (cmd & BCI_SRC_TRANSP))
                opaque = 0;
        } else {
            switch (bytespp) {
                case 1:
                    s = buf[xx];
                    break;
                case 2:
                    s = ((const uint16_t *) buf)[xx];
                    break;
                default:
                    s = ((const uint32_t *) buf)[xx];
                    break;
            }
            if ((cmd & BCI_SRC_TRANSP) && (s == (dev->bci.p[P_COLOR0] & cmask)))
                opaque = 0;
        }

        savage_put(dev, dbd, bytespp, dev->bci.x + xx, y, s, opaque);
    }
}

static void
savage_line(savage_t *dev)
{
    uint32_t     cmd     = dev->bci.cmd;
    savage_bd_t *dbd     = savage_bd(dev, savage_dst_bd(cmd));
    int          bytespp = savage_bd_bytespp(dbd);
    uint32_t     color   = dev->bci.p[P_COLOR0] & savage_color_mask(bytespp);
    int          x       = (int16_t) (dev->bci.p[P_DST_XY] & 0xffff);
    int          y       = (int16_t) (dev->bci.p[P_DST_XY] >> 16);
    int          k1      = (int16_t) (dev->bci.p[P_LINE_K] >> 16);
    int          k2      = (int16_t) (dev->bci.p[P_LINE_K] & 0xffff);
    uint32_t     e_dw    = dev->bci.p[P_LINE_E];
    int          err     = (int16_t) (e_dw >> 16);
    int          len     = e_dw & 0xfff;
    int          xp      = !!(e_dw & 0x4000);
    int          yp      = !!(e_dw & 0x8000);
    int          ymajor  = !!(e_dw & 0x2000);
    int          count   = len + (((cmd >> 24) & 0xfc) == 0x5c ? 1 : 0);

    for (int i = 0; i < count; i++) {
        savage_put(dev, dbd, bytespp, x, y, color, 1);

        if (ymajor) {
            y += yp ? 1 : -1;
            if (err >= 0) {
                x += xp ? 1 : -1;
                err += k2;
            } else
                err += k1;
        } else {
            x += xp ? 1 : -1;
            if (err >= 0) {
                y += yp ? 1 : -1;
                err += k2;
            } else
                err += k1;
        }
    }
}

static void
savage_glyph(savage_t *dev)
{
    uint32_t     cmd     = dev->bci.cmd;
    savage_bd_t *dbd     = savage_bd(dev, savage_dst_bd(cmd));
    int          bytespp = savage_bd_bytespp(dbd);
    uint32_t     cmask   = savage_color_mask(bytespp);
    int          w       = dev->bci.glyph_w;
    int          bpl     = (w + 7) >> 3;

    for (int yy = 0; yy < dev->bci.glyph_h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            uint32_t bitpos = dev->bci.glyph_bitpacked ? (yy * w + xx) : (yy * bpl * 8 + xx);
            uint32_t byte   = bitpos >> 3;
            int      bit;

            if (byte >= sizeof(dev->bci.glyph_buf))
                return;
            bit = (dev->bci.glyph_buf[byte] >> (7 - (bitpos & 7))) & 1;

            if (bit)
                savage_put(dev, dbd, bytespp, dev->bci.glyph_x + xx, dev->bci.glyph_y + yy, dev->bci.p[P_COLOR0] & cmask, 1);
            else if (!(cmd & BCI_SRC_TRANSP))
                savage_put(dev, dbd, bytespp, dev->bci.glyph_x + xx, dev->bci.glyph_y + yy, dev->bci.p[P_COLOR1] & cmask, 1);
        }
    }
}

static int
savage_cmd_class(uint32_t cmd)
{
    return cmd >> 27;
}

/* A rectangle's parameters are complete: draw it or start its host data. */
static void
savage_packet_done(savage_t *dev)
{
    uint32_t cmd = dev->bci.cmd;
    int      cls = savage_cmd_class(cmd);
    int      src = savage_src_type(cmd);

    dev->bci.state = BCI_IDLE;

    switch (cls) {
        case 0x09: /* rectangle */
            if ((src == 2) || (src == 3)) {
                savage_bd_t *dbd     = savage_bd(dev, savage_dst_bd(cmd));
                int          bytespp = savage_bd_bytespp(dbd);

                dev->bci.x    = (int16_t) (dev->bci.p[P_DST_XY] & 0xffff);
                dev->bci.y    = (int16_t) (dev->bci.p[P_DST_XY] >> 16);
                dev->bci.w    = dev->bci.p[P_WH] & 0xfff;
                dev->bci.h    = (dev->bci.p[P_WH] >> 16) & 0xfff;
                dev->bci.mono = (src == 3);
                if (dev->bci.mono)
                    dev->bci.dwords_per_line = (dev->bci.w + 31) >> 5;
                else
                    dev->bci.dwords_per_line = (dev->bci.w * bytespp + 3) >> 2;
                if (dev->bci.dwords_per_line > (int) (sizeof(dev->bci.line_buf) / 4))
                    dev->bci.dwords_per_line = sizeof(dev->bci.line_buf) / 4;
                dev->bci.line  = 0;
                dev->bci.dword = 0;
                if (dev->bci.w && dev->bci.h && dev->bci.dwords_per_line)
                    dev->bci.state = BCI_HOSTDATA;
            } else
                savage_rect(dev);
            break;

        case 0x0b: /* line */
            savage_line(dev);
            break;

        case 0x0c:
        case 0x0d: /* text: glyphs follow */
            dev->bci.state = BCI_TEXT_XY;
            break;

        default:
            break;
    }
}

static void
savage_add_slot(savage_t *dev, int slot)
{
    dev->bci.slots[dev->bci.nslots++] = slot;
}

static void
savage_start_command(savage_t *dev, uint32_t cmd)
{
    int cls = savage_cmd_class(cmd);
    int src = savage_src_type(cmd);
    int pat = savage_pat_type(cmd);
    int colors;

    s3_savage_log("BCI cmd %08x\n", cmd);
    dev->bci.cmd    = cmd;
    dev->bci.nslots = 0;
    dev->bci.slot   = 0;

    /* Bitmap descriptors: source, destination, pattern (as both drivers send them). */
    if (savage_src_bd_new(src))
        savage_add_slot(dev, P_SRC_BD_LO), savage_add_slot(dev, P_SRC_BD_HI);
    if (savage_dst_bd_new(cmd))
        savage_add_slot(dev, P_DST_BD_LO), savage_add_slot(dev, P_DST_BD_HI);
    if ((pat == 6) || (pat == 7) || (pat == 0x0a) || (pat == 0x0b))
        savage_add_slot(dev, P_PAT_BD_LO), savage_add_slot(dev, P_PAT_BD_HI);

    if (cls == 0x08) { /* NOP: may only load descriptors */
        dev->bci.first_coord = dev->bci.nslots;
        if (!dev->bci.nslots)
            return;
        dev->bci.state = BCI_PARAMS;
        return;
    }

    switch ((cmd >> 13) & 3) {
        case 2:
            if (cls != 0x0b)
                savage_add_slot(dev, P_CLIP_LR);
            break;
        case 3:
            if (cls != 0x0b)
                savage_add_slot(dev, P_CLIP_TL), savage_add_slot(dev, P_CLIP_BR);
            break;
        default:
            break;
    }

    if (cmd & BCI_CS) {
        if (savage_src_is_mono(src) || savage_pat_is_mono(pat) || (cls == 0x0c) || (cls == 0x0d))
            colors = ((cmd & (BCI_SRC_TRANSP | BCI_PAT_TRANSP)) || (cls == 0x0c) || (cls == 0x0d)) ? 1 : 2;
        else
            colors = 1;
        /* Opaque text sends a background color too. */
        if (((cls == 0x0c) || (cls == 0x0d)) && !(cmd & BCI_SRC_TRANSP))
            colors = 2;
        savage_add_slot(dev, P_COLOR0);
        if (colors == 2)
            savage_add_slot(dev, P_COLOR1);
    }

    dev->bci.first_coord = dev->bci.nslots;

    switch (cls) {
        case 0x09: /* rectangle */
            if (savage_src_bd(src) != BD_NONE)
                savage_add_slot(dev, P_SRC_XY);
            savage_add_slot(dev, P_DST_XY);
            savage_add_slot(dev, P_WH);
            if (pat == 3)
                savage_add_slot(dev, P_PAT0), savage_add_slot(dev, P_PAT1);
            break;
        case 0x0b: /* line */
            savage_add_slot(dev, P_DST_XY);
            savage_add_slot(dev, P_LINE_K);
            savage_add_slot(dev, P_LINE_E);
            break;
        default:
            break;
    }

    if (dev->bci.nslots)
        dev->bci.state = BCI_PARAMS;
    else
        savage_packet_done(dev);
}

static void
savage_param(savage_t *dev, uint32_t val)
{
    int slot = dev->bci.slots[dev->bci.slot++];

    dev->bci.p[slot] = val;

    switch (slot) {
        case P_SRC_BD_HI:
            savage_bd(dev, savage_src_bd(savage_src_type(dev->bci.cmd)))->offset = dev->bci.p[P_SRC_BD_LO];
            savage_bd(dev, savage_src_bd(savage_src_type(dev->bci.cmd)))->hi     = val;
            break;
        case P_DST_BD_HI:
            savage_bd(dev, savage_dst_bd(dev->bci.cmd))->offset = dev->bci.p[P_DST_BD_LO];
            savage_bd(dev, savage_dst_bd(dev->bci.cmd))->hi     = val;
            break;
        case P_PAT_BD_HI:
            savage_bd(dev, (savage_pat_type(dev->bci.cmd) >= 8) ? BD_SBD : BD_PBD)->offset = dev->bci.p[P_PAT_BD_LO];
            savage_bd(dev, (savage_pat_type(dev->bci.cmd) >= 8) ? BD_SBD : BD_PBD)->hi     = val;
            break;
        case P_CLIP_LR:
            dev->bci.clip_l = val & 0xfff;
            dev->bci.clip_r = (val >> 16) & 0xfff;
            dev->bci.clip_t = 0;
            dev->bci.clip_b = 0xfff;
            break;
        case P_CLIP_TL:
            dev->bci.clip_l = val & 0xfff;
            dev->bci.clip_t = (val >> 16) & 0xfff;
            break;
        case P_CLIP_BR:
            dev->bci.clip_r = val & 0xfff;
            dev->bci.clip_b = (val >> 16) & 0xfff;
            break;
        default:
            break;
    }

    if (dev->bci.slot >= dev->bci.nslots)
        savage_packet_done(dev);
}

static void
savage_text_dword(savage_t *dev, uint32_t val)
{
    switch (dev->bci.state) {
        case BCI_TEXT_XY:
            dev->bci.glyph_x = (int16_t) (val & 0xffff);
            dev->bci.glyph_y = (int16_t) (val >> 16);
            dev->bci.state   = BCI_TEXT_HEADER;
            break;

        case BCI_TEXT_HEADER:
            /* The DIB engine's glyph header, as the Win9x driver copies it:
               x origin, y origin (above the baseline), width, height. */
            dev->bci.glyph_x += (int8_t) (val & 0xff);
            dev->bci.glyph_y -= (int8_t) ((val >> 8) & 0xff);
            dev->bci.glyph_w         = (val >> 16) & 0xff;
            dev->bci.glyph_h         = (val >> 24) & 0xff;
            dev->bci.glyph_bitpacked = (savage_cmd_class(dev->bci.cmd) == 0x0d);
            if (dev->bci.glyph_bitpacked)
                dev->bci.glyph_dwords = (dev->bci.glyph_w * dev->bci.glyph_h + 31) >> 5;
            else
                dev->bci.glyph_dwords = (((dev->bci.glyph_w + 7) >> 3) * dev->bci.glyph_h + 3) >> 2;
            dev->bci.glyph_pos = 0;
            if (dev->bci.glyph_dwords)
                dev->bci.state = BCI_TEXT_DATA;
            else
                dev->bci.state = BCI_TEXT_XY;
            break;

        case BCI_TEXT_DATA:
            if ((dev->bci.glyph_pos * 4 + 4) <= sizeof(dev->bci.glyph_buf))
                *(uint32_t *) &dev->bci.glyph_buf[dev->bci.glyph_pos * 4] = val;
            if (++dev->bci.glyph_pos >= dev->bci.glyph_dwords) {
                savage_glyph(dev);
                dev->bci.state = BCI_TEXT_XY;
            }
            break;

        default:
            break;
    }
}

static void
savage_set_register(savage_t *dev, int index, uint32_t val)
{
    uint32_t addr;

    if ((index >= 0xa0) && (index <= 0xc6))
        addr = 0x8180 + ((index - 0xa0) << 2);
    else {
        switch (index) {
            case 0xd7:
                addr = 0x8128;
                break;
            case 0xd8:
                addr = 0x812c;
                break;
            case 0xe0:
                dev->gbd.offset = val;
                return;
            case 0xe1:
                dev->gbd.hi = val;
                return;
            case 0xe2:
                dev->pbd.offset = val;
                return;
            case 0xe3:
                dev->pbd.hi = val;
                return;
            case 0xe4:
                dev->sbd.offset = val;
                return;
            case 0xe5:
                dev->sbd.hi = val;
                return;
            default: /* 3D registers (texture, Z, destination, scissors) */
                dev->regs3d[index & 0xff] = val;
                return;
        }
    }

    s3_savage_mmio_writel_reg(dev, addr, val);
}

static void
s3_savage_bci_write(savage_t *dev, uint32_t val)
{
    dev->bci_hist[dev->bci_hist_pos & 255] = val;
    dev->bci_hist_state[dev->bci_hist_pos & 255] = dev->bci.state;
    dev->bci_hist_pos++;

    switch (dev->bci.state) {
        case BCI_PARAMS:
            savage_param(dev, val);
            return;

        case BCI_HOSTDATA:
            dev->bci.line_buf[dev->bci.dword++] = val;
            if (dev->bci.dword >= dev->bci.dwords_per_line) {
                savage_host_line(dev);
                dev->bci.dword = 0;
                if (++dev->bci.line >= dev->bci.h)
                    dev->bci.state = BCI_IDLE;
            }
            return;

        case BCI_TEXT_HEADER:
        case BCI_TEXT_DATA:
            savage_text_dword(dev, val);
            return;

        case BCI_TEXT_XY:
            if (!(val & 0xc0000000)) {
                savage_text_dword(dev, val);
                return;
            }
            break;

        case BCI_SETREG:
            savage_set_register(dev, dev->bci.setreg_index++, val);
            if (--dev->bci.setreg_count <= 0)
                dev->bci.state = BCI_IDLE;
            return;

        case BCI_3DDATA:
            if (--dev->bci.skip_count <= 0)
                dev->bci.state = BCI_IDLE;
            return;

        case BCI_SKIP:
            if (!(val & 0xc0000000))
                return;
            break;

        default:
            /* A parameter where a command is expected repeats the last 2D packet's coordinates. */
            if (!(val & 0xc0000000)) {
                int cls = savage_cmd_class(dev->bci.cmd);

                if (((cls == 0x09) || (cls == 0x0b)) && (dev->bci.first_coord < dev->bci.nslots)) {
                    dev->bci.slot  = dev->bci.first_coord;
                    dev->bci.state = BCI_PARAMS;
                    savage_param(dev, val);
                }
                return;
            }
            break;
    }

    dev->bci.state = BCI_IDLE;

    switch (val >> 27) {
        case 0x08 ... 0x0f: /* 2D */
            savage_start_command(dev, val);
            break;

        case 0x10: /* 3D draw primitive: X, Y and the vertex components not switched off */
            dev->bci.skip_count = ((val >> 16) & 0xff) * (8 - __builtin_popcount(val & 0x3f));
            if (dev->bci.skip_count)
                dev->bci.state = BCI_3DDATA;
            break;

        case 0x11: /* 3D draw indexed primitive: two 16-bit indices a dword */
            dev->bci.skip_count = (((val >> 16) & 0xff) + 1) >> 1;
            if (dev->bci.skip_count)
                dev->bci.state = BCI_3DDATA;
            break;

        case 0x12: /* set registers */
            dev->bci.setreg_index = val & 0xff;
            dev->bci.setreg_count = (val >> 16) & 0xff;
            if (dev->bci.setreg_count)
                dev->bci.state = BCI_SETREG;
            break;

        case 0x13: /* update the event tag */
            dev->event_tag = val & 0xffff;
            s3_savage_update_shadow(dev);
            break;

        case 0x14: /* queued page flip: the registers are already set */
        case 0x18: /* wait for idle */
            break;

        default:
            s3_savage_log("BCI unknown command %08x\n", val);
            dev->bci.state = BCI_SKIP;
            break;
    }
}

/* ------------------------------------------------------------------------ */
/* PCI                                                                      */
/* ------------------------------------------------------------------------ */

static uint8_t
s3_savage_pci_read(UNUSED(int func), int addr, UNUSED(int len), void *priv)
{
    const savage_t *dev = (savage_t *) priv;
    uint8_t         ret = 0x00;

    switch (addr) {
        case 0x00:
            ret = 0x33;
            break;
        case 0x01:
            ret = 0x53;
            break;
        case 0x02:
            ret = dev->device_id & 0xff;
            break;
        case 0x03:
            ret = dev->device_id >> 8;
            break;

        case PCI_REG_COMMAND:
            ret = dev->pci_regs[PCI_REG_COMMAND] & 0x27;
            break;
        case 0x06:
            ret = PCI_STATUS_L_CAPAB;
            break;
        case 0x07:
            ret = dev->pci_regs[0x07] & 0x36;
            break;

        case 0x08:
            ret = 0x00; /* revision */
            break;
        case 0x0b:
            ret = 0x03; /* display controller */
            break;
        case 0x0d:
            ret = dev->pci_regs[0x0d] & 0xf8;
            break;

        case 0x10:
            ret = 0x08; /* memory, prefetchable */
            break;
        case 0x13:
            ret = dev->bar0 >> 24;
            break;

        case 0x2c:
            ret = 0x33;
            break;
        case 0x2d:
            ret = 0x53;
            break;
        case 0x2e:
            ret = dev->device_id & 0xff;
            break;
        case 0x2f:
            ret = dev->device_id >> 8;
            break;

        case 0x30:
            ret = dev->pci_regs[0x30] & 0x01;
            break;
        case 0x32:
            ret = dev->pci_regs[0x32];
            break;
        case 0x33:
            ret = dev->pci_regs[0x33];
            break;

        case 0x34:
            ret = 0xdc;
            break;

        case 0x3c:
            ret = dev->pci_regs[0x3c];
            break;
        case 0x3d:
            ret = PCI_INTA;
            break;
        case 0x3e:
            ret = 0x04;
            break;
        case 0x3f:
            ret = 0xff;
            break;

        case 0x80:
            ret = 0x02; /* AGP capability */
            break;
        case 0x82:
            ret = 0x10; /* AGP 1.0 */
            break;
        case 0x84:
            ret = 0x03; /* 1x and 2x */
            break;
        case 0x87:
            ret = 0x1f;
            break;
        case 0x88:
        case 0x89:
        case 0x8a:
        case 0x8b:
            ret = dev->pci_regs[addr];
            break;

        case 0xdc:
            ret = 0x01; /* power management capability */
            break;
        case 0xdd:
            ret = dev->is_agp ? 0x80 : 0x00;
            break;
        case 0xde:
            ret = 0x21;
            break;
        case 0xe0:
        case 0xe1:
        case 0xe2:
        case 0xe3:
            ret = dev->pci_regs[addr];
            break;

        default:
            break;
    }

    return ret;
}

static void
s3_savage_pci_write(UNUSED(int func), int addr, UNUSED(int len), uint8_t val, void *priv)
{
    savage_t *dev  = (savage_t *) priv;
    svga_t   *svga = &dev->svga;

    switch (addr) {
        case PCI_REG_COMMAND:
            io_removehandler(0x03c0, 0x0020, s3_savage_in, NULL, NULL, s3_savage_out, NULL, NULL, dev);
            if (val & PCI_COMMAND_IO)
                io_sethandler(0x03c0, 0x0020, s3_savage_in, NULL, NULL, s3_savage_out, NULL, NULL, dev);
            dev->pci_regs[PCI_REG_COMMAND] = val & 0x27;
            s3_savage_updatemapping(dev);
            break;
        case 0x07:
            dev->pci_regs[0x07] &= ~(val & 0x30);
            break;
        case 0x0d:
            dev->pci_regs[0x0d] = val & 0xf8;
            break;

        case 0x13:
            dev->bar0        = (val & 0xf8) << 24;
            svga->crtc[0x59] = val & 0xf8;
            s3_savage_updatemapping(dev);
            break;

        case 0x30:
        case 0x32:
        case 0x33:
            dev->pci_regs[addr] = val;
            if (dev->pci_regs[0x30] & 0x01) {
                uint32_t biosaddr = (dev->pci_regs[0x32] << 16) | (dev->pci_regs[0x33] << 24);

                mem_mapping_set_addr(&dev->bios_rom.mapping, biosaddr & ~0xffff, 0x10000);
            } else
                mem_mapping_disable(&dev->bios_rom.mapping);
            break;

        case 0x3c:
            dev->pci_regs[0x3c] = val;
            break;

        case 0x88:
            dev->pci_regs[0x88] = val & 0x27;
            break;
        case 0x89:
            dev->pci_regs[0x89] = val & 0x03;
            break;
        case 0x8b:
        case 0xe1:
        case 0xe3:
            dev->pci_regs[addr] = val;
            break;
        case 0xe0:
            dev->pci_regs[0xe0] = val & 0x03;
            break;
        case 0xe2:
            dev->pci_regs[0xe2] = val & 0xc0;
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------------------ */
/* Device                                                                   */
/* ------------------------------------------------------------------------ */

static void
s3_savage_reset_regs(savage_t *dev)
{
    svga_t *svga = &dev->svga;

    memset(dev->streams, 0, sizeof(dev->streams));
    memset(dev->tiled_surf, 0, sizeof(dev->tiled_surf));
    memset(&dev->bci, 0, sizeof(dev->bci));
    memset(&dev->gbd, 0, sizeof(dev->gbd));
    memset(&dev->pbd, 0, sizeof(dev->pbd));
    memset(&dev->sbd, 0, sizeof(dev->sbd));
    dev->write_mask  = 0xffffffff;
    dev->read_mask   = 0xffffffff;
    dev->bci_ctrl    = 0;
    dev->shadow_ctrl = 0;
    dev->event_tag   = 0;
    dev->bank        = 0;
    dev->ma_ext      = 0;

    /* CR36: 8 MB, bits 7:6 are set by the BIOS memory test. */
    svga->crtc[0x36] = 0x00;
    /* CR37 bit 6: AGP bus. */
    svga->crtc[0x37] = 0x01 | (dev->is_agp ? 0x40 : 0x00) | 0x80;
    svga->crtc[0x53] = 0x00;
    svga->crtc[0x59] = dev->bar0 >> 24;

    dev->pci_regs[PCI_REG_COMMAND] = 0x07;
    dev->pci_regs[0x07]            = 0x02;
    dev->pci_regs[0x32]            = 0x0c;
    dev->pci_regs[0x3d]            = 0x01;
}

static savage_t *savage_debug_dev;

/* "dev" in the debug command file: the BCI parser's state and its last dwords. */
static void
s3_savage_debug_hook(UNUSED(const char *args))
{
    savage_t *dev = savage_debug_dev;

    if (!dev)
        return;

    pclog("Savage BCI: state %d cmd %08x slot %d/%d glyph %dx%d %u/%u host line %d/%d dword %d/%d\n",
          dev->bci.state, dev->bci.cmd, dev->bci.slot, dev->bci.nslots, dev->bci.glyph_w, dev->bci.glyph_h,
          dev->bci.glyph_pos, dev->bci.glyph_dwords, dev->bci.line, dev->bci.h, dev->bci.dword,
          dev->bci.dwords_per_line);
    pclog("Savage GBD %08x %08x PBD %08x %08x SBD %08x %08x\n", dev->gbd.offset, dev->gbd.hi,
          dev->pbd.offset, dev->pbd.hi, dev->sbd.offset, dev->sbd.hi);
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t n = dev->bci_hist_pos - 256 + i;

        if ((int32_t) n < 0)
            continue;
        pclog("Savage BCI -%3u: %08x (state %d)\n", 256 - i, dev->bci_hist[n & 255], dev->bci_hist_state[n & 255]);
    }
}

static void *
s3_savage_init(const device_t *info)
{
    savage_t *dev = (savage_t *) calloc(1, sizeof(savage_t));

    dev->is_agp    = !!(info->flags & DEVICE_AGP);
    dev->device_id = 0x8a21; /* 86C390 (the BIOS looks itself up by this ID) */

    rom_init(&dev->bios_rom, ROM_STB_NITRO_3200, 0xc0000, 0x10000, 0xffff, 0, MEM_MAPPING_EXTERNAL);
    mem_mapping_disable(&dev->bios_rom.mapping);

    svga_init(info, &dev->svga, dev, SAVAGE_VRAM_SIZE,
              s3_savage_recalctimings,
              s3_savage_in, s3_savage_out,
              s3_savage_hwcursor_draw,
              NULL);
    dev->svga.hwcursor.cur_ysize = 64;
    dev->svga.decode_mask        = BAR0_MMIO - 1;
    dev->svga.force_old_addr     = 1;

    video_inform(VIDEO_FLAG_TYPE_SPECIAL, dev->is_agp ? &timing_savage_agp : &timing_savage_pci);

    mem_mapping_add(&dev->linear_mapping, 0, 0,
                    svga_read_linear, svga_readw_linear, svga_readl_linear,
                    svga_write_linear, svga_writew_linear, svga_writel_linear,
                    NULL, MEM_MAPPING_EXTERNAL, &dev->svga);
    mem_mapping_add(&dev->mmio_mapping, 0, 0,
                    s3_savage_mmio_read, s3_savage_mmio_readw, s3_savage_mmio_readl,
                    s3_savage_mmio_write, s3_savage_mmio_writew, s3_savage_mmio_writel,
                    NULL, MEM_MAPPING_EXTERNAL, dev);
    mem_mapping_add(&dev->aperture_mapping, 0, 0,
                    s3_savage_aperture_read, s3_savage_aperture_readw, s3_savage_aperture_readl,
                    s3_savage_aperture_write, s3_savage_aperture_writew, s3_savage_aperture_writel,
                    NULL, MEM_MAPPING_EXTERNAL, dev);
    mem_mapping_add(&dev->old_mmio_mapping, 0, 0,
                    s3_savage_old_mmio_read, s3_savage_old_mmio_readw, s3_savage_old_mmio_readl,
                    s3_savage_old_mmio_write, s3_savage_old_mmio_writew, s3_savage_old_mmio_writel,
                    NULL, MEM_MAPPING_EXTERNAL, dev);
    mem_mapping_disable(&dev->linear_mapping);
    mem_mapping_disable(&dev->mmio_mapping);
    mem_mapping_disable(&dev->aperture_mapping);
    mem_mapping_disable(&dev->old_mmio_mapping);

    io_sethandler(0x03c0, 0x0020, s3_savage_in, NULL, NULL, s3_savage_out, NULL, NULL, dev);

    s3_savage_reset_regs(dev);

    pci_add_card(dev->is_agp ? PCI_ADD_AGP : PCI_ADD_NORMAL, s3_savage_pci_read, s3_savage_pci_write, dev, &dev->pci_slot);

    dev->i2c = i2c_gpio_init("ddc_s3_savage");
    dev->ddc = ddc_init(i2c_gpio_get_bus(dev->i2c));

    savage_debug_dev = dev;
    debug_cmd_set_device_hook(s3_savage_debug_hook);

    return dev;
}

static void
s3_savage_close(void *priv)
{
    savage_t *dev = (savage_t *) priv;

    if (savage_debug_dev == dev) {
        debug_cmd_set_device_hook(NULL);
        savage_debug_dev = NULL;
    }

    svga_close(&dev->svga);

    ddc_close(dev->ddc);
    i2c_gpio_close(dev->i2c);

    free(dev);
}

static void
s3_savage_reset(void *priv)
{
    savage_t *dev = (savage_t *) priv;

    s3_savage_reset_regs(dev);
    s3_savage_updatemapping(dev);
}

static int
s3_savage_available(void)
{
    return rom_present(ROM_STB_NITRO_3200);
}

static void
s3_savage_speed_changed(void *priv)
{
    savage_t *dev = (savage_t *) priv;

    svga_recalctimings(&dev->svga);
}

static void
s3_savage_force_redraw(void *priv)
{
    savage_t *dev = (savage_t *) priv;

    dev->svga.fullchange = changeframecount;
}

const device_t s3_savage3d_stb_nitro3200_agp_device = {
    .name          = "S3 Savage3D (STB Nitro 3200) AGP",
    .internal_name = "stb_nitro3200_agp",
    .flags         = DEVICE_AGP,
    .local         = 0,
    .init          = s3_savage_init,
    .close         = s3_savage_close,
    .reset         = s3_savage_reset,
    .available     = s3_savage_available,
    .speed_changed = s3_savage_speed_changed,
    .force_redraw  = s3_savage_force_redraw,
    .config        = NULL
};

const device_t s3_savage3d_stb_nitro3200_pci_device = {
    .name          = "S3 Savage3D (STB Nitro 3200) PCI",
    .internal_name = "stb_nitro3200_pci",
    .flags         = DEVICE_PCI,
    .local         = 0,
    .init          = s3_savage_init,
    .close         = s3_savage_close,
    .reset         = s3_savage_reset,
    .available     = s3_savage_available,
    .speed_changed = s3_savage_speed_changed,
    .force_redraw  = s3_savage_force_redraw,
    .config        = NULL
};
