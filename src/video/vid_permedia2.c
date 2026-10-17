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
#include <stddef.h>
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
#include <86box/i2c.h>
#include <86box/vid_ddc.h>
#include <86box/plat_unused.h>
#include <86box/debug_cmd.h>

#define P2_ROM_DIR "roms/video/3dlabs/permedia2/"

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
#define P2_OUT_DMA_ADDR   0x0080
#define P2_OUT_DMA_COUNT  0x0088
#define P2_AGP_TEX_BASE   0x0090 /* P2A: host texture base (TextureBaseAddress bit 30). */
#define P2_BYDMA_ADDRESS  0x00a0 /* P2A bypass DMA controller (HRM 3.2.19-3.2.25). */
#define P2_BYDMA_STRIDE   0x00b8
#define P2_BYDMA_MEMADDR  0x00c0
#define P2_BYDMA_SIZE     0x00c8
#define P2_BYDMA_BYTEMASK 0x00d0
#define P2_BYDMA_CONTROL  0x00d8
#define P2_BYDMA_COMPLETE 0x00e8

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

/* Serial buses: DisplayData (VTG, the monitor's DDC) and VSSerialBusControl
   (the video streams' bus to a TV encoder or decoder), XFree86's glint bits.
   The inputs read the wired-AND of the outputs and the devices. */
#define P2_VS_SERIAL_BUS 0x5810
#define P2_I2C_DATA_IN   0x01
#define P2_I2C_CLK_IN    0x02
#define P2_I2C_DATA_OUT  0x04
#define P2_I2C_CLK_OUT   0x08

#define VC_ENABLE      0x00000001
#define VC_LINE_DOUBLE 0x00000004
#define VC_DATA64      0x00010000

#define INT_DMA      0x01
#define INT_SYNC     0x02
#define INT_ERROR    0x08
#define INT_VRETRACE 0x10
#define INT_SCANLINE 0x20
#define INT_TEX_INVALID 0x40
#define INT_BYPASS_DMA  0x80

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

/* Host-readable output FIFO. The real one is 8 words and stalls the
   rasterizer; here a primitive runs to the end at once, so a whole upload
   (and the Sync after it) must fit. */
#define P2_OUT_FIFO 16384
/* An upload stalls once this many words wait to be read. */
#define P2_OUT_STALL 4096
/* Register writes held back while a primitive is stalled. */
#define P2_HOLD_QUEUE 65536

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
    int      use3d;      /* Fragments through the texture, depth and blend units. */
    int      frag_x;     /* The fragment in the 3D units (LUT DirectIndex). */
    int      frag_y;
    int64_t  z;          /* 17.11 */
    int64_t  dz_dx;
    int64_t  dz_dy;
    int64_t  tex[3];     /* S, T (texels at bit 20), Q (one at bit 30). */
    int64_t  dtex_dx[3];
    int64_t  dtex_dy[3];
    int64_t  k[3];       /* Fog, Ks, Kd: 2.16. */
    int64_t  dk_dx[3];
    int64_t  dk_dy[3];
    int      out_wait;   /* An upload waiting for output FIFO space. */
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
    uint8_t revision;
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
    int      flash_cycle;   /* JEDEC command bytes seen (ROM access). */
    int      flash_id_mode; /* Software ID mode: the flash part's IDs at 0/1. */
    uint32_t dma_address;
    uint32_t dma_count;
    uint32_t dma_control;
    uint32_t out_dma_address;
    uint32_t out_dma_cursor;
    uint32_t out_dma_count;
    uint32_t fifo_discon;
    uint32_t chip_config;
    uint32_t agp_tex_base;
    uint32_t bydma[10];          /* 0xA0-0xE8 in 8-byte steps (bypass DMA). */
    uint32_t bydma_x;            /* Aperture auto addressing cursor. */
    uint32_t bydma_y;
    int      tex_invalid;        /* TextureID/TexelLUTID loaded an invalid handle: halted. */
    uint32_t tex_invalid_tag;
    uint32_t tex_invalid_val;
    uint32_t mem_control;
    uint32_t boot_address;
    uint32_t mem_config;
    uint32_t bypass_mask;
    uint32_t fb_write_mask;
    uint32_t vs_regs[0x400];     /* 0x5000-0x5fff, kept for read back. */
    uint32_t ext_brd[2];
    uint32_t vtg[VTG_NUM];

    void *i2c_ddc; /* DisplayData: the monitor's EDID. */
    void *ddc;
    void *i2c_vs;  /* VSSerialBusControl: nothing on it. */

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
    uint32_t    tag_count[GP_TAGS]; /* Debug: writes per tag ("dev p2 tags"). */
    int         mode_log;           /* Debug: "dev p2 modes" logs each new 3D mode set. */
    int         modes_seen;
    uint32_t    modes[256][9];
    uint32_t    dl_start; /* Debug: the current texture download. */
    uint32_t    dl_words;
    int         range_logged;
    uint32_t    texel_lut[256];
    p2_raster_t raster;
    double      vtx[3][16];      /* Delta vertex parameters (see p2_delta_vertex). */
    uint32_t    delta_cmd;       /* The last DrawTriangle/DrawLine, for the Repeat commands. */
    int         delta_line10;    /* The last line ran from vertex 1 to 0. */
    int         suspended;       /* SuspendUntilFrameBlank: waiting for the next blank. */
    uint32_t    suspend_base;
    uint32_t fifo_hdr_count;     /* Data words still owed to the current header. */
    uint32_t fifo_hdr_tag;
    uint32_t fifo_hdr_mode;
    uint32_t fifo_hdr_mask;
    uint32_t out_fifo[P2_OUT_FIFO];
    int      out_rd;
    int      out_wr;
    int      out_count;
    uint32_t *hold_tag;
    uint32_t *hold_val;
    int       hold_rd;
    int       hold_wr;
    int       hold_count;
    int       in_resume;
    int       hold_overflow;
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
static void     p2_out_space(p2_t *p2);
static void     p2_vtg_write(p2_t *p2, int idx, uint32_t val);
static int      p2_gp_stalled(p2_t *p2);
static int      p2_lb_upload_cmd(p2_t *p2, uint32_t command);

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

    if (p2->suspended) {
        /* SuspendUntilFrameBlank: the new frame is shown from this blank on,
           and the graphics processor goes on with what was held back. */
        p2->suspended = 0;
        p2_vtg_write(p2, VTG_SCREEN_BASE, p2->suspend_base);
        if (p2_vtg_active(p2)) {
            /* The SVGA core latched this frame's start before calling us: the
               new page must show from this frame on, or the drivers (released
               now) draw into the page still on screen. */
            if (svga->interlace && svga->oddeven)
                svga->memaddr = svga->memaddr_backup = svga->memaddr_latch + (svga->rowoffset << 1) + svga->hblank_sub;
            else
                svga->memaddr = svga->memaddr_backup = svga->memaddr_latch + svga->hblank_sub;
            if (!(svga->adv_flags & FLAG_NO_SHIFT3)) {
                svga->memaddr <<= 2;
                svga->memaddr_backup <<= 2;
            }
        }
        p2_out_space(p2);
    }

    p2_raise_int(p2, INT_VRETRACE);
}

/* ------------------------------------------------------------------------- */
/* Output FIFO (host reads, filled by Sync and readback tags)                */
/* ------------------------------------------------------------------------- */

static uint32_t
p2_out_fifo_pop(p2_t *p2)
{
    uint32_t val;

    if (!p2->out_count)
        return 0;
    val        = p2->out_fifo[p2->out_rd];
    p2->out_rd = (p2->out_rd + 1) & (P2_OUT_FIFO - 1);
    p2->out_count--;
    return val;
}

static uint32_t
p2_dma_swap(p2_t *p2, uint32_t v)
{
    if (p2->dma_control & 0x10)
        v = (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
    return v;
}

/* Output DMA: while OutDMACount runs, the output FIFO drains into system
   memory (completion is polled through OutDMACount). */
static void
p2_out_dma_run(p2_t *p2)
{
    while (p2->out_dma_count && p2->out_count) {
        mem_writel_phys(p2->out_dma_cursor, p2_dma_swap(p2, p2_out_fifo_pop(p2)));
        p2->out_dma_cursor += 4;
        p2->out_dma_count--;
    }
}

static void
p2_out_fifo_push(p2_t *p2, uint32_t val)
{
    if (p2->out_count >= P2_OUT_FIFO)
        return;
    p2->out_fifo[p2->out_wr] = val;
    p2->out_wr               = (p2->out_wr + 1) & (P2_OUT_FIFO - 1);
    p2->out_count++;
    p2_out_dma_run(p2);
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

/* Memory clock synthesiser (MCLK, 30h-32h), the same layout (BIOSes leave
   its P bit 3 clear: ELSA's 7Bh/0Bh/01h is 80 MHz). */
static double
p2_memclk(p2_t *p2)
{
    const uint8_t m = p2->dac_regs[TVP_MEMCLK_1];
    const uint8_t d = p2->dac_regs[TVP_MEMCLK_1 + 1];
    const uint8_t p = p2->dac_regs[TVP_MEMCLK_1 + 2];

    if (!m || !d)
        return 70000000.0;

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

    if (!p2_vtg_active(p2) && (svga->bpp != 8)) {
        /* Back from the VTG: its depth is no VGA mode's (Windows 98's
           shutdown screen came out at 15 bpp). Work the timings out again. */
        svga->bpp = 8;
        svga_recalctimings(svga);
        return;
    }

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

/* Unaligned words and dwords (the DIB engine reads 16 bpp sources at odd
   pixels as dwords) reach the card as aligned PCI cycles with byte enables:
   the bytes at the address asked for, each through its own byte lane. */
static uint16_t
p2_lfb_readw(uint32_t addr, void *priv)
{
    const p2_t *p2 = (p2_t *) priv;

    if (addr & 1)
        return p2_lfb_readb(addr, priv) | (p2_lfb_readb(addr + 1, priv) << 8);
    return *(uint16_t *) &p2->svga.vram[addr & p2->vram_mask];
}

static uint32_t
p2_lfb_readl(uint32_t addr, void *priv)
{
    const p2_t *p2 = (p2_t *) priv;

    if (addr & 3)
        return p2_lfb_readw(addr, priv) | ((uint32_t) p2_lfb_readw(addr + 2, priv) << 16);
    return *(uint32_t *) &p2->svga.vram[addr & p2->vram_mask];
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

    if (addr & 1) {
        p2_lfb_writeb(addr, val, priv);
        p2_lfb_writeb(addr + 1, val >> 8, priv);
        return;
    }
    addr &= p2->vram_mask;
    v  = (uint16_t *) &p2->svga.vram[addr];
    *v = (*v & ~mask) | (val & mask);
    p2_vram_changed(p2, addr);
}

static void
p2_lfb_writel(uint32_t addr, uint32_t val, void *priv)
{
    p2_t     *p2 = (p2_t *) priv;
    uint32_t *v;

    if (addr & 3) {
        p2_lfb_writew(addr, val, priv);
        p2_lfb_writew(addr + 2, val >> 16, priv);
        return;
    }
    addr &= p2->vram_mask;
    v  = (uint32_t *) &p2->svga.vram[addr];
    *v = (*v & ~p2->bypass_mask) | (val & p2->bypass_mask);
    p2_vram_changed(p2, addr);
}

/* ApertureOne/Two bit 9 (ROM access): that aperture reaches the expansion ROM
   instead of memory. ELSA's drivers read the board's ID block (the word at 0Ah
   points to it) and identify the flash part through it, with the JEDEC
   software ID commands; the part reported is an Atmel AT29C257 (32 KB) or
   AT29C512 (64 KB), as the image's size calls for. Programming is ignored. */
static inline int
p2_ap_rom(const p2_t *p2, int n)
{
    return (p2->aperture[n] & 0x200) && (p2->bios_rom.rom != NULL);
}

static uint8_t
p2_ap_rom_readb(const p2_t *p2, uint32_t addr)
{
    addr &= p2->bios_rom.sz - 1;
    if (p2->flash_id_mode && (addr < 2))
        return addr ? ((p2->bios_rom.sz > 0x8000) ? 0x5d : 0xdc) : 0x1f;

    return p2->bios_rom.rom[addr];
}

static void
p2_ap_rom_writeb(p2_t *p2, uint32_t addr, uint8_t val)
{
    static const uint16_t cmd_addr[2] = { 0x5555, 0x2aaa };
    static const uint8_t  cmd_val[2]  = { 0xaa, 0x55 };

    addr &= 0x7fff;
    if (val == 0xf0) {
        p2->flash_id_mode = 0;
        p2->flash_cycle   = 0;
    } else if (p2->flash_cycle < 2) {
        p2->flash_cycle = ((addr == cmd_addr[p2->flash_cycle]) && (val == cmd_val[p2->flash_cycle])) ? (p2->flash_cycle + 1) : 0;
    } else {
        if (addr == 0x5555) {
            if (val == 0x90)
                p2->flash_id_mode = 1;
        }
        p2->flash_cycle = 0;
    }
}

static uint32_t
p2_ap_read(p2_t *p2, int n, uint32_t addr, int size)
{
    uint32_t ret = 0;

    if (!p2_ap_rom(p2, n)) {
        switch (size) {
            case 1:
                return p2_lfb_readb(addr, p2);
            case 2:
                return p2_lfb_readw(addr, p2);
            default:
                return p2_lfb_readl(addr, p2);
        }
    }

    for (int i = 0; i < size; i++)
        ret |= (uint32_t) p2_ap_rom_readb(p2, addr + i) << (i << 3);
    return ret;
}

static int p2_bypass_aperture(p2_t *p2, int n, uint32_t val, int size);

static void
p2_ap_write(p2_t *p2, int n, uint32_t addr, uint32_t val, int size)
{
    if (p2_bypass_aperture(p2, n, val, size))
        return;
    if (!p2_ap_rom(p2, n)) {
        switch (size) {
            case 1:
                p2_lfb_writeb(addr, val, p2);
                break;
            case 2:
                p2_lfb_writew(addr, val, p2);
                break;
            default:
                p2_lfb_writel(addr, val, p2);
                break;
        }
        return;
    }

    for (int i = 0; i < size; i++)
        p2_ap_rom_writeb(p2, addr + i, val >> (i << 3));
}

static uint8_t
p2_ap0_readb(uint32_t addr, void *priv)
{
    return p2_ap_read((p2_t *) priv, 0, addr, 1);
}

static uint16_t
p2_ap0_readw(uint32_t addr, void *priv)
{
    return p2_ap_read((p2_t *) priv, 0, addr, 2);
}

static uint32_t
p2_ap0_readl(uint32_t addr, void *priv)
{
    return p2_ap_read((p2_t *) priv, 0, addr, 4);
}

static void
p2_ap0_writeb(uint32_t addr, uint8_t val, void *priv)
{
    p2_ap_write((p2_t *) priv, 0, addr, val, 1);
}

static void
p2_ap0_writew(uint32_t addr, uint16_t val, void *priv)
{
    p2_ap_write((p2_t *) priv, 0, addr, val, 2);
}

static void
p2_ap0_writel(uint32_t addr, uint32_t val, void *priv)
{
    p2_ap_write((p2_t *) priv, 0, addr, val, 4);
}

static uint8_t
p2_ap1_readb(uint32_t addr, void *priv)
{
    return p2_ap_read((p2_t *) priv, 1, addr, 1);
}

static uint16_t
p2_ap1_readw(uint32_t addr, void *priv)
{
    return p2_ap_read((p2_t *) priv, 1, addr, 2);
}

static uint32_t
p2_ap1_readl(uint32_t addr, void *priv)
{
    return p2_ap_read((p2_t *) priv, 1, addr, 4);
}

static void
p2_ap1_writeb(uint32_t addr, uint8_t val, void *priv)
{
    p2_ap_write((p2_t *) priv, 1, addr, val, 1);
}

static void
p2_ap1_writew(uint32_t addr, uint16_t val, void *priv)
{
    p2_ap_write((p2_t *) priv, 1, addr, val, 2);
}

static void
p2_ap1_writel(uint32_t addr, uint32_t val, void *priv)
{
    p2_ap_write((p2_t *) priv, 1, addr, val, 4);
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

/* ------------------------------------------------------------------------- */
/* Bypass unit (P2A): host data straight into Permedia memory, by its own    */
/* DMA master or written through an aperture, laid out as a texture (data    */
/* size, pitch from partial products, X/Y offsets). Patch layouts are kept   */
/* linear like everywhere else here.                                         */
/* ------------------------------------------------------------------------- */

static uint32_t p2_pitch(uint32_t mode);
static void     p2_fb_write(p2_t *p2, int64_t addr, int bpp, uint32_t val, uint32_t hwmask);

/* One data item (bypass data format: 8/16/32 bits, 4 bits, YUV planes) at
   item X, Y of the transfer. */
static void
p2_bypass_store(p2_t *p2, uint32_t x, uint32_t y, uint32_t v)
{
    const uint32_t ctl   = p2->bydma[(P2_BYDMA_CONTROL - P2_BYDMA_ADDRESS) >> 3];
    const uint32_t fmt   = (ctl >> 3) & 7;
    const uint32_t pitch = p2_pitch(ctl >> 8);
    const int64_t  base  = p2->bydma[(P2_BYDMA_MEMADDR - P2_BYDMA_ADDRESS) >> 3] & 0xffffff;
    const int64_t  tx    = x + ((ctl >> 17) & 31);
    const int64_t  ty    = y + ((ctl >> 22) & 31);
    const int64_t  texel = base + ty * pitch + tx;

    switch (fmt) {
        case 0:
            p2_fb_write(p2, texel, 1, v, 0xffffffff);
            break;
        case 1:
            p2_fb_write(p2, texel * 2, 2, v, 0xffffffff);
            break;
        case 2:
            p2_fb_write(p2, texel * 4, 4, v, 0xffffffff);
            break;
        case 3: {
            /* 4 bits, the low nibble first. */
            const uint32_t m = (texel & 1) ? 0xf0 : 0x0f;

            p2_fb_write(p2, texel >> 1, 1, (texel & 1) ? (v << 4) : v, m * 0x01010101);
            break;
        }
        case 4: /* Y plane into YUV 4:2:2 texels (byte 0). */
            p2_fb_write(p2, texel * 2, 1, v, 0xffffffff);
            break;
        case 5: /* U plane: byte 1 of the even texel of each pair. */
        case 6: /* V plane: byte 1 of the odd one. */
        {
            const int64_t t = base + ty * pitch + tx * 2 + (fmt == 6);

            p2_fb_write(p2, t * 2 + 1, 1, v, 0xffffffff);
            break;
        }
        default:
            break;
    }
}

static uint32_t
p2_bypass_swap(uint32_t ctl, uint32_t w)
{
    switch ((ctl >> 27) & 3) {
        case 1:
            return (w << 24) | ((w & 0xff00) << 8) | ((w >> 8) & 0xff00) | (w >> 24);
        case 2:
            return (w << 16) | (w >> 16);
        default:
            return w;
    }
}

/* One host dword of a transfer: the data items it holds, the scanline's
   edge byte masks applied to its first and last dwords. */
static void
p2_bypass_word(p2_t *p2, uint32_t w, uint32_t *x, uint32_t y, uint32_t width, int first)
{
    const uint32_t ctl   = p2->bydma[(P2_BYDMA_CONTROL - P2_BYDMA_ADDRESS) >> 3];
    const uint32_t mask  = p2->bydma[(P2_BYDMA_BYTEMASK - P2_BYDMA_ADDRESS) >> 3];
    const uint32_t fmt   = (ctl >> 3) & 7;
    const int      bits  = (fmt == 1) ? 16 : ((fmt == 2) ? 32 : ((fmt == 3) ? 4 : 8));
    const int      items = 32 / bits;
    const int      last  = (*x + items) >= width;

    w = p2_bypass_swap(ctl, w);
    for (int i = 0; (i < items) && (*x < width); i++) {
        const int byte = (i * bits) >> 3;

        /* Set mask bits leave that byte of the edge dword alone. */
        if (!((first && ((mask >> byte) & 1)) || (last && ((mask >> (16 + byte)) & 1))))
            p2_bypass_store(p2, *x, y, (bits == 32) ? w : ((w >> (i * bits)) & ((1u << bits) - 1)));
        (*x)++;
    }
}

static void p2_tex_restart(p2_t *p2);

/* ByDMAControl mode 1: the whole transfer from host memory at once. */
static void
p2_bypass_dma(p2_t *p2)
{
    const uint32_t ctl    = p2->bydma[(P2_BYDMA_CONTROL - P2_BYDMA_ADDRESS) >> 3];
    const uint32_t size   = p2->bydma[(P2_BYDMA_SIZE - P2_BYDMA_ADDRESS) >> 3];
    const uint32_t width  = size & 0xfff;
    const uint32_t height = (size >> 16) & 0xfff;
    const uint32_t fmt    = (ctl >> 3) & 7;
    const int      bits   = (fmt == 1) ? 16 : ((fmt == 2) ? 32 : ((fmt == 3) ? 4 : 8));
    const uint32_t words  = (width * bits + 31) / 32;
    uint32_t       src    = p2->bydma[0];

    for (uint32_t y = 0; y < height; y++) {
        uint32_t x = 0;
        uint32_t a = src;

        for (uint32_t i = 0; i < words; i++, a += 4)
            p2_bypass_word(p2, mem_readl_phys(a), &x, y, width, i == 0);
        src += p2->bydma[(P2_BYDMA_STRIDE - P2_BYDMA_ADDRESS) >> 3];
    }
    p2_raise_int(p2, INT_BYPASS_DMA);
    if (ctl & 4)
        p2_tex_restart(p2);
}

/* ByDMAControl modes 2/3: host writes to aperture 1/2 land at the transfer's
   next position, whatever their address. Returns 1 if the write was taken. */
static int
p2_bypass_aperture(p2_t *p2, int n, uint32_t val, int size)
{
    const uint32_t ctl    = p2->bydma[(P2_BYDMA_CONTROL - P2_BYDMA_ADDRESS) >> 3];
    const uint32_t sz     = p2->bydma[(P2_BYDMA_SIZE - P2_BYDMA_ADDRESS) >> 3];
    const uint32_t width  = sz & 0xfff;
    const uint32_t height = (sz >> 16) & 0xfff;

    if (((ctl & 3) != (uint32_t) (2 + n)) || (p2->bydma_y >= height) || !width)
        return 0;
    if (size < 4)
        val &= (1u << (size * 8)) - 1;
    p2_bypass_word(p2, val, &p2->bydma_x, p2->bydma_y, width, !p2->bydma_x);
    if (p2->bydma_x >= width) {
        p2->bydma_x = 0;
        if (++p2->bydma_y >= height) {
            p2_raise_int(p2, INT_BYPASS_DMA);
            if (ctl & 4)
                p2_tex_restart(p2);
        }
    }
    return 1;
}

/* Input DMA: feeds the input FIFO from system memory. DMACount counts words
   moved into the FIFO, not executed ones: behind a stalled graphics processor
   it goes on until the 256-entry FIFO is full (p2_out_space resumes it). */
static void
p2_dma_run(p2_t *p2)
{
    if (!p2->dma_count)
        return;
    while (p2->dma_count && (p2->hold_count < 256)) {
        p2_fifo_write(p2, p2_dma_swap(p2, mem_readl_phys(p2->dma_address)));
        p2->dma_address += 4;
        p2->dma_count--;
    }
    if (!p2->dma_count)
        p2_raise_int(p2, INT_DMA);
}

static void
p2_i2c_write(void *i2c, uint32_t val)
{
    i2c_gpio_set(i2c, !!(val & P2_I2C_CLK_OUT), !!(val & P2_I2C_DATA_OUT));
}

static uint32_t
p2_i2c_read(void *i2c, uint32_t val)
{
    val &= ~(P2_I2C_DATA_IN | P2_I2C_CLK_IN);
    if (i2c_gpio_get_scl(i2c))
        val |= P2_I2C_CLK_IN;
    if (i2c_gpio_get_sda(i2c))
        val |= P2_I2C_DATA_IN;
    return val;
}

/* Both buses released at reset. */
static void
p2_i2c_reset(p2_t *p2)
{
    p2->vtg[VTG_DISPLAY_DATA]                    = P2_I2C_CLK_OUT | P2_I2C_DATA_OUT;
    p2->vs_regs[(P2_VS_SERIAL_BUS & 0xfff) >> 2] = P2_I2C_CLK_OUT | P2_I2C_DATA_OUT;
    p2_i2c_write(p2->i2c_ddc, p2->vtg[VTG_DISPLAY_DATA]);
    p2_i2c_write(p2->i2c_vs, p2->vs_regs[(P2_VS_SERIAL_BUS & 0xfff) >> 2]);
}

static void
p2_vtg_write(p2_t *p2, int idx, uint32_t val)
{
    svga_t *svga = &p2->svga;

    switch (idx) {
        case VTG_LINE_COUNT:
            return;
        case VTG_DISPLAY_DATA:
            p2->vtg[idx] = val;
            p2_i2c_write(p2->i2c_ddc, val);
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
            if (addr == P2_VS_SERIAL_BUS)
                p2_i2c_write(p2->i2c_vs, val);
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
            p2_dma_run(p2);
            break;
        case P2_ERROR_FLAGS:
            p2->error_flags &= ~val;
            break;
        case P2_OUT_DMA_ADDR:
            p2->out_dma_address = val & ~3;
            break;
        case P2_OUT_DMA_COUNT:
            p2->out_dma_count  = val & 0xffff;
            p2->out_dma_cursor = p2->out_dma_address;
            p2_out_dma_run(p2);
            p2_out_space(p2);
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
        case P2_AGP_TEX_BASE:
            p2->agp_tex_base = val;
            break;
        case P2_BYDMA_ADDRESS:
        case P2_BYDMA_STRIDE:
        case P2_BYDMA_MEMADDR:
        case P2_BYDMA_SIZE:
        case P2_BYDMA_BYTEMASK:
            p2->bydma[(addr - P2_BYDMA_ADDRESS) >> 3] = val;
            break;
        case P2_BYDMA_CONTROL:
            p2->bydma[(addr - P2_BYDMA_ADDRESS) >> 3] = val;
            p2->bydma_x = p2->bydma_y = 0;
            if ((val & 3) == 1)
                p2_bypass_dma(p2);
            break;
        case P2_BYDMA_COMPLETE:
            p2_tex_restart(p2);
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
            ret = p2_out_fifo_pop(p2);
            p2_out_space(p2);
            return ret;

        case 0x3000:
            if (!(addr & 7) && ((addr & 0xfff) >> 3) < VTG_NUM) {
                if (((addr & 0xfff) >> 3) == VTG_LINE_COUNT)
                    return svga->vc;
                if (((addr & 0xfff) >> 3) == VTG_DISPLAY_DATA)
                    return p2_i2c_read(p2->i2c_ddc, p2->vtg[VTG_DISPLAY_DATA]);
                return p2->vtg[(addr & 0xfff) >> 3];
            }
            return 0;

        case 0x4000:
            return p2_dac_read(p2, (addr >> 3) & 0x0f);

        case 0x5000:
            if (addr == P2_VS_SERIAL_BUS)
                return p2_i2c_read(p2->i2c_vs, p2->vs_regs[(addr & 0xfff) >> 2]);
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
            /* Empty: 256 entries and two more in the input stage. ELSA's NT
               driver waits for 258 before it touches the core. */
            ret = 258;
            if (p2->hold_count)
                /* Words held behind a stall fill the FIFO: the host waits. */
                ret = (p2->hold_count >= 256) ? 0 : (256 - p2->hold_count);
            break;
        case P2_OUT_FIFO_WORDS:
            /* The real FIFO holds 8 words: drivers read as many as this says
               into buffers sized for the transfer (3Dlabs' 3dextdd). */
            ret = MIN(p2->out_count, 8);
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
        case P2_OUT_DMA_ADDR:
            ret = p2->out_dma_address;
            break;
        case P2_OUT_DMA_COUNT:
            ret = p2->out_dma_count;
            break;
        case P2_FIFO_DISCON:
            ret = p2->fifo_discon;
            break;
        case P2_AGP_TEX_BASE:
            ret = p2->agp_tex_base;
            break;
        case P2_BYDMA_ADDRESS:
        case P2_BYDMA_STRIDE:
        case P2_BYDMA_MEMADDR:
        case P2_BYDMA_SIZE:
        case P2_BYDMA_BYTEMASK:
        case P2_BYDMA_CONTROL:
            ret = p2->bydma[(addr - P2_BYDMA_ADDRESS) >> 3];
            break;
        case P2_BYDMA_COMPLETE:
            ret = p2->tex_invalid ? 1 : 0; /* The texture invalid flag. */
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
            /* Free-running MClk cycle count: drivers time loops and
               measure the clock with it (ELSA's waits on it at boot). */
            ret = (uint32_t) (uint64_t) ((double) tsc * 4294967296.0 / (double) TIMER_USEC * (p2_memclk(p2) / 1000000.0));
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

#define TAG_FILTER_MODE        0x180
#define TAG_SYNC               0x188
#define TAG_SUSPEND_FRAMEBLANK 0x18f

/* The input FIFO is not drained while an upload waits for the host or a
   page flip waits for the frame blank. */
static int
p2_gp_stalled(p2_t *p2)
{
    return (p2->raster.active && p2->raster.out_wait) || p2->suspended || p2->tex_invalid;
}

static void
p2_gp_reset(p2_t *p2)
{
    p2->fifo_hdr_count = 0;
    p2->out_rd = p2->out_wr = p2->out_count = 0;
    p2->hold_rd = p2->hold_wr = p2->hold_count = 0;
    p2->raster.active   = 0;
    p2->raster.out_wait = 0;
    p2->suspended       = 0;
    p2->tex_invalid     = 0;
}

static void
p2_gp_write(p2_t *p2, uint32_t tag, uint32_t val)
{
    tag &= GP_TAGS - 1;

    /* Behind a stalled upload or a frame blank wait, later writes wait in the input FIFO. */
    if (p2_gp_stalled(p2) || (p2->hold_count && !p2->in_resume)) {
        if (p2->hold_count >= P2_HOLD_QUEUE) {
            if (!p2->hold_overflow++)
                pclog("P2: input FIFO hold queue overflow (tag %03X)\n", tag);
        } else {
            p2->hold_tag[p2->hold_wr] = tag;
            p2->hold_val[p2->hold_wr] = val;
            p2->hold_wr               = (p2->hold_wr + 1) & (P2_HOLD_QUEUE - 1);
            p2->hold_count++;
        }
        return;
    }

    if (p2_do_log >= 3)
        pclog("P2: GP %03X = %08X\n", tag, val);
    p2->tag_count[tag]++;

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

        case TAG_SUSPEND_FRAMEBLANK:
            /* Page flip: hold everything after it until the next frame blank,
               which shows the frame at this address (VTG ScreenBase units). */
            p2->gp[tag]        = val;
            p2->suspend_base   = val;
            p2->suspended      = 1;
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
#define TAG_DS_DY_DOM           0x073
#define TAG_DT_DX               0x075
#define TAG_DT_DY_DOM           0x076
#define TAG_TEXEL_LUT_INDEX     0x098
#define TAG_TEXEL_LUT_DATA      0x099
#define TAG_TEXEL0              0x0c0
#define TAG_TEXEL_LUT_MODE      0x0cf
#define TAG_TEXEL_LUT0          0x1d0
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
#define TAG_TEXTURE_DATA        0x11d
#define TAG_TEXTURE_DL_OFFSET   0x11e
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

/* 3D units. */
#define TAG_TEXTURE_ADDR_MODE   0x070
#define TAG_Q_START             0x077
#define TAG_DQ_DX               0x078
#define TAG_DQ_DY_DOM           0x079
#define TAG_TEXTURE_COLOR_MODE  0x0d0
#define TAG_FOG_MODE            0x0d2
#define TAG_FOG_COLOR           0x0d3
#define TAG_F_START             0x0d4
#define TAG_DF_DX               0x0d5
#define TAG_DF_DY_DOM           0x0d6
#define TAG_KS_START            0x0d9
#define TAG_DKS_DX              0x0da
#define TAG_DKS_DY_DOM          0x0db
#define TAG_KD_START            0x0dc
#define TAG_DKD_DX              0x0dd
#define TAG_DKD_DY_DOM          0x0de
#define TAG_ALPHA_BLEND_MODE    0x102
#define TAG_LB_READ_MODE        0x110
#define TAG_LB_READ_FORMAT      0x111
#define TAG_LB_SOURCE_OFFSET    0x112
#define TAG_LB_WINDOW_BASE      0x117
#define TAG_LB_WRITE_MODE       0x118
#define TAG_LB_WRITE_FORMAT     0x119
#define TAG_WINDOW              0x130
#define TAG_STENCIL_MODE        0x131
#define TAG_STENCIL_DATA        0x132
#define TAG_STENCIL             0x133
#define TAG_DEPTH_MODE          0x134
#define TAG_DEPTH               0x135
#define TAG_Z_START_U           0x136
#define TAG_Z_START_L           0x137
#define TAG_DZ_DX_U             0x138
#define TAG_DZ_DX_L             0x139
#define TAG_DZ_DY_DOM_U         0x13a
#define TAG_DZ_DY_DOM_L         0x13b
#define TAG_YUV_MODE            0x1e0
#define TAG_CHROMA_UPPER        0x1e1
#define TAG_CHROMA_LOWER        0x1e2
#define TAG_ALPHA_MAP_UPPER     0x1e3
#define TAG_ALPHA_MAP_LOWER     0x1e4
#define TAG_TEXEL_LUT_ADDRESS   0x09a
#define TAG_TEXEL_LUT_TRANSFER  0x09b
#define TAG_LB_DATA             0x113
#define TAG_LB_STENCIL          0x115
#define TAG_LB_DEPTH            0x116
#define TAG_TEXTURE_ID          0x1ee
#define TAG_TEXEL_LUT_ID        0x1ef
/* Delta unit: three vertices of 16 parameters, fixed point then float. */
#define TAG_V0_FIXED            0x200
#define TAG_V0_FLOAT            0x230
#define TAG_DELTA_MODE          0x260
#define TAG_DRAW_TRIANGLE       0x261
#define TAG_REPEAT_TRIANGLE     0x262
#define TAG_DRAW_LINE01         0x263
#define TAG_DRAW_LINE10         0x264
#define TAG_REPEAT_LINE         0x265

#define RENDER_AREA_STIPPLE  (1 << 0)
#define RENDER_FAST_FILL     (1 << 3)
#define RENDER_PRIM_MASK     0xc0
#define RENDER_SYNC_BITMASK  (1 << 11)
#define RENDER_SYNC_HOSTDATA (1 << 12)
#define RENDER_TEXTURE       (1 << 13)
#define RENDER_FOG           (1 << 14)
#define RENDER_SUBPIXEL      (1 << 16)
#define RENDER_REUSE_BITMASK (1 << 17)
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

/* Colour formats of the texture, alpha blend and dither units: R, G, B and A
   widths and the lowest bit (the "Back" formats sit in the upper half). In
   BGR order red is packed lowest, in RGB order blue. */
typedef struct p2_color_fmt_t {
    uint8_t r, g, b, a, base;
} p2_color_fmt_t;

static const p2_color_fmt_t p2_color_fmts[32] = {
    [0] = { 8, 8, 8, 8, 0 },  [1] = { 5, 5, 5, 1, 0 },  [2] = { 4, 4, 4, 4, 0 },
    [5] = { 3, 3, 2, 0, 0 },  [6] = { 3, 3, 2, 0, 8 },  [9] = { 2, 3, 2, 1, 0 },
    [10] = { 2, 3, 2, 1, 8 }, [11] = { 2, 3, 2, 0, 0 }, [12] = { 2, 3, 2, 0, 8 },
    [13] = { 5, 5, 5, 1, 16 }, [16] = { 5, 6, 5, 0, 0 }, [17] = { 5, 6, 5, 0, 16 },
};

static uint32_t p2_pack_abgr(const uint32_t c[4]);
static void     p2_unpack_abgr(uint32_t v, uint32_t c[4]);

/* An internal AABBGGRR colour in the framebuffer's format (the dither unit,
   DitherMode: every format of its table, Front/Back and CI values replicated
   across the pixel, Offset formats +64, ordered dither, ForceAlpha). */
static uint32_t
p2_format_color(p2_t *p2, uint32_t v, int x, int y)
{
    const uint32_t        dm  = p2->gp[TAG_DITHER_MODE];
    const uint32_t        fmt = ((dm >> 2) & 15) | ((dm >> 12) & 16);
    const int             rgb = !!(dm & 0x400);
    const p2_color_fmt_t *f   = &p2_color_fmts[fmt];
    uint32_t              c[4];
    uint32_t              packed;
    int                   pos;

    if (!(dm & 1))
        return v;
    if (fmt == 14) {
        /* Colour index, in every byte. */
        v &= 0xff;
        return v * 0x01010101;
    }
    p2_unpack_abgr(v, c);
    if ((dm >> 12) & 3)
        c[3] = (((dm >> 12) & 3) == 1) ? 0 : 0xf8;

    if (!f->r) {
        /* 8888. */
        return rgb ? (c[2] | (c[1] << 8) | (c[0] << 16) | (c[3] << 24)) : p2_pack_abgr(c);
    }

    if (dm & 2) {
        /* 2x2 ordered dither before truncation, a quarter step of each width. */
        static const uint8_t dither[2][2] = { { 0, 2 }, { 3, 1 } };
        const uint32_t       d            = dither[(y + (dm >> 8)) & 1][(x + (dm >> 6)) & 1];
        const int            w[3]         = { f->r, f->g, f->b };

        for (int i = 0; i < 3; i++)
            c[i] = MIN(255, c[i] + ((d << (8 - w[i])) >> 2));
    }

    /* Lowest field first: red in BGR order, blue in RGB order. */
    {
        const uint32_t vals[3]  = { rgb ? c[2] : c[0], c[1], rgb ? c[0] : c[2] };
        const int      width[3] = { rgb ? f->b : f->r, f->g, rgb ? f->r : f->b };

        packed = 0;
        pos    = 0;
        for (int i = 0; i < 3; i++) {
            packed |= (vals[i] >> (8 - width[i])) << pos;
            pos += width[i];
        }
        if (f->a)
            packed |= (c[3] >> (8 - f->a)) << pos;
        pos += f->a;
    }
    if ((fmt == 11) || (fmt == 12))
        packed += 64; /* Offset formats. */

    /* Front and Back formats carry the value in both halves. */
    if (pos <= 8)
        return (packed | (packed << 8)) * 0x00010001;
    if (pos <= 16)
        return packed | (packed << 16);
    return packed;
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
    const uint32_t lutm  = p2->gp[TAG_TEXEL_LUT_MODE];
    uint64_t       pat   = p2->gp[TAG_FB_BLOCK_COLOR_L] | ((uint64_t) p2->gp[TAG_FB_BLOCK_COLOR_U] << 32);

    if ((lutm & 3) == 3) {
        /* LUT DirectIndex block fills: Y's low 3 bits pick two consecutive
           entries for the lower and upper halves of the block colour. */
        const uint32_t i = ((y & 7) * 2 + ((lutm >> 2) & 0xff)) & 0xff;

        const uint32_t j = (i + 1) & 0xff;

        pat = ((i < 16) ? p2->gp[TAG_TEXEL_LUT0 + i] : p2->texel_lut[i]) |
              ((uint64_t) ((j < 16) ? p2->gp[TAG_TEXEL_LUT0 + j] : p2->texel_lut[j]) << 32);
    }
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

/* The DDAs' step along the dominant edge to the next scanline. */
static void
p2_step_interpolants(p2_raster_t *r)
{
    if (r->shade) {
        for (int i = 0; i < 3; i++)
            r->color[i] += r->dcolor_dy[i];
    }
    if (r->use3d) {
        r->z += r->dz_dy;
        for (int i = 0; i < 3; i++) {
            r->tex[i] += r->dtex_dy[i];
            r->k[i] += r->dk_dy[i];
        }
    }
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
    p2_step_interpolants(r);
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

static int64_t
p2_tex_wrap(int64_t v, int bits, int mode)
{
    const int64_t size = (int64_t) 1 << bits;

    switch (mode & 3) {
        case 1: /* Repeat. */
            return ((v % size) + size) % size;
        case 2: /* Mirror. */
            v = ((v % (2 * size)) + 2 * size) % (2 * size);
            return (v < size) ? v : (2 * size - 1 - v);
        default: /* Clamp. */
            return MAX(0, MIN(size - 1, v));
    }
}

/* ------------------------------------------------------------------------- */
/* YUV unit: video playback                                                  */
/*                                                                           */
/* The chip has no overlay scaler: its video streams unit (control space     */
/* 5000h-5FFFh) is the VMI capture port and the TV encoder port, and neither */
/* reaches the monitor. Drivers show video by texture-mapping YUV surfaces   */
/* onto the screen (DirectDraw FourCC stretch blits: YUY2, Y411), with the   */
/* YUV unit converting to RGB and its chroma test giving the colour key.     */
/* ------------------------------------------------------------------------- */

#define TEXFMT_YUV444 18
#define TEXFMT_YUV422 19

/* A YUV texel as an internal colour: Y in the red byte, U in green, V in
   blue (the BGR column of the TextureDataFormat table). YUV 4:2:2 texels are
   16 bits, luma and one chroma sample each, U in the even texel of a pair
   and V in the odd one; ColorOrder swaps the bytes (YUY2 / UYVY). */
static uint32_t
p2_texel_yuv(p2_t *p2, int fmt, int64_t tex, uint32_t v, int rgb_order)
{
    uint32_t y;
    uint32_t u;
    uint32_t w;

    if (fmt == TEXFMT_YUV444) {
        if (rgb_order)
            v = (v & 0xff00ff00) | ((v >> 16) & 0xff) | ((v & 0xff) << 16);
        return v;
    }

    w = p2_fb_read(p2, (tex ^ 1) * 2, 2);
    if (rgb_order) {
        y = (v >> 8) & 0xff;
        u = v & 0xff;
        w &= 0xff;
    } else {
        y = v & 0xff;
        u = (v >> 8) & 0xff;
        w = (w >> 8) & 0xff;
    }
    if (tex & 1) {
        const uint32_t tmp = u; /* This texel carries V, its partner U. */

        u = w;
        w = tmp;
    }
    return y | (u << 8) | (w << 16) | 0xff000000;
}

static uint8_t
p2_clamp8(int v)
{
    return (v < 0) ? 0 : ((v > 255) ? 255 : v);
}

/* ITU-R BT.601 (CCIR 601) YCbCr to RGB, Y 16-235, in 16.16 fixed point. */
static uint32_t
p2_yuv_to_rgb(uint32_t c)
{
    const int y = ((int) (c & 0xff) - 16) * 76284;
    const int u = (int) ((c >> 8) & 0xff) - 128;
    const int v = (int) ((c >> 16) & 0xff) - 128;
    const int r = (y + 104595 * v + 32768) >> 16;
    const int g = (y - 25690 * u - 53281 * v + 32768) >> 16;
    const int b = (y + 132186 * u + 32768) >> 16;

    return p2_clamp8(r) | (p2_clamp8(g) << 8) | (p2_clamp8(b) << 16) | (c & 0xff000000);
}

/* Each byte of a colour within ChromaLowerBound..ChromaUpperBound. */
static int
p2_chroma_in_bounds(p2_t *p2, uint32_t c)
{
    const uint32_t lo = p2->gp[TAG_CHROMA_LOWER];
    const uint32_t hi = p2->gp[TAG_CHROMA_UPPER];

    for (int i = 0; i < 32; i += 8) {
        const uint32_t b = (c >> i) & 0xff;

        if ((b < ((lo >> i) & 0xff)) || (b > ((hi >> i) & 0xff)))
            return 0;
    }
    return 1;
}

/* YUVMode: bit 0 converts the texel to RGB; bits 2:1 test it against the
   chroma bounds (1 passes texels inside, 2 rejects them -- the source colour
   key); bit 3 tests the converted colour rather than the texel; bit 5 keeps
   the texel out of the fragment's colour, which then comes from the colour
   DDA or the framebuffer source as without texturing (the 3Dlabs driver's
   overlay emulation: it marks the destination colour key's pixels with the
   5551 alpha bit, then copies the converted overlay onto the marked ones).
   Returns 0 when the fragment is rejected. */
static int
p2_yuv_unit(p2_t *p2, uint32_t *color)
{
    const uint32_t mode = p2->gp[TAG_YUV_MODE];
    const uint32_t in   = *color;
    int            pass = 1;

    if (mode & 1)
        *color = p2_yuv_to_rgb(in);
    switch ((mode >> 1) & 3) {
        case 1:
            pass = p2_chroma_in_bounds(p2, (mode & 8) ? *color : in);
            break;
        case 2:
            pass = !p2_chroma_in_bounds(p2, (mode & 8) ? *color : in);
            break;
        default:
            break;
    }
    return pass;
}

static int
p2_texel_test_only(p2_t *p2)
{
    return !!(p2->gp[TAG_YUV_MODE] & 0x20);
}

/* A texel for texture copies: nearest sampling of S/T (integer at bit 20),
   stepped per pixel (d?dx) and per scanline (d?dyDom). Returns an internal
   AABBGGRR colour (*raw clear) or, for indexes without the LUT, the index
   itself (*raw set). */
static uint32_t
p2_texture_color(p2_t *p2, int *raw)
{
    p2_raster_t   *r    = &p2->raster;
    const uint32_t trm  = p2->gp[TAG_TEXTURE_READ_MODE];
    const uint32_t tmf  = p2->gp[TAG_TEXTURE_MAP_FORMAT];
    const uint32_t tdf  = p2->gp[TAG_TEXTURE_DATA_FORMAT];
    const uint32_t lutm = p2->gp[TAG_TEXEL_LUT_MODE];
    const int      fmt  = (tdf & 15) | ((tdf >> 2) & 16);
    const int64_t  ss   = (int64_t) p2_sext(p2->gp[TAG_S_START], 32) + (int64_t) r->col * p2_sext(p2->gp[TAG_DS_DX], 32) +
                       (int64_t) r->row * p2_sext(p2->gp[TAG_DS_DY_DOM], 32);
    const int64_t  tt   = (int64_t) p2_sext(p2->gp[TAG_T_START], 32) + (int64_t) r->col * p2_sext(p2->gp[TAG_DT_DX], 32) +
                       (int64_t) r->row * p2_sext(p2->gp[TAG_DT_DY_DOM], 32);
    const int64_t  s    = p2_tex_wrap(p2_fixed_int(ss / 16), (trm >> 9) & 15, (trm >> 1) & 3);
    const int64_t  t    = p2_tex_wrap(p2_fixed_int(tt / 16), (trm >> 13) & 15, (trm >> 3) & 3);
    const int64_t  tex  = (int64_t) (p2->gp[TAG_TEXTURE_BASE] & 0xffffff) + t * p2_pitch(tmf) + s;
    uint32_t       v;
    uint32_t       rr;
    uint32_t       gg;
    uint32_t       bb;
    uint32_t       aa = 0xff;

    *raw = 0;
    if ((lutm & 3) == 3) {
        /* LUT DirectIndex (PRM 5.8.6): an 8x8 pattern by the fragment's
           position, no texture read. The 2D drivers load a colour brush as
           framebuffer pixels, PixelsPerEntry of them in an entry (16 bpp:
           two), and fill packed rectangles with it; each pixel takes its
           part of the entry. */
        const int ppe = (lutm >> 10) & 3;
        const int sub = r->frag_x & ((1 << ppe) - 1);

        v = (uint32_t) (((r->frag_y & 7) * (8 >> ppe)) + ((r->frag_x & 7) >> ppe));
        v = (v + ((lutm >> 2) & 0xff)) & 0xff;
        v = (v < 16) ? p2->gp[TAG_TEXEL_LUT0 + v] : p2->texel_lut[v];
        if (ppe)
            v >>= sub * (32 >> ppe);
        *raw = 1;
        return v;
    }
    switch ((tmf >> 19) & 7) {
        case 1:
            v = p2_fb_read(p2, tex * 2, 2);
            break;
        case 2:
            v = p2_fb_read(p2, tex * 4, 4);
            break;
        case 3: /* 4 bits a texel, the low nibble first. */
            v = (p2_fb_read(p2, tex >> 1, 1) >> ((tex & 1) << 2)) & 15;
            break;
        case 4:
            v = p2_fb_read(p2, tex * 3, 3);
            break;
        default:
            v = p2_fb_read(p2, tex, 1);
            break;
    }

    if ((fmt == TEXFMT_YUV444) || (fmt == TEXFMT_YUV422))
        return p2_texel_yuv(p2, fmt, tex, v, !!(tdf & 0x20));
    if ((fmt == 14) || (fmt == 15)) {
        if (!(lutm & 1)) {
            *raw = 1;
            return v;
        }
        /* CI4 indexes count from TexelLUTIndex (without DirectIndex): the
           3Dlabs Win9x driver loads a 4-bit DIB's 16 colours at 80h and
           sets TexelLUTIndex back to 80h before each CI4 draw, LUTOffset 0;
           its CI8 draws leave TexelLUTIndex wherever the last load left it. */
        if ((fmt == 15) && !(lutm & 2))
            v += p2->gp[TAG_TEXEL_LUT_INDEX];
        v = (v + ((lutm >> 2) & 0xff)) & 0xff;
        return (v < 16) ? p2->gp[TAG_TEXEL_LUT0 + v] : p2->texel_lut[v];
    }

    switch (fmt) {
        case 1: /* 5551 */
            rr = (v & 0x1f) << 3;
            gg = ((v >> 5) & 0x1f) << 3;
            bb = ((v >> 10) & 0x1f) << 3;
            aa = (v & 0x8000) ? 0xff : 0;
            break;
        case 2: /* 4444 */
            rr = (v & 0xf) << 4;
            gg = ((v >> 4) & 0xf) << 4;
            bb = ((v >> 8) & 0xf) << 4;
            aa = ((v >> 12) & 0xf) << 4;
            break;
        case 16: /* 565 */
            rr = (v & 0x1f) << 3;
            gg = ((v >> 5) & 0x3f) << 2;
            bb = ((v >> 11) & 0x1f) << 3;
            break;
        default: /* 8888 */
            return v;
    }
    if (tdf & 0x20) {
        /* RGB order: red in the high bits. */
        const uint32_t tmp = rr;

        rr = bb;
        bb = tmp;
    }
    return rr | (gg << 8) | (bb << 16) | (aa << 24);
}

/* ------------------------------------------------------------------------- */
/* 3D units: texture address/read, texture application, fog, stencil/depth  */
/* (localbuffer) and alpha blending (Permedia 2 Programmer's Reference      */
/* Manual, chapter 5, and its register reference).                         */
/* ------------------------------------------------------------------------- */

static uint32_t
p2_expand(uint32_t v, int bits, int shift)
{
    if (!bits)
        return 0;
    v &= (1u << bits) - 1;
    if (shift)
        return v << (8 - bits);
    return (v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1);
}

/* A packed pixel to R, G, B, A (0-255). Formats without alpha read 0xFF. */
static void
p2_unpack(uint32_t v, int fmt, int rgb, int shift_color, int shift_alpha, uint32_t c[4])
{
    const p2_color_fmt_t *f = &p2_color_fmts[fmt & 31];
    int                   pos;

    if (!f->r) {
        /* 8888 (and anything unknown). */
        c[rgb ? 2 : 0] = v & 0xff;
        c[1]           = (v >> 8) & 0xff;
        c[rgb ? 0 : 2] = (v >> 16) & 0xff;
        c[3]           = v >> 24;
        return;
    }
    v >>= f->base;
    pos = 0;
    if (rgb) {
        c[2] = p2_expand(v, f->b, shift_color);
        pos += f->b;
        c[1] = p2_expand(v >> pos, f->g, shift_color);
        pos += f->g;
        c[0] = p2_expand(v >> pos, f->r, shift_color);
        pos += f->r;
    } else {
        c[0] = p2_expand(v, f->r, shift_color);
        pos += f->r;
        c[1] = p2_expand(v >> pos, f->g, shift_color);
        pos += f->g;
        c[2] = p2_expand(v >> pos, f->b, shift_color);
        pos += f->b;
    }
    c[3] = f->a ? p2_expand(v >> pos, f->a, shift_alpha) : 0xff;
}

static uint32_t
p2_pack_abgr(const uint32_t c[4])
{
    return c[0] | (c[1] << 8) | (c[2] << 16) | (c[3] << 24);
}

static void
p2_unpack_abgr(uint32_t v, uint32_t c[4])
{
    for (int i = 0; i < 4; i++)
        c[i] = (v >> (i * 8)) & 0xff;
}

static uint32_t
p2_clamp255(int64_t v)
{
    return (uint32_t) MAX(0, MIN(255, v));
}

/* Texture memory: in the framebuffer, or (TextureBaseAddress bit 30) in
   host memory, read across the bus. */
static uint32_t
p2_tex_read(p2_t *p2, int64_t addr, int bytes)
{
    uint32_t v = 0;

    if (p2->gp[TAG_TEXTURE_BASE] & (1 << 30)) {
        for (int i = 0; i < bytes; i++)
            v |= (uint32_t) mem_readb_phys((uint32_t) (p2->agp_tex_base + addr + i)) << (i * 8);
        return v;
    }
    return p2_fb_read(p2, addr, bytes);
}

/* The YUV unit's conversion on unpacked R/G/B/A (Y, U, V in R, G, B). */
static void
p2_yuv_to_rgb_c(uint32_t c[4])
{
    p2_unpack_abgr(p2_yuv_to_rgb(p2_pack_abgr(c)), c);
}

static int
p2_in_bounds(const uint32_t c[4], uint32_t lower, uint32_t upper)
{
    for (int i = 0; i < 4; i++) {
        const uint32_t lo = (lower >> (i * 8)) & 0xff;
        const uint32_t hi = (upper >> (i * 8)) & 0xff;

        if ((c[i] < lo) || (c[i] > hi))
            return 0;
    }
    return 1;
}

/* One texel at integer S, T after the wrap modes; 0 if the alpha map
   rejected it. */
static int
p2_texel(p2_t *p2, int64_t s, int64_t t, uint32_t c[4])
{
    const uint32_t trm  = p2->gp[TAG_TEXTURE_READ_MODE];
    const uint32_t tmf  = p2->gp[TAG_TEXTURE_MAP_FORMAT];
    const uint32_t tdf  = p2->gp[TAG_TEXTURE_DATA_FORMAT];
    const uint32_t lutm = p2->gp[TAG_TEXEL_LUT_MODE];
    const int      fmt  = (tdf & 15) | ((tdf >> 2) & 16);
    const int      rgb  = !!(tdf & 0x20);
    int64_t        tex;
    uint32_t       v;

    s   = p2_tex_wrap(s, (trm >> 9) & 15, (trm >> 1) & 3);
    t   = p2_tex_wrap(t, (trm >> 13) & 15, (trm >> 3) & 3);
    tex = (int64_t) (p2->gp[TAG_TEXTURE_BASE] & 0xffffff) + t * p2_pitch(tmf) * ((tmf & (1 << 16)) ? -1 : 1) + s;

    switch ((tmf >> 19) & 7) {
        case 1:
            v = p2_tex_read(p2, tex * 2, 2);
            break;
        case 2:
            v = p2_tex_read(p2, tex * 4, 4);
            break;
        case 3:
            v = (p2_tex_read(p2, tex >> 1, 1) >> ((tex & 1) << 2)) & 15;
            break;
        case 4:
            v = p2_tex_read(p2, tex * 3, 3);
            break;
        default:
            v = p2_tex_read(p2, tex, 1);
            break;
    }

    if ((fmt == 14) || (fmt == 15)) {
        /* Colour index through the texel LUT (entries are BGR 888, no alpha:
           the texel is opaque). */
        if (lutm & 1) {
            if (lutm & 2) {
                /* DirectIndex: an 8x8 pattern by the fragment's position,
                   PixelsPerEntry pixels to an entry (PRM 5.8.6). */
                const int ppe = (lutm >> 10) & 3;

                v = (uint32_t) (((p2->raster.frag_y & 7) * (8 >> ppe)) + ((p2->raster.frag_x & 7) >> ppe));
            }
            else if (fmt == 15) /* CI4 counts from TexelLUTIndex (see p2_texture_color). */
                v += p2->gp[TAG_TEXEL_LUT_INDEX];
            v = (v + ((lutm >> 2) & 0xff)) & 0xff;
            v = (v < 16) ? p2->gp[TAG_TEXEL_LUT0 + v] : p2->texel_lut[v];
        } else
            v = v | (v << 8) | (v << 16);
        p2_unpack(v | 0xff000000, 0, 0, 0, 0, c);
    } else if ((fmt == 18) || (fmt == 19)) {
        /* YUV, left unconverted here (Y, U, V in R, G, B): the YUV unit
           converts after filtering. */
        if (fmt == 19) {
            /* 4:2:2: 16-bit texels of luma and one chroma sample, U in the
               even texel of a pair and V in the odd; ColorOrder swaps bytes. */
            const uint32_t w   = p2_tex_read(p2, (tex ^ 1) * 2, 2);
            const uint32_t own = rgb ? (v & 0xff) : ((v >> 8) & 0xff);
            const uint32_t oth = rgb ? (w & 0xff) : ((w >> 8) & 0xff);

            c[0] = rgb ? ((v >> 8) & 0xff) : (v & 0xff);
            c[1] = (tex & 1) ? oth : own;
            c[2] = (tex & 1) ? own : oth;
            c[3] = 0xff;
        } else
            p2_unpack(v, 0, rgb, 0, 0, c);
    } else
        p2_unpack(v, fmt, rgb, 0, 0, c);

    if (tdf & 0x10)
        c[3] = 0xff; /* No alpha buffer. */

    if (tdf & 0x180) {
        /* Alpha map (PRM 5.8.8), a colour key before filtering: Include
           passes texels inside the bounds, Exclude fails them. A failed
           texel's alpha becomes 0 and its colour takes no part in the
           filter; a passed one keeps its alpha. */
        const int in = p2_in_bounds(c, p2->gp[TAG_ALPHA_MAP_LOWER], p2->gp[TAG_ALPHA_MAP_UPPER]);

        if (((tdf >> 7) & 3) == 1 ? !in : in) {
            c[3] = 0;
            return 0;
        }
    }
    return 1;
}

/* The texture colour for the current fragment; 0 if the chroma test drops
   the fragment, 2 if it leaves the fragment untextured. */
static int
p2_texture_3d(p2_t *p2, int64_t ss, int64_t tt, int64_t qq, uint32_t c[4])
{
    const uint32_t trm = p2->gp[TAG_TEXTURE_READ_MODE];
    const uint32_t yuv = p2->gp[TAG_YUV_MODE];
    double         s;
    double         t;

    if (p2->gp[TAG_TEXTURE_ADDR_MODE] & 2) {
        /* Perspective correction: S/Q, T/Q. */
        if (!qq)
            qq = 1;
        s = (double) ss * 1024.0 / (double) qq;
        t = (double) tt * 1024.0 / (double) qq;
    } else {
        s = (double) ss / 1048576.0;
        t = (double) tt / 1048576.0;
    }
    s = MAX(-1048576.0, MIN(1048576.0, s));
    t = MAX(-1048576.0, MIN(1048576.0, t));

    if (trm & (1 << 17)) {
        /* Bilinear: the four texels around the sample, by 8-bit weights. */
        const double  fs = s - 0.5;
        const double  ft = t - 0.5;
        const int64_t s0 = (int64_t) floor(fs);
        const int64_t t0 = (int64_t) floor(ft);
        const int     ws = (int) ((fs - (double) s0) * 256.0);
        const int     wt = (int) ((ft - (double) t0) * 256.0);
        uint32_t      c00[4];
        uint32_t      c10[4];
        uint32_t      c01[4];
        uint32_t      c11[4];

        const int     k00 = p2_texel(p2, s0, t0, c00);
        const int     k10 = p2_texel(p2, s0 + 1, t0, c10);
        const int     k01 = p2_texel(p2, s0, t0 + 1, c01);
        const int     k11 = p2_texel(p2, s0 + 1, t0 + 1, c11);
        const int     w00 = (256 - ws) * (256 - wt);
        const int     w10 = ws * (256 - wt);
        const int     w01 = (256 - ws) * wt;
        const int     w11 = ws * wt;
        /* Colour over the texels the alpha map passed, alpha over all four. */
        const int     wc  = k00 * w00 + k10 * w10 + k01 * w01 + k11 * w11;

        for (int i = 0; i < 3; i++)
            c[i] = wc ? (uint32_t) (((int64_t) c00[i] * k00 * w00 + (int64_t) c10[i] * k10 * w10 +
                                     (int64_t) c01[i] * k01 * w01 + (int64_t) c11[i] * k11 * w11) / wc)
                      : 0;
        c[3] = (uint32_t) ((c00[3] * w00 + c10[3] * w10 + c01[3] * w01 + c11[3] * w11) >> 16);
    } else
        p2_texel(p2, (int64_t) floor(s), (int64_t) floor(t), c);

    {
        /* YUV unit: BT.601 conversion, then the chroma test on the texel
           (TestData: on the converted colour). */
        const int fmt = (p2->gp[TAG_TEXTURE_DATA_FORMAT] & 15) | ((p2->gp[TAG_TEXTURE_DATA_FORMAT] >> 2) & 16);
        uint32_t  pre[4];

        memcpy(pre, c, sizeof(pre));
        if ((yuv & 1) && ((fmt == 18) || (fmt == 19)))
            p2_yuv_to_rgb_c(c);
        if (yuv & 6) {
            const uint32_t *k    = (yuv & 8) ? c : pre;
            const int       in   = p2_in_bounds(k, p2->gp[TAG_CHROMA_LOWER], p2->gp[TAG_CHROMA_UPPER]);
            const int       pass = (((yuv >> 1) & 3) == 1) ? in : !in;

            if (!pass)
                return (yuv & 0x10) ? 2 : 0;
        }
        if (yuv & 0x20)
            return 2; /* TexelDisableUpdate: the texel is only tested. */
    }
    return 1;
}

/* Texture application (TextureColorMode); k[] = Ks, Kd in 0..2 (x256). */
static void
p2_apply_texture(p2_t *p2, uint32_t f[4], const uint32_t t[4], int ks, int kd)
{
    const uint32_t tcm = p2->gp[TAG_TEXTURE_COLOR_MODE];
    const int      mode = (tcm >> 1) & 7;

    if (tcm & 0x10) {
        /* Ramp (QuickDraw 3D): decal, then modulate by Kd, then highlight Ks. */
        if (mode & 1) {
            for (int i = 0; i < 3; i++)
                f[i] = (t[i] * t[3] + f[i] * (255 - t[3])) / 255;
        } else {
            for (int i = 0; i < 3; i++)
                f[i] = t[i];
            f[3] = t[3] * f[3] / 255;
        }
        if (mode & 2) {
            for (int i = 0; i < 4; i++)
                f[i] = p2_clamp255(((int64_t) f[i] * kd) >> 8);
        }
        if (mode & 4) {
            for (int i = 0; i < 4; i++)
                f[i] = p2_clamp255(f[i] + ((ks * 255) >> 8));
        }
        return;
    }

    switch (mode & 3) {
        case 0: /* Modulate. */
            for (int i = 0; i < 4; i++)
                f[i] = (t[i] * f[i] + 127) / 255;
            break;
        case 1: /* Decal. */
            for (int i = 0; i < 3; i++)
                f[i] = (t[i] * t[3] + f[i] * (255 - t[3]) + 127) / 255;
            break;
        default: /* Copy. */
            for (int i = 0; i < 4; i++)
                f[i] = t[i];
            break;
    }
    if (mode & 4) {
        /* Highlight: add the specular term. */
        for (int i = 0; i < 3; i++)
            f[i] = p2_clamp255(f[i] + ((ks * 255) >> 8));
    }
}

static int
p2_compare(int func, uint32_t frag, uint32_t src)
{
    switch (func & 7) {
        case 0:
            return 0;
        case 1:
            return frag < src;
        case 2:
            return frag == src;
        case 3:
            return frag <= src;
        case 4:
            return frag > src;
        case 5:
            return frag != src;
        case 6:
            return frag >= src;
        default:
            return 1;
    }
}

static uint32_t
p2_stencil_op(int op, uint32_t s, uint32_t ref, uint32_t max)
{
    switch (op & 7) {
        case 1:
            return 0;
        case 2:
            return ref;
        case 3:
            return (s < max) ? s + 1 : max;
        case 4:
            return s ? s - 1 : 0;
        case 5:
            return ~s & max;
        default:
            return s;
    }
}

/* Stencil and depth tests against the localbuffer (16-bit pixels: 16 or 15
   bits of depth, the top bit stencil with 15) and its update. Returns 0 if
   the fragment is dropped. */
static int
p2_lb_fragment(p2_t *p2, int x, int y, int64_t z)
{
    const uint32_t dm    = p2->gp[TAG_DEPTH_MODE];
    const uint32_t sm    = p2->gp[TAG_STENCIL_MODE];
    const uint32_t win   = p2->gp[TAG_WINDOW];
    const uint32_t lrm   = p2->gp[TAG_LB_READ_MODE];
    const uint32_t rfmt  = p2->gp[TAG_LB_READ_FORMAT];
    const uint32_t wfmt  = p2->gp[TAG_LB_WRITE_FORMAT];
    const uint32_t sd    = p2->gp[TAG_STENCIL_DATA];
    const int      depth = dm & 1;
    const int      sten  = sm & 1;
    const int      force = !!(win & 8);
    const uint32_t rdmax = ((rfmt & 3) == 3) ? 0x7fff : 0xffff;
    const uint32_t wdmax = ((wfmt & 3) == 3) ? 0x7fff : 0xffff;
    const int      wsten = (((wfmt >> 2) & 3) == 3) && (wdmax == 0x7fff);
    const int64_t  addr  = ((int64_t) (p2->gp[TAG_LB_WINDOW_BASE] & 0xffffff) +
                          (int64_t) y * p2_pitch(lrm) * ((lrm & (1 << 18)) ? -1 : 1) + x) * 2;
    uint32_t       old;
    uint32_t       src_z;
    uint32_t       src_s;
    uint32_t       frag_z;
    uint32_t       new_z;
    uint32_t       new_s;
    int            spass = 1;
    int            zpass = 1;

    if (!depth && !sten && !force)
        return 1;

    old   = p2_fb_read(p2, addr, 2);
    src_z = old & rdmax;
    src_s = (((rfmt >> 2) & 3) == 3) && (rdmax == 0x7fff) ? (old >> 15) & 1 : 0;

    switch ((dm >> 2) & 3) {
        case 1: /* LBData (copies). */
        case 3: /* LBSourceData. */
            frag_z = p2_fb_read(p2, addr + (int64_t) p2_sext(p2->gp[TAG_LB_SOURCE_OFFSET], 24) * 2, 2) & rdmax;
            break;
        case 2: /* The Depth register. */
            frag_z = p2->gp[TAG_DEPTH] & wdmax;
            break;
        default:
            z      = z >> 11;
            frag_z = (uint32_t) MAX(0, MIN((int64_t) wdmax, z));
            break;
    }

    if (sten) {
        const uint32_t mask = (sd >> 8) & 0xff;

        spass = p2_compare(sm >> 10, sd & mask, src_s & mask);
    }
    if (depth && spass)
        zpass = p2_compare(dm >> 4, frag_z, src_z);

    new_z = src_z;
    new_s = src_s;
    if (sten) {
        const int op = !spass ? (sm >> 7) : (!zpass ? (sm >> 4) : (sm >> 1));
        uint32_t  ws;

        /* The stencil source: the update method's result, the Stencil
           register, or the source pixel's stencil (LBData / LBSourceData). */
        switch ((sm >> 13) & 3) {
            case 1:
                ws = p2->gp[TAG_STENCIL] & 1;
                break;
            case 2:
            case 3:
                ws = (p2_fb_read(p2, addr + (int64_t) p2_sext(p2->gp[TAG_LB_SOURCE_OFFSET], 24) * 2, 2) >> 15) & 1;
                break;
            default:
                ws = p2_stencil_op(op, src_s, sd & 1, 1);
                break;
        }

        new_s = (ws & (sd >> 16) & 1) | (src_s & ~(sd >> 16) & 1);
    }
    if (depth && spass && zpass && (dm & 2))
        new_z = frag_z;
    if (force) {
        /* Forced update: from the Depth/Stencil registers (clears), or the
           source pixel's raw word (localbuffer copies, LBSourceOffset). */
        if (win & 0x10) {
            new_z = p2->gp[TAG_DEPTH] & wdmax;
            new_s = p2->gp[TAG_STENCIL] & 1;
        } else {
            const uint32_t src = p2_fb_read(p2, addr + (int64_t) p2_sext(p2->gp[TAG_LB_SOURCE_OFFSET], 24) * 2, 2);

            if (p2->gp[TAG_LB_WRITE_MODE] & 1)
                p2_fb_write(p2, addr, 2, src, 0xffffffff);
            return 1;
        }
    }

    if ((p2->gp[TAG_LB_WRITE_MODE] & 1) && !(win & (1 << 18)) &&
        (force || (new_z != src_z) || (new_s != src_s))) {
        const uint32_t v = (new_z & wdmax) | (wsten ? (new_s << 15) : (old & ~wdmax & 0xffff));

        p2_fb_write(p2, addr, 2, v, 0xffffffff);
    }

    return force || (spass && zpass);
}

/* A blend factor (0-256) for one channel. */
static int
p2_blend_factor(int code, int dst_side, const uint32_t s[4], const uint32_t d[4], int i)
{
    const uint32_t sa = s[3] + (s[3] >> 7);
    const uint32_t da = d[3] + (d[3] >> 7);

    switch (code) {
        case 0:
            return 0;
        case 1:
            return 256;
        case 2: /* DST_COLOR for the source, SRC_COLOR for the destination. */
            return dst_side ? (s[i] + (s[i] >> 7)) : (d[i] + (d[i] >> 7));
        case 3:
            return 256 - (dst_side ? (s[i] + (s[i] >> 7)) : (d[i] + (d[i] >> 7)));
        case 4:
            return sa;
        case 5:
            return 256 - sa;
        case 6:
            return da;
        case 7:
            return 256 - da;
        default: /* SRC_ALPHA_SATURATE. */
            return (i == 3) ? 256 : MIN(sa, 256 - da);
    }
}

static int64_t
p2_span_error(const p2_raster_t *r)
{
    /* From the dominant edge (the Delta adds a half pixel less a bit) to the
       centre of the span's first fragment, in the direction of the span. */
    const int     fwd   = r->xdom <= r->xsub;
    const int64_t first = p2_fixed_int(r->xdom) - (fwd ? 0 : 1);
    const int64_t e     = first * 65536 + 65535 - r->xdom;

    return fwd ? e : -e;
}

static int
p2_stipple_pass(p2_t *p2, int x, int y)
{
    const uint32_t am = p2->gp[TAG_AREA_STIPPLE_MODE];
    uint32_t       ax;
    uint32_t       ay;
    int            bit;

    if (!(p2->raster.command & RENDER_AREA_STIPPLE) || !(am & 1))
        return 1;
    ax = (x + (am >> 7)) & 7;
    ay = (y + (am >> 12)) & 7;
    if (am & (1 << 18))
        ax = 7 - ax;
    if (am & (1 << 19))
        ay = 7 - ay;
    bit = (p2->gp[TAG_AREA_STIPPLE_PAT + ay] >> ax) & 1;
    if (am & (1 << 17))
        bit = !bit;
    return bit;
}

/* One fragment of a 3D primitive through every unit to the framebuffer. */
static void
p2_fragment_3d(p2_t *p2, int x, int y)
{
    p2_raster_t   *r     = &p2->raster;
    const uint32_t cmd   = r->command;
    const int      span  = (r->prim == PRIM_TRAP) || (r->prim == PRIM_RECT);
    const int64_t  col   = span ? (int64_t) r->col : 0;
    const int64_t  err   = ((r->prim == PRIM_TRAP) && (cmd & RENDER_SUBPIXEL)) ? p2_span_error(r) : 0;
    const uint32_t cdm   = p2->gp[TAG_COLOR_DDA_MODE];
    const uint32_t abm   = p2->gp[TAG_ALPHA_BLEND_MODE];
    const uint32_t wc    = p2->gp[TAG_FB_WRITE_CONFIG];
    const uint32_t lop   = p2->gp[TAG_LOGICAL_OP_MODE];
    const int      bpp   = r->bpp;
    uint32_t       f[4];
    uint32_t       t[4];
    int64_t        addr;
    uint32_t       old;
    uint32_t       value;
    int            ks = 0;
    int            kd = 256;

#define P2_AT(v, d) ((v) + col * (d) + ((err * (d)) >> 16))

    if (p2_clipped(p2, x, y, 0) || !p2_stipple_pass(p2, x, y))
        return;
    r->frag_x = x;
    r->frag_y = y;

    /* Colour DDA. */
    if (r->shade) {
        for (int i = 0; i < 3; i++)
            f[i] = p2_clamp255(P2_AT(r->color[i], r->dcolor_dx[i]) / 2048);
        f[3] = r->alpha;
    } else
        p2_unpack_abgr((cdm & 1) ? p2->gp[TAG_CONSTANT_COLOR] : p2->gp[TAG_COLOR], f);

    /* Specular and diffuse terms for the texture unit: DDAs or the start registers. */
    {
        const uint32_t tcm = p2->gp[TAG_TEXTURE_COLOR_MODE];

        ks = (int) (((tcm & 0x40) ? P2_AT(r->k[1], r->dk_dx[1]) : p2_sext(p2->gp[TAG_KS_START] >> 6, 18)) >> 8);
        kd = (int) (((tcm & 0x20) ? P2_AT(r->k[2], r->dk_dx[2]) : p2_sext(p2->gp[TAG_KD_START] >> 6, 18)) >> 8);
        ks = MAX(0, MIN(512, ks));
        kd = MAX(0, MIN(512, kd));
    }

    /* Texture. */
    if ((cmd & RENDER_TEXTURE) && (p2->gp[TAG_TEXTURE_READ_MODE] & 1)) {
        const int res = p2_texture_3d(p2, P2_AT(r->tex[0], r->dtex_dx[0]), P2_AT(r->tex[1], r->dtex_dx[1]),
                                      P2_AT(r->tex[2], r->dtex_dx[2]), t);

        if (!res)
            return;
        if ((res == 1) && (p2->gp[TAG_TEXTURE_COLOR_MODE] & 1))
            p2_apply_texture(p2, f, t, ks, kd);
    }

    /* Fog: C = f * C + (1 - f) * FogColor. */
    if ((cmd & RENDER_FOG) && (p2->gp[TAG_FOG_MODE] & 1)) {
        const int64_t fog = P2_AT(r->k[0], r->dk_dx[0]);
        const int     fi  = (int) MAX(0, MIN(65536, fog)) >> 8;
        uint32_t      fc[4];

        if ((p2->gp[TAG_FOG_MODE] & 4) && (fog < 0))
            return;
        p2_unpack_abgr(p2->gp[TAG_FOG_COLOR], fc);
        for (int i = 0; i < 3; i++)
            f[i] = (f[i] * fi + fc[i] * (256 - fi)) >> 8;
    }

    /* Stencil and depth. */
    if (!p2_lb_fragment(p2, x, y, P2_AT(r->z, r->dz_dx)))
        return;

#undef P2_AT

    if (!(p2->gp[TAG_FB_WRITE_MODE] & 1))
        return;

    addr = ((int64_t) p2->gp[TAG_FB_WINDOW_BASE] + p2->gp[TAG_FB_PIXEL_OFFSET] + x +
            (int64_t) y * p2_pitch(wc) * ((wc & FBRM_ORIGIN_BOT) ? -1 : 1)) * bpp;
    old  = p2_fb_read(p2, addr, bpp);

    if ((abm & 1) && (((abm >> 1) & 0x7f) != 16)) {
        /* Alpha blend with the framebuffer pixel in AlphaBlendMode's format. */
        const int fmt  = ((abm >> 8) & 15) | ((abm >> 12) & 16);
        const int sfac = (abm >> 1) & 15;
        const int dfac = (abm >> 5) & 7;
        uint32_t  d[4];
        uint32_t  o[4];

        if (fmt == 14)
            p2_unpack_abgr(old * 0x01010101, d);
        else
            p2_unpack(old, fmt, !!(abm & (1 << 13)), !!(abm & (1 << 17)), !!(abm & (1 << 18)), d);
        if (abm & (1 << 12))
            d[3] = 0xff;
        for (int i = 0; i < 4; i++)
            o[i] = p2_clamp255(((int64_t) f[i] * p2_blend_factor(sfac, 0, f, d, i) +
                                (int64_t) d[i] * p2_blend_factor(dfac, 1, f, d, i)) >> 8);
        memcpy(f, o, sizeof(o));
    }

    value = p2_format_color(p2, p2_pack_abgr(f), x, y);
    {
        const uint32_t dst = (p2->gp[TAG_FB_READ_MODE] & FBRM_DST_ENABLE) ? old : 0;
        const uint32_t swm = p2->gp[TAG_FB_SOFT_WRITE_MASK];

        if (lop & 1)
            value = p2_logic_op(lop >> 1, value, dst);
        value = (value & swm) | (old & ~swm);
    }
    p2_fb_write(p2, addr, bpp, value, p2->gp[TAG_FB_HW_WRITE_MASK]);
}

/* Whether a primitive needs the 3D fragment path rather than the 2D one. */
static int
p2_need_3d(p2_t *p2, uint32_t cmd)
{
    const uint32_t abm = p2->gp[TAG_ALPHA_BLEND_MODE];

    if (cmd & (RENDER_FAST_FILL | RENDER_SYNC_BITMASK | RENDER_SYNC_HOSTDATA))
        return 0;
    if (p2_upload_cmd(p2, cmd) || p2_lb_upload_cmd(p2, cmd))
        return 0;
    if (p2->gp[TAG_YUV_MODE] & 0x20)
        return 0; /* Texel-test-only copies (overlay emulation) read the framebuffer source. */
    if ((p2->gp[TAG_DEPTH_MODE] & 1) || (p2->gp[TAG_STENCIL_MODE] & 1) ||
        ((p2->gp[TAG_LB_WRITE_MODE] & 1) && (p2->gp[TAG_WINDOW] & 8)))
        return 1;
    if ((abm & 1) && (((abm >> 1) & 0x7f) != 16))
        return 1;
    if ((cmd & RENDER_FOG) && (p2->gp[TAG_FOG_MODE] & 1))
        return 1;
    if (cmd & RENDER_TEXTURE) {
        const uint32_t tcm = p2->gp[TAG_TEXTURE_COLOR_MODE];

        /* YUV conversion and the chroma test alone stay on the 2D path (the
           drivers' video blits and overlay colour keys). */
        if ((p2->gp[TAG_TEXTURE_ADDR_MODE] & 2) || (p2->gp[TAG_TEXTURE_READ_MODE] & (1 << 17)) ||
            (p2->gp[TAG_TEXTURE_DATA_FORMAT] & 0x180))
            return 1;
        if ((tcm & 1) && ((tcm & 0x1e) != 6))
            return 1;
    }
    return 0;
}

/* A fragment's localbuffer word (16-bit pixels from LBWindowBase). */
static int64_t
p2_lb_addr(p2_t *p2, int x, int y)
{
    const uint32_t lrm = p2->gp[TAG_LB_READ_MODE];

    return ((int64_t) (p2->gp[TAG_LB_WINDOW_BASE] & 0xffffff) +
            (int64_t) y * p2_pitch(lrm) * ((lrm & (1 << 18)) ? -1 : 1) + x) * 2;
}

/* Localbuffer uploads: LBReadMode DataType stencil (1) or depth (2) with
   destination reads. */
static int
p2_lb_upload_cmd(p2_t *p2, uint32_t command)
{
    const uint32_t lrm = p2->gp[TAG_LB_READ_MODE];

    return !(command & RENDER_FAST_FILL) && (lrm & (1 << 10)) && ((((lrm >> 16) & 3) == 1) || (((lrm >> 16) & 3) == 2));
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
    if (mask && !host && (cmd & RENDER_REUSE_BITMASK) && !r->payload_left) {
        /* ReuseBitMask: an expired mask starts over instead of waiting. */
        r->payload      = p2->gp[TAG_BIT_MASK_PATTERN];
        r->payload_left = 32;
    }
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
    r->frag_x = x;
    r->frag_y = y;

    if (p2_lb_upload_cmd(p2, cmd)) {
        /* Localbuffer reads to the output FIFO (LBReadMode DataType: depth or
           stencil; FilterMode bits 4/5, 6/7 pass tag and data). */
        if (p2->out_count >= P2_OUT_STALL) {
            r->out_wait = 1;
            return 0;
        }
        if (!p2_clipped(p2, x, y, 0)) {
            const uint32_t v       = p2_fb_read(p2, p2_lb_addr(p2, x, y), 2);
            const int      stencil = (((p2->gp[TAG_LB_READ_MODE] >> 16) & 3) == 1);
            const int      filter  = (p2->gp[TAG_FILTER_MODE] >> (stencil ? 6 : 4)) & 3;
            const uint32_t dmask   = ((p2->gp[TAG_LB_READ_FORMAT] & 3) == 3) ? 0x7fff : 0xffff;

            if (filter & 1)
                p2_out_fifo_push(p2, stencil ? TAG_LB_STENCIL : TAG_LB_DEPTH);
            if (filter & 2)
                p2_out_fifo_push(p2, stencil ? ((v >> 15) & 1) : (v & dmask));
        }
        p2_next_fragment(p2);
        return 1;
    }

    if (p2_upload_cmd(p2, cmd)) {
        /* Framebuffer reads to the output FIFO (FilterMode bits 8/9); the
           rasterizer stalls while the host has not taken the words. */
        if (p2->out_count >= P2_OUT_STALL) {
            r->out_wait = 1;
            return 0;
        }
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

    if (r->use3d) {
        p2_fragment_3d(p2, x, y);
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
    } else if ((cmd & RENDER_TEXTURE) && !(cmd & RENDER_FAST_FILL) && !p2_texel_test_only(p2)) {
        color = p2_texture_color(p2, &raw);
        if (!raw)
            draw = p2_yuv_unit(p2, &color);
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
    } else if (host && ((r->payload_tag == TAG_DEPTH) || (r->payload_tag == TAG_STENCIL))) {
        /* Depth / Stencil from the host (the registers hold the value):
           through the stencil/depth unit to the localbuffer only. */
        if (!p2_clipped(p2, x, y, 0))
            p2_lb_fragment(p2, x, y, (int64_t) (p2->gp[TAG_DEPTH] & 0xffff) << 11);
        r->payload_left--;
        draw = 0;
    } else if (host && (r->payload_tag == TAG_TEXEL0)) {
        /* Texel0 from the host (procedural texture): the texel colour. */
        color = r->payload;
        raw   = 0;
        r->payload_left--;
    } else if (host && (r->payload_tag == TAG_LB_DATA)) {
        /* Localbuffer download: the raw depth/stencil word, no framebuffer write. */
        if ((p2->gp[TAG_LB_WRITE_MODE] & 1) && !p2_clipped(p2, x, y, 0))
            p2_fb_write(p2, p2_lb_addr(p2, x, y), 2, r->payload, 0xffffffff);
        r->payload_left--;
        draw = 0;
    } else if (host) {
        const uint32_t abm = p2->gp[TAG_ALPHA_BLEND_MODE];

        color = r->payload;
        raw   = (r->payload_tag != TAG_COLOR);
        if ((r->payload_tag == TAG_FB_DATA) && (abm & 1) && (((abm >> 1) & 0x7f) == 16) && !(rm & FBRM_PACKED)) {
            /* Image download with formatting: AlphaBlendMode's format to the
               internal colour, then DitherMode's (PRM 5.12.6). */
            const int fmt = ((abm >> 8) & 15) | ((abm >> 12) & 16);
            uint32_t  c[4];

            if (fmt == 14)
                p2_unpack_abgr((color & 0xff) * 0x01010101, c);
            else
                p2_unpack(color, fmt, !!(abm & (1 << 13)), !!(abm & (1 << 17)), !!(abm & (1 << 18)), c);
            color = p2_pack_abgr(c);
            raw   = 0;
        }
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
    } else if (!(cmd & RENDER_FAST_FILL) && (rm & FBRM_PACKED) && !r->shade && !(p2->gp[TAG_COLOR_DDA_MODE] & 1)) {
        /* Packed data with neither host data nor a source read is the
           FBSourceData register, a byte lane per pixel: the 3Dlabs Win9x
           driver's disabled (embossed) text XORs a rectangle with the
           colour it writes there. */
        const uint32_t lane = (uint32_t) ((p2->gp[TAG_FB_WINDOW_BASE] + p2->gp[TAG_FB_PIXEL_OFFSET] + x) * bpp) & 3;

        color = p2->gp[TAG_FB_SOURCE_DATA] >> (lane << 3);
        raw   = 1;
    }
    if ((cmd & RENDER_TEXTURE) && !(cmd & RENDER_FAST_FILL) && p2_texel_test_only(p2)) {
        int      texel_raw;
        uint32_t texel = p2_texture_color(p2, &texel_raw);

        if (!texel_raw)
            draw &= p2_yuv_unit(p2, &texel);
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

/* The host made room in the output FIFO: continue a stalled upload, then
   the writes held behind it. */
static void
p2_out_space(p2_t *p2)
{
    if (p2->in_resume)
        return;
    p2->in_resume = 1;
    if (p2->raster.active && p2->raster.out_wait && (p2->out_count < P2_OUT_STALL)) {
        p2->raster.out_wait = 0;
        p2_raster_run(p2);
    }
    while (p2->hold_count && !p2_gp_stalled(p2)) {
        const uint32_t tag = p2->hold_tag[p2->hold_rd];
        const uint32_t val = p2->hold_val[p2->hold_rd];

        p2->hold_rd = (p2->hold_rd + 1) & (P2_HOLD_QUEUE - 1);
        p2->hold_count--;
        p2_gp_write(p2, tag, val);
    }
    p2->in_resume = 0;
    p2_dma_run(p2);
}

static void
p2_raster_start(p2_t *p2, uint32_t command)
{
    p2_raster_t   *r    = &p2->raster;
    const uint32_t rm   = p2->gp[TAG_FB_READ_MODE];
    const int      bias = (p2->gp[TAG_RASTERIZER_MODE] >> 4) & 3;

    /* Up to rel_offset (sizeof() would include the struct's tail padding). */
    memset(r, 0, offsetof(p2_raster_t, rel_offset));
    r->command  = command;
    r->prim     = command & RENDER_PRIM_MASK;
    r->bpp      = p2_fb_bpp(p2);
    if (p2->gp[TAG_FB_WRITE_CONFIG] & (1 << 18)) {
        /* PatchEnable: a texture download through the framebuffer unit
           (the ICD's glTexImage) writes texels of the texture map's size,
           whatever FBReadPixel says (CI8 indices sent as 32-bit colours to
           a byte address). The patch permutation itself is not modelled:
           downloads and texture reads both stay linear. */
        static const int texel_bytes[8] = { 1, 2, 4, 1, 3, 4, 4, 4 };

        r->bpp = texel_bytes[(p2->gp[TAG_TEXTURE_MAP_FORMAT] >> 19) & 7];
    }
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

    r->use3d = p2_need_3d(p2, command);
    if (r->use3d && p2->mode_log) {
        /* Debug ("dev p2 modes"): each new combination of the 3D unit modes once. */
        const uint32_t m[9] = { command, p2->gp[TAG_TEXTURE_COLOR_MODE], p2->gp[TAG_ALPHA_BLEND_MODE], p2->gp[TAG_DEPTH_MODE],
                                p2->gp[TAG_TEXTURE_DATA_FORMAT], p2->gp[TAG_TEXTURE_READ_MODE], p2->gp[TAG_YUV_MODE],
                                p2->gp[TAG_FOG_MODE], p2->gp[TAG_STENCIL_MODE] };
        int            seen = 0;

        for (int i = 0; (i < p2->modes_seen) && !seen; i++)
            seen = !memcmp(p2->modes[i], m, sizeof(m));
        if (!seen && (p2->modes_seen < 256)) {
            memcpy(p2->modes[p2->modes_seen++], m, sizeof(m));
            pclog("P2: 3D render %08X tcm %X abm %X dm %X tdf %X trm %X yuv %X fog %X sm %X | tmf %X lut %X dither %X "
                  "fbrm %X lbrm %X lbf %X win %X cdda %X key %08X-%08X amap %08X-%08X base %X\n",
                  m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], p2->gp[TAG_TEXTURE_MAP_FORMAT],
                  p2->gp[TAG_TEXEL_LUT_MODE], p2->gp[TAG_DITHER_MODE], p2->gp[TAG_FB_READ_MODE],
                  p2->gp[TAG_LB_READ_MODE], p2->gp[TAG_LB_READ_FORMAT], p2->gp[TAG_WINDOW], p2->gp[TAG_COLOR_DDA_MODE],
                  p2->gp[TAG_CHROMA_LOWER], p2->gp[TAG_CHROMA_UPPER], p2->gp[TAG_ALPHA_MAP_LOWER], p2->gp[TAG_ALPHA_MAP_UPPER],
                  p2->gp[TAG_TEXTURE_BASE]);
        }
    }
    if (r->use3d) {
        /* Depth 17.11 (upper: sign and integer, lower: 11 fraction bits at 31:21). */
        r->z     = (int64_t) p2_sext(p2->gp[TAG_Z_START_U], 17) * 2048 + (p2->gp[TAG_Z_START_L] >> 21);
        r->dz_dx = (int64_t) p2_sext(p2->gp[TAG_DZ_DX_U], 17) * 2048 + (p2->gp[TAG_DZ_DX_L] >> 21);
        r->dz_dy = (int64_t) p2_sext(p2->gp[TAG_DZ_DY_DOM_U], 17) * 2048 + (p2->gp[TAG_DZ_DY_DOM_L] >> 21);
        for (int i = 0; i < 3; i++) {
            /* S, T, Q: start, d/dx, d/dyDom at consecutive tags. */
            r->tex[i]     = p2_sext(p2->gp[TAG_S_START + i * 3], 32);
            r->dtex_dx[i] = p2_sext(p2->gp[TAG_S_START + i * 3 + 1], 32);
            r->dtex_dy[i] = p2_sext(p2->gp[TAG_S_START + i * 3 + 2], 32);
        }
        /* Fog, Ks and Kd: signed 2.16 in bits 23:6. */
        {
            static const uint32_t ktag[3] = { TAG_F_START, TAG_KS_START, TAG_KD_START };

            for (int i = 0; i < 3; i++) {
                r->k[i]     = p2_sext(p2->gp[ktag[i]] >> 6, 18);
                r->dk_dx[i] = p2_sext(p2->gp[ktag[i] + 1] >> 6, 18);
                r->dk_dy[i] = p2_sext(p2->gp[ktag[i] + 2] >> 6, 18);
            }
        }
    }

    r->texture_block = ((command & (RENDER_FAST_FILL | RENDER_TEXTURE)) == (RENDER_FAST_FILL | RENDER_TEXTURE)) &&
                       (p2->gp[TAG_TEXTURE_READ_MODE] & 1);

    if (r->prim == PRIM_RECT) {
        r->columns = p2->gp[TAG_RECT_SIZE] & 0xffff;
        r->rows    = p2->gp[TAG_RECT_SIZE] >> 16;
        /* A rectangle leaves the trapezoid registers describing it: Windows
           2000's perm2 uploads an 8x8 brush as a rectangle, then widens it
           with trapezoid copies that set only StartXDom/StartXSub (tooltip
           and balloon backgrounds came out black but for every fourth
           8 pixels). */
        p2->gp[TAG_START_X_DOM] = (uint32_t) r->origin_x << 16;
        p2->gp[TAG_START_X_SUB] = (uint32_t) (r->origin_x + (int) r->columns) << 16;
        p2->gp[TAG_START_Y]     = (uint32_t) r->origin_y << 16;
        p2->gp[TAG_DX_DOM]      = 0;
        p2->gp[TAG_DX_SUB]      = 0;
        p2->gp[TAG_DY]          = 1 << 16;
        p2->gp[TAG_COUNT]       = r->rows;
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
    if (command & RENDER_TEXTURE)
        p2_log("P2: render tex base %08X tmf %08X tdf %08X trm %08X tam %08X tcm %08X lutm %08X lutidx %X s %08X %08X %08X t %08X %08X %08X\n",
               p2->gp[TAG_TEXTURE_BASE], p2->gp[TAG_TEXTURE_MAP_FORMAT], p2->gp[TAG_TEXTURE_DATA_FORMAT],
               p2->gp[TAG_TEXTURE_READ_MODE], p2->gp[TAG_TEXTURE_ADDR_MODE], p2->gp[TAG_TEXTURE_COLOR_MODE],
               p2->gp[TAG_TEXEL_LUT_MODE], p2->gp[TAG_TEXEL_LUT_INDEX], p2->gp[TAG_S_START], p2->gp[TAG_S_START + 1],
               p2->gp[TAG_S_START + 2], p2->gp[TAG_S_START + 3], p2->gp[TAG_S_START + 4], p2->gp[TAG_S_START + 5]);

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
        p2_step_interpolants(r);
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

/* ------------------------------------------------------------------------- */
/* Delta unit: triangle and line setup from three float or fixed vertices.   */
/* It writes the rasterizer, colour, depth, texture and fog registers and    */
/* issues the render as the host would (PRM chapter 8 and Appendix D). The   */
/* edge and sample rules follow ES40's Permedia 2 model.                     */
/* ------------------------------------------------------------------------- */

/* Vertex parameters, by the low 4 bits of the tag. */
enum {
    VP_S = 0, VP_T, VP_Q, VP_KS, VP_KD, VP_R, VP_G, VP_B, VP_A, VP_F, VP_X, VP_Y, VP_Z,
    VP_PACKED = 14
};

static double
p2_u2f(uint32_t v)
{
    union {
        uint32_t u;
        float    f;
    } x;

    x.u = v;
    return x.f;
}

static void
p2_delta_vertex(p2_t *p2, uint32_t tag, uint32_t val)
{
    const int      slot  = (tag - TAG_V0_FIXED) >> 4; /* 0-2 fixed, 3-5 float. */
    const int      v     = slot % 3;
    const int      param = tag & 15;
    const uint32_t mode  = p2->gp[TAG_DELTA_MODE];
    double         d;

    if (param == VP_PACKED) {
        /* Packed 8-bit colour, ABGR or (ColorOrder) ARGB. */
        const int rgb = !!(mode & (1 << 18));

        p2->vtx[v][VP_R] = ((val >> (rgb ? 16 : 0)) & 0xff) / 255.0;
        p2->vtx[v][VP_G] = ((val >> 8) & 0xff) / 255.0;
        p2->vtx[v][VP_B] = ((val >> (rgb ? 0 : 16)) & 0xff) / 255.0;
        p2->vtx[v][VP_A] = (val >> 24) / 255.0;
        return;
    }
    if ((param == 13) || (param == 15))
        return;

    if (slot >= 3)
        d = p2_u2f(val);
    else {
        switch (param) {
            case VP_X:
            case VP_Y:
                d = p2_sext(val, 32) / 65536.0;
                break;
            case VP_KS:
            case VP_KD:
                d = (val & 0xffffff) / 4194304.0;
                break;
            case VP_F:
                d = p2_sext(val, 32) / 4194304.0;
                break;
            case VP_S:
            case VP_T:
            case VP_Q:
                d = p2_sext(val, 32) / 1073741824.0;
                break;
            default:
                d = (val & 0x7fffffff) / 1073741824.0;
                break;
        }
    }
    if (!isfinite(d))
        d = 0.0;
    if (param <= VP_Q) {
        if (((mode >> 14) & 3) == 1)
            d = MAX(-1.0, MIN(1.0, d));
    } else if (mode & (1 << 13)) {
        /* ClampEnable: each parameter to its range. */
        const double lo = (param == VP_F) ? -512.0 : (((param == VP_X) || (param == VP_Y)) ? -32768.0 : 0.0);
        const double hi = (param == VP_KS) ? 2.0 : ((param == VP_F) ? 512.0 : (((param == VP_X) || (param == VP_Y)) ? 32767.0 : 1.0));

        d = MAX(lo, MIN(hi, d));
    }
    p2->vtx[v][param] = d;
}

static int64_t
p2_d2fix(double v, double scale, int64_t lo, int64_t hi)
{
    const double f = trunc(v * scale);

    if (!isfinite(f))
        return (f > 0) ? hi : lo;
    return (int64_t) MAX((double) lo, MIN((double) hi, f));
}

/* Rasterizer coordinates: signed 12.15 aligned to 16.16. */
static uint32_t
p2_delta_coord(double x)
{
    return (uint32_t) p2_d2fix(x, 32768.0, -67108864, 67108863) * 2;
}

static int64_t
p2_delta_scanline(double y)
{
    return p2_fixed_int((int64_t) trunc(y * 65536.0) + 0x7fff);
}

/* Interpolant registers: [start, d/dx, d/dyDom] with the formats of the
   colour (9.11 at bit 4), depth (17.11 split), texture (S, T at bit 20, Q
   at bit 30) and fog/Ks/Kd (2.16 at bit 6) DDAs. */
static void
p2_delta_write_param(p2_t *p2, int param, const double v[3], double zscale)
{
    switch (param) {
        case VP_R:
        case VP_G:
        case VP_B: {
            static const uint32_t tag[3] = { TAG_R_START, TAG_R_START + 3, TAG_R_START + 6 };

            for (int i = 0; i < 3; i++)
                p2->gp[tag[param - VP_R] + i] = (uint32_t) p2_d2fix(v[i], 255.0 * 2048.0, -524288, 524287) << 4;
            break;
        }
        case VP_Z: {
            static const uint32_t tag[3] = { TAG_Z_START_U, TAG_DZ_DX_U, TAG_DZ_DY_DOM_U };

            for (int i = 0; i < 3; i++) {
                const int64_t z = p2_d2fix(v[i], zscale, -134217728, 134217727);

                p2->gp[tag[i]]     = (uint32_t) (z >> 11) & 0x1ffff;
                p2->gp[tag[i] + 1] = (uint32_t) (z & 0x7ff) << 21;
            }
            break;
        }
        case VP_S:
        case VP_T:
            for (int i = 0; i < 3; i++)
                p2->gp[TAG_S_START + (param - VP_S) * 3 + i] = (uint32_t) p2_d2fix(v[i], 2147483648.0, INT32_MIN, INT32_MAX);
            break;
        case VP_Q:
            for (int i = 0; i < 3; i++)
                p2->gp[TAG_Q_START + i] = (uint32_t) p2_d2fix(v[i], 1073741824.0, INT32_MIN, INT32_MAX);
            break;
        default: {
            const uint32_t base = (param == VP_F) ? TAG_F_START : ((param == VP_KS) ? TAG_KS_START : TAG_KD_START);

            for (int i = 0; i < 3; i++)
                p2->gp[base + i] = ((uint32_t) p2_d2fix(v[i], 65536.0, -131072, 131071) & 0x3ffff) << 6;
            break;
        }
    }
}

/* The parameters the DeltaMode and the draw command ask for. */
static int
p2_delta_params(p2_t *p2, uint32_t value, int *list)
{
    const uint32_t mode = p2->gp[TAG_DELTA_MODE];
    int            n    = 0;

    if (mode & 0x40) {
        list[n++] = VP_R;
        list[n++] = VP_G;
        list[n++] = VP_B;
    }
    if (mode & 0x80)
        list[n++] = VP_Z;
    if ((mode & 0x20) && (value & RENDER_TEXTURE)) {
        list[n++] = VP_S;
        list[n++] = VP_T;
        list[n++] = VP_Q;
    }
    if ((mode & 0x10) && (value & RENDER_FOG))
        list[n++] = VP_F;
    if (mode & 0x100)
        list[n++] = VP_KS;
    if (mode & 0x200)
        list[n++] = VP_KD;
    return n;
}

/* S, T and Q of the three vertices after TextureParameterMode. */
static void
p2_delta_texparams(p2_t *p2, double p[3][16])
{
    if (((p2->gp[TAG_DELTA_MODE] >> 14) & 3) == 2) {
        /* Normalize: scale all three together into -1..1 (S/Q, T/Q kept). */
        double m = 0.0;

        for (int v = 0; v < 3; v++)
            for (int i = VP_S; i <= VP_Q; i++)
                m = MAX(m, fabs(p[v][i]));
        if (m > 0.0) {
            for (int v = 0; v < 3; v++)
                for (int i = VP_S; i <= VP_Q; i++)
                    p[v][i] /= m;
        }
    }
}

static void
p2_delta_render(p2_t *p2, uint32_t cmd)
{
    p2->gp[TAG_RENDER] = cmd;
    if (!(p2->gp[TAG_DELTA_MODE] & (1 << 12))) /* NoDraw. */
        p2_raster_start(p2, cmd);
}

static void
p2_delta_triangle(p2_t *p2, uint32_t value)
{
    const uint32_t mode       = p2->gp[TAG_DELTA_MODE];
    const uint32_t cmd        = (value & ~(0x80000u | 0x100000u) & ~RENDER_PRIM_MASK) | PRIM_TRAP;
    const int      correction = (value & RENDER_SUBPIXEL) && (mode & 0x400);
    const double   zscale     = (((mode >> 2) & 3) ? 65535.0 : 32767.0) * 2048.0;
    const int      bias       = (p2->gp[TAG_RASTERIZER_MODE] >> 4) & 3;
    const int64_t  b          = (bias == 1) ? 0x8000 : ((bias == 2) ? 0x7fff : 0);
    double         p[3][16];
    const double  *a;
    const double  *bv;
    const double  *c;
    int            order[3] = { 0, 1, 2 };
    int            list[16];
    int            n;
    double         area;

    memcpy(p, p2->vtx, sizeof(p));
    if (p2->mode_log && (p2->range_logged < 16)) {
        /* Debug: vertices beyond the rasterizer's 12.15 range. */
        for (int v = 0; v < 3; v++) {
            if ((fabs(p[v][VP_X]) > 2047.0) || (fabs(p[v][VP_Y]) > 2047.0)) {
                p2->range_logged++;
                pclog("P2: delta vertex out of range: (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f)\n", p[0][VP_X], p[0][VP_Y],
                      p[1][VP_X], p[1][VP_Y], p[2][VP_X], p[2][VP_Y]);
                break;
            }
        }
    }
    area = (p[1][VP_X] - p[0][VP_X]) * (p[2][VP_Y] - p[0][VP_Y]) - (p[2][VP_X] - p[0][VP_X]) * (p[1][VP_Y] - p[0][VP_Y]);
    if ((area == 0.0) || !isfinite(area))
        return;
    if ((mode & 0x20000) && ((value & 0x100000) ? (area < 0.0) : (area > 0.0)))
        return; /* Back face culled. */
    p2_delta_texparams(p2, p);

    /* Top to bottom (stable). */
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2 - i; j++)
            if (p[order[j]][VP_Y] > p[order[j + 1]][VP_Y]) {
                const int t  = order[j];
                order[j]     = order[j + 1];
                order[j + 1] = t;
            }
    a  = p[order[0]];
    bv = p[order[1]];
    c  = p[order[2]];

    {
        const double   long_slope   = (c[VP_X] - a[VP_X]) / (c[VP_Y] - a[VP_Y]);
        const double   first_slope  = (bv[VP_Y] == a[VP_Y]) ? 0.0 : (bv[VP_X] - a[VP_X]) / (bv[VP_Y] - a[VP_Y]);
        const double   second_slope = (c[VP_Y] == bv[VP_Y]) ? 0.0 : (c[VP_X] - bv[VP_X]) / (c[VP_Y] - bv[VP_Y]);
        const int64_t  first_y      = p2_delta_scanline(a[VP_Y]);
        const int64_t  middle_y     = p2_delta_scanline(bv[VP_Y]);
        const int64_t  last_y       = p2_delta_scanline(c[VP_Y]);
        const uint32_t first_rows   = (uint32_t) MAX(0, middle_y - first_y);
        const uint32_t second_rows  = (uint32_t) MAX(0, last_y - middle_y);
        const int64_t  start_y      = first_rows ? first_y : middle_y;
        const double   sample_y     = (double) start_y + 0.5;
        const double   error_y      = correction ? sample_y - a[VP_Y] : 0.0;
        const double   xdom         = a[VP_X] + error_y * long_slope;
        const double   xsub         = first_rows ? a[VP_X] + error_y * first_slope
                                                 : bv[VP_X] + (correction ? sample_y - bv[VP_Y] : 0.0) * second_slope;
        const int      split        = first_rows && second_rows;
        const double   knee_x       = bv[VP_X] + (correction ? (double) middle_y + 0.5 - bv[VP_Y] : 0.0) * second_slope;
        const double   det          = (bv[VP_X] - a[VP_X]) * (c[VP_Y] - a[VP_Y]) - (c[VP_X] - a[VP_X]) * (bv[VP_Y] - a[VP_Y]);
        const double   dir          = (det > 0.0) ? 1.0 : -1.0;

        if ((!first_rows && !second_rows) || !isfinite(long_slope) || (det == 0.0))
            return;

        /* Edges; the Delta supplies the (nearly) half pixel bias itself. */
        p2->gp[TAG_START_X_DOM] = p2_delta_coord(xdom) + 0x7fff - (uint32_t) b;
        p2->gp[TAG_START_X_SUB] = p2_delta_coord(xsub) + 0x7fff - (uint32_t) b;
        p2->gp[TAG_START_Y]     = (uint32_t) (start_y * 65536) - (uint32_t) b;
        p2->gp[TAG_DX_DOM]      = p2_delta_coord(long_slope);
        p2->gp[TAG_DX_SUB]      = p2_delta_coord(first_rows ? first_slope : second_slope);
        p2->gp[TAG_DY]          = 65536;
        p2->gp[TAG_COUNT]       = first_rows ? first_rows : second_rows;

        /* Interpolants: plane gradients; d/dyDom walks the dominant edge,
           d/dx steps along the span (away from the dominant edge). */
        n = p2_delta_params(p2, value, list);
        for (int i = 0; i < n; i++) {
            const int    k  = list[i];
            const double gx = ((bv[k] - a[k]) * (c[VP_Y] - a[VP_Y]) - (c[k] - a[k]) * (bv[VP_Y] - a[VP_Y])) / det;
            const double gy = ((bv[VP_X] - a[VP_X]) * (c[k] - a[k]) - (c[VP_X] - a[VP_X]) * (bv[k] - a[k])) / det;
            const double dom = gy + long_slope * gx;
            const double v[3] = { a[k] + error_y * dom, gx * dir, dom };

            p2_delta_write_param(p2, k, v, zscale);
        }
        if (mode & 0x40) {
            /* Alpha is flat across a primitive (not interpolated): the mean. */
            const double alpha = (p[0][VP_A] + p[1][VP_A] + p[2][VP_A]) / 3.0;

            p2->gp[TAG_A_START] = (uint32_t) p2_d2fix(alpha, 255.0 * 2048.0, 0, 524287) << 4;
        }

        p2_delta_render(p2, cmd);
        if (split && !(mode & (1 << 12)) && !p2->raster.active) {
            p2->gp[TAG_START_X_SUB] = p2_delta_coord(knee_x) + 0x7fff;
            p2->gp[TAG_DX_SUB]      = p2_delta_coord(second_slope);
            p2_raster_continue(p2, TAG_CONTINUE_NEW_SUB, second_rows);
        }
    }
}

/* DrawLine01 (from vertex 0 to 1) and DrawLine10. */
static void
p2_delta_line(p2_t *p2, uint32_t value, int from, int to)
{
    const uint32_t mode   = p2->gp[TAG_DELTA_MODE];
    const uint32_t cmd    = (value & ~(0x80000u | 0x100000u) & ~RENDER_PRIM_MASK) | PRIM_LINE;
    const double   zscale = (((mode >> 2) & 3) ? 65535.0 : 32767.0) * 2048.0;
    const int      bias   = (p2->gp[TAG_RASTERIZER_MODE] >> 4) & 3;
    const int64_t  b      = (bias == 1) ? 0x8000 : ((bias == 2) ? 0x7fff : 0);
    double         p[3][16];
    const double  *a;
    const double  *e;
    double         dx;
    double         dy;
    double         steps;
    int            list[16];
    int            n;

    memcpy(p, p2->vtx, sizeof(p));
    p2_delta_texparams(p2, p);
    a     = p[from];
    e     = p[to];
    dx    = e[VP_X] - a[VP_X];
    dy    = e[VP_Y] - a[VP_Y];
    steps = floor(MAX(fabs(dx), fabs(dy)) + 0.5);
    if ((steps < 1.0) || !isfinite(steps))
        return;

    p2->gp[TAG_START_X_DOM] = p2_delta_coord(a[VP_X]) + 0x7fff - (uint32_t) b;
    p2->gp[TAG_START_Y]     = p2_delta_coord(a[VP_Y]) + 0x7fff - (uint32_t) b;
    p2->gp[TAG_DX_DOM]      = p2_delta_coord(dx / steps);
    p2->gp[TAG_DY]          = p2_delta_coord(dy / steps);
    p2->gp[TAG_COUNT]       = (uint32_t) MIN(65535.0, steps);

    n = p2_delta_params(p2, value, list);
    for (int i = 0; i < n; i++) {
        const int    k    = list[i];
        const double v[3] = { a[k], 0.0, (e[k] - a[k]) / steps };

        p2_delta_write_param(p2, k, v, zscale);
    }
    if (mode & 0x40)
        p2->gp[TAG_A_START] = (uint32_t) p2_d2fix(a[VP_A], 255.0 * 2048.0, 0, 524287) << 4;

    p2_delta_render(p2, cmd);
}

/* The host has loaded a texture the graphics processor halted on (bypass
   DMA RestartGP, ByDMAComplete): load the handle again and go on. */
static void
p2_tex_restart(p2_t *p2)
{
    if (!p2->tex_invalid)
        return;
    p2->tex_invalid = 0;
    p2_gp_command(p2, p2->tex_invalid_tag, p2->tex_invalid_val);
    if (!p2->tex_invalid)
        p2_out_space(p2);
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
                                                                : ((tag == TAG_COLOR) || (tag == TAG_FB_DATA) || (tag == TAG_FB_SOURCE_DATA) ||
                                                                   (tag == TAG_LB_DATA) || (tag == TAG_DEPTH) || (tag == TAG_STENCIL) ||
                                                                   (tag == TAG_TEXEL0));
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

    if ((tag >= TAG_V0_FIXED) && (tag < TAG_DELTA_MODE)) {
        p2_delta_vertex(p2, tag, val);
        return;
    }

    switch (tag) {
        case TAG_RENDER:
            p2_raster_start(p2, val);
            break;

        case TAG_DRAW_TRIANGLE:
            p2->delta_cmd = val;
            p2_delta_triangle(p2, val);
            break;
        case TAG_REPEAT_TRIANGLE:
            p2_delta_triangle(p2, p2->delta_cmd);
            break;
        case TAG_DRAW_LINE01:
        case TAG_DRAW_LINE10:
            p2->delta_cmd                = val;
            p2->delta_line10             = (tag == TAG_DRAW_LINE10);
            p2_delta_line(p2, val, (tag == TAG_DRAW_LINE10), (tag != TAG_DRAW_LINE10));
            break;
        case TAG_REPEAT_LINE:
            p2_delta_line(p2, p2->delta_cmd, p2->delta_line10, !p2->delta_line10);
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

        case TAG_TEXTURE_ID:
        case TAG_TEXEL_LUT_ID: {
            /* Indirect handles: load TextureBaseAddress / TexelLUTAddress
               from the dword at this address (32-bit units; bit 30 host
               memory). Bit 31 of the loaded value marks it invalid: the
               chip would halt for the host to page the data in. */
            const uint32_t a = (val & 0xffffff) * 4;
            const uint32_t v = (val & (1 << 30)) ? mem_readl_phys(a) : p2_fb_read(p2, a, 4);

            if (v & (1u << 31)) {
                /* Not resident: halt with the texture invalid interrupt
                   until the host loads it (bypass DMA RestartGP or
                   ByDMAComplete), then load the handle again. */
                p2->tex_invalid     = 1;
                p2->tex_invalid_tag = tag;
                p2->tex_invalid_val = val;
                p2_raise_int(p2, INT_TEX_INVALID);
                break;
            }
            p2->gp[(tag == TAG_TEXTURE_ID) ? TAG_TEXTURE_BASE : TAG_TEXEL_LUT_ADDRESS] = v;
            break;
        }
        case TAG_TEXEL_LUT_TRANSFER: {
            /* Load Count entries from TexelLUTAddress (32-bit units, bit 30
               host memory) into the LUT from Index on. */
            const uint32_t la    = p2->gp[TAG_TEXEL_LUT_ADDRESS];
            const uint32_t count = (val >> 8) & 0x1ff;

            for (uint32_t i = 0; i < count; i++) {
                const uint32_t a   = ((la & 0xffffff) + i) * 4;
                const uint32_t e   = (la & (1 << 30)) ? mem_readl_phys(a) : p2_fb_read(p2, a, 4);
                const uint32_t idx = (val + i) & 0xff;

                p2->texel_lut[idx] = e;
                if (idx < 16)
                    p2->gp[TAG_TEXEL_LUT0 + idx] = e;
            }
            break;
        }

        case TAG_TEXEL_LUT_DATA:
            p2->texel_lut[p2->gp[TAG_TEXEL_LUT_INDEX] & 0xff] = val;
            if ((p2->gp[TAG_TEXEL_LUT_INDEX] & 0xff) < 16)
                p2->gp[TAG_TEXEL_LUT0 + (p2->gp[TAG_TEXEL_LUT_INDEX] & 15)] = val;
            p2->gp[TAG_TEXEL_LUT_INDEX] = (p2->gp[TAG_TEXEL_LUT_INDEX] + 1) & 0xff;
            break;

        case TAG_TEXTURE_DL_OFFSET:
            if (p2->mode_log && p2->dl_words)
                pclog("P2: texture download at %06X: %u words\n", p2->dl_start, p2->dl_words);
            p2->gp[TAG_TEXTURE_DL_OFFSET] = val & 0x3fffff;
            p2->dl_start                  = val & 0x3fffff;
            p2->dl_words                  = 0;
            break;
        case TAG_TEXTURE_DATA:
            /* Raw texture download (cached glyph masks, textures): a dword
               at TextureDownloadOffset, past the rendering units, then the
               offset steps. */
            p2_fb_write(p2, (int64_t) (p2->gp[TAG_TEXTURE_DL_OFFSET] & 0x3fffff) * 4, 4, val, p2->gp[TAG_FB_HW_WRITE_MASK]);
            p2->gp[TAG_TEXTURE_DL_OFFSET] = (p2->gp[TAG_TEXTURE_DL_OFFSET] + 1) & 0x3fffff;
            p2->dl_words++;
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
            /* Loaded from the ROM at reset (SubsystemFromRom strap): read-only
               then. STB's BIOS writes the same values again at POST; ELSA's
               write zeros, which the real board ignores. */
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

    if (!strncmp(args, "tags", 4)) {
        /* Tags written since the last "tags", with their current values. */
        for (int i = 0; i < GP_TAGS; i++) {
            if (p2->tag_count[i])
                pclog("P2: tag %03X n %8u = %08X\n", i, p2->tag_count[i], p2->gp[i]);
        }
        memset(p2->tag_count, 0, sizeof(p2->tag_count));
        return;
    }

    if (!strncmp(args, "dump ", 5)) {
        /* The whole framebuffer memory to a file (texture layouts etc.). */
        FILE *f = plat_fopen(args + 5, "wb");

        if (f) {
            fwrite(svga->vram, 1, p2->vram_size, f);
            fclose(f);
        }
        pclog("P2: VRAM dump to %s %s\n", args + 5, f ? "done" : "failed");
        return;
    }

    if (!strncmp(args, "modes", 5)) {
        /* Start (again) logging every new combination of 3D unit modes. */
        p2->mode_log   = 1;
        p2->modes_seen = 0;
        pclog("P2: logging 3D modes\n");
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
    pclog("P2: filter %08X out %i intf %08X inte %08X fifo hdr %u tag %X active %i cmd %08X left %u\n",
          p2->gp[TAG_FILTER_MODE], p2->out_count, p2->int_flags, p2->int_enable, p2->fifo_hdr_count, p2->fifo_hdr_tag,
          p2->raster.active, p2->raster.command, p2->raster.payload_left);
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
    p2->flash_cycle   = 0;
    p2->flash_id_mode = 0;
    p2->dma_count     = 0;
    p2->agp_tex_base  = 0;
    memset(p2->bydma, 0, sizeof(p2->bydma));
    p2->out_dma_count = 0;
    p2->bypass_mask   = 0xffffffff;
    p2->fb_write_mask = 0xffffffff;
    p2->chip_config   = 0x00000006 | (p2->is_agp ? 0x00000200 : 0);
    p2->mem_config    = 0xe6002021;
    memset(p2->vtg, 0, sizeof(p2->vtg));
    p2_i2c_reset(p2);
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
    p2->pci_regs[0x08] = p2->revision; /* 01h Permedia 2, 11h Permedia 2A */
    p2->pci_regs[0x0b] = 0x03; /* VGA compatible display controller */
    p2->pci_regs[0x16] = 0x00;
    /* Not prefetchable: ELSA's drivers take BAR1 as read for the aperture's
       address (its ROM access reads 8 bytes off with bit 3 set). */
    p2->pci_regs[0x14] = 0x00;
    p2->pci_regs[0x18] = 0x00;
    /* The board's subsystem IDs: the chip loads them from the ROM's last four
       bytes at reset (7FFCh / FFFCh). Some BIOSes (STB's) write them again at
       POST from offset 34h; ELSA's keep a different layout there. */
    if (p2->bios_rom.rom && (p2->bios_rom.sz >= 4)) {
        const uint8_t *ids = &p2->bios_rom.rom[p2->bios_rom.sz - 4];

        p2->pci_regs[0x2c] = ids[0];
        p2->pci_regs[0x2d] = ids[1];
        p2->pci_regs[0x2e] = ids[2];
        p2->pci_regs[0x2f] = ids[3];
    }
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

/* ELSA's boards keep their factory data in the flash ROM: 64 words at the
   offset in word 0Ah -- 0: CRC of words 1-63, 1: 4433h, 2: board code,
   6: feature flags, 7 and 8: clocks in 4 kHz units (8: the memory clock),
   14h/15h: a memory timing override (0: the chip's default). BIOS update
   images leave the block blank (the flash tool keeps the board's own), and
   ELSA's drivers refuse a board whose block fails its CRC; give such an
   image the values a board would carry (MCLK 80 MHz, as the BIOS programs). */
static uint16_t
p2_elsa_crc(const uint16_t *words, int n)
{
    uint16_t crc = 0;

    for (int w = 0; w < n; w++) {
        for (int i = 0; i < 16; i++) {
            const int bit = ((words[w] >> i) ^ (crc >> 14) ^ (crc >> 15) ^ (crc >> 1) ^ 1) & 1;

            crc = (crc << 1) | bit;
        }
    }

    return crc;
}

static void
p2_elsa_id_block(p2_t *p2)
{
    uint8_t *rom = p2->bios_rom.rom;
    uint16_t words[64];
    uint32_t ptr;
    int      sum = 0;

    if (!rom || (p2->bios_rom.sz < 0x8000) || (rom[0] != 0x55) || (rom[1] != 0xaa))
        return;
    ptr = rom[0x0a] | (rom[0x0b] << 8);
    if (!ptr || ((ptr + sizeof(words)) > 0x7ff0))
        return;
    for (int i = 0; i < 64; i++)
        words[i] = rom[ptr + i * 2] | (rom[ptr + i * 2 + 1] << 8);
    if ((words[1] != 0x4433) || (words[0] == p2_elsa_crc(&words[1], 63)))
        return;

    for (uint32_t i = 0; i < sizeof(words); i++)
        sum += rom[ptr + i];
    for (int i = 3; i < 64; i++)
        words[i] = 0x0000;
    words[7] = 230000 / 4; /* 230 MHz */
    words[8] = 80000 / 4;  /* 80 MHz */
    words[0] = p2_elsa_crc(&words[1], 63);
    for (int i = 0; i < 64; i++) {
        rom[ptr + i * 2]     = words[i] & 0xff;
        rom[ptr + i * 2 + 1] = words[i] >> 8;
        sum -= rom[ptr + i * 2] + rom[ptr + i * 2 + 1];
    }
    rom[0x7ff0] += sum; /* Padding: the image's checksum stays right. */
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

    for (uint32_t half = 0; (half + 0x8000) <= (uint32_t) p2->bios_rom.sz; half += 0x8000) {
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

    p2->hold_tag = calloc(P2_HOLD_QUEUE, sizeof(uint32_t));
    p2->hold_val = calloc(P2_HOLD_QUEUE, sizeof(uint32_t));
    svga_t *svga = &p2->svga;

    p2->is_agp    = !!(info->flags & DEVICE_AGP);
    p2->vram_size = device_get_config_int("memory") << 20;
    p2->vram_mask = p2->vram_size - 1;

    {
        const char    *bios = device_get_config_bios("bios");
        const uint32_t size = device_get_bios_file_size(info, bios);

        p2->revision = device_get_bios_local(info, bios);
        rom_init(&p2->bios_rom, device_get_bios_file(info, bios, 0), 0xc0000, size, size - 1, 0, MEM_MAPPING_EXTERNAL);
    }
    p2_elsa_id_block(p2);
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
    mem_mapping_add(&p2->ap_mapping[0], 0, 0,
                    p2_ap0_readb, p2_ap0_readw, p2_ap0_readl,
                    p2_ap0_writeb, p2_ap0_writew, p2_ap0_writel,
                    NULL, MEM_MAPPING_EXTERNAL, p2);
    mem_mapping_add(&p2->ap_mapping[1], 0, 0,
                    p2_ap1_readb, p2_ap1_readw, p2_ap1_readl,
                    p2_ap1_writeb, p2_ap1_writew, p2_ap1_writel,
                    NULL, MEM_MAPPING_EXTERNAL, p2);
    mem_mapping_disable(&p2->ctrl_mapping);
    mem_mapping_disable(&p2->ap_mapping[0]);
    mem_mapping_disable(&p2->ap_mapping[1]);

    p2->i2c_ddc = i2c_gpio_init("ddc_permedia2");
    p2->ddc     = ddc_init(i2c_gpio_get_bus(p2->i2c_ddc));
    p2->i2c_vs  = i2c_gpio_init("i2c_permedia2_vs");

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

static void
p2_close(void *priv)
{
    p2_t *p2 = (p2_t *) priv;

    if (p2_dbg == p2) {
        debug_cmd_set_device_hook(NULL);
        p2_dbg = NULL;
    }
    svga_close(&p2->svga);
    ddc_close(p2->ddc);
    i2c_gpio_close(p2->i2c_ddc);
    i2c_gpio_close(p2->i2c_vs);
    free(p2->hold_tag);
    free(p2->hold_val);
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

static const device_config_t p2_pci_config[] = {
    // clang-format off
    {
        .name           = "bios",
        .description    = "Board (BIOS)",
        .type           = CONFIG_BIOS,
        .default_string = "stb_glyder_max2",
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .bios           = {
            {
                .name          = "STB GLyder MAX-2+ 1.50 (Permedia 2A)",
                .internal_name = "stb_glyder_max2",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0x11,
                .size          = 65536,
                .flags         = 0,
                .files         = { P2_ROM_DIR "stbp2a.bin", "" }
            },
            {
                .name          = "ELSA WINNER 2000/Office 7.03.04",
                .internal_name = "elsa_winner2000_70304",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0x01,
                .size          = 32768,
                .flags         = 0,
                .files         = { P2_ROM_DIR "w2o70304.pcn", "" }
            },
            {
                .name          = "ELSA WINNER 2000/Office 8.04.01 (Permedia 2A)",
                .internal_name = "elsa_winner2000_80401",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0x11,
                .size          = 32768,
                .flags         = 0,
                .files         = { P2_ROM_DIR "w2o80401.pan", "" }
            },
            {
                .name          = "ELSA GLoria Synergy 7.03.04",
                .internal_name = "elsa_synergy_70304",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0x01,
                .size          = 32768,
                .flags         = 0,
                .files         = { P2_ROM_DIR "syn70304.pcn", "" }
            },
            {
                .name          = "ELSA GLoria Synergy 8.04.01 (Permedia 2A)",
                .internal_name = "elsa_synergy_80401",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0x11,
                .size          = 32768,
                .flags         = 0,
                .files         = { P2_ROM_DIR "syn80401.pan", "" }
            },
            {
                .name          = "ELSA GLoria Synergy 8.07.00 (Permedia 2A)",
                .internal_name = "elsa_synergy_80700",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0x11,
                .size          = 32768,
                .flags         = 0,
                .files         = { P2_ROM_DIR "syn80700.pan", "" }
            },
            { .files_no = 0 }
        },
    },
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

static const device_config_t p2_agp_config[] = {
    // clang-format off
    {
        .name           = "bios",
        .description    = "Board (BIOS)",
        .type           = CONFIG_BIOS,
        .default_string = "stb_glyder_max2",
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .bios           = {
            {
                .name          = "STB GLyder MAX-2+ 1.50 (Permedia 2A)",
                .internal_name = "stb_glyder_max2",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0x11,
                .size          = 65536,
                .flags         = 0,
                .files         = { P2_ROM_DIR "stbp2a.bin", "" }
            },
            {
                .name          = "ELSA WINNER 2000/Office 8.06.02",
                .internal_name = "elsa_winner2000_agp_80602",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0x01,
                .size          = 32768,
                .flags         = 0,
                .files         = { P2_ROM_DIR "w2o80602.acn", "" }
            },
            {
                .name          = "ELSA WINNER 2000/Office 8.06.04 (Permedia 2A)",
                .internal_name = "elsa_winner2000_agp_80604",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0x11,
                .size          = 32768,
                .flags         = 0,
                .files         = { P2_ROM_DIR "w2o80604.aan", "" }
            },
            {
                .name          = "ELSA GLoria Synergy 8.07.00",
                .internal_name = "elsa_synergy_agp_80700c",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0x01,
                .size          = 32768,
                .flags         = 0,
                .files         = { P2_ROM_DIR "syn80700.acn", "" }
            },
            {
                .name          = "ELSA GLoria Synergy 8.07.00 (Permedia 2A)",
                .internal_name = "elsa_synergy_agp_80700a",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0x11,
                .size          = 32768,
                .flags         = 0,
                .files         = { P2_ROM_DIR "syn80700.aan", "" }
            },
            { .files_no = 0 }
        },
    },
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
    .name          = "3Dlabs Permedia 2 PCI",
    .internal_name = "glyder_max2_pci",
    .flags         = DEVICE_PCI,
    .local         = 0,
    .init          = p2_init,
    .close         = p2_close,
    .reset         = p2_reset,
    .available     = NULL,
    .speed_changed = p2_speed_changed,
    .force_redraw  = p2_force_redraw,
    .config        = p2_pci_config
};

const device_t permedia2_stb_glyder_max2_agp_device = {
    .name          = "3Dlabs Permedia 2 AGP",
    .internal_name = "glyder_max2_agp",
    .flags         = DEVICE_AGP,
    .local         = 0,
    .init          = p2_init,
    .close         = p2_close,
    .reset         = p2_reset,
    .available     = NULL,
    .speed_changed = p2_speed_changed,
    .force_redraw  = p2_force_redraw,
    .config        = p2_agp_config
};
