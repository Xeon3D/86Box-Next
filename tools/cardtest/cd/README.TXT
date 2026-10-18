CARDTEST 1.0 - graphics card test CD for 86Box-Next (DOS to Windows 2000)
==========================================================================

What it is
----------
A test suite for emulated graphics cards, from register level up to
OpenGL. The CARDTEST tools check themselves against an oracle wherever
one exists:
  - VGA rules (IBM VGA behaviour) for the DOS VGA tests
  - the card's own read-back for VESA memory and registers
  - Windows' software DIB engine for GDI
  - the DirectDraw HEL (system memory) for DirectDraw
  - the DirectX 7 "RGB Emulation" rasterizer for Direct3D
  - Microsoft's generic software OpenGL for the OpenGL ICD
The well-known third-party benchmarks are on the CD as well.

Every tool writes C:\CTRES\<TOOL>.LOG, and the Windows tools also write
C:\CTRES\<TOOL>\*.BMP: _S = the card, _R = the reference, _D = the
difference (red = pixels that differ). Log lines:
  INFO  facts (caps, mode lists, registers, EDID ...)
  CASE  <name> PASS | WARN | FAIL | SKIP | VISUAL  <details>
  DONE  pass=N warn=N fail=N skip=N
VISUAL cases can't be checked by reading memory back (overlays,
split screens, refresh). Judge them by eye, or with a host screenshot
compared against a known-good card.

ONE-CLICK RUN  (RUNALL.BAT; Windows 95/98/Me/NT4/2000)
------------------------------------------------------
Double-click RUNALL.BAT on the CD (or Start > Run > D:\RUNALL.BAT).
RUNALL /SHUTDOWN also shuts Windows down at the end. If RUNGUI has to
restart Windows (installing DirectX 7 on Windows 98), RunOnce starts
RUNALL /RESUME, which finishes RUNGUI, the summary and the shutdown. It:
 1. clears C:\CTRES, saves the current mode (DEPTH /SAVE) and lists
    every mode the driver offers (C:\CTRES\MODES.TXT)
 2. for 8, 16, 24 and 32 bpp, where the driver has that depth at the
    current resolution (WIN\DEPTH /SET n): GDICHK, DDCHK, D3DCHK and
    GLCHK with /Q, results in C:\CTRES\8BPP, 16BPP, 24BPP, 32BPP
 3. puts the original mode back
 4. runs the extras in EXTRAS.TXT through WIN\CTRUN.EXE, each with a
    time limit: GLQuake timedemo (windowed), Quake II timedemo in the
    software and OpenGL renderers (windowed), glean (OpenGL tests).
    The games are copied to C:\CTRUN first. Scores go to
    C:\CTRES\EXTRAS.LOG ("EXTRA <name> RESULT ... fps").
 5. writes C:\CTRES\SUMMARY.TXT: per depth, each tool's pass/warn/fail
    counts and failing cases; the depth changes; the extras' results
Nothing to answer on the way. Steps 1-5 take about 10 minutes on a
233 MHz guest. A depth Windows can only change after a restart
(Windows 95) is skipped and noted in DEPTH.LOG.
Last, if the CD has it, \GUI\RUNGUI.BAT runs the GUI benchmarks (see the
next section; it needs disc 2 in a second CD drive for most of them).

Still by hand: the WHQL DCT 1.35 / 5.3 kits and the HCT 9.5 / 9.6 discs;
DCT 1.30 and HCT 9.1 run automatically from RUNGUI.BAT below.

GUI BENCHMARKS  (RUNGUI.BAT; Windows 95/98/Me/NT4/2000; called by RUNALL)
-------------------------------------------------------------------------
WIN\AUTOGUI.EXE installs and runs the benchmarks that have no command
line, unattended, from small scripts in GUI\*.AGS (the commands are listed
at the top of the source, win\autogui.c on the host). Licence and EULA
screens are accepted, registration pages get "Purchase Later" (no key is
typed), "restart now?" gets No, folder windows setups leave open are
closed, unexpected message boxes are answered and screenshotted. Needs
both CARDTEST discs at once (two CD drives); the HCT script also wants
the HCT 9.1 disc in a further drive. Results: C:\CTRES\GUI\<NAME>.LOG
(RESULT lines = scores) and C:\CTRES\GUI\<NAME>\*.BMP / exported files.
  3DM99.AGS    3DMark 99 Max: install, default benchmark, 3DMarks + CPU 3DMarks
  3DM2000.AGS  3DMark 2000: install, default benchmark, scores
  FINALR.AGS   Final Reality: install, benchmark, result screens
  ZDWB.AGS     WinBench 99 graphics tests + 3D WinBench 2000 (their own
               batch mode, GUI\ZD\*.ZDR; text/CSV exports in GUI\ZDWB)
  SPECVP.AGS   SPECviewperf 6.1.2: install, Dx/Light/AWadvs viewsets
               (GUI\CTSPEC.BAT ALL for all six), summaries in GUI\SPECVP
  DX7SDK.AGS   the DirectX 7 SDK samples: each started, screenshotted, closed
  DCT130.AGS   WHQL DCT 1.30 (disc 2): install, "All - Auto" run, logs
               copied to GUI\DCT130; manual prompts answered No + listed
  HCT91.AGS    WHQL HCT 9.1 automatic display tests: GDIMan, guijr video,
               OpenGL conformance (all pixel formats), DPMS; prompts that
               need a human are answered No and listed as MANUAL
On a 266 MHz guest a full RUNGUI takes several hours. One script alone:
  D:\WIN\AUTOGUI.EXE D:\GUI\3DM99.AGS     (AUTOGUI script /DUMP logs all
  windows and controls -- for writing a new script)

DOS  (RUNDOS.BAT; boot clean DOS without EMM386)
------------------------------------------------
DOS\VGACHK.COM [/V]
  REGS     every writable CRTC/SEQ/GC/ATTR bit reads back
  DAC      palette write/read, auto-increment, PEL mask, DAC state
  ALU      4000 random planar writes (write modes 0-3, rotate, set/reset,
           logical function, bit mask, map mask), checked per plane
  RDMODE1  read mode 1 colour compare / don't care
  TEXTMEM  text memory and cursor-location registers
  RETRACE  refresh rate of modes 3, 12h, 13h (70/60/70 Hz +-4)
  INFO     register state the BIOS sets for each standard mode, font CRC
           (compare with a reference card, e.g. 86Box's IBM VGA)
  /V       V1 256-colour grid, V2 split screen + panning, V3 text
           attributes/blink/cursor - one key per screen
DOS\VBECHK.COM [/Q] [/B]
  VBEINFO  4F00, OEM strings, memory, mode list
  M<mode>  every supported 8/15/16/24/32 bpp mode: set + 4F03, banked
           pattern write/verify over the whole frame, 4F06 bytes per line,
           4F07 display start read-back, LFB pattern write/verify
  MEMSIZE  bank through ALL reported memory (catches aliasing)
  DDC      4F15 EDID header + checksum, manufacturer, preferred mode
  INFO     4F10 power states, 4F0A protected-mode interface
  /Q information only, /B no LFB tests

WINDOWS  (RUNWIN.BAT; run at 8, 16 and 32 bpp)
-----------------------------------------------
WIN\GDICHK.EXE   against the DIB engine in the screen's own format; exact:
  FILL ROP3(all 256) SCROLL BRUSH LINES(solid, ROP2, wide) SHAPES TEXT
  MONO CLIP OFFSCRN(video-memory bitmaps), DIB (colour rounding only);
  loose (2 px slack, never FAIL - GDI lets the device decide these):
  STYLED (dash lengths) STRETCH DIBSTR (stretch sampling)
WIN\DDCHK.EXE    DirectX 3+ interfaces (works on NT 4):
  caps/FourCC/mode list; COLORFILL UPLOAD VIDBLT OVERLAP SRCKEY MIRROR
  STRETCH GETDC LOCKSYNC (lock right after a blit sees the finished
  blit); full screen 640x480x16: FLIP rate vs monitor, VBLANK +
  scan line, OVERLAY (VISUAL: YUY2/UYVY colour bars, dest colour key)
  /NOFS skips the full-screen part
WIN\D3DCHK.EXE   DirectX 7 (Win98 + DX7, Win2000); HAL vs RGB rasterizer
  with tolerances: FLATGOUR ZBUFFER ZFUNC TEXPOINT TEXBILIN PERSP
  ALPHABLD BLENDMOD ALPHATST COLORKEY FOG SPECULAR TEXADDR TEXOPS
  MIPMAP TRILIN MULTITEX; the device caps are logged
  /VIDTEX loads the card's textures by Blt into video memory (the
  DirectX 5/6 way) instead of through the DirectX 7 texture manager
WIN\GLCHK.EXE    ICD vs Microsoft generic OpenGL, with tolerances:
  FLATGOUR DEPTH TEXNEAR TEXLIN PERSP BLEND ALPHATST FOG MIPMAP TRILIN
  TEXWRAP TEXENV LINES STENCIL PIXELS LOGICOP; strings, extensions and
  all pixel formats logged
All accept /Q (no pauses between cases). 3D grading: a pixel is bad
only if no reference pixel within 1-2 px matches it within the
tolerance (real hardware and software rasterizers legitimately differ
in edge rules and texel centres); the strict figure is logged too. The card's picture is shown on
the left and the reference on the right while the 3D tools run.

THIRD-PARTY  (\THIRD)
---------------------
DOSBENCH   DOSBench 1.6 menu: 3DBench 1/2, PCPBench, Chris's 3D, Doom
           timedemo, Quake timedemo, FastVid, cache/CPU checks
UNIVBE     SciTech UniVBE 6.7 + VBETEST (VESA modes, hi-res, refresh)
SPEEDSYS   Speedsys (video memory bandwidth)
QUAKE1     Quake 1.06 shareware: DOS VESA/VGA "timedemo demo1"
GLQUAKE    GLQuake: "glquake -width 640 -height 480", "timedemo demo1"
GLEAN      glean 1.0 for Win32 (OpenGL tests): BIN\GLEAN.EXE -r C:\RESULTS
QUAKE2     Quake II: software and OpenGL "timedemo 1; demo demo1.dm2"
3DMARK99   3DMark 99 Max (DX6)
3DM2000    3DMark 2000 (DX7)
UTDEMO     Unreal Tournament demo 3.48 (D3D/OpenGL/software)
DX7, DX81  DirectX 7.0 / 8.1 redistributables (DX7 needed by D3DCHK on 98)
VIDEO      TEST.MPG (MPEG-1, for Media Player overlay tests)

Suggested order per card x OS
-----------------------------
1. DOS: RUNDOS (then RUNDOS /V for the visual screens), DOSBENCH.
2. Windows, each colour depth: install the driver, RUNWIN, then
   DxDiag (DirectDraw + Direct3D tests), Media Player TEST.MPG,
   3DMark 99/2000, GLQuake/Quake II timedemo, UT demo.
3. Integration (by hand): Display Properties through every mode and
   refresh; full-screen DOS box and back; Safe Mode; warm restart;
   Alt-Tab out of a full-screen game; screen saver (OpenGL ones).

Disc 2 (CARDTEST2.ISO)
----------------------
3D WinBench 2000 (with its image-quality tests), WinBench 99 1.1
(Graphics WinMark = real applications' GDI calls), Audio WinBench 99,
Final Reality 1.01, glean 1.0 (OpenGL conformance), SPECviewperf 6.1.2
and the DirectX 7.0a
SDK's DDTest, caps viewer and prebuilt Direct3D/DirectDraw samples.
Also Microsoft's WHQL display tests: DCT 1.30 (Win95/98/2000/NT4), DCT 1.35
(Windows Me) and DCT 5.3 (Windows XP SP2). The full HCT 9.1/9.5/9.6 kits
are separate discs (see README2.TXT).
See its README2.TXT.

Host side
---------
cardtest\host\review.py (on the 86Box-Next host) pulls C:\CTRES from a
VM's disk (FAT; for NTFS guests copy the folder out and pass --dir),
builds a review page with card / reference / diff pictures and compares
them with the approved ("golden") results of earlier runs.
