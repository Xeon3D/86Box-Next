/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          NV3 PBUS DMA: DMA & Notifier Engine
 *
 *
 *
 * Authors: Connor Hyde, <mario64crashed@gmail.com> I need a better email address ;^)
 *
 *          Copyright 2024-2025 starfrost
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/dma.h>
#include <86box/mem.h>
#include <86box/pci.h>
#include <86box/rom.h> // DEPENDENT!!!
#include <86box/video.h>
#include <86box/nv/vid_nv.h>
#include <86box/nv/vid_nv3.h>

/* Nvidia DMA Engine

   A DMA object is 16+ bytes of RAMIN: word 0 holds the byte adjust (11:0),
   whether a page table follows (16) and the target (25:24: VRAM, cartridge,
   PCI, AGP); word 1 the limit; words 2 on the page table entries (frame
   address in 31:12, bit 0 present, bit 1 writable). An object's grobj word 1
   holds its DMA instance (15:0) and notifier instance (31:16); M2MF's output
   object is in word 2 (15:0). */

/* A byte offset within a DMA object to an address in its target */
static uint32_t
nv3_dma_translate(uint32_t inst, uint32_t offset, uint32_t *target)
{
    uint32_t base   = inst << 4;
    uint32_t info   = nv3_ramin_read32(base, nv3);
    uint32_t linear = (info & 0xFFF) + offset;
    uint32_t pte    = nv3_ramin_read32(base + 8 + ((linear >> 12) << 2), nv3);

    *target = (info >> NV3_NOTIFICATION_TARGET) & 0x03;

    nv_log_verbose_only("DMA inst %04x offset %x: info %08x pte %08x -> %08x target %d\n", inst, offset, info, pte,
                        (pte & 0xFFFFF000) | (linear & 0xFFF), *target);
    return (pte & 0xFFFFF000) | (linear & 0xFFF);
}

uint32_t
nv3_dma_read8(uint32_t inst, uint32_t offset)
{
    uint32_t target;
    uint32_t addr = nv3_dma_translate(inst, offset, &target);
    uint8_t  val  = 0;

    if (target == NV3_DMA_TARGET_NODE_VRAM)
        return nv3->nvbase.svga.vram[addr & nv3->nvbase.svga.vram_mask];
    dma_bm_read(addr, &val, 1, 1);
    return val;
}

void
nv3_dma_write8(uint32_t inst, uint32_t offset, uint8_t val)
{
    uint32_t target;
    uint32_t addr = nv3_dma_translate(inst, offset, &target);

    if (target == NV3_DMA_TARGET_NODE_VRAM) {
        addr &= nv3->nvbase.svga.vram_mask;
        nv3->nvbase.svga.vram[addr]                  = val;
        nv3->nvbase.svga.changedvram[addr >> 12]     = changeframecount;
    } else
        dma_bm_write(addr, &val, 1, 1);
}

static void
nv3_dma_write32(uint32_t inst, uint32_t offset, uint32_t val)
{
    uint32_t target;
    uint32_t addr = nv3_dma_translate(inst, offset, &target);

    if (target == NV3_DMA_TARGET_NODE_VRAM) {
        addr &= nv3->nvbase.svga.vram_mask & ~3;
        *(uint32_t *) &nv3->nvbase.svga.vram[addr] = val;
        nv3->nvbase.svga.changedvram[addr >> 12]    = changeframecount;
    } else
        dma_bm_write(addr, (uint8_t *) &val, 4, 4);
}

/* For repeated reads (textures): the addresses of `pages` 4K pages of a DMA
   object starting with the one holding `offset`. *lin gets offset's linear
   address within the object (page i holds linear (*lin & ~0xFFF) + i * 4K);
   returns the target. */
uint32_t nv3_dma_map_pages(uint32_t inst, uint32_t offset, uint32_t *addr, uint32_t pages, uint32_t *lin)
{
    uint32_t base = inst << 4;
    uint32_t info = nv3_ramin_read32(base, nv3);

    *lin = (info & 0xFFF) + offset;
    for (uint32_t i = 0; i < pages; i++)
        addr[i] = nv3_ramin_read32(base + 8 + (((*lin >> 12) + i) << 2), nv3) & 0xFFFFF000;
    return (info >> NV3_NOTIFICATION_TARGET) & 0x03;
}

/* Memory to memory format (class 0x0D): NUM_SCANLINES lines of
   SCANLINE_LENGTH bytes, input bytes taken every 1/2/4 bytes and written
   every 1/2/4 bytes, from the DMA_IN object to the DMA_OUT object */
void nv3_perform_dma_m2mf(nv3_grobj_t grobj)
{
    uint32_t in_inst  = grobj.grobj_1 & 0xFFFF;
    uint32_t out_inst = grobj.grobj_2 & 0xFFFF;
    uint32_t offset_in  = nv3->pgraph.m2mf.offset_in;
    uint32_t offset_out = nv3->pgraph.m2mf.offset_out;
    uint32_t inc_in  = (nv3->pgraph.m2mf.format >> NV3_M2MF_FORMAT_INPUT) & 0x07;
    uint32_t inc_out = (nv3->pgraph.m2mf.format >> NV3_M2MF_FORMAT_OUTPUT) & 0x07;

    if (!inc_in)
        inc_in = 1;
    if (!inc_out)
        inc_out = 1;

    for (uint32_t line = 0; line < nv3->pgraph.m2mf.num_scanlines; line++) {
        uint32_t in  = offset_in;
        uint32_t out = offset_out;

        for (uint32_t i = 0; i < nv3->pgraph.m2mf.scanline_length; i += inc_in) {
            nv3_dma_write8(out_inst, out, nv3_dma_read8(in_inst, in + i));
            out += inc_out;
        }
        offset_in += nv3->pgraph.m2mf.pitch_in;
        offset_out += nv3->pgraph.m2mf.pitch_out;
    }
}

/* Write an NvNotification (16 bytes: the time in ns, info32, info16 and the
   status) at notifier index `index` of the object's notifier DMA object */
void nv3_write_notifier(nv3_grobj_t grobj, uint32_t index, uint16_t status, uint32_t info32, uint16_t info16)
{
    uint32_t inst = grobj.grobj_1 >> 16;
    uint32_t off  = index << 4;
    uint64_t time = nv3->ptimer.time;

    if (!inst)
        return;
    nv3_dma_write32(inst, off + 0, (uint32_t) time);
    nv3_dma_write32(inst, off + 4, (uint32_t) (time >> 32));
    nv3_dma_write32(inst, off + 8, info32);
    nv3_dma_write32(inst, off + 12, info16 | ((uint32_t) status << 16));
}

/* After a method: if a notify was armed (NOTIFY method), write the
   notifier and disarm it (NOTIFY register bit 16). */
void nv3_notify_if_needed(uint32_t name, uint32_t method_id, nv3_ramin_context_t context, nv3_grobj_t grobj)
{
    if (!nv3->pgraph.notify_pending)
        return;
    /* the NOTIFY method itself only arms it */
    if (method_id == NV3_SET_NOTIFY)
        return;

    nv3->pgraph.notify_pending = false;
    nv3->pgraph.notifier &= ~(1 << NV3_PGRAPH_NOTIFY_REQUEST_PENDING);
    nv3_write_notifier(grobj, (nv3->pgraph.notify_index), NV3_NOTIFICATION_STATUS_DONE_OK, 0, 0);
}
