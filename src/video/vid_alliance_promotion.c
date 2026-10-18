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
#include <86box/i2c.h>
#include <86box/vid_ddc.h>
#include <86box/plat_unused.h>
#include <86box/debug_cmd.h>

#define BIOS_PM6410_PCPARTNER "roms/video/alliance/pcpartner_pm6410.bin"
#define BIOS_AT3D_STINGRAY_230 "roms/video/alliance/at3d/hercules_stingray128_3d_v2.30.00-b27.bin"
#define BIOS_AT3D_STINGRAY_435 "roms/video/alliance/at3d/hercules_stingray128_3d_v4.35.00-b11.bin"
#define BIOS_AT3D_GA_RUSH6     "roms/video/alliance/at3d/gigabyte_ga-rush6_v4.350010.bin"

enum {
    PM_6410 = 0, /* "GXX", PCI 1142:3210 */
    PM_6422,     /* "GXS", PCI 1142:6422 */
    PM_AT3D,     /* ProMotion-AT3D / AT25, PCI 1142:643D (2D only here) */
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
    mem_mapping_t port_mapping; /* the blit port at B8000h (SR1B bits 5:3 = 3) */

    int      chip;
    void    *ramdac;
    uint16_t xr_io_base; /* the XR I/O window (6422): SR1F:SR1E */
    int      blt_pending; /* a start (or quick start) seen in this register access */
    uint8_t  qs_written;  /* quick-start registers written since the last start (bit per mode) */
    void    *i2c;
    void    *ddc;
    uint32_t vram_size;
    uint32_t vram_mask;
    uint32_t lfb_base;
    uint32_t lfb_size;

    uint8_t pci_regs[256];
    uint8_t pci_slot;

    uint8_t sr[0x40]; /* SR08 and up */
    uint8_t cr[0x40]; /* CR19 and up */
    uint8_t xr[XR_SIZE];

    /* Drawing engine: an operation in progress. */
    struct {
        uint32_t ctrl;
        int      op;
        int      bpp;   /* bytes per pixel the engine works in */
        int      pitch; /* bytes per line in X/Y addressing */
        int      w, h;
        int      dx, dy; /* direction: +1 or -1 */
        int      x, y;   /* destination X/Y of the current row */
        int      x0, y0; /* destination X/Y at the start */
        uint32_t dst;    /* linear destination of the current row */
        int      row;    /* rows done */

        /* Host data (blit port writes) for the current row. */
        uint8_t  host[16384];
        int      host_len;
        int      host_row_bytes;
        int      host_skip; /* leading bits (mono) or bytes (colour) */

        /* Screen-to-host data waiting to be read. */
        uint8_t *rd_buf;
        uint32_t rd_len, rd_pos, rd_size;
    } blt;
} pm_t;

static int pm_do_log = 0;

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
static int  pm_ext_bpp(pm_t *pm);

/* ------------------------------------------------------------------------- */
/* Extended registers                                                        */
/* ------------------------------------------------------------------------- */

static void
pm_update_banks(pm_t *pm)
{
    svga_t  *svga = &pm->svga;
    uint32_t bank = (pm->xr[0xc0] | (pm->xr[0xc1] << 8)) << 12; /* 4 KB units */

    /* The AT3D's BIOS sizes memory through a bank at 2 MB: on a 2 MB board
       that is empty, not a mirror. */
    svga->write_bank = (pm->chip >= PM_AT3D) ? (bank & 0x3fffff) : (bank & pm->vram_mask);
    svga->read_bank  = svga->write_bank;
    /* A banked chain-4 window addresses video memory a byte at a time. */
    svga->packed_chain4 = !!bank || (pm->sr[0x1c - 8] & 0x10);
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
    /* The 6422's BIOS sizes memory with XRC5 bit 0 and SR1C bit 4 instead: a
       1 MB board then leaves the upper half of each 64-bit word empty. */
    if ((pm->chip >= PM_6422) && (pm->xr[XR_MEM_CONFIG + 1] & 0x01) && (pm->sr[0x1c - 8] & 0x10) &&
        (pm->vram_size <= 0x100000) && (addr & 4))
        return svga->vram_max;
    return addr;
}

/* ------------------------------------------------------------------------- */
/* Drawing engine                                                            */
/*                                                                           */
/* XR40 (dword) is the command: bits 3:0 the operation, 6/7 X/Y direction    */
/* negative, 8 Y major (lines), 9 source linear, 11 source contiguous,       */
/* 10 the mono pattern (XR48-XR4F) as source, 12 mono source, 13 source      */
/* transparency, 16 24 bpp (byte-wise), 18 destination linear, 19 contiguous,*/
/* 23:22 pattern (1 = 4x4 dither, 3 = 8x8 colour), 26:24 the screen width    */
/* model, 28:27 destination update, 30:29 quick start, 31 start. Linear      */
/* addresses are pixel addresses split as bits 31:16 = address 23:12 and     */
/* bits 11:0 = address 11:0. The ROP (XR46) is a Windows ROP3 in which the   */
/* chips without a pattern unit see the pattern as the source (the drivers   */
/* use CCh for both).                                                        */
/* ------------------------------------------------------------------------- */

enum {
    BLT_OP_NOP       = 0x0,
    BLT_OP_SS        = 0x1,
    BLT_OP_RECT      = 0x2,
    BLT_OP_STRETCH   = 0x3,
    BLT_OP_STRIP     = 0x4,
    BLT_OP_MS        = 0x8, /* host to screen through the blit port */
    BLT_OP_SM        = 0x9, /* screen to host */
    BLT_OP_LINE      = 0xc,
    BLT_OP_LINE_NOEND = 0xd
};

#define BLT_DIR_XNEG     (1u << 6)
#define BLT_DIR_YNEG     (1u << 7)
#define BLT_MAJOR_Y      (1u << 8)
#define BLT_SRC_LINEAR   (1u << 9)
#define BLT_SRC_PATTERN  (1u << 10)
#define BLT_SRC_CONTIG   (1u << 11)
#define BLT_SRC_MONO     (1u << 12)
#define BLT_SRC_XPARENT  (1u << 13)
#define BLT_24BPP        (1u << 16)
#define BLT_BYTE_LINEAR  (1u << 17) /* AT3D: see pm_blt_lin() */
#define BLT_DST_LINEAR   (1u << 18)
#define BLT_DST_CONTIG   (1u << 19)
#define BLT_PAT_MASK     (3u << 22)
#define BLT_PAT_DITHER   (1u << 22)
#define BLT_PAT_MONO     (2u << 22)
#define BLT_PAT_COLOR8   (3u << 22)
#define BLT_START        (1u << 31)

static uint16_t
pm_xr16(pm_t *pm, int addr)
{
    return pm->xr[addr] | (pm->xr[addr + 1] << 8);
}

static uint32_t
pm_xr32(pm_t *pm, int addr)
{
    return pm_xr16(pm, addr) | (pm_xr16(pm, addr + 2) << 16);
}

static void
pm_xr_set16(pm_t *pm, int addr, uint16_t val)
{
    pm->xr[addr]     = val & 0xff;
    pm->xr[addr + 1] = val >> 8;
}

/* Bytes per pixel of the current display format. */
static int
pm_bytes_pp(pm_t *pm)
{
    switch (pm->xr[0x80] & 0x0f) {
        case 0x0c:
        case 0x0d:
            return 2;
        case 0x0e:
            return 3;
        case 0x0f:
            return 4;
        default:
            break;
    }
    if (pm->ramdac && (pm->svga.bpp > 8))
        return (pm->svga.bpp + 7) >> 3;
    return 1;
}

static uint32_t
pm_lin_addr(uint32_t reg)
{
    return ((reg >> 16) << 12) | (reg & 0xfff);
}

/* A linear source or destination address. X.org's format: a pixel address
   with bits 23:12 in register bits 31:16. The AT3D's own Windows driver sets
   XR40 bit 17 instead and gives plain byte addresses, its rectangles' row
   strides in bytes in XR5C (destination) and XR5E (source). */
static uint32_t
pm_blt_lin(pm_t *pm, uint32_t reg)
{
    if ((pm->chip >= PM_AT3D) && (pm->blt.ctrl & BLT_BYTE_LINEAR))
        return reg & 0xffffff;
    return pm_lin_addr(reg) * pm->blt.bpp;
}

/* The step from one row of a linear rectangle to the next. */
static int
pm_blt_lin_step(pm_t *pm, int contig, int reg)
{
    const int w = pm->blt.w * pm->blt.bpp;

    if (contig)
        return w;
    if ((pm->chip >= PM_AT3D) && (pm->blt.ctrl & BLT_BYTE_LINEAR))
        return (int16_t) pm_xr16(pm, reg);
    return w + (int16_t) pm_xr16(pm, reg);
}

static uint32_t
pm_rop(uint8_t rop, uint32_t p, uint32_t s, uint32_t d)
{
    uint32_t r = 0;

    for (int i = 0; i < 8; i++) {
        if (!(rop & (1 << i)))
            continue;
        r |= ((i & 4) ? p : ~p) & ((i & 2) ? s : ~s) & ((i & 1) ? d : ~d);
    }
    return r;
}

static uint32_t
pm_vram_read(pm_t *pm, uint32_t addr, int bpp)
{
    const svga_t *svga = &pm->svga;
    uint32_t      val  = 0;

    for (int i = 0; i < bpp; i++)
        val |= svga->vram[(addr + i) & pm->vram_mask] << (i << 3);
    return val;
}

static void
pm_vram_write(pm_t *pm, uint32_t addr, int bpp, uint32_t val)
{
    svga_t *svga = &pm->svga;

    for (int i = 0; i < bpp; i++) {
        const uint32_t a = (addr + i) & pm->vram_mask;

        svga->vram[a]             = val >> (i << 3);
        svga->changedvram[a >> 12] = svga->monitor->mon_changeframecount;
    }
}

static int
pm_clipped(pm_t *pm, int x, int y)
{
    if (!(pm->xr[XR_CLIP_CTRL] & 0x01))
        return 0;
    return (x < (int16_t) pm_xr16(pm, 0x38)) || (y < (int16_t) pm_xr16(pm, 0x3a)) ||
           (x > (int16_t) pm_xr16(pm, 0x3c)) || (y > (int16_t) pm_xr16(pm, 0x3e));
}

/* The pattern colour at destination (x, y), or the foreground colour. */
static uint32_t
pm_pattern(pm_t *pm, int x, int y, int *valid)
{
    const uint32_t ctrl = pm->blt.ctrl;
    uint8_t        bits;

    *valid = 1;
    switch (ctrl & BLT_PAT_MASK) {
        case BLT_PAT_DITHER: {
            /* 4x4 nibbles: the 16 colours at the ends of the palette. */
            const uint32_t pat = (y & 2) ? pm_xr32(pm, 0x4c) : pm_xr32(pm, 0x48);
            const int      sh  = (((y & 1) << 4) | ((x & 3) << 2));
            uint8_t        c   = (pat >> sh) & 0x0f;

            return (c < 8) ? c : (c + 0xf0);
        }
        case BLT_PAT_COLOR8: {
            /* An 8x8 block of pixels in video memory at the source X/Y,
               aligned to the screen origin (X.org's HARDWARE_PATTERN_
               SCREEN_ORIGIN); transparency against the background colour. */
            const int      sx = (int16_t) pm_xr16(pm, 0x50) + (x & 7);
            const int      sy = (int16_t) pm_xr16(pm, 0x52) + (y & 7);
            const uint32_t c  = pm_vram_read(pm, sy * pm->blt.pitch + sx * pm->blt.bpp, pm->blt.bpp);

            if ((ctrl & BLT_SRC_XPARENT) && (c == pm_xr32(pm, 0x64)))
                *valid = 0;
            return c;
        }
        default:
            break;
    }
    if ((ctrl & BLT_SRC_PATTERN) || ((ctrl & BLT_PAT_MASK) == BLT_PAT_MONO)) {
        bits = pm->xr[0x48 + (y & 7)];
        if (bits & (0x80 >> (x & 7)))
            return pm_xr32(pm, 0x60);
        if (ctrl & BLT_SRC_XPARENT)
            *valid = 0;
        return pm_xr32(pm, 0x64);
    }
    return pm_xr32(pm, 0x60);
}

static void
pm_plot(pm_t *pm, uint32_t addr, int x, int y, uint32_t src, int has_src)
{
    const int      bpp  = pm->blt.bpp;
    const uint32_t mask = (bpp >= 4) ? 0xffffffff : ((1u << (bpp << 3)) - 1);
    uint32_t       pat;
    uint32_t       d;
    uint32_t       r;
    int            valid;

    if (pm_clipped(pm, x, y))
        return;

    pat = pm_pattern(pm, x, y, &valid);
    if (!valid)
        return;
    if (!has_src)
        src = pat;
    else if (!(pm->blt.ctrl & (BLT_SRC_PATTERN | BLT_PAT_MASK)))
        pat = src;

    d = pm_vram_read(pm, addr, bpp);
    r = pm_rop(pm->xr[0x46], pat, src, d) & mask;
    if (bpp == 1)
        r = (r & pm->xr[0x47]) | (d & ~pm->xr[0x47]);
    pm_vram_write(pm, addr, bpp, r);
}

static uint32_t
pm_blt_dst_addr(pm_t *pm, int x, int y)
{
    return (uint32_t) (y * pm->blt.pitch + x * pm->blt.bpp);
}

/* One row of a host-to-screen or screen-to-screen operation: src_at(i)
   gives the colour of pixel i of the row, or -1 for a transparent one. */
static void
pm_blt_row(pm_t *pm, const uint8_t *data, int skip, int mono)
{
    const int bpp = pm->blt.bpp;

    for (int i = 0; i < pm->blt.w; i++) {
        const int x = pm->blt.x + i * pm->blt.dx;
        uint32_t  addr;
        uint32_t  src;

        if (pm->blt.ctrl & BLT_DST_LINEAR)
            addr = (pm->blt.dst + i * pm->blt.dx * bpp);
        else
            addr = pm_blt_dst_addr(pm, x, pm->blt.y);

        if (mono) {
            const int bit = skip + i;

            if (data[bit >> 3] & (0x80 >> (bit & 7)))
                src = pm_xr32(pm, 0x60);
            else if (pm->blt.ctrl & BLT_SRC_XPARENT)
                continue;
            else
                src = pm_xr32(pm, 0x64);
        } else {
            src = 0;
            for (int b = 0; b < bpp; b++)
                src |= data[skip + i * bpp + b] << (b << 3);
        }
        pm_plot(pm, addr, x, pm->blt.y, src, 1);
    }
}

static void
pm_blt_next_row(pm_t *pm)
{
    pm->blt.row++;
    pm->blt.y += pm->blt.dy;
    if (pm->blt.ctrl & BLT_DST_LINEAR)
        pm->blt.dst += pm_blt_lin_step(pm, pm->blt.ctrl & BLT_DST_CONTIG, 0x5c) * pm->blt.dy;
}

static void
pm_blt_done(pm_t *pm)
{
    /* XR40 bits 28:27: leave the destination just past the top right (1)
       or the bottom left (2) corner, so the next operation can start
       without rewriting it. STB's polygon fill (physsl.asm) depends on it:
       bottom left with "start on destination X", one span after another,
       never rewriting the Y (it adds the height itself for empty spans), so
       the new Y is y + height. Lines (3, last pixel) stay where they end. */
    const int upd = (pm->blt.ctrl >> 27) & 3;

    if ((pm->blt.op != BLT_OP_NOP) && (pm->blt.op != BLT_OP_LINE) && (pm->blt.op != BLT_OP_LINE_NOEND) &&
        !(pm->blt.ctrl & BLT_DST_LINEAR)) {
        if (upd == 1) {
            pm_xr_set16(pm, 0x54, pm->blt.x0 + pm->blt.w * pm->blt.dx);
            pm_xr_set16(pm, 0x56, pm->blt.y0);
        } else if (upd == 2) {
            pm_xr_set16(pm, 0x54, pm->blt.x0);
            pm_xr_set16(pm, 0x56, pm->blt.y0 + pm->blt.h * pm->blt.dy);
        }
    }
    pm->blt.op = BLT_OP_NOP;
}

static void
pm_blt_line(pm_t *pm)
{
    int       x     = (int16_t) pm_xr16(pm, 0x54);
    int       y     = (int16_t) pm_xr16(pm, 0x56);
    const int axial = (int16_t) pm_xr16(pm, 0x70);
    const int diag  = (int16_t) pm_xr16(pm, 0x72);
    int       err   = (int16_t) pm_xr16(pm, 0x74);
    int       n     = pm->blt.w;

    if (pm->blt.op == BLT_OP_LINE_NOEND)
        n--;
    for (int i = 0; i < n; i++) {
        pm_plot(pm, pm_blt_dst_addr(pm, x, y), x, y, 0, 0);
        if (err >= 0) {
            x += pm->blt.dx;
            y += pm->blt.dy;
            err += diag;
        } else {
            if (pm->blt.ctrl & BLT_MAJOR_Y)
                y += pm->blt.dy;
            else
                x += pm->blt.dx;
            err += axial;
        }
    }
    pm_xr_set16(pm, 0x54, x);
    pm_xr_set16(pm, 0x56, y);
}

static void
pm_blt_screen_to_screen(pm_t *pm)
{
    const uint32_t ctrl = pm->blt.ctrl;
    const int      bpp  = pm->blt.bpp;
    uint8_t        row[16384];

    if (ctrl & BLT_SRC_MONO) {
        /* Mono source in video memory, rows of whole bytes, MSB first. */
        uint32_t  src   = pm_blt_lin(pm, pm_xr32(pm, 0x50));
        const int bytes = (pm->blt.w + 7) >> 3;

        for (int r = 0; r < pm->blt.h; r++) {
            for (int i = 0; i < bytes && i < (int) sizeof(row); i++)
                row[i] = pm->svga.vram[(src + i) & pm->vram_mask];
            pm_blt_row(pm, row, 0, 1);
            src += bytes;
            pm_blt_next_row(pm);
        }
        return;
    }

    {
        int      sx = (int16_t) pm_xr16(pm, 0x50);
        int      sy = (int16_t) pm_xr16(pm, 0x52);
        uint32_t src;

        if (ctrl & BLT_SRC_LINEAR)
            src = pm_blt_lin(pm, pm_xr32(pm, 0x50));
        else
            src = sy * pm->blt.pitch + sx * bpp;

        for (int r = 0; r < pm->blt.h; r++) {
            uint32_t a = src;

            /* Read the whole row first: the copy may overlap itself. */
            for (int i = 0; i < pm->blt.w && (i + 1) * bpp <= (int) sizeof(row); i++) {
                const uint32_t v = pm_vram_read(pm, a, bpp);

                for (int b = 0; b < bpp; b++)
                    row[i * bpp + b] = v >> (b << 3);
                a += pm->blt.dx * bpp;
            }
            pm_blt_row(pm, row, 0, 0);

            if (ctrl & BLT_SRC_LINEAR)
                src += pm_blt_lin_step(pm, ctrl & BLT_SRC_CONTIG, pm_xr16(pm, 0x5e) ? 0x5e : 0x5c) * pm->blt.dy;
            else
                src += pm->blt.pitch * pm->blt.dy;
            pm_blt_next_row(pm);
        }
    }
}

static void
pm_blt_screen_to_host(pm_t *pm)
{
    const int bpp   = pm->blt.bpp;
    const int skip  = (pm_xr16(pm, 0x54) & 3) * bpp & 3;
    const int bytes = ((skip + pm->blt.w * bpp) + 3) & ~3;
    int       sx    = (int16_t) pm_xr16(pm, 0x50);
    int       sy    = (int16_t) pm_xr16(pm, 0x52);
    uint32_t  need  = (uint32_t) bytes * pm->blt.h;

    if (need > pm->blt.rd_size) {
        free(pm->blt.rd_buf);
        pm->blt.rd_buf  = malloc(need);
        pm->blt.rd_size = need;
    }
    memset(pm->blt.rd_buf, 0, need);
    for (int r = 0; r < pm->blt.h; r++) {
        uint8_t *p = pm->blt.rd_buf + r * bytes + skip;
        uint32_t a = (sy + r * pm->blt.dy) * pm->blt.pitch + sx * bpp;

        for (int i = 0; i < pm->blt.w * bpp; i++)
            p[i] = pm->svga.vram[(a + i) & pm->vram_mask];
    }
    pm->blt.rd_len = need;
    pm->blt.rd_pos = 0;
}

static void
pm_blt_start(pm_t *pm)
{
    static const int models[8] = { 0, 640, 800, 0, 1024, 1152, 1280, 1600 };
    const uint32_t   ctrl      = pm_xr32(pm, XR_BLT_CTRL);
    const int        model     = models[(ctrl >> 24) & 7];

    pm->blt.ctrl = ctrl;
    pm->blt.op   = ctrl & 0x0f;
    pm->blt.bpp  = (ctrl & BLT_24BPP) ? 1 : pm_bytes_pp(pm);
    if (pm->chip >= PM_AT3D) {
        /* XR40 bits 16:14: 1 = 8, 2 = 16, 3 = 32 bpp; 4 = 24 bpp, worked
           byte by byte with linear addresses (X.org's 24 bpp paths). */
        static const int depth[8] = { 0, 1, 2, 4, 1, 0, 0, 0 };

        if (depth[(ctrl >> 14) & 7])
            pm->blt.bpp = depth[(ctrl >> 14) & 7];
    }
    pm->blt.pitch = model ? (model * pm->blt.bpp) : ((int16_t) pm_xr16(pm, 0x5c));
    pm->blt.w    = pm_xr16(pm, 0x58);
    pm->blt.h    = pm_xr16(pm, 0x5a);
    pm->blt.dx   = (ctrl & BLT_DIR_XNEG) ? -1 : 1;
    pm->blt.dy   = (ctrl & BLT_DIR_YNEG) ? -1 : 1;
    pm->blt.x    = (int16_t) pm_xr16(pm, 0x54);
    pm->blt.y    = (int16_t) pm_xr16(pm, 0x56);
    pm->blt.x0   = pm->blt.x;
    pm->blt.y0   = pm->blt.y;
    pm->blt.dst  = pm_blt_lin(pm, pm_xr32(pm, 0x54));
    pm->blt.row  = 0;
    pm->blt.host_len = 0;

    if (pm_do_log)
        pm_log("PM: BLT %08X rop %02X src %08X dst %08X dim %08X fg %08X bg %08X clip %02X\n", ctrl, pm->xr[0x46],
               pm_xr32(pm, 0x50), pm_xr32(pm, 0x54), pm_xr32(pm, 0x58), pm_xr32(pm, 0x60), pm_xr32(pm, 0x64),
               pm->xr[XR_CLIP_CTRL]);

    switch (pm->blt.op) {
        case BLT_OP_NOP:
            break;

        case BLT_OP_RECT:
        case BLT_OP_STRIP:
            for (int r = 0; r < pm->blt.h; r++) {
                for (int i = 0; i < pm->blt.w; i++) {
                    const int x = pm->blt.x + i * pm->blt.dx;
                    uint32_t  addr;

                    if (ctrl & BLT_DST_LINEAR)
                        addr = pm->blt.dst + i * pm->blt.dx * pm->blt.bpp;
                    else
                        addr = pm_blt_dst_addr(pm, x, pm->blt.y);
                    pm_plot(pm, addr, x, pm->blt.y, 0, 0);
                }
                pm_blt_next_row(pm);
            }
            pm_blt_done(pm);
            break;

        case BLT_OP_SS:
            if ((pm->chip >= PM_AT3D) && ((ctrl & BLT_PAT_MASK) >= BLT_PAT_MONO)) {
                /* With the 8x8 pattern unit selected the source registers
                   locate the pattern: a pattern fill. */
                for (int r = 0; r < pm->blt.h; r++) {
                    for (int i = 0; i < pm->blt.w; i++) {
                        const int x = pm->blt.x + i * pm->blt.dx;

                        pm_plot(pm, pm_blt_dst_addr(pm, x, pm->blt.y), x, pm->blt.y, 0, 0);
                    }
                    pm_blt_next_row(pm);
                }
            } else
                pm_blt_screen_to_screen(pm);
            pm_blt_done(pm);
            break;

        case BLT_OP_MS:
            /* Rows come through the blit port, each padded to a dword --
               or, with a contiguous mono source, to a byte (the AT3D
               driver's glyphs: (w + 7) / 8 * h bytes, the whole padded to a
               dword). */
            if ((ctrl & BLT_SRC_MONO) && (ctrl & BLT_SRC_CONTIG)) {
                pm->blt.host_skip      = pm->xr[0x50] & 7;
                pm->blt.host_row_bytes = (pm->blt.host_skip + pm->blt.w + 7) >> 3;
            } else if (ctrl & BLT_SRC_MONO) {
                pm->blt.host_skip      = pm->xr[0x50] & 0x1f;
                pm->blt.host_row_bytes = ((pm->blt.host_skip + pm->blt.w + 31) >> 5) << 2;
            } else {
                pm->blt.host_skip      = (pm->xr[0x50] & 3) * pm->blt.bpp & 3;
                pm->blt.host_row_bytes = (pm->blt.host_skip + pm->blt.w * pm->blt.bpp + 3) & ~3;
            }
            if ((pm->blt.w <= 0) || (pm->blt.h <= 0) || (pm->blt.host_row_bytes > (int) sizeof(pm->blt.host)))
                pm_blt_done(pm);
            break;

        case BLT_OP_SM:
            if ((pm->blt.w > 0) && (pm->blt.h > 0))
                pm_blt_screen_to_host(pm);
            pm_blt_done(pm);
            break;

        case BLT_OP_LINE:
        case BLT_OP_LINE_NOEND:
            pm_blt_line(pm);
            pm_blt_done(pm);
            break;

        default:
            pm_log("PM: BLT unhandled operation %X\n", pm->blt.op);
            pm_blt_done(pm);
            break;
    }
}

/* A dword written to the blit port. */
static void
pm_blt_port_write(pm_t *pm, uint32_t val, int bytes)
{
    if (pm->blt.op != BLT_OP_MS)
        return;

    for (int i = 0; i < bytes; i++) {
        pm->blt.host[pm->blt.host_len++] = val >> (i << 3);
        if (pm->blt.host_len >= pm->blt.host_row_bytes) {
            pm_blt_row(pm, pm->blt.host, pm->blt.host_skip, !!(pm->blt.ctrl & BLT_SRC_MONO));
            pm->blt.host_len = 0;
            pm_blt_next_row(pm);
            if (pm->blt.row >= pm->blt.h) {
                pm_blt_done(pm);
                return;
            }
        }
    }
}

static uint8_t
pm_blt_port_read(pm_t *pm)
{
    if (pm->blt.rd_pos < pm->blt.rd_len)
        return pm->blt.rd_buf[pm->blt.rd_pos++];
    return 0xff;
}

static void pm_xr_access_done(pm_t *pm);

/* The blit port in the B8000h window (SR1B bits 5:3 = 3, the AT3D driver's
   choice; 4 puts it at the end of the aperture). Any address there is the
   port. */
static uint8_t
pm_port_readb(UNUSED(uint32_t addr), void *priv)
{
    return pm_blt_port_read((pm_t *) priv);
}

static uint16_t
pm_port_readw(UNUSED(uint32_t addr), void *priv)
{
    pm_t *pm = (pm_t *) priv;

    return pm_blt_port_read(pm) | (pm_blt_port_read(pm) << 8);
}

static uint32_t
pm_port_readl(UNUSED(uint32_t addr), void *priv)
{
    pm_t *pm = (pm_t *) priv;

    return pm_port_readw(addr, pm) | (pm_port_readw(addr, pm) << 16);
}

static void
pm_port_writeb(UNUSED(uint32_t addr), uint8_t val, void *priv)
{
    pm_blt_port_write((pm_t *) priv, val, 1);
    pm_xr_access_done((pm_t *) priv);
}

static void
pm_port_writew(UNUSED(uint32_t addr), uint16_t val, void *priv)
{
    pm_blt_port_write((pm_t *) priv, val, 2);
    pm_xr_access_done((pm_t *) priv);
}

static void
pm_port_writel(UNUSED(uint32_t addr), uint32_t val, void *priv)
{
    pm_blt_port_write((pm_t *) priv, val, 4);
    pm_xr_access_done((pm_t *) priv);
}

/* ------------------------------------------------------------------------- */
/* Video window (XR82-XR9F, the first ProMotion design: one window)          */
/*                                                                           */
/* XR82 control: bit 0 on, bits 3:1 source depth (2 = 8, 4 = 15, 5 = 16,     */
/* 7 = 32 bpp), bits 6:4 format (0 indexed, 2 RGB, 4 UYVY, 5 YYVU, 7 Y),     */
/* 8 YUV to RGB, 9 stretch, 10 interpolate, 13:12 replication. XR84 source   */
/* width, XR86 source pitch in dwords, XR88/8A/8C/8E left/top/right/bottom   */
/* on the screen, XR90 source address, XR98/9C source width/height - 1,      */
/* XR9A/9E source - destination width/height (STB's promdd32.c). No colour   */
/* key on this design: the window covers its rectangle.                      */
/* ------------------------------------------------------------------------- */

static void
pm_update_overlay(pm_t *pm)
{
    svga_t        *svga = &pm->svga;
    const uint16_t ctl  = pm_xr16(pm, 0x82);
    const int      l    = pm_xr16(pm, 0x88) & 0xfff;
    const int      t    = pm_xr16(pm, 0x8a) & 0xfff;
    const int      r    = pm_xr16(pm, 0x8c) & 0xfff;
    const int      b    = pm_xr16(pm, 0x8e) & 0xfff;

    svga->overlay.ena       = (ctl & 0x0001) && (r >= l) && (b >= t) && (pm->sr[0x1c - 8] & 0x04);
    svga->overlay.x         = l;
    svga->overlay.y         = t;
    svga->overlay.cur_xsize = r - l + 1;
    svga->overlay.cur_ysize = b - t + 1;
    svga->overlay.xoff      = 0;
    svga->overlay.yoff      = 0;
}

static inline uint8_t
pm_clamp8(int v)
{
    return (v < 0) ? 0 : ((v > 255) ? 255 : v);
}

static uint32_t
pm_yuv_rgb(int y, int u, int v)
{
    const int c = (y - 16) * 298;
    const int d = u - 128;
    const int e = v - 128;

    return (pm_clamp8((c + 409 * e + 128) >> 8) << 16) | (pm_clamp8((c - 100 * d - 208 * e + 128) >> 8) << 8) |
           pm_clamp8((c + 516 * d + 128) >> 8);
}

static uint32_t
pm_overlay_pixel(pm_t *pm, uint16_t ctl, uint32_t line, int sx)
{
    const uint8_t *vram = pm->svga.vram;
    const uint32_t mask = pm->vram_mask;

    switch ((ctl >> 4) & 7) {
        case 4: /* UYVY */
        case 5: /* YYVU */
        case 7: { /* Y */
            const uint32_t a  = line + (sx & ~1) * 2;
            const uint8_t  b0 = vram[a & mask];
            const uint8_t  b1 = vram[(a + 1) & mask];
            const uint8_t  b2 = vram[(a + 2) & mask];
            const uint8_t  b3 = vram[(a + 3) & mask];
            int            y;
            int            u;
            int            v;

            if (((ctl >> 4) & 7) == 7) {
                y = vram[(line + sx) & mask];
                return (y << 16) | (y << 8) | y;
            }
            if (((ctl >> 4) & 7) == 5) {
                y = (sx & 1) ? b1 : b0;
                v = b2;
                u = b3;
            } else {
                y = (sx & 1) ? b3 : b1;
                u = b0;
                v = b2;
            }
            if (!(ctl & 0x0100))
                return (y << 16) | (u << 8) | v;
            return pm_yuv_rgb(y, u, v);
        }

        case 2: /* RGB */
            switch ((ctl >> 1) & 7) {
                case 4: {
                    const uint16_t c = vram[(line + sx * 2) & mask] | (vram[(line + sx * 2 + 1) & mask] << 8);

                    return video_15to32[c & 0x7fff];
                }
                case 5: {
                    const uint16_t c = vram[(line + sx * 2) & mask] | (vram[(line + sx * 2 + 1) & mask] << 8);

                    return video_16to32[c];
                }
                case 7:
                    return (vram[(line + sx * 4 + 2) & mask] << 16) | (vram[(line + sx * 4 + 1) & mask] << 8) |
                           vram[(line + sx * 4) & mask];
                default:
                    break;
            }
            return pm->svga.pallook[vram[(line + sx) & mask]];

        default: /* indexed */
            return pm->svga.pallook[vram[(line + sx) & mask]];
    }
}

static void
pm_overlay_draw(svga_t *svga, int displine)
{
    pm_t          *pm   = (pm_t *) svga->priv;
    const uint16_t ctl  = pm_xr16(pm, 0x82);
    const int      dw   = svga->overlay_latch.cur_xsize;
    const int      dh   = svga->overlay_latch.cur_ysize;
    const int      row  = dh - svga->overlay_on;
    int            sw   = pm_xr16(pm, 0x84);
    int            sh   = dh;
    uint32_t      *p    = &svga->monitor->target_buffer->line[displine][svga->x_add];
    uint32_t       line;
    int            sy;

    if (ctl & 0x0200) {
        sw = (pm_xr16(pm, 0x98) & 0xfff) + 1;
        sh = (pm_xr16(pm, 0x9c) & 0xfff) + 1;
    }
    if ((sw <= 0) || (dw <= 0) || (dh <= 0) || (row < 0))
        return;

    sy   = (row * sh) / dh;
    line = pm_xr32(pm, 0x90) + sy * pm_xr16(pm, 0x86) * 4;
    for (int i = 0; i < dw; i++) {
        const int x = svga->overlay_latch.x + i;

        if (x >= svga->hdisp)
            break;
        if ((ctl & 0x0400) && (sw < dw)) {
            /* VC_INTERP (bit 10): a stretched window blends neighbouring
               source pixels horizontally (the second design calls the same
               bit VC2_HOR_INTERP). */
            const int      pos = (i * sw * 256) / dw;
            const int      sx  = pos >> 8;
            const int      f   = pos & 0xff;
            const uint32_t c0  = pm_overlay_pixel(pm, ctl, line, sx);
            const uint32_t c1  = f ? pm_overlay_pixel(pm, ctl, line, (sx + 1 < sw) ? (sx + 1) : sx) : c0;
            uint32_t       out = 0;

            for (int s = 0; s < 24; s += 8)
                out |= (uint32_t) (((((c0 >> s) & 0xff) * (256 - f)) + (((c1 >> s) & 0xff) * f)) >> 8) << s;
            p[x] = out;
        } else
            p[x] = pm_overlay_pixel(pm, ctl, line, (i * sw) / dw);
    }
}

/* ------------------------------------------------------------------------- */
/* Hardware cursor (XR140-XR14D)                                             */
/* ------------------------------------------------------------------------- */

static void
pm_update_cursor(pm_t *pm)
{
    svga_t *svga = &pm->svga;

    /* The cursor belongs to the extended modes: Windows leaves it on when it
       goes back to a VGA mode. */
    svga->hwcursor.ena       = (pm->xr[0x140] & 0x01) && (pm->sr[0x1c - 8] & 0x04);
    svga->hwcursor.cur_xsize = 64;
    svga->hwcursor.cur_ysize = 64;
    svga->hwcursor.x         = pm_xr16(pm, 0x148) & 0x7ff;
    svga->hwcursor.y         = pm_xr16(pm, 0x14a) & 0x7ff;
    svga->hwcursor.xoff      = pm->xr[0x14c] & 0x3f;
    svga->hwcursor.yoff      = pm->xr[0x14d] & 0x3f;
    svga->hwcursor.addr      = ((pm_xr16(pm, 0x144) << 10) + svga->hwcursor.yoff * 16) & pm->vram_mask;
}

static uint32_t
pm_cursor_color(pm_t *pm, uint8_t c)
{
    if (pm_bytes_pp(pm) == 1)
        return pm->svga.pallook[c];
    /* Direct colour modes: RGB 3:3:2. */
    return ((c & 0xe0) * 0x124 / 0x20) << 16 | (((c & 0x1c) >> 2) * 0x24) << 8 | ((c & 0x03) * 0x55);
}

static void
pm_hwcursor_draw(svga_t *svga, int displine)
{
    pm_t          *pm   = (pm_t *) svga->priv;
    const uint8_t *src  = &svga->vram[svga->hwcursor_latch.addr & pm->vram_mask];
    uint32_t      *line = &svga->monitor->target_buffer->line[displine][svga->x_add];
    const uint32_t c0   = pm_cursor_color(pm, pm->xr[0x142]);
    const uint32_t c1   = pm_cursor_color(pm, pm->xr[0x141]);
    int            x    = svga->hwcursor_latch.x;

    for (int i = svga->hwcursor_latch.xoff; i < 64; i++, x++) {
        const int c = (src[i >> 2] >> ((i & 3) << 1)) & 3;

        if (x >= svga->hdisp)
            break;
        switch (c) {
            case 0:
                line[x] = c0;
                break;
            case 1:
                line[x] = c1;
                break;
            case 3:
                line[x] ^= 0xffffff;
                break;
            default:
                break;
        }
    }
    svga->hwcursor_latch.addr += 16;
}

/* ------------------------------------------------------------------------- */
/* Extended register access                                                  */
/* ------------------------------------------------------------------------- */

static uint8_t
pm_xr_read(pm_t *pm, uint32_t addr)
{
    uint8_t ret;

    addr &= (XR_SIZE - 1);
    switch (addr) {
        case XR_STATUS:
            ret = 0x08; /* FIFO empty: eight free entries */
            break;
        case XR_STATUS + 1:
            ret = 0x00;
            if (pm->blt.op == BLT_OP_MS)
                ret |= 0x01; /* the blit port wants data */
            if (pm->blt.rd_pos < pm->blt.rd_len)
                ret |= 0x02; /* screen-to-host data ready */
            break;
        case XR_STATUS + 2:
            /* The DDC lines: bit 16 SDA, bit 17 SCL. */
            ret = (i2c_gpio_get_sda(pm->i2c) ? 0x01 : 0x00) | (i2c_gpio_get_scl(pm->i2c) ? 0x02 : 0x00);
            break;
        case XR_STATUS + 3:
            ret = 0x00;
            break;
        case 0x180 ... 0x183:
            /* The PCI vendor and device IDs (the NT 4 miniport checks for
               1142h here before it accepts the card). */
            ret = pm->pci_regs[addr & 3];
            break;
        case 0x1f3:
            /* Bit 30 of XR1F0: no Voodoo Rush on the board. Alliance's AT3D
               driver checks it (after XRDB = 4) and otherwise hands
               DirectDraw the Rush's VG96DD32.DLL, which fails to load. */
            ret = pm->xr[addr] | ((pm->chip >= PM_AT3D) ? 0x40 : 0x00);
            break;
        case 0x1f4 ... 0x1f7:
            /* The AT3D's SST status (Voodoo Rush): idle. */
            ret = 0x00;
            break;
        case 0x190 ... 0x193:
            /* The aperture base: on the PCI chips, BAR 0 (the drivers read
               it here). The BIOS writes a VL-bus address of its choosing. */
            ret = pm->pci_regs[0x10 + (addr & 3)];
            break;
        case 0x194 ... 0x195:
            /* The XR I/O window's base (6422): PCI BAR 1. The BIOS copies it
               into SR1E/SR1F. */
            ret = (pm->chip >= PM_6422) ? (pm->pci_regs[0x14 + (addr & 1)] & ((addr & 1) ? 0xff : 0xfc)) : pm->xr[addr];
            break;
        default:
            ret = pm->xr[addr];
            break;
    }
    if (pm_do_log >= 2)
        pm_log("PM: XR R %03X = %02X\n", addr, ret);
    return ret;
}

static void
pm_xr_write(pm_t *pm, uint32_t addr, uint8_t val)
{
    addr &= (XR_SIZE - 1);
    if (pm_do_log >= 2 || (pm_do_log && (addr >= 0x80) && (addr != 0xc0) && (addr != 0xc1) && ((addr < 0x140) || (addr > 0x14f))))
        pm_log("PM: XR W %03X = %02X\n", addr, val);
    /* Not on the 6410/6422: the tile registers of the second video window
       design (XR200 on) and XR40 bit 16 (24 bpp). The drivers write them
       to tell the generations apart (STB's Detect_Overlay_Hardware_Version
       and Detect_2D_Hardware_Version). */
    if ((addr >= 0x200) && (addr < 0x2c0) && (pm->chip < PM_AT3D))
        return;
    if ((addr == 0x42) && (pm->chip < PM_AT3D))
        val &= ~0x01;
    pm->xr[addr] = val;

    switch (addr) {
        case 0x43:
            if (val & 0x80)
                pm->blt_pending = 1;
            else if (pm->qs_written & (1 << ((val >> 5) & 3)))
                /* Arming a quick start whose register was written after
                   the last operation starts it: Alliance's polygon code
                   clears XR40 at the end of a batch, writes the next
                   span's extent and destination, then re-arms "start on
                   destination" and expects that span drawn. */
                pm->blt_pending = 1;
            break;
        case 0x50 ... 0x5b: {
            /* Quick start (XR40 bits 30:29) on the X extent (1), source (2)
               or destination (3) X -- the low word: the AT3D driver's
               polygon fill writes a span's Y first and its X last. The
               operation starts when the access ends, after a dword write
               has delivered both halves (the STB driver writes each glyph's
               width and height at once). */
            static const uint8_t qs_reg[4] = { 0, 0x58, 0x50, 0x54 };
            const int            qs        = (pm_xr32(pm, XR_BLT_CTRL) >> 29) & 3;
            const int            mode      = ((addr & ~3) == 0x58) ? 1 : (((addr & ~3) == 0x50) ? 2 : 3);

            if (qs && ((addr & ~1) == qs_reg[qs]))
                pm->blt_pending = 1;
            else if ((addr & ~3) != 0x5c)
                pm->qs_written |= 1 << mode;
            break;
        }
        case 0x80:
            svga_recalctimings(&pm->svga);
            break;
        case 0xc0:
        case 0xc1:
            pm_update_banks(pm);
            break;
        case 0xd0:
            /* DDC: bits 5/6 drive SCL/SDA with the levels in bits 3/4;
               released, a line floats high. */
            i2c_gpio_set(pm->i2c, !(val & 0x20) || (val & 0x08), !(val & 0x40) || (val & 0x10));
            break;
        case 0x140 ... 0x14f:
            pm_update_cursor(pm);
            break;
        case 0x82 ... 0x9f:
            pm_update_overlay(pm);
            break;
        case 0x1ff:
            /* Abort. */
            pm_blt_done(pm);
            pm->blt.rd_len = pm->blt.rd_pos = 0;
            break;
        case 0xd8:
        case 0xec ... 0xef:
            svga_recalctimings(&pm->svga);
            break;
        default:
            break;
    }
}

/* The end of a register access: run the operation it started. */
static void
pm_xr_access_done(pm_t *pm)
{
    if (pm->blt_pending) {
        pm->blt_pending = 0;
        pm->qs_written  = 0;
        pm_blt_start(pm);
    }
}

/* The 6422's XR I/O window: four ports at SR1F:SR1E, SR1D the dword index. */
static uint8_t
pm_xr_io_read(uint16_t port, void *priv)
{
    pm_t *pm = (pm_t *) priv;

    return pm_xr_read(pm, (pm->sr[0x1d - 8] << 2) + (port & 3));
}

static void
pm_xr_io_write(uint16_t port, uint8_t val, void *priv)
{
    pm_t *pm = (pm_t *) priv;

    pm_xr_write(pm, (pm->sr[0x1d - 8] << 2) + (port & 3), val);
    pm_xr_access_done(pm);
}

static void
pm_xr_io_writew(uint16_t port, uint16_t val, void *priv)
{
    pm_t          *pm   = (pm_t *) priv;
    const uint32_t addr = (pm->sr[0x1d - 8] << 2) + (port & 3);

    pm_xr_write(pm, addr, val & 0xff);
    pm_xr_write(pm, addr + 1, val >> 8);
    pm_xr_access_done(pm);
}

static void
pm_xr_io_writel(uint16_t port, uint32_t val, void *priv)
{
    pm_t          *pm   = (pm_t *) priv;
    const uint32_t addr = (pm->sr[0x1d - 8] << 2) + (port & 3);

    for (int i = 0; i < 4; i++)
        pm_xr_write(pm, addr + i, val >> (i << 3));
    pm_xr_access_done(pm);
}

static void
pm_update_xr_io(pm_t *pm)
{
    const uint16_t base = (pm->sr[0x1f - 8] << 8) | (pm->sr[0x1e - 8] & 0xfc);

    if (pm->xr_io_base)
        io_removehandler(pm->xr_io_base, 4, pm_xr_io_read, NULL, NULL, pm_xr_io_write, pm_xr_io_writew, pm_xr_io_writel, pm);
    pm->xr_io_base = 0;
    /* Never over the VGA ports or the legacy ones below 100h. */
    if ((pm->chip >= PM_6422) && (base >= 0x100) && ((base & 0xffe0) != 0x03c0) && ((base & 0xfff0) != 0x03b0) &&
        (pm->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_IO)) {
        pm->xr_io_base = base;
        io_sethandler(base, 4, pm_xr_io_read, NULL, NULL, pm_xr_io_write, pm_xr_io_writew, pm_xr_io_writel, pm);
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
    pm_xr_access_done((pm_t *) priv);
}

static void
pm_xr_writew(uint32_t addr, uint16_t val, void *priv)
{
    pm_t *pm = (pm_t *) priv;

    pm_xr_write(pm, addr, val & 0xff);
    pm_xr_write(pm, addr + 1, val >> 8);
    pm_xr_access_done(pm);
}

static void
pm_xr_writel(uint32_t addr, uint32_t val, void *priv)
{
    pm_t *pm = (pm_t *) priv;

    for (int i = 0; i < 4; i++)
        pm_xr_write(pm, addr + i, val >> (i << 3));
    pm_xr_access_done(pm);
}

/* ------------------------------------------------------------------------- */
/* Linear aperture                                                           */
/* ------------------------------------------------------------------------- */

static void    pm_out(uint16_t addr, uint8_t val, void *priv);
static uint8_t pm_in(uint16_t addr, void *priv);

/* The AT3D's 16 MB aperture (X.org's apm driver): video memory at 0 MB with
   the blit port over its top 32 KB, all of video memory again at 8 MB, the
   extended registers at FFEC00h and the VGA ports at FFF000h + port.
   Returns 1 and the XR offset, 2 for a VGA port, 3 for the blit port, 0 for
   video memory (*off the video memory address), -1 for nothing. */
static int
pm_at3d_decode(pm_t *pm, uint32_t addr, uint32_t *off)
{
    addr &= 0xffffff;
    if ((addr >= 0xffec00) && (addr < 0xfff000)) {
        *off = addr - 0xffec00;
        return 1;
    }
    if (addr >= 0xfff000) {
        *off = addr & 0x3ff;
        return 2;
    }
    /* SR1B as on the older chips, in the first 4 MB: bits 2:0 = 4 put the
       registers in its last 2 KB (the 6422 drivers, e.g. the NT 4 1.410
       miniport, look there), bits 5:3 = 4 the blit port below them. */
    if ((addr >= 0x3ff800) && (addr < 0x400000) && ((pm->sr[0x1b - 8] & 0x07) == 0x04)) {
        *off = addr - 0x3ff800;
        return 1;
    }
    if ((addr >= 0x3f8000) && (addr < 0x400000) && ((pm->sr[0x1b - 8] & 0x38) == 0x20))
        return 3;
    if ((addr < 0x400000) || ((addr >= 0x800000) && (addr < 0xc00000))) {
        *off = addr & 0x3fffff;
        return 0;
    }
    return -1;
}

static int
pm_lfb_is_xr(pm_t *pm, uint32_t addr)
{
    /* SR1B bit 2: the register block occupies the last 2 KB of the
       aperture (the 30 KB below it is the blit port). */
    return (pm->sr[0x1b - 8] & 0x04) && ((addr & (pm->lfb_size - 1)) >= (pm->lfb_size - XR_SIZE));
}

/* SR1B bits 5:3 map the blit port into the 30 KB below the registers. */
static int
pm_lfb_is_port(pm_t *pm, uint32_t addr)
{
    addr &= (pm->lfb_size - 1);
    return (pm->sr[0x1b - 8] & 0x38) && (addr >= (pm->lfb_size - 0x8000)) && (addr < (pm->lfb_size - XR_SIZE));
}

static uint8_t
pm_lfb_readb(uint32_t addr, void *priv)
{
    pm_t   *pm   = (pm_t *) priv;
    svga_t *svga = &pm->svga;

    addr -= pm->lfb_base;
    if (pm->chip >= PM_AT3D) {
        uint32_t off = 0;

        switch (pm_at3d_decode(pm, addr, &off)) {
            case 1:
                return pm_xr_read(pm, off);
            case 2:
                return ((off >= 0x3b0) && (off < 0x3e0)) ? pm_in(off, pm) : 0xff;
            case 3:
                return pm_blt_port_read(pm);
            case 0:
                return (off < pm->vram_size) ? svga->vram[off] : 0xff;
            default:
                return 0xff;
        }
    }
    if (pm_lfb_is_xr(pm, addr))
        return pm_xr_read(pm, addr);
    if (pm_lfb_is_port(pm, addr))
        return pm_blt_port_read(pm);
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
pm_lfb_write_raw(uint32_t addr, uint8_t val, void *priv)
{
    pm_t   *pm   = (pm_t *) priv;
    svga_t *svga = &pm->svga;

    addr -= pm->lfb_base;
    if (pm->chip >= PM_AT3D) {
        uint32_t off = 0;

        switch (pm_at3d_decode(pm, addr, &off)) {
            case 1:
                pm_xr_write(pm, off, val);
                return;
            case 2:
                if ((off >= 0x3b0) && (off < 0x3e0))
                    pm_out(off, val, pm);
                return;
            case 3:
                pm_blt_port_write(pm, val, 1);
                return;
            case 0:
                if (off < pm->vram_size) {
                    svga->vram[off]               = val;
                    svga->changedvram[off >> 12] = svga->monitor->mon_changeframecount;
                }
                return;
            default:
                return;
        }
    }
    if (pm_lfb_is_xr(pm, addr)) {
        pm_xr_write(pm, addr, val);
        return;
    }
    if (pm_lfb_is_port(pm, addr)) {
        pm_blt_port_write(pm, val, 1);
        return;
    }
    addr &= svga->decode_mask;
    if (addr >= pm->vram_size)
        return;
    svga->vram[addr]                   = val;
    svga->changedvram[addr >> 12] = svga->monitor->mon_changeframecount;
}

static void
pm_lfb_writeb(uint32_t addr, uint8_t val, void *priv)
{
    pm_lfb_write_raw(addr, val, priv);
    pm_xr_access_done((pm_t *) priv);
}

static void
pm_lfb_writew(uint32_t addr, uint16_t val, void *priv)
{
    pm_lfb_write_raw(addr, val & 0xff, priv);
    pm_lfb_write_raw(addr + 1, val >> 8, priv);
    pm_xr_access_done((pm_t *) priv);
}

static void
pm_lfb_writel(uint32_t addr, uint32_t val, void *priv)
{
    for (int i = 0; i < 4; i++)
        pm_lfb_write_raw(addr + i, val >> (i << 3), priv);
    pm_xr_access_done((pm_t *) priv);
}

static void
pm_recalc_mappings(pm_t *pm)
{
    svga_t *svga = &pm->svga;

    if (!(pm->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_MEM)) {
        mem_mapping_disable(&svga->mapping);
        mem_mapping_disable(&pm->lfb_mapping);
        mem_mapping_disable(&pm->xr_mapping);
        mem_mapping_disable(&pm->port_mapping);
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

    if ((pm->sr[0x1b - 8] & 0x38) == 0x18)
        mem_mapping_set_addr(&pm->port_mapping, 0xb8000, 0x8000);
    else
        mem_mapping_disable(&pm->port_mapping);

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
        case 0x3c4:
            /* The AT3D decodes six index bits: its BIOS sizes memory with
               SR1C written as index DCh (its own encoding, bits 7:6 = SR). */
            if (pm->chip >= PM_AT3D)
                val &= 0x3f;
            break;

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
                            pm_update_banks(pm);
                            pm_recalc_mappings(pm);
                            svga_recalctimings(svga);
                        }
                        break;
                    case 0x1e:
                    case 0x1f:
                        pm_update_xr_io(pm);
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
                        svga->fullchange = 3;
                        /* Extended modes: CR1C bits 3:0 above, as in
                           pm_recalctimings (a DirectDraw flip writes all
                           three). In dwords: the core makes it bytes. */
                        if (pm_ext_bpp(pm))
                            svga->memaddr_latch = (((pm->cr[0x1c] & 0x0f) << 16) | (svga->crtc[0x0c] << 8) | svga->crtc[0x0d]);
                        else
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

/* XR80 bits 3:0, the pixel format of the extended (packed) modes. 02h is
   also there in mode 13h: what sets the extended modes apart is SR1C, the
   "flat model" control (bit 2; 05h in the BIOS's modes, 25h under the
   Windows driver, 00h in the VGA modes). */
static int
pm_ext_bpp(pm_t *pm)
{
    if (!(pm->sr[0x1c - 8] & 0x04))
        return 0;

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

    pm_update_cursor(pm);
    pm_update_overlay(pm);
    svga->force_old_addr = 0;
    svga->hoverride      = 0;

    /* CR1A: vertical bit 10s (total, display end, blank start, retrace
       start, and bit 4 line compare -- the AT3D BIOS's 1600x1200 modes);
       CR1B: horizontal bit 8s (total, display end), X.org's ApmModeInit.
       1280x1024 needs them. */
    if (pm->cr[0x1a] & 0x10)
        svga->split += 0x400; /* the core has added 1 already */
    if (pm->cr[0x1a] & 0x01)
        svga->vtotal |= 0x400;
    if (pm->cr[0x1a] & 0x02)
        svga->dispend |= 0x400;
    if (pm->cr[0x1a] & 0x04)
        svga->vblankstart |= 0x400;
    if (pm->cr[0x1a] & 0x08)
        svga->vsyncstart |= 0x400;
    if (pm->cr[0x1b] & 0x01)
        svga->htotal |= 0x100;
    if (pm->cr[0x1b] & 0x02) {
        svga->hdisp_time |= 0x100;
        svga->hdisp |= 0x100 * svga->dots_per_clock;
    }

    /* The 6422's pixel PLL, XREC: N in bits 23:16, M in 15:8, the post
       divider (2^L) in bits 3:2, from the 14.318 MHz crystal (X.org's
       comp_lmn). MISC clock select 3 picks it. */
    if ((pm->chip >= PM_6422) && ((svga->miscout & 0x0c) == 0x0c)) {
        const uint32_t v = pm_xr32(pm, 0xec);
        const double   n = ((v >> 16) & 0xff) + 1;
        const double   m = ((v >> 8) & 0xff) + 1;
        const double   f = 14318180.0 * n / (m * (double) (1 << ((v >> 2) & 3)));

        if ((v & 0xffff00) && (f > 1000000.0))
            svga->clock = (cpuclock * (double) (1ULL << 32)) / f;
    }

    /* The 6410's clock comes from an external synthesizer: INT 10h AX=3005h
       writes the clock index to XRD8 and relatches MISC (clock select 3).
       The frequencies are those the same BIOS programs into the 6422's PLL
       for each index (its table at 6775h). Through the 20C498's 16-bit
       port, 15/16 and 32 bpp pixels take two clocks each. */
    if ((pm->chip == PM_6410) && ((svga->miscout & 0x0c) == 0x0c)) {
        static const double pm6410_clocks[0x15] = {
            25.057, 28.636, 40.568, 71.591, 50.114, 78.750, 35.795, 44.744,
            128.864, 121.705, 81.136, 31.321, 110.966, 64.432, 75.170, 96.648,
            100.227, 133.636, 147.955, 157.500, 174.682
        };
        const uint8_t idx = pm->xr[0xd8];
        double        f   = (idx < 0x15) ? pm6410_clocks[idx] * 1000000.0 : 25175000.0;

        if (pm->ramdac && (svga->bpp > 8))
            f /= 2.0;
        svga->clock = (cpuclock * (double) (1ULL << 32)) / f;
    }

    if (!bpp)
        return;

    /* Extended modes: start address in dwords (CR1C bits 3:0 are bits
       19:16), offset in 8-byte units (CR1C bits 7:4 are bits 11:8). */
    svga->force_old_addr = 1;
    /* Blanking spans more than the 64 characters a 6-bit end compare can
       hold in the AT3D's 1600x1200 modes; show extended modes with plain
       borders. */
    svga->hoverride      = 1;
    svga->memaddr_latch  = (((pm->cr[0x1c] & 0x0f) << 16) | (svga->crtc[0x0c] << 8) | svga->crtc[0x0d]); /* dwords: the core shifts it */
    svga->rowoffset      = svga->crtc[0x13] | ((pm->cr[0x1c] & 0xf0) << 4);
    svga->lowres         = 0;

    /* An external RAMDAC (the 6410's) assembles wider pixels itself. */
    if (pm->ramdac && (bpp == 8) && (svga->bpp > 8))
        bpp = svga->bpp;

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
            pm_update_xr_io(pm);
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

        case 0x14:
        case 0x15:
            /* BAR 1 (6422): 4 I/O ports, the XR window's address. */
            if (pm->chip >= PM_6422)
                pm->pci_regs[addr] = (addr == 0x14) ? ((val & 0xfc) | 0x01) : val;
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
    if (pm->chip == PM_AT3D) {
        pm->pci_regs[0x02] = 0x3d;
        pm->pci_regs[0x03] = 0x64;
    } else if (pm->chip == PM_6422) {
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
    if (pm->chip >= PM_6422)
        pm->pci_regs[0x14] = 0x01; /* I/O */
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
    if (svga->clock > 0.0) {
        const double f = (cpuclock * (double) (1ULL << 32)) / svga->clock;

        pclog("PM: pixel clock %.3f MHz, %.1f Hz refresh (XRD8 %02X XREC %08X)\n", f / 1000000.0,
              f / ((double) svga->htotal * svga->dots_per_clock * svga->vtotal), pm->xr[0xd8], pm_xr32(pm, 0xec));
    }
    pclog("PM: cursor ena %i x %i y %i xoff %i yoff %i addr %06X latch ena %i y %i draw %p\n",
          svga->hwcursor.ena, svga->hwcursor.x, svga->hwcursor.y, svga->hwcursor.xoff, svga->hwcursor.yoff,
          svga->hwcursor.addr, svga->hwcursor_latch.ena, svga->hwcursor_latch.y, (void *) svga->hwcursor_draw);
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
    pm_update_xr_io(pm);
}

/* The BIOS is a unified ProMotion BIOS (it identifies the chip from its
   registers), but the PCI data structure must match the device for the
   system BIOS to run it: on the 6422, rewrite its device ID and keep the
   32 KB image's checksum. */
static void
pm_patch_bios_id(pm_t *pm)
{
    uint8_t *rom = pm->bios_rom.rom;
    uint16_t pcir;
    int      diff;

    if (!rom || (rom[0] != 0x55) || (rom[1] != 0xaa))
        return;
    pcir = rom[0x18] | (rom[0x19] << 8);
    if ((pcir > 0x7ff0) || memcmp(&rom[pcir], "PCIR", 4))
        return;
    diff = rom[pcir + 6] + rom[pcir + 7];
    rom[pcir + 6] = pm->pci_regs[0x02];
    rom[pcir + 7] = pm->pci_regs[0x03];
    diff -= rom[pcir + 6] + rom[pcir + 7];
    rom[0x7fff] += diff;
}

static void *
pm_init(const device_t *info)
{
    pm_t   *pm   = calloc(1, sizeof(pm_t));
    svga_t *svga = &pm->svga;

    pm->chip      = info->local;
    pm->vram_size = device_get_config_int("memory") << 20;
    pm->vram_mask = pm->vram_size - 1;
    pm->lfb_size  = (pm->chip >= PM_AT3D) ? 0x1000000 : 0x400000;

    if (pm->chip >= PM_AT3D)
        rom_init(&pm->bios_rom, device_get_bios_file(info, device_get_config_bios("bios"), 0),
                 0xc0000, 0x8000, 0x7fff, 0, MEM_MAPPING_EXTERNAL);
    else
        rom_init(&pm->bios_rom, BIOS_PM6410_PCPARTNER, 0xc0000, 0x8000, 0x7fff, 0, MEM_MAPPING_EXTERNAL);
    mem_mapping_disable(&pm->bios_rom.mapping);

    video_inform(VIDEO_FLAG_TYPE_SPECIAL, &timing_pm_pci);

    svga_init(info, svga, pm, pm->vram_size,
              pm_recalctimings,
              pm_in, pm_out,
              NULL,
              NULL);
    svga->decode_mask       = (pm->chip >= PM_AT3D) ? 0x3fffff : (pm->lfb_size - 1);

    /* The 6410 drives an external RAMDAC: an AT&T 20C498, which the BIOS
       knows as its 16-bit DAC (type 81h). */
    if (pm->chip == PM_6410)
        pm->ramdac = device_add(&att498_ramdac_device);
    svga->vram_display_mask = pm->vram_mask;
    svga->translate_address = pm_translate_address;
    svga->hwcursor_draw     = pm_hwcursor_draw;
    svga->overlay_draw      = pm_overlay_draw;

    mem_mapping_add(&pm->lfb_mapping, 0, 0,
                    pm_lfb_readb, pm_lfb_readw, pm_lfb_readl,
                    pm_lfb_writeb, pm_lfb_writew, pm_lfb_writel,
                    NULL, MEM_MAPPING_EXTERNAL, pm);
    mem_mapping_add(&pm->xr_mapping, 0, 0,
                    pm_xr_readb, pm_xr_readw, pm_xr_readl,
                    pm_xr_writeb, pm_xr_writew, pm_xr_writel,
                    NULL, MEM_MAPPING_EXTERNAL, pm);
    mem_mapping_add(&pm->port_mapping, 0, 0,
                    pm_port_readb, pm_port_readw, pm_port_readl,
                    pm_port_writeb, pm_port_writew, pm_port_writel,
                    NULL, MEM_MAPPING_EXTERNAL, pm);
    mem_mapping_disable(&pm->lfb_mapping);
    mem_mapping_disable(&pm->xr_mapping);
    mem_mapping_disable(&pm->port_mapping);

    pm->i2c = i2c_gpio_init("ddc_alliance_promotion");
    pm->ddc = ddc_init(i2c_gpio_get_bus(pm->i2c));

    pm_init_pci_regs(pm);
    if (pm->chip == PM_6422)
        pm_patch_bios_id(pm);
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
    ddc_close(pm->ddc);
    i2c_gpio_close(pm->i2c);
    svga_close(&pm->svga);
    free(pm->blt.rd_buf);
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

/* The 6422 ("GXS"): internal RAMDAC and pixel PLL, the XR I/O window, the
   STB Horizon 64 Video and Alliance's own drivers' chip. The PM6410 BIOS
   drives it too. */
const device_t pm6422_pci_device = {
    .name          = "Alliance ProMotion 6422 PCI",
    .internal_name = "pm6422_pci",
    .flags         = DEVICE_PCI,
    .local         = PM_6422,
    .init          = pm_init,
    .close         = pm_close,
    .reset         = pm_reset,
    .available     = pm6410_available,
    .speed_changed = pm_speed_changed,
    .force_redraw  = pm_force_redraw,
    .config        = pm6410_config
};

static const device_config_t pm_at3d_config[] = {
    {
        .name           = "bios",
        .description    = "Board (BIOS)",
        .type           = CONFIG_BIOS,
        .default_string = "stingray_435",
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .bios           = {
            {
                .name          = "Hercules Stingray 128/3D (AT25, BIOS 4.35.00-B11)",
                .internal_name = "stingray_435",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0,
                .size          = 32768,
                .flags         = 0,
                .files         = { BIOS_AT3D_STINGRAY_435, "" }
            },
            {
                .name          = "Hercules Stingray 128/3D (AT3D, BIOS 2.30.00-B27)",
                .internal_name = "stingray_230",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0,
                .size          = 32768,
                .flags         = 0,
                .files         = { BIOS_AT3D_STINGRAY_230, "" }
            },
            {
                .name          = "Gigabyte GA-RUSH6 (AT25, BIOS 4.350010)",
                .internal_name = "ga_rush6",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0,
                .size          = 32768,
                .flags         = 0,
                .files         = { BIOS_AT3D_GA_RUSH6, "" }
            },
            { .files_no = 0 }
        }
    },
    {
        .name           = "memory",
        .description    = "Memory size",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 4,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "2 MB", .value = 2 },
            { .description = "4 MB", .value = 4 },
            { .description = ""                 }
        },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
};

/* The AT3D/AT25: the ProMotion 2D core of the Voodoo Rush boards (the 3D
   side is not emulated). */
const device_t pm_at3d_pci_device = {
    .name          = "Alliance ProMotion AT3D/AT25 PCI",
    .internal_name = "pm_at3d_pci",
    .flags         = DEVICE_PCI,
    .local         = PM_AT3D,
    .init          = pm_init,
    .close         = pm_close,
    .reset         = pm_reset,
    .available     = NULL,
    .speed_changed = pm_speed_changed,
    .force_redraw  = pm_force_redraw,
    .config        = pm_at3d_config
};
