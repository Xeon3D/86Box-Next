/* 86Box-Next: the old recompiler's exec386_dynarec_dyn(), included by
   386_dynarec.c when NEW_DYNAREC is off.  Upstream removed the old
   recompiler (86Box/86Box f50f8cdf7); this is its last upstream version
   with the fork's changes, kept in its own file so upstream merges into
   386_dynarec.c don't touch it. */

static __inline void
exec386_dynarec_dyn(void)
{
    uint32_t start_pc  = 0;
    uint32_t phys_addr = get_phys(cs + cpu_state.pc);
    int      hash      = HASH(phys_addr);
    codeblock_t *block = codeblock_hash[hash];
    int valid_block = 0;

    /* Code in device memory (a BIOS copying a routine into video memory) is
       fetched through the device's read handlers, and nothing tracks writes
       there, so it is interpreted every time rather than kept in a block. */
    if (!cpu_state.abrt && (_mem_exec[phys_addr >> MEM_GRANULARITY_BITS] == NULL)) {
        exec386_dynarec_int();
        return;
    }
    cpu_fetch_device = 0;

    /* Refresh before the lookup AND before a fresh compile: the old
       dynarec skips the lookup on an empty hash slot, and the new block
       still takes its key from cpu_cur_status. */
    if (cpu_state.npxc & 0x300)
        cpu_cur_status &= ~CPU_STATUS_FPU_PC24;
    else
        cpu_cur_status |= CPU_STATUS_FPU_PC24;
    if ((cpu_state.npxc >> 10) & 3)
        cpu_cur_status |= CPU_STATUS_FPU_RC_NZ;
    else
        cpu_cur_status &= ~CPU_STATUS_FPU_RC_NZ;
    /* ES flat: its limit checks are left out (CHECK_SEG_LIMITS). It is
       loaded by far more paths than DS and SS, which keep their bits as
       they load; this is the one place it is read. */
    if ((cpu_state.seg_es.base == 0) && (cpu_state.seg_es.limit_low == 0) && (cpu_state.seg_es.limit_high == 0xffffffff))
        cpu_cur_status &= ~CPU_STATUS_NOTFLATES;
    else
        cpu_cur_status |= CPU_STATUS_NOTFLATES;

    if (block && !cpu_state.abrt)
    {
        page_t *page = &pages[phys_addr >> 12];

        /* Block must match current CS, PC, code segment size,
           and physical address. The physical address check will
           also catch any page faults at this stage */
        valid_block = (block->pc == cs + cpu_state.pc) && (block->_cs == cs) && (block->phys == phys_addr) && !((block->status ^ cpu_cur_status) & CPU_STATUS_FLAGS) && ((block->status & cpu_cur_status & CPU_STATUS_MASK) == (cpu_cur_status & CPU_STATUS_MASK));
        if (!valid_block) {
            uint64_t mask = (uint64_t) 1 << ((phys_addr >> PAGE_MASK_SHIFT) & PAGE_MASK_MASK);
            if (page->code_present_mask[(phys_addr >> PAGE_MASK_INDEX_SHIFT) & PAGE_MASK_INDEX_MASK] & mask)
            {
                /* Walk page tree to see if we find the correct block */
                codeblock_t *new_block = codeblock_tree_find(phys_addr, cs);
                if (new_block) {
                    valid_block = (new_block->pc == cs + cpu_state.pc) && (new_block->_cs == cs) && (new_block->phys == phys_addr) && !((new_block->status ^ cpu_cur_status) & CPU_STATUS_FLAGS) && ((new_block->status & cpu_cur_status & CPU_STATUS_MASK) == (cpu_cur_status & CPU_STATUS_MASK));
                    if (valid_block) {
                        block = new_block;
                    }
                }
            }
        }

        if (valid_block && (block->page_mask & *block->dirty_mask)) {
            codegen_check_flush(page, page->dirty_mask[(phys_addr >> 10) & 3], phys_addr);
            page->dirty_mask[(phys_addr >> 10) & 3] = 0;
            if (!block->valid)
                valid_block = 0;
        }
        if (valid_block && block->page_mask2) {
            /* We don't want the second page to cause a page
               fault at this stage - that would break any
               code crossing a page boundary where the first
               page is present but the second isn't. Instead
               allow the first page to be interpreted and for
               the page fault to occur when the page boundary
               is actually crossed.*/
            uint32_t phys_addr_2 = get_phys_noabrt(block->endpc);
            page_t *page_2 = &pages[phys_addr_2 >> 12];

            if ((block->phys_2 ^ phys_addr_2) & ~0xfff)
                valid_block = 0;
            else if (block->page_mask2 & *block->dirty_mask2) {
                codegen_check_flush(page_2, page_2->dirty_mask[(phys_addr_2 >> 10) & 3], phys_addr_2);
                page_2->dirty_mask[(phys_addr_2 >> 10) & 3] = 0;
                if (!block->valid)
                    valid_block = 0;
            }
        }
        if (valid_block && block->was_recompiled && (block->flags & CODEBLOCK_STATIC_TOP) && block->TOP != cpu_state.TOP)
        {
            /* FPU top-of-stack does not match the value this block was compiled
               with, re-compile using dynamic top-of-stack*/
            block->flags &= ~CODEBLOCK_STATIC_TOP;
            block->was_recompiled = 0;
        }
    }

    if (valid_block && block->was_recompiled)
    {
        void (*code)(void) = (void *) &block->data[BLOCK_START];

        codeblock_hash[hash] = block;
        inrecomp = 1;
        code();
#    ifdef USE_ACYCS
        acycs = 0;
#    endif
        inrecomp = 0;

        if (x86_was_reset)
            codegen_reset();
    } else if (valid_block && !cpu_state.abrt) {
        start_pc = cpu_state.pc;

        cpu_block_end = 0;
        x86_was_reset = 0;
        int record_discarded = 0;

#    if defined(__APPLE__) && defined(__aarch64__)
        if (__builtin_available(macOS 11.0, *)) {
            pthread_jit_write_protect_np(0);
        }
#    endif
        codegen_record_faulted = 0;
        codegen_block_start_recompile(block);
        codegen_in_recompile = 1;

        while (!cpu_block_end) {
            oldcs  = CS;
            oldcpl = CPL;
            cpu_state.oldpc = cpu_state.pc;
            cpu_state.op32  = use32;

            cpu_state.ea_seg = &cpu_state.seg_ds;
            cpu_state.ssegs  = 0;

            fetchdat = fastreadl_fetch(cs + cpu_state.pc);
#    ifdef ENABLE_386_DYNAREC_LOG
            if (in_smm)
                x386_dynarec_log("[%04X:%08X] fetchdat = %08X\n", CS, cpu_state.pc, fetchdat);
#    endif

            if (!cpu_state.abrt) {
                /* Temp variables for FPU exception reporting. */
                cpu_state.temp_CS = CS;
                cpu_state.temp_cs = cs;
                cpu_state.temp_pc = cpu_state.pc;

                opcode = fetchdat & 0xFF;
                fetchdat >>= 8;

                cpu_state.pc++;

                codegen_generate_call(opcode, x86_opcodes[(opcode | cpu_state.op32) & 0x3ff], fetchdat, cpu_state.pc, cpu_state.pc - 1);

                x86_opcodes[(opcode | cpu_state.op32) & 0x3ff](fetchdat);

                if (x86_was_reset)
                    break;
            }

                /* Cap source code at 4000 bytes per block; this
                   will prevent any block from spanning more than
                   2 pages. In practice this limit will never be
                   hit, as host block size is only 2kB*/
            if ((cpu_state.pc - start_pc) > 1000)
                CPU_BLOCK_END();

            if (cpu_init)
                CPU_BLOCK_END();

            if (new_ne)
                CPU_BLOCK_END();
            if ((cpu_state.flags & T_FLAG) || (trap == 2))
                CPU_BLOCK_END();
            if (smi_line)
                CPU_BLOCK_END();
            if (nmi && nmi_enable && nmi_mask)
                CPU_BLOCK_END();
            if ((cpu_state.flags & I_FLAG) && pic.int_pending && !cpu_end_block_after_ins)
                CPU_BLOCK_END();

            if (cpu_end_block_after_ins) {
                cpu_end_block_after_ins--;
                if (!cpu_end_block_after_ins)
                    CPU_BLOCK_END();
            }

            /* Cut short by an exception delivered as it was recorded: the
               block would keep only the instructions before it for good.
               Not kept; it is recorded afresh next time. */
            if (codegen_record_faulted) {
                codegen_record_faulted = 0;
                codegen_block_remove();
                record_discarded = 1;
                CPU_BLOCK_END();
            }

            /* An instruction ran on into device memory: interpreted, never
               kept. A block already removed above is not removed twice: that
               would put it on the free list twice. */
            if (cpu_fetch_device) {
                if (!record_discarded && (!cpu_state.abrt || (cpu_state.abrt & ABRT_EXPECTED)))
                    codegen_block_remove();
                CPU_BLOCK_END();
            }

            if (cpu_state.abrt) {
                if (!record_discarded && !(cpu_state.abrt & ABRT_EXPECTED))
                    codegen_block_remove();
                CPU_BLOCK_END();
            }
        }

        cpu_end_block_after_ins = 0;

        /* #MF can interrupt the interpret-while-compiling pass. The block
           was marked recompiled at entry, but its host code was never
           finalized. Never publish that unfinished allocation. */
        if (new_ne && (!cpu_state.abrt || (cpu_state.abrt & ABRT_EXPECTED)) && !x86_was_reset && !cpu_fetch_device && !record_discarded)
            codegen_block_remove();
        else if ((!cpu_state.abrt || (cpu_state.abrt & ABRT_EXPECTED)) && !new_ne && !x86_was_reset && !record_discarded &&
                 !cpu_fetch_device)
            codegen_block_end_recompile(block);

        if (x86_was_reset)
            codegen_reset();

        codegen_in_recompile = 0;
#    if defined(__APPLE__) && defined(__aarch64__)
        if (__builtin_available(macOS 11.0, *)) {
            pthread_jit_write_protect_np(1);
        }
#    endif
    } else if (!cpu_state.abrt) {
        /* Mark block but do not recompile */
        start_pc = cpu_state.pc;

        cpu_block_end = 0;
        x86_was_reset = 0;
        int record_discarded = 0;

        codegen_record_faulted = 0;
        codegen_block_init(phys_addr);

        while (!cpu_block_end) {
            oldcs  = CS;
            oldcpl = CPL;
            cpu_state.oldpc = cpu_state.pc;
            cpu_state.op32  = use32;

            cpu_state.ea_seg = &cpu_state.seg_ds;
            cpu_state.ssegs  = 0;

            /* The furthest byte this instruction can take: 15 bytes long at
               most (Intel SDM vol. 2, 2.3.11). The mark covers through it, so
               a long last instruction's tail is marked, on whichever page. */
            codegen_endpc = (cs + cpu_state.pc) + 14;

            fetchdat      = fastreadl_fetch(cs + cpu_state.pc);

#    ifdef ENABLE_386_DYNAREC_LOG
            if (in_smm)
                x386_dynarec_log("[%04X:%08X] fetchdat = %08X\n", CS, cpu_state.pc, fetchdat);
#    endif

            if (!cpu_state.abrt) {
                /* Temp variables for FPU exception reporting. */
                cpu_state.temp_CS = CS;
                cpu_state.temp_cs = cs;
                cpu_state.temp_pc = cpu_state.pc;

                opcode = fetchdat & 0xFF;
                fetchdat >>= 8;

                cpu_state.pc++;

                x86_opcodes[(opcode | cpu_state.op32) & 0x3ff](fetchdat);

                if (x86_was_reset)
                    break;
            }

                /* Cap source code at 4000 bytes per block; this
                   will prevent any block from spanning more than
                   2 pages. In practice this limit will never be
                   hit, as host block size is only 2kB */
            if ((cpu_state.pc - start_pc) > 1000)
                CPU_BLOCK_END();

            if (cpu_init)
                CPU_BLOCK_END();

            if (new_ne)
                CPU_BLOCK_END();
            if (cpu_state.flags & T_FLAG)
                CPU_BLOCK_END();
            if (smi_line)
                CPU_BLOCK_END();
            if (nmi && nmi_enable && nmi_mask)
                CPU_BLOCK_END();
            if ((cpu_state.flags & I_FLAG) && pic.int_pending && !cpu_end_block_after_ins)
                CPU_BLOCK_END();

            if (cpu_end_block_after_ins) {
                cpu_end_block_after_ins--;
                if (!cpu_end_block_after_ins)
                    CPU_BLOCK_END();
            }

            /* Cut short by an exception delivered as it was recorded: the
               block would keep only the instructions before it for good.
               Not kept; it is recorded afresh next time. */
            if (codegen_record_faulted) {
                codegen_record_faulted = 0;
                codegen_block_remove();
                record_discarded = 1;
                CPU_BLOCK_END();
            }

            /* An instruction ran on into device memory: interpreted, never
               kept. A block already removed above is not removed twice: that
               would put it on the free list twice. */
            if (cpu_fetch_device) {
                if (!record_discarded && (!cpu_state.abrt || (cpu_state.abrt & ABRT_EXPECTED)))
                    codegen_block_remove();
                CPU_BLOCK_END();
            }

            if (cpu_state.abrt) {
                if (!record_discarded && !(cpu_state.abrt & ABRT_EXPECTED))
                    codegen_block_remove();
                CPU_BLOCK_END();
            }
        }

        cpu_end_block_after_ins = 0;

        if (new_ne && (!cpu_state.abrt || (cpu_state.abrt & ABRT_EXPECTED)) && !x86_was_reset && !cpu_fetch_device && !record_discarded)
            codegen_block_remove();
        else if ((!cpu_state.abrt || (cpu_state.abrt & ABRT_EXPECTED)) && !new_ne && !x86_was_reset && !record_discarded &&
                 !cpu_fetch_device)
            codegen_block_end();

        if (x86_was_reset)
            codegen_reset();
    }

}
