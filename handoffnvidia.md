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
  - `le.py` (LE fixup parser), `dis.py`, `xref.py` — capstone helpers.
- Debug command file (src/debug_cmd.c): `key`, `type`, `shot`, `log`, `cpu`,
  `peek lin`, `bp lin [n]`, `bp clear`, `wp lin`, `dev nv3 ...`.
- `dev nv3` (nv3_core.c hook): no args = state dump; `inst N`, `mmio N`, `m2mf N`,
  `d3d N` (triangle trace), `notify N`, `watch lo hi n` (VRAM/RAMIN writes),
  `swm N` (software methods, RAMHT misses/binds, ctx writes + resman MMIO),
  `methods` (last 256 methods).

## State (2026-10-09)
Works: Win98 16bpp desktop at 640–1280, mode switches in both directions
(fixed in 58f4c78c8: CACHE1 ctx 0x32F0 read), DirectDraw (dxdiag all OK),
GetPixel/M2MF readback, DMA pusher, context switches between channels,
dxdiag Direct3D 7 and 8 tests render correctly (37ce5fc63). 3DMark 99 MAX
Game 1 (Race) renders textured with fog and Z (tools/nv3/3dmark.sh starts it);
the whole default benchmark runs through: 1468 3DMarks, 3333 CPU 3DMarks
(800x600x16, Z16, triple buffer). Game 2, fill rate, texture speed and
filtering tests draw plausibly; bump mapping says "not supported" (right).

Open:
1. 3DMark "n Pixel Polygons" tests draw nearly black: the vertices arrive with
   colour 0xff000000 (lighting computed on the guest CPU with "Intel processor
   optimizations"), every triangle is rasterised and written. Check by switching
   3DMark's CPU optimisation to D3D software, or compare on a non-NV3 card; may be
   a CPU-emulation issue, not NV3.
2. Not yet checked against real hardware: texture filtering, mipmaps, Z,
   alpha blending. Fully fogged far geometry shows hard-edged grey shapes in
   3DMark Race (may be right: per-vertex fog 0xff).
3. D3D_CONFIG bits 0-15 are only partly decoded (rnndb: UNK0/UNK4/UNK10/UNK12/
   UNK15). NV3DD32.DLL builds the config word in its render-state code
   (VA 0xB00C5xxx/0xB00DCxxx, image base 0xB00B3000): a per-texture-format word
   OR a global state word at VA 0xB00EC118.
4. 8-px artifact at the right edge at 640/1024 widths.

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
