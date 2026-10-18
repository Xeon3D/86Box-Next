# CARDTEST — working notes

Living notes for the CARDTEST CD (`F:\Claude\86Box-Next\cardtest`, not in a git repo yet).

> 2026-10-17 17:30: this file was accidentally truncated by the autogui session and rebuilt from
> its last known content (the copy shown to that session at 13:22 plus its own AUTOGUI section).
> Sessions that added text here after 13:22: please re-add it.

## Layout
- `dos/` NASM sources: `lib.inc` (log/CRC helpers, included at the END of each tool),
  `vgachk.asm`, `vbechk.asm`.
- `win/` freestanding Win32 C (no CRT, `-march=i586`, DirectDraw/OpenGL loaded at run time):
  `ct.h` (log, BMP, grading), `gdichk.c`, `ddchk.c`, `d3dchk.c`, `glchk.c`, `autogui.c` (AUTOGUI below).
- `cd/` files copied to the CD root (README.TXT, RUNDOS.BAT, RUNWIN.BAT, CARDTEST.TAG).
- `third.sh` copies the owner's local benchmarks into `THIRD/` and forces CRLF on the root text files.
- `build.sh` builds everything into `out/CD` and `out/CARDTEST.ISO`; `--tools-only` skips the ISO.
- `host/review.py` pulls `C:\CTRES` from a VM disk (raw/VHD, FAT with long names; NTFS via
  `host/ntfsread.py`) into `runs/<key>/<time>/review.html`; `--bless` makes it `golden/<key>`.
  `host/sheet.py` makes card|reference|diff contact sheets.

## Driving a test VM
`PM_DIR=<vm> bash wt-alliance/tools/alliance/pm.sh start|cmd|shot|stop` (minimized, debug
command file, no focus). Point `cdrom_01_image_path` at `cardtest/out/CARDTEST.ISO` (cfg backed up
as `86box.cfg.bak-cardtest`). Win98: Start > Run = `key 1d,01,81,9d` then `key 13,93`.
After rebuilding the ISO the guest must restart (CDFS caches).
Keystroke loops longer than 2 minutes: run them with run_in_background from the start.
(Shared rules for parallel sessions: COORDINATION.md in the session group's notes folder --
each session its own VM and a private copy of the ISOs inside its VM folder.)

## Grading lessons (first runs, 2026-10-17)
- GDI exact vs the DIB engine works, except where GDI lets the device decide:
  styled cosmetic pens (dash lengths) and stretches (DDA) -> STYLED/STRETCH/DIBSTR use 2 px slack and never FAIL.
- D3D's RGB rasterizer and Microsoft's generic GL are imperfect oracles (seams, half-texel
  offsets, generic GL draws with GL_NEVER) -> 3D cases use tolerance + position slack
  (`ct_grade_r`), log the strict number too, and DEPTH leaves GL_NEVER out.
- Diff pictures: red = beyond tolerance, orange = within tolerance/slack only.
- DDCHK: GetMonitorFrequency answers nonsense on the TNT driver (6559) -> ignored outside 40-200 Hz.

## Findings on the emulated cards (2026-10-17 runs; runs/ has the pictures)
### RIVA TNT (work-tnt: Win98 SE, NVIDIA 4.11.01.0115, 1024x768x32) -- runs/tnt-w98
- DOS (work-dos-tnt, ELSA ERAZOR II BIOS): VGACHK 8/8, VBECHK 28/28 (LFB not run: EMM386 loaded).
- DDCHK 10 PASS (MIRROR not in caps), overlay shown. D3DCHK 15 PASS / 2 WARN (TEXBILIN, MIPMAP).
- GDI BRUSH (bug): hatch brushes in TRANSPARENT mode fill the background black.
- GDI SHAPES (bug): 4-point polygons (convex quad, WINDING quad) fill only one triangle.
- GDI DIB: 16 bpp (555) source DIBs expand 5->8 bits differently (max 6 levels; now graded ROUND).
- GL (bug, TNT-specific -- the same GLCHK passes 13/17 on the P2 ICD): textures created right
  after the context draw garbage; once a 4x4 GL_RGB texture with UNPACK_ALIGNMENT 1 (48 bytes) is
  uploaded, every later draw has wrong colours. With per-case wglMakeCurrent switching (first run)
  the untextured cases were exact. Start with TEXSMPL + FLATGOUR.
### Permedia 2 (work-p2d3d: Win98 SE, ELSA Synergy BIOS + 3Dlabs ICD, 1024x768x16) -- runs/p2-w98*
Fixed on branch permedia2-cardtest, merged into master 82a9a00b0 (staged build 177); details in
side-session-notes ...\permedia2-cardtest.md; final run runs/p2ct-all:
- GDI ROP3 FIXED (exact): unaligned aperture dword reads (DIB engine software ROP3s). The "bit 0"
  pattern was a coincidence: those cells all had an odd source x.
- GDI BRUSH FIXED (exact): LUT DirectIndex colour brushes.
- GDI LINES/SHAPES/CLIP/OFFSCRN: line pixels at pixel centres -> 69/2/36/45 px left: the ELSA driver's
  end-inclusive horizontal lines, its 16.16 slope truncation and clipped-line restarts.
- D3D FIXED: single-level textures (Blt loads with TextureMapFormat 0). Left, all hardware:
  SPECULAR (scalar Ks), MIPMAP/TRILIN (one level per triangle), ALPHATST (no alpha test, acmp=0:
  the tool should SKIP).
- DDCHK MIRROR: ELSA HAL bug, sends a plain copy for DDBLTFX_MIRRORLEFTRIGHT.
- GLCHK: MIPMAP/TRILIN base level only (no P2 mipmapping); TEXENV only GL_BLEND (no such P2 mode);
  PERSP = REFERENCE problem: generic GL is affine, the TNT and P2 agree 99.86% -> set
  GL_PERSPECTIVE_CORRECTION_HINT/GL_NICEST in GLCHK or grade loosely.
### Found by the AUTOGUI runs (details: the notes folder's autogui-findings.md)
- TNT2: the PCI subsystem ID reaches Windows as missing (DCT: "SUBSYS ID section of the PNPID ...
  missing"); DCT D3D tests log "Test N corrupted the floating point state" (driver or emulated FPU
  control word); the DCT's timecore.cpp assertion (timing); an older TNT2 build hung in 3DMark2000
  Game 1 high detail (not with the 14:53 staged build).

## Not done yet
- Runs on Savage3D, ViRGE/MX, 65554, ProMotion/AT3D, NV3, and NT4/Win2000 guests; 8 and 16 bpp
  passes on the TNT; DOS with LFB (no EMM386). No goldens blessed yet.
- WHQL display tests: the owner's MSDN 1846-4/5 ISOs (Downloads) hold HCT 12.1 and DCT 5.3,
  both XP SP2 / Server 2003 only. DCT 5.3 (the display tests) is on disc 2 as DCT53; HCT 12.1 has
  no adapter test binaries, but its test spec (Win9x/Win2000 GDI, DirectDraw, D3D DX6/7, OpenGL,
  AGP assertions) is in `ref/hct12-graphics-spec/index.htm` -- use it to extend GDICHK/DDCHK/
  D3DCHK. HCT ISO kept in dl/cardtest-extra/hct.

## Disc 2 (2026-10-17)
`disc2.sh` -> `out/CARDTEST2.ISO` from `dl/cardtest-extra` (owner-approved downloads):
3D WinBench 2000 CD (archive.org 3-d-win-bench-2000; also WinBench 99 1.1, Audio WinBench 99) at
the root, `DX7SDK` (archive.org dx7sdk-7001: bin/dxutil + prebuilt d3dim/d3drm/d3dx/ddraw
samples with media), `FINALRLT` (archive.org final-reality-1997), `GLEAN` (SourceForge glean
OldFiles glean-win-exe-1-0.zip; needs opengl32+glu32), `SPECVP` (SPECviewperf 6.1.2,
specviewperf612is01.exe from vogonsdrivers.com fileid 2136 -- owner's link; 55,863,930 bytes =
SPEC's own listed size; unpacked InstallShield set), `DCT53`, `DCT130`, `DCT135` (below).
Text in `cd2/README2.TXT`.

## Microsoft HCT/DCT kits (2026-10-17, owner-approved downloads, MD5 = archive.org's)
- `dl/cardtest-extra/hct91/HCT91.iso` (MSDN X04-95176, Aug 1999): HCT 9.1 for Win98 + NT4. Video
  tests in hct/Device/Video (GDI .scr scripts, rop4, text/fonts, DOS-box VGA vvd*.exe, OpenGL
  conformance oglcfm*.bat, DirectDraw tdd*.pro) and hct/vidauto (ddraw, mcdtest, pc97). Install
  with hct\hwtest.exe. Docs: hct/docs/tvideo.htm.
- `hct95/HCT95.iso` (X05-44899, Feb 2000): HCT 9.5; its devkits/DCT_300 = DCT 1.30.0.300 for
  Win95/98/2000/NT4 -> disc 2 `DCT130`.
- `hct96/HCT96.iso` (X05-97897, Aug 2000): HCT 9.6 for Windows Me; devkits = DCT 1.35.0.350
  (Me only) -> disc 2 `DCT135`.
- Data ISOs only; the discs' CD audio tracks (audio tests) were not downloaded.

## AUTOGUI -- unattended GUI benchmarks (2026-10-17, autogui session)
- `win/autogui.c` -> WIN\AUTOGUI.EXE: script runner (command list at the top of the source).
  Scripts in `gui/*.AGS` (on disc 1 as \GUI, RUNGUI.BAT also at the root for RUNALL's hook);
  `gui/ZD/*.ZDR/.ZDS` = our WinBench batch files; `gui/CTSPEC.BAT` = SPECviewperf without Notepad;
  `gui/LEARN*.AGS` = developer aids (install + dump windows). Results C:\CTRES\GUI\<NAME>.LOG
  (RESULT/CASE/ERROR/MANUAL lines) + C:\CTRES\GUI\<NAME>\*.BMP; `host/guiresults.py` puts them
  in review.html (OCR of shots named *score*/*result* via tesseract in WSL, cached .ocr.txt).
- Host helpers: `host/pullres.py VM DEST` (C:\CTRES, FAT or NTFS), `host/pullfile.py VM DIR DEST`,
  `host/ntfsread.py` (ntfs-3g in WSL, raw images), `host/vmlock.sh` (optional, unused now).
- Test VMs: work-autogui (copy of work-tnt2, Win98 SE + TNT2, exe from Latest 14:53, two CD drives
  cd1.iso/cd2.iso + the HCT 9.1 disc hct.iso on IDE 0:1 -- so drive letters D:=HCT E:=disc 1
  F:=disc 2), work-autogui2k (copy of work-w2k-tnt, Win2000 + TNT2 + NVIDIA 71.84, NTFS, D:/E:).
- Learned:
  - The generic `wizard` walks all setups unattended (3DMark 99/2000 InstallShield 5, Final Reality
    IS3, SPECviewperf IS5, DCT 1.30): ~7.5 min for five. ZD benchmarks install with their own
    SilentSU + setup.iss (C:\ZDBENCH).
  - 3DMark 99/2000 are MFC: welcome dialog buttons ("New 3DMark Benchmark" -> project window ->
    "Benchmark"; 2000: "Run Default Benchmark"); result window "... Overall Score", scores readable
    with WM_GETTEXT (TNT2: 3DMark 99 2615 / CPU 2575; 3DMark2000 1038 "3D marks").
  - Win9x: a full-screen DirectX benchmark holds the Win16 lock, so AUTOGUI's window calls stall
    until it yields; use `pause` (kernel-only) right after starting one, `waitmode` to detect the end
    of a run whose launcher stays visible (Final Reality).
  - Final Reality: licence dialog "Final Reality License Agreement" / Accept; launcher buttons
    "Run benchmark", "Display results".
  - ZD WinBench 99 / 3D WinBench 2000: documented batch mode (.ZDR: ExitBenchmark, TextFile,
    CSVFile, Suite / SuiteSection), started through the .zdr file association (`open`). First start:
    licence whose Proceed only enables after scrolling the text to the end (AUTOGUI rules scroll edit
    boxes when the button is disabled); local "Registration" box -> Yes; "System Configuration
    Problem" wizard (red X = taskbar always-on-top / not full screen) -> `taskbar off` (hide the tray,
    drop topmost, full-screen work area; ABM_SETSTATE is XP-only). The video tests insist on the ZD CD
    in the FIRST CD drive -> dropped from CTWB99.ZDS (Cancel aborts the whole batch).
  - SPECviewperf: run_viewset.bat VIEWSET SHORT results -> sum_results\SHORT\summary.txt ("Mean").
  - HCT 9.1 display tests are command-line batches in hct\Device\Video (+ NTLOG/TLOG DLLs): GDIMan
    (gdim_run.bat), guijr video (vidtest2.bat), OpenGL conformance 1.0.9 (oglcfm.bat), DPMS (9x),
    VDM VGA tests vvideo1.bat MODE LEVEL (NT).
  - WHQL DCT 1.30: manager C:\dct\Common\Shell\x86\dct.exe ("Display Conformance Test"), a debug
    build (timecore.cpp assertion -> Ignore). Run combo: [0] All [1] All - Manual [2] All - Auto
    [3] Not Run - Auto [4] Not Run - Manual [5] PCI [6] PC 99 [7] Power Mgmt [8] DirectDraw
    [9] Direct3D [10] D3D Dx6 [11] Beta Preview [12] Win9x GDI [13] AGP [14] OpenGL. Command line:
    -c<n> auto-start, -l no low-res modes, -x<cmd> run on completion (DCT130.AGS writes DCTEND.TXT).
    Untick "Run As Windows Shell" (shell replacement + reboots). It also wants the taskbar not
    always-on-top, and leaves the desktop in another mode.
- Pitfalls: Git-Bash heredocs collapse backslashes (write C edits from a Python file made with the
  Write tool); a failed Python `open(p,'w').write()` truncates the file -- encode first;
  never mount cardtest/out/*.ISO in a running guest (copy it into the VM folder); the emulator
  rewrites 86box.cfg on exit (re-add extra CD drives before each start); files copied from a CD
  stay read-only (AUTOGUI copy/copyx/copydir clear it); never let a watcher say Yes to "restart
  now?" (Win2000 asks after new hardware; AUTOGUI answers No to restart/reboot/delete/format).

## One-click run: RUNALL.BAT (2026-10-17, runall session)
- `cd/RUNALL.BAT [/SHUTDOWN]` -> `RUNDEPTH.BAT n` for 8/16/24/32 bpp -> `RUNXTRA.BAT` (copies GLQuake
  and the Quake II exe/dlls to C:\CTRUN, runs `CTRUN /JOBS EXTRAS.TXT`, then \GUI\RUNGUI.BAT if
  present -- the AUTOGUI stage, which also runs SPECviewperf; it needs disc 2 in a second drive)
  -> `CTRUN /SUMMARY` -> `DEPTH /SHUTDOWN`. RUNALL sets CTRESUME=`command.com /c start <cd>\RUNALL.BAT /RESUME %1` ("start": RunOnce must not hold Explorer back, or D3D is missing)
  before RUNXTRA; when RUNGUI restarts Windows for the DirectX 7 install it puts that in RunOnce, and
  /RESUME skips the depths and extras: RUNGUI /RESUME -> summary -> shutdown. (/RESUME path not yet
  run in a guest: work-runall has only disc 1.)
- `win/depth.c` (DEPTH.EXE, console): /LIST /SET n /SAVE f /RESTORE f /SHUTDOWN; errorlevel 0/1/2
  (2 = Windows wants a restart for that depth); log C:\CTRES\DEPTH.LOG.
- `win/ctrun.c` (CTRUN.EXE, console): /JOBS file (WAIT = watch a log for a pattern, then close the
  program: WM_CLOSE, TerminateProcess after 10 s; RUN = stdout to a file, wait) -> EXTRAS.LOG;
  /SUMMARY -> C:\CTRES\SUMMARY.TXT (per depth DONE line + FAIL/WARN case names, DEPTH.LOG,
  EXTRAS.LOG, glean pass/fail). Console subsystem on purpose: command.com waits for it.
- Tools take `/OUT=<folder>` (or env CTRES) for their results folder (ct.h ct_open).
- review.py shows SUMMARY/EXTRAS/DEPTH/MODES and every <n>BPP folder.
- build.sh / disc2.sh: "build lock" (mkdir out/.buildlock; a second build waits; if a build was
  killed, rmdir it by hand).
- Lessons: Quake 1 needs `-condebug` (a + command is ignored); Quake asks "Press Y" on WM_CLOSE
  (the TerminateProcess fallback handles it); command.com treats < > in REM lines as redirection;
  on Windows 9x ExitWindowsEx from a console program (it runs in a DOS-box session) took 674 s to
  reach "safe to turn off" -- DEPTH /SHUTDOWN now starts `RUNDLL32 SHELL32.DLL,SHExitWindowsEx 5`
  detached and returns: seconds, and p6bap powers the VM off (the emulator exits). NT keeps
  ExitWindowsEx with the shutdown privilege.
- Verified on `work-runall` (copy of work-tnt2: Win98 SE, TNT2 ERAZOR III, 1024x768x32, own ISO
  copy, no DirectX 7 so D3DCHK skips): 8/16/32 bpp (no 24 bpp at 1024x768), GLQuake 114.6 fps,
  Quake II soft 21.5 / GL 57.4 fps, glean 40/44 (blendFunc fails). Launch -> VM powered itself off
  in 534 s, no input. Results: runs/runall-tnt2. Host side: wait until `pm.sh running` says 0,
  then review.py. NT4/2000 not run yet.
- TNT2 results there: GDICHK BRUSH+SHAPES fail at every depth (+DIB at 8 bpp); DDCHK at 8 bpp fails
  UPLOAD/SRCKEY/STRETCH/GETDC (not triaged; may be the 8 bpp HEL-vs-HAL comparison itself);
  GLCHK 15 PASS, TEXLIN/PERSP fail.
- Seen on the side (TNT2): Win98's "safe to turn off" screen shows as an 80x400 mode (it is a
  320x400 graphics screen) -- probably an NV5 mode-decode bug; noted for the TNT session.
