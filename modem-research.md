# Modem research: an internal 56k modem and a USB ADSL modem

Written 2026-10-10 (tasks D and E of the overnight list). Nothing here is implemented yet.

## D. The most compatible internal 56k modem

### What "compatible" rules out

Most internal 56k modems sold after about 1998 are **soft modems** (winmodems: HSP, HCF,
AC'97/MC'97 risers, Lucent/Agere "LT Win", 3Com/USR "WinModem", PCTel). Only Windows drivers
exist for them, and the driver does the modem's work on the host CPU. DOS, Windows 3.x, OS/2,
NT 4 and most Linux/BSD versions cannot use them, and emulating one means emulating its DSP.
That leaves **controller-based** modems: a 16550 UART in front of a complete modem. Every OS
drives these with its standard serial driver plus, at most, an INF file. The modem engine in
`char_modem.c` already is such a modem. A card is only a UART plus the bus glue, the same way
the Hayes Accura and 3C562 PC Cards carry it (`char_modem_attach()`).

### Recommendation: ISA first, PCI second

**1. ISA PnP: Diamond SupraExpress 56i Sp (PnP ID `SUP2171` / `SUP2170`). Recommended.**

- It is the internal sister of the SupraExpress 56e PRO we already emulate: the same Rockwell
  RCV56ACF/SP controller, the same AT command set, the same voice set (`#CLS=8`). It can reuse
  the Supra's ATI table and voice support unchanged. Only the ISA PnP card and a detached
  16550 are new, on the PC Card modems' pattern.
- **Windows 95/98/ME:** the Diamond CD in the owner's Downloads (`Diamond SupraExpress
  56e-i PRO.iso`, `INSTALL\MODEM\33_56\INSTALL\MDMISUPV.INF`) installs it as
  `%Supra4% = Modem4, *SUP2170` / `*SUP2171` ("SupraExpress 56i Sp Intl"). Copies of that
  driver are easy to find online.
- **Linux:** `8250_pnp.c` matches `SUP2171` by ID (along with SUP1310, SUP1381, SUP1421,
  SUP1590, SUP1620 and SUP1760). It is a plain ttyS port, and pppd/wvdial work.
- **DOS, Windows 3.x, OS/2, NT 4:** it is a COM port. With PnP turned off (a jumper on the real
  card; a "PnP / COM1-4 + IRQ" option in our config), it sits at a legacy COM address and needs
  no driver at all.
- **Windows 2000/XP:** neither has `SUP2171` in its inbox INFs (checked in the rig's Win2000
  image). Unimodem's "Standard 56000 bps Modem" or the Diamond INF works.
- Alternative with **inbox drivers in both Win98 SE and Win2000**: Motorola **ModemSURFR 56K
  Internal PnP** (`MOT1550`) / **VoiceSURFR 56K Internal PnP** (`MOT15F0`), `mdmmoto1.inf` in
  both images, and both IDs are in Linux's `8250_pnp.c` too. It is also Rockwell-based (K56flex,
  `#CLS` voice). Its datasheet lists RPI (Rockwell Protocol Interface, host-side V.42), but RPI
  is optional; the INF's strings decide whether Windows uses it, and our engine would simply
  not offer `+H`. If inbox drivers in Win2000 matter more than reusing the Supra's identity,
  pick this one.

**2. PCI: U.S. Robotics 56K Performance Pro / 56K Faxmodem PCI, model 5610 (PCI `12B9:1008`).**

- It is one of the few PCI modems that are not soft modems. All modem work happens on the
  card. To the host it is a single **16550-compatible UART**: PCI class 07h, subclass 00h,
  prog-if 02h ("16550-compatible"). NetBSD attaches it as an `ns16550a` with a working FIFO.
- **Linux** (and the BSDs) drive it with the generic PCI serial driver. Its class code is
  enough for `8250_pci`'s fallback match on serial-class devices with one 8-byte I/O BAR.
- **Windows 95/98/ME/NT 4/2000/XP:** USR's own driver (`usr5610.inf`, also `usr2973.inf` for
  the same ID; still on usr.com and in driver archives). Win9x puts it on COM5. Neither the
  Win98 SE nor the Win2000 image has an inbox INF for it; Win2000's only USR PCI ID is
  `12B9:1006`, a 3Com/ADI *WinModem*.
- **DOS, Windows 3.x, OS/2:** not usable. A PCI UART is not at a legacy COM address, and those
  systems have no driver that finds it.
- Emulation is easy: PCI config space + one I/O BAR + a detached 16550 + the modem engine.

**Verdict:** to cover the most OSes, build the **ISA SupraExpress 56i Sp** (DOS through XP and
Linux, and it reuses the Supra we already have), with a jumper option for non-PnP COM1-4. Add
the **USR 5610 PCI** later for boards without ISA slots (Win2000/XP and Linux guests).

A side finding: the same Diamond INF lists the external 56e as **serial PnP**
(`SERENUM\SUP2150`, `SUP2151`, `SUP2140`, `SUP2141`, "SupraExpress 56e Intl Plug & Play").
If our COM-port Supra answered the serial PnP enumeration (the PnP ID string sent when DTR/RTS
toggle), Win9x/2000 would find and install it by itself. That would be a small, separate
improvement.

## E. Emulating a USB ADSL modem

### Which model: Alcatel / Thomson SpeedTouch USB (the "green frog"), USB `06B9:4061`

| Candidate | Chip | Drivers | Effort |
|---|---|---|---|
| **Alcatel SpeedTouch USB / 330** | Alcatel + ARM, host-loaded firmware | Win98/98SE/ME/2000/XP (Alcatel/Thomson and many ISP-branded packages), Mac OS 8.6/9/X, Linux (`speedtch`, in the kernel since 2.6), FreeBSD/NetBSD (ports: `net/pppoa`, the open `modem_run` / speedtouch.sf.net userspace driver) | **Lowest:** documented protocol, simple status requests |
| Analog Devices Eagle (Sagem F@st 800/840, USR 9000...) | ADI Eagle DSP | Windows, Linux `ueagle-atm` | High: the host streams DSP code pages on demand all the time; we would have to answer its DSP protocol |
| Conexant AccessRunner (Zoom, D-Link DSL-200 A, many OEMs) | Conexant | Windows, Linux `cxacru` | Medium: firmware upload plus a command/response protocol, fewer documents |

The SpeedTouch has the widest OS coverage, the best public documentation (the kernel driver and
the open userspace driver), and a trivial control path.

### What the emulated SpeedTouch has to do (from Linux's `speedtch.c` and `usbatm`)

- Descriptors: vendor-specific class, interface 1 (data: bulk alt 1, iso alt 3), interface 2
  (firmware), serial-number string = 12 hex digits (the "ESI", used as the MAC address).
- **Firmware:** the host writes stage 1 and stage 2 to bulk OUT `0x05` and reads 512-byte
  blocks from bulk IN `0x85`. We do not run the ARM firmware: we accept and discard it, return
  plausible 512-byte blocks, then act "booted" (`0x07` control read returns 1 byte = firmware
  already loaded). **Risk:** the Windows driver may check what those reads return. That needs
  a USB trace of the real Windows driver (the passthrough trace, `BOX86NEXT_USB_TRACE`, on a
  real modem would give it).
- **Control:** vendor OUT request `0x01` (config writes: wValue `0x0b`, `0x02`, `0x03`, `0x04`,
  `0x11` ModemMode, `0x14` options, `0x12` BMaxDSL); vendor IN request `0x12` wValue `0x07` =
  line state (`0x00` down, `0x10` training, `0x20` up), wValue `0x0b` = rates (down/up,
  32-bit LE, kbit/s), wValue `0x04` = "start sync" prod. The interrupt IN `0x81` sends
  `a1 00 01 00 00 00` for line up and `a1 00 00 ...` for line down. A "Line" status bar item
  could pull the plug.
- **Data:** bulk `0x07` OUT/IN carries raw 53-byte ATM cells (5-byte header: VPI/VCI/PTI/CLP/HEC,
  48 bytes payload). The emulation does **AAL5 SAR**: reassemble cells per VPI/VCI into AAL5
  PDUs (pad + 8-byte trailer, CRC-32), and segment replies back into cells with a correct HEC.
  Rate-limit to the reported sync rate (e.g. 8128/832 kbit/s ADSL1) so the guest sees a
  plausible line.
- 86Box-Next side: an emulated `usbn_device_t` (`src/include/86box/usb_next.h`: `packet()` per
  token, `frame()` per ms, `reset`, `destroy`). Our USB code has only host passthrough so far,
  so this would be the first emulated device. A small control-endpoint state machine (SETUP /
  DATA / STATUS, descriptors) is reusable for later devices. It needs the USB 1.1 (UHCI) card,
  which Win98 SE already uses.

### What payload the guest sends, and what isp-server has to change

ISPs ran two encapsulations; Windows drivers ask which one at install time:

- **PPPoA** (RFC 2364), usually VC-mux on VPI/VCI 0/38 or 8/35: each AAL5 PDU is **one PPP
  frame**: no HDLC flags, no byte stuffing, no FCS (AAL5's CRC does that job). LLC-encap PPPoA
  adds `FE FE 03 CF` in front.
- **PPPoE over RFC 2684 bridged Ethernet** (LLC/SNAP `AA AA 03 00 80 C2 00 07 00 00` + an
  Ethernet frame): PPPoE discovery (PADI/PADO/PADR/PADS/PADT, RFC 2516), then PPP in session
  frames. MRU 1492.

isp-server's PPP already runs on frames: `ppp_session.c` takes whole frames (`ppp_input()`),
and only `isp.c` deals with HDLC (`ppp_rx_feed()` / `ppp_encode()`). The changes:

1. **A frame transport for sessions** (`isp.c`): next to today's HDLC byte stream, a
   "packet" mode that passes frames through unencoded (PPPoA). ACCM negotiation becomes moot:
   accept and ignore it, as pppd's PPPoA plugin does. LCP MRU defaults to 1500 (PPPoA) or 1492
   (PPPoE).
2. **A way in for an always-on line:** an exchange greeting variant, e.g. `86BOX-EXCHANGE 1
   ADSL <vpi>/<vci> <encap>`, opening a session with no dialling. The connection then carries
   length-prefixed PDUs (AAL5 payloads) instead of a byte stream, because TCP has no record
   boundaries. The SAR stays in the emulator; isp-server sees PDUs.
3. **A PPPoE server** (new, `ppp_pppoe.c`): discovery with an AC-Name / Service-Name,
   session IDs, PADT on hang-up, plus the bridged-Ethernet LLC header. A guest whose driver
   runs PPPoE (Win2000/XP's built-in PPPoE, RASPPPoE on Win98, rp-pppoe on Linux) needs it;
   PPPoA guests do not.
4. **"Calls" without a phone number:** the status page lists the session as "ADSL 8/35
   PPPoA", the call number column becomes the line's ESI/MAC, and port forwards key on that.
   Multilink is off for these sessions. Authentication, CCP/MPPE, NAT and the guest LAN are
   unchanged.
5. **Settings:** VPI/VCI and encapsulation the ISP expects (a guest configured wrong gets no
   answer, as with a real ISP), and the advertised sync rates.
6. **Tests:** PPPoA and PPPoE against Linux's pppd in WSL (`pppd plugin pppoatm.so` needs an
   ATM device, so test the PDU transport directly; `pppd plugin rp-pppoe.so` over a veth +
   a small LLC bridge shim). This would extend `pppd_interop.sh`.

### Suggested order

1. A USB trace of the real Windows SpeedTouch driver (firmware upload reads, status polling),
   if a SpeedTouch and the passthrough are at hand. Without it, start from the Linux driver's
   sequence and adjust on the first Windows failure.
2. isp-server: the frame transport + the ADSL greeting + PPPoA (small).
3. The emulated SpeedTouch with AAL5, tested with Linux's `speedtch` first (open source, the
   easiest to debug), then the Windows 98 SE driver.
4. PPPoE in isp-server, for Win2000/XP's own PPPoE and bridged-mode drivers.

## Sources

- Linux `drivers/usb/atm/speedtch.c`:
  <https://github.com/torvalds/linux/blob/master/drivers/usb/atm/speedtch.c>
- Linux `drivers/tty/serial/8250/8250_pnp.c`:
  <https://github.com/torvalds/linux/blob/master/drivers/tty/serial/8250/8250_pnp.c>
- SpeedTouch Linux driver docs: <https://linux-usb.sourceforge.net/SpeedTouch/docs/howto.html>;
  DSL HOWTO appendix: <https://www.linuxdoc.org/HOWTO/DSL-HOWTO/speedtouchusb.html>;
  FreeBSD handbook PPPoA: <https://docs-archive.freebsd.org/doc/6.2-RELEASE/usr/share/doc/handbook/pppoa.html>
- USR 5610 as a 16550 PCI serial device: NetBSD port-i386
  <https://mail-index.NetBSD.org/port-i386/1999/11/21/0007.html>,
  tech-net <https://mail-index.netbsd.org/tech-net/2000/01/24/0000.html>, LKML
  <https://lkml.indiana.edu/0001.2/0422.html>; USR 5610B guide
  <https://support.usr.com/support/5610b/5610b-ug/five.html>; 8250_exar USR298x patch
  <https://lkml.iu.edu/hypermail/linux/kernel/2304.2/05718.html>
- Motorola VoiceSURFR/ModemSURFR 56K internal specs: <https://th99.classic-computing.de/t/M-O/53719.htm>,
  <https://th99.classic-computing.de/t/M-O/53721.htm>
- Local checks: the inbox modem INFs of the rigs' Windows 98 SE and Windows 2000 images (read
  only), and the Diamond SupraExpress 56e-i PRO CD's `MDMISUPV.INF`.
