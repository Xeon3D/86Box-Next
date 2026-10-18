# Alliance ProMotion -- handoff

Read this first; keep it current as the work moves.

## Where things stand (2026-10-18, 275c11c11)

- Branch `alliance`, worktree `F:\Claude\86Box-Next\wt-alliance` (build with `build-static`
  there). **Not merged, not pushed** -- the owner said "don't merge yet". Never PR upstream.
- One file, `src/video/vid_alliance_promotion.c`, three devices on one code path
  (`pm->chip` tells them apart), listed in `vid_table.c` after the Voodoo 3 3000:

  | Device | Chip / PCI ID | BIOS | State |
  |---|---|---|---|
  | `pm6410_pci` | ProMotion 6410, 1142:3210, AT&T 20C498 RAMDAC | PC Partner PM6410 (PhoenixVIEW) | done |
  | `pm6422_pci` | ProMotion 6422, 1142:6422, internal DAC/PLL | same ROM, PCIR patched at init | done |
  | `pm_at3d_pci` | ProMotion AT3D/AT25, 1142:643D | Stingray 128/3D 4.35 (default) / 2.30, GA-RUSH6 | 2D done |

- Works: POST and VESA modes on all three; drawing engine; hardware cursor; DDC lines; video
  window (first design on the 6410/6422, with horizontal interpolation; the second design --
  two windows and tiles -- on the AT3D); Win95 OSR2, Win98 SE and
  NT 4 with Alliance's drivers (accelerated, full mode lists, live mode switching);
  DirectDraw (DxDiag's three tests) on the 6422 with Alliance's 1600 build and STB's build,
  and on the AT3D with Alliance's 2072 driver; the AT3D under NT 4 (4.00.1005 and 1.410).
- Before merging (owner's call): the ROMs into the Xeon3D/roms fork (they are only in
  ROMS-EXTRA now), a README / CLAUDE.md "Fork additions" entry. Logging is already off by
  default (`pm_do_log = 0`).

## Open items, in order of value

1. The AT3D's second video window design is in and verified (see below), both windows.
   Not modelled: the filter bit (XR82/92 bit 12, VC2_FILTER). Alliance's driver sets it
   with every stretch, X.org never; the AT3D manual mentions a "downscaling filter", but
   the driver refuses to shrink an overlay (UpdateOverlay returns 80070057), so there is
   nothing to compare against. Left as is.
2. 24 bpp: AT3D engine depth 4 is in; no BIOS or Windows mode uses 24 bpp, so untested.
3. VC_NOFLICKER (XR82 bit 11, set by STB's build) is not modelled (a TV flicker filter).
4. Not planned: the AT3D's 3D engine (its driver offers Direct3D; DxDiag says Enabled, but
   nothing would draw), Voodoo Rush boards (AT25 + 3Dfx).
5. Interlace (XRD2 bit 0, the drivers' 1024x768 "43 Hz" mode) is shown progressive at the
   frame rate the CRTC holds -- same picture; left as is.

Done 2026-10-18: AT3D DirectDraw (92bc9574b), AT3D NT 4 (5562a576a), STB build's
DirectDraw (packaging only, see below), video window interpolation (c9f9af681).

## Test VMs and tools

All in `F:\Claude\86Box-Next\`. Each has its own `86Box-Next.exe`, `86box.cfg` and `romsx\`
(the Alliance ROMs, passed with `-R`). Shut Windows guests down from Start before stopping
(a hard stop means Safe Mode / ScanDisk next boot); `cycle.sh` does that for you.

| VM | Guest | Card now | Notes |
|---|---|---|---|
| work-pmdos | DOS 6.22, tc430hx, P200 | `pm_at3d_pci` (4.35) | VBETEST.EXE + UNIVBE in C:\ |
| work-pmw98 | Win98 SE, p55tvp4, P55C 233 | `pm6422_pci` | STB source build installed with STBALL16/32.DLL (1600 files in C:\PMBAK); C:\GDITEST.EXE; CD video2.iso |
| work-pmat3d | Win98 SE (copy of work-pmw98) | `pm_at3d_pci` (4.35) | Alliance AT3D/AT25 driver 2072 from at3ddrv.iso; 1024x768x16 now |
| work-pmw95 | Win95 OSR2, p54tp4xe | `pm6422_pci` | 1600 driver from pmdrv.iso |
| work-pmnt4 | NT 4 WS, p6bap, Celeron 233, SCSI | `pm_at3d_pci` (4.35) | AT3D NT driver 4.00.1005 (at3dnt.iso), 1024x768x16; Administrator, blank password; US keyboard |

Scripts (`tools/alliance/`, `PM_DIR=/f/Claude/86Box-Next/<vm>` selects the VM, default
work-pmdos):
- `pm.sh start|cmd "<debug cmds>"|shot NAME|stop|running` -- starts minimized, no focus
  steal; shots land in `<vm>\shots\NAME.png`.
- `vbe.sh "0 3 2 1" NAME` -- boots DOS, runs VBETest, types the menu choices, screenshots.
  (0 = interactive, then colour list 1/3/4/5 = 8/16/24/32 bpp, then the mode, then
  0 banked / 1 linear where the BIOS offers both.)
- `cycle.sh [SHOT] [WAIT]` -- Win9x: clean shutdown, copy the fresh exe, boot, screenshot.
- `ntcycle.sh` -- the same for NT 4, logs on.
- `gditest/gditest.c` -- no-CRT Win32 GDI test picture (polygons, star, ellipses, hatches,
  line fan); build in WSL with the command in its header comment (`-nostdlib`, entry
  `_entry@0`: Win98's msvcrt lacks what mingw-w64's start-up needs).
  The AT3D and the 6422 draw it identically.

Finding a Win9x driver's code (debug_cmd, added 2026-10-18): `findp <hex bytes>` searches
RAM for code bytes from the file (avoid bytes with relocations), `bpp <phys> [n]` breaks
there (logs CS and registers), `sel <selector>` gives a selector's base (the driver's DS is
`ds` in a bp line; its far pointers are DS-relative). scratchpad ne.py/segdis.py/segs.py/
xref.py disassemble NE segments and find immediates.

Debug command file: `dev` dumps SR/CR/XR, CRTC, pixel clock/refresh, cursor; `dev log N`:
1 = extended register writes and every engine command as `PM: BLT ctrl rop src dst dim fg
bg clip`, 2 = every register access. Off by default.

Guest keyboard on the Win98 VMs is Portuguese: debug `type` mangles `/ : \ .` -- use
scancodes: `:` = `2a,34,b4,aa`, `\` = `29,a9`, `.` = `34,b4`, space `39,b9`. Run dialog:
Ctrl+Esc (`1d,01,81,9d`), R (`13,93`). Display Properties: `control desk.cpl`.
Mouse: `mouse 0 5 0` steps of 5 move ~2 px; bigger steps accelerate.

## Material

- `F:\Claude\86Box-Next\alliance-stb\` (its README lists everything): Alliance's Win95 driver
  source as STB built it (`driver-source/DRIVER/DIB/GXX/physical.h|inc` = register map,
  `physsetm.c` mode set, `physblt/physline/physcurs/physcach/physsl.asm` = engine use,
  `VDD/promtn.asm`); built drivers; STB install sets; 6422 datasheet + brief (no register
  tables); Win95 6422 driver (win95642.exe) and NT 4 driver 1.410 (nt40-1410.exe), LHA SFX.
- X.org apm driver (MIT): `F:\Claude\86Box-Next\dl\xf86-video-apm\src` -- `apm_regs.h`
  (engine bits), `apm_driver.c` (mode init, clocks, AT3D aperture), `apm_funcs.c`,
  `apm_cursor.c`, `apm_video.c`. Older accel (AT24 pattern/stretch): git history, d9a3071^.
- AT3D drivers and manual: `G:\Old OS\Drivers\video\alliance\` (at24, at3d, AT3D-AT25 zips).
- ROMs in `F:\Claude\86Box-Next\ROMS-EXTRA\video\alliance\`: `pcpartner_pm6410.bin`;
  `at3d\hercules_stingray128_3d_v4.35.00-b11.bin`, `..._v2.30.00-b27.bin`,
  `gigabyte_ga-rush6_v4.350010.bin` (the first 32 KB of each dump).

## Hardware model -- what the BIOSes and drivers taught

### Registers
- Extended SRs: SR10 = 12h unlocks (not modelled). The 6410 has SR10-SR1C only; SR20-2F read
  the XRF0 scratch there (so the BIOS's "SR23 writable -> 6410 rev 3" test fails as it
  should). 6412+: SR1D-SR2F real. The AT3D decodes only six sequencer index bits (its BIOS
  writes SR1C as index DCh).
- SR1A = VL aperture base. SR1B bits 2:0 = 1 maps the XR block at A0000h (2 KB mirrored);
  SR1B bit 2 maps it in the last 2 KB of the aperture, bits 5:3 the blit port below it.
  SR1C bit 2 = flat model (the extended, packed display), bit 3 = VGA memory off, bit 4 =
  packed window. SR1D/1E/1F = XR I/O window (dword index, base port) on the 6422+.
- SR20 = memory size (64 KB units), SR21 = DAC type (81h 20C498, 85h internal).
- CR1A: vertical bit 10s (total, display end, blank, retrace; bit 4 line compare). CR1B:
  horizontal bit 8s. CR1C: start address bits 19:16 (3:0), offset bits 11:8 (7:4).
  Extended modes: start in dwords, offset in 8-byte units, CRTC counts pixels.
- XR80 bits 3:0 pixel format (2 = 8, C = 15, D = 16, E = 24, F = 32 bpp). XRC0 (word) bank in
  4 KB units, applied to A0000h and B8000h. XRC4/C5 memory config. XRD0 DDC (SCL/SDA bits 5/6,
  read in XR1FC 17/16). XRD8 6410 clock index; XREC 6422+ PLL: f = 14.318 MHz * (N+1) /
  ((M+1) * 2^L), N 23:16, M 15:8, L 3:2. XR140-14F cursor. XR180-183 read the PCI IDs (the NT
  miniport and Win9x driver insist). XR190-193 read BAR 0 (aperture), XR194/195 BAR 1 (I/O).
- AT3D aperture (16 MB): VRAM at 0 (blit port over 3F8000h-3FFFFFh), full VRAM again at
  +8 MB, XR at +FFEC00h, VGA ports at +FFF000h + port.

### Memory sizing (each BIOS differs)
- 6410: XRC4 bit 0 (2 MB organisation) + bank 10h, 55AAh at B800:4, FFFFh at B800:14. Needs
  GR8 = FFh at power-on and the bank applied to B8000h.
- 6422: XRC5 bit 0 + SR1C bit 4; a 1 MB board reads the upper half of each 64-bit word as
  empty (modelled as address bit 2).
- AT3D (BIOS 2B96h): bank 0 then bank 2 MB (XRC0/C1 = 0200h), dword tests at B800:10/14;
  the bank is not wrapped to the fitted memory (a 2 MB board's upper bank is empty).

### Drawing engine (comment block above `pm_blt_start()`)
- XR40 command: op 3:0 (1 blit, 2 rect, 4 strip, 8/9 host blits, C/D lines), 6/7 X/Y
  negative, 8 Y major, 9/11 source linear/contiguous, 12 mono source, 13 transparency,
  18/19 destination linear/contiguous, 23:22 pattern, 26:24 screen-width model, 28:27
  destination update, 30:29 quick start, 31 start. ROP3 in XR46.
- Starts run at the end of the whole register access (a dword write to XR58 is width +
  height). Quick start fires on the low (X) word of its register only. Arming a quick start
  whose register was written since the last start starts it (Alliance's polygon re-arm).
- Destination update 28:27 leaves X+W / Y+H for the next span (STB's physsl.asm).
- AT24+ (AT3D): depth field 16:14 (1/2/3/4 = 8/16/32/24 bpp), 8x8 mono/colour pattern unit,
  XR42 bit 0 kept. Below the AT3D, XR42 bit 0 and the tile registers XR200-2BF ignore writes
  (the drivers detect the generation that way).
- XR40 bit 17 (AT3D Windows driver, not in X.org): linear addresses are plain byte addresses
  and rectangle strides are XR5C (destination) / XR5E (source) in bytes. Without it, linear
  addresses are X.org's split pixel format (bits 23:12 in 31:16) and XR5C is extra bytes.
- Not on these chips: stretch blit (op 3). Cursor: 64x64, AND<<1|XOR, XR141/142 colours,
  shown only in flat-model modes.

### Display
- 6410 clocks: XRD8 index into the BIOS's table (from the 6422 PLL table at 6775h), halved
  above 8 bpp through the 20C498's 16-bit port.
- Extended modes use `hoverride` (the AT3D's 1600x1200 blanking is longer than a 6-bit end
  compare holds); line compare bit 10 is `split += 400h` (the core already added 1).
- Video window V1: XR82-XR9F, UYVY/YUV->RGB, stretch, flips through XR90; no colour key.
  Verified with Media Player MPEG-1 on the 6422 (work-pmw98\video2.iso).

## AT3D DirectDraw and NT 4 (2026-10-18)

- XR1F0 bit 30 (read as XR1F3 bit 6 after the driver writes XRDB = 4) = no Voodoo Rush.
  Clear, the 2072 driver gives DirectDraw VG96DD32.DLL (the Rush's, not installed) as its
  32-bit driver, DriverInit never runs, DDHAL_SetInfo fails: "DirectDraw not available".
- SR1B bits 5:3 = 3 put the blit port at B8000h (32 KB): the 2072 driver sends glyphs there
  once DirectDraw owns off-screen memory (host mono blit 24003A08, contiguous rows = byte
  padded). 4 = end of the first 4 MB; bits 2:0 = 4 the registers in its last 2 KB (the NT
  1.410 miniport polls status there -- it hung at logon before).
- Start address: the SVGA core shifts memaddr_latch left by 2, so the extended start is in
  dwords. It was in bytes, so every nonzero start (DirectDraw flips, VBETest scrolling)
  showed 4x too far, on all three chips.
- STB's build asks DirectDraw for STBALL32.DLL (and loads STBALL16.DLL): its build.bat copies
  promdd32.dll/promdd16.dll to those names. With them in C:\WINDOWS\SYSTEM its DirectDraw
  passes too (it was never an emulation bug).

## Second video window design (AT3D, 2026-10-18, 275c11c11)

- Two windows: XR82 and XR92 control (format 3 = YUY2 here -- Alliance's 2072 driver; X.org
  writes 6 for YUY2; bit 8 YUV->RGB, 9 stretch, 10/11 horizontal/vertical interpolation,
  12 filter), +2 pitch in dwords, +4/+8 scale (source/destination x 4096, 0 = 1:1), +6/+0Ah
  phase. The scale accumulator runs over screen positions from the phase: vertically
  phase + scale * y, horizontally phase + scale * (x - 1) (the driver's phase uses
  left - 1 and its tile addresses take the ceiling; only that way do tiles starting
  mid-window line up -- fixed in 0e310f205).
- Tiles at XR200-2BF, 16 bytes (STB physical.h TILEREGS): control (bits 1:0 window, 0 = none;
  10h = last tile of a band), left, right (exclusive), bottom (the band's last line,
  inclusive), source width, 24-bit source address (the tile's top-left pixel). A band starts
  on the line below the previous band's bottom; control 0 ends the list (the 2072 driver
  clears the unused tiles; X.org also writes the length to XR8E). The 2072 driver writes a
  dummy band (window 0, left FFFh) down to the line above the video, then one tile per band,
  and flips by rewriting the tile's address (triple-buffered).
- XR1FC bit 11 = vertical retrace (X.org waits on it).
- Verified: Media Player 6.4 + test.mpg (work-pmat3d\video2.iso) at 1024x768x16, 1:1 and
  maximised (stretched). The 6422 (first design) unchanged.
- Covered video: tools/alliance/ovtest (C:\OVTEST.EXE on work-pmat3d) shows a YUY2 overlay
  with a clipper and calls UpdateOverlay five times a second; the driver builds tiles from
  the clip list. Its window opens where the Start menu covers its lower left; Calculator
  moved over its middle gives three bands with two tiles in one -- all correct after
  40e9d3f96 (packed YUV pairs by absolute address: a tile can start on an odd pixel).
  Media Player itself drops the overlay when covered and draws frames in software.
- Window 2: the driver offers two overlays with NumberOfOverlays = "2" (a string) in
  HKLM\SOFTWARE\Alliance Semiconductor\Driver\Miscellaneous (C:\OVL2.REG on work-pmat3d,
  already imported). "OVTEST 400 300 60 120" and "OVTEST 400 300 300 260" (client size,
  position) overlap; both stacking orders are right (0e310f205).

## History of the hard bugs (so they are not re-chased)

- Win98 offered only 640x480x8: XR180-183 must read the PCI IDs (also the NT miniport).
- STB build's text missing pixels: quick start fired on byte 59h before the height.
- Polygon gaps: destination update bits + quick-start re-arm.
- AT3D reported 1 MB: the BIOS's SR index DCh was dropped, leaving VGA memory off.
- AT3D Start menu garbage: XR40 bit 17 byte addressing with XR5C/XR5E strides.
- AT3D stray polygon lines: quick start fired on the Y write (XR56).
- Win98 VMs in Safe Mode: always shut down from Start first.
