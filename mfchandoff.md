# Handoff: multi-function PC Cards (PC Card 95 MFC)

Started 2026-10-05 on branch `pcmcia-mfc`. Read `CLAUDE.md` first; `flashhandoff.md` has the
guest-test setup (pcictest copy, `flash-investigation\tools\run_guest.ps1`, `infgrep.py`,
`fat32.py`/`fatput.py`, `creg.py`) and the VxD tooling (`le3.py`, `dumpobj.py`, `dis.py`,
`stackmatch.py`).

## Done

- `src/pcmcia/pccard_mfc.c` + `pcmcia.h`: the framework. A card fills a `pccard_mfc_t` with its
  CIS and up to four `pccard_func_t` (I/O length, I/O handlers taking the offset into the
  function's range, reset, enable), each with the attribute address of its configuration
  registers, and inserts `&m->card`. Per function: COR (SRESET, level, index, IREQ enable,
  address decode, function enable), CCSR (interrupt pending = the function's request, read-only
  bit), PRR/SCR/ESR kept, I/O base 0-3, I/O limit. I/O goes to the first enabled function whose
  range holds the port (by the I/O base registers with address decode on, else by the low
  address lines). IREQ = OR of the functions' requests that their COR enables; `pccard_mfc_irq()`.
- `pccard_cis.c`: `pccard_cis_longlink_mfc()` / `pccard_cis_mfc_link()` (the function's chain
  address coded as its real attribute address, 2 x the byte index), `pccard_cis_linktarget()`,
  `pccard_cis_cftable_io()` (any index, 8-bit-only or 8/16, base 0 = anywhere).
- `tests/pcmcia/mfc_test.c` (40 checks): a 3CXEM556-shaped two-function card; its CIS read
  back through the card's attribute memory and walked per function by cistpl.c's rules; COR,
  CCSR, I/O base/limit, decode on and off, the shared IREQ, SRESET, card reset.
  Build: `gcc -std=gnu11 -Isrc/include -Ibuild-static/src/include tests/pcmcia/mfc_test.c
  src/pcmcia/pccard_mfc.c src/pcmcia/pccard_cis.c` (UCRT64).

## How Windows 98 handles an MFC card (PCCARD.VXD obj3 offsets)

- Tuple walk: request packet Socket word = socket | function << 8. `0x36fc` handles link
  tuples; LONGLINK_MFC (`0x37cd`) follows **only the requested function's entry** (needs the
  tuple to be >= 5f+6 long and its count > f), like Linux. So a function sees the primary
  chain, then its own chain.
- Link address: an attribute-space MFC link (space 0) is doubled (taken as a byte index), then
  `0x35ce` looks for LINKTARGET "CIS" there and, failing that, at half (`0x3638`): either coding
  works, as in Linux (which tries them in the same order).
- IDs: the parent is `PCMCIA\<s1>-<s2>-<CRC>` as for a single-function card (`0xbd98`, CRC from
  `0xbbxx` walking with the record's function `[rec+0x1f]`; function records are numbered 0..n-1
  at `0x5838`, so most likely function 0: CRC over the primary chain + function 0's chain --
  to be confirmed in the guest). Each child is the parent's ID with `-DEV<n>` put before the
  `-<CRC>` (`0xc364`); the parent also gets the compatible ID `*PCMCIA\MFC` (obj1+0x94).

## First card: 3Com 3C562D/3C563D EtherLink III LAN+33.6 Modem

Windows 98 has drivers for both functions in the box:

- LAN: `NET3C562.INF`, `PCMCIA\3COM_CORPORATION-3C562D/3C563D-DEV0-E4C0` (also older
  3C562/B/C IDs), driver `elpc3r.sys` (NDIS 3). The INF carries an `Override` logical
  configuration: I/O 16 ports, 16-aligned, 0x100-0xFFFF; IRQ any; memory 4 KB (a window on
  attribute memory, presumably for the station address tuple); and a PC Card record =
  RequestConfiguration's fields: Vcc 5.0 V, interface memory+I/O (2), **ConfigBase 0x1800**
  (0x1080 in the 3C562/B/C override), Status 0, Pin 0, Copy 0, **COR 0x47** (index 7 = function
  enable + address decode + IREQ enable, level IRQ), **Present 0x23** (COR, CCSR, I/O base 0).
- Modem: `MDMGATEW.INF`, `PCMCIA\3COM_CORPORATION-3C562D/3C563D-DEV1-E4C0` = "3Com (3C562) EL
  III LAN+33.6 Modem PC Card" (Modem8), no override: from its own CIS chain.
- The ID's CRC E4C0 is shared; make it with the CONFIG filler subtuple as for the other cards
  (in function 0's CONFIG tuple, if the CRC does cover function 0's chain).

From the BSD/Linux drivers: the 3C562 is an EtherLink III (the 3C589's chip; `net_3c509b.c`'s
PC Card mode); its station address is in a vendor CIS tuple **0x88** (three words, each byte
pair swapped -- NetBSD "3c562a-c magic"), not only the EEPROM; the LAN decodes A7 too (Linux
puts it at xx00-xx7f); the LAN COR's bit 3 is a "serial disable" bit that drivers clear
(Linux serial_cs: `ConfigIndex &= ~0x08` for the modem). No real 3C562 CIS dump was found
(pcmcia-cs `etc/cis` has the 3CXEM556 / 3CCFEM556, which are PC Card 95 MFC with registers at
0x800/0x900 resp. 0x1000/0x1100).

## Next steps

1. LAN function: let `net_3c509b.c`'s PC Card model run as a `pccard_func_t` (offset-based I/O,
   IRQ through `pccard_mfc_irq`), MAC also in CIS tuple 0x88.
2. Modem function: a 16550 not on a COM port. `serial.c` ties a UART to `com_ports[]` and raises
   its IRQ with `picint_common`; add (86Box-Next block) a detached mode: no I/O handler, the
   card calls `serial_read`/`serial_write` with the offset, IRQ through a callback. Attach the
   modem backend (`char_modem.c`, `char_init(&serial->char_port, device, instance)`); decide its
   config instance/section and how the modem status bar icon sees it.
3. `pccard_cis_3c562d()`: primary (DEVICE none, VERS_1 "3Com Corporation", "3C562D/3C563D", ...,
   MANFID 0101:0562, FUNCID multi, LONGLINK_MFC 2), function 0 (FUNCID network, CONFIG base
   0x1800 present 0x23, CFTABLE 16 ports, tuple 0x88), function 1 (FUNCID serial, CONFIG,
   CFTABLE 8 ports); CRC E4C0. Card in `pcmcia.c`'s list (network: yes).
4. Guest test in `pcictest`: device manager shows both; registry IDs (`creg.py`); network up;
   the modem answers AT (HyperTerminal or a batch `ECHO ATI3 > COMx`).
