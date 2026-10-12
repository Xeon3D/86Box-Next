# Handoff: NVIDIA RIVA TNT / TNT2 (NV4/NV5) emulation

Living document: whoever works on the TNT updates it as they go (status, the
log at the bottom). Last updated: 2026-10-12.

## The goal
The owner asked for RIVA TNT/TNT2 emulation done "the way skiretic did the ATI
Rage 128". The owner's plan is in `C:\Users\xeon4\Downloads\RIVA TNT and TNT2
Emulation Plan.md`. Its phases:
1. Shared NV code and the nv4 layout.
2. Boot to DOS/VESA.
3. FIFO and channels.
4. 2D.
5. 3D in the Rage 128 split: setup → span records → shading on threads.
6. TNT2 / M64 / Vanta.
7. Optional GPU lane.

The owner wants the Vulkan compute lane for emulation speed ("shaders
compilation on GPU"). Keep the CPU/GPU record seam (draw states, triangles,
spans) intact.

## Where the work is
- Branch `tnt` in worktree `F:\Claude\86Box-Next\wt-tnt`. It is local only (not
  pushed); ask the owner before pushing.
- Code:
  - `src/video/nv/nv4/` (`nv4_core.c`, `nv4_vga.c`, `subsystems/*`).
  - Header `src/include/86box/nv/vid_nv4.h`.
  - Generated SPIR-V header `src/include/86box/nv/nv4_3d_gpu_spv.h`.
- This is a new, instance-based device (`nv4_t *` passed everywhere), not the
  NV3's global-singleton code. NV3 lives on branch `nv3` (worktree `wt-nv3`,
  `handoffnvidia.md`).
- Boards and ROMs (ELSA, under `roms/video/nvidia/nv4/`, in Xeon3D/roms master
  and every rig):

  | Board | ROM | Chip (PCI ID) |
  |---|---|---|
  | ERAZOR II PCI | E2D20301.PTN | TNT (10DE:0020) |
  | ERAZOR II AGP | E2D20500.ATN | TNT (10DE:0020) |
  | ERAZOR III | E3230500.A2N | TNT2 (0028) |
  | ERAZOR III ULTRA | E3G30500.AUN | TNT2 Ultra (0029) |
  | ERAZOR III LT | E3230500.AMN | TNT2 M64 (002D) |

  TNT2 boards are listed but not yet tested in a guest.

## Rules (from the owner)
- Never open PRs/issues against 86Box/86Box. All PRs target Xeon3D/86Box-Next.
- No host GUI driving or focus stealing. VMs start minimized and are driven
  through the debug command file (`BOX86NEXT_DEBUG_CMD`); see `tools/nv4/`.
- Never touch the owner's rigs' cfg/nvr/images/roms. Test only in `work-tnt`
  (below).
- Code comments must not name or quote NVIDIA's confidential documents or
  source (same rule as NV3, commit f41d4318d). Cite them in commit messages
  only.
- Shut the guest down cleanly: Start > Shut Down, Enter. A hard kill sends
  Win98 to Safe Mode and resets the colour depth.
- Use the recommended answer instead of blocking on questions.

## Building
From Bash with MSYS2 UCRT64 first on PATH:
`export PATH=/c/msys64/ucrt64/bin:$PATH && cmake --build build-static`
(the `build-static` tree in the worktree).

- Python heredocs that write C: a `\n` inside a C string can come out as a
  real newline. Check the result: `grep -n 'pclog(".*$' file`. Writing the
  script to a file first avoids it.
- Debug log: temporarily add `#define ENABLE_NV4_LOG 1` at the top of
  `vid_nv4.h` to get `nv4_log`. Never commit it.
- GPU kernel: edit `subsystems/nv4_pgraph_3d_gpu.comp`, then run
  `sh scripts/regen-nv4-gpu-spv.sh`. It needs glslc: MSYS2
  `mingw-w64-ucrt-x86_64-shaderc`, installed on this machine. Commit the
  regenerated header.

## Testing
- **VM:** `F:\Claude\86Box-Next\work-tnt`, a private copy of a Win98 SE rig.
  - Machine: p6bap, Celeron 233, ELSA ERAZOR II AGP, 512 MB.
  - HDD on IDE 0:0, CD on ATAPI 1:0. The Adaptec was removed at the owner's
    request; the old cfg is in `86box.cfg.bak-scsi`.
  - NV4 ROMs are in `romsx/`, passed with `-R`.
  - Copy the exe there after each build:
    `cp build-static/src/86Box-Next.exe ../work-tnt/`.
- **Guest software:**
  - NVIDIA driver 4.11.01.0115 (from `nv4drv.iso`).
  - 3DMark 99 Max, installed from `nv4test.iso`, the CD image now in the cfg.
    The owner accepted the EULA; no serial.
  - Windows logs in by itself; ~50 s to the desktop.
- **`tools/nv4/tnt.sh`:** `start`, `cmd "<debug lines>"`, `shot NAME` (PNG in
  `work-tnt/shots/`), `stop` (last resort). Useful debug lines:
  - `key 1d,01,81,9d` (Ctrl+Esc, Start menu)
  - `type p`
  - `key 1c,9c` (Enter)
  - `key 01,81` (Esc)
  - `dev` (state dump)
  - `dev trace N` (log the next N methods)
  - `dev 3d` (3D flush count, spans and host shading time; then resets them)
- **`tools/nv4/bench.sh THREADS GPU`:** boots, starts 3DMark's benchmark, times
  40 s of 3D shading, shuts down.
- **3DMark's score** (2620 at 800x600x16) is in guest time, so it does not move
  with host speed. Compare µs per span from `dev 3d` instead.
- **Environment switches:**
  - `NV4_GPU=0/1` overrides the "Draw 3D on the host GPU" option.
  - `NV4_3D_VERIFY=1` shades every parallel batch twice and logs any byte that
    differs ("nv4 3D verify: ..."). It is slow, since it copies the whole VRAM
    3 times per flush.
- **Screenshot seams** are the shot catching a half-drawn frame, not tearing.

## Status (what works)
- **Boot/VGA/VESA/display:** modes up to 1600x1200 at 16/32 bpp. Lines past
  1024 fixed (no LCOMP bit 10). PRAMIN = VRAM ^ (size-64).
- **PFIFO:** PIO, DMA pusher, puller, RAMHT/RAMFC/RAMRO, context switches.
- **PGRAPH:**
  - The register file is the engine state; context switch via the
    CONTEXT_SWITCH trap.
  - NV4 traps set-context and SetOperation methods; NV5 runs them in hardware
    per DEBUG_3.
  - Notifiers.
  - All the 2D classes Win98's driver uses.
  - The driver gives an accelerated desktop up to 1600x1200x32. Full-screen
    DOS box and back works.
- **3D** (`nv4_pgraph_3d.c`, DX5 class 0x54, DX6 class 0x55):
  - The pipeline:
    - Immutable draw states.
    - Setup to 12.4 fixed point, int64 edges, top-left rule.
    - Per-row spans with plane equations.
    - Span shader: swizzled textures with all filters and wraps, DX5 texture
      blend / DX6 combiners, specular, fog, alpha test, stencil, Z/W, blend,
      dither, write masks.
  - A batch is flushed before anything that must see its pixels: a non-3D
    method, a notify, LFB access, the FIFO going idle, a full batch.
  - Both of these are hardware methods on the TNT (trapping them loops the
    resman):
    - SetContextSurface (0x18c) of the D3D classes.
    - Writes through a swizzled-surface object (CTX_SWITCH1 bit 14) into
      surface 5.
  - DxDiag's D3D test passes. 3DMark 99 Max runs fully and correctly.
- **Render threads** (option 1-8, default 2):
  - Each worker shades rows y mod n, in submission order, so output is
    bit-exact with one thread.
  - The batch is flushed when the render-target layout changes or a texture
    would read bytes the batch wrote.
  - Aliasing rows (zero pitch, Z rows narrower than colour rows) are shaded
    serially.
- **GPU lane** (`nv4_pgraph_3d_gpu.c` + `.comp`, Vulkan loaded at run time):
  - One workgroup per row, the CPU span shader ported operation for operation.
  - Bit-exact rules, on both sides (don't break them):
    - `precise` floats in the kernel; `-ffp-contract=off` for
      `nv4_pgraph_3d.c`.
    - Every divide goes through `fdiv_cr` (from the Rage 128 kernel).
    - log2 is computed from the float's bits (`nv4_log2_bits`).
    - Float-to-int conversions go through `nv4_f2i` / `f2i` (INT_MIN when out
      of range).
  - Each flush is synchronous:
    1. Copy the touched colour/Z rows into a device-local VRAM mirror.
    2. Gather the batch's textures through their DMA objects into a 32 MB
       arena.
    3. Dispatch the kernel.
    4. Copy the rows back.
  - Verify result: 2621 GPU batches and 17000 threaded batches identical to the
    serial path.
- **Speed** (40 s of 3DMark Game 1, i7-12700K + RTX 2080 SUPER):

  | Path | µs/span |
  |---|---|
  | 1 thread | 4.9 |
  | 4 threads | 0.9 |
  | GPU | 0.57 |

  With 1 thread, the 3D took 34 s of the 40 s.

## Next (in rough order)
1. **GPU lane speed.**
   - Cache gathered textures across flushes. Invalidate on LFB writes, M2MF
     and 2D writes into their VRAM ranges; textures in system/AGP memory need
     a checksum or an always-regather policy.
   - Submit asynchronously and wait only when something reads VRAM (the Rage
     128 does this with fences and range tracking).
   - Avoid re-uploading unchanged rows.
2. **CPU span shader speed:**
   - Fixed-point attribute stepping instead of 14 float plane evaluations per
     pixel.
   - A per-span texture setup.
   - Must stay bit-exact with the GPU kernel; change both together.
3. **Rendering into a swizzled 3D colour surface** (SURFACE type 2): not
   handled.
4. **TNT2 boards:** test the ERAZOR III / ULTRA / LT in a guest with a TNT2
   driver.
5. **The rest of the plan:**
   - PVIDEO overlay (stub register file now).
   - InstallShield's register-page banner glitch (some 2D stretch path).
   - Docs and tests.
6. **DAC checksum:** the NV3 has PRAMDAC TEST_CONTROL/CHECKSUM (fed56d813,
   NVIDIA's mfgtest). The TNT has the same registers, but its LFSR is
   unverified; only worth it if a TNT test program needs it.

## Sources
- Owner's NVIDIA archive `98dec\Alpha.87`: `nv4_ref.h`, the resman (RM), BIOS
  and the D3D driver. Confidential: cite only in commit messages.
- TNT datasheet 1.0, NV5 datasheet 0.24.
- envytools (MIT):
  - rnndb.
  - `hwtest/pgraph_state.cc` (register list and masks).
  - `pgraph_mthd.cc`.
  - `hwtest/pgraph.cc` (class method lists).
  - `nvhw/pgraph_xy4.c`.
  - `hwtest pgraph_class_d3d56`.
- Vort's Bochs `geforce.cc` (NV2x, but useful for the NV4 lineage).
- Rage 128 GPU lane for patterns: `src/video/vid_ati_rage128_gpu.c`, `_seg.comp`,
  `_fdiv.glsl`.

## Log
- 2026-10-11: phase 1 (b974d0ea7), PFIFO (1370fe587).
- 2026-10-12:
  - PGRAPH + 2D (3b784aa2f).
  - >1024 lines (946d6c441).
  - D3D classes (3d2d9c6ba).
  - Swizzled destination (54e852148).
  - Threads + GPU lane + verify (52d3e1441).
- 2026-10-12:
  - Ported NV3's "memory decode off unmaps the VGA window" (5ee69da2e). The
    other post-branch NV3 fixes were checked: already covered or NV3-specific
    (see that commit message).
  - `tools/nv4/` scripts and this document added.
