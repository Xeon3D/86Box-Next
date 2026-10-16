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
#include <math.h>
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
    BCI_3DIDX,       /* indices of an indexed 3D primitive */
    BCI_3DVERTS,     /* vertices of a 3D primitive */
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
    uint32_t disp_base; /* primary stream start, latched each frame */

    /* Secondary stream (overlay), latched each frame. */
    void (*primary_render)(struct svga_t *svga);
    struct {
        int      x, y, w, h;   /* window on the screen */
        int      src_w, lines; /* source pixels a line, source lines */
        uint32_t base, stride;
        uint32_t hstep, vstep; /* source step per screen pixel/line, 1.15 */
        uint32_t vinit;
        int      fmt, hfilter, vfilter;
    } ov;

    /* MM8504: interrupt status 7:0 (bit 0 vertical sync), enables 15:8. */
    uint8_t subsys_stat;
    uint8_t subsys_cntl;

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

        /* 3D primitive being received. */
        uint32_t prim_cmd;
        int      prim_vert_dwords;
        int      prim_dword;
        uint32_t prim_buf[8];
    } bci;

    /* 3D: the vertices of the triangle being assembled (strips and fans carry
       two over to the next packet when its continue bit is set). */
    struct {
        float    x, y, z, w;
        uint32_t diff, spec;
        float    u, v;
    } vtx[4];
    int strip_odd;
    int      nvtx;
    int      prim_count;
    int      mip_levels;
    uint32_t mip_off[12];
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
static int     savage_bci_log;
static void    s3_savage_bci_write(savage_t *dev, uint32_t val);
static uint8_t s3_savage_in(uint16_t addr, void *priv);
static void    s3_savage_out(uint16_t addr, uint8_t val, void *priv);

/* ------------------------------------------------------------------------ */
/* Tiled memory                                                             */
/* ------------------------------------------------------------------------ */

/* Tiled surfaces are made of 2 KB tiles: 64x32 pixels at 8 bpp, 64x16 at
   16 bpp, 32x16 at 32 bpp. A tile holds 8x8-pixel blocks in rows; a block
   holds two columns 4 pixels wide, each 8 rows top to bottom. (This is the
   layout the Win9x driver's texture code assumes: mip levels are allocated
   in 8x8 blocks, and the smallest levels share one block.) */
static inline int
savage_tile_w(int bytespp)
{
    return (bytespp == 4) ? 32 : 64;
}

static inline int
savage_tile_h(int bytespp)
{
    return (bytespp == 1) ? 32 : 16;
}

/* Offset of pixel (px, y) inside its 8x8 block. */
static inline uint32_t
savage_block_off(int bytespp, uint32_t px, uint32_t y)
{
    return ((((px >> 2) & 1) << 5) + ((y & 7) << 2) + (px & 3)) * bytespp;
}

/* Address of byte xb on line y of a tiled surface with tiles_per_row tiles. */
static inline uint32_t
savage_tiled_addr(uint32_t base, int tiles_per_row, uint32_t xb, uint32_t y, int bytespp)
{
    int      shift = (bytespp == 4) ? 2 : (bytespp - 1);
    uint32_t px    = xb >> shift;
    int      tw    = savage_tile_w(bytespp);
    int      th    = savage_tile_h(bytespp);
    uint32_t tile  = (y / th) * tiles_per_row + px / tw;
    uint32_t blk   = (((y % th) >> 3) * (tw >> 3)) + ((px % tw) >> 3);

    return base + (tile << 11) + blk * (bytespp << 6) + savage_block_off(bytespp, px, y) + (xb & (bytespp - 1));
}

/* Tiled surface registers (MM48C40-48C54): base 23:0, width in tiles 29:24,
   bits per pixel 31:30 (1 = 8, 2 = 16, 3 = 32). */
static uint32_t
savage_aperture_addr(savage_t *dev, uint32_t addr)
{
    static const int bpp[4] = { 2, 1, 2, 4 };
    uint32_t         surf   = addr >> 24;
    uint32_t         reg    = (surf < 6) ? dev->tiled_surf[surf] : 0;
    int              tiles  = (reg >> 24) & 0x3f;
    uint32_t         y      = (addr >> 13) & 0x7ff;
    uint32_t         xb     = addr & 0x1fff;

    if (!tiles)
        tiles = 1;

    return savage_tiled_addr(reg & 0xffffff, tiles, xb, y, bpp[reg >> 30]) & SAVAGE_VRAM_MASK;
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
    uint32_t  base;
    int       y      = svga->displine;
    uint32_t *p;

    /* The streams processor takes a new start address (a page flip) at the
       start of a frame, not in the middle of one. */
    if (svga->displine == 0)
        dev->disp_base = dev->streams[((dev->streams[(0x81cc - 0x8180) >> 2] & 1) ? (0x81c4 - 0x8180) : (0x81c0 - 0x8180)) >> 2] & 0x7ffff8;
    base = dev->disp_base;

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
        uint32_t va = savage_tiled_addr(base, tiles, x * bytespp, y, bytespp) & SAVAGE_VRAM_MASK;

        /* Past the tile row's end the next tile row would show; past the
           display width, the padding beyond it. */
        if ((x >= (tiles * savage_tile_w(bytespp))) || (x >= svga->hdisp))
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

/* ------------------------------------------------------------------------ */
/* Streams processor: the secondary stream (overlay)                        */
/* ------------------------------------------------------------------------ */

/* Secondary stream registers, as S3's GX3 DirectDraw driver (DDOVER.C,
   S3DATA.H) programs them. The scale factors are source steps per screen
   pixel or line in 1.15 fixed point, not the ViRGE's K1/K2 DDA terms. */
#define SS_CKEY_LOW  0x8184 /* key 23:0 (R/Y, G/U, B/V), compared bits 26:24 */
#define SS_CONTROL   0x8190 /* source width 11:0, format 26:24, filter 30:28 */
#define SS_CKEY_HI   0x8194 /* upper bounds of a range key */
#define SS_HSCALE    0x8198
#define SS_COLOR_ADJ 0x819c /* brightness 7:0, contrast 12:8 (1.4), on 15; hue/saturation 20:16, 28:24 (s1.3), on 31 */
#define SS_BLEND     0x81a0 /* Ks 5:2, Kp 13:10, compose 26:24 */
#define SS_DBL_BUF   0x81cc
#define SS_FIFO      0x81ec /* bit 0: horizontal interpolation (the GX3 driver's choice) */ /* primary buffer 0, secondary buffer 2:1 */
#define SS_ADDR0     0x81d0
#define SS_ADDR1     0x81d4
#define SS_STRIDE    0x81d8
#define SS_VSCALE    0x81e0
#define SS_VINIT     0x81e4
#define SS_LINES     0x81e8 /* source lines 11:0, vertical filter 15 */
#define SS_XY        0x81f8 /* x + 1 26:16, y + 1 10:0 */
#define SS_WH        0x81fc /* width - 1 26:16, height 10:0 */

#define SREG(dev, a) ((dev)->streams[((a) - 0x8180) >> 2])

/* Take the overlay registers for the frame about to be shown. */
static void
savage_overlay_latch(savage_t *dev)
{
    uint32_t xy   = SREG(dev, SS_XY);
    uint32_t wh   = SREG(dev, SS_WH);
    uint32_t ctrl = SREG(dev, SS_CONTROL);

    dev->ov.x       = (int) ((xy >> 16) & 0x7ff) - 1;
    dev->ov.y       = (int) (xy & 0x7ff) - 1;
    dev->ov.w       = (int) ((wh >> 16) & 0x7ff) + 1;
    dev->ov.h       = (int) (wh & 0x7ff);
    dev->ov.src_w   = ctrl & 0xfff;
    dev->ov.fmt     = (ctrl >> 24) & 7;
    dev->ov.hfilter = !!((ctrl >> 28) & 7) || (SREG(dev, SS_FIFO) & 1);
    dev->ov.lines   = SREG(dev, SS_LINES) & 0xfff;
    dev->ov.vfilter = !!(SREG(dev, SS_LINES) & 0x8000);
    dev->ov.hstep   = SREG(dev, SS_HSCALE) & 0x1ffff;
    dev->ov.vstep   = SREG(dev, SS_VSCALE) & 0x1ffff;
    dev->ov.vinit   = SREG(dev, SS_VINIT) & 0x7fff;
    dev->ov.stride  = SREG(dev, SS_STRIDE) & 0x1fff;
    dev->ov.base    = SREG(dev, (((SREG(dev, SS_DBL_BUF) >> 1) & 3) == 1) ? SS_ADDR1 : SS_ADDR0) & SAVAGE_VRAM_MASK & ~7;

    if (!dev->ov.src_w)
        dev->ov.src_w = dev->ov.w;
    if (!dev->ov.lines)
        dev->ov.lines = 2048;
    if (!dev->ov.hstep)
        dev->ov.hstep = 0x8000;
    if (!dev->ov.vstep)
        dev->ov.vstep = 0x8000;
}

static inline int
savage_clamp8(int v)
{
    return (v < 0) ? 0 : ((v > 255) ? 255 : v);
}

static inline uint8_t
savage_ov_byte(savage_t *dev, uint32_t addr)
{
    return dev->svga.vram[addr & SAVAGE_VRAM_MASK];
}

/* Pixel x of the source line at addr as RGB888. *key gets what a source key
   compares: Y/U/V for the YUV formats (after the colour adjustment), else
   R/G/B. Formats: 0 YVYU, 1 YUY2 (CCIR 601 levels), 2 YUY2 (full range),
   3 RGB 1555, 4 Y211, 5 RGB 565, 6 RGB 888, 7 XRGB 8888. */
static uint32_t
savage_overlay_pixel(savage_t *dev, uint32_t addr, int x, uint32_t *key)
{
    uint32_t adj = SREG(dev, SS_COLOR_ADJ);
    uint32_t c;
    int      y, u, v, r, g, b;

    switch (dev->ov.fmt) {
        case 3:
            c = savage_ov_byte(dev, addr + x * 2) | (savage_ov_byte(dev, addr + x * 2 + 1) << 8);
            r = (c >> 10) & 0x1f;
            g = (c >> 5) & 0x1f;
            b = c & 0x1f;
            c = (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
            *key = c;
            return c;
        case 5:
            c = savage_ov_byte(dev, addr + x * 2) | (savage_ov_byte(dev, addr + x * 2 + 1) << 8);
            r = (c >> 11) & 0x1f;
            g = (c >> 5) & 0x3f;
            b = c & 0x1f;
            c = (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
            *key = c;
            return c;
        case 6:
            c = savage_ov_byte(dev, addr + x * 3) | (savage_ov_byte(dev, addr + x * 3 + 1) << 8) |
                (savage_ov_byte(dev, addr + x * 3 + 2) << 16);
            *key = c;
            return c;
        case 7:
            c = (savage_ov_byte(dev, addr + x * 4) | (savage_ov_byte(dev, addr + x * 4 + 1) << 8) |
                 (savage_ov_byte(dev, addr + x * 4 + 2) << 16));
            *key = c;
            return c;
        case 4: /* Y211: Y0 U Y2 V for four pixels */
            addr += (x >> 2) << 2;
            y = savage_ov_byte(dev, addr + ((x & 2) ? 2 : 0));
            u = savage_ov_byte(dev, addr + 1);
            v = savage_ov_byte(dev, addr + 3);
            break;
        case 0: /* Y0 V Y1 U */
            addr += (x >> 1) << 2;
            y = savage_ov_byte(dev, addr + ((x & 1) << 1));
            v = savage_ov_byte(dev, addr + 1);
            u = savage_ov_byte(dev, addr + 3);
            break;
        default: /* Y0 U Y1 V */
            addr += (x >> 1) << 2;
            y = savage_ov_byte(dev, addr + ((x & 1) << 1));
            u = savage_ov_byte(dev, addr + 1);
            v = savage_ov_byte(dev, addr + 3);
            break;
    }

    /* Colour adjustment (MM819C): Y' = Y * contrast + brightness; U/V
       rotated and scaled by the hue/saturation factors (8 = unity). */
    if (adj & 0x8000)
        y = savage_clamp8(((y * (int) ((adj >> 8) & 0x1f)) >> 4) + (int8_t) (adj & 0xff));
    if (adj & 0x80000000) {
        int f1 = (int) ((adj >> 16) & 0x1f);
        int f2 = (int) ((adj >> 24) & 0x1f);
        int du = u - 128;
        int dv = v - 128;

        if (f1 >= 16)
            f1 -= 32;
        if (f2 >= 16)
            f2 -= 32;
        u = savage_clamp8(128 + ((f1 * du - f2 * dv) >> 3));
        v = savage_clamp8(128 + ((f2 * du + f1 * dv) >> 3));
    }
    *key = (y << 16) | (u << 8) | v;

    u -= 128;
    v -= 128;
    if (dev->ov.fmt == 2) {
        r = y + ((359 * v) >> 8);
        g = y - ((88 * u + 183 * v) >> 8);
        b = y + ((454 * u) >> 8);
    } else {
        y = 298 * (y - 16);
        r = (y + 409 * v + 128) >> 8;
        g = (y - 100 * u - 208 * v + 128) >> 8;
        b = (y + 516 * u + 128) >> 8;
    }

    return (savage_clamp8(r) << 16) | (savage_clamp8(g) << 8) | savage_clamp8(b);
}

static inline uint32_t
savage_lerp_rgb(uint32_t a, uint32_t b, int f)
{
    uint32_t out = 0;

    for (int sh = 0; sh < 24; sh += 8)
        out |= ((((a >> sh) & 0xff) * (256 - f) + ((b >> sh) & 0xff) * f) >> 8) << sh;

    return out;
}

/* A key compare on the top bits (CKEY_LOW 26:24: 0 compares bit 7 only, 7
   all eight) of each component, against the range CKEY_LOW..CKEY_HI (just
   CKEY_LOW when the upper bound is below it). A YUV source's chroma key
   compares whole bytes: the driver leaves the bit count at 0 for those. */
static int
savage_key_match(uint32_t c, uint32_t lo, uint32_t hi, int whole)
{
    uint8_t mask = whole ? 0xff : (uint8_t) (0xff << (7 - ((lo >> 24) & 7)));

    for (int sh = 0; sh < 24; sh += 8) {
        uint8_t cv = ((c >> sh) & 0xff) & mask;
        uint8_t l  = ((lo >> sh) & 0xff) & mask;
        uint8_t h  = ((hi >> sh) & 0xff) & mask;

        if (h < l)
            h = l;
        if ((cv < l) || (cv > h))
            return 0;
    }

    return 1;
}

/* Compose the secondary stream into the screen line just drawn. Blend
   control 26:24: 0 secondary on primary, 1 primary on secondary, 2 dissolve
   (Kp/8 primary, the rest secondary), 3 fade (Kp/8 primary + Ks/8
   secondary), 5 secondary where the primary matches the key, 6 secondary
   except where it matches the key. */
static void
savage_overlay_line(savage_t *dev)
{
    svga_t  *svga  = &dev->svga;
    int      row   = svga->displine - dev->ov.y;
    uint32_t blend = SREG(dev, SS_BLEND);
    int      mode  = (blend >> 24) & 7;
    int      kp    = MIN((blend >> 10) & 0xf, 8);
    int      ks    = MIN((blend >> 2) & 0xf, 8);
    uint32_t lo    = SREG(dev, SS_CKEY_LOW);
    uint32_t hi    = SREG(dev, SS_CKEY_HI);
    uint32_t pmask = 0xffffff;
    uint64_t fy;
    uint32_t a0, a1;
    int      sl, vf, x0, x1;
    uint32_t *p;

    if ((row < 0) || (row >= dev->ov.h) || (mode == 1) || (dev->ov.x >= svga->hdisp))
        return;
    if (((svga->displine + svga->y_add) < 0) || (svga->monitor->target_buffer == NULL) ||
        (svga->monitor->target_buffer->line[svga->displine + svga->y_add] == NULL))
        return;

    fy = dev->ov.vinit + (uint64_t) row * dev->ov.vstep;
    sl = (int) (fy >> 15);
    vf = dev->ov.vfilter ? (int) ((fy >> 7) & 0xff) : 0;
    if (sl >= (dev->ov.lines - 1)) {
        sl = dev->ov.lines - 1;
        vf = 0;
    }
    a0 = dev->ov.base + sl * dev->ov.stride;
    a1 = a0 + dev->ov.stride;

    /* The key compares the primary pixel's own bits, not the screen's
       expansion of them: the driver gives 565 magenta as F800F8. */
    if (svga->bpp == 15)
        pmask = 0xf8f8f8;
    else if (svga->bpp == 16)
        pmask = 0xf8fcf8;
    else if ((svga->bpp == 8) && !(svga->seqregs[0x1b] & 0x10))
        pmask = 0xfcfcfc;

    x0 = MAX(dev->ov.x, 0);
    x1 = MIN(dev->ov.x + dev->ov.w, svga->hdisp);
    p  = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

    for (int x = x0; x < x1; x++) {
        uint32_t fx = (uint32_t) (x - dev->ov.x) * dev->ov.hstep;
        int      sx = (int) (fx >> 15);
        int      hf = dev->ov.hfilter ? (int) ((fx >> 7) & 0xff) : 0;
        uint32_t key, dummy, s, pc = p[x] & 0xffffff;

        if (sx >= (dev->ov.src_w - 1)) {
            sx = dev->ov.src_w - 1;
            hf = 0;
        }

        s = savage_overlay_pixel(dev, a0, sx, &key);
        if (hf)
            s = savage_lerp_rgb(s, savage_overlay_pixel(dev, a0, sx + 1, &dummy), hf);
        if (vf) {
            uint32_t s1 = savage_overlay_pixel(dev, a1, sx, &dummy);

            if (hf)
                s1 = savage_lerp_rgb(s1, savage_overlay_pixel(dev, a1, sx + 1, &dummy), hf);
            s = savage_lerp_rgb(s, s1, vf);
        }

        switch (mode) {
            case 2:
            case 3:
                {
                    int      ws  = (mode == 2) ? (8 - kp) : ks;
                    uint32_t out = 0;

                    for (int sh = 0; sh < 24; sh += 8)
                        out |= (uint32_t) MIN((((pc >> sh) & 0xff) * kp + ((s >> sh) & 0xff) * ws) >> 3, 255) << sh;
                    s = out;
                }
                break;
            case 5:
                if (!savage_key_match(pc & pmask, lo, hi, 0))
                    continue;
                break;
            case 6:
                if (savage_key_match(key, lo, hi, (dev->ov.fmt != 3) && (dev->ov.fmt < 5)))
                    continue;
                break;
            default:
                break;
        }

        p[x] = s;
    }
}

/* The primary stream's renderer, then the secondary stream over it. While
   an overlay is up every line is redrawn, so that keying always looks at
   the primary stream rather than last frame's mix. */
static void
savage_render_streams(svga_t *svga)
{
    savage_t *dev = (savage_t *) svga->priv;

    if (svga->displine == 0) {
        savage_overlay_latch(dev);
        if ((dev->ov.h > 0) && (dev->ov.x < svga->hdisp) && (dev->ov.y < svga->dispend))
            svga->fullchange = changeframecount;
    }

    dev->primary_render(svga);
    if (dev->ov.h > 0)
        savage_overlay_line(dev);
}

/* ------------------------------------------------------------------------ */
/* Interrupts                                                               */
/* ------------------------------------------------------------------------ */

/* MM8504 status bits, as on the ViRGE: S3's mini-VDD (KMVT.ASM) enables
   bit 8 for DirectDraw's vertical sync interrupt, with CR32 bit 4 as the
   master enable, and acknowledges by writing the status bits back. */
#define SUBSYS_VSY      0x01
#define SUBSYS_GE_DONE  0x02
#define SUBSYS_FIFO_EMP 0x08

static void
s3_savage_update_irq(savage_t *dev)
{
    if ((dev->svga.crtc[0x32] & 0x10) && (dev->subsys_stat & dev->subsys_cntl))
        pci_set_irq(dev->pci_slot, PCI_INTA, &dev->irq_state);
    else
        pci_clear_irq(dev->pci_slot, PCI_INTA, &dev->irq_state);
}

static void
s3_savage_vblank_start(svga_t *svga)
{
    savage_t *dev = (savage_t *) svga->priv;

    dev->subsys_stat |= SUBSYS_VSY;
    s3_savage_update_irq(dev);
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

        if (svga->render != savage_render_streams) {
            dev->primary_render = svga->render;
            svga->render        = savage_render_streams;
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
                    s3_savage_update_irq(dev);
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
        case 0x8504:
            return dev->subsys_stat | (dev->subsys_cntl << 8);
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

    if (savage_bci_log)
        pclog("MMW %05x %08x\n", addr, val);

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
        case 0x8504: /* status bits written as 1 are acknowledged; 15:8 are the enables */
            dev->subsys_stat &= ~(val & 0xff);
            dev->subsys_cntl = (val >> 8) & 0xff;
            s3_savage_update_irq(dev);
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

    if ((addr >= 0x10000) && (addr < 0x30000)) {
        if (savage_bci_log)
            pclog("BCIB %05x %02x\n", addr, val);
        return; /* The BCI only takes dwords. */
    }

    if (addr == 0x8504) {
        dev->subsys_stat &= ~val;
        s3_savage_update_irq(dev);
        return;
    }
    if (addr == 0x8505) {
        dev->subsys_cntl = val;
        s3_savage_update_irq(dev);
        return;
    }

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

        /* The BCI runs each dword as it arrives: its FIFO is always empty,
           and the engine is done whenever no command is half received. */
        dev->subsys_stat |= SUBSYS_FIFO_EMP | ((dev->bci.state == BCI_IDLE) ? SUBSYS_GE_DONE : 0);
        if (dev->subsys_cntl & (SUBSYS_FIFO_EMP | SUBSYS_GE_DONE))
            s3_savage_update_irq(dev);
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

    if ((off >= 0x10000) && (off < 0x30000)) {
        if (savage_bci_log)
            pclog("BCIW %05x %04x\n", off, val);
        return;
    }

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
        int tiles = (int) ((stride + savage_tile_w(bytespp) - 1) / savage_tile_w(bytespp));

        return savage_tiled_addr(bd->offset, tiles, x * bytespp, y, bytespp) & SAVAGE_VRAM_MASK;
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

/* ------------------------------------------------------------------------ */
/* 3D engine (GX3): DrawPrimitive triangles                                 */
/* ------------------------------------------------------------------------ */

/* 3D register indices (SetRegister), from S3's GX3PI.H. */
#define R3D_TEX_PAL_ADDR 0x18
#define R3D_TEX_XPR_CLR  0x19
#define R3D_TEX_ADDR     0x1a
#define R3D_TEX_DESCR    0x1b
#define R3D_TEX_CTRL     0x1c
#define R3D_DRAW_CTRL    0x31
#define R3D_ZBUF_CTRL    0x32
#define R3D_ZBUF_OFFSET  0x33
#define R3D_DST_CTRL     0x34
#define R3D_SCISSOR_TL   0x35
#define R3D_SCISSOR_BR   0x36

static inline float
savage_float(uint32_t val)
{
    float f;

    memcpy(&f, &val, sizeof(f));
    return f;
}

static inline uint32_t
savage_565_to_argb(uint32_t c)
{
    uint32_t r = (c >> 11) & 0x1f;
    uint32_t g = (c >> 5) & 0x3f;
    uint32_t b = c & 0x1f;

    return 0xff000000 | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

static inline uint32_t
savage_1555_to_argb(uint32_t c)
{
    uint32_t r = (c >> 10) & 0x1f;
    uint32_t g = (c >> 5) & 0x1f;
    uint32_t b = c & 0x1f;

    return ((c & 0x8000) ? 0xff000000 : 0) | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
}

static inline uint32_t
savage_4444_to_argb(uint32_t c)
{
    return (((c >> 12) & 0xf) * 0x11000000) | (((c >> 8) & 0xf) * 0x110000) | (((c >> 4) & 0xf) * 0x1100) | ((c & 0xf) * 0x11);
}

/* Texture and palette memory: an offset in video memory, or a bus address in
   system (AGP) memory. */
static inline uint32_t
savage_3d_read(savage_t *dev, int sys, uint32_t addr, int bytes)
{
    if (sys) {
        switch (bytes) {
            case 1:
                return mem_readb_phys(addr);
            case 2:
                return mem_readw_phys(addr);
            default:
                return mem_readl_phys(addr);
        }
    }

    addr &= SAVAGE_VRAM_MASK;
    switch (bytes) {
        case 1:
            return dev->svga.vram[addr];
        case 2:
            return *(const uint16_t *) &dev->svga.vram[addr & ~1];
        default:
            return *(const uint32_t *) &dev->svga.vram[addr & ~3];
    }
}

/* Mip levels follow each other, each taking whole 8x8 blocks (32 bytes for
   S3TC); levels of 4x4 and smaller share the last block. */
static void
savage_3d_mip_setup(savage_t *dev)
{
    static const int blk[8] = { 32, 64, 64, 256, 128, 128, 128, 64 };
    uint32_t         descr  = dev->regs3d[R3D_TEX_DESCR];
    int              wlog   = descr & 0x0f;
    int              hlog   = (descr >> 8) & 0x0f;
    int              w      = 1 << wlog;
    int              h      = 1 << hlog;
    uint32_t         off    = 0;

    dev->mip_levels = MIN(MAX(wlog, hlog) + 1, 12);
    for (int i = 0; i < dev->mip_levels; i++) {
        dev->mip_off[i] = off;
        if ((w > 4) || (h > 4))
            off += ((w + 7) >> 3) * ((h + 7) >> 3) * blk[(descr >> 16) & 7];
        w = MAX(1, w >> 1);
        h = MAX(1, h >> 1);
    }
}

/* Offset of texel (u, v) in a w x h level. A level filling whole tiles is
   tiled like any surface; a smaller one is a row of blocks as wide as the
   level; 4x4 and smaller levels sit in one block (2x2 in its right column,
   1x1 below that). */
static uint32_t
savage_tex_off(int bytespp, int w, int h, int u, int v)
{
    int m = MAX(w, h);

    if (m <= 4) {
        if (m == 2)
            u += 4;
        else if (m == 1) {
            u += 4;
            v += 4;
        }
        return savage_block_off(bytespp, u, v);
    }

    if ((w >= savage_tile_w(bytespp)) && (h >= savage_tile_h(bytespp)))
        return savage_tiled_addr(0, w / savage_tile_w(bytespp), u * bytespp, v, bytespp);

    return (((v >> 3) * ((w + 7) >> 3) + (u >> 3)) * (bytespp << 6)) + savage_block_off(bytespp, u, v);
}

/* Offset of the 8-byte DXT1 block holding texel (u, v) in a w x h S3TC level:
   8x8 groups of four blocks ((0,0), (0,1), (1,0), (1,1)), in 64x64 tiles when
   the level fills them, else in rows as wide as the level. */
static uint32_t
savage_s3tc_off(int w, int h, int u, int v)
{
    int bx = u >> 2;
    int by = v >> 2;
    int gx = bx >> 1;
    int gy = by >> 1;
    int sub = ((bx & 1) << 4) + ((by & 1) << 3);

    if ((w == h) && (w <= 4))
        return (w == 2) ? 16 : ((w == 1) ? 24 : 0);

    if ((w >= 64) && (h >= 64))
        return (((gy >> 3) * (w >> 6) + (gx >> 3)) << 11) + ((((gy & 7) << 3) + (gx & 7)) << 5) + sub;

    return ((gy * MAX(1, w >> 3) + gx) << 5) + sub;
}

/* DXT1: two 565 colours, then 2-bit indices row by row; with colour 0 <=
   colour 1 index 3 is transparent black. */
static uint32_t
savage_dxt1_texel(uint32_t colors, uint32_t indices, int u, int v)
{
    uint32_t c0 = colors & 0xffff;
    uint32_t c1 = colors >> 16;
    uint32_t a  = savage_565_to_argb(c0);
    uint32_t b  = savage_565_to_argb(c1);
    int      i  = (indices >> ((((v & 3) << 2) + (u & 3)) << 1)) & 3;
    uint32_t out = 0xff000000;

    switch (i) {
        case 0:
            return a;
        case 1:
            return b;
        default:
            if ((c0 <= c1) && (i == 3))
                return 0;
            for (int sh = 0; sh < 24; sh += 8) {
                uint32_t ca = (a >> sh) & 0xff;
                uint32_t cb = (b >> sh) & 0xff;
                uint32_t c;

                if (c0 > c1)
                    c = (i == 2) ? ((2 * ca + cb) / 3) : ((ca + 2 * cb) / 3);
                else
                    c = (ca + cb) / 2;
                out |= c << sh;
            }
            return out;
    }
}

/* A texel of mip level 'level' of the texture in TexDescr/TexAddr as
   ARGB8888; *raw gets the 16-bit colour the transparency compare looks at.
   TexAddr bit 0 puts the texture in system memory (bit 1: through AGP); a
   palette address past video memory is a bus address too. */
static uint32_t
savage_3d_texel(savage_t *dev, int level, int u, int v, uint32_t *raw)
{
    uint32_t descr  = dev->regs3d[R3D_TEX_DESCR];
    int      w      = MAX(1, (1 << (descr & 0x0f)) >> level);
    int      h      = MAX(1, (1 << ((descr >> 8) & 0x0f)) >> level);
    uint32_t taddr  = dev->regs3d[R3D_TEX_ADDR];
    int      sys    = taddr & 1;
    uint32_t base   = (taddr & ~7) + dev->mip_off[level];
    uint32_t pal    = dev->regs3d[R3D_TEX_PAL_ADDR] & ~7;
    int      palsys = pal >= SAVAGE_VRAM_SIZE;
    uint32_t c;

    switch ((descr >> 16) & 7) {
        case 1: /* palettized, 565 palette */
        case 2: /* palettized, 1555 palette */
        case 7: /* palettized, 4444 palette */
            c    = savage_3d_read(dev, sys, base + savage_tex_off(1, w, h, u, v), 1);
            c    = savage_3d_read(dev, palsys, pal + (c << 1), 2);
            *raw = c;
            switch ((descr >> 16) & 7) {
                case 1:
                    return savage_565_to_argb(c);
                case 2:
                    return savage_1555_to_argb(c);
                default:
                    return savage_4444_to_argb(c);
            }

        case 3: /* ARGB8888 */
            c    = savage_3d_read(dev, sys, base + savage_tex_off(4, w, h, u, v), 4);
            *raw = ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f);
            return c;

        case 4: /* ARGB1555 */
        case 5: /* ARGB4444 */
        case 6: /* RGB565 */
            c    = savage_3d_read(dev, sys, base + savage_tex_off(2, w, h, u, v), 2);
            *raw = c;
            if (((descr >> 16) & 7) == 4)
                return savage_1555_to_argb(c);
            if (((descr >> 16) & 7) == 5)
                return savage_4444_to_argb(c);
            return savage_565_to_argb(c);

        default: /* S3TC (DXT1) */
            {
                uint32_t off = base + savage_s3tc_off(w, h, u, v);

                c    = savage_dxt1_texel(savage_3d_read(dev, sys, off, 4), savage_3d_read(dev, sys, off + 4, 4), u, v);
                *raw = ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f);
                return c;
            }
    }
}

static inline uint32_t
savage_mul8(uint32_t a, uint32_t b)
{
    return (a * b + 127) / 255;
}

/* Texture addressing (TexCtrl 15:14): 0 wrap, 1 clamp, 2 mirror. */
static inline int
savage_3d_wrap(int t, int size, int mode)
{
    switch (mode) {
        case 1:
            return (t < 0) ? 0 : ((t >= size) ? (size - 1) : t);
        case 2:
            t &= (size << 1) - 1;
            return (t >= size) ? ((size << 1) - 1 - t) : t;
        default:
            return t & (size - 1);
    }
}

/* Sample one mip level, point or bilinear. */
static uint32_t
savage_3d_sample_level(savage_t *dev, int level, float u, float v, int bilinear, uint32_t *raw, uint32_t *near_a)
{
    uint32_t tex   = dev->regs3d[R3D_TEX_CTRL];
    uint32_t descr = dev->regs3d[R3D_TEX_DESCR];
    int      tw    = MAX(1, (1 << (descr & 0x0f)) >> level);
    int      th    = MAX(1, (1 << ((descr >> 8) & 0x0f)) >> level);
    int      mode  = (tex >> 14) & 3;
    float    fu    = u * (float) tw;
    float    fv    = v * (float) th;
    uint32_t t[4], dummy, out;
    int      u0, v0;
    uint32_t a, b;

    out     = savage_3d_texel(dev, level, savage_3d_wrap((int) floorf(fu), tw, mode), savage_3d_wrap((int) floorf(fv), th, mode), raw);
    *near_a = out >> 24;
    if (!bilinear)
        return out;

    out = 0;
    fu -= 0.5f;
    fv -= 0.5f;
    u0 = (int) floorf(fu);
    v0 = (int) floorf(fv);
    a  = (uint32_t) ((fu - (float) u0) * 256.0f);
    b  = (uint32_t) ((fv - (float) v0) * 256.0f);

    t[0] = savage_3d_texel(dev, level, savage_3d_wrap(u0, tw, mode), savage_3d_wrap(v0, th, mode), &dummy);
    t[1] = savage_3d_texel(dev, level, savage_3d_wrap(u0 + 1, tw, mode), savage_3d_wrap(v0, th, mode), &dummy);
    t[2] = savage_3d_texel(dev, level, savage_3d_wrap(u0, tw, mode), savage_3d_wrap(v0 + 1, th, mode), &dummy);
    t[3] = savage_3d_texel(dev, level, savage_3d_wrap(u0 + 1, tw, mode), savage_3d_wrap(v0 + 1, th, mode), &dummy);

    for (int sh = 0; sh < 32; sh += 8) {
        uint32_t c0 = (t[0] >> sh) & 0xff, c1 = (t[1] >> sh) & 0xff;
        uint32_t c2 = (t[2] >> sh) & 0xff, c3 = (t[3] >> sh) & 0xff;
        uint32_t top = c0 * (256 - a) + c1 * a;
        uint32_t bot = c2 * (256 - a) + c3 * a;

        out |= (((top * (256 - b) + bot * b) >> 16) & 0xff) << sh;
    }

    return out;
}

/* Sample the texture at (u, v) with level of detail lod (log2 of texels per
   pixel). TexCtrl: filter 1:0 (0 point, 1 bilinear, 3 trilinear), bit 2
   mipmapping off, LOD bias 11:3 in 1/16 (bit 8 of it: negative). *raw is the
   nearest texel's colour for the colour-key compare, *near_a its alpha. */
static uint32_t
savage_3d_sample(savage_t *dev, float u, float v, float lod, uint32_t *raw, uint32_t *near_a)
{
    uint32_t tex    = dev->regs3d[R3D_TEX_CTRL];
    int      filter = tex & 3;
    uint32_t dbias  = (tex >> 3) & 0x1ff;
    int      level;
    float    frac;
    uint32_t t0, t1, dummy, out = 0;

    if ((tex & 4) || (dev->mip_levels <= 1))
        return savage_3d_sample_level(dev, 0, u, v, filter != 0, raw, near_a);

    lod += (float) ((dbias & 0x100) ? ((int) (dbias & 0xff) - 256) : (int) (dbias & 0xff)) / 16.0f;
    if (lod <= 0.0f)
        return savage_3d_sample_level(dev, 0, u, v, filter != 0, raw, near_a);
    if (lod >= (float) (dev->mip_levels - 1))
        return savage_3d_sample_level(dev, dev->mip_levels - 1, u, v, filter != 0, raw, near_a);

    if (filter != 3)
        return savage_3d_sample_level(dev, (int) (lod + 0.5f), u, v, filter != 0, raw, near_a);

    /* Trilinear: blend the two nearest levels. */
    level = (int) lod;
    frac  = lod - (float) level;
    t0    = savage_3d_sample_level(dev, level, u, v, 1, raw, near_a);
    t1    = savage_3d_sample_level(dev, level + 1, u, v, 1, &dummy, &dummy);
    for (int sh = 0; sh < 32; sh += 8) {
        float c = (float) ((t0 >> sh) & 0xff) * (1.0f - frac) + (float) ((t1 >> sh) & 0xff) * frac;

        out |= ((uint32_t) (c + 0.5f) & 0xff) << sh;
    }

    return out;
}

/* Fog (FogCtrl: colour 23:0, table shift 26:24, enable 28, vertex fog 29):
   the factor (255 = no fog) is the specular alpha for vertex fog, else the
   64-entry table at registers 20h-2Fh indexed by Z. */
static void
savage_3d_fog(savage_t *dev, float z, const float *spc, uint32_t *r, uint32_t *g, uint32_t *b)
{
    uint32_t fog = dev->regs3d[0x30];
    uint32_t f;

    if (!(fog & 0x10000000))
        return;

    if (fog & 0x20000000)
        f = (uint32_t) spc[0];
    else {
        float start = 1.0f - 1.0f / (float) (1 << ((fog >> 24) & 7));
        float pos   = (z - start) / (1.0f - start) * 64.0f;
        int   idx   = (pos < 0.0f) ? 0 : ((pos > 63.0f) ? 63 : (int) pos);

        f = (dev->regs3d[0x20 + (idx >> 2)] >> ((idx & 3) << 3)) & 0xff;
    }

    *r = (*r * f + ((fog >> 16) & 0xff) * (255 - f) + 127) / 255;
    *g = (*g * f + ((fog >> 8) & 0xff) * (255 - f) + 127) / 255;
    *b = (*b * f + (fog & 0xff) * (255 - f) + 127) / 255;
}

/* Blend factor for one channel: D3DBLEND - 1, with the source's 2/3 meaning
   DESTCOLOR/INVDESTCOLOR and the destination's SRCCOLOR/INVSRCCOLOR. */
static inline uint32_t
savage_blend_factor(int mode, uint32_t sc, uint32_t dc, uint32_t sa, uint32_t da, int is_src)
{
    switch (mode) {
        case 0:
            return 0;
        case 1:
            return 255;
        case 2:
            return is_src ? dc : sc;
        case 3:
            return 255 - (is_src ? dc : sc);
        case 4:
            return sa;
        case 5:
            return 255 - sa;
        case 6:
            return da;
        default:
            return 255 - da;
    }
}

static int
savage_3d_compare(int func, uint32_t a, uint32_t b)
{
    switch (func) {
        case 0:
            return 0;
        case 1:
            return a < b;
        case 2:
            return a == b;
        case 3:
            return a <= b;
        case 4:
            return a > b;
        case 5:
            return a != b;
        case 6:
            return a >= b;
        default:
            return 1;
    }
}

/* Shade, test, blend and write one pixel. */
static void
savage_3d_pixel(savage_t *dev, int x, int y, float z, float u, float v, float lod, const float *cd, const float *spc)
{
    uint32_t draw  = dev->regs3d[R3D_DRAW_CTRL];
    uint32_t tex   = dev->regs3d[R3D_TEX_CTRL];
    uint32_t zctrl = dev->regs3d[R3D_ZBUF_CTRL];
    uint32_t dst   = dev->regs3d[R3D_DST_CTRL];
    uint32_t dbase = ((dst >> 8) & 0x1fff) << 11;
    int      dtile = dst & 0x7f;
    int      d32   = ((dst >> 29) & 3) != 0;
    uint32_t daddr;
    uint32_t sr, sg, sb, sa;
    uint32_t dr, dg, db, da;
    uint32_t zaddr = 0, zval = 0;
    int      z32   = 0;
    int      zbuf  = !!(zctrl & 0x20);

    if (!dtile)
        dtile = 1;

    /* Diffuse colour (flat or Gouraud). */
    sa = (uint32_t) cd[0];
    sr = (uint32_t) cd[1];
    sg = (uint32_t) cd[2];
    sb = (uint32_t) cd[3];

    if (tex & 0x10000) {
        uint32_t raw, near_a;
        uint32_t t, ta, tr, tg, tb;

        t = savage_3d_sample(dev, u, v, lod, &raw, &near_a);

        /* Texture transparency (TexCtrl bit 20): texels with alpha 0, or with
           the colour key when the compare (bit 18) is on, are not drawn. */
        if (tex & 0x100000) {
            if ((tex & 0x40000) ? (raw == (dev->regs3d[R3D_TEX_XPR_CLR] & 0xffff)) : (near_a == 0))
                return;
        }

        ta = t >> 24;
        tr = (t >> 16) & 0xff;
        tg = (t >> 8) & 0xff;
        tb = t & 0xff;

        switch ((draw >> 26) & 7) {
            case 1: /* modulate: the texture's alpha */
                sr = savage_mul8(tr, sr);
                sg = savage_mul8(tg, sg);
                sb = savage_mul8(tb, sb);
                sa = ta;
                break;
            case 3: /* modulate alpha */
            case 5: /* modulate mask */
                sr = savage_mul8(tr, sr);
                sg = savage_mul8(tg, sg);
                sb = savage_mul8(tb, sb);
                sa = savage_mul8(ta, sa);
                break;
            case 2: /* decal alpha */
                sr = (tr * ta + sr * (255 - ta) + 127) / 255;
                sg = (tg * ta + sg * (255 - ta) + 127) / 255;
                sb = (tb * ta + sb * (255 - ta) + 127) / 255;
                break;
            case 7: /* add */
                sr = MIN(tr + sr, 255);
                sg = MIN(tg + sg, 255);
                sb = MIN(tb + sb, 255);
                break;
            default: /* decal, decal mask, copy */
                sr = tr;
                sg = tg;
                sb = tb;
                sa = ta;
                break;
        }
    }

    if (draw & 0x40) { /* specular */
        sr = MIN(sr + (uint32_t) spc[1], 255);
        sg = MIN(sg + (uint32_t) spc[2], 255);
        sb = MIN(sb + (uint32_t) spc[3], 255);
    }

    savage_3d_fog(dev, z, spc, &sr, &sg, &sb);

    /* Alpha test. */
    if ((draw & 0x20000) && !savage_3d_compare((draw >> 14) & 7, sa, (draw >> 18) & 0xff))
        return;

    /* Z test. ZBufOffset bit 31 selects 32-bit Z: GX3 "1.8.15" floats, the
       top 24 bits of an IEEE single (the driver clears to 007F7FFFh). */
    if (zbuf) {
        uint32_t zoff   = dev->regs3d[R3D_ZBUF_OFFSET];
        int      ztiles = (zoff >> 25) & 0x3f;
        float    zc     = (z < 0.0f) ? 0.0f : ((z > 1.0f) ? 1.0f : z);
        uint32_t zcur;

        z32 = !!(zoff & 0x80000000);
        if (z32) {
            uint32_t bits;

            memcpy(&bits, &zc, 4);
            zval  = bits >> 8;
            zaddr = savage_tiled_addr((zoff & 0x1fff) << 11, ztiles ? ztiles : 1, x << 2, y, 4) & SAVAGE_VRAM_MASK & ~3;
            zcur  = *(uint32_t *) &dev->svga.vram[zaddr] & 0xffffff;
        } else {
            zval  = (uint32_t) (zc * 65535.0f);
            zaddr = savage_tiled_addr((zoff & 0x1fff) << 11, ztiles ? ztiles : 1, x << 1, y, 2) & SAVAGE_VRAM_MASK & ~1;
            zcur  = *(uint16_t *) &dev->svga.vram[zaddr];
        }
        if (!savage_3d_compare(zctrl & 7, zval, zcur))
            return;
    }

    if (zctrl & 0x08) { /* draw buffer update */
        int src_mode = (draw >> 10) & 7;
        int dst_mode = (draw >> 7) & 7;

        if (d32)
            daddr = savage_tiled_addr(dbase, dtile, x << 2, y, 4) & SAVAGE_VRAM_MASK & ~3;
        else
            daddr = savage_tiled_addr(dbase, dtile, x << 1, y, 2) & SAVAGE_VRAM_MASK & ~1;

        /* Anything but "source x 1 (or x its own opaque alpha) + destination x 0" reads the destination. */
        if ((dst_mode != 0) || !((src_mode == 1) || ((src_mode == 4) && (sa == 255)))) {
            uint32_t d = d32 ? *(uint32_t *) &dev->svga.vram[daddr] : savage_565_to_argb(*(uint16_t *) &dev->svga.vram[daddr]);
            uint32_t fsr, fsg, fsb, fsa, fdr, fdg, fdb, fda;

            da  = d32 ? (d >> 24) : 255;
            dr  = (d >> 16) & 0xff;
            dg  = (d >> 8) & 0xff;
            db  = d & 0xff;
            fsr = savage_blend_factor(src_mode, sr, dr, sa, da, 1);
            fsg = savage_blend_factor(src_mode, sg, dg, sa, da, 1);
            fsb = savage_blend_factor(src_mode, sb, db, sa, da, 1);
            fsa = savage_blend_factor(src_mode, sa, da, sa, da, 1);
            fdr = savage_blend_factor(dst_mode, sr, dr, sa, da, 0);
            fdg = savage_blend_factor(dst_mode, sg, dg, sa, da, 0);
            fdb = savage_blend_factor(dst_mode, sb, db, sa, da, 0);
            fda = savage_blend_factor(dst_mode, sa, da, sa, da, 0);
            sr  = MIN(savage_mul8(sr, fsr) + savage_mul8(dr, fdr), 255);
            sg  = MIN(savage_mul8(sg, fsg) + savage_mul8(dg, fdg), 255);
            sb  = MIN(savage_mul8(sb, fsb) + savage_mul8(db, fdb), 255);
            sa  = MIN(savage_mul8(sa, fsa) + savage_mul8(da, fda), 255);
        }

        if (d32)
            *(uint32_t *) &dev->svga.vram[daddr] = (sa << 24) | (sr << 16) | (sg << 8) | sb;
        else
            *(uint16_t *) &dev->svga.vram[daddr] = ((sr >> 3) << 11) | ((sg >> 2) << 5) | (sb >> 3);
        dev->svga.changedvram[daddr >> 12] = changeframecount;
    }

    if (zbuf && (zctrl & 0x10)) {
        if (z32)
            *(uint32_t *) &dev->svga.vram[zaddr] = zval;
        else
            *(uint16_t *) &dev->svga.vram[zaddr] = zval;
        dev->svga.changedvram[zaddr >> 12]   = changeframecount;
    }
}

static void
savage_3d_unpack_color(uint32_t c, float *out)
{
    out[0] = (float) (c >> 24);
    out[1] = (float) ((c >> 16) & 0xff);
    out[2] = (float) ((c >> 8) & 0xff);
    out[3] = (float) (c & 0xff);
}

/* Edge function, positive inside a triangle wound clockwise on the screen. */
static inline double
savage_edge(double ax, double ay, double bx, double by, double px, double py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Top-left fill rule: pixels on a top or left edge belong to the triangle. */
static inline int
savage_edge_top_left(double ax, double ay, double bx, double by)
{
    return ((ay == by) && (bx > ax)) || (by < ay);
}

/* Perspective-correct U and V at screen barycentrics l0..l2. */
static void
savage_3d_uv_at(savage_t *dev, int i0, int i1, int i2, const float *w, double l0, double l1, double l2, float *u, float *v)
{
    double b0 = l0 * w[0], b1 = l1 * w[1], b2 = l2 * w[2];
    double q  = b0 + b1 + b2;

    if (q == 0.0)
        q = 1.0;
    *u = (float) ((b0 * dev->vtx[i0].u + b1 * dev->vtx[i1].u + b2 * dev->vtx[i2].u) / q);
    *v = (float) ((b0 * dev->vtx[i0].v + b1 * dev->vtx[i1].v + b2 * dev->vtx[i2].v) / q);
}

static void
savage_3d_triangle(savage_t *dev, int i0, int i1, int i2)
{
    uint32_t draw  = dev->regs3d[R3D_DRAW_CTRL];
    uint32_t sc_tl = dev->regs3d[R3D_SCISSOR_TL];
    uint32_t sc_br = dev->regs3d[R3D_SCISSOR_BR];
    int      persp = !(draw & 0x80000000);
    double   x0 = dev->vtx[i0].x, y0 = dev->vtx[i0].y;
    double   x1 = dev->vtx[i1].x, y1 = dev->vtx[i1].y;
    double   x2 = dev->vtx[i2].x, y2 = dev->vtx[i2].y;
    double   area = savage_edge(x0, y0, x1, y1, x2, y2);
    int      cull = (draw >> 2) & 3; /* D3DCULL: 1 none, 2 clockwise, 3 counter-clockwise */
    int      minx, miny, maxx, maxy;
    int      sx0, sy0, sx1, sy1;
    int      tl0, tl1, tl2;
    float    cd[3][4], spc[3][4];
    float    w[3];
    uint32_t tex    = dev->regs3d[R3D_TEX_CTRL];
    int      do_lod = (tex & 0x10000) && !(tex & 4);
    float    tw = 1.0f, th = 1.0f;
    double   dx0, dx1, dx2, dy0, dy1, dy2;

    if (area == 0.0)
        return;
    if (((cull == 2) && (area > 0.0)) || ((cull == 3) && (area < 0.0)))
        return;

    /* Work with a clockwise triangle. */
    if (area < 0.0) {
        int t = i1;

        i1   = i2;
        i2   = t;
        x1   = dev->vtx[i1].x;
        y1   = dev->vtx[i1].y;
        x2   = dev->vtx[i2].x;
        y2   = dev->vtx[i2].y;
        area = -area;
    }

    for (int i = 0; i < 3; i++) {
        int n = (i == 0) ? i0 : ((i == 1) ? i1 : i2);

        /* Flat shading takes the first vertex's colours. */
        savage_3d_unpack_color(dev->vtx[(draw & 0x20) ? i0 : n].diff, cd[i]);
        savage_3d_unpack_color(dev->vtx[(draw & 0x20) ? i0 : n].spec, spc[i]);
        w[i] = (persp && (dev->vtx[n].w > 0.0f)) ? dev->vtx[n].w : 1.0f;
    }

    if (tex & 0x10000) {
        savage_3d_mip_setup(dev);
        tw = (float) (1 << (dev->regs3d[R3D_TEX_DESCR] & 0x0f));
        th = (float) (1 << ((dev->regs3d[R3D_TEX_DESCR] >> 8) & 0x0f));
    }

    /* Edge function steps one pixel right and one down. */
    dx0 = -(y2 - y1);
    dx1 = -(y0 - y2);
    dx2 = -(y1 - y0);
    dy0 = x2 - x1;
    dy1 = x0 - x2;
    dy2 = x1 - x0;

    sx0 = sc_tl & 0x7ff;
    sy0 = (sc_tl >> 16) & 0xfff;
    sx1 = sc_br & 0x7ff;
    sy1 = (sc_br >> 16) & 0xfff;

    minx = MAX((int) floor(MIN(x0, MIN(x1, x2))), sx0);
    maxx = MIN((int) ceil(MAX(x0, MAX(x1, x2))), sx1);
    miny = MAX((int) floor(MIN(y0, MIN(y1, y2))), sy0);
    maxy = MIN((int) ceil(MAX(y0, MAX(y1, y2))), sy1);

    tl0 = savage_edge_top_left(x1, y1, x2, y2);
    tl1 = savage_edge_top_left(x2, y2, x0, y0);
    tl2 = savage_edge_top_left(x0, y0, x1, y1);

    for (int y = miny; y <= maxy; y++) {
        double py = y + 0.5;

        for (int x = minx; x <= maxx; x++) {
            double px = x + 0.5;
            double e0 = savage_edge(x1, y1, x2, y2, px, py);
            double e1 = savage_edge(x2, y2, x0, y0, px, py);
            double e2 = savage_edge(x0, y0, x1, y1, px, py);
            float  b0, b1, b2, q, u, v, z, lod = 0.0f;
            float  c[4], s[4];

            if ((e0 < 0.0) || (e1 < 0.0) || (e2 < 0.0))
                continue;
            if (((e0 == 0.0) && !tl0) || ((e1 == 0.0) && !tl1) || ((e2 == 0.0) && !tl2))
                continue;

            /* Barycentric weights, perspective-corrected by the vertices' 1/W. */
            b0 = (float) (e0 / area) * w[0];
            b1 = (float) (e1 / area) * w[1];
            b2 = (float) (e2 / area) * w[2];
            q  = b0 + b1 + b2;
            b0 /= q;
            b1 /= q;
            b2 /= q;

            u = b0 * dev->vtx[i0].u + b1 * dev->vtx[i1].u + b2 * dev->vtx[i2].u;
            v = b0 * dev->vtx[i0].v + b1 * dev->vtx[i1].v + b2 * dev->vtx[i2].v;

            /* Level of detail: log2 of the texels a pixel step covers. */
            if (do_lod) {
                float dudx, dvdx, dudy, dvdy, rho;

                savage_3d_uv_at(dev, i0, i1, i2, w, (e0 + dx0) / area, (e1 + dx1) / area, (e2 + dx2) / area, &dudx, &dvdx);
                savage_3d_uv_at(dev, i0, i1, i2, w, (e0 + dy0) / area, (e1 + dy1) / area, (e2 + dy2) / area, &dudy, &dvdy);
                dudx = (dudx - u) * tw;
                dvdx = (dvdx - v) * th;
                dudy = (dudy - u) * tw;
                dvdy = (dvdy - v) * th;
                rho  = MAX(dudx * dudx + dvdx * dvdx, dudy * dudy + dvdy * dvdy);
                lod  = (rho > 0.0f) ? (0.5f * log2f(rho)) : 0.0f;
            }
            z = (float) ((e0 * dev->vtx[i0].z + e1 * dev->vtx[i1].z + e2 * dev->vtx[i2].z) / area);
            for (int k = 0; k < 4; k++) {
                c[k] = b0 * cd[0][k] + b1 * cd[1][k] + b2 * cd[2][k];
                s[k] = b0 * spc[0][k] + b1 * spc[1][k] + b2 * spc[2][k];
            }

            savage_3d_pixel(dev, x, y, z, u, v, lod, c, s);
        }
    }
}

/* A complete vertex: assemble triangles by the primitive type. */
static void
savage_3d_vertex(savage_t *dev, const uint32_t *p, uint32_t cmd)
{
    int      type = (dev->bci.prim_cmd >> 25) & 3; /* 0 list, 1 strip, 2 fan, 3 quad list */
    int      n    = 0;
    int      slot;

    if (type == 3)
        slot = dev->nvtx & 3;
    else if (dev->nvtx < 3)
        slot = dev->nvtx;
    else
        slot = 2;

    /* Unpack X, Y, then the components the command does not switch off. */
    {
        float           x = savage_float(p[n++]);
        float           y = savage_float(p[n++]);
        float           z = (cmd & 0x01) ? 0.0f : savage_float(p[n++]);
        float           w = (cmd & 0x02) ? 1.0f : savage_float(p[n++]);
        uint32_t        d = (cmd & 0x04) ? 0xffffffff : p[n++];
        uint32_t        s = (cmd & 0x08) ? 0x00000000 : p[n++];
        float           u = (cmd & 0x10) ? 0.0f : savage_float(p[n++]);
        float           v = (cmd & 0x20) ? 0.0f : savage_float(p[n++]);

        if ((dev->nvtx >= 3) && (type != 3)) {
            /* Strip: drop the oldest; fan: keep the centre. */
            if (type == 2)
                dev->vtx[1] = dev->vtx[2];
            else {
                dev->vtx[0] = dev->vtx[1];
                dev->vtx[1] = dev->vtx[2];
            }
        }

        dev->vtx[slot].x  = x;
        dev->vtx[slot].y  = y;
        dev->vtx[slot].z  = z;
        dev->vtx[slot].w  = w;
        dev->vtx[slot].diff = d;
        dev->vtx[slot].spec = s;
        dev->vtx[slot].u  = u;
        dev->vtx[slot].v  = v;
    }

    /* Quad list (the Win9x OpenGL ICD's quads): 0-1-2, then 0-2-3. */
    if (type == 3) {
        if (++dev->nvtx == 3) {
            savage_3d_triangle(dev, 0, 1, 2);
            dev->prim_count++;
        } else if (dev->nvtx >= 4) {
            savage_3d_triangle(dev, 0, 2, 3);
            dev->prim_count++;
            dev->nvtx = 0;
        }
        return;
    }

    if (dev->nvtx < 3) {
        dev->nvtx++;
        dev->strip_odd = 0;
    }
    if (dev->nvtx < 3)
        return;

    /* A strip's odd triangles start at their newest vertex (3-2-1, 5-4-3...),
       which is where flat shading takes the colour from: the Win9x OpenGL
       ICD sends a quad as a 4-vertex strip with colours in vertices 0 and 3. */
    if ((type == 1) && dev->strip_odd)
        savage_3d_triangle(dev, 2, 1, 0);
    else
        savage_3d_triangle(dev, 0, 1, 2);
    dev->strip_odd ^= 1;
    dev->prim_count++;

    if (type == 0)
        dev->nvtx = 0;
}

static void
savage_3d_vertex_dword(savage_t *dev, uint32_t val)
{
    dev->bci.prim_buf[dev->bci.prim_dword++] = val;
    if (dev->bci.prim_dword < dev->bci.prim_vert_dwords)
        return;

    dev->bci.prim_dword = 0;
    savage_3d_vertex(dev, dev->bci.prim_buf, dev->bci.prim_cmd);

    if (--dev->bci.skip_count <= 0)
        dev->bci.state = BCI_IDLE;
}

/* DrawPrimitive: vertex count 23:16, continue 24, type 26:25; bits 5:0 switch
   off Z, W, diffuse, specular, U and V. Vertices are D3DTLVERTEX-ordered. */
static void
savage_3d_start(savage_t *dev, uint32_t cmd)
{
    int count = (cmd >> 16) & 0xff;

    dev->bci.prim_cmd         = cmd;
    dev->bci.prim_vert_dwords = 8 - __builtin_popcount(cmd & 0x3f);
    dev->bci.prim_dword       = 0;
    dev->bci.skip_count       = count;

    /* Without the continue bit a list, strip or fan starts afresh. */
    if (!(cmd & 0x01000000))
        dev->nvtx = 0;

    if (count)
        dev->bci.state = BCI_3DVERTS;
}

/* An indexed vertex: a whole D3DTLVERTEX (32 bytes) from the vertex buffer
   at VertBufAddr (register 3Eh; bit 0 = system memory, bit 1 = AGP). */
static void
savage_3d_index(savage_t *dev, uint32_t index)
{
    uint32_t vb   = dev->regs3d[0x3e];
    uint32_t addr = (vb & ~3) + (index << 5);
    uint32_t v[8];

    for (int i = 0; i < 8; i++)
        v[i] = savage_3d_read(dev, vb & 1, addr + (i << 2), 4);

    savage_3d_vertex(dev, v, 0);
}

static void
savage_3d_index_dword(savage_t *dev, uint32_t val)
{
    savage_3d_index(dev, val & 0xffff);
    if (--dev->bci.skip_count > 0) {
        savage_3d_index(dev, val >> 16);
        dev->bci.skip_count--;
    }
    if (dev->bci.skip_count <= 0)
        dev->bci.state = BCI_IDLE;
}

/* DrawIndexedPrimitive: the first index in bits 15:0, count 23:16, continue
   24, type 26:25; the other indices follow two to a dword. */
static void
savage_3d_start_indexed(savage_t *dev, uint32_t cmd)
{
    int count = (cmd >> 16) & 0xff;

    dev->bci.prim_cmd   = cmd;
    dev->bci.skip_count = count;

    if (!(cmd & 0x01000000))
        dev->nvtx = 0;

    if (!count)
        return;

    savage_3d_index(dev, cmd & 0xffff);
    if (--dev->bci.skip_count > 0)
        dev->bci.state = BCI_3DIDX;
}

static void
s3_savage_bci_write(savage_t *dev, uint32_t val)
{
    dev->bci_hist[dev->bci_hist_pos & 255] = val;
    dev->bci_hist_state[dev->bci_hist_pos & 255] = dev->bci.state;
    dev->bci_hist_pos++;
    if (savage_bci_log)
        pclog("BCI %08x %d\n", val, dev->bci.state);

    switch (dev->bci.state) {
        case BCI_PARAMS:
            /* Coordinates never have bits 31:30 set: such a dword where one is
               expected is the next command. The Win9x driver sends a fill's
               command and colour, then a position and size for each clip
               rectangle, so a fill clipped away entirely has none. */
            if (val & 0xc0000000) {
                int slot = dev->bci.slots[dev->bci.slot];

                if ((slot == P_SRC_XY) || (slot == P_DST_XY) || (slot == P_WH))
                    break;
            }
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

        case BCI_3DIDX:
            savage_3d_index_dword(dev, val);
            return;

        case BCI_3DVERTS:
            savage_3d_vertex_dword(dev, val);
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
            savage_3d_start(dev, val);
            break;

        case 0x11: /* 3D draw indexed primitive */
            savage_3d_start_indexed(dev, val);
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
    memset(dev->regs3d, 0, sizeof(dev->regs3d));
    dev->nvtx        = 0;
    dev->write_mask  = 0xffffffff;
    dev->read_mask   = 0xffffffff;
    dev->bci_ctrl    = 0;
    dev->shadow_ctrl = 0;
    dev->event_tag   = 0;
    dev->bank        = 0;
    dev->ma_ext      = 0;
    dev->subsys_stat = 0;
    dev->subsys_cntl = 0;

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
s3_savage_debug_hook(const char *args)
{
    savage_t *dev = savage_debug_dev;

    if (!dev)
        return;

    /* "dev texdump <TexAddr> <TexDescr> <file.bmp>": decode a texture as the
       3D engine reads it (the palette from the current TexPalAddr). */
    if (!strncmp(args, "texdump", 7)) {
        uint32_t taddr = 0, descr = 0;
        char     path[512] = "";

        if (sscanf(args + 7, "%x %x %511s", &taddr, &descr, path) == 3) {
            uint32_t save_addr  = dev->regs3d[R3D_TEX_ADDR];
            uint32_t save_descr = dev->regs3d[R3D_TEX_DESCR];
            int      w          = 1 << (descr & 0x0f);
            int      h          = 1 << ((descr >> 8) & 0x0f);
            FILE    *f          = plat_fopen(path, "wb");

            dev->regs3d[R3D_TEX_ADDR]  = taddr;
            dev->regs3d[R3D_TEX_DESCR] = descr;
            savage_3d_mip_setup(dev);
            if (f) {
                uint8_t hdr[54] = { 'B', 'M' };
                uint32_t size   = 54 + w * h * 4;

                memcpy(&hdr[2], &size, 4);
                hdr[10] = 54;
                hdr[14] = 40;
                memcpy(&hdr[18], &w, 4);
                memcpy(&hdr[22], &h, 4);
                hdr[26] = 1;
                hdr[28] = 32;
                fwrite(hdr, 1, 54, f);
                for (int y = h - 1; y >= 0; y--)
                    for (int x = 0; x < w; x++) {
                        uint32_t raw, c = savage_3d_texel(dev, 0, x, y, &raw);

                        fwrite(&c, 1, 4, f);
                    }
                fclose(f);
            }
            dev->regs3d[R3D_TEX_ADDR]  = save_addr;
            dev->regs3d[R3D_TEX_DESCR] = save_descr;
            pclog("Savage: texture %08x/%08x (%dx%d) to %s\n", taddr, descr, w, h, path);
        }
        return;
    }

    /* "dev bcilog 1|0": log every BCI dword and MMIO register write. */
    if (!strncmp(args, "bcilog", 6)) {
        savage_bci_log = atoi(args + 6) != 0;
        pclog("Savage: BCI log %s\n", savage_bci_log ? "on" : "off");
        return;
    }

    pclog("Savage BCI: state %d cmd %08x slot %d/%d glyph %dx%d %u/%u host line %d/%d dword %d/%d\n",
          dev->bci.state, dev->bci.cmd, dev->bci.slot, dev->bci.nslots, dev->bci.glyph_w, dev->bci.glyph_h,
          dev->bci.glyph_pos, dev->bci.glyph_dwords, dev->bci.line, dev->bci.h, dev->bci.dword,
          dev->bci.dwords_per_line);
    pclog("Savage GBD %08x %08x PBD %08x %08x SBD %08x %08x\n", dev->gbd.offset, dev->gbd.hi,
          dev->pbd.offset, dev->pbd.hi, dev->sbd.offset, dev->sbd.hi);
    pclog("Savage 3D: %d triangles, TexAddr %08x TexDescr %08x TexCtrl %08x DrawCtrl %08x ZBufCtrl %08x ZBufOffset %08x DstCtrl %08x FogCtrl %08x\n",
          dev->prim_count, dev->regs3d[0x1a], dev->regs3d[0x1b], dev->regs3d[0x1c], dev->regs3d[0x31],
          dev->regs3d[0x32], dev->regs3d[0x33], dev->regs3d[0x34], dev->regs3d[0x30]);
    pclog("Savage overlay: %dx%d at %d,%d from %dx%d fmt %d base %06x stride %d hstep %05x vstep %05x filter %d/%d blend %08x key %08x-%08x adj %08x; IRQ stat %02x enable %02x CR32 %02x line %d\n",
          dev->ov.w, dev->ov.h, dev->ov.x, dev->ov.y, dev->ov.src_w, dev->ov.lines, dev->ov.fmt, dev->ov.base,
          dev->ov.stride, dev->ov.hstep, dev->ov.vstep, dev->ov.hfilter, dev->ov.vfilter, SREG(dev, SS_BLEND),
          SREG(dev, SS_CKEY_LOW), SREG(dev, SS_CKEY_HI), SREG(dev, SS_COLOR_ADJ), dev->subsys_stat, dev->subsys_cntl,
          dev->svga.crtc[0x32], dev->pci_regs[0x3c]);
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
    dev->svga.vblank_start       = s3_savage_vblank_start;

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
