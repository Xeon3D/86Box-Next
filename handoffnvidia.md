# Handoff: NVIDIA RIVA 128 (NV3) emulation

Living document — whoever works on the NV3 updates it as they go.
Last updated: 2026-10-09.

## Where the work is
- Branch `nv3`, worktree `F:\Claude\86Box-Next\wt-nv3` (keep using that worktree;
  the main checkout is shared with other work).
- Code: `src/video/nv/nv3/` (core, subsystems/, classes/, render/), header
  `src/include/86box/nv/vid_nv3.h`.
- **Read `src/video/nv/nv3/NV3-DRIVER-NOTES.md` first**: what the Win9x drivers
  (NV3RM.VXD resman, NV3DISP.DRV, NV3DD32.DLL) expect from the hardware.
- References: envytools (rnndb/graph/nv3_pgraph.xml, nvhw/, hwtest/), the
  datasheet `F:\86box FTP\Specs\NVIDIA RIVA 128.pdf`, Linux rivafb.
- Driver binaries can be extracted from the rig image with 7-Zip (only when no
  VM has it open): `7z e <img> WINDOWS/SYSTEM/NV3RM.VXD` etc.

## Rules (from the owner)
- Never open PRs/issues against 86Box/86Box.
- Don't drive the host GUI or steal focus: test VMs start minimized without
  activation (tools/nv3/launch-noactivate.ps1) and are driven through the debug
  command file (BOX86NEXT_DEBUG_CMD).
- Never touch the owner's rigs' cfg/nvr/images/roms. Only replace the exe in
  `F:\Claude\86Box-Next\Latest\NVIDIA-RIG`, and only when no VM runs from there.
  Test on a private copy of the rig (see below).
- Commit from PowerShell (not the MSYS2 shell). Build from Bash with
  `/c/msys64/ucrt64/bin` first on PATH: `cmake --build build-static -j16`.
- Win98 boots in ~25 s; take screenshots every few seconds, don't sleep long.

## Testing
- Private rig: a copy of NVIDIA-RIG (cfg, nvr, roms, the Win98 image) in a work
  dir, e.g. `$NV3_WORK/rig/`. Put the stripped build there:
  `cp build-static/src/86Box-Next.exe $NV3_WORK/rig/ && strip ...`.
- `tools/nv3/` (set `NV3_WORK` to the work dir, POSIX path; `NV3_WORK_WIN` not needed):
  - `nvtest.sh start|stop|cmd "<line>"...|shot NAME` — start the VM minimized
    with the debug command file, send commands, screenshot to $NV3_WORK/t/NAME.png.
    After boot send `key 1c,9c` (Enter) for the network login.
  - `res.sh left|right N PREFIX` — Display Properties, move the screen-area
    slider N steps, apply, confirm.
  - `dxdiag.sh dd|d3d PREFIX` — dxdiag Display tab, start the DirectDraw or
    Direct3D test.
  - `rmbp.sh FILEOFF...` — breakpoints at NV3RM.VXD file offsets (needs this
    boot's load delta: it reads the first logged `cache1 ctx[0]` EIP, which is
    only logged with `dev nv3 swm N` armed — arm it right after start, or
    calibrate by hand: see the notes file).
  - `le.py` (LE fixup parser), `pedis.py` (disassemble a PE at a VA), `xref.py` (search NV3DD32 code) — capstone helpers; capstone is in the user Python (`~/AppData/Local/Python/bin/python`), not MSYS2's.
- tools/nv3/ovltest: OVLTEST.EXE, a DirectDraw overlay test (build.sh: WSL's i686 MinGW, no CRT
  -- MinGW's CRT has CMOV, which the Pentium MMX rig faults on -- onto an ISO; point the private
  rig's cdrom_01_image_path at it; run.sh starts it and takes a shot per phase). It logs to
  C:\OVLTEST.TXT in the guest (7z e the image after stopping the VM).
- `glsaver.sh NAME` runs Win98's OpenGL screensaver "3D NAME" (Pipes, Maze, Text, Flower Box,
  Flying Objects) through NVIDIA's OpenGL ICD (NV3OGL.DLL); needs a 16 bpp desktop.
- Debug command file (src/debug_cmd.c): `key`, `type`, `shot`, `log`, `cpu`,
  `peek lin`, `bp lin [n]`, `bp clear`, `wp lin`, `interp 1|0` (force the interpreter), `dev nv3 ...`.
- `dev nv3` (nv3_core.c hook): no args = state dump; `inst N`, `mmio N`, `m2mf N`,
  `d3d N` (triangle trace), `tex` (mip levels of the next mipmapped texture), `notify N`, `watch lo hi n` (VRAM/RAMIN writes),
  `swm N` (software methods, RAMHT misses/binds, ctx writes + resman MMIO),
  `methods` (last 256 methods).

## State (2026-10-09)
Works: Win98 16bpp desktop at 640–1280, mode switches in both directions
(fixed in 58f4c78c8: CACHE1 ctx 0x32F0 read), DirectDraw (dxdiag all OK),
GetPixel/M2MF readback, DMA pusher, context switches between channels,
dxdiag Direct3D 7 and 8 tests render correctly (37ce5fc63). 3DMark 99 MAX
Game 1 (Race) renders textured with fog and Z (tools/nv3/3dmark.sh starts it);
the whole default benchmark runs through: 1468 3DMarks, 3333 CPU 3DMarks
(800x600x16, Z16, triple buffer). Game 2, fill rate, texture speed,
filtering and n Pixel Polygons tests draw plausibly; bump mapping says "not supported" (right).
DirectDraw video overlay (PVIDEO): YUY2/UYVY, scaled, colour-keyed, buffer handoff with
interrupts -- tools/nv3/ovltest (a Win98 test program, see below) shows it.

Open:
1. Not yet checked against real hardware: filtering details (bilinear is
   config bits 1:0 = 2; FILTER 0x30C = SPREAD_X 4:0, SPREAD_Y 12:8, SIZE_ADJUST
   23:16 per rnndb nv3_3d.xml, unused: NV3DD32 sends 0x80EC0000 everywhere),
   alpha blending. The 3DMark filtering tunnel shows hatching at mid distance:
   its second pass (Z EQUAL) is finer geometry than the Z-writing pass and the
   app's own vertex z differs by 1-3 zeta units (checked from full-precision
   vertex dumps), so EQUAL fails in a pattern; likely on real hardware too.
2. D3D_CONFIG bits 0-15: rnndb graph/nv3_3d.xml (not nv3_pgraph.xml) names them:
   INTERPOLATOR 3:0 (ZOH corner / ZOH centre / FOH), WRAP_U 5:4, WRAP_V 7:6,
   SOURCE_COLOR 11:8 (1 normal, 2 colour inverse, 3 alpha inverse, 6 alpha one),
   CULLING 14:12, Z_PERSPECTIVE 15. Unknown: what 11:10 = 3 (NV3DD32's
   MODULATE, alpha from the texture) really means; ZOH corner vs centre; how
   the w buffer (bit 15) is stored -- untested, no app seen using it.
3. Right edge: the dumped buffer has stray columns just past the active width
   (800x600: column 800 black, 801-807 content; 1024: 1025-1027). They are in the
   overscan border, which is cropped unless Show overscan is on (off in the rig),
   so not visible normally. Low priority; the svga core's overscan maths with
   override = 0 (nv3_recalc_timings) is where to look.

## Log
- 2026-10-09: started 3D. The D3D7 test drew black: (a) the DMA pusher added the
  push buffer DMA object's adjust (0x80) to DMA_GET; the resman already folds it
  in, so it parsed from inside a command (runouts with vertex floats on methods
  0x04..0x70, `dev nv3 push N` shows headers); (b) the vertex fog byte is the
  amount of fog (0 = none), not D3D's 255 = none. Both fixed; cube renders.
  Flips are CRTC start-address writes (CR0C/0D/CR19), already working.
- 2026-10-09: 3DMark 99 Race drew opaque geometry black: vertex alpha 0 with
  config "src x srcalpha, dst x 0" (blending off). With config bits 11:10 = 3
  the alpha is the texture's (R5G6B5 = 1), not texture x vertex. Fog byte =
  amount of fog (0 none): 3DMark sends 0xff only for far geometry (z~1).
  The guest keyboard is Portuguese: tools/nv3/3dmark.sh shows the scancodes
  for '"', ':' and '\'.
- 2026-10-09: the "n Pixel Polygons" tests are not broken: each frame is a fixed
  number of triangles (6 push buffer batches of 2412 at y 5/116/230/341/455/566 for
  the small sizes), so the small-polygon frames are mostly black bands and the large
  ones fill the screen with the lit cloud texture. Same with the interpreter
  (`interp 1`, new debug command) and with fpu_softfloat. tools/nv3/3dmark-tests.sh
  (then 3dmark-npoly.sh) selects and runs only some tests. The driver double-buffers its push buffer:
  GET 0x180 / 0x40180, ~0x3FA00 bytes each.
- 2026-10-09: depth was inverted for triangles: zeta = (1 - z) * 65535 (after
  envytools' convert_z) with the compare a <= b (new, current) let the farther
  surface win -- 3DMark Race drew its sky over the track, the z = 0 title text
  never passed. Now zeta = z * 65535 (0 = near); Race and Game 2 render right
  (the earlier "correct depth ordering" note was wrong). Also: per-pixel mip
  level from the texture-coordinate derivatives (was one level per triangle),
  barycentrics in double. tools/nv3/3dmark-tests.sh SC... runs chosen tests
  (Alt-letter scancodes listed in it); `dev nv3 tex` dumps mip levels.
- 2026-10-09: full default 3DMark 99 run after the zeta fix: 1465 3DMarks, 2093
  CPU 3DMarks (CPU score tracks host load). The private rig copy's desktop is
  1024x768x8 now (my resolution tests), so 3DMark's windowed loading screen is
  posterised there -- expected at 256 colours.
- 2026-10-09: NV3DD32.DLL state upload mapped (NV3-DRIVER-NOTES.md, "Direct3D
  driver"). Point vs bilinear is CONFIG bits 1:0 (0 / 2), confirmed in the
  3DMark filtering subtests; FILTER stays 0x80EC0000. tools/nv3/dis.py renamed
  pedis.py (it shadowed the stdlib dis that capstone imports).
- 2026-10-09: video overlay implemented (nv3_pvideo.c): registers mapped from NV3RM's code
  (NV3-DRIVER-NOTES.md "Video overlay"), scanout through the svga overlay hook (4:2:2,
  bilinear, BT.601, destination colour key), buffers taken at vblank (taking them at once
  made the resman's handler re-submit and re-interrupt without end). Checked with
  tools/nv3/ovltest: scaled, 1:1 and colour-keyed phases all right. Not yet: YV12/YVU9
  planar FOURCCs (the driver may convert them), interlaced (bob) fields, 8/32 bpp desktops.
- 2026-10-09: the 2 MB option (NEC G7AGK) no longer fatal()s: BAR1 laid out as for 4 MB with a
  2 MB framebuffer and mirror, PFB_BOOT reports 2 MB. Win98: dxdiag shows 2.0 MB, DirectDraw
  tests pass, desktop 1024x768x8. To test: add `[nVIDIA RIVA 128 (NV3) PCI]` /
  `vram_size = 2097152` to the private rig's cfg.
- 2026-10-09: OpenGL: NV3OGL.DLL (the ICD) renders all five 3D screensavers in hardware
  (class 0x17; untextured geometry uses a 4x4 white X1R5G5B5 texture with CONFIG 11:8 = 0xC,
  so the texture-alpha rule keeps it opaque). The private rig's desktop is now 1024x768x16.
- 2026-10-09: 1024x768x32 desktop works (2D, Start menu); OpenGL there falls back to
  Microsoft's software renderer (no class 0x17 methods, frames blitted with class 0x11) and
  draws right. The RIVA 128 ZX variants (nv3t_pci/agp) cannot be tried: none of their BIOS
  ROMs (nv3t182b.rom, A170D03T.rom, vgasgram.rom, "BIOS_49_Riva 128") is on this machine.
- 2026-10-09: Release 2 published (tag release-2 = d7a1479f0, the pre-ZX code plus
  doc/nvidia-riva128.md, a human-oriented write-up of the whole emulation); built in a clean
  worktree F:\Claude\86Box-Next\wt-rel2, zip in RELEASES-ARCHIVE, staged in every rig.
- 2026-10-09: RIVA 128 ZX. The owner's ZX ROMs (7, all PCI
  12D2:0018 rev C) were copied into NVIDIA-RIG's and the private rig's roms/video/nvidia/nv3 and
  added as BIOS choices; nv3t_pci/nv3_agp/nv3t_agp were missing from vid_table.c (added).
  Win98 installs the driver, then every bind misses RAMHT and the driver never instantiates its
  objects (black screen, then safe mode). Fixed on the way: RAMIN BAR1 addresses masked to the
  4 MB window (8 MB cards kept bit 22). Next lead: on rev C the resman's cache-error handler
  reads CACHE1 entries at 0x3400+GET*2 (64-entry cache) -- check the emulator serves them there.
  Temporary "TEMP" logs are still in nv3_core.c, nv3_core_arbiter.c and nv3_pramin.c.
- 2026-10-09: Windows 2000 (Latest\Windows 2000 rig, nv3_pci, default V330 BIOS) booted to a black
  screen with only the hardware cursor: its miniport writes the sequencer index with a 32-bit
  store to PRMVIO 0x0C03C4, and bytes 2-3 fell through to the DAC pixel mask (3C6 = 0). Each
  VGA window (PRMVIO / PRMCIO / PRMDIO) now decodes only its own ports (envytools nv_vga.xml);
  Win2000 reaches its desktop, Win98 still does. The class 0x1C method 0x304 trap it logs twice
  at startup is a software method the NT driver handles. `dev nv3` now prints the DAC mask and
  a few palette entries.
- 2026-10-09: RIVA 128 ZX reaches the Win98 desktop (commit 8ae6d033f): CACHE1 entries at
  0x3400-0x35FF on rev C (patch from another session), and rev C's PUT/GET as 7-bit Gray
  pointers over 64 entries (the resman's own stepping, see the notes file); the pointer fields
  were uint8_t and dropped the wrap bit (GET 0x180 -> 0x80, the ring stalled after 64 methods).
  Tested in the private rig (nv3t_pci, STB V128ZX BIOS): desktop at 640x480x8, 1024x768x8 and
  x16, dxdiag D3D7 and D3D8 cubes. After a failed boot Windows comes back in 16 colours (VGA);
  set 256 colours and restart. Not tried yet: 32 bpp, DirectDraw test, 3DMark, OpenGL, AGP.
- 2026-10-09: Windows 2000 (private copy of Latest\Windows 2000; drop `uuid` from a copied cfg,
  else the "moved or copied" dialog blocks the start). The RIVA 128 ZX (nv3t_pci, STB BIOS ->
  "Velocity 128" in the inbox INF) reaches the desktop at 1024x768x32 accelerated, dxdiag
  DirectDraw: all tests successful. Direct3D in software on both cards = the driver: the inbox
  nv3.dll 3.43 has no D3D HAL at all (notes file). A restart from Windows 2000 hangs in the
  p55tvp4 BIOS at F000:E5A3 (keyboard controller wait loop) -- with the Cirrus GD5446 too, so
  not NV3; a cold start (stop/start the emulator) is fine.
- 2026-10-09: Release 2.1 published (tag release-2.1 = 01c2ab5f9: the RIVA 128 ZX, the AGP entries, the
  Windows 2000 fixes); zip in RELEASES-ARCHIVE, staged in every rig.
