# Alliance ProMotion -- living handoff

Branch `alliance`, worktree `F:\Claude\86Box-Next\wt-alliance` (build-static there).
Device: `src/video/vid_alliance_promotion.c`: `pm6410_pci` ("Alliance ProMotion 6410 PCI") and
`pm6422_pci` ("Alliance ProMotion 6422 PCI"), one code path, `pm->chip` tells them apart.
Keep this file updated as the work moves.

## Material

- `F:\Claude\86Box-Next\alliance-stb\` (README.md there lists it all):
  - Alliance's ProMotion Win95 display driver source as STB built it (Horizon 64 Video,
    `driver-source/DRIVER/DIB/GXX/physical.h|inc` = register map; `physsetm.c` mode set;
    `physblt.asm`, `physline.asm`, `physcurs.asm` = blitter/line/cursor use; `VDD/promtn.asm`).
  - Built drivers (`deleted-built-drivers/`), STB install sets (`install-packages/`).
  - ROMs: `rom/PCPartner-6410.BIN` (PM6410, PCI 1142:3210, 32 KB image in 64 KB);
    `from-laptop/` Stingray 128/3D 2.30 and 4.35 (AT3D/AT25, 1142:643D), GA-RUSH6 (AT25).
  - 6422 datasheet + brief (no register tables).
  - Win95 6422 driver (win95642.exe), NT 4 driver 1.410 (nt40-1410.exe), LHA SFX.
- X.org apm driver (MIT): `F:\Claude\86Box-Next\dl\xf86-video-apm\src` -- 6422/AT24/AT3D register
  use: `apm_regs.h` (drawing engine DEC bits, status), `apm_driver.c` (mode init, clocks
  comp_lmn, memory sizing), `apm_cursor.c`, `apm_funcs.c` (accel), `apm_video.c` (overlay).
- ROM in `F:\Claude\86Box-Next\ROMS-EXTRA\video\alliance\pcpartner_pm6410.bin`
  (not in the Xeon3D/roms fork yet).

## Test VM

`F:\Claude\86Box-Next\work-pmdos` (copy of work-p2dos: tc430hx, Pentium 200, DOS 6.22,
VBETEST.EXE + UNIVBE in C:\). `tools/alliance/pm.sh start|cmd|shot|stop` (PM_DIR selects the VM),
`tools/alliance/vbe.sh "0 1 3" NAME` boots, runs VBETest and picks menu entries.
Debug: `dev` dumps SR/CR/XR state, `dev log N` sets the log level (1 = extended register
writes, 2 = everything). Logging is ON by default while bringing the card up -- turn the
default (`pm_do_log`) to 0 before merging.

## What the BIOS (PM6410, PhoenixVIEW) taught

- It is a unified ProMotion BIOS: chip ID by writable SRs. SR1D writable -> 6412+ (then XR
  scratch via the I/O window SR1D/SR1E/SR1F decides 6422); SR23 writable -> 6410 rev 3. The
  first 6410s implement SR10-SR1C only. INT 10h AX=3006h returns AX=chip, BX='SA', DX=rev.
- Extended registers ("XR", the COP block of physical.h): SR1B bits 2:0 = 1 maps them at
  A0000h (offset = address & 7FFh here, 2 KB mirrored); SR1B bit 2 maps them in the last 2 KB
  of the linear aperture (blit port in the 30 KB below). SR1C bit 3 = VGA memory off.
  SR1A = aperture base bits 27:20 (VL). SR10 = 12h unlocks (lock not modelled).
- Scratch: XRF0 = memory size (64 KB units), XRF1 = DAC type (81h = AT&T 16-bit, the 20C498;
  85h = internal, 6422). The BIOS READS them as SR20/SR21 on every chip; on a rev-0 6410 those
  SR reads return XRF0+ (writes to SR20+ do nothing, so the SR23 test fails as it should).
- Memory sizing: XRC4 bit 0 (2 MB organisation) + bank XRC0 = 10h, write 55AAh at B800:4 and
  FFFFh at B800:14, read back. Needs GR8 = FFh at power-on and the bank applied to B8000h.
- Banking: XRC0 (word) in 4 KB units, applied to A0000h and B8000h windows.
- XR80 bits 3:0 pixel format (2 = 8 bpp, Ch 15, Dh 16, Eh 24, Fh 32); bit 6 set by VBE modes
  only. XR80 = 02h in mode 13h too: the extended (packed, one pixel a dot clock) display is
  SR1C bit 2 ("flat model": 05h BIOS modes, 25h Windows driver, 00h VGA modes).
  XRC6 DAC control (03h in 16 bpp, 0Ah/09h variants for double index), XRD8 clock select
  (00h at 640x480x8, 04h at 640x480x16, 03h at 800x600x16), XRD4-D6 FIFO thresholds.
- XR190: the BIOS writes a VL-bus aperture address it picks (07C00000h); the Windows driver
  reads XR190 as the aperture base on PCI -> XR190-193 read back PCI BAR 0.
- Extended modes: CRTC timings count pixels (16 bpp modes have the same CRTC as 8 bpp);
  start address in dwords (+ CR1C 3:0), offset in 8-byte units (+ CR1C 7:4).
- VBE 1.2, no LFB reported. Modes seen: 640x400/480, 800x600, 1024x768 x8; 640x480,
  800x600 x16 (2 MB). VBETest bank switching passes for 640x480x8/16, 800x600x16, 1024x768x8.

## Drawing engine and cursor (done, from STB's physblt/physline/physcurs + apm_regs.h)

See the comment block above pm_blt_start(). Ops used by the Windows driver: 81000002 (solid
fill), 81000402 (mono 8x8 pattern fill, pattern = source), 81001A01/81003A01 (mono expansion
from off-screen video memory, linear+contiguous, opaque/transparent: text), 81000041/01
(screen copy), 81000008 (colour host blit through the blit port). Lines (C/D) and screen-to-
host (9) are implemented from the driver source but not yet seen in a guest. Not done:
stretch (op 3), 8x8 colour pattern (22:23 = 3) data source, destination update bits 28:27,
quick start other than "on X extent". Cursor: 64x64, AND<<1|XOR, XR142 = colour 0,
XR141 = colour 1 (RGB332 above 8 bpp), shown only in flat-model modes.
DDC: XRD0 bits 5/6 drive SCL/SDA (levels bits 3/4), lines read in XR1FC bits 17/16 (X.org);
wired to 86Box's DDC monitor but neither the 6410 BIOS nor Alliance's driver use it.

## Windows 98 SE (VM work-pmw98)

Copy of work-p2w98 (p55tvp4, P55C 233, Win98 SE). Driver: Alliance 4.10.01.1600 (Dec 1997,
from-laptop/win95642.exe, an LHA SFX -> 64221600.EXE -> files); its INF lists only
1142:6422, so the test copy adds DEV_3210 (pmdrv*.iso in the VM folder; scratchpad pmiso/).
Desktop, Start menu, Explorer draw correctly, accelerated; the Director's Chair 95 property
pages load. tools/alliance/cycle.sh shuts Win98 down cleanly, deploys the exe and reboots
(a hard stop sends Win98 to Safe Mode). The guest keyboard is Portuguese: debug `type`
mangles / : \ -- use scancodes (':' = 2a,34,b4,aa). Mouse: `mouse 0 5 0` steps of 5 move
~2 px predictably; larger steps accelerate.

OPEN: Display Properties offers only 640x480x8. Not the driver: its ValidateMode accepts the
modes (PROMTN.INI "Resolution Preferences" default 1, memory 2 MB from INT 10h 3002h), and
no INT 10h happens while desk.cpl lists modes. Not the monitor either (set to Super VGA
1280x1024). The display class installer had pruned MODES to 4\640,480 + 8\640,480; the INF's
list was re-added with regedit (pmiso/PM1600/MODES.REG, key DISPLAY\0008) -- still one mode
after a reboot. Next ideas: CHIPID value in DEFAULT, the STB source build of the driver
(scratchpad pmiso2: deleted-built-drivers promtn.drv/vxd/promdd32), or reinstalling the
driver now that the monitor allows more.

## The 6422 (pm6422_pci, done 2026-10-16)

Same BIOS, its PCIR rewritten to 1142:6422 at init (pm_patch_bios_id, checksum byte 7FFFh).
The BIOS then takes its 6422 path:
- SR1D-SR2F are real registers; SR20 = memory size (64 KB units), SR21 = DAC type 85h.
- XR I/O window: SR1D = dword index, base port SR1F:SR1E, which the BIOS copies from
  XR194/195 at POST. XR194/195 read PCI BAR 1 (4 I/O ports; the PCI BIOS put it at FFF4h).
  The BIOS tells a 6422 from a 6412 by writing XRE0 through that window.
- Internal RAMDAC (no 20C498). Pixel PLL XREC: N bits 23:16, M 15:8, post divider 2^L bits
  3:2 (EC bit 7 = resync), f = 14.318 MHz * (N+1) / ((M+1) * 2^L), selected by MISC clock
  select 3. BIOS values check out (N=27 M=1 L=3 = 25.06 MHz; N=47 M=2 L=3 = 28.6 MHz).
- Memory sizing: XRC5 bit 0 + SR1C bit 4 (packed window), 55AAh at B800:4; a 1 MB board reads
  the upper half of each 64-bit word as empty (modelled: address bit 2).
- More VBE modes than the 6410 gets: 1280x1024x8, 1024x768x16, 640x480/800x600x32. VBETest
  bank switching passes for all of those. 1280x1024 needed CR1A (vertical bit 10s) and CR1B
  (horizontal bit 8s), now applied for both chips.
- Win98 (work-pmw98 switched to pm6422_pci): Windows found 1142:6422 and installed Alliance's
  1600 driver by itself (DISPLAY\0009, full MODES list from the INF). Accelerated desktop;
  DxDiag: DirectDraw acceleration enabled, 2.0 MB, "Test DirectDraw" all passed (windowed
  primary, off-screen, full-screen flipping).
- Still only 640x480x8 in Display Properties, on the 6422 as on the 6410, with the monitor at
  Super VGA 1280x1024, with refresh lists on the MODES keys (MODES2.REG) and with STB's own
  source build of the driver. So the limit is in Windows' mode enumeration, not the chip
  model or a driver build. DESK never calls ValidateMode (no INT 10h 3002h while it lists).
- STB's source build (pmiso2; C:\PMBAK on the guest holds the 1600 files) is what work-pmw98
  runs now. Its text was missing glyph pixels: the glyph cache (physcach.asm) sets XR40 once
  with "start on X extent" (bit 29) and starts each glyph with a dword write of width+height to
  XR58; the engine started on byte 59h, before the height. Fixed (35790a2f3): starts run when
  the whole access completes. Its DirectDraw HAL does not come up (DxDiag: not available),
  the 1600 driver's does.

## Windows NT 4 (VM work-pmnt4, done 2026-10-16)

Copy of work-p2nt4: p6bap, Celeron 233, NT 4 Workstation on an AHA-2944UW SCSI VHD, CD on
SCSI 0:1 = work-pmnt4\pmnt.iso (Alliance NT 4 driver 1.410: PROMTN.SYS/DLL, OEMSETUP.INF
without PCI IDs). Log on: Administrator, blank password. Keyboard is US here.
tools/alliance/ntcycle.sh shuts NT down, redeploys, boots and logs on.
- The miniport is legacy style: maps A0000h, SR1B = 1, reads XRCA bit 0 (PCI) and XR190 (the
  aperture base) and insists on XR180 = 1142h (fixed: XR180-183 read the PCI IDs).
- Installed through Display > Display Type > Change > Have Disk (d:\). Works on the 6422:
  full mode list (640x480 to 1280x1024 at 8 bpp, 16 bpp to 1152x864, True Color 640/800),
  the 800x600 True Color test pattern is right, desktop and Explorer at 800x600x32 draw
  correctly with the hardware cursor. Engine ops seen: fills, mono text (81003A01, 82003A01),
  pattern fills, host blits, lines (0Dh in all directions, Y major). Nothing unhandled.
- Also runs on the 6410 (pm6410_pci): 800x600x32 through the 20C498 (XR80 = 0Fh).

## Windows 98 mode list -- SOLVED (2026-10-16)

The XR180 fix (PCI IDs at XR180-183, da0bfe80b) also fixed Win98: after it, Win98 boots at
1024x768x16 with a full Display Properties mode list and switches modes live (tested
1024x768x16 -> 1024x768x8 with the 1600 driver and with STB's build). The Win9x driver /
DirectDraw side evidently checks XR180 too and fell back to a single mode. The MODES.REG /
MODES2.REG / NODDC.REG imports and the Super VGA 1280x1024 monitor on work-pmw98 are left
over from the hunt and not needed.

## Windows 95 (VM work-pmw95, 2026-10-16)

Copy of Latest\Windows 95 (p54tp4xe, P54C 200, Win95 OSR2, IDE VHD; the cfg had two
[PS/2 Mouse] sections, the stale 2-button one removed). Its wizard picks up PROMTN.INF from
the CD (work-pmw95\pmdrv.iso; type d:\ in "Copy files from" -- clear the box first, Win95's
dialog keeps the old text) and installs the 1600 driver on the 6422 (DC95.DLL/PMGAMMA
skipped). Comes up at 1024x768 High Color, accelerated, slider works.
Shut down: Ctrl+Esc, U, Enter.


## Not done / next

1. Interlaced modes: XRD2 bit 0 (STB's InterlaceControl) is set by the drivers' 43 Hz
   1024x768 mode (44.7 MHz). The CRTC then holds frame values (768 lines), so feeding
   svga->interlace (per-field) would double them; shown progressive for now.
2. Done: 6410 pixel clocks (3916e0723): XRD8 = clock index (INT 10h 3005h), frequencies
   from the BIOS's 6422 PLL table at 6775h, /2 for 15/16/32 bpp through the 20C498.
   The 6422's 16 bpp 1024x768 runs at 96.6 MHz / 93.7 Hz from the drivers' own timing
   tables -- plausible (85 Hz class), not checked against hardware.
3. Op 3 (stretch), 8x8 colour patterns, destination update bits, the video overlay window
   (XR82-XRC0, physical.h VIDWINDOW); the STB build's DirectDraw.
4. NT 4 on the 6410 (pm6410_pci).
5. AT3D/AT25 with the Stingray/GA-RUSH6 BIOSes (16 MB aperture: VGA regs at +FFF000h, XR at
   +FFEC00h; X.org apm_driver.c), no Voodoo Rush 3D.
6. Before merging: pm_do_log default 0, ROM into Xeon3D/roms, README.
