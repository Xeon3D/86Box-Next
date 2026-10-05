# Handoff: multi-function PC Cards (PC Card 95 MFC)

Started 2026-10-05 on branch `pcmcia-mfc` (not merged, not pushed). Read `CLAUDE.md` first;
`flashhandoff.md` has the guest-test setup (pcictest copy, `flash-investigation\tools\run_guest.ps1`,
`infgrep.py`, `fat32.py`/`fatput.py`, `creg.py`) and the VxD tooling (`le3.py`, `dumpobj.py`,
`dis.py`, `stackmatch.py`).

## Status (2026-10-05, build 67-69)

**The 3Com 3C562D LAN+33.6 Modem card works as a multi-function card in Windows 98 SE.**
Windows found both functions by their INF IDs on the first try and installed both drivers
from its own CABs: "3Com (3C562D-3C563D) EtherLink LAN+336 Modem PC Card" (NET3C562.INF,
elpc3r.sys) and "3Com (3C562) EL III LAN+33.6 Modem PC Card" (MDMGATEW.INF). Windows put the
LAN at 110h-11Fh (socket B I/O window 0) and the modem at 2E8h-2EFh (window 1, COM4).
- **LAN: works**: `PING 10.0.2.2` over SLiRP, 3/3 replies. (Socket A also holds a 3C589D and
  the machine an RTL8139, so it is not yet proven which adapter answered; check with
  `WINIPCFG` or remove the others.)
- **Modem: works on the card side; the guest-side check is unfinished.** A trace of the
  modem function's register accesses (temporary pclog in `c562_modem_read/write`, since removed)
  shows Windows' serial driver probing the 16550, then a test program's `ATI4` going out and
  "ATI4", CR LF, "3Com EtherLink III LAN+33.6 Modem PC Card"... coming back through RBR byte by
  byte with the right LSR/IIR. But the test program (`ATPROBE.EXE`, below) got that many bytes
  from `ReadFile`, **all 00h**. Without its own `SetCommState` it gets nothing at all (Windows'
  default DCB holds transmission: only the driver's two probe bytes reached THR). **Cause found:
  the probe itself** -- `gcc -m32` emitted CMOVcc (P6 only) and the test machine is a Pentium
  MMX: Windows reported "invalid instruction 0F 45" in ATPROBE. `build.sh` now uses
  `-march=pentium-mmx` (no cmov in the binary); DCB restored, hex logging kept. **Rerun: still
  zeros** -- e.g. ATI0 gives 20 bytes (the right count: echo + 33600 + OK), every one 00h, via
  ReadFile (SetCommState ok; COM4's DCB before it: flags 3011h, baud 0). So the zeroing is
  between the UART (right bytes in RBR) and ReadFile. Next: the same probe on a COM2 modem
  (COM port, not the card) to tell a card/UART-detached bug from a Windows/probe one.
- WINIPCFG now works (`WINIPCFG /BATCH C:\IPCFG.TXT`): three Ethernet adapters, adapter 2 has
  10.0.2.15 from SLiRP; pings answer. Only socket B (the 3C562D) has a link now (socket A
  emptied, [Network] has no cards linked): **the 3C562D's LAN is proven.**
- Comparison run: `pcictest\86box.cfg` got `[Ports (COM & LPT)] serial1_device = modem_supra`
  (backup before it: `86box.cfg.mfc-bak`). The same probe on COM1 reads the Supra perfectly
  ("AT | OK", "SupraExpress 56e PRO"...), COM4 (the card) still all 00h. **So the bug is on the
  card side** (the detached UART / PC Card path), even though RBR read back right in the
  build-69 register trace. Remove the COM1 modem from the cfg when done.
- **MDMGATEW.INF overrides the modem function too** (`ADDREG_3COMA.reg`, Override 0000-0004):
  8 ports, 8-aligned, at 3E8h / 2E8h / 3F8h / 2F8h or anywhere; IRQ any; PC Card record:
  **ConfigBase 0x1900, COR 0x47, Present 0x23** (I/O base 0 only: the modem also compares
  A7-A0). Our CIS/`pccard_mfc_add` had the modem at mask 63h (A15-A0 against base 0+1, but
  Windows writes only base 0): fixed to 23h / index 7 (build 70; mfc_test updated, 52 pass).
  **Still zeros.** A register trace (build 71, temporary pclog in `c562_modem_read/write`)
  with the fixed probe shows Windows' serial driver getting exactly the right bytes from RBR
  (`ATI4` echo, "3Com EtherLink III LAN..."), with IIR C4h/C1h, LSR 61h/60h and an ISR-like
  sequence (IIR, RBR+LSR pairs, IER 0Fh -> 0Dh). So the card and UART deliver; the bytes become
  00h **inside Windows**, between serial.vxd and ReadFile, only for the card's COM4 (COM1's
  Supra modem reads fine with the same probe).
  Leads for next time:
  - COM4's DCB at open has baud 0 (COM1: 1200): MDMGATEW's sections may lack the usual `DCB`
    value; compare the two ports' registry entries (`creg.py`, Enum\PCMCIA\...DEV1-E4C0 and the
    modem class key: PortDriver Serial.vxd, Contention *vcd, DeviceType 03).
  - Whether the bytes really come through serial.vxd's ISR (the shared, level PC Card IRQ):
    log the guest CS:EIP of the RBR reads (as build 58 did for power writes: `ss+ESP`,
    `mmutranslate_noabrt`, `cpu.h`) and match it with `stackmatch.py` against SERIAL.VXD.
  - Try HyperTerminal on COM4 by hand (the owner can), or Unimodem's "More Info"
    (Control Panel > Modems > Diagnostics), which reads ATI answers itself.
- The owner removed the 3C589D from socket A, so a ping now proves the 3C562D's LAN.
- Not yet done: the modem in the PC Card status bar / COM modem status icon (it is "unlisted",
  see below); CLAUDE.md; merge; restage the rigs.

## What was built

- `src/pcmcia/pccard_mfc.c` + `pcmcia.h`: the framework. A card fills a `pccard_mfc_t` with its
  CIS and up to four `pccard_func_t` (I/O length, I/O handlers taking the offset into the
  function's range, reset, enable), each added with the attribute address of its configuration
  registers **and their mask from its CONFIG tuple**, and inserts `&m->card`. Per function: COR
  (SRESET, level, index, IREQ enable, address decode, function enable), CCSR (interrupt pending
  = the function's request, read-only bit), PRR/SCR/ESR kept, I/O base 0-3, I/O limit. I/O goes
  to the first enabled function whose range holds the port: with address decode on, by I/O
  base 0/1 (A15-A0) -- or **A7-A0 only when the mask has no I/O base 1** (the 3C562 LAN; hence
  Linux's xx00-xx7Fh) -- else by the low address lines. IREQ = OR of the functions' requests
  their COR enables; `pccard_mfc_irq()`.
- `pccard_cis.c`: `pccard_cis_longlink_mfc()` / `pccard_cis_mfc_link()` (chain address coded as
  its real attribute address, 2 x the byte index), `pccard_cis_linktarget()`,
  `pccard_cis_cftable_io()` (any index, 8-bit-only or 8/16, base 0 = anywhere); the Windows 9x
  checksum walk (`crc_walk()`) now follows LONGLINK_MFC to function 0's chain, as PCCARD.VXD
  does (confirmed: the 3C562D's IDs matched); `pccard_cis_3c562d()`.
- `src/pcmcia/pccard_3c562.c` (new, `3c562d`, in `pcmcia.c`'s list as a network card): the card
  device. Makes the LAN with `threec562_lan_device.init()` in its own device context (so the
  MAC and the network link are the card's settings), the modem as a detached 16550 with the
  modem engine on it, builds the CIS, inserts the MFC card. Settings: MAC, and the modem's line
  / host / port / connect rate (default 33600) / speaker, the same names `char_modem.c` reads.
- `net_3c509b.c` (upstream file, 86Box-Next blocks): `BOARD_PCCARD_MFC` (0x562),
  `threec562_lan_device` ("3c562d_lan", in no list; EEPROM `eeprom_3c562d_lan_<socket+1>.nvr`),
  product ID 9562h (by the family's numbering, not checked against a real card), 10BASE-T;
  `el3_mfc_function()` hands the card its `pccard_func_t` (I/O by offset, reset = global reset)
  and the MAC; its IRQ goes to `pccard_mfc_irq()` instead of the socket.
- `serial.c` / `serial.h` (upstream, marked): `serial_init_detached()` -- a UART with no
  com_ports slot and no I/O handler, the owner calls `serial_read`/`serial_write` (now declared)
  with the offset, its interrupt through `irq_func` (`serial_do_irq` hands it over);
  `serial_reset_detached()`, `serial_close_detached()`.
- `char.c` / `char.h` (upstream, marked): `char_open_unlisted(port, device)` -- a char device
  on a port outside the device list (hot-plugged cards live outside it), in the caller's
  device context; the owner calls `device->close()`.
- `char_modem.c` (86Box-Next's): `MODEM_MODEL_3C562` ("3Com 3C562D LAN+33.6 Modem", ATI answers
  plausible but invented) and `char_modem_3c562_device` with `MODEM_UNLISTED`: such a modem
  takes no `modems[]` slot (that table is by COM port, for the status bar icon).
- Tests: `tests/pcmcia/mfc_test.c`, 52 checks: the framework on a 3CXEM556-shaped card, and the
  3C562D's CIS (layout per the INF, tuple 88h, parent ID `PCMCIA\3Com_Corporation-3C562D/3C563D-E4C0`,
  the LAN's A7-A0 decode). cis_test (117) and the others still pass. Build:
  `gcc -std=gnu11 -Isrc/include -Ibuild-static/src/include tests/pcmcia/mfc_test.c
  src/pcmcia/pccard_mfc.c src/pcmcia/pccard_cis.c` (UCRT64).

## How Windows 98 handles an MFC card (PCCARD.VXD obj3 offsets)

- Tuple walk: request packet Socket word = socket | function << 8. `0x36fc` handles link
  tuples; LONGLINK_MFC (`0x37cd`) follows **only the requested function's entry** (needs the
  tuple to be >= 5f+6 long and its count > f), like Linux. So a function sees the primary
  chain, then its own chain.
- Link address: an attribute-space MFC link (space 0) is doubled (taken as a byte index), then
  `0x35ce` looks for LINKTARGET "CIS" there and, failing that, at half (`0x3638`): either coding
  works, as in Linux (which tries them in the same order).
- IDs: the parent is `PCMCIA\<s1>-<s2>-<CRC>` (`0xbd98`), the CRC from a walk with function
  record 0 (records numbered 0..n-1 at `0x5838`): **the primary chain + function 0's chain**
  (confirmed in the guest). Each child is the parent's ID with `-DEV<n>` put before the
  `-<CRC>` (`0xc364`); the parent also gets the compatible ID `*PCMCIA\MFC` (obj1+0x94).

## The 3C562D

- LAN: `NET3C562.INF`, `PCMCIA\3COM_CORPORATION-3C562D/3C563D-DEV0-E4C0`, driver `elpc3r.sys`
  (NDIS 3, already in `C:\WINDOWS\SYSTEM` of the test image). The INF carries an `Override`
  logical configuration: I/O 16 ports, 16-aligned, 100h-FFFFh; IRQ any; memory 4 KB (a window on
  attribute memory, presumably for the station address tuple); and a PC Card record =
  RequestConfiguration's fields: Vcc 5.0 V, interface memory+I/O (2), **ConfigBase 0x1800**
  (0x1080 in the 3C562/B/C override), Status 0, Pin 0, Copy 0, **COR 0x47** (index 7 = function
  enable + address decode + IREQ enable, level IRQ), **Present 0x23** (COR, CCSR, I/O base 0).
- Modem: `MDMGATEW.INF`, `...-DEV1-E4C0` (Modem8), no override: from its own CIS chain (ours:
  registers at 1900h, mask 63h, configuration 27h, eight ports, 8-bit, any base).
- From the BSD/Linux drivers: station address in vendor tuple 88h (byte pairs swapped); the
  LAN decodes A7 too; the LAN COR's bit 3 is a "serial disable" bit drivers clear (not
  emulated: our functions have separate CORs). No real 3C562 CIS dump was found; ours is built
  from the INFs and drivers.
- A network adapter install makes the network login (Client for Microsoft Networks) the
  primary logon again, so the test copy asks for the login again after it; the runner's
  screen check presses Enter.

## Guest test kit (flash-investigation\tools)

- `pcictest\86box.cfg`: `socket_b = 3c562d`, `socket_b_net_type = slirp` (the flash card config
  is in `86box.cfg.flash-bak`). `pcictest\b67.exe` = build 67 (normal), `trace.exe` = build 69
  (PCIC log + the temporary modem register log).
- `MFCTEST.BAT` (in the guest's StartUp now -- remove when done): waits 30 s, `WINIPCFG /BATCH`,
  `PING -n 3 10.0.2.2`, `C:\WINDOWS\ATPROBE.EXE`, all into `C:\MFCLOG.TXT`, then shuts down.
  (`WINIPCFG /BATCH` wrote nothing; check its syntax.) `WAITOFF.BAT`: wait 2 min, shut down.
- `atprobe\`: `ATPROBE.EXE` for Windows 9x built without a C runtime: `gcc -m32`, `ld -m i386pe`,
  a kernel32 import library made by `dlltool -m i386 --as-flags=--32` from `kernel32.def`
  (`build.sh`). Opens COM1-COM8, sends AT/ATI0/ATI3/ATI4, logs replies (and now each ReadFile in
  hex) to `C:\ATPROBE.TXT`. The current source has the DCB/SetCommState removed -- put it back
  (9600 8N1, fBinary, DTR/RTS enable, no flow control).
- `fatput.py` now writes files of any size (a cluster chain); the root directory can't be named,
  use `WINDOWS`.
- Pitfall: Python code passed through a Bash heredoc loses `\\`-escapes (`\n` became a real
  newline inside C strings twice). Edit C/BAT files with the Edit/Write tools, or Python files
  written with Write.

## Next steps

1. Settle the probe's zero bytes (restore its SetCommState; read its hex log). If ReadFile really
   returns zeros while RBR gives the right bytes, compare with a COM-port modem (Settings >
   Ports, COM2 = a modem) under the same probe.
2. Prove the LAN is the 3C562's (take the 3C589D out of socket A for the test, or WINIPCFG).
3. Optional: show the PC Card modem in the modem status bar icon (`char_modem.c`'s `modems[]`
   is by COM port; a PC Card modem has none -- extend the registry with socket entries).
4. CLAUDE.md (PCMCIA paragraph: MFC framework, the 3C562D, detached UART, unlisted char
   devices), commit, merge into master, build, restage all rigs (`tools/stage-rig.sh`).
