# 86Box-Next: additions over upstream 86Box

Status as of **Release 2.1.5** (build 179, 17 October 2026). For each addition: what it is,
its status, and the known bugs or limitations. "Driver" or "hardware" means real hardware with
that driver behaves the same; those are not emulation bugs. Details live in `CLAUDE.md`, the
handoff files and `tools/cardtest/NOTES.md`.

**Status legend:** ✅ working and verified in guests · 🟡 working, partly verified or with open
issues · 🧪 experimental / not tried in a guest · ⛔ not in this release (unmerged branch)

---

## Emulator and user interface

| Addition | Status | Known bugs / limitations |
|---|---|---|
| **86Box-Next branding** (`EMU_DISPLAY_NAME`; `EMU_NAME` stays "86Box" for paths and guest-visible IDs) | ✅ | None known. |
| **Per-machine config only** (no global config; `vmm.ini` beside `86box.cfg`) | ✅ | None known. |
| **VM Manager optional, off by default** | ✅ | None known. |
| **Build number and releases** (`.build-number` in the title bar, `NEXT_RELEASE=N` → "Release N", `tools/make-release.sh`) | ✅ | Releases are Qt 6 only. `make-release.sh` refuses to run while any 86Box-Next process runs, and stages branch-owned rigs too (the ISDN rig's exe must be kept aside). |
| **Status bar icon order** (Preferences > Status bar) | ✅ | A new icon group needs its key and name added by hand. The PC Card/USB/modem/Sound Canvas menus used to be lost when Preferences replaced the status bar; fixed (`adoptMenus()`). |
| **Soft settings changes** (PC Cards, COM/LPT devices, MIDI, ISA MPU-401 without a PC reset) | ✅ | Still need a hard reset: port enable/disable, ECP DMA jumper, MCA MPU-401, a port held by a non-hotplug device (serial mouse), fitting or removing the PCMCIA controller. A COM-port soft-reset crash was fixed (b33868148). |
| **BIOS vendor in Settings > Machine** | ✅ | Machines the generator cannot place show "Unknown"; rerun `tools/machine-bios-vendors.py` when upstream adds machines. |
| **Debug command file** (`BOX86NEXT_DEBUG_CMD`: keys, typing, mouse, screenshots, breakpoints, memory peeks, device dumps, CD swap, reset) | ✅ | `type` assumes a US keyboard layout (other layouts need scancodes). `interp 1` is cleared by every CPU reset. `bp` works in the interpreter only. Alt+Enter injection does not toggle full screen. |
| **Upstream CI disabled**, weekly `sync-upstream.yml` merge | ✅ | None known. |

## Devices

| Addition | Status | Known bugs / limitations |
|---|---|---|
| **Arcade I/O boards** (funworld Photo Play; Merit Megatouch XL/MAXX) | ✅ | None known. |
| **Elo TouchSystems SmartSet serial touchscreen** | ✅ | None known. |
| **PCMCIA controller** (Cirrus CL-PD6722, two hot-plug sockets, status-bar PC Card menu) | ✅ Win98 | SRAM/flash memory cards were removed (2026-10-07). The Win98 image's USB-backport tray "Stop" does nothing: guest software, not the emulator. |
| ↳ 3Com 3C589D | ✅ | None known. |
| ↳ TRENDnet TE100-PC16 | ✅ | None known. |
| ↳ 3Com 3C562D LAN + 33.6 modem (multi-function) | ✅ Win98 | Windows tends to put the modem on COM4 at 2E8h, which S3 cards' 8514/A registers shadow (reads 00h, real-hardware behaviour): move the modem's I/O. ATI answers are invented; the LAN product ID is unchecked against a real card. |
| ↳ Hayes Accura 56K | 🧪 | Not tried in a guest; ATI answers are plausible, not dumped. |
| ↳ Adaptec APA-1460 SlimSCSI | ✅ Win98 | PORTA/B read FFh. |
| **USB** (UHCI VIA/Intel, EHCI + companion, host passthrough via libusb/UsbDk, connect prompt, trace) | ✅ | No siTDs: full-speed devices on the USB 2.0 card go through the companion controller. A crash when two VMs listed USB devices at once is fixed (7f62b62c1); older exes can still hit it. |
| **COM-port modems** (Diamond SupraExpress 56e PRO, ELSA MicroLink 56k, Standard Hayes 56k; replaces upstream's network modem) | ✅ | The Hayes model has no voice; only the Supra has a Serial PnP ID. |
| **Internal modem** (Diamond SupraExpress 56i Sp, ISA PnP or jumpered) | 🧪 | No guest test recorded. |
| **Voice calls** (Rockwell `#` set and V.253, ADPCM, host handset/speaker/microphone) | ✅ | PC Card modems have no voice. |
| **isp-server** (submodule: virtual ISP + telephone exchange, web status page and browser phone; Windows/Linux/macOS/Docker) | ✅ | The browser phone's microphone needs a secure context (localhost or HTTPS). MS-CHAPv1 + MPPE can't be checked against Linux's pppd (pppd's own bug). |
| **Roland Sound Canvas MIDI out** (88emu, 24 boards, front panel; replaces MUNT and the CLAP device) | ✅ | Builds with it are GPLv3. Upstream changes to its CMake/`midi.h` blocks can conflict on sync. Needs the owner's ROM dumps. |
| **x87 EMMS fix** (old dynarec) | ✅ new in 2.1.5 | After MMX code the FPU tags were left "valid"; WHQL's DCT reported "corrupted floating point state". Fixed for every card (765b2e726). |

## Video cards

| Card | Status | Verified | Known bugs / limitations |
|---|---|---|---|
| **NVIDIA RIVA 128 / 128 ZX** (NV3, PCI/AGP) | ✅ | Win98 SE (3DMark 99 ≈1465, DxDiag D3D, OpenGL screensavers), Win95, Win2000, NT4 AGP, WfW 3.11; mfgtest modes 33/33 | Filtering and alpha blending unchecked against real hardware. Stray columns in the overscan (hidden with overscan off). Overlay: no YV12/YVU9, no interlaced bob. Win2000 D3D is software only and OpenGL at 32 bpp uses Microsoft's renderer (driver limits). Two BIOS versions undumped. |
| **NVIDIA RIVA TNT / TNT2 / Ultra / M64** (NV4/NV5, 18 board BIOSes; CPU-thread or Vulkan 3D) | ✅ | Win98 SE (drivers 4.11–45.23: 3DMark 99/2000, UT, Quake III 39 fps, GLQuake 85, Quake II 38.5), Win2000, NT4 SP6a, DOS VESA | New in 2.1.5: GDI transparent hatches, quads, texel origin and lost-trap fixes. Open: TNT2 shows Win98's "safe to turn off" screen as 80×400 (suspected NV5 mode decode); at 8 bpp some DirectDraw checks fail (not triaged); glean blend test fails; D3D mipmap WARN. Not emulated: TV encoder, capture, class 0x48, 3D into swizzled surfaces. Driver limits: 71.84 draws no Direct3D; Alt+Tab from full-screen OpenGL crashes driver 4.11; 45.23 D3D needs VIAGART on VIA boards. |
| **S3 Savage3D** (STB Nitro 3200, AGP/PCI) | ✅ | Win98 SE (DxDiag 6.1, Tomb Raider 2 with S3TC/trilinear), NT4 (PCI card) | NT4 can't use the AGP card (NT4 HAL rejects it, as on real hardware): use the PCI card. Status-bit layout borrowed from the ViRGE, unverified. Not emulated: video capture, TV out. BIOS only in ROMS-EXTRA. Not yet run through CARDTEST. |
| **3Dlabs Permedia 2** (STB GLyder MAX-2+, ELSA WINNER 2000/Office, GLoria Synergy) | 🟡 | Win98 SE (3Dlabs, ELSA, Synergy drivers: DxDiag DX7, 3DMark 99 2050, TR2, GLQuake), NT4, Win2000 inbox, DOS UniVBE | New in 2.1.5: 3D, YUV blits and CARDTEST fixes. **Open (17 Oct CARDTEST runs, not fixed yet):** further rendering bugs with the 3Dlabs-written drivers (Win98 reference driver, Win2000 perm2), and line/ellipse errors on NT4 and Win2000. Hardware limits: no mipmapping, no alpha test, no scan-out overlay. Not modelled: patch address permutation, overlay filtering (nearest only), video capture/TV. 24 bpp untested (no driver offers it). |
| **S3 ViRGE/MX** (STB Symmetry 3D), generic **ViRGE/MX AGP**, **ViRGE/MX+** (Spectrah LD-S500) | ✅ | MX+ on Win95, Win98 SE, NT4 SP6a, Win2000 at 1024×768; GDI exact on Win98/2000/NT4 | New in 2.1.5: CRTC register pairing and 2D lines per the datasheet. Generic MX on Win95/98 shows only a 640×480 window of a larger desktop (S3 BIOS/driver display-switch state, not emulation; Win95 confirmation pending). The STB ROM holds its sign-on screen 20–25 s (the ROM). Not yet run: STB Symmetry 3D in Windows, DDTEST. |
| **Chips & Technologies 65554** (STB Monocle) | 🟡 | Win98 SE built-in driver | Without a panel it shows the CRTC timing, not the panel-cut picture. Interlaced modes halve vertical timings. The driver's 1024×600 entry fails. VBETest's 4-bit tests look wrong (VBETest's doing). Not run through CARDTEST yet. |
| **Alliance ProMotion 6410 / 6422 / AT3D (AT25)** | 🟡 new in 2.1.5 | POST/VESA on all three; Win95, Win98 SE, NT4 with Alliance drivers; DirectDraw and Media Player overlay on the 6422; AT3D 2D | AT3D is 2D only (no 3D engine, no Voodoo Rush). AT3D DirectDraw and NT4 untested. The STB source build's DirectDraw HAL doesn't come up (cause unknown). Video window is nearest-neighbour, interlace shown progressive, 24 bpp untested. ROMs only in ROMS-EXTRA. |
| **SpeedStar 24X** as a WD90C31A-LR BIOS option | 🧪 | — | Not booted in a guest yet. |

## Machines and chipsets

| Addition | Status | Known bugs / limitations |
|---|---|---|
| AGP chipset fixes (incl. SiS) | ✅ | None known. |
| ASUS 486/25 added; some machine-table entries removed | ✅ | None known. |

## Test tooling

| Addition | Status | Known bugs / limitations |
|---|---|---|
| **CARDTEST** (`tools/cardtest`): self-checking DOS VGA/VESA and Win32 GDI/DirectDraw/Direct3D/OpenGL tests, one-click `RUNALL.BAT`, unattended GUI-benchmark runner (3DMark, Final Reality, WinBench, WHQL DCT, SPECviewperf), host tools that read results from VM disks | 🟡 new in 2.1.5 | Run so far on TNT, TNT2 and Permedia 2 (Win98, Win2000, NT4, DOS). Not yet run on Savage3D, ViRGE/MX, 65554, ProMotion, NV3. The unattended GUI runner is still being finished; the RUNALL restart-and-resume path is untested in a guest. A few oracle mismatches are known (GLCHK PERSP needs GL_NICEST; D3DCHK ALPHATST should skip on the P2). Built discs: `F:\VGATESTSUITE`. |

## Not in this release (unmerged branches)

| Branch | What | Status |
|---|---|---|
| `guest-tools` | 86Box Guest Tools: mouse integration and shared clipboard (Win32 and Win16 agents, built-in install media) | ⛔ 🧪 experimental; DOS agent shelved, Linux/OS2 agents only ideas. |
| `isdn-fritz` | AVM FRITZ!Card PnP ISDN adapter | ⛔ 🧪 passes only a host-side harness with AVM's driver library; not tried in a guest. Its rig (`AVM-ISDN-RIG`) keeps the branch build. |
| upstream-PR branches (`nv3-riva128-*`, `speedstar24x-*`, `txp4x-*`, `via-apollo-vpx-tests`, `agp-fixes`) | Work prepared for upstream submissions | ⛔ kept for reference. |
