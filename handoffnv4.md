# Handoff: NVIDIA RIVA TNT / TNT2 (NV4/NV5) emulation

Living document: whoever works on the TNT updates it as they go (status, the
log at the bottom). Last updated: 2026-10-15.

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
  sample (tools/nv4/sampler.py: suspends threads, symbolizes RIP with
  nm) shows 76% of the emulation thread in codegen_block_init: the
  dynarec keeps recompiling. Same board and card at 266 MHz
  (work-post: a POST-only copy, no disk, BIOS defaults or work-q3's CMOS)
  stays at 100% through the same screen, and the owner sees no slowdown
  on other boards at 266: it comes with the Celeron 466. **The owner caps
  test CPUs at a Celeron 266** (their PC can't run faster ones), so this
  is parked: not an NV4 bug, and not hit under the cap.
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
- Branch `tnt` in worktree `F:\Claude\86Box-Next\wt-tnt`. Merged into master
  and pushed on 2026-10-14 (owner's request), shipped in Release 2.1.4.8;
  keep working on `tnt` and fast-forward master when the owner asks.
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

  Other makers' boards (2b1b66874, all AGP, ROMs in Xeon3D/roms
  `video/nvidia/nv4`): Diamond Viper V550 (195c0550.rom), Creative
  Graphics Blaster RIVA TNT (nv4-creative.rom), STB Velocity 4400
  (nv4-stbvelocity4400-110.rom), ASUS AGP-V3400TNT (v3400tnt.rom,
  SGRAM) -- TNT; Diamond Viper V770 (diamond-viper-v770.BIN), Leadtek
  WinFast 3D S320 II (rivatnt2leadteks320ii.BIN) -- TNT2. All post on
  the p6bap; V770 + 8.05 and Creative + 71.84 run the Win98 desktop
  accelerated. Not added: ASUS AGP-V3800PRO (PCI ID 0029 vs its name),
  the other ELSA variants (E3230500.A2P/A2V/AON/APN/APV/AUV, Synergy
  II-32, ER220300.ATN).

  The driver on `nv4drv.iso` (4.11.01.0115) lists only 0020 (TNT), 0028
  (TNT2) and 002C (Vanta); the ULTRA (0029) and LT (002D) need NVIDIA
  8.05 or 71.84 (dl/iso-extra, on nv4tools.iso).

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
  `gfxcard = nv5_erazor3` (now `nv5_tnt2_agp`, model ERAZOR III) and `nv4drv.iso` in the CD drive; Windows found
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

## What is left (2026-10-14, after Release 2.1.4.8)

Done: boot/VGA/VESA, PFIFO, 2D, D3D (DX5/DX6), OpenGL ICD (Quake III),
threads + GPU lane, TNT2 straps, merged into master and released. The
plan below (2026-10-13) has the detail; this is where each item stands.
Test VMs: no CPU faster than a Celeron 266 (owner's PC; see the memory
note), so expect slow runs rather than raising the CPU.

Recommended order: 4, then 7, then 5.

**GLQuake / Quake II on the TNT (2026-10-15, later session).** VM
work-tnt (ERAZOR II AGP, 4.11 ICD, Celeron 233, 2 threads + GPU lane,
640x480x16 fullscreen):
- GLQuake (C:\GLQUAKE, `glquake.exe -basedir c:\glquake`): windowed
  (`-window`) renders correctly at once. Fullscreen showed the old
  desktop and drew nothing (0 spans): **not an emulation bug.** GLQuake
  minimizes its fullscreen window on WM_KILLFOCUS (its WndProc,
  0x4151e0, case 8 -> ShowWindow SW_SHOWMINNOACTIVE), and with the
  shareware PAK0 Quake's file system never opens loose files in
  subdirectories, so `id1\glquake\15to8.pal` is never read back: every
  start regenerates it behind a topmost progress dialog (template 0x6d)
  that takes the focus. The ICD then sees an iconic window (USER: 3000,3000,
  client 0x0) and sends every frame to a dummy target (colour = its 8 px
  fence surface, Z at the top of VRAM with pitch 32, clip x=8 width 0).
  Restoring the window (`c:\winlist.exe r 10 <cmd>`, see below) makes
  it render: **timedemo demo1 = 969 frames, 11.7 s, 82.7 fps**, ~80%
  emulation speed; console, particles, models all correct.
  Confirmed with the owner's full Quake (G:\Old OS\Games\DOS\Quake,
  on games.iso as QUAKEFUL\, its PAK0/PAK1 copied over
  C:\GLQUAKE\ID1): fullscreen GLQuake then works straight from
  Start > Run, **timedemo demo1 = 969 frames, 11.4 s, 85.3 fps**, 83%.
- **Quake II 3.05** (C:\QUAKE2, `quake2.exe +set basedir c:\quake2 +set
  vid_ref gl`; the owner's config has vid_ref soft): fullscreen OpenGL
  correct (textures, lightmaps, models, HUD). **timedemo 1 / demomap
  demo1.dm2 = 689 frames, 17.9 s, 38.5 fps**, 87% speed; GPU waits 12.4 s
  on class 5f and 4.5 s on class 77 per ~100 s (speed leads).
- Alt+Tab out of fullscreen GLQuake and back crashed the 4.11 ICD
  (NV40GL.DLL 0167:695925bd: IDirectDraw::CreateSurface through a stale
  DDraw pointer 0x1800, after the mode was restored and set again), then
  the process spun forever on the ICD's global lock (0x695d1e70) and
  Windows could not shut down (hard stop; Win98 came back normally).
  Probably the 1999 driver's own alt-tab bug; not chased.
- The TNT traps class 0x53 0x2f8/0x2fc (clip horizontal/vertical); the
  resman emulates them by writing PGRAPH ABS_UCLIP min/max with
  CTX_SWITCH1 parked on a dummy object (NV4 forces the clip minimum to 0
  for the 3D surface class). The ICD sends them from its clip rects
  (x = left + offset, w = right - left). Our ILLEGAL return + the RM
  write path is right.
- New aids: `dev` prints the user clip / image clip registers.
  `tools/nv4/guest/winlist.c` (on nv4tools.iso as WINLIST.EXE; copy it
  to C:): `winlist [r] [N] [cmd]` starts cmd and logs every 5 s to
  C:\WINLIST.TXT the screen mode, foreground window and each top-level
  window's class/title/styles/rects/IsIconic/GDI clip box; `r` restores
  a minimized window of cmd from the 5th snapshot. Read the file back
  with the VM down: the disks are dynamic VHDs -- convert in WSL
  (`python3 tools/nv4/vhd2raw.py X.vhd /tmp/x.raw`, sparse, in
  WSL), partition at sector 2048, `mtype -i raw@@1048576 ::WINLIST.TXT`.
  nv4ogl.dll disassembles fine with capstone + pefile (py), as does
  GLQUAKE.EXE.
- **DOS/VESA done (2026-10-15)** on work-dos-tnt (TC430HX, Pentium 200,
  ERAZOR II PCI, MS-DOS 6.22 + MSCDEX, CD = D:). Its cfg had duplicate
  [General]/[Hard disks] sections (merged; old one in
  86box.cfg.bak-merge) and no `roms` (junction to Latest\98se-nv3\roms
  added; without it the emulator stops at a ROM dialog before
  debug_cmd starts). SciTech VBETest 8.00 (D:\VBETEST): "ELSA ERAZOR
  II", VBE 3.0, linear framebuffer; 640x480x16 banked and 1600x1200x16
  linear pass every step (Moire, double buffering 19.2/29.8 fps,
  virtual-screen scrolling). Full Quake (owner's 1.01, GO.EXE = quake,
  in C:\QUAKE): "VESA 2.0 compliant adapter: ELSA ERAZOR II", modes
  11-18 (640x400..1280x1024, 320x200/400/240) render; 1.01 never prints
  a timedemo summary, so the 1.06 exe (C:\QUAKE\Q106.EXE, from the
  shareware folder) was used: **timedemo demo1 640x480 = 969 frames,
  44.0 s, 22.0 fps**. 1280x1024 and 320x240 (line-doubled) checked.
  DOS 6.22 xcopy has no /i (md first).
  **VBE reports 8192 KB on the 16 MB card: genuine BIOS behaviour, not
  ours.** The VBE 4F00h handler takes TotalMemory from a routine that is
  `push bx / push esi / mov ax,CONST / pop esi / pop bx / ret` (ERAZOR II
  PCI at C000:04DF, unchanged after POST). NVIDIA's reference NV4 BIOS
  (98dec Alpha.87 Nv4_bios.zip, Chip/NV4/NV4.asm OEMGetMemSize) has the
  PFB_BOOT_0 read commented out ("limit to 4MB size for VGA") and
  returns 64 blocks; vendors changed only the constant. Per ROM
  (search `53 66 56 b8 xx xx 66 5e 5b`): ELSA TNT 2.03/2.05 = 8 MB,
  ELSA TNT2 3.05 + Synergy II + Creative + ASUS V3400/V3800 + Leadtek
  = 4 MB, Diamond V770 = 16320 KB; STB 4400 1.10 = `mov ax,0ffh`
  (16320 KB); Diamond V550 1.95 reads PFB_BOOT_0 bits 1:0 (our 0x1f ->
  16 MB). README row updated.
- README on GitHub (master d943c7cc6, cherry-picked from tnt) now has a
  tested-configurations table (OS x driver x card); keep it current.
  master and tnt now differ by those cherry-picks: next time tnt goes
  to master, merge (not fast-forward).
- The owner's software library for any OS is G:\Old OS\ (Games,
  Drivers, Apps).

**71.84 Direct3D (2026-10-15 night): a driver limit, not ours.** Traced a
whole 3DMark 99 run on work-7184 (Creative TNT) with the new bind trace
(`dev trace N [CH] novtx`: object binds with their instance words,
methods without 3D vertex data or push words) and disassembled 71.84's
nvdd32.dll (capstone; `scratchpad dis.py` + header scan for
`(count<<18)|(subc<<13)|mthd` immediates):
- The D3D channel (ch 1 here) binds 0x54/0x55 on subchannel 7 with
  SetContextSurface (0x18c) = the 0x53 object d3d05301 (inst 28d1), which
  only ever gets SET_CONTEXT_DMA_COLOR/ZETA. No 0x53 FORMAT/PITCH/OFFSET,
  no 0x58-0x5b, and no RM register write to BOFFSET/BPITCH/BPIXEL
  (`dev regs`) in the whole run. Per frame the driver clears Z (0xd08000,
  Y16 0xffff) and the colour buffers (0xdf3000 / 0x1000, flipping) through
  the 2D surfaces object (0x42, subchannel 1, DST offset) and writes a
  Y32 fence at DST offset 0 (0x5e 1x1) before each draw batch.
- In nvdd32.dll the only 0x53 FORMAT/PITCH/OFFSET writes are in the
  GeForce (Celsius/Kelvin) init paths (variable subchannel, `or 0x40300`
  at b0296f28 / b02b450f / b02d8985 / b0306632) and in a 3D blit helper
  (b026c574, its own 0xd3d05301 bind). The TNT render-target code
  (b0266070, called from b0266880 etc.) programs the 0x42 object: SRC =
  Z buffer, DST = colour, PITCH = colour<<16 | Z.
- envytools (d3d56 TLVERTEX test): a vertex with the colour surface
  format 0 raises FORMAT_EXCEPTION (NSOURCE bit 9) when DEBUG_3 bit 22 is
  set -- and the 71.84 RM sets that bit. Modelled as an experiment: the
  RM just acks it, nothing fixes the surfaces, and 3DMark then hangs.
  Reverted (triangles are dropped as before, counted by `dev 3d`).
- Real hardware: VOGONS reports 71.84 on a TNT2 Ultra running no 3DMark
  99/2000 tests (back to the desktop; other cards froze or bluescreened)
  and older drivers fixing it. README row updated.
- Aborting 3DMark under 71.84 (Esc) crashes it in KERNEL32 with FS=0
  (0167:bff71ec8, EDI in nvdd32's globals at b03e8ae8), followed by
  Explorer/Msgsrv32 faults; reproducible twice. Not chased: probably the
  same driver path; the VM needed hard stops. work-7184 is back to a
  clean shutdown but its desktop is now 800x600 (was 1024x768).

**NVIDIA 45.23 on the TNT2, July 1999 / ASUS Detonators (2026-10-15 day).**
VM `work-4523` (copy of work-tnt2: P6BAP-AP VIA 694X, Celeron 233, ERAZOR
III TNT2, Win98 SE, DirectX 8.1). 45.23 lists only the TNT2 family and
Vanta (NV5 IDs 0028/0029/002C/002D): Setup refuses a TNT (0020).
- July 1999 Detonator 2.08 (`july_1999_0208.zip`, work-jul99, TNT):
  3DMark 99 2792, GLQuake 85.9 fps. ASUS December 1998 (work-asus, ASUS
  V3400TNT): 3DMark 99 2805, GLQuake 85.9 fps. Both fine.
- **Mode-change deadlock, fixed (nv4_pgraph.c).** Switching colour depth
  (16 <-> 8) hung about one time in three: the resman's mode set resets
  PGRAPH and reinitialises it with FIFO ACCESS off and CTX_CONTROL CHID
  invalid, then expects the next channel's method to raise
  CONTEXT_SWITCH -- its context-switch handler loads the channel and
  turns FIFO access back on (STB tree GR.C grService). We checked FIFO
  access first and stalled silently. Now a method from another channel
  (or with CHID invalid) raises CONTEXT_SWITCH whatever the FIFO access
  bit says (only once while it is pending); same-channel methods still
  stall while access is off. PGRAPH reset values also set (FIFO = 1,
  BLIMIT0-5 = 0x80000000, nv4_ref.h "I" values). `depthsw` guest tool
  (tools/nv4/guest/depthsw.c: N ChangeDisplaySettings flips, log in
  C:\DEPTHSW.TXT) ran 8 flips + restore without a hang. A slow mode set
  can show a black screen for ~30 s first; that is not a hang.
- **Direct3D 6/7 HAL missing ("3DMark needs a 3D-accelerator that is
  capable of using textures", DxDiag D3D7 "step 8 Creating 3D Device
  0x80004005"; DX8 passed): a VIA chipset emulation bug, fixed
  (src/chipset/via_apollo.c).** Found by dumping 45.23's nvdd32.dll from
  guest memory (most of its .text is encrypted on disk; decrypted in RAM:
  `peek`/`peekp` page dump, scratchpad memdump.sh) and breakpoints:
  D3DHAL ContextCreate (b0127970) allocates a 4 KB buffer from its AGP
  heap; the heap commits AGP memory through NvRmAllocMemory (class 0x3e,
  flags 0x2100 = AGP, write-combined) -> resman osAllocPages ->
  VGARTD _GARTReserve/_GARTCommit -> VIA's miniport VIAGART.VXD (VIA
  4in1 4.25, now installed in work-4523, AGP part only, Standard mode).
  VIAGART's init requires the host bridge (0:0.0) to have a capability
  list (status bit 4) with an AGP capability (ID 2); ours reported
  status 0xa0 for every VIA chipset because of
  `(dev->id >= VIA_585) || (dev->id < VIA_597)` -- always true, VIA_585
  is the lowest id (came with upstream's "Merged the VIA VPX and AMD 640
  on the common Apollo tree", 22cdad8c7; still in upstream). Fixed for
  the status register first (VP3 on: 0x90, cap list at A0h); the rest of
  that test's uses were audited and fixed afterwards (see "VIA Apollo
  audit" below). Without a working GART
  driver 45.23's D3D 6/7 should fail the same way on real VIA boards
  (a forum report of the same DxDiag "step 8" failure with 45.23 on
  Win98 SE; card not named).
- **8-bpp desktop icons drawn as silhouettes, fixed (nv4_pgraph_2d.c
  nv4_img_color_data).** At 8 bpp the display driver sets IFC/SIFC's
  colour format to X8R8G8B8 and sends four 8-bit indices a word (sizeIn
  rounded to 4) -- the STB tree's dmaenab.c does the same (it sets
  X8R8G8B8 "if colorDepth == 32 || colorDepth == 8" and packs
  translated indices). We took one pixel a word, so only the top quarter
  of each 32x32 icon was drawn. Now a 32-bit colour format into a Y8
  surface (index mode) unpacks four Y8 pixels a word. Every NVIDIA driver
  at 8 bpp was affected, not only 45.23.
- Results on 45.23 after the fixes (work-4523, 2 threads + GPU lane):
  3DMark 99 Max **2679** (800x600x16; rendering correct; Esc abort is
  clean, unlike 71.84); GLQuake (full Quake data in C:\GLQUAKE)
  timedemo demo1 **969 frames, 17.4 s, 55.6 fps** at 640x480x16 -- lower
  than 4.11's ~85 fps, most likely 45.23's OpenGL waiting for vsync at
  60 Hz by default (not checked); DxDiag DX7/DX8 not rerun after the
  AGP fix (3DMark 99 is DX6). Once, after a hard stop, Windows came up in
  16-colour VGA; setting High Colour and restarting brought the driver
  back.
- Regression with all three fixes: work-tnt (TNT, 4.11, same VIA board)
  3DMark 99 Max 2619 (was 2620), clean shutdown.
- Guest automation tip: Win+R (`key e0,5b,13,93,e0,db`) opens Run
  reliably; Ctrl+Esc then `r` often misses right after a console window.

**VIA Apollo audit (2026-10-15 evening).** `(dev->id >= VIA_585) ||
(dev->id < VIA_597)` (always true) came with upstream's 22cdad8c7, which
added the VPX: at the time VIA_585 was the only id below VIA_597, so it
meant "the VPX", now "VPX or AMD 640" = `dev->id < VIA_597`. Every use is
now that (src/chipset/via_apollo.c). For the AGP parts this restores the
pre-22cdad8c7 behaviour: 0x52 defaults 0x90 (694X) / 0x10 / 0x00, 0x59 =
0, 0x64 = 0xec (VT8601 datasheet: Rx52 = Dynamic Defer Timer, default
10h, bit 7 = GTL pull-up strap), write masks 0x51 0xeb, 0x53/0x59 0xf0,
0x76 0xf0, 0x70/0x73 full on 598/691, 0x71 full except VPX/694X, 0x58
full on 598/691. 0x05/0x0f keep the masks for every chipset (only SERR#
and fast back-to-back exist in 0x05). Two `(id != A) || (id != B)` tests
(0x69, 0x7e) are now `&&`: 0x69 read-only on VPX/AMD 640, 0x7e on
694X/8601. Not touched: `(dev->id == VIA_693A) && (dev->id < VIA_8601)`
in 0x69/0x6b/0x6c (redundant, intent unclear). AMD 640 and VT8601 have
no machines.
- POST, old vs new build, from clean CMOS and past F1 to the boot
  attempt: ficva502 (VPX), ficpa2012 (VP3), ax59pro (MVP3), ficka6130
  (Pro), p3v133 (Pro133), p6bap (Pro133A): identical screens (memory,
  cache, PCI device lists).
- Win98: work-tnt (p6bap) 3DMark 99 2616 (baseline 2619/2620); work-indeo
  (ax59pro, MVP3) boots to the desktop, no new hardware, clean shutdown.
- One work-tnt run scored 3162 (CPU 3DMarks 3131) and could not be
  reproduced (2616 next with the same exe; 2625/2629 with experiments).
  While chasing it: final PCI config of 0:0.0, 0:1.0, 0:7.x and 1:0.0
  differs only in 0x52 bit 7 (GTL strap) between old and new defaults,
  and guest CPU speed is identical (`cpuspeed`: 234 MHz, same memory
  pass) -- so treat one-off 3DMark outliers as noise and rerun.
- New debug aids: `pci BUS DEV [FUNC]` debug command (256 config bytes
  through the card's own read handler; pci_debug_read in pci.c), and
  tools/nv4/guest/cpuspeed.c (integer loop + 4 MB memory pass timed with
  GetTickCount and RDTSC, C:\CPUSPEED.TXT).

**Where the 2026-10-15 session stopped (read this first).** The owner
asked for items 1-4 and 6 of the list they were given (= here: 71.84,
NT4, the NV5 classes 0x64-0x66, Quake III/GLQuake/Quake II, DOS/VESA).
- Done and committed on `tnt` (e6cfa815c, not merged to master, not
  released, rigs not staged -- ask before merging/pushing/staging):
  71.84 hang fixed (awaken notify), scan-out start latch, debug aids.
  71.84's D3D still draws nothing (item 4 below has the analysis).
- NT4 finished: DirectDraw overlay (OVLTEST, all three phases) on 28.32
  and 43.45; 43.45 installs (pick "NVIDIA RIVA TNT"), desktop, OpenGL
  pipes, Restart. The "Bt868" label: the miniport never probes I2C
  (no CR3E/3F traffic) -- the straps say TV disabled (bits 8:7 = 3), so
  the panel shows its first encoder name. Not an emulation bug.
- 0x64-0x66: no driver at hand uses them. Checked: Win2000 inbox driver
  and 71.84 for Win2000 on a PCI TNT2 (work-w2k-tnt), 8.05, 71.84 9x --
  all use 0x60/0x61 (and 0x72 once). NVIDIA's display driver picks the
  NV05 classes only when the 16-bit half reports "dithering control"
  (STB tree dmaenab.c), which none of these do. They alias the
  driver-tested 0x60/0x61/0x76 paths plus one ignored colour method, so
  the risk is low; a bare-metal DOS test would be the next step if ever.
- Quake III timedemo (work-q3, TNT2 ERAZOR III, Celeron 266, 640x480,
  `timedemo 1; demo demo001`): 1346 frames, 39.0 fps in guest time on
  both paths; emulation speed 48% with the GPU lane (4 threads + GPU),
  29% on the CPU path (4 threads, NV4_GPU=0). The GPU lane waited
  48 s of 196 for syncs, all caused by class 0x5f blits -- a speed lead.
  The work-q3 config migrated cleanly from `gfxcard = nv5_erazor3`.
- Icon cache (owner's request): `C:\WINDOWS\ShellIconCache` deleted on
  work-q3 from MS-DOS mode; the Quake III shortcut arrow is back, so
  loose end 1 was the stale cache. work-tnt still has its old cache.
- GLQuake / Quake II / DOS: prepared, not run yet.
  - `F:\Claude\86Box-Next\dl\games.iso` (source folder dl\games\iso):
    `GLQUAKE\GLQUAKE.EXE` (from the owner's GLQ1114.EXE) +
    `GLQUAKE\ID1\PAK0.PAK` (Quake 1.06 shareware, from the BeOS apps
    folder's quake106.zip), `QUAKE1\` (DOS Quake 1.06 shareware:
    QUAKE.EXE, CWSDPMI -- also a VESA test), `QUAKE2\` (the owner's full
    Quake II 3.05 from G:\Old OS\Games\Win9x\Quake 2\QUAKE2.EXE,
    extracted), `VBETEST\VBETEST.EXE` (SciTech, from UniVBE 6.70 --
    run it without loading UNIVBE so it tests the card's own VBE).
  - work-tnt (TNT + 4.11, US keyboard, CD = D:) has games.iso in its
    cfg now (old cfg in 86box.cfg.bak-games); GLQuake is copied to
    C:\GLQUAKE (`xcopy d:\glquake c:\glquake /s /i` through Start > Run;
    VM shut down cleanly). Next: run `c:\glquake\glquake.exe` (OpenGL through 4.11's ICD on
    the TNT), console `timedemo demo1`; then Quake II (copy D:\QUAKE2,
    `quake2 +set vid_ref gl` / timedemo demo1); compare fps and look
    for missing triangles/texture faults.
  - DOS/VESA: VM `F:\Claude\86Box-Next\work-dos-tnt` = copy of the
    owner's Latest\MS-DOS rig: Intel TC430HX (AMI BIOS, Socket 7,
    PCI), Pentium 200, now `nv4_tnt_pci` model ERAZOR II PCI, games.iso
    in the CD, muted, uuid removed, romsx copied. Never booted yet:
    check MSCDEX gives a CD letter, run D:\VBETEST\VBETEST.EXE (mode
    list, every mode's test pattern, bank/linear framebuffer, display
    start/scrolling), then D:\QUAKE1\QUAKE.EXE in its VESA modes
    (`vid_describemodes`, `vid_mode N`).
- VM states: work-7184 back to the Creative TNT (TNT2 Ultra test cfg
  was temporary; `86box.cfg.tnt` = its saved TNT cfg). work-w2k-tnt is
  now a PCI TNT2 (ERAZOR III) with NVIDIA 71.84 for Win2000 installed
  (its ERAZOR II PCI cfg is in `86box.cfg.tnt`). work-nt4-tnt has
  43.45 installed over 28.32. work-ultra unchanged (8.05). All shut
  down cleanly; no VM is running.
- Driver ISOs made this session: dl\nt4drv2.iso (NT2832, NT4345, SP6A),
  dl\w2kdrv.iso (W7184, W4523 = NVIDIA 71.84 / 45.23 for Win2000/XP,
  extracted), dl\nv805root.iso (8.05 at the root, for Win98's wizard).
- NVIDIA source in the owner's STB archive (confidential, cite only in
  commit messages): C:\Users\xeon4\Downloads\stbdata\Archive\recovered\
  latest\SChanGfx -- NV4Ver1 (Dell, nv4_cpq, V128II trees), NV4Ver2\V4400
  (STB Velocity 4400: Win9x display/ddraw/D3D, resman incl. gr\nv4\
  GRCTXT.C, modular class064-066.c), NTNV4Ver3 (NT4). The D3D driver's
  render-target code is nv4state.c nvSetD3DSurfaceState.

**Loose ends**
1. ~~Desktop shortcut arrows~~: confirmed a stale ShellIconCache
   (fixed on work-q3 2026-10-15 by deleting it from MS-DOS mode; do the
   same on work-tnt if its arrows are missing).
2. POST slowdown at 466 MHz (see In progress): parked, not hit at 266.
3. Quake III follow-ups: ~~timedemo with the GPU lane~~ (39.0 fps,
   48% vs 29% emulation speed; see the session note above); ~~GLQuake
   and Quake II on the TNT~~ done (82.7 / 38.5 fps, see above); Quake
   III on the TNT if wanted (needs installing on work-tnt).

**More software and drivers**
4. Later NVIDIA drivers (B6): 8.05 and 71.84 are already unpacked in
   `F:\Claude\86Box-Next\dl` (`NV805`, `NV7184`) and list the TNT2 Ultra
   (0029), M64 (002D) and Vanta. They test the ERAZOR III ULTRA and LT,
   and the NV5 classes 0x64-0x66 (13ba43f0b, never run). There is no
   0x63 on NV5: envytools lists `sifm_nv5` (0x63) from NV10 on only.
   VM: `work-ultra` (copy of work-tnt2, ERAZOR III ULTRA, Celeron 233,
   nv4tools.iso in the CD drive: `NV805`, `NV7184`); 8.05 is an INF
   install (NVAML.INF), so Windows' new-hardware wizard takes it.
   **8.05 done on the ULTRA (2026-10-14):** installed (keep the newer
   vgartd.vxd when asked), desktop 1024x768x32 accelerated; its 2D still
   uses 0x60/0x61, not 0x64-0x66; it copies DirectDraw overlay frames with
   class 0x38 (now drawn). 3DMark 99 MAX on the ULTRA + 8.05: 2714
   3DMarks after two fixes -- DMA transfers split at the window's pages
   (76d77a669: 8.05 writes through objects whose window starts 0x1f8
   into a page; the tail landed in an unrelated frame and Windows died
   with 0E in VMM at random) and the cursor address bits in CR2F
   (8b4101702: 8.05 keeps the cursor above 16 MB). Regression: the TNT
   + 4.11 full run gives 2613 (2616-2620 before).
   LT (M64) + 8.05: 2717 3DMarks (VM `work-lt`, now fitted with the
   Viper V770). Black blue screen fixed (0f68a7bd8): the CRTC start
   registers and PCRTC_START are one latch.
   **71.84 (2005) is parked:** VM `work-7184` (copy of work-tnt2, now
   fitted with the Creative TNT). Installs and runs the desktop on the
   ULTRA and the TNT, but 3DMark 99 never gets its 800x600 mode: no
   CRTC/RAMDAC write at all reaches the card (`dev regs`), the D3D
   channel (ch 14) gets a few setup methods, and every thread blocks
   (VMM/APM idle). nvcore.vxd pings ch 14 now and then with NOTIFY+NOP
   on a clip object (status 0xffff, a heartbeat) and our notifier
   answers it -- not the blocker. Verified 2026-10-14: desktop
   resolution and colour-depth changes work, plain DirectDraw works
   (OVLTEST takes 800x600x16; its overlay does not show), but 3DMark
   (D3D) blocks with every thread idle -- the same with DirectX 7
   installed and with the start latch disabled; no register is polled
   (`dev reads N`), the RM only services vblank. Most likely its D3D
   driver waits on something in memory. Not chased further.
   **2026-10-15: the hang is fixed, the 3D still draws nothing.**
   - The hang: D3D's NOTIFY with WRITE_THEN_AWAKEN (class 4a, ch 1).
     We cleared NOTIFY's REQ/STYLE before raising the interrupt; the
     RM's grService (GR.C) only learns the style from PGRAPH_NOTIFY
     (REQ pending -> NotifyAction = STYLE -> notifyMethodComplete), so it
     never woke the waiter. Awaken notifies are now left pending for
     the RM (nv4_pgraph.c nv4_pgraph_do_notify).
   - The desktop shifted 302 lines (start 0x97000 shown from 0): 71.84
     writes PCRTC_START with PCRTC_CONFIG clear. Scan-out now always
     takes the shared start latch (nv4_pcrtc.c); CONFIG only times it.
   - Since then 3DMark 99 runs through (2656-2658 3DMarks on the
     Creative TNT, also on the ULTRA) but every triangle is dropped:
     `dev 3d` "N triangles without a colour surface". 71.84 never
     programs a 3D render target: its D3D_CONTEXT_SURFACES object
     (handle d3d05301, class 0x53, inst 28d1, all-zero instance words)
     only gets SET_CONTEXT_DMA_COLOR/ZETA (0x184/0x188) once, bound on
     subchannel 7; no 0x300-0x310 anywhere (NV4_LOGCLASS=53 from boot,
     on the TNT and the TNT2 Ultra); the RM's context restores load
     buffer 2/3 as zero (`dev regs`); no PIO user writes but PUT. 8.05
     by contrast binds its 0x53 object on subchannel 1 (NV_DD_SURFACES)
     and sends FORMAT 0x103 / CLIP / PITCH / OFFSET_COLOR / OFFSET_ZETA
     per scene. 71.84's subchannel 1 instead holds the 2D surfaces
     (dd003f00: colour/Z clears with 0x5e, a Y32 fence written at
     offset 0) and the software flip object dd006020 (RM writes
     PCRTC_START; flips work). How real NV4/NV5 hardware would know the
     target is open; ideas: the 0x18c surface context loading state we
     do not model, or 71.84's NV4 path really being broken. Early runs
     also crashed (0E, FS=0 in KERNEL32) before the notify fix; the
     later runs did not.
   - New aids: `dev trace N CH`, `dev logclass C` / NV4_LOGCLASS=C,
     `dev ramht HANDLE`, `dev reads` counts the channels' user areas,
     `dev` shows surfaces 0-3, debug_cmd `peekp PHYS [N]`. NVIDIA's
     NV4 D3D/display/RM sources are in the STB archive
     (C:\Users\xeon4\Downloads\stbdata\Archive\recovered\latest\SChanGfx\
     NV4Ver1, NV4Ver2\V4400, NTNV4Ver3; confidential like nvdocs).
   Script pitfall: TaskStop on a background tnt.sh/fullrun.sh leaves
   its children running -- they keep sending keys (one shut Windows
   down mid-test). Run keystroke scripts in the foreground.
5. Windows 2000 / NT4 (B7). **Windows 2000 done (2026-10-14):** VM
   `work-w2k-tnt` (copy of work-w2k: p55tvp4, Pentium 233, ERAZOR II
   PCI). The inbox NVIDIA RIVA TNT driver installs by itself; 1024x768x32
   desktop; text was missing until 7c1a49089 (its driver sets a glyph's
   size/point through GDI block E and sends the bits through block C);
   Start > Restart reboots cleanly; OVLTEST passes all three phases;
   Media Player 6.4 plays an MPEG-1 clip in real time.
   **NT4 done (2026-10-14):** VM `work-nt4-tnt` (copy of work-nt4:
   p6bap, Celeron 233, `nv4_tnt_agp` ERAZOR II AGP, SCSI CD =
   `dl/nt4drv2.iso`: `NT2832`, `NT4345` = NVIDIA 28.32 and 43.45 NT4
   packages from the owner's Downloads, `SP6A` = sp6ai386.exe from
   `G:\Old OS\Windows NT 4.x\...\Service Pack 6`). The guest was SP1;
   NVIDIA's setup needs SP4+ ("Internal error 10" otherwise), so SP6a is
   installed now. 28.32 (setup.exe, pick "NVIDIA RIVA TNT") hung at boot
   until the DMA_CTL fix: `nvHalFifoContextSwitch_NV04` clears
   DMA_CTL.AT_INFO (bit 31) and spins until the chip sets it again (the
   pusher re-reads the push buffer's DMA object) -- now done at once in
   nv4_pfifo.c. Works: 800x600x16 and 1024x768x32 desktops, mode test
   bitmap, mode change without reboot, OpenGL (3D Pipes screensaver
   through nvoglnt.dll, 16 and 32 bpp), Start > Restart through the
   ERAZOR II BIOS. The NVIDIA tab says "TV Encoder: Conexant Bt868"
   although no I2C traffic at all reaches CR3E/CR3F during its start
   (`dev regs`): most likely the panel's default name; not chased.
   2026-10-15: OVLTEST passes all three phases on 28.32 and 43.45;
   43.45 (setup.exe, "NVIDIA RIVA TNT") works: desktop, GL pipes,
   Restart (its nView wizard complains about IE < 5 -- guest issue).
6. Linux (B8, optional): XFree86 `nv`, `rivafb`.

**Hardware not modeled yet**
7. ~~PVIDEO overlay~~ done (ec094b9f0): `nv4_pvideo.c`, NV3's overlay
   (same registers); OVLTEST (tools/nv3/ovltest, OVLTEST.EXE on
   nv4tools.iso via dl/iso-extra) passes all three phases on the TNT
   (4.11, writes the buffer itself) and the ULTRA (8.05, copies with
   0x38). Media Player 6.4 with an MPEG-1 clip (dl/iso-extra/VIDEO/
   TEST.MPG, made with ffmpeg in WSL; on nv4tools.iso): Win98 + 8.05 on
   the ULTRA plays it through the overlay (YUY2, frames copied with
   0x38, `dev` shows the PVIDEO registers), Win2000 inbox too, both in
   real time. **Indeo done (2026-10-14):** VM `work-indeo` (the NV3
   Indeo session's Win98 SE copy: ax59pro, P233 MMX, PT keyboard), now
   `nv4_tnt_agp` ERAZOR II AGP + 8.05 (CD `dl/nv805root.iso` = NV805 at
   the root, so the new-hardware wizard finds NVAML.INF by itself; the
   NV3 cfg is kept as 86box.cfg.nv3). Media Player 6.4 plays the desktop
   clips correctly and in real time, all through the overlay (control
   0x111, buffers flipping; `dev`'s "shown" is the buffer index):
   mcmi5.avi (Indeo 5), open.avi (Indeo 4) -- the two the NV3 needed
   fixes for -- barneysw.avi and keyboard.mpg. Open a clip by type-ahead
   on the desktop (`type mcm`, Enter).
8. Classes (C10): ~~0x38 DVD subpicture~~ done (ec094b9f0; subpicture
   blend untested -- the drivers seen use it transparent), 0x48 DX3
   triangle; 3D into a swizzled surface (A3) when something needs it.
9. PME / TV encoders (C11): only if a BIOS or driver hangs on them.

**Platforms, speed, docs**
10. DOS/VESA (D12): VBE tester, VESA games, an AMI board, ERAZOR II PCI
    on Socket 7. Prepared 2026-10-15, not run: work-dos-tnt (TC430HX AMI
    board + ERAZOR II PCI) and games.iso with VBETEST and DOS Quake.
11. More boards (D13/D14). **Regrouped (2026-10-14, owner's request):**
    like NV3, one device per chip and bus -- `nv4_tnt_pci/agp`,
    `nv5_tnt2_pci/agp`, `nv5_tnt2_ultra_pci/agp` ("nVIDIA RIVA TNT (NV4)
    AGP" ...) -- and the board is the CONFIG_BIOS option `bios` ("Model",
    first in the config so ROM availability works). Its values are the old
    device internal names, so `video_cards_migrate` maps an old `gfxcard =
    nv4_erazor2_agp` etc. to the new device + model, and its new
    `old_name` moves the old "<board> #1" section (memory, threads, GPU)
    over (`config_video_old_section()` in config.c). 18 models, all POST
    (`tools/nv4/postall.sh` on work-post): TNT -- ERAZOR II PCI 2.03.01,
    AGP 2.03.00 (SGRAM) and 2.05.00, Viper V550 1.93E/1.95E5802/1.95C0550,
    Creative 2.04.6.18, Velocity 4400 1.01/1.10, ASUS V3400TNT 2.04.20;
    TNT2 -- ERAZOR III, III Pro, III Pro OEM (SGRAM), III LT (M64),
    Synergy II-32, Viper V770, WinFast S320 II; Ultra -- ERAZOR III ULTRA.
    Left out on purpose: ASUS v3800pro.rom (PCIR says 0029) and
    v3800ultra.rom (PCIR 0028) contradict their names; the ELSA
    Video/Panel variants (E3230500.A2V/APV/AUV/A2P) need a video decoder
    or panel. Still to check: the TNT2's AGP 4x capability.
12. Speed (E15-E17): fixed-point CPU span stepping, GPU kernel variants,
    a capture/replay harness (more useful under the 266 MHz cap).
13. Docs (F20): driver notes like NV3's, a test matrix
    (board x OS x driver x program).

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
   use the NV5 class variants (0x64-0x66) and may move set-context
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
    - NV5 0x64-0x66 (IIFC/IFC/SIFC variants): done in 13ba43f0b, not
      yet run by a driver. 0x63 is an NV10 class. On the TNT (NV4 only)
      0x10/0x11/0x13/0x15 and 0x64-0x67 are the old operation objects
      (clip, blend, ROP, chroma, srccopy...), not modeled: 4.11 never
      makes them.
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
- Owner's STB archive (`C:\Users\xeon4\Downloads\stbdata`,
  `Archive\recovered\latest\SChanGfx\NV4Ver1`, `NV4Ver2\V4400`,
  `NTNV4Ver3`): NVIDIA's NV4 Win9x/NT4 display, DirectDraw/D3D and
  resman sources of 1999. Same rule.
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
  - Master merged into tnt (6d143bdd9), tnt fast-forwarded into master,
    README section for the NVIDIA cards, Release 2.1.4.8 published and
    staged in every rig (AVM-ISDN-RIG kept its own isdn-fritz exe).
  - Quake III works on the TNT2 (OpenGL ICD): 2D pitch clip removed
    (5b120b5de); debug aids speedlog / bounded trace (4c499c8e7).
  - POST slowdown on work-q3 reproduced and profiled (dynarec
    recompiling in the ELSA BIOS's text output); only at 466 MHz, gone
    at 266 on the same board. Owner: no test CPU above a Celeron 266.
- 2026-10-14: tnt merged into master (287ad9076) and released as Release 2.1.4.9.
- 2026-10-15: e6cfa815c (71.84 hang: awaken notify; start latch; debug
  aids). NT4 overlay + 43.45, Win2000 TNT2 + 71.84, Quake III timedemo,
  work-q3 icon cache; games.iso and work-dos-tnt prepared (see "Where
  the 2026-10-15 session stopped").
- 2026-10-15 (later): GLQuake (82.7 fps) and Quake II (38.5 fps) run
  fullscreen on the TNT; GLQuake's "draws nothing" was its own
  minimize-on-focus-loss + shareware 15to8.pal dialog, not the card.
  `dev` shows the clip registers; guest probe winlist.exe.
- 2026-10-15 (later): full Quake data -> fullscreen GLQuake 85.3 fps; README
  table pushed; DOS/VESA: VBETest passes, DOS Quake 22 fps at 640x480 VESA
  (P200). VBE reports 8 MB of 16 (open).
- 2026-10-15 (night): 71.84 D3D on the TNT is a driver limit (its TNT path
  never programs a 3D surface; real TNT2s fail too). Bind/novtx trace aids.
- 2026-10-15 (day): 45.23 on TNT2 -- mode-change deadlock (context switch
  with FIFO access off) and the VIA host bridge's missing capability list
  (VIAGART, so no AGP memory, so no D3D 6/7 HAL) and 8-bpp IFC index
  packing fixed. 3DMark 99 2679 on 45.23. July 1999 and ASUS Dec 1998
  Detonators pass.
- 2026-10-15 (evening): VIA Apollo's always-true VPX tests fixed across
  the board (POST on six VIA boards unchanged, Win98 + 3DMark unchanged);
  `pci` debug command, cpuspeed guest tool.
