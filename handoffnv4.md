# Handoff: NVIDIA RIVA TNT / TNT2 (NV4/NV5) emulation

Living document: whoever works on the TNT updates it as they go (status, the
log at the bottom). Last updated: 2026-10-14.

**In progress (2026-10-14):**
- B5 done: 3DMark 2000 1.1 full default run on the TNT (work-tnt,
  1024x768x16, Celeron 233): every test renders, **1050 3DMarks**; the only
  unmodeled thing hit is DVD subpicture 0x38's context methods. A3 (3D into
  a swizzled surface) is NOT needed by it. UT demo 348 (D3D) installed in
  work-tnt and played by the owner: "ran fine" (slow on 2 CPU threads; turn
  on gpu_raster for it).
- **B4 OpenGL / Quake III: works** (5b120b5de). VM
  `F:\Claude\86Box-Next\work-q3` (TNT2 ERAZOR III, p6bap, Celeron 466,
  4 threads + gpu_raster, DX7 + Quake III 1.11; the owner gave the CD key
  in chat 2026-10-14 and it was typed into Q3 as scancodes -- never write
  it into a file). Menus and demo001 (q3dm1, timedemo) render correctly;
  29% emulation speed on the CPU 3D path. The bug: 2D drawing into a
  linear surface was clipped to pitch / bpp; the ICD writes a progress
  word with a 1x1 rect (0x5e) at x = 8 of an 8-pixel Y32 surface (VRAM
  0x12c000, pitch 32) after each texture upload through its staging ring
  and spins (padding the pushbuffer with 17-word NOP runs) until it reads
  it back -- the "all-zero pushbuffer" was that NOP padding, not lost
  writes. Next for B4: a timedemo figure with the GPU lane, GLQuake /
  Quake II, the TNT (ERAZOR II) with the same ICD.
- **POST slowdown (owner report, confirmed 2026-10-14):** on work-q3 the
  speed drops to 0-3% for ~15 s while the Award logo screen prints (the
  memory count): the guest is in the ELSA BIOS's planar-mode character
  output (c000:3a86 loop, INT 10h wrapper c000:1890 that unlocks/relocks
  CR1F/CR2C per call). Not the GPU lane (same with NV4_GPU=0). A host
  sample (scratchpad sampler.py: suspends threads, symbolizes RIP with
  nm) shows 76% of the emulation thread in codegen_block_init: the
  dynarec keeps recompiling. The owner sees no slowdown on another board
  at 266 MHz. Suspects: the VIA Apollo Pro 133A's shadow RAM state for
  C0000-C9FFF during POST (the ELSA BIOS is 40 KB and writes into its own
  segment: c000:101e/1022 via DS = C000 -- self-modifying code when the
  shadow is writable). Next: `cpu` (now with SS/CR0) and `map c1000` /
  `map c3000` during the slow phase; compare an Intel 440BX board with the
  same card and CPU.
- Debug aids (4c499c8e7): `speedlog 1|0` (speed + CS:EIP each second),
  `dev trace N` counts every line (it ran unbounded on a NOP-only channel
  and wrote a 9 GB log), user-area writes show the guest address.
- **Desktop shortcut arrows** (owner report) were broken on work-tnt and
  work-q3; tools/nv4/guest/icons.exe (DrawIconEx + image lists with
  overlays at every depth) draws them correctly and identically to GDI's
  own engine on the current build, so it is most likely a stale
  C:\WINDOWS\ShellIconCache built before c59c71997 (4-bit index order).
  To do: delete it (restart in MS-DOS mode, `attrib -h -s` + `del
  c:\windows\shelli~1`) and check the desktop again.
- **POST memory count "corruption"** (owner report): the same with a plain
  VGA card (work-q3 booted once with gfxcard = vga, restored): Award
  4.51PG prints "OK" while still counting and frames catch digits
  mid-update. The slowness is NOT host load: see "POST slowdown" above.
- Uncommitted: tools/nv4/guest/icons.c, build.sh (links shell32 and
  comctl32), this file.

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

  The driver on `nv4drv.iso` (4.11.01.0115) lists only 0020 (TNT), 0028
  (TNT2) and 002C (Vanta), so with it only the ERAZOR III (0028) can be
  tested; the ULTRA (0029) and LT (002D) need a later driver.

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
- **TNT2 VM:** `F:\Claude\86Box-Next\work-tnt2`, a copy of work-tnt with
  `gfxcard = nv5_erazor3` and `nv4drv.iso` in the CD drive; Windows found
  the TNT2 and installed the same driver by itself. Point the scripts at
  it with `TNT_DIR=/f/Claude/86Box-Next/work-tnt2`. When copying a VM:
  delete `uuid` from the copy's `[General]` (or 86Box asks "moved or
  copied?" in a dialog), and recreate `roms` as a junction
  (`New-Item -ItemType Junction`), since `cp` copies its 731 MB.
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
- **`tools/nv4/fullrun.sh THREADS GPU [SHOTS]`:** a whole 3DMark run, a shot
  every 30 s, `dev 3d` at the end, shutdown.
- **`tools/nv4/bench.sh THREADS GPU`:** boots, starts 3DMark's benchmark, times
  40 s of 3D shading, shuts down. If the owner is using the guest at the same
  time, the keystrokes go astray: check with `shot`.
- **Shutting down mid-benchmark:** Esc aborts, Enter closes the "aborted" box.
  A stray Enter can open the Result Browser, whose "save?" prompt then blocks
  Windows' shutdown ("not responding" box): `key 12,92` (End Task).
- **3DMark's score** (2620 at 800x600x16) is in guest time, so it does not move
  with host speed. Compare `dev 3d`'s emulation speed (guest CPU time over host
  time) and µs per span instead.
- **Environment switches:**
  - `NV4_GPU=0/1` overrides the "Draw 3D on the host GPU" option.
  - `NV4_3D_VERIFY=1` shades every parallel batch twice and logs any byte that
    differs ("nv4 3D verify: ..."). It is slow, since it copies the whole VRAM
    3 times per flush.
  - `NV4_GPU_PROFILE=N` logs the next N GPU batches one by one: spans,
    pixels, and GPU time for uploads, kernel and download.
- **Screenshot seams** are the shot catching a half-drawn frame, not tearing.
- **`tools/nv4/cycle.sh [WAIT]`:** Start > Shut Down in the guest, waits for
  the emulator to exit, copies `build-static`'s exe into the VM and boots it
  (minimized), then waits WAIT s (60). Use it after every build.
- **Changing CDs while the VM runs:** debug line `cdrom 0 F:\path\x.iso`
  (`cdrom 0` empties the drive). The UI does the mount, and saves it in
  the cfg.
- **Guest test programs:** `tools/nv4/guest/*.c` are Win9x programs built
  without the C runtime by `tools/nv4/guest/build.sh` (WSL's
  `i686-w64-mingw32-gcc`, `genisoimage`), which puts them on
  `work-tnt/nv4tools.iso`; `NV4_ISO_EXTRA=<folder>` adds that folder's
  files. `stretch.exe [ref] [bw]` draws a banner from 8/4/16/24/32 bpp
  DIBs and a device bitmap at six scales; `ref` draws through GDI's own
  engine for comparison.
- **Test software** (downloaded 2026-10-13, owner OK'd them and their
  EULAs; in `F:\Claude\86Box-Next\dl`, gathered in `dl\iso-extra`, all
  on the current `nv4tools.iso`): DirectX 7.0 (`DX7\dx70eng.exe`,
  installed on work-tnt), 3DMark 2000 1.1 (`3DM2000`, installed on
  work-tnt, registered as Pro: user `Futuremark`, code
  `6VZTY-SDZ75-KA9V7`, from the owner), UT demo 348 (`UTDEMO`), NVIDIA
  9x drivers 8.05 and 71.84 unpacked (`NV805`, `NV7184`; both list the
  TNT2 ULTRA/M64/Vanta).

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
    for the GPU's pixels (LFB, VGA window, the 2D classes that read VRAM:
    blit, scaled image, M2MF). `nv4_3d_touch(lo, hi)` flushes/waits only if
    the batch (its regions or textures) or the batch in flight reaches
    those bytes. Vertical blank waits only when the batch in flight draws
    into the displayed bytes. Anything new that reads or writes VRAM from
    the CPU must call `nv4_3d_sync` or `nv4_3d_touch` first.
  - **Regions:** a batch holds up to 4 row layouts (base, pitch) whose bytes
    don't meet (e.g. back buffer + Z buffer); rows stay on their own
    bytes, so threads/GPU stay correct. A state whose layout would meet
    another layout's bytes flushes first.
  - **2D joining the batch:** the classes that only write their
    destination (lines, triangles, rectangles 0x1e/0x5e, GDI 0x4a/0x4b,
    image from CPU) don't sync at method entry; `nv4_draw_begin` decides.
    A plain store (SRCCOPY / ROP 0xCC, no chroma, no pattern, linear
    surface) into a layout the batch already draws, or a large fill (64K+
    pixels while 3D is active), becomes fill spans in the batch (a fill
    state per surface; each fill's triangle record carries the packed
    pixel in the bits of `c[0]`); `nv4_put` gathers such pixels into
    one-row runs (`nv4->run2d`), queued at the next method or flush.
    Anything else calls `nv4_3d_touch` before its first pixel.
  - Both of these are hardware methods on the TNT (trapping them loops the
    resman):
    - SetContextSurface (0x18c) of the D3D classes.
    - Writes through a swizzled-surface object (CTX_SWITCH1 bit 14) into
      surface 5.
  - DxDiag's D3D test passes. 3DMark 99 Max runs fully and correctly.
- **TNT2 (ERAZOR III, 0028):**
  - Its straps follow envytools' nv4_straps: bits 5:2 are a memory
    configuration the ELSA BIOS looks up in a table of PFB_BOOT_0 values
    (entry 0: 32 MB SDRAM, 128-bit; 1: 64-bit), bus type in bit 14, device
    id low bits in 13:12. The TNT keeps the layout its BIOS expects (RAM
    type 3:2, width 4, AGP 5).
  - The BIOS probes 32 MB through the real-mode window (3D0-3D3): the
    pointer is 26 bits on the TNT2, and framebuffer byte = pointer - 16 MB.
  - With those, BOOT_0 reads 0x24 and the driver uses 32 MB (it showed 16
    MB before).
  - Win98 + 4.11: desktop at 1024x768x32, DxDiag shows DirectDraw and
    Direct3D accelerated, 3DMark 99 Max completes on the GPU lane with
    every test rendered (2620 3DMarks, as the TNT: the emulated Celeron
    sets the score).
  - 3DMark on this VM is slower to start (~35 s splash after the card
    change); the scripts now wait 80 s for the desktop and 40 s for 3DMark.
- **AGP textures:** both the CPU span shader and the GPU gather read system
  memory in place (`ram`), resolving AGP aperture addresses with
  `agpgart_resolve()` (added to `src/video/agpgart.c`, shared code). Before
  that every texel went through the aperture's mapping byte by byte.
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
  - A flush submits and returns. Two batches can be in flight (slots with
    their own records, command buffer, fence and timestamps; they complete
    oldest first):
    1. If this slot's last batch is still in flight, complete it (and any
       older one): wait, copy its rows back into VRAM.
    2. Textures: the 32 MB arena is a cache keyed by (DMA object, offset,
       size). Each batch hashes its textures' bytes where they are (VRAM,
       or system memory, through the AGP aperture via `agpgart_resolve`)
       and copies only new or changed ones. A full arena is emptied and the
       batch gathered again. A texture whose bytes a batch in flight draws
       (render to texture) completes that batch first; any texture copy
       waits for every batch in flight (they read the arena).
    3. Upload the touched colour/Z rows, except bytes a batch in flight
       owns (the GPU's mirror is newer there), and the records.
    4. Dispatch; copy the rows to a staging buffer.
    The rows reach VRAM when the slot is reused or a sync point waits.
  - Verify mode completes each batch at once, so it cannot exercise the
    2-slot pipeline (uploads that skip in-flight rows). Reason about every
    reader of VRAM when changing it.
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

  Emulation speed in the same window ("dev 3d" prints guest time over host
  time): GPU 96%, 8 threads 68%, 1 thread ~15%. The GPU figure above is
  submit time plus waits. Before this work, 1 thread spent
  34 s of the 40 s on 3D.
- **GPU waits:** `dev 3d` lists them by cause. State-only classes (surfaces,
  ROP, beta, chroma, pattern, clip) neither flush nor wait
  (`nv4_pgraph_state_only`); 2D draws join the batch or wait by byte range
  (see 3D above). Over a whole 3DMark run (TNT, GPU lane) the emulation
  thread spends 9.5 s submitting and 4.9 s waiting (was ~70 s in all at
  the start of the GPU work); most waits are at vertical blank when a
  batch in flight draws into the displayed bytes (front-buffer rendering).

## Next (plan of 2026-10-13)

Where we stand against the owner's plan: phases 1-5 and 7 are done for
one driver (4.11.01.0115) and one benchmark (3DMark 99) on Win98. The
biggest risk now is coverage, not speed: every new driver, API and OS
has found new paths so far. So the order is: finish what is open, widen
the software tested, fill the missing units, then variants, speed and
integration.

Work that touches separate files can run in parallel sessions: overlay
(new `nv4_pvideo.c`), 2D classes (`nv4_pgraph_2d.c`), 3D
(`nv4_pgraph_3d*.c`). Say in the "In progress" line at the top what you
took, and commit only your own paths.

### A. Close what is open
1. ~~InstallShield banner glitch~~ done (c59c71997): a 4 bpp DIB drawn
   1:1 goes through the indexed image class with 4-bit indexes, high
   nibble first; we took the low one. Same commit: SIFM bilinear (FOH)
   filtering, which the driver asks for on every stretched DIB.
2. ~~Unhandled-object counter~~ done (411712ac9): `nv4_gap()` logs
   "nv4: not modeled: ..." once per case and `dev` lists the counts.
   First catch: 3DMark 2000's startup sets up a DVD subpicture object
   (0x38; context methods only so far).
3. **Rendering into a swizzled 3D colour surface** (SURFACE type 2,
   render to texture): implement when A2 shows a program using it
   (3DMark 2000 and DX6/7 demos do). Rows in the swizzled layout break
   the "row = base + y*pitch" region model: either shade such batches
   serially on the CPU, or give regions a swizzled kind on both CPU and
   GPU (bit-exact rules apply).

### B. Widen the software tested (Win98, TNT then TNT2)
4. **OpenGL ICD.** `nv4ogl.dll` is already installed with 4.11. The ICD
   drives the card itself (its own channel, DMA pusher, likely other
   method patterns than the D3D driver), so expect new paths. Tests:
   GLQuake / Quake II, then Quake III Arena (ISO in
   `C:\Users\xeon4\Downloads\Quake_III_Arena_Id_Software_1999`). Q3
   needs a faster guest CPU than the Celeron 233: use a PII/PIII on the
   p6bap in a VM copy (`work-tnt-q3`), not by editing work-tnt.
5. **More Direct3D/DirectDraw:** DxDiag's DirectDraw tests (all modes,
   8/16/32 bpp), 3DMark 2000 (needs DX7 runtime on the guest; exercises
   DX6 multitexture, render to texture, high-res), and games the owner
   has (Unreal Tournament D3D, Incoming, Forsaken, Expendable...).
   Ask the owner what is on hand; use the recommended default (3DMark
   2000 + UT demo) if no answer.
6. **A 1999 Detonator** (2.08 / 3.53 / 3.68): lists 0029 (ULTRA), 002C
   (Vanta), 002D (M64), so it is what tests those boards; newer drivers
   use the NV5 class variants (0x63-0x66) and may move set-context
   methods to hardware via DEBUG_3. The owner has to supply the files
   (not in the archive drop or `F:\86box FTP`).
7. **Windows 2000** (inbox NVIDIA driver) and **NT4** (archive driver):
   different resman, multiple channels, NT's DMA/AGP handling. Copy a
   VM from the `Latest\Windows 2000` / NT4 rigs into `work-w2k-tnt` /
   `work-nt4-tnt`, switch the card, never edit the rigs. Remember the
   NV3 Win2000 restart hang lesson (dynarec stale ROM blocks on PAM
   change, NV reset on hard reset): test Start > Restart too.
8. **Linux (optional):** XFree86 3.3.x/4.x `nv` driver and `rivafb` use
   documented paths and are a second opinion on 2D/PFIFO behaviour.

### C. Missing hardware
9. **PVIDEO overlay** (now a register file in `nv4_stubs.c`): YUY2/UYVY
   overlay with scaling, two buffers, colour key, and its interrupt
   (buffer done). NV3's PVIDEO in `wt-nv3` works and was tested with
   Indeo/Media Player and the colour key; port the behaviour to NV4's
   register layout (rnndb, envytools). Tests: Media Player with MPEG-1 /
   Indeo files (as on `work-indeo`), DirectDraw overlay in DxDiag.
10. **Classes still ignored:**
    - 0x48, the DX3 textured triangle kept from NV3 (old D3D apps,
      possibly the ICD's fallbacks).
    - 0x38 DVD subpicture (with the overlay; DVD players).
    - NV5 0x63-0x66 (NV5 SIFM/IIFC/IFC/SIFC variants): needed by the
      Detonators of B6. envytools `hwtest/pgraph.cc` has their method
      lists.
    Do them in the order A2's counters show them.
11. **PME (media port), TV encoders** (Bt869/Chrontel on I2C): only if a
    BIOS or driver hangs probing them.

### D. Platforms and board variants
12. **DOS/VESA:** a VBE tester (mode list, bank switching, linear
    framebuffer, scrolling/display start), DOS games in VESA modes (Quake
    1 `-vesa`), on the p6bap (Award) and an AMI board (where NV3's ROM
    sizing bug hid). The ERAZOR II PCI on a Socket 7 board
    (430TX/VX), to keep the PCI-only path honest.
13. **M64 / Vanta:** the 64-bit memory configuration (BIOS strap table
    entry 1) is wired; test with the LT (002D) once B6 gives a driver.
    More boards are in `F:\86box FTP\NVidia Roms\nv4` / `nv5` (Creative,
    Diamond Viper V550/V770, STB Velocity 4400, Leadtek, Hercules,
    ELSA Synergy II): add a few once the ELSA set is solid, the same way
    (ROM into `roms/video/nvidia/nv4`, Xeon3D/roms master, rigs).
14. **AGP 4x on NV5:** check the AGP capability (version, rate bits) the
    TNT2 reports against the datasheet; the driver may pick its AGP rate
    from it.

### E. Speed (when a workload from B needs it)
Quake III and UT will be the first loads heavier than 3DMark 99; measure
them with `dev 3d` before optimising.
15. **CPU span shader:** fixed-point attribute stepping instead of ~14
    float plane evaluations per pixel; per-span texture setup. Must stay
    bit-exact with the GPU kernel: change both together, check with
    `NV4_3D_VERIFY=1`.
16. **GPU lane:** kernel variants by state (specialization constants, as
    the Rage 128 does; `combine()`'s dynamic `src[]` indexing goes to
    local memory); skip texture hashing for VRAM pages nothing wrote
    (needs write tracking in every VRAM writer); at vertical blank wait
    only for rows already scanned; move row grouping/copies to a worker.
17. **Capture/replay harness** (the owner's plan asked for it): record a
    method stream (FIFO pushes + the VRAM/texture bytes they read) from a
    real run and replay it headless, comparing frames. Lets E15/E16 and
    refactors be checked without booting Win98 each time.

### F. Integration
18. **Merge master into `tnt`:** master is 8 commits ahead, all NV3
    (5ee69da2e already checked them; fed56d813 is F21). Rebuild, re-run
    3DMark on TNT and TNT2, then a review pass over `src/video/nv/nv4`.
19. **Owner's call:** push `tnt` to Xeon3D/86Box-Next, PR into master,
    a TNT rig in `Latest` (with `.stage-branch = tnt`, as NVIDIA-RIG),
    and a Qt 6 release build. Ask before pushing.
20. **Docs:** a driver/RM notes file like NV3's `NV3-DRIVER-NOTES.md`,
    and a test matrix (board x OS x driver x program) in this document.
21. **DAC checksum** (TEST_CONTROL/CHECKSUM): only if a TNT test program
    needs it; NV3's LFSR (fed56d813) is the starting guess.

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
  - TNT2 brought up: strap layout and real-mode window fixed so the ELSA
    BIOS finds 32 MB; work-tnt2 VM.
  - GPU texture cache (hash, copy only changed textures) and AGP textures
    read in place: texture step over a full run 49 s -> 6 s.
  - Batch regions, 2D fills/runs joining the batch, range-based waits
    (`nv4_3d_touch`): GPU waits over a full run 37 s -> 12 s (ffbfc79c2).
    Threaded verify (4 threads) over a whole run: 12622 batches, none
    differ.
  - 2-slot GPU pipeline: submit 19.5 -> 9.5 s, waits 12 -> 4.9 s per run.
  - Mishap: bench.sh started a VM while the previous one hung at shutdown;
    two instances shared the disk for ~8 min. Win98 came back clean. tnt.sh
    now refuses a second instance.
  - Plan for the next steps written ("Next" above): coverage first
    (OpenGL ICD, more D3D, Detonator, Win2000/NT4), then overlay and the
    missing classes, variants, speed, integration.
- 2026-10-14:
  - Quake III works on the TNT2 (OpenGL ICD): 2D pitch clip removed
    (5b120b5de); debug aids speedlog / bounded trace (4c499c8e7).
  - POST slowdown on work-q3 reproduced and profiled (dynarec
    recompiling in the ELSA BIOS's text output); cause still open.
