# Alliance ProMotion -- living handoff

Branch `alliance`, worktree `F:\Claude\86Box-Next\wt-alliance` (build-static there).
Device: `src/video/vid_alliance.c`, `pm6410_pci` ("Alliance ProMotion 6410 PCI").
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
- ROM in `F:\Claude\86Box-Next\ROMS-EXTRA\video\alliance\pm6410\PCPartner-6410.BIN`
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
- XR80 bits 3:0 pixel format (2 = 8 bpp, Ch 15, Dh 16, Eh 24, Fh 32); bit 6 set by VBE modes.
  XRC6 DAC control (03h in 16 bpp, 0Ah/09h variants for double index), XRD8 clock select
  (00h at 640x480x8, 04h at 640x480x16, 03h at 800x600x16), XRD4-D6 FIFO thresholds,
  XR190 = linear aperture base (BIOS writes the PCI BAR there).
- Extended modes: CRTC timings count pixels (16 bpp modes have the same CRTC as 8 bpp);
  start address in dwords (+ CR1C 3:0), offset in 8-byte units (+ CR1C 7:4).
- VBE 1.2, no LFB reported. Modes seen: 640x400/480, 800x600, 1024x768 x8; 640x480,
  800x600 x16 (2 MB). VBETest bank switching passes for 640x480x8/16, 800x600x16, 1024x768x8.

## Not done / next

1. Pixel clock: XRD8 + MISC bits 3:2 -> frequency table unknown; recalctimings runs extended
   modes at 60 Hz meanwhile. Collect XRD8 per mode and match the 6422 datasheet's mode table.
2. Drawing engine (XR30-XR74, status XR1FC, blit port): physical.h/physblt.asm + apm_regs.h.
3. Hardware cursor XR140-XR14D (64x64 2bpp, apm_cursor.c ConvertTable: 00/01 colours, 10
   transparent, 11 invert -- verify bit order).
4. Windows 95/98 with the STB Horizon 64 Video driver (install-packages) -> needs 2+3 and the
   linear aperture (SR1C/SR1A, regs at the aperture end).
5. 6422 (internal DAC + PLL XREC/XRE8, I/O XR window SR1D-1F/XR194-195, 4 MB), then AT3D/AT25
   with the Stingray/GA-RUSH6 BIOSes (16 MB aperture: VGA regs at +FFF000h, XR at +FFEC00h).
