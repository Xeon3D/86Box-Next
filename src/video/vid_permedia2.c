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
 *          2D paths the display drivers use.
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
#define GP_TAGS 0x200

typedef struct p2_t {
    svga_t svga;

    rom_t         bios_rom;
    mem_mapping_t ctrl_mapping;
    mem_mapping_t ap_mapping[2];

    uint8_t pci_slot;
    int     is_agp;
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
    uint32_t gp[GP_TAGS];
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

static void
p2_vsync_callback(svga_t *svga)
{
    /* The border follows the palette entry, which the BIOS loads after AR11. */
    svga->overscan_color = (svga->seqregs[5] & 0x08) ? svga->pallook[svga->attrregs[0x11]] : 0x000000;

    p2_raise_int((p2_t *) svga->priv, INT_VRETRACE);
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
    svga->dac_hwcursor.addr = svga->dac_hwcursor.yoff * (size >> 3);
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
    const int      plane  = (size == 64) ? 512 : 128;
    const int      mode   = p2->dac_regs[TVP_CURSOR_CONTROL] & 0x03;
    const uint8_t *cd     = p2->cursor_ram;
    uint32_t       base   = svga->dac_hwcursor_latch.addr;
    int            offset = svga->dac_hwcursor_latch.x;
    uint32_t      *p      = svga->monitor->target_buffer->line[displine];

    /* A 32x32 cursor is one of four images in the cursor RAM. */
    if (size == 32)
        base += ((p2->dac_regs[TVP_CURSOR_CONTROL] >> 4) & 3) * 256;

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

    svga->dac_hwcursor_latch.addr += size >> 3;
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

    if (svga->seqregs[5] & 0x08) {
        /* The VGA core drives the display. */
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
        return;
    }

    /* The VGA core is off: the video timing generator drives the display. */
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

    svga->clock = (cpuclock * (double) (1ULL << 32)) / p2_pixclk(p2, p2->vclk_ctl & 3);

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

    if (!(vc & VC_ENABLE) || (svga->seqregs[5] & 0x40)) {
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
        p2->fifo_hdr_tag  = val & 0x1ff;
        p2->fifo_hdr_mode = (val >> 14) & 3;
        if (p2->fifo_hdr_mode == 2) {
            p2->fifo_hdr_mask  = val >> 16;
            p2->fifo_hdr_tag   = val & 0x1f0;
            p2->fifo_hdr_count = 0;
            for (int i = 0; i < 16; i++)
                p2->fifo_hdr_count += (p2->fifo_hdr_mask >> i) & 1;
        } else
            p2->fifo_hdr_count = (val >> 16) + 1;
        return;
    }

    switch (p2->fifo_hdr_mode) {
        case 1: /* Increment. */
            p2_gp_write(p2, p2->fifo_hdr_tag++, val);
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
            if (!(svga->seqregs[5] & 0x08))
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
p2_ctrl_write(p2_t *p2, uint32_t addr, uint32_t val)
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
p2_ctrl_read(p2_t *p2, uint32_t addr)
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

/* Command tags: filled in by the rasterizer. */
static void
p2_gp_command(UNUSED(p2_t *p2), UNUSED(uint32_t tag), UNUSED(uint32_t val))
{
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

    p2->pci_regs[0x00] = 0x4c; /* Texas Instruments */
    p2->pci_regs[0x01] = 0x10;
    p2->pci_regs[0x02] = 0x07; /* TVP4020 (Permedia 2) */
    p2->pci_regs[0x03] = 0x3d;
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

static void *
p2_init(const device_t *info)
{
    p2_t   *p2   = calloc(1, sizeof(p2_t));
    svga_t *svga = &p2->svga;

    p2->is_agp    = !!(info->flags & DEVICE_AGP);
    p2->vram_size = device_get_config_int("memory") << 20;
    p2->vram_mask = p2->vram_size - 1;

    rom_init(&p2->bios_rom, BIOS_STB_GLYDER_MAX2, 0xc0000, 0x10000, 0xffff, 0, MEM_MAPPING_EXTERNAL);
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
