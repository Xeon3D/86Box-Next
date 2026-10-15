/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          3Dlabs/TI Permedia 2 (TVP4020) emulation: the STB GLyder MAX-2+.
 *
 *          The chip has a VGA core of its own, with two extension registers
 *          (SR05 VGAControlReg, GR09 Mode640Reg), and a hi-res video timing
 *          generator (VTG) in its control register space that takes over the
 *          display once the VGA core is switched off. The RAMDAC and its
 *          clock synthesisers are TI TVP4020 style, built in: reached through
 *          control space 0x4000 or, from the VGA ports, through SR05's DAC
 *          address bits. A real mode BIOS reaches the rest of control space
 *          through PCI configuration registers F8h (address) and FCh (data).
 *
 *          Graphics processor (GLINT core): register file, input FIFO
 *          parsing (tag/data words, hold/increment/indexed), Sync, and the
 *          2D paths the display drivers use. The rasterizer's fragment rules
 *          follow the Permedia 2 model in the ES40 emulator
 *          (https://github.com/ES40-Emu/es40, src/graphics/Permedia2.cpp,
 *          Copyright (C) 2026 gdwnldsKSC, BSD-1-Clause).
 *
 * Authors: Marcos Alves, <xeon4d@gmail.com>
 *
 *          Copyright 2026 Marcos Alves.
 */
#include <math.h>
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

#define BIOS_STB_GLYDER_MAX2 "roms/video/3dlabs/permedia2/stbp2a.bin"

#define P2_CTRL_SIZE 0x20000 /* 64 KB of registers, then a byte-swapped mirror. */
#define P2_APERTURE_SIZE 0x800000

/* Control space. */
#define P2_RESET_STATUS   0x0000
#define P2_INT_ENABLE     0x0008
#define P2_INT_FLAGS      0x0010
#define P2_IN_FIFO_SPACE  0x0018
#define P2_OUT_FIFO_WORDS 0x0020
#define P2_DMA_ADDRESS    0x0028
#define P2_DMA_COUNT      0x0030
#define P2_ERROR_FLAGS    0x0038
#define P2_VCLK_CTL       0x0040
#define P2_TEST_REGISTER  0x0048
#define P2_APERTURE_ONE   0x0050
#define P2_APERTURE_TWO   0x0058
#define P2_DMA_CONTROL    0x0060
#define P2_FIFO_DISCON    0x0068
#define P2_CHIP_CONFIG    0x0070

#define P2_REBOOT         0x1000
#define P2_MEM_CONTROL    0x1040
#define P2_BOOT_ADDRESS   0x1080
#define P2_MEM_CONFIG     0x10c0
#define P2_BYPASS_MASK    0x1100
#define P2_FB_WRITE_MASK  0x1140
#define P2_MCLK_COUNT     0x1180

/* Video timing generator, 0x3000 + 8 * index. */
enum {
    VTG_SCREEN_BASE = 0,
    VTG_SCREEN_STRIDE,
    VTG_H_TOTAL,
    VTG_HG_END,
    VTG_HB_END,
    VTG_HS_START,
    VTG_HS_END,
    VTG_V_TOTAL,
    VTG_VB_END,
    VTG_VS_START,
    VTG_VS_END,
    VTG_VIDEO_CONTROL,
    VTG_INTERRUPT_LINE,
    VTG_DISPLAY_DATA,
    VTG_LINE_COUNT,
    VTG_FIFO_CONTROL,
    VTG_SCREEN_BASE_RIGHT,
    VTG_NUM
};

#define VC_ENABLE      0x00000001
#define VC_LINE_DOUBLE 0x00000004
#define VC_DATA64      0x00010000

#define INT_DMA      0x01
#define INT_SYNC     0x02
#define INT_ERROR    0x08
#define INT_VRETRACE 0x10
#define INT_SCANLINE 0x20

/* TVP4020 indexed registers. */
#define TVP_CURSOR_CONTROL 0x06
#define TVP_COLOR_MODE     0x18
#define TVP_MODE_CONTROL   0x19
#define TVP_PALETTE_PAGE   0x1c
#define TVP_MISC_CONTROL   0x1e
#define TVP_PIXCLK_A1      0x20
#define TVP_PIXCLK_STATUS  0x29
#define TVP_MEMCLK_1       0x30
#define TVP_MEMCLK_STATUS  0x33

/* Graphics processor tags (register = 0x8000 + 8 * tag). */
#define GP_TAGS 0x1000

/* A primitive in progress (it may wait for host data). */
typedef struct p2_raster_t {
    int      active;
    uint32_t command;
    uint32_t prim;
    int      bpp;
    int      texture_block;
    int      origin_x;
    int      origin_y;
    uint32_t columns;
    uint32_t rows;
    uint32_t col;
    uint32_t row;
    int64_t  xdom;
    int64_t  xsub;
    int64_t  y;
    int64_t  dxdom;
    int64_t  dxsub;
    int64_t  dy;
    uint32_t payload;
    uint32_t payload_left;
    uint32_t payload_tag;
    int      shade;        /* Gouraud colour interpolation. */
    int64_t  color[3];     /* R, G, B in 9.11 fixed point. */
    int64_t  dcolor_dx[3];
    int64_t  dcolor_dy[3];
    uint32_t alpha;
    int      rel_offset; /* Last: kept from one primitive to the next. */
} p2_raster_t;

typedef struct p2_t {
    svga_t svga;

    rom_t         bios_rom;
    mem_mapping_t ctrl_mapping;
    mem_mapping_t ap_mapping[2];

    uint8_t pci_slot;
    int     is_agp;
    int     tdlabs_id;
    uint8_t irq_state;
    uint8_t pci_regs[256];

    uint32_t vram_size;
    uint32_t vram_mask;

    uint32_t cfg_indirect_addr;
    uint32_t cfg_indirect_f4;

    /* Control registers. */
    uint32_t int_enable;
    uint32_t int_flags;
    uint32_t error_flags;
    uint32_t vclk_ctl;
    uint32_t test_register;
    uint32_t aperture[2];
    uint32_t dma_address;
    uint32_t dma_count;
    uint32_t dma_control;
    uint32_t fifo_discon;
    uint32_t chip_config;
    uint32_t mem_control;
    uint32_t boot_address;
    uint32_t mem_config;
    uint32_t bypass_mask;
    uint32_t fb_write_mask;
    uint32_t vs_regs[0x400];     /* 0x5000-0x5fff, kept for read back. */
    uint32_t ext_brd[2];
    uint32_t vtg[VTG_NUM];

    /* TVP4020 RAMDAC. */
    uint8_t  dac_index;
    uint8_t  dac_regs[0x50];
    uint8_t  cursor_ram[1024];
    uint8_t  cursor_col_addr;
    uint8_t  cursor_col_pos;
    uint8_t  cursor_col[3][3];
    uint32_t cursor_pal[3];
    uint16_t cursor_x;
    uint16_t cursor_y;

    /* Graphics processor. */
    uint32_t    gp[GP_TAGS];
    p2_raster_t raster;
    uint32_t fifo_hdr_count;     /* Data words still owed to the current header. */
    uint32_t fifo_hdr_tag;
    uint32_t fifo_hdr_mode;
    uint32_t fifo_hdr_mask;
    uint32_t out_fifo[256];
    int      out_rd;
    int      out_wr;
    int      out_count;
} p2_t;

static video_timings_t timing_p2_pci = { .type = VIDEO_PCI, .write_b = 2, .write_w = 2, .write_l = 1, .read_b = 10, .read_w = 10, .read_l = 10 };
static video_timings_t timing_p2_agp = { .type = VIDEO_AGP, .write_b = 2, .write_w = 2, .write_l = 1, .read_b = 10, .read_w = 10, .read_l = 10 };

#ifdef ENABLE_P2_LOG
int p2_do_log = ENABLE_P2_LOG;
#else
int p2_do_log = 0;
#endif

static void
p2_log(const char *fmt, ...)
{
    va_list ap;

    if (p2_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}

static void     p2_ctrl_write(p2_t *p2, uint32_t addr, uint32_t val);
static uint32_t p2_ctrl_read(p2_t *p2, uint32_t addr);
static void     p2_gp_write(p2_t *p2, uint32_t tag, uint32_t val);
static void     p2_gp_command(p2_t *p2, uint32_t tag, uint32_t val);
static void     p2_gp_reset(p2_t *p2);

/* ------------------------------------------------------------------------- */
/* Interrupts                                                                */
/* ------------------------------------------------------------------------- */

static void
p2_update_irq(p2_t *p2)
{
    if (p2->int_flags & p2->int_enable)
        pci_set_irq(p2->pci_slot, PCI_INTA, &p2->irq_state);
    else
        pci_clear_irq(p2->pci_slot, PCI_INTA, &p2->irq_state);
}

static void
p2_raise_int(p2_t *p2, uint32_t flag)
{
    p2->int_flags |= flag;
    p2_update_irq(p2);
}

/* The graphics pipeline is on screen while the VTG runs (VideoControl, which
   the STB VDD's EnableVGA clears) and the RAMDAC takes graphics pixels rather
   than the VGA core's (colour mode bit 4, which the BIOS mode set clears). */
static int
p2_vtg_active(p2_t *p2)
{
    return (p2->vtg[VTG_VIDEO_CONTROL] & VC_ENABLE) && (p2->dac_regs[TVP_COLOR_MODE] & 0x10);
}

static void
p2_vsync_callback(svga_t *svga)
{
    p2_t *p2 = (p2_t *) svga->priv;

    /* The border follows the palette entry, which the BIOS loads after AR11. */
    svga->overscan_color = p2_vtg_active(p2) ? 0x000000 : svga->pallook[svga->attrregs[0x11]];

    p2_raise_int(p2, INT_VRETRACE);
}

/* ------------------------------------------------------------------------- */
/* Output FIFO (host reads, filled by Sync and readback tags)                */
/* ------------------------------------------------------------------------- */

static void
p2_out_fifo_push(p2_t *p2, uint32_t val)
{
    if (p2->out_count >= 256)
        return;
    p2->out_fifo[p2->out_wr] = val;
    p2->out_wr               = (p2->out_wr + 1) & 255;
    p2->out_count++;
}

static uint32_t
p2_out_fifo_pop(p2_t *p2)
{
    uint32_t val;

    if (!p2->out_count)
        return 0;
    val        = p2->out_fifo[p2->out_rd];
    p2->out_rd = (p2->out_rd + 1) & 255;
    p2->out_count--;
    return val;
}

/* ------------------------------------------------------------------------- */
/* TVP4020 RAMDAC                                                            */
/* ------------------------------------------------------------------------- */

/* Pixel clock synthesiser n (0-2 = A-C): Fvco = 14.31818 MHz * M / N, Fout = Fvco >> P. */
static double
p2_pixclk(p2_t *p2, int n)
{
    const uint8_t m = p2->dac_regs[TVP_PIXCLK_A1 + n * 3];
    const uint8_t d = p2->dac_regs[TVP_PIXCLK_A1 + n * 3 + 1];
    const uint8_t p = p2->dac_regs[TVP_PIXCLK_A1 + n * 3 + 2];

    if (!m || !d || !(p & 0x08))
        return (n == 1) ? 28322000.0 : 25175000.0;

    return (14318180.0 * m / d) / (double) (1 << (p & 7));
}

static void
p2_update_cursor_pal(p2_t *p2)
{
    for (int i = 0; i < 3; i++)
        p2->cursor_pal[i] = makecol32(p2->cursor_col[i][0], p2->cursor_col[i][1], p2->cursor_col[i][2]);
}

static uint16_t
p2_cursor_ram_addr(p2_t *p2)
{
    return ((p2->dac_regs[TVP_CURSOR_CONTROL] & 0x0c) << 6) | p2->dac_index;
}

static void
p2_cursor_ram_advance(p2_t *p2)
{
    uint16_t a = (p2_cursor_ram_addr(p2) + 1) & 0x3ff;

    p2->dac_index                         = a & 0xff;
    p2->dac_regs[TVP_CURSOR_CONTROL] = (p2->dac_regs[TVP_CURSOR_CONTROL] & ~0x0c) | ((a >> 6) & 0x0c);
}

static void
p2_update_cursor(p2_t *p2)
{
    svga_t *svga = &p2->svga;
    int     size = (p2->dac_regs[TVP_CURSOR_CONTROL] & 0x40) ? 64 : 32;

    svga->dac_hwcursor.ena       = !!(p2->dac_regs[TVP_CURSOR_CONTROL] & 0x03);
    svga->dac_hwcursor.cur_xsize = size;
    svga->dac_hwcursor.cur_ysize = size;
    /* The position registers hold the cursor's bottom right corner. */
    svga->dac_hwcursor.x    = (int) p2->cursor_x - size;
    svga->dac_hwcursor.y    = (int) p2->cursor_y - size;
    svga->dac_hwcursor.xoff = 0;
    svga->dac_hwcursor.yoff = (svga->dac_hwcursor.y < 0) ? -svga->dac_hwcursor.y : 0;
    if (svga->dac_hwcursor.yoff > size)
        svga->dac_hwcursor.yoff = size;
    svga->dac_hwcursor.addr = svga->dac_hwcursor.yoff * 8;
}

static void
p2_dac_index_write(p2_t *p2, uint8_t idx, uint8_t val)
{
    svga_t *svga = &p2->svga;

    if (idx >= sizeof(p2->dac_regs))
        return;

    switch (idx) {
        case TVP_PIXCLK_STATUS:
        case TVP_MEMCLK_STATUS:
            return;

        default:
            break;
    }

    p2->dac_regs[idx] = val;

    switch (idx) {
        case TVP_CURSOR_CONTROL:
            p2_update_cursor(p2);
            break;
        case TVP_MISC_CONTROL:
            svga_set_ramdac_type(svga, (val & 0x02) ? RAMDAC_8BIT : RAMDAC_6BIT);
            svga_recalctimings(svga);
            break;
        case TVP_COLOR_MODE:
        case TVP_MODE_CONTROL:
        case TVP_PIXCLK_A1 ... TVP_PIXCLK_A1 + 8:
            svga_recalctimings(svga);
            break;
        default:
            break;
    }
}

static uint8_t
p2_dac_index_read(p2_t *p2, uint8_t idx)
{
    if (idx >= sizeof(p2->dac_regs))
        return 0x00;

    switch (idx) {
        case TVP_PIXCLK_STATUS:
        case TVP_MEMCLK_STATUS:
            return 0x10; /* Always locked. */
        default:
            return p2->dac_regs[idx];
    }
}

/* The 16 direct registers: 0-3 are the VGA DAC's own (write address, data, mask, read address). */
static void
p2_dac_write(p2_t *p2, int reg, uint8_t val)
{
    svga_t *svga = &p2->svga;

    switch (reg & 0x0f) {
        case 0x00:
            p2->dac_index = val;
            svga_out(0x3c8, val, svga);
            break;
        case 0x01:
            svga_out(0x3c9, val, svga);
            break;
        case 0x02:
            svga_out(0x3c6, val, svga);
            break;
        case 0x03:
            p2->dac_index = val;
            svga_out(0x3c7, val, svga);
            break;
        case 0x04:
            p2->cursor_col_addr = val;
            p2->cursor_col_pos  = 0;
            break;
        case 0x05:
            if ((p2->cursor_col_addr >= 1) && (p2->cursor_col_addr <= 3)) {
                p2->cursor_col[p2->cursor_col_addr - 1][p2->cursor_col_pos] = val;
                p2_update_cursor_pal(p2);
            }
            if (++p2->cursor_col_pos == 3) {
                p2->cursor_col_pos = 0;
                p2->cursor_col_addr++;
            }
            break;
        case 0x0a:
            p2_dac_index_write(p2, p2->dac_index, val);
            break;
        case 0x0b:
            p2->cursor_ram[p2_cursor_ram_addr(p2)] = val;
            p2_cursor_ram_advance(p2);
            break;
        case 0x0c:
            p2->cursor_x = (p2->cursor_x & 0x0f00) | val;
            p2_update_cursor(p2);
            break;
        case 0x0d:
            p2->cursor_x = (p2->cursor_x & 0x00ff) | ((val & 0x0f) << 8);
            p2_update_cursor(p2);
            break;
        case 0x0e:
            p2->cursor_y = (p2->cursor_y & 0x0f00) | val;
            p2_update_cursor(p2);
            break;
        case 0x0f:
            p2->cursor_y = (p2->cursor_y & 0x00ff) | ((val & 0x0f) << 8);
            p2_update_cursor(p2);
            break;
        default:
            break;
    }
}

static uint8_t
p2_dac_read(p2_t *p2, int reg)
{
    svga_t *svga = &p2->svga;
    uint8_t ret  = 0x00;

    switch (reg & 0x0f) {
        case 0x00:
            ret = p2->dac_index;
            break;
        case 0x01:
            ret = svga_in(0x3c9, svga);
            break;
        case 0x02:
            ret = svga_in(0x3c6, svga);
            break;
        case 0x03:
            ret = svga_in(0x3c7, svga);
            break;
        case 0x04:
            ret = p2->cursor_col_addr;
            break;
        case 0x05:
            if ((p2->cursor_col_addr >= 1) && (p2->cursor_col_addr <= 3))
                ret = p2->cursor_col[p2->cursor_col_addr - 1][p2->cursor_col_pos];
            if (++p2->cursor_col_pos == 3) {
                p2->cursor_col_pos = 0;
                p2->cursor_col_addr++;
            }
            break;
        case 0x0a:
            ret = p2_dac_index_read(p2, p2->dac_index);
            break;
        case 0x0b:
            ret = p2->cursor_ram[p2_cursor_ram_addr(p2)];
            p2_cursor_ram_advance(p2);
            break;
        case 0x0c:
            ret = p2->cursor_x & 0xff;
            break;
        case 0x0d:
            ret = p2->cursor_x >> 8;
            break;
        case 0x0e:
            ret = p2->cursor_y & 0xff;
            break;
        case 0x0f:
            ret = p2->cursor_y >> 8;
            break;
        default:
            break;
    }

    return ret;
}

static void
p2_hwcursor_draw(svga_t *svga, int displine)
{
    p2_t          *p2     = (p2_t *) svga->priv;
    const int      size   = svga->dac_hwcursor_latch.cur_xsize;
    const int      plane  = 0x200;
    const int      mode   = p2->dac_regs[TVP_CURSOR_CONTROL] & 0x03;
    const uint8_t *cd     = p2->cursor_ram;
    uint32_t       base   = svga->dac_hwcursor_latch.addr;
    int            offset = svga->dac_hwcursor_latch.x;
    uint32_t      *p      = svga->monitor->target_buffer->line[displine];

    /* The RAM always holds two 64x64 planes, 8 bytes a row, MSB first; a
       32x32 cursor is one quadrant of them. */
    if (size == 32) {
        const int sel = (p2->dac_regs[TVP_CURSOR_CONTROL] >> 4) & 3;

        base += (sel & 1) * 4 + (sel >> 1) * 256;
    }

    for (int x = 0; x < size; x += 8) {
        const uint8_t b0 = cd[(base + (x >> 3)) & 0x3ff];
        const uint8_t b1 = cd[(base + plane + (x >> 3)) & 0x3ff];

        for (int xx = 0; xx < 8; xx++, offset++) {
            const int comb  = ((b0 >> (7 - xx)) & 1) | (((b1 >> (7 - xx)) & 1) << 1);
            const int x_pos = (offset + svga->x_add) & 2047;

            if (offset < 0)
                continue;

            switch (mode) {
                case 1: /* Three colours. */
                    if (comb)
                        p[x_pos] = p2->cursor_pal[comb - 1];
                    break;
                case 2: /* XGA. */
                    if (comb == 0)
                        p[x_pos] = p2->cursor_pal[0];
                    else if (comb == 1)
                        p[x_pos] = p2->cursor_pal[1];
                    else if (comb == 3)
                        p[x_pos] ^= 0xffffff;
                    break;
                case 3: /* X Windows. */
                    if (comb == 2)
                        p[x_pos] = p2->cursor_pal[0];
                    else if (comb == 3)
                        p[x_pos] = p2->cursor_pal[1];
                    break;
                default:
                    break;
            }
        }
    }

    svga->dac_hwcursor_latch.addr += 8;
}

/* ------------------------------------------------------------------------- */
/* VGA core                                                                  */
/* ------------------------------------------------------------------------- */

static int
p2_dac_port_reg(uint16_t addr)
{
    switch (addr) {
        case 0x3c8:
            return 0;
        case 0x3c9:
            return 1;
        case 0x3c6:
            return 2;
        default: /* 0x3c7 */
            return 3;
    }
}

static void
p2_update_banks(p2_t *p2)
{
    svga_t *svga = &p2->svga;

    if (svga->gdcreg[9] & 0x80) {
        svga->write_bank = (svga->gdcreg[9] & 0x07) << 16;
        svga->read_bank  = svga->write_bank;
    } else {
        svga->write_bank = 0;
        svga->read_bank  = 0;
    }
}

static void
p2_out(uint16_t addr, uint8_t val, void *priv)
{
    p2_t   *p2   = (p2_t *) priv;
    svga_t *svga = &p2->svga;
    uint8_t old;

    if (((addr & 0xfff0) == 0x3d0 || (addr & 0xfff0) == 0x3b0) && !(svga->miscout & 1))
        addr ^= 0x60;

    switch (addr) {
        case 0x3c5:
            if (svga->seqaddr == 0x05) {
                old              = svga->seqregs[5];
                svga->seqregs[5] = val;
                if ((old ^ val) & 0x48)
                    svga_recalctimings(svga);
                return;
            }
            if (svga->seqaddr > 0x05)
                return; /* No other extended sequencer registers. */
            break;

        case 0x3c6:
        case 0x3c7:
        case 0x3c8:
        case 0x3c9:
            if (svga->seqregs[5] & 0x30) {
                p2_dac_write(p2, ((svga->seqregs[5] & 0x30) >> 2) | p2_dac_port_reg(addr), val);
                return;
            }
            if (addr == 0x3c8 || addr == 0x3c7)
                p2->dac_index = val;
            break;

        case 0x3d4:
            svga->crtcreg = val & 0x3f;
            return;
        case 0x3d5:
            if (svga->crtcreg > 0x18)
                return;
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
                        if (svga->gdcreg[9] & 0x80)
                            svga->memaddr_latch |= (svga->gdcreg[9] & 0x40) << 10;
                    } else {
                        svga->fullchange = svga->monitor->mon_changeframecount;
                        svga_recalctimings(svga);
                    }
                }
            }
            return;

        case 0x3cf:
            if (svga->gdcaddr == 0x09) {
                old              = svga->gdcreg[9];
                svga->gdcreg[9]  = val;
                p2_update_banks(p2);
                if ((old ^ val) & 0xc0)
                    svga_recalctimings(svga);
                return;
            }
            if (svga->gdcaddr > 0x09)
                return;
            break;

        default:
            break;
    }

    svga_out(addr, val, svga);
}

static uint8_t
p2_in(uint16_t addr, void *priv)
{
    p2_t   *p2   = (p2_t *) priv;
    svga_t *svga = &p2->svga;

    if (((addr & 0xfff0) == 0x3d0 || (addr & 0xfff0) == 0x3b0) && !(svga->miscout & 1))
        addr ^= 0x60;

    switch (addr) {
        case 0x3c5:
            if (svga->seqaddr == 0x05)
                return svga->seqregs[5];
            if (svga->seqaddr > 0x05)
                return 0xff;
            break;

        case 0x3c6:
        case 0x3c7:
        case 0x3c8:
        case 0x3c9:
            if (svga->seqregs[5] & 0x30)
                return p2_dac_read(p2, ((svga->seqregs[5] & 0x30) >> 2) | p2_dac_port_reg(addr));
            break;

        case 0x3cf:
            if (svga->gdcaddr == 0x09)
                return svga->gdcreg[9];
            if (svga->gdcaddr > 0x09)
                return 0xff;
            break;

        case 0x3d4:
            return svga->crtcreg;
        case 0x3d5:
            if (svga->crtcreg > 0x18)
                return 0xff;
            return svga->crtc[svga->crtcreg];

        default:
            break;
    }

    return svga_in(addr, svga);
}

/* ------------------------------------------------------------------------- */
/* Display                                                                   */
/* ------------------------------------------------------------------------- */

static int
p2_bpp(p2_t *p2)
{
    const uint8_t cm = p2->dac_regs[TVP_COLOR_MODE];

    if (!(cm & 0x10))
        return 8;

    switch (cm & 0x0f) {
        case 0x04:
            return 15;
        case 0x05:
        case 0x06:
            return 16;
        case 0x08:
            return 32;
        case 0x09:
            return 24;
        default:
            return 8;
    }
}

static void
p2_recalctimings(svga_t *svga)
{
    p2_t *p2 = (p2_t *) svga->priv;

    svga->border_override = 0;
    /* The attribute controller's blink bit is the VGA core's: the graphics
       pipeline never blinks (the drivers pass through a text mode on each
       mode switch and leave AR10 bit 3 set). */
    svga->disable_blink = p2_vtg_active(p2);

    if (!p2_vtg_active(p2)) {
        /* The VGA core drives the display, if SR05 lets it. */
        const int    sel  = (svga->miscout >> 2) & 3;
        const double freq = p2_pixclk(p2, (sel > 2) ? 2 : sel);

        svga->clock = (cpuclock * (double) (1ULL << 32)) / freq;

        /* The BIOS runs most VGA modes from a doubled pixel clock with SR01's
           divide by two on (TVP4020DivClk): that is a normal width picture,
           not a 40 column/320 pixel one. A real low resolution mode keeps
           the 25/28 MHz clock. */
        if ((svga->seqregs[1] & 0x08) && (freq > 40000000.0) && !svga->scrblank) {
            if (svga->render == svga_render_text_40)
                svga->render = svga_render_text_80;
            else if (svga->render == svga_render_4bpp_lowres)
                svga->render = svga_render_4bpp_highres;
            else if (svga->render == svga_render_2bpp_lowres)
                svga->render = svga_render_2bpp_highres;
            svga->hdisp >>= 1;
            svga->dots_per_clock >>= 1;
        }

        if (svga->gdcreg[9] & 0x80) {
            /* Mode640: start address bit 16; packed 256-colour at one byte per pixel. */
            svga->memaddr_latch |= (svga->gdcreg[9] & 0x40) << 10;
            if ((svga->attrregs[0x10] & 0x40) && (svga->render != svga_render_blank) &&
                (svga->gdcreg[6] & 1)) {
                /* 8 bit colour with chain 4 (GR05 has no 256 colour shift
                   here): one byte per pixel, two dot clocks each. */
                svga->map8           = svga->pallook;
                svga->render         = svga_render_8bpp_highres;
                svga->hdisp >>= 1;
                svga->dots_per_clock = 4;
            }
        }
        if (!(svga->seqregs[5] & 0x08))
            svga->render = svga_render_blank;
        return;
    }

    /* The video timing generator drives the display. */
    const uint32_t vc       = p2->vtg[VTG_VIDEO_CONTROL];
    const int      bpp      = p2_bpp(p2);
    const int      unitbits = (vc & VC_DATA64) ? 64 : 32;
    const int      htotal   = (p2->vtg[VTG_H_TOTAL] & 0x7ff) + 1;
    const int      hbend    = p2->vtg[VTG_HB_END] & 0x7ff;
    const int      vtotal   = (p2->vtg[VTG_V_TOTAL] & 0x7ff) + 1;
    const int      vbend    = p2->vtg[VTG_VB_END] & 0x7ff;
    const int      bits     = (bpp == 15) ? 16 : bpp;
    int            hdisp    = ((htotal - hbend) * unitbits) / bits;
    int            htot_px  = (htotal * unitbits) / bits;
    int            vdisp    = vtotal - vbend;

    if (hdisp <= 0 || vdisp <= 0 || htot_px <= 0) {
        hdisp   = 640;
        htot_px = 800;
        vdisp   = 480;
    }

    svga->clock = (cpuclock * (double) (1ULL << 32)) / p2_pixclk(p2, ((p2->vclk_ctl & 3) == 3) ? 2 : (p2->vclk_ctl & 3));

    svga->vtotal      = vtotal;
    svga->dispend     = vdisp;
    svga->vblankstart = vdisp;
    svga->vsyncstart  = vdisp + (p2->vtg[VTG_VS_START] & 0x7ff);
    if (svga->vsyncstart >= vtotal)
        svga->vsyncstart = vtotal - 1;
    svga->split = 0x7ff + 1;

    svga->hdisp          = hdisp;
    svga->hdisp_time     = hdisp >> 3;
    svga->htotal         = htot_px >> 3;
    svga->char_width     = 8;
    svga->dots_per_clock = 8;
    svga->hblankstart    = svga->htotal;
    svga->hblank_sub     = 0;

    svga->rowoffset     = p2->vtg[VTG_SCREEN_STRIDE] & 0xfff;
    svga->memaddr_latch = (p2->vtg[VTG_SCREEN_BASE] & 0xfffff) << 1;
    svga->rowcount      = 0;
    svga->linedbl       = !!(vc & VC_LINE_DOUBLE);
    svga->lowres        = 0;
    svga->interlace     = 0;
    svga->bpp           = bpp;

    svga->border_override = 1;
    svga->border_left     = 0;
    svga->border_top      = 0;
    svga->monitor->mon_overscan_x = 0;
    svga->monitor->mon_overscan_y = 0;

    /* Syncs forced low (bits 4:3, 6:5 = 2) is the drivers' DPMS blanking. */
    if ((((vc >> 3) & 3) == 2) || (((vc >> 5) & 3) == 2)) {
        svga->render = svga_render_blank;
        return;
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
/* Linear framebuffer apertures                                              */
/* ------------------------------------------------------------------------- */

static inline void
p2_vram_changed(p2_t *p2, uint32_t addr)
{
    p2->svga.changedvram[addr >> 12] = p2->svga.monitor->mon_changeframecount;
}

static uint8_t
p2_lfb_readb(uint32_t addr, void *priv)
{
    const p2_t *p2 = (p2_t *) priv;

    return p2->svga.vram[addr & p2->vram_mask];
}

static uint16_t
p2_lfb_readw(uint32_t addr, void *priv)
{
    const p2_t *p2 = (p2_t *) priv;

    return *(uint16_t *) &p2->svga.vram[addr & p2->vram_mask & ~1];
}

static uint32_t
p2_lfb_readl(uint32_t addr, void *priv)
{
    const p2_t *p2 = (p2_t *) priv;

    return *(uint32_t *) &p2->svga.vram[addr & p2->vram_mask & ~3];
}

static void
p2_lfb_writeb(uint32_t addr, uint8_t val, void *priv)
{
    p2_t          *p2   = (p2_t *) priv;
    const uint8_t  mask = p2->bypass_mask >> ((addr & 3) << 3);

    addr &= p2->vram_mask;
    p2->svga.vram[addr] = (p2->svga.vram[addr] & ~mask) | (val & mask);
    p2_vram_changed(p2, addr);
}

static void
p2_lfb_writew(uint32_t addr, uint16_t val, void *priv)
{
    p2_t          *p2   = (p2_t *) priv;
    const uint16_t mask = p2->bypass_mask >> ((addr & 2) << 3);
    uint16_t      *v;

    addr &= p2->vram_mask & ~1;
    v  = (uint16_t *) &p2->svga.vram[addr];
    *v = (*v & ~mask) | (val & mask);
    p2_vram_changed(p2, addr);
}

static void
p2_lfb_writel(uint32_t addr, uint32_t val, void *priv)
{
    p2_t     *p2 = (p2_t *) priv;
    uint32_t *v;

    addr &= p2->vram_mask & ~3;
    v  = (uint32_t *) &p2->svga.vram[addr];
    *v = (*v & ~p2->bypass_mask) | (val & p2->bypass_mask);
    p2_vram_changed(p2, addr);
}

/* ------------------------------------------------------------------------- */
/* Control space                                                             */
/* ------------------------------------------------------------------------- */

static void
p2_fifo_write(p2_t *p2, uint32_t val)
{
    if (!p2->fifo_hdr_count) {
        /* A tag word: [8:0] tag, [15:14] mode, [31:16] count - 1 or the indexed mask. */
        p2->fifo_hdr_tag  = val & 0x3ff;
        p2->fifo_hdr_mode = (val >> 14) & 3;
        if (p2->fifo_hdr_mode == 2) {
            p2->fifo_hdr_mask  = val >> 16;
            p2->fifo_hdr_tag   = val & 0x3f0;
            p2->fifo_hdr_count = 0;
            for (int i = 0; i < 16; i++)
                p2->fifo_hdr_count += (p2->fifo_hdr_mask >> i) & 1;
        } else
            p2->fifo_hdr_count = (val >> 16) + 1;
        return;
    }

    switch (p2->fifo_hdr_mode) {
        case 1: /* Increment. */
            p2_gp_write(p2, p2->fifo_hdr_tag, val);
            p2->fifo_hdr_tag = (p2->fifo_hdr_tag + 1) & 0x3ff;
            break;
        case 2: /* Indexed. */
            for (int i = 0; i < 16; i++) {
                if (p2->fifo_hdr_mask & (1 << i)) {
                    p2->fifo_hdr_mask &= ~(1 << i);
                    p2_gp_write(p2, p2->fifo_hdr_tag | i, val);
                    break;
                }
            }
            break;
        default: /* Hold. */
            p2_gp_write(p2, p2->fifo_hdr_tag, val);
            break;
    }

    p2->fifo_hdr_count--;
}

static void
p2_vtg_write(p2_t *p2, int idx, uint32_t val)
{
    svga_t *svga = &p2->svga;

    switch (idx) {
        case VTG_LINE_COUNT:
            return;
        case VTG_SCREEN_BASE:
            p2->vtg[idx] = val;
            if (p2_vtg_active(p2))
                svga->memaddr_latch = (val & 0xfffff) << 1;
            svga->fullchange = 3;
            return;
        default:
            break;
    }

    if (p2->vtg[idx] != val) {
        p2->vtg[idx] = val;
        if (idx <= VTG_VIDEO_CONTROL)
            svga_recalctimings(svga);
    }
}

static void
p2_ctrl_write_int(p2_t *p2, uint32_t addr, uint32_t val)
{
    svga_t *svga = &p2->svga;

    addr &= 0xfffc;

    if (addr >= 0x8000) {
        p2_gp_write(p2, (addr - 0x8000) >> 3, val);
        return;
    }

    switch (addr & 0xf000) {
        case 0x2000:
            p2_fifo_write(p2, val);
            return;

        case 0x3000:
            if (!(addr & 7) && ((addr & 0xfff) >> 3) < VTG_NUM)
                p2_vtg_write(p2, (addr & 0xfff) >> 3, val);
            return;

        case 0x4000:
            p2_dac_write(p2, (addr >> 3) & 0x0f, val & 0xff);
            return;

        case 0x5000:
            p2->vs_regs[(addr & 0xfff) >> 2] = val;
            return;

        case 0x6000:
            if ((addr & 0xffe0) == 0x63c0) {
                /* The VGA registers, byte for byte at 0x3c0 + offset. */
                p2_out(0x3c0 + (addr & 0x1f), val & 0xff, p2);
                p2_out(0x3c1 + (addr & 0x1f), (val >> 8) & 0xff, p2);
                return;
            }
            if (addr < 0x6010)
                p2->ext_brd[(addr >> 3) & 1] = val;
            return;

        default:
            break;
    }

    switch (addr) {
        case P2_RESET_STATUS:
            p2_gp_reset(p2);
            break;
        case P2_INT_ENABLE:
            p2->int_enable = val;
            p2_update_irq(p2);
            break;
        case P2_INT_FLAGS:
            p2->int_flags &= ~val;
            p2_update_irq(p2);
            break;
        case P2_DMA_ADDRESS:
            p2->dma_address = val;
            break;
        case P2_DMA_COUNT:
            p2->dma_count = val & 0xffff;
            if (p2->dma_count) {
                /* Feed the input FIFO from system memory at once. */
                while (p2->dma_count) {
                    p2_fifo_write(p2, mem_readl_phys(p2->dma_address));
                    p2->dma_address += 4;
                    p2->dma_count--;
                }
                p2_raise_int(p2, INT_DMA);
            }
            break;
        case P2_ERROR_FLAGS:
            p2->error_flags &= ~val;
            break;
        case P2_VCLK_CTL:
            p2->vclk_ctl = val;
            svga_recalctimings(svga);
            break;
        case P2_TEST_REGISTER:
            p2->test_register = val;
            break;
        case P2_APERTURE_ONE:
            p2->aperture[0] = val;
            break;
        case P2_APERTURE_TWO:
            p2->aperture[1] = val;
            break;
        case P2_DMA_CONTROL:
            p2->dma_control = val;
            break;
        case P2_FIFO_DISCON:
            p2->fifo_discon = val;
            break;
        case P2_CHIP_CONFIG:
            p2->chip_config = (p2->chip_config & 0x00000200) | (val & ~0x00000200);
            break;

        case P2_REBOOT:
            break;
        case P2_MEM_CONTROL:
            p2->mem_control = val;
            break;
        case P2_BOOT_ADDRESS:
            p2->boot_address = val;
            break;
        case P2_MEM_CONFIG:
            p2->mem_config = val;
            break;
        case P2_BYPASS_MASK:
            p2->bypass_mask = val;
            break;
        case P2_FB_WRITE_MASK:
            p2->fb_write_mask = val;
            break;

        default:
            p2_log("P2: unhandled control write %04X = %08X\n", addr, val);
            break;
    }
}

static uint32_t
p2_ctrl_read_int(p2_t *p2, uint32_t addr)
{
    svga_t  *svga = &p2->svga;
    uint32_t ret  = 0x00000000;

    addr &= 0xfffc;

    if (addr >= 0x8000)
        return p2->gp[((addr - 0x8000) >> 3) & (GP_TAGS - 1)];

    switch (addr & 0xf000) {
        case 0x2000:
            return p2_out_fifo_pop(p2);

        case 0x3000:
            if (!(addr & 7) && ((addr & 0xfff) >> 3) < VTG_NUM) {
                if (((addr & 0xfff) >> 3) == VTG_LINE_COUNT)
                    return svga->vc;
                return p2->vtg[(addr & 0xfff) >> 3];
            }
            return 0;

        case 0x4000:
            return p2_dac_read(p2, (addr >> 3) & 0x0f);

        case 0x5000:
            return p2->vs_regs[(addr & 0xfff) >> 2];

        case 0x6000:
            if ((addr & 0xffe0) == 0x63c0)
                return p2_in(0x3c0 + (addr & 0x1f), p2) | (p2_in(0x3c1 + (addr & 0x1f), p2) << 8);
            if (addr < 0x6010)
                return p2->ext_brd[(addr >> 3) & 1];
            return 0;

        default:
            break;
    }

    switch (addr) {
        case P2_RESET_STATUS:
            ret = 0; /* Never busy resetting. */
            break;
        case P2_INT_ENABLE:
            ret = p2->int_enable;
            break;
        case P2_INT_FLAGS:
            ret = p2->int_flags;
            break;
        case P2_IN_FIFO_SPACE:
            ret = 256;
            break;
        case P2_OUT_FIFO_WORDS:
            ret = p2->out_count;
            break;
        case P2_DMA_ADDRESS:
            ret = p2->dma_address;
            break;
        case P2_DMA_COUNT:
            ret = p2->dma_count;
            break;
        case P2_ERROR_FLAGS:
            ret = p2->error_flags;
            break;
        case P2_VCLK_CTL:
            ret = p2->vclk_ctl;
            break;
        case P2_TEST_REGISTER:
            ret = p2->test_register;
            break;
        case P2_APERTURE_ONE:
            ret = p2->aperture[0];
            break;
        case P2_APERTURE_TWO:
            ret = p2->aperture[1];
            break;
        case P2_DMA_CONTROL:
            ret = p2->dma_control;
            break;
        case P2_FIFO_DISCON:
            ret = p2->fifo_discon;
            break;
        case P2_CHIP_CONFIG:
            ret = p2->chip_config;
            break;
        case P2_MEM_CONTROL:
            ret = p2->mem_control;
            break;
        case P2_BOOT_ADDRESS:
            ret = p2->boot_address;
            break;
        case P2_MEM_CONFIG:
            ret = p2->mem_config;
            break;
        case P2_BYPASS_MASK:
            ret = p2->bypass_mask;
            break;
        case P2_FB_WRITE_MASK:
            ret = p2->fb_write_mask;
            break;
        case P2_MCLK_COUNT:
            ret = 0;
            break;
        default:
            p2_log("P2: unhandled control read %04X\n", addr);
            break;
    }

    return ret;
}

/* Level 2 logging traces every control space access (not the GP's). */
static void
p2_ctrl_write(p2_t *p2, uint32_t addr, uint32_t val)
{
    if ((p2_do_log >= 2) && ((addr & 0xfffc) < 0x8000) && ((addr & 0xf000) != 0x2000))
        pclog("P2: W %04X = %08X\n", addr & 0xfffc, val);
    if ((p2_do_log >= 3) && ((addr & 0xfffc) >= 0x8000))
        pclog("P2: GP %03X = %08X\n", ((addr & 0xfffc) - 0x8000) >> 3, val);
    p2_ctrl_write_int(p2, addr, val);
}

static uint32_t
p2_ctrl_read(p2_t *p2, uint32_t addr)
{
    uint32_t ret = p2_ctrl_read_int(p2, addr);

    if ((p2_do_log >= 2) && ((addr & 0xfffc) != P2_IN_FIFO_SPACE) && ((addr & 0xfffc) != 0x3070))
        pclog("P2: R %04X = %08X\n", addr & 0xfffc, ret);
    return ret;
}

static uint8_t
p2_mmio_readb(uint32_t addr, void *priv)
{
    p2_t *p2 = (p2_t *) priv;

    addr &= 0xffff;
    if ((addr & 0xffe0) == 0x63c0)
        return p2_in(0x3c0 + (addr & 0x1f), p2);
    return p2_ctrl_read(p2, addr) >> ((addr & 3) << 3);
}

static uint16_t
p2_mmio_readw(uint32_t addr, void *priv)
{
    p2_t *p2 = (p2_t *) priv;

    addr &= 0xffff;
    if ((addr & 0xffe0) == 0x63c0)
        return p2_in(0x3c0 + (addr & 0x1f), p2) | (p2_in(0x3c1 + (addr & 0x1f), p2) << 8);
    return p2_ctrl_read(p2, addr) >> ((addr & 2) << 3);
}

static uint32_t
p2_mmio_readl(uint32_t addr, void *priv)
{
    p2_t *p2 = (p2_t *) priv;

    return p2_ctrl_read(p2, addr & 0xffff);
}

static void
p2_mmio_writeb(uint32_t addr, uint8_t val, void *priv)
{
    p2_t *p2 = (p2_t *) priv;

    addr &= 0xffff;
    if ((addr & 0xffe0) == 0x63c0)
        p2_out(0x3c0 + (addr & 0x1f), val, p2);
    else if ((addr & 0xf000) == 0x4000)
        p2_dac_write(p2, (addr >> 3) & 0x0f, val);
    else
        p2_ctrl_write(p2, addr, val);
}

static void
p2_mmio_writew(uint32_t addr, uint16_t val, void *priv)
{
    p2_t *p2 = (p2_t *) priv;

    addr &= 0xffff;
    if ((addr & 0xffe0) == 0x63c0) {
        p2_out(0x3c0 + (addr & 0x1f), val & 0xff, p2);
        p2_out(0x3c1 + (addr & 0x1f), val >> 8, p2);
    } else
        p2_ctrl_write(p2, addr, val);
}

static void
p2_mmio_writel(uint32_t addr, uint32_t val, void *priv)
{
    p2_t *p2 = (p2_t *) priv;

    p2_ctrl_write(p2, addr & 0xffff, val);
}

/* ------------------------------------------------------------------------- */
/* Graphics processor                                                        */
/* ------------------------------------------------------------------------- */

#define TAG_FILTER_MODE 0x180
#define TAG_SYNC        0x188

static void
p2_gp_reset(p2_t *p2)
{
    p2->fifo_hdr_count = 0;
    p2->out_rd = p2->out_wr = p2->out_count = 0;
}

static void
p2_gp_write(p2_t *p2, uint32_t tag, uint32_t val)
{
    tag &= GP_TAGS - 1;

    switch (tag) {
        case TAG_SYNC:
            /* Everything before it has finished: tell the host. */
            if (p2->gp[TAG_FILTER_MODE] & (1 << 10))
                p2_out_fifo_push(p2, TAG_SYNC);
            if (p2->gp[TAG_FILTER_MODE] & (1 << 11))
                p2_out_fifo_push(p2, val);
            if (val & (1 << 31))
                p2_raise_int(p2, INT_SYNC);
            break;

        default:
            p2->gp[tag] = val;
            p2_gp_command(p2, tag, val);
            break;
    }
}

/* ------------------------------------------------------------------------- */
/* Rasterizer and framebuffer units (the 2D paths)                           */
/*                                                                           */
/* The fragment rules follow the Permedia 2 model in ES40's                  */
/* src/graphics/Permedia2.cpp (Copyright (C) 2026 gdwnldsKSC,                */
/* BSD-1-Clause): packed DWORD groups widened to native pixels, separate     */
/* source (FBReadMode) and destination (FBWriteConfig) pitches, the 64-bit   */
/* block fill pattern, bit mask and host data streams.                       */
/* ------------------------------------------------------------------------- */

#define TAG_START_X_DOM         0x000
#define TAG_DX_DOM              0x001
#define TAG_START_X_SUB         0x002
#define TAG_DX_SUB              0x003
#define TAG_START_Y             0x004
#define TAG_DY                  0x005
#define TAG_COUNT               0x006
#define TAG_RENDER              0x007
#define TAG_CONTINUE_NEW_LINE   0x008
#define TAG_CONTINUE_NEW_DOM    0x009
#define TAG_CONTINUE_NEW_SUB    0x00a
#define TAG_CONTINUE            0x00b
#define TAG_BIT_MASK_PATTERN    0x00d
#define TAG_RASTERIZER_MODE     0x014
#define TAG_Y_LIMITS            0x015
#define TAG_X_LIMITS            0x019
#define TAG_RECT_ORIGIN         0x01a
#define TAG_RECT_SIZE           0x01b
#define TAG_PACKED_DATA_LIMITS  0x02a
#define TAG_SCISSOR_MODE        0x030
#define TAG_SCISSOR_MIN         0x031
#define TAG_SCISSOR_MAX         0x032
#define TAG_SCREEN_SIZE         0x033
#define TAG_AREA_STIPPLE_MODE   0x034
#define TAG_WINDOW_ORIGIN       0x039
#define TAG_AREA_STIPPLE_PAT    0x040
#define TAG_S_START             0x071
#define TAG_DS_DX               0x072
#define TAG_T_START             0x074
#define TAG_TEXTURE_BASE        0x0b0
#define TAG_TEXTURE_MAP_FORMAT  0x0b1
#define TAG_TEXTURE_DATA_FORMAT 0x0b2
#define TAG_TEXEL0              0x0c0
#define TAG_TEXTURE_READ_MODE   0x0ce
#define TAG_R_START             0x0f0
#define TAG_A_START             0x0f9
#define TAG_COLOR_DDA_MODE      0x0fc
#define TAG_CONSTANT_COLOR      0x0fd
#define TAG_COLOR               0x0fe
#define TAG_DITHER_MODE         0x103
#define TAG_FB_SOFT_WRITE_MASK  0x104
#define TAG_LOGICAL_OP_MODE     0x105
#define TAG_FB_WRITE_DATA       0x106
#define TAG_FB_READ_MODE        0x150
#define TAG_FB_SOURCE_OFFSET    0x151
#define TAG_FB_PIXEL_OFFSET     0x152
#define TAG_FB_COLOR            0x153
#define TAG_FB_DATA             0x154
#define TAG_FB_SOURCE_DATA      0x155
#define TAG_FB_WINDOW_BASE      0x156
#define TAG_FB_WRITE_MODE       0x157
#define TAG_FB_HW_WRITE_MASK    0x158
#define TAG_FB_BLOCK_COLOR      0x159
#define TAG_FB_READ_PIXEL       0x15a
#define TAG_FB_WRITE_CONFIG     0x15d
#define TAG_FB_BLOCK_COLOR_U    0x18d
#define TAG_FB_BLOCK_COLOR_L    0x18e
#define TAG_FB_SOURCE_BASE      0x1b0
#define TAG_FB_SOURCE_DELTA     0x1b1
#define TAG_CONFIG              0x1b2

#define RENDER_AREA_STIPPLE  (1 << 0)
#define RENDER_FAST_FILL     (1 << 3)
#define RENDER_PRIM_MASK     0xc0
#define RENDER_SYNC_BITMASK  (1 << 11)
#define RENDER_SYNC_HOSTDATA (1 << 12)
#define RENDER_TEXTURE       (1 << 13)
#define RENDER_X_POSITIVE    (1 << 21)
#define RENDER_Y_POSITIVE    (1 << 22)

#define PRIM_LINE  0x00
#define PRIM_TRAP  0x40
#define PRIM_POINT 0x80
#define PRIM_RECT  0xc0

#define FBRM_SRC_ENABLE (1 << 9)
#define FBRM_DST_ENABLE (1 << 10)
#define FBRM_DATA_TYPE  (1 << 15)
#define FBRM_ORIGIN_BOT (1 << 16)
#define FBRM_PACKED     (1 << 19)

static int
p2_sext(uint32_t v, int bits)
{
    return (int32_t) (v << (32 - bits)) >> (32 - bits);
}

/* The integer part of a 16.16 DDA value, rounding towards minus infinity. */
static int64_t
p2_fixed_int(int64_t v)
{
    return (v >= 0) ? (v / 65536) : -((-v + 65535) / 65536);
}

/* Partial products: three 3-bit codes, each 0 or 32 << (code - 1) pixels. */
static uint32_t
p2_pitch(uint32_t mode)
{
    uint32_t pitch = 0;

    for (int i = 0; i < 9; i += 3) {
        const uint32_t pp = (mode >> i) & 7;

        if (pp)
            pitch += 1 << (pp + 4);
    }

    return pitch;
}

static int
p2_fb_bpp(p2_t *p2)
{
    switch (p2->gp[TAG_FB_READ_PIXEL] & 7) {
        case 0:
            return 1;
        case 1:
            return 2;
        case 4:
            return 3;
        default:
            return 4;
    }
}

static uint32_t
p2_fb_read(p2_t *p2, int64_t addr, int bpp)
{
    const uint8_t *vram = p2->svga.vram;
    uint32_t       v    = 0;

    if ((addr < 0) || (addr > (int64_t) p2->vram_size - bpp))
        return 0;
    for (int i = 0; i < bpp; i++)
        v |= (uint32_t) vram[addr + i] << (i << 3);
    return v;
}

static void
p2_fb_write(p2_t *p2, int64_t addr, int bpp, uint32_t val, uint32_t hwmask)
{
    uint8_t *vram = p2->svga.vram;

    if ((addr < 0) || (addr > (int64_t) p2->vram_size - bpp))
        return;
    for (int i = 0; i < bpp; i++) {
        const uint32_t a = (uint32_t) addr + i;
        const uint8_t  m = hwmask >> ((a & 3) << 3);

        vram[a] = (vram[a] & ~m) | ((val >> (i << 3)) & m);
    }
    p2_vram_changed(p2, (uint32_t) addr);
    p2_vram_changed(p2, (uint32_t) addr + bpp - 1);
}

static uint32_t
p2_logic_op(int op, uint32_t s, uint32_t d)
{
    switch (op & 15) {
        case 0:
            return 0;
        case 1:
            return s & d;
        case 2:
            return s & ~d;
        case 3:
            return s;
        case 4:
            return ~s & d;
        case 5:
            return d;
        case 6:
            return s ^ d;
        case 7:
            return s | d;
        case 8:
            return ~(s | d);
        case 9:
            return ~(s ^ d);
        case 10:
            return ~d;
        case 11:
            return s | ~d;
        case 12:
            return ~s;
        case 13:
            return ~s | d;
        case 14:
            return ~(s & d);
        default:
            return 0xffffffff;
    }
}

/* An internal AABBGGRR colour in the framebuffer's format (the dither unit). */
static uint32_t
p2_format_color(p2_t *p2, uint32_t v, int x, int y)
{
    const uint32_t dm  = p2->gp[TAG_DITHER_MODE];
    const uint32_t fmt = ((dm >> 2) & 15) | ((dm >> 12) & 16);
    const int      rgb = !!(dm & 0x400);
    uint32_t       r   = v & 0xff;
    uint32_t       g   = (v >> 8) & 0xff;
    uint32_t       b   = (v >> 16) & 0xff;
    uint32_t       lo  = rgb ? b : r;
    uint32_t       hi  = rgb ? r : b;
    uint32_t       a   = v >> 24;

    if (!(dm & 1))
        return v;
    if (fmt == 14)
        return v & 0xff; /* Colour index. */

    if (dm & 2) {
        /* 2x2 ordered dither before truncation. */
        static const uint8_t dither[2][2] = { { 0, 2 }, { 3, 1 } };
        const uint32_t       f            = dither[(y + (dm >> 8)) & 1][(x + (dm >> 6)) & 1];

        lo = MIN(255, lo + f * 2);
        g  = MIN(255, g + f);
        hi = MIN(255, hi + f * 2);
    }
    if (dm & 0x1000)
        a = 0;
    if (dm & 0x2000)
        a = 0xf8;

    switch (fmt) {
        case 0:
            return lo | (g << 8) | (hi << 16) | (a << 24);
        case 1:
            return (lo >> 3) | ((g >> 3) << 5) | ((hi >> 3) << 10) | ((a >> 7) << 15);
        case 2:
            return (lo >> 4) | ((g >> 4) << 4) | ((hi >> 4) << 8) | ((a >> 4) << 12);
        case 16:
            return (lo >> 3) | ((g >> 2) << 5) | ((hi >> 3) << 11);
        default:
            return 0;
    }
}

static int
p2_clipped(p2_t *p2, int64_t x, int64_t y, int packed_limits)
{
    const uint32_t sm = p2->gp[TAG_SCISSOR_MODE];

    if (sm & 1) {
        const uint32_t mn = p2->gp[TAG_SCISSOR_MIN];
        const uint32_t mx = p2->gp[TAG_SCISSOR_MAX];

        if ((x < p2_sext(mn, 16)) || (y < p2_sext(mn >> 16, 16)) ||
            (x >= p2_sext(mx, 16)) || (y >= p2_sext(mx >> 16, 16)))
            return 1;
    }
    if (sm & 2) {
        const uint32_t ss = p2->gp[TAG_SCREEN_SIZE];

        if ((x < 0) || (y < 0) || (x >= (int64_t) (ss & 0xffff)) || (y >= (int64_t) (ss >> 16)))
            return 1;
    }
    if (packed_limits && (p2->gp[TAG_FB_READ_MODE] & FBRM_PACKED)) {
        /* Native pixel edges, in either order. */
        const int a = p2_sext(p2->gp[TAG_PACKED_DATA_LIMITS] >> 16, 12);
        const int b = p2_sext(p2->gp[TAG_PACKED_DATA_LIMITS], 12);

        if ((x < MIN(a, b)) || (x >= MAX(a, b)))
            return 1;
    }

    return 0;
}

static uint32_t
p2_block_color(p2_t *p2, int x, int y)
{
    const int      bpp   = p2->raster.bpp;
    const int64_t  sign  = (p2->gp[TAG_FB_WRITE_CONFIG] & FBRM_ORIGIN_BOT) ? -1 : 1;
    const int64_t  pixel = (int64_t) p2->gp[TAG_FB_WINDOW_BASE] + p2->gp[TAG_FB_PIXEL_OFFSET] + x +
                          (int64_t) y * p2_pitch(p2->gp[TAG_FB_WRITE_CONFIG]) * sign;
    const uint64_t pat   = p2->gp[TAG_FB_BLOCK_COLOR_L] | ((uint64_t) p2->gp[TAG_FB_BLOCK_COLOR_U] << 32);

    if (bpp == 3)
        return p2->gp[TAG_FB_BLOCK_COLOR];
    return (uint32_t) (pat >> ((((uint64_t) pixel * bpp) & 7) * 8));
}

/* One fragment through stipple, the write address, logic ops and masks. */
static void
p2_emit_pixel(p2_t *p2, int x, int y, uint32_t value, int raw)
{
    p2_raster_t   *r    = &p2->raster;
    const int      fast = !!(r->command & RENDER_FAST_FILL);
    const uint32_t lop  = p2->gp[TAG_LOGICAL_OP_MODE];
    const uint32_t wc   = p2->gp[TAG_FB_WRITE_CONFIG];
    const int      bpp  = r->bpp;
    int64_t        addr;
    uint32_t       old;

    if (p2_clipped(p2, x, y, !fast))
        return;

    if ((r->command & RENDER_AREA_STIPPLE) && (p2->gp[TAG_AREA_STIPPLE_MODE] & 1)) {
        /* 8x8 pattern. */
        const uint32_t am = p2->gp[TAG_AREA_STIPPLE_MODE];
        uint32_t       ax = (x + (am >> 7)) & 7;
        uint32_t       ay = (y + (am >> 12)) & 7;
        int            bit;

        if (am & (1 << 18))
            ax = 7 - ax;
        if (am & (1 << 19))
            ay = 7 - ay;
        bit = (p2->gp[TAG_AREA_STIPPLE_PAT + ay] >> ax) & 1;
        if (am & (1 << 17))
            bit = !bit;
        if (!bit) {
            if (!(am & (1 << 20)))
                return;
            value = p2->gp[TAG_TEXEL0];
            raw   = 1;
        }
    }

    if (!(p2->gp[TAG_FB_WRITE_MODE] & 1))
        return;

    addr = ((int64_t) p2->gp[TAG_FB_WINDOW_BASE] + p2->gp[TAG_FB_PIXEL_OFFSET] + x +
            (int64_t) y * p2_pitch(wc) * ((wc & FBRM_ORIGIN_BOT) ? -1 : 1)) * bpp;

    if (!fast && (lop & 0x20)) {
        /* UseConstantFBWriteData: packed fragments take their byte lane. */
        value = p2->gp[TAG_FB_WRITE_DATA];
        if (p2->gp[TAG_FB_READ_MODE] & FBRM_PACKED)
            value >>= ((uint32_t) addr & 3) << 3;
    } else if (!raw)
        value = p2_format_color(p2, value, x, y);

    old = p2_fb_read(p2, addr, bpp);
    if (!fast) {
        const uint32_t dst = (p2->gp[TAG_FB_READ_MODE] & FBRM_DST_ENABLE) ? old : 0;
        const uint32_t swm = p2->gp[TAG_FB_SOFT_WRITE_MASK];

        if (lop & 1)
            value = p2_logic_op(lop >> 1, value, dst);
        value = (value & swm) | (dst & ~swm);
    }
    p2_fb_write(p2, addr, bpp, value, p2->gp[TAG_FB_HW_WRITE_MASK]);
}

static int
p2_packed_trap(p2_t *p2, uint32_t command)
{
    return ((command & RENDER_PRIM_MASK) == PRIM_TRAP) && !(command & RENDER_FAST_FILL) &&
           (p2->gp[TAG_FB_READ_MODE] & FBRM_PACKED);
}

/* Reads back to the host (FBReadMode DataType with destination reads). */
static int
p2_upload_cmd(p2_t *p2, uint32_t command)
{
    return !(command & RENDER_FAST_FILL) &&
           ((p2->gp[TAG_FB_READ_MODE] & (FBRM_DATA_TYPE | FBRM_DST_ENABLE)) == (FBRM_DATA_TYPE | FBRM_DST_ENABLE));
}

static void
p2_set_span(p2_t *p2)
{
    p2_raster_t *r = &p2->raster;

    if (r->prim != PRIM_TRAP) {
        r->columns = 1;
        return;
    }
    {
        const int64_t a = p2_fixed_int(r->xdom);
        const int64_t b = p2_fixed_int(r->xsub);
        const int64_t w = (a > b) ? (a - b) : (b - a);

        r->columns = (uint32_t) MIN(w, 65535);
    }
    if (p2_packed_trap(p2, r->command))
        r->columns *= 4 / r->bpp;
}

static void
p2_next_fragment(p2_t *p2)
{
    p2_raster_t *r = &p2->raster;

    if (++r->col < r->columns)
        return;
    r->col = 0;
    if (r->texture_block)
        r->payload_left = 0;
    if (++r->row >= r->rows) {
        r->active = 0;
        return;
    }
    if (r->shade) {
        for (int i = 0; i < 3; i++)
            r->color[i] += r->dcolor_dy[i];
    }
    if (r->prim != PRIM_RECT) {
        r->xdom += r->dxdom;
        r->xsub += r->dxsub;
        r->y += r->dy;
        p2_set_span(p2);
    }
    /* Host data, and bit masks with BitMaskPacking set, restart each span on a new word. */
    if ((r->command & RENDER_SYNC_HOSTDATA) ||
        ((r->command & RENDER_SYNC_BITMASK) && (p2->gp[TAG_RASTERIZER_MODE] & 0x200)))
        r->payload_left = 0;
}

/* A block fill's mask from texture memory: one word per 32 pixels. */
static void
p2_load_texture_mask(p2_t *p2)
{
    p2_raster_t   *r      = &p2->raster;
    const int      tbytes = 1 << ((p2->gp[TAG_TEXTURE_MAP_FORMAT] >> 19) & 3);
    const int64_t  blocks = (r->columns + 31) / 32;
    const int64_t  block  = (int64_t) r->row * blocks + r->col / 32;
    const int64_t  s      = (int64_t) p2_sext(p2->gp[TAG_S_START], 32) / 0x100000 +
                      block * ((int64_t) p2_sext(p2->gp[TAG_DS_DX], 32) / 0x100000);
    const int64_t  width  = (int64_t) 1 << ((p2->gp[TAG_TEXTURE_READ_MODE] >> 9) & 15);
    const int64_t  texel  = ((s % width) + width) % width;
    uint32_t       mask   = p2_fb_read(p2, ((p2->gp[TAG_TEXTURE_BASE] & 0xffffff) + texel) * tbytes, tbytes);

    if (p2->gp[TAG_TEXTURE_DATA_FORMAT] & 0x200) {
        /* SpanFormat mirrors each byte. */
        mask = ((mask & 0x55555555) << 1) | ((mask >> 1) & 0x55555555);
        mask = ((mask & 0x33333333) << 2) | ((mask >> 2) & 0x33333333);
        mask = ((mask & 0x0f0f0f0f) << 4) | ((mask >> 4) & 0x0f0f0f0f);
    }
    r->payload      = mask;
    r->payload_left = MIN(32, r->columns - r->col);
}

/* A texture copy's source texel: 8 bit, nearest, clamped, unit steps. */
static uint32_t
p2_texture_copy_color(p2_t *p2)
{
    p2_raster_t   *r   = &p2->raster;
    const uint32_t trm = p2->gp[TAG_TEXTURE_READ_MODE];
    const int      wb  = (trm >> 9) & 15;
    const int      hb  = (trm >> 13) & 15;
    int64_t        s   = (int64_t) p2_sext(p2->gp[TAG_S_START], 32) / 0x100000 + r->col;
    int64_t        t   = (int64_t) p2_sext(p2->gp[TAG_T_START], 32) / 0x100000 + r->row;

    s = MAX(0, MIN(((int64_t) 1 << wb) - 1, s));
    t = MAX(0, MIN(((int64_t) 1 << hb) - 1, t));
    return p2_fb_read(p2, (int64_t) p2->gp[TAG_TEXTURE_BASE] + t * p2_pitch(p2->gp[TAG_TEXTURE_MAP_FORMAT]) + s, 1);
}

/* One step of the primitive; 0 when it waits for host data. */
static int
p2_draw_step(p2_t *p2)
{
    p2_raster_t   *r    = &p2->raster;
    const uint32_t cmd  = r->command;
    const int      mask = !!(cmd & RENDER_SYNC_BITMASK);
    const int      host = !!(cmd & RENDER_SYNC_HOSTDATA);
    const int      bpp  = r->bpp;
    const uint32_t rm   = p2->gp[TAG_FB_READ_MODE];
    uint32_t       color;
    int            raw;
    int            draw = 1;
    int            x;
    int            y;

    if (!r->columns) {
        p2_next_fragment(p2);
        return 1;
    }
    if (r->texture_block && !r->payload_left)
        p2_load_texture_mask(p2);
    if ((mask || host) && !r->payload_left)
        return 0;

    if (r->prim == PRIM_RECT) {
        x = r->origin_x + ((cmd & RENDER_X_POSITIVE) ? (int) r->col : (int) (r->columns - 1 - r->col));
        y = r->origin_y + ((cmd & RENDER_Y_POSITIVE) ? (int) r->row : (int) (r->rows - 1 - r->row));
    } else {
        y = (int) p2_fixed_int(r->y);
        x = (int) p2_fixed_int(r->xdom);
        if (p2_packed_trap(p2, cmd))
            x *= 4 / bpp;
        if (r->prim == PRIM_TRAP)
            x += (r->xsub >= r->xdom) ? (int) r->col : -(int) r->col - 1;
    }

    if (p2_upload_cmd(p2, cmd)) {
        /* Framebuffer reads to the output FIFO (FilterMode bits 8/9). */
        const int     packed = !!(rm & FBRM_PACKED);
        const int64_t px     = (int64_t) x * (packed ? 4 / bpp : 1);
        const int     filter = (p2->gp[TAG_FILTER_MODE] >> 8) & 3;

        if (!p2_clipped(p2, px, y, 0)) {
            const int64_t pixel = (int64_t) p2->gp[TAG_FB_WINDOW_BASE] + px + p2->gp[TAG_FB_PIXEL_OFFSET] +
                                  (int64_t) y * p2_pitch(rm) * ((rm & FBRM_ORIGIN_BOT) ? -1 : 1);
            const uint32_t v = p2_fb_read(p2, pixel * bpp, packed ? 4 : bpp);

            if (filter & 1)
                p2_out_fifo_push(p2, TAG_FB_COLOR);
            if (filter & 2)
                p2_out_fifo_push(p2, v);
        }
        p2_next_fragment(p2);
        return 1;
    }

    if ((cmd & RENDER_TEXTURE) && !(cmd & RENDER_FAST_FILL) && p2_clipped(p2, x, y, 0)) {
        p2_next_fragment(p2);
        return 1;
    }

    if (r->shade) {
        /* Colour registers hold signed 9.11 values; X steps run along the span. */
        color = r->alpha << 24;
        for (int i = 0; i < 3; i++) {
            int64_t c = r->color[i] + ((r->prim == PRIM_TRAP) ? (int64_t) r->col * r->dcolor_dx[i] : 0);

            c = MAX(0, MIN(255, c / 2048));
            color |= (uint32_t) c << (i * 8);
        }
        raw = 0;
    } else if (p2->gp[TAG_COLOR_DDA_MODE] & 1) {
        color = p2->gp[TAG_CONSTANT_COLOR];
        raw   = 0;
    } else {
        color = p2->gp[TAG_COLOR];
        raw   = 1;
    }
    if (cmd & RENDER_FAST_FILL) {
        color = p2_block_color(p2, x, y);
        raw   = 1;
    }

    if (r->texture_block) {
        draw = r->payload & 1;
        r->payload >>= 1;
        r->payload_left--;
    } else if ((cmd & RENDER_TEXTURE) && !(cmd & RENDER_FAST_FILL)) {
        color = p2_texture_copy_color(p2);
        raw   = 1;
    } else if (mask) {
        const uint32_t rmode  = p2->gp[TAG_RASTERIZER_MODE];
        const int      mirror = rmode & 1;
        int            bit    = mirror ? !!(r->payload & 0x80000000) : (r->payload & 1);

        if (rmode & 2)
            bit = !bit;
        if (mirror)
            r->payload <<= 1;
        else
            r->payload >>= 1;
        r->payload_left--;
        if (!bit) {
            if (rmode & 0x40) {
                color = p2->gp[TAG_TEXEL0]; /* ForceBackgroundColor. */
                raw   = 1;
            } else
                draw = 0;
        }
    } else if (host) {
        color = r->payload;
        raw   = (r->payload_tag != TAG_COLOR);
        if (bpp < 4) {
            if (raw || !(p2->gp[TAG_DITHER_MODE] & 1) || (rm & FBRM_PACKED))
                color &= (1u << (bpp * 8)) - 1;
            r->payload >>= bpp * 8;
        }
        r->payload_left--;
        if ((rm & FBRM_PACKED) && ((r->prim == PRIM_RECT) || p2_packed_trap(p2, cmd)))
            x += r->rel_offset;
    } else if (!(cmd & RENDER_FAST_FILL) && (rm & FBRM_SRC_ENABLE)) {
        const int64_t src = (int64_t) p2->gp[TAG_FB_SOURCE_BASE] + x + p2->gp[TAG_FB_PIXEL_OFFSET] +
                            (int64_t) y * p2_pitch(rm) * ((rm & FBRM_ORIGIN_BOT) ? -1 : 1) +
                            p2_sext(p2->gp[TAG_FB_SOURCE_OFFSET], 24) - ((rm & FBRM_PACKED) ? r->rel_offset : 0);

        color = p2_fb_read(p2, src * bpp, bpp);
        raw   = 1;
    }

    if (draw)
        p2_emit_pixel(p2, x, y, color, raw);
    p2_next_fragment(p2);
    return 1;
}

static void
p2_raster_run(p2_t *p2)
{
    while (p2->raster.active && p2_draw_step(p2))
        ;
}

static void
p2_raster_start(p2_t *p2, uint32_t command)
{
    p2_raster_t   *r    = &p2->raster;
    const uint32_t rm   = p2->gp[TAG_FB_READ_MODE];
    const int      bias = (p2->gp[TAG_RASTERIZER_MODE] >> 4) & 3;

    memset(r, 0, sizeof(p2_raster_t) - sizeof(r->rel_offset));
    r->command  = command;
    r->prim     = command & RENDER_PRIM_MASK;
    r->bpp      = p2_fb_bpp(p2);
    r->origin_x = p2_sext(p2->gp[TAG_RECT_ORIGIN], 16);
    r->origin_y = p2_sext(p2->gp[TAG_RECT_ORIGIN] >> 16, 16);
    r->xdom     = p2_sext(p2->gp[TAG_START_X_DOM], 32);
    r->xsub     = p2_sext(p2->gp[TAG_START_X_SUB], 32);
    r->y        = p2_sext(p2->gp[TAG_START_Y], 32);
    r->dxdom    = p2_sext(p2->gp[TAG_DX_DOM], 32);
    r->dxsub    = p2_sext(p2->gp[TAG_DX_SUB], 32);
    r->dy       = p2_sext(p2->gp[TAG_DY], 32);

    r->shade = !(command & RENDER_FAST_FILL) && ((p2->gp[TAG_COLOR_DDA_MODE] & 3) == 3);
    if (r->shade) {
        for (int i = 0; i < 3; i++) {
            r->color[i]     = p2_sext(p2->gp[TAG_R_START + i * 3] >> 4, 20);
            r->dcolor_dx[i] = p2_sext(p2->gp[TAG_R_START + i * 3 + 1] >> 4, 20);
            r->dcolor_dy[i] = p2_sext(p2->gp[TAG_R_START + i * 3 + 2] >> 4, 20);
        }
        r->alpha = MAX(0, MIN(255, p2_sext(p2->gp[TAG_A_START] >> 4, 20) / 2048));
    }

    r->texture_block = ((command & (RENDER_FAST_FILL | RENDER_TEXTURE)) == (RENDER_FAST_FILL | RENDER_TEXTURE)) &&
                       (p2->gp[TAG_TEXTURE_READ_MODE] & 1);

    if (r->prim == PRIM_RECT) {
        r->columns = p2->gp[TAG_RECT_SIZE] & 0xffff;
        r->rows    = p2->gp[TAG_RECT_SIZE] >> 16;
        /* Packed rectangles count DWORD groups; the framebuffer unit widens them. */
        if ((rm & FBRM_PACKED) && !(command & RENDER_FAST_FILL) && !p2_upload_cmd(p2, command) && (r->bpp != 3)) {
            r->origin_x *= 4 / r->bpp;
            r->columns *= 4 / r->bpp;
        }
    } else {
        const int64_t b = (bias == 1) ? 0x8000 : ((bias == 2) ? 0x7fff : 0);

        r->rows = (r->prim == PRIM_POINT) ? 1 : (p2->gp[TAG_COUNT] & 0xffff);
        r->xdom += b;
        r->xsub += b;
        r->y += b;
        p2_set_span(p2);
    }

    p2_log("P2: render %08X rect %08X %08X tr %08X %08X y %08X d %08X %08X %08X n %i rm %08X wc %08X lop %08X dda %08X cc %08X col %08X blk %08X dm %08X wb %08X sb %08X so %08X po %08X rp %X lim %08X rast %08X sc %X %08X %08X wd %08X t0 %08X wm %08X fs %08X\n",
           command, p2->gp[TAG_RECT_ORIGIN], p2->gp[TAG_RECT_SIZE], p2->gp[TAG_START_X_DOM], p2->gp[TAG_START_X_SUB],
           p2->gp[TAG_START_Y], p2->gp[TAG_DX_DOM], p2->gp[TAG_DX_SUB], p2->gp[TAG_DY], p2->gp[TAG_COUNT], rm,
           p2->gp[TAG_FB_WRITE_CONFIG], p2->gp[TAG_LOGICAL_OP_MODE], p2->gp[TAG_COLOR_DDA_MODE], p2->gp[TAG_CONSTANT_COLOR],
           p2->gp[TAG_COLOR], p2->gp[TAG_FB_BLOCK_COLOR], p2->gp[TAG_DITHER_MODE], p2->gp[TAG_FB_WINDOW_BASE],
           p2->gp[TAG_FB_SOURCE_BASE], p2->gp[TAG_FB_SOURCE_OFFSET], p2->gp[TAG_FB_PIXEL_OFFSET], p2->gp[TAG_FB_READ_PIXEL],
           p2->gp[TAG_PACKED_DATA_LIMITS], p2->gp[TAG_RASTERIZER_MODE], p2->gp[TAG_SCISSOR_MODE], p2->gp[TAG_SCISSOR_MIN],
           p2->gp[TAG_SCISSOR_MAX], p2->gp[TAG_FB_WRITE_DATA], p2->gp[TAG_TEXEL0], p2->gp[TAG_FB_WRITE_MODE],
           p2->gp[TAG_FB_SOURCE_DELTA]);

    r->active = r->rows && (r->columns || (r->prim == PRIM_TRAP));
    p2_raster_run(p2);
}

static void
p2_raster_continue(p2_t *p2, uint32_t tag, uint32_t count)
{
    p2_raster_t *r = &p2->raster;

    /* The finished primitive's DDAs take one more step, then reload as asked. */
    if (r->prim != PRIM_RECT) {
        r->xdom += r->dxdom;
        r->xsub += r->dxsub;
        r->y += r->dy;
    }
    switch (tag) {
        case TAG_CONTINUE_NEW_DOM:
            r->xdom  = p2_sext(p2->gp[TAG_START_X_DOM], 32);
            r->dxdom = p2_sext(p2->gp[TAG_DX_DOM], 32);
            break;
        case TAG_CONTINUE_NEW_SUB:
            r->xsub  = p2_sext(p2->gp[TAG_START_X_SUB], 32);
            r->dxsub = p2_sext(p2->gp[TAG_DX_SUB], 32);
            break;
        case TAG_CONTINUE_NEW_LINE:
            r->dxdom = p2_sext(p2->gp[TAG_DX_DOM], 32);
            r->dy    = p2_sext(p2->gp[TAG_DY], 32);
            break;
        default:
            break;
    }
    r->rows         = count & 0xffff;
    r->row          = 0;
    r->col          = 0;
    r->payload_left = 0;
    p2_set_span(p2);
    r->active = r->rows && (r->columns || (r->prim == PRIM_TRAP));
    p2_raster_run(p2);
}

/* Register side effects and host data. Returns 1 if the write was data for a
   waiting primitive. */
static void
p2_gp_command(p2_t *p2, uint32_t tag, uint32_t val)
{
    p2_raster_t *r = &p2->raster;

    /* A primitive waiting for data is abandoned by anything else. */
    if (r->active && (r->command & (RENDER_SYNC_BITMASK | RENDER_SYNC_HOSTDATA))) {
        const int is_data = (r->command & RENDER_SYNC_BITMASK) ? (tag == TAG_BIT_MASK_PATTERN)
                                                                : ((tag == TAG_COLOR) || (tag == TAG_FB_DATA) || (tag == TAG_FB_SOURCE_DATA));
        if (is_data) {
            if (r->command & RENDER_SYNC_BITMASK) {
                const uint32_t rmode = p2->gp[TAG_RASTERIZER_MODE];
                const int      swap  = (rmode >> 7) & 3;
                const int      skip  = (rmode >> 10) & 31;

                if (swap & 1)
                    val = ((val & 0x00ff00ff) << 8) | ((val >> 8) & 0x00ff00ff);
                if (swap & 2)
                    val = (val << 16) | (val >> 16);
                if (skip)
                    val = (rmode & 1) ? ((val << skip) | (val >> (32 - skip))) : ((val >> skip) | (val << (32 - skip)));
                r->payload_left = 32;
            } else {
                const int swap = (p2->gp[TAG_RASTERIZER_MODE] >> 15) & 3;

                if (swap & 1)
                    val = ((val & 0x00ff00ff) << 8) | ((val >> 8) & 0x00ff00ff);
                if (swap & 2)
                    val = (val << 16) | (val >> 16);
                r->payload_left = (p2->gp[TAG_FB_READ_MODE] & FBRM_PACKED) ? (4 / r->bpp) : 1;
            }
            r->payload     = val;
            r->payload_tag = tag;
            p2_raster_run(p2);
            return;
        }
        r->active = 0;
    }

    switch (tag) {
        case TAG_RENDER:
            p2_raster_start(p2, val);
            break;

        case TAG_CONTINUE_NEW_LINE:
        case TAG_CONTINUE_NEW_DOM:
        case TAG_CONTINUE_NEW_SUB:
        case TAG_CONTINUE:
            p2_raster_continue(p2, tag, val);
            break;

        case TAG_FB_READ_MODE:
            p2->gp[TAG_FB_WRITE_CONFIG] = val;
            r->rel_offset               = p2_sext(val >> 20, 3);
            break;
        case TAG_PACKED_DATA_LIMITS:
            r->rel_offset                = p2_sext(val >> 29, 3);
            p2->gp[TAG_FB_READ_MODE]     = (p2->gp[TAG_FB_READ_MODE] & ~0x700000) | ((uint32_t) (r->rel_offset & 7) << 20);
            break;
        case TAG_FB_WINDOW_BASE:
            p2->gp[TAG_FB_SOURCE_BASE] = val;
            break;
        case TAG_FB_SOURCE_DELTA: {
            const uint32_t rm    = p2->gp[TAG_FB_READ_MODE];
            const int64_t  delta = (int64_t) p2_sext(val >> 16, 12) * p2_pitch(rm) * ((rm & FBRM_ORIGIN_BOT) ? -1 : 1) +
                                  p2_sext(val, 12);

            p2->gp[TAG_FB_SOURCE_OFFSET] = (uint32_t) delta & 0xffffff;
            break;
        }
        case TAG_CONFIG:
            p2->gp[TAG_FB_READ_MODE] = (p2->gp[TAG_FB_READ_MODE] & ~(FBRM_SRC_ENABLE | FBRM_DST_ENABLE | FBRM_PACKED)) |
                                       ((val & 1) ? FBRM_SRC_ENABLE : 0) | ((val & 2) ? FBRM_DST_ENABLE : 0) |
                                       ((val & 4) ? FBRM_PACKED : 0);
            p2->gp[TAG_FB_WRITE_MODE]    = (p2->gp[TAG_FB_WRITE_MODE] & ~1) | ((val >> 3) & 1);
            p2->gp[TAG_COLOR_DDA_MODE]   = (p2->gp[TAG_COLOR_DDA_MODE] & ~1) | ((val >> 4) & 1);
            p2->gp[TAG_LOGICAL_OP_MODE]  = (p2->gp[TAG_LOGICAL_OP_MODE] & ~31) | ((val >> 5) & 31);
            break;
        case TAG_FB_BLOCK_COLOR:
            p2->gp[TAG_FB_BLOCK_COLOR_L] = p2->gp[TAG_FB_BLOCK_COLOR_U] = val;
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------------------- */
/* PCI                                                                       */
/* ------------------------------------------------------------------------- */

static void
p2_update_mappings(p2_t *p2)
{
    svga_t  *svga = &p2->svga;
    uint32_t base;

    io_removehandler(0x03c0, 0x0020, p2_in, NULL, NULL, p2_out, NULL, NULL, p2);
    if (p2->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_IO)
        io_sethandler(0x03c0, 0x0020, p2_in, NULL, NULL, p2_out, NULL, NULL, p2);

    if (!(p2->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_MEM)) {
        mem_mapping_disable(&svga->mapping);
        mem_mapping_disable(&p2->ctrl_mapping);
        mem_mapping_disable(&p2->ap_mapping[0]);
        mem_mapping_disable(&p2->ap_mapping[1]);
        return;
    }

    switch (svga->gdcreg[6] & 0x0c) {
        case 0x0:
            mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x20000);
            break;
        case 0x4:
            mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x10000);
            break;
        case 0x8:
            mem_mapping_set_addr(&svga->mapping, 0xb0000, 0x08000);
            break;
        default:
            mem_mapping_set_addr(&svga->mapping, 0xb8000, 0x08000);
            break;
    }

    base = (p2->pci_regs[0x11] << 8 | p2->pci_regs[0x12] << 16 | p2->pci_regs[0x13] << 24) & 0xfffe0000;
    if (base)
        mem_mapping_set_addr(&p2->ctrl_mapping, base, P2_CTRL_SIZE);
    else
        mem_mapping_disable(&p2->ctrl_mapping);

    for (int i = 0; i < 2; i++) {
        base = (p2->pci_regs[0x16 + i * 4] << 16 | p2->pci_regs[0x17 + i * 4] << 24) & 0xff800000;
        if (base)
            mem_mapping_set_addr(&p2->ap_mapping[i], base, P2_APERTURE_SIZE);
        else
            mem_mapping_disable(&p2->ap_mapping[i]);
    }
}

static uint8_t
p2_pci_read(UNUSED(int func), int addr, UNUSED(int len), void *priv)
{
    p2_t   *p2  = (p2_t *) priv;
    uint8_t ret = 0x00;

    if (func)
        return 0xff;

    switch (addr) {
        case 0xf4 ... 0xf7:
            ret = p2->cfg_indirect_f4 >> ((addr & 3) << 3);
            break;
        case 0xf8 ... 0xfb:
            ret = p2->cfg_indirect_addr >> ((addr & 3) << 3);
            break;
        case 0xfc ... 0xff:
            ret = p2_ctrl_read(p2, p2->cfg_indirect_addr & 0xfffc) >> ((addr & 3) << 3);
            break;
        default:
            ret = p2->pci_regs[addr];
            break;
    }

    if (p2_do_log >= 2)
        pclog("P2: cfg R %02X = %02X\n", addr, ret);
    return ret;
}

static void
p2_pci_write(UNUSED(int func), int addr, UNUSED(int len), uint8_t val, void *priv)
{
    p2_t   *p2   = (p2_t *) priv;
    svga_t *svga = &p2->svga;
    int     shift;

    if (func)
        return;
    if (p2_do_log >= 2)
        pclog("P2: cfg W %02X = %02X\n", addr, val);

    switch (addr) {
        case PCI_REG_COMMAND:
            p2->pci_regs[addr] = val & 0x27;
            p2_update_mappings(p2);
            break;
        case 0x05:
            p2->pci_regs[addr] = val & 0x01;
            break;
        case 0x07:
            p2->pci_regs[addr] &= ~(val & 0xf8);
            break;
        case 0x0c:
        case 0x0d:
            p2->pci_regs[addr] = val;
            break;

        case 0x11:
            p2->pci_regs[addr] = val & 0x00;
            break;
        case 0x12:
            p2->pci_regs[addr] = val & 0xfe;
            p2_update_mappings(p2);
            break;
        case 0x13:
            p2->pci_regs[addr] = val;
            p2_update_mappings(p2);
            break;
        case 0x16:
        case 0x1a:
            p2->pci_regs[addr] = val & 0x80;
            p2_update_mappings(p2);
            break;
        case 0x17:
        case 0x1b:
            p2->pci_regs[addr] = val;
            p2_update_mappings(p2);
            break;

        case 0x2c ... 0x2f:
            /* The video BIOS writes the subsystem IDs at POST. */
            p2->pci_regs[addr] = val;
            break;

        case 0x30:
        case 0x32:
        case 0x33:
            p2->pci_regs[addr] = (addr == 0x30) ? (val & 0x01) : val;
            if (p2->pci_regs[0x30] & 0x01) {
                uint32_t biosaddr = (p2->pci_regs[0x32] << 16) | (p2->pci_regs[0x33] << 24);
                mem_mapping_set_addr(&p2->bios_rom.mapping, biosaddr, 0x10000);
            } else
                mem_mapping_disable(&p2->bios_rom.mapping);
            break;

        case 0x3c:
            p2->pci_regs[addr] = val;
            break;

        case 0x58 ... 0x5b:
            if (p2->is_agp)
                p2->pci_regs[addr] = val;
            break;

        case 0xf4 ... 0xf7:
            shift                = (addr & 3) << 3;
            p2->cfg_indirect_f4 = (p2->cfg_indirect_f4 & ~(0xffu << shift)) | (val << shift);
            break;
        case 0xf8 ... 0xfb:
            shift                  = (addr & 3) << 3;
            p2->cfg_indirect_addr = (p2->cfg_indirect_addr & ~(0xffu << shift)) | (val << shift);
            break;
        case 0xfc ... 0xff:
            /* The BIOS only writes whole dwords here; act on the last byte. */
            shift                = (addr & 3) << 3;
            p2->cfg_indirect_f4 = (p2->cfg_indirect_f4 & ~(0xffu << shift)) | (val << shift);
            if ((addr & 3) == 3)
                p2_ctrl_write(p2, p2->cfg_indirect_addr & 0xfffc, p2->cfg_indirect_f4);
            break;

        default:
            break;
    }

    (void) svga;
}

/* ------------------------------------------------------------------------- */
/* Debug hook ("dev p2 ..." in the BOX86NEXT_DEBUG_CMD file)                 */
/* ------------------------------------------------------------------------- */

static p2_t *p2_dbg;

static void
p2_debug_hook(const char *args)
{
    p2_t   *p2   = p2_dbg;
    svga_t *svga;
    char    line[256];
    int     n;

    if (!p2)
        return;
    svga = &p2->svga;

    if (!strncmp(args, "p2", 2))
        args += 2;
    while (*args == ' ')
        args++;

    if (!strncmp(args, "pal", 3)) {
        for (int i = 0; i < 256; i += 8) {
            n = 0;
            for (int j = i; j < i + 8; j++)
                n += sprintf(line + n, "%02X%02X%02X ", svga->vgapal[j].r, svga->vgapal[j].g, svga->vgapal[j].b);
            pclog("P2: pal %02X: %s\n", i, line);
        }
        pclog("P2: pallook 4 %08X 9 %08X F7 %08X type %i vram(100,450) %02X (300,200) %02X map8 %i\n",
              svga->pallook[4], svga->pallook[9], svga->pallook[0xf7], svga->ramdac_type,
              svga->vram[450 * 640 + 100], svga->vram[200 * 640 + 300], svga->map8 == svga->pallook);
        return;
    }

    if (!strncmp(args, "log ", 4)) {
        p2_do_log = atoi(args + 4);
        pclog("P2: log %i\n", p2_do_log);
        return;
    }

    pclog("P2: misc %02X SR05 %02X GR09 %02X seq %02X %02X %02X %02X %02X gdc5/6 %02X %02X ar10 %02X cmd %02X\n",
          svga->miscout, svga->seqregs[5], svga->gdcreg[9], svga->seqregs[0], svga->seqregs[1], svga->seqregs[2],
          svga->seqregs[3], svga->seqregs[4], svga->gdcreg[5], svga->gdcreg[6], svga->attrregs[0x10], p2->pci_regs[4]);
    n = 0;
    for (int i = 0; i < 0x19; i++)
        n += sprintf(line + n, "%02X ", svga->crtc[i]);
    pclog("P2: crtc %s\n", line);
    pclog("P2: hdisp %i htotal %i dispend %i vtotal %i rowoff %i ma %06X bpp %i clock %f render %p blank %p dacmask %02X scrblank %i palen %i\n",
          svga->hdisp, svga->htotal, svga->dispend, svga->vtotal, svga->rowoffset, svga->memaddr_latch, svga->bpp, svga->clock,
          (void *) svga->render, (void *) svga_render_blank, svga->dac_mask, svga->scrblank, svga->attr_palette_enable);
    n = 0;
    for (int i = 0; i < VTG_NUM; i++)
        n += sprintf(line + n, "%X ", p2->vtg[i]);
    pclog("P2: vtg %s vclk %X\n", line, p2->vclk_ctl);
    n = 0;
    for (int i = 0; i < 0x50; i++)
        n += sprintf(line + n, "%02X", p2->dac_regs[i]);
    pclog("P2: dac %s\n", line);
    pclog("P2: pal0 %02X %02X %02X pal7 %02X %02X %02X\n", svga->vgapal[0].r, svga->vgapal[0].g, svga->vgapal[0].b,
          svga->vgapal[7].r, svga->vgapal[7].g, svga->vgapal[7].b);
    pclog("P2: x_add %i left_overscan %i y_add %i mon_overscan %i,%i hblankstart %i hblank_sub %i dpc %i ovc %08X\n",
          svga->x_add, svga->left_overscan, svga->y_add, svga->monitor->mon_overscan_x, svga->monitor->mon_overscan_y,
          svga->hblankstart, svga->hblank_sub, svga->dots_per_clock, svga->overscan_color);
}

/* ------------------------------------------------------------------------- */
/* Device                                                                    */
/* ------------------------------------------------------------------------- */

static void
p2_reset_state(p2_t *p2)
{
    svga_t *svga = &p2->svga;

    p2->int_enable    = 0;
    p2->int_flags     = 0;
    p2->error_flags   = 0;
    p2->vclk_ctl      = 0;
    p2->aperture[0]   = 0;
    p2->aperture[1]   = 0;
    p2->dma_count     = 0;
    p2->bypass_mask   = 0xffffffff;
    p2->fb_write_mask = 0xffffffff;
    p2->chip_config   = 0x00000006 | (p2->is_agp ? 0x00000200 : 0);
    p2->mem_config    = 0xe6002021;
    memset(p2->vtg, 0, sizeof(p2->vtg));
    memset(p2->gp, 0, sizeof(p2->gp));
    memset(&p2->raster, 0, sizeof(p2->raster));
    p2->gp[0x104] = 0xffffffff; /* FBSoftwareWriteMask */
    p2->gp[0x158] = 0xffffffff; /* FBHardwareWriteMask */

    memset(p2->dac_regs, 0, sizeof(p2->dac_regs));
    /* Clock synthesisers A and B start at the VGA's 25 and 28 MHz. */
    p2->dac_regs[TVP_PIXCLK_A1]     = 0x1c;
    p2->dac_regs[TVP_PIXCLK_A1 + 1] = 0x02;
    p2->dac_regs[TVP_PIXCLK_A1 + 2] = 0x0b;
    p2->dac_regs[TVP_PIXCLK_A1 + 3] = 0xae;
    p2->dac_regs[TVP_PIXCLK_A1 + 4] = 0x0b;
    p2->dac_regs[TVP_PIXCLK_A1 + 5] = 0x0b;
    p2->dac_regs[TVP_PIXCLK_A1 + 6] = 0x1c;
    p2->dac_regs[TVP_PIXCLK_A1 + 7] = 0x02;
    p2->dac_regs[TVP_PIXCLK_A1 + 8] = 0x0b;
    p2->cursor_x = p2->cursor_y = 0;
    p2_update_cursor(p2);

    p2->cfg_indirect_addr = 0;
    p2->cfg_indirect_f4   = 0;

    p2_gp_reset(p2);

    svga->seqregs[5] = 0x0b;
    svga->gdcreg[9]  = 0x00;
    p2_update_banks(p2);
    svga_set_ramdac_type(svga, RAMDAC_6BIT);
}

static void
p2_init_pci_regs(p2_t *p2)
{
    memset(p2->pci_regs, 0, sizeof(p2->pci_regs));

    if (p2->tdlabs_id) {
        p2->pci_regs[0x00] = 0x3d; /* 3Dlabs */
        p2->pci_regs[0x01] = 0x3d;
        p2->pci_regs[0x02] = 0x07; /* Permedia 2 */
        p2->pci_regs[0x03] = 0x00;
    } else {
        p2->pci_regs[0x00] = 0x4c; /* Texas Instruments */
        p2->pci_regs[0x01] = 0x10;
        p2->pci_regs[0x02] = 0x07; /* TVP4020 (Permedia 2) */
        p2->pci_regs[0x03] = 0x3d;
    }
    p2->pci_regs[PCI_REG_COMMAND] = 0x00;
    p2->pci_regs[0x06] = p2->is_agp ? 0x10 : 0x00;
    p2->pci_regs[0x07] = 0x02;
    p2->pci_regs[0x08] = 0x01; /* Revision */
    p2->pci_regs[0x0b] = 0x03; /* VGA compatible display controller */
    p2->pci_regs[0x16] = 0x00;
    p2->pci_regs[0x14] = 0x08; /* Prefetchable */
    p2->pci_regs[0x18] = 0x08;
    /* STB Systems, GLyder MAX-2+ (as the BIOS sets them). */
    p2->pci_regs[0x2c] = 0xb4;
    p2->pci_regs[0x2d] = 0x10;
    p2->pci_regs[0x2e] = 0x2d;
    p2->pci_regs[0x2f] = 0x25;
    p2->pci_regs[0x30] = 0x00;
    p2->pci_regs[0x32] = 0x0c;
    p2->pci_regs[0x3d] = PCI_INTA;
    p2->pci_regs[0x3e] = 0x03;
    p2->pci_regs[0x3f] = 0x03;

    if (p2->is_agp) {
        p2->pci_regs[0x34] = 0x4c;
        p2->pci_regs[0x4c] = 0x02; /* AGP 1.0, 1x/2x, no sideband */
        p2->pci_regs[0x4d] = 0x00;
        p2->pci_regs[0x4e] = 0x10;
        p2->pci_regs[0x4f] = 0x00;
        p2->pci_regs[0x50] = 0x03;
        p2->pci_regs[0x53] = 0x07;
    }
}

/* The same chip was sold by 3Dlabs as 3D3D:0007. The BIOS takes either, but
   its PCI data structure must match the device for the system BIOS to run
   it: rewrite the IDs there and keep the 32 KB image's checksum (the file
   holds the image twice; the byte before the trailing subsystem IDs is
   padding). */
static void
p2_patch_bios_id(p2_t *p2)
{
    uint8_t *rom = p2->bios_rom.rom;

    if (!rom || (rom[0] != 0x55) || (rom[1] != 0xaa))
        return;

    for (uint32_t half = 0; half < 0x10000; half += 0x8000) {
        uint8_t *img  = rom + half;
        uint16_t pcir = img[0x18] | (img[0x19] << 8);
        int      diff;

        if ((pcir > 0x7ff0) || memcmp(&img[pcir], "PCIR", 4))
            continue;
        diff = img[pcir + 4] + img[pcir + 5] + img[pcir + 6] + img[pcir + 7];
        img[pcir + 4] = 0x3d;
        img[pcir + 5] = 0x3d;
        img[pcir + 6] = 0x07;
        img[pcir + 7] = 0x00;
        diff -= img[pcir + 4] + img[pcir + 5] + img[pcir + 6] + img[pcir + 7];
        img[0x7ff0] += diff;
    }
}

static void *
p2_init(const device_t *info)
{
    p2_t   *p2   = calloc(1, sizeof(p2_t));
    svga_t *svga = &p2->svga;

    p2->is_agp    = !!(info->flags & DEVICE_AGP);
    p2->vram_size = device_get_config_int("memory") << 20;
    p2->vram_mask = p2->vram_size - 1;

    rom_init(&p2->bios_rom, BIOS_STB_GLYDER_MAX2, 0xc0000, 0x10000, 0xffff, 0, MEM_MAPPING_EXTERNAL);
    p2->tdlabs_id = device_get_config_int("chip_id");
    if (p2->tdlabs_id)
        p2_patch_bios_id(p2);
    mem_mapping_disable(&p2->bios_rom.mapping);

    video_inform(VIDEO_FLAG_TYPE_SPECIAL, p2->is_agp ? &timing_p2_agp : &timing_p2_pci);

    svga_init(info, svga, p2, p2->vram_size,
              p2_recalctimings,
              p2_in, p2_out,
              NULL,
              NULL);
    svga->dac_hwcursor_draw = p2_hwcursor_draw;
    svga->vsync_callback    = p2_vsync_callback;
    svga->force_old_addr    = 1;
    svga->decode_mask       = p2->vram_mask;
    svga->vram_display_mask = p2->vram_mask;

    mem_mapping_add(&p2->ctrl_mapping, 0, 0,
                    p2_mmio_readb, p2_mmio_readw, p2_mmio_readl,
                    p2_mmio_writeb, p2_mmio_writew, p2_mmio_writel,
                    NULL, MEM_MAPPING_EXTERNAL, p2);
    for (int i = 0; i < 2; i++)
        mem_mapping_add(&p2->ap_mapping[i], 0, 0,
                        p2_lfb_readb, p2_lfb_readw, p2_lfb_readl,
                        p2_lfb_writeb, p2_lfb_writew, p2_lfb_writel,
                        NULL, MEM_MAPPING_EXTERNAL, p2);
    mem_mapping_disable(&p2->ctrl_mapping);
    mem_mapping_disable(&p2->ap_mapping[0]);
    mem_mapping_disable(&p2->ap_mapping[1]);

    p2_init_pci_regs(p2);
    p2_reset_state(p2);

    io_sethandler(0x03c0, 0x0020, p2_in, NULL, NULL, p2_out, NULL, NULL, p2);
    p2->pci_regs[PCI_REG_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEM;

    pci_add_card(p2->is_agp ? PCI_ADD_AGP : PCI_ADD_NORMAL, p2_pci_read, p2_pci_write, p2, &p2->pci_slot);

    p2_dbg = p2;
    debug_cmd_set_device_hook(p2_debug_hook);

    return p2;
}

static void
p2_reset(void *priv)
{
    p2_t *p2 = (p2_t *) priv;

    p2_init_pci_regs(p2);
    p2_reset_state(p2);
    p2->pci_regs[PCI_REG_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEM;
    p2_update_mappings(p2);
    mem_mapping_disable(&p2->bios_rom.mapping);
    p2->irq_state = 0;
    svga_recalctimings(&p2->svga);
}

static int
p2_available(void)
{
    return rom_present(BIOS_STB_GLYDER_MAX2);
}

static void
p2_close(void *priv)
{
    p2_t *p2 = (p2_t *) priv;

    if (p2_dbg == p2) {
        debug_cmd_set_device_hook(NULL);
        p2_dbg = NULL;
    }
    svga_close(&p2->svga);
    free(p2);
}

static void
p2_speed_changed(void *priv)
{
    p2_t *p2 = (p2_t *) priv;

    svga_recalctimings(&p2->svga);
}

static void
p2_force_redraw(void *priv)
{
    p2_t *p2 = (p2_t *) priv;

    p2->svga.fullchange = p2->svga.monitor->mon_changeframecount;
}

static const device_config_t p2_config[] = {
    // clang-format off
    {
        .name           = "memory",
        .description    = "Memory size",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 8,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "4 MB", .value = 4 },
            { .description = "8 MB", .value = 8 },
            { .description = ""                 }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "chip_id",
        .description    = "PCI ID",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "Texas Instruments (104C:3D07)", .value = 0 },
            { .description = "3Dlabs (3D3D:0007)",            .value = 1 },
            { .description = ""                                          }
        },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
    // clang-format on
};

const device_t permedia2_stb_glyder_max2_pci_device = {
    .name          = "STB GLyder MAX-2+ (3Dlabs Permedia 2) PCI",
    .internal_name = "glyder_max2_pci",
    .flags         = DEVICE_PCI,
    .local         = 0,
    .init          = p2_init,
    .close         = p2_close,
    .reset         = p2_reset,
    .available     = p2_available,
    .speed_changed = p2_speed_changed,
    .force_redraw  = p2_force_redraw,
    .config        = p2_config
};

const device_t permedia2_stb_glyder_max2_agp_device = {
    .name          = "STB GLyder MAX-2+ (3Dlabs Permedia 2) AGP",
    .internal_name = "glyder_max2_agp",
    .flags         = DEVICE_AGP,
    .local         = 0,
    .init          = p2_init,
    .close         = p2_close,
    .reset         = p2_reset,
    .available     = p2_available,
    .speed_changed = p2_speed_changed,
    .force_redraw  = p2_force_redraw,
    .config        = p2_config
};
