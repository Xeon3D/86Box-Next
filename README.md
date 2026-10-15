# ⚠️ No support or instructions will be given for this fork.

# This was AI coded... grab an AI to sort it out.

# Do not bug the 86Box devs about this. They have nothing to do with it, other than writing the base code this is messed up with.

---

86Box-Next
==========

**86Box-Next is a fork of [86Box](https://github.com/86Box/86Box)**, the low level x86 emulator for IBM PC systems and compatibles from 1981 to the PCI era. It is merged with upstream 86Box every week. Everything 86Box does, this does too; below is what it adds.

Windows x64 builds are on the [releases page](https://github.com/Xeon3D/86Box-Next/releases). They are static: one `86Box-Next.exe` and one `isp-server.exe`, no DLLs. The ROMs are not included: use the [86Box ROM set](https://github.com/86Box/roms). The NVIDIA BIOSes are not in it; they are in [Xeon3D/roms](https://github.com/Xeon3D/roms) (`roms/video/nvidia/nv3` and `nv4`), which is the 86Box set plus those.

Extra features
--------------

### NVIDIA graphics cards
* **RIVA 128 and RIVA 128 ZX** (NV3), PCI and AGP:
  * 2D acceleration, DirectDraw and Direct3D with NVIDIA's own Windows drivers;
  * the video overlay (YUY2/UYVY, scaled, colour-keyed) used by video players;
  * VGA, VESA and NVIDIA's extended modes up to 1600x1200, and the RAMDAC's checksum, so NVIDIA's manufacturing test passes.
* **RIVA TNT, TNT2 and TNT2 Ultra** (NV4/NV5), PCI and AGP, with 18 boards' own BIOSes as the card's Model: ELSA ERAZOR II/III/III Pro/III LT/III ULTRA and Synergy II-32, Diamond Viper V550/V770, Creative Graphics Blaster RIVA TNT, STB Velocity 4400, ASUS AGP-V3400TNT and Leadtek WinFast 3D S320 II:
  * 2D acceleration up to 1600x1200 in 32-bit colour;
  * Direct3D (DirectX 5 and 6 triangles, multitexturing) and the **OpenGL ICD**: 3DMark 99 and 2000, Unreal Tournament and Quake III Arena run on NVIDIA's Windows 98 driver;
  * the 3D is drawn on worker threads (1 to 8), or on the **host's GPU** through Vulkan ("Draw 3D on the host GPU" in the card's settings), with the same pixels either way.

#### RIVA TNT / TNT2: what has been tested

Guests ran on Pentium II-class machines (ASUS P6BAP, Celeron 233-266, or a Pentium 233 on Socket 7 for the PCI cards). Benchmark figures are the guest's own (its emulated CPU sets them), not host speed.

| OS | NVIDIA driver | Cards | Works | Does not work / not tested |
|---|---|---|---|---|
| Windows 98 SE | 4.11.01.0115 (reference, 1999) | TNT, TNT2 (not Ultra/M64: the driver does not list them) | 2D up to 1600x1200x32; DirectDraw and its overlay; Direct3D: DxDiag, **3DMark 99 MAX 2620**, **3DMark 2000 1050**, Unreal Tournament demo; OpenGL: **Quake III Arena** (39 fps timedemo), **GLQuake 85 fps**, **Quake II 38.5 fps** | Alt+Tab out of a fullscreen OpenGL game and back crashes the driver (its own 1999 bug, as far as we can tell) |
| Windows 98 SE | 8.05 | TNT, TNT2, TNT2 Ultra, TNT2 M64 | 2D, Direct3D (**3DMark 99: 2714 Ultra, 2717 M64**), overlay and DVD subpicture: Media Player plays MPEG-1 and Indeo 4/5 through the overlay in real time | |
| Windows 98 SE | 71.84 (2005) | TNT, TNT2 Ultra | Desktop, resolution and colour-depth changes, DirectDraw | **Direct3D draws nothing** (3DMark runs to the end on a blank screen): open |
| Windows 2000 | built-in NVIDIA driver | TNT (PCI) | 1024x768x32 desktop, overlay, Media Player MPEG-1, restart | 3D not tested |
| Windows 2000 | 71.84 | TNT2 (PCI) | Installs, desktop | 3D not tested |
| Windows NT 4.0 SP6a | 28.32, 43.45 | TNT (AGP) | 800x600x16 and 1024x768x32 desktops, mode change without reboot, OpenGL (3D Pipes), DirectDraw overlay | Direct3D: NT4 has none |
| DOS | card BIOS (VBE 3.0) | all 18 boards POST; tested in depth: ERAZOR II PCI on a Socket 7 board (Intel TC430HX, Pentium 200) | Text and VGA modes; SciTech VBETest: banked and linear framebuffer, double buffering, virtual-screen scrolling (640x480 and 1600x1200, 16-bit); **Quake 1.01 and 1.06** in every VESA mode from 320x200 to 1280x1024, **timedemo 22 fps at 640x480** | The ELSA BIOS reports 8 MB to VESA programs on a 16 MB card (being checked) |
| Linux | XFree86 `nv`, `rivafb` | | | Not tested |

Not emulated: the TV encoders and video capture (PME), the DirectX 3 triangle class (0x48), 3D drawn into a swizzled surface. Nothing found so far needs them.

Tip: with the **shareware** Quake data GLQuake shows the desktop instead of the game in fullscreen. That is GLQuake, not the card: it cannot read back its palette cache with shareware data, regenerates it behind a dialog on every start, and minimizes itself when that dialog takes the focus. Use the full game's PAK0/PAK1 or `-window`.

### Dial-up, telephone lines and voice calls
* **Modems on COM ports:** Diamond SupraExpress 56e PRO and ELSA MicroLink 56k, with a status bar icon to change the line while the machine runs. The line can be:
  * not connected;
  * a TCP host;
  * isp-server's telephone network.
* **isp-server**, a virtual ISP that runs as a program of its own:
  * dial any number and you get PPP and NAT to the host's Internet;
  * guests on it can reach each other;
  * port forwarding;
  * an optional modem-speed throttle;
  * a status page in the browser, where you can hang up calls and change settings;
  * on Windows, a log window that minimizes to the notification area (no console window).
  * real accounts with PAP, CHAP-MD5, MS-CHAP, MS-CHAP-2 and CHAP-SHA1 to SHA3-512, MPPE encryption, MPPC/Deflate/BSD-Compress/Predictor-1 compression, WINS and Multilink;
  * a status page in tabs (status, phone, port forwards, settings, log, users) with logins for hosting it;
  * Linux, macOS and Docker (`xeon3d/86box-next-isp`) builds too: it has its own repository, [Xeon3D/ISP-Server](https://github.com/Xeon3D/ISP-Server) (here as the `isp-server` submodule: clone with `--recurse-submodules`).
* **Telephone numbers:** every VM's modem gets a number on isp-server's exchange, and VMs can call each other.
  * A call rings the other modem, with caller ID; ATA or auto-answer picks it up, and the two modems connect byte for byte.
  * BUSY and NO ANSWER work as on a real line.
  * Any other number reaches the ISP.
* **Voice calls:**
  * Two voice command sets:
    * Rockwell's, which Windows 9x's Unimodem/V and vgetty use.
    * The ITU's V.253.
  * Playing and recording are supported, including Rockwell ADPCM that matches Rockwell's own encoder bit for bit.
  * Other supported features:
    * DTMF;
    * busy and silence detection;
    * full duplex.
  * Every modem also has a **phone beside it**, which is the host's microphone and speakers. Pick it up from the modem menu to answer, call a number, or join the guest's call.
  * isp-server's page has a **phone** of its own, so you can call a VM from the browser.

### PC Cards (PCMCIA)
* A Cirrus CL-PD6722 controller with two sockets, and cards you can insert and remove while the machine runs, from a PC Card icon in the status bar.
* Cards:
  * 3Com 3C589D Ethernet;
  * TRENDnet TE100-PC16 Fast Ethernet;
  * 3Com 3C562D LAN + 33.6 modem (a multi-function card);
  * Adaptec APA-1460 SlimSCSI.

### USB
* VIA and Intel USB 1.1 (UHCI) and USB 2.0 (EHCI) controllers.
* Host device passthrough (libusb, or UsbDk on Windows), including isochronous transfers such as USB audio.
* A connect prompt, a USB menu and an activity trace.

### Sound
* **Roland Sound Canvas** MIDI output: the boards of [88emu](https://github.com/dsp56300/gearmulator) (SC-55, SC-55mkII, SC-88, SC-88VL, SC-88Pro, SC-8820, SC-8850 and their relatives, the MT-32s and the CM-32L/CM-32P/CM-64), running their own firmware.
  * Its Configure window lists the synthesizers and, for the selected one, every ROM image it needs, with its MD5 checksum and a green lamp for each one found in `roms/soundcanvas` (any file name: known dumps are recognized by their contents) and a red one for each one missing.
  * While the machine runs, the synthesizer's front panel opens in a window of its own: the display, the lamps, the buttons (mouse or keyboard) and the volume knob all work. The piano icon at the left of the status bar brings it back.

### Input and arcade
* Elo TouchSystems SmartSet serial touchscreen.
* Arcade I/O boards: funworld Photo Play and Merit Megatouch XL/MAXX.

### Other
* Every setting lives in the machine's own `86box.cfg`; there is no global configuration.
* The VM Manager is optional and off by default.
* A new icon set.
* The release (or build number) in the title bar.
* Static Windows builds.
* A debug command file (`BOX86NEXT_DEBUG_CMD`) to drive a running machine from scripts: keys, screenshots, breakpoints, memory and device state.

Licensing
---------

86Box-Next, like 86Box, is released under the [GNU General Public License, version 2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html) or later. For more information, see the `COPYING` file in the root of the repository.

The Roland Sound Canvas device includes 88emu from [gearmulator](https://github.com/dsp56300/gearmulator), which is released under the [GNU General Public License, version 3](https://www.gnu.org/licenses/gpl-3.0.html); builds that include it are therefore distributed under GPLv3.

The emulator can also optionally make use of [FluidSynth](https://www.fluidsynth.org/), [Ghostscript](https://www.ghostscript.com/) and [Discord Game SDK](https://discord.com/developers/docs/game-sdk/sdk-starter-guide), which are distributed under their respective licenses.

Credits
-------

* **86Box and its developers** wrote the emulator this is built on: [86Box/86Box](https://github.com/86Box/86Box).
* The COM port modems, the Elo touchscreen and the funworld I/O board come from PeepeeBox.
* The PC Card controller and the Megatouch board come from MegaPPBox.
* The Roland Sound Canvas emulation and its front-panel artwork are 88emu's, by The Usual Suspects ([gearmulator](https://github.com/dsp56300/gearmulator)), building on nukeykt's [Nuked-SC55](https://github.com/nukeykt/Nuked-SC55) and [munt](https://github.com/munt/munt).
* The Rockwell ADPCM coder is a port of Peter Jaeckel's fixed-point version in [mgetty/vgetty](https://github.com/Distrotech/mgetty) (GPL).
