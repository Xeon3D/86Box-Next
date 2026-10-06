# ⚠️ No support or instructions will be given for this fork.

# This was AI coded... grab an AI to sort it out.

# Do not bug the 86Box devs about this. They have nothing to do with it, other than writing the base code this is messed up with.

---

86Box-Next
==========

**86Box-Next is a fork of [86Box](https://github.com/86Box/86Box)**, the low level x86 emulator for IBM PC systems and compatibles from 1981 to the PCI era. It is merged with upstream 86Box every week. Everything 86Box does, this does too; below is what it adds.

Windows x64 builds are on the [releases page](https://github.com/Xeon3D/86Box-Next/releases). They are static: one `86Box-Next.exe` and one `isp-server.exe`, no DLLs. The ROMs are not included: use the [86Box ROM set](https://github.com/86Box/roms).

Extra features
--------------

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
  * a status page in the browser, where you can hang up calls and change settings.
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
  * SRAM memory card;
  * Intel Series 2 flash card (works with TrueFFS);
  * Adaptec APA-1460 SlimSCSI.

### USB
* VIA and Intel USB 1.1 (UHCI) and USB 2.0 (EHCI) controllers.
* Host device passthrough (libusb, or UsbDk on Windows), including isochronous transfers such as USB audio.
* A connect prompt, a USB menu and an activity trace.

### Input and arcade
* Elo TouchSystems SmartSet serial touchscreen.
* Arcade I/O boards: funworld Photo Play and Merit Megatouch XL/MAXX.

### Other
* Every setting lives in the machine's own `86box.cfg`; there is no global configuration.
* The VM Manager is optional and off by default.
* A new icon set.
* The release (or build number) in the title bar.
* Static Windows builds.

Licensing
---------

86Box-Next, like 86Box, is released under the [GNU General Public License, version 2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html) or later. For more information, see the `COPYING` file in the root of the repository.

The emulator can also optionally make use of [munt](https://github.com/munt/munt), [FluidSynth](https://www.fluidsynth.org/), [Ghostscript](https://www.ghostscript.com/) and [Discord Game SDK](https://discord.com/developers/docs/game-sdk/sdk-starter-guide), which are distributed under their respective licenses.

Credits
-------

* **86Box and its developers** wrote the emulator this is built on: [86Box/86Box](https://github.com/86Box/86Box).
* The COM port modems, the Elo touchscreen and the funworld I/O board come from PeepeeBox.
* The PC Card controller and the Megatouch board come from MegaPPBox.
* The Rockwell ADPCM coder is a port of Peter Jaeckel's fixed-point version in [mgetty/vgetty](https://github.com/Distrotech/mgetty) (GPL).
