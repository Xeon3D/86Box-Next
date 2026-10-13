# Handoff: NVIDIA RIVA TNT / TNT2 (NV4/NV5) emulation

Living document: whoever works on the TNT updates it as they go (status, the
log at the bottom). Last updated: 2026-10-13.

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
  - The VM is muted (`sound_muted = 1` in its `[General]`); keep it that way,
    the owner hears it otherwise.
  - Edit `86box.cfg` only while the VM is not running.
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
  40 s of 3D shading, shuts down. If the owner is using the guest at the same
  time, the keystrokes go astray: check with `shot`.
- **Shutting down mid-benchmark:** Esc aborts, Enter closes the "aborted" box.
  A stray Enter can open the Result Browser, whose "save?" prompt then blocks
  Windows' shutdown ("not responding" box): `key 12,92` (End Task).
- **3DMark's score** (2620 at 800x600x16) is in guest time, so it does not move
  with host speed. Compare µs per span from `dev 3d` instead.
- **Environment switches:**
  - `NV4_GPU=0/1` overrides the "Draw 3D on the host GPU" option.
  - `NV4_3D_VERIFY=1` shades every parallel batch twice and logs any byte that
    differs ("nv4 3D verify: ..."). It is slow, since it copies the whole VRAM
    3 times per flush.
  - `NV4_GPU_PROFILE=N` logs the next N GPU batches one by one: spans,
    pixels, and GPU time for uploads, kernel and download.
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
    method, a notify, LFB or VGA-window access, the start of vertical blank,
    a full batch (16K triangles / 256K spans). Not when the FIFO runs dry:
    the driver moves PUT every few triangles.
  - `nv4_3d_flush` draws (with the GPU: submits); `nv4_3d_sync` also waits
    for the GPU's pixels and is what LFB, VGA window and 2D methods call.
    Vertical blank waits only when the batch in flight draws into the
    displayed bytes. Anything new that reads or writes VRAM from the CPU
    must call `nv4_3d_sync` first.
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
  - The CPU span shader ported operation for operation. One invocation per
    pixel walks its row's spans in submission order (no barriers);
    workgroups are rows x 64 pixels.
  - Bit-exact rules, on both sides (don't break them):
    - `precise` floats in the kernel; `-ffp-contract=off` for
      `nv4_pgraph_3d.c`.
    - Every divide goes through `fdiv_cr` (from the Rage 128 kernel).
    - log2 is computed from the float's bits (`nv4_log2_bits`).
    - Float-to-int conversions go through `nv4_f2i` / `f2i` (INT_MIN when out
      of range).
  - A flush submits and returns:
    1. Wait for the batch still in flight, if any, and copy its rows back.
    2. Gather the batch's textures through their DMA objects into a 32 MB
       arena; copy the touched colour/Z rows and the records to the GPU.
    3. Dispatch; copy the rows to a staging buffer.
    The rows reach VRAM at the next flush or sync point.
  - The GPU stays in P5/P8 (~700 MHz) under this bursty load (nvidia-smi), so
    each dispatch has ~0.5 ms of latency whatever its size. Fewer, larger,
    overlapped batches are what helps.
  - Verify result: 2621 synchronous and 2000 asynchronous GPU batches, and
    17000 threaded batches, identical to the serial path.
- **Speed:** host time on the emulation thread per span, over 40 s of
  3DMark's first tests (i7-12700K + RTX 2080 SUPER, `tools/nv4/bench.sh`):

  | Path | µs/span |
  |---|---|
  | 1 thread | 4.9 |
  | 4 threads | 0.85 |
  | 8 threads | 0.54 |
  | GPU (async) | 0.23 |

  The GPU figure is submit time plus waits. Before this work, 1 thread spent
  34 s of the 40 s on 3D.
- **GPU waits:** `dev 3d` lists them by cause. State-only classes (surfaces,
  ROP, beta, chroma, pattern, clip) neither flush nor wait
  (`nv4_pgraph_state_only`). What remains is the 2D rectangle clear (class
  0x5e) at the start of each frame, ~0.8 s per 40 s.

## Next (in rough order)
1. **GPU lane speed.**
   - The remaining waits are the 2D rectangle clear (class 0x5e). A
     rectangle fill on the GPU (or a wait only when the rectangle overlaps
     the batch's rows) would let the batch stay in flight.
   - The submit side (~2.4 s per 40 s) is gather + row copies + grouping
     spans by row; it could move to a worker thread.
   - Cache gathered textures across flushes (~40 MB/s of re-gathering now).
     Invalidate on LFB writes, M2MF and 2D writes into their VRAM ranges;
     textures in system/AGP memory need a checksum or always-regather.
   - Avoid re-uploading unchanged rows.
   - Kernel: `combine()` indexes `src[]` dynamically (local memory); the
     whole kernel is one big function. Splitting variants by state
     (specialization constants, as the Rage 128 does) would help.
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
- 2026-10-13:
  - GPU lane profiled and made asynchronous; FIFO-idle flush dropped,
    batches 4x larger (fddb3b009).
  - GPU waits by cause; state-only classes skip the sync; vblank range fixed
    (a27854013).
  - Full 3DMark 99 run on the async GPU lane: every test renders, 2616
    3DMarks (2620 on the CPU path).
  - Mishap: bench.sh started a VM while the previous one hung at shutdown;
    two instances shared the disk for ~8 min. Win98 came back clean. tnt.sh
    now refuses a second instance.
