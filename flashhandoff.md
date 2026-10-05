# Handoff: PC Card memory cards under Windows 98 + TrueFFS

Sessions of 2026-10-04/05. **Status: done** -- the flash card works in the guest end to end
(build 65, merged into `master`). Read `CLAUDE.md` first (repo layout, build, the per-OS rigs
in `F:\Claude\86Box-Next\Latest`). The first session's notes are summarised at the end; several
of its conclusions were wrong and are corrected here.

## Bottom line

**The SRAM card was never the bug.** TrueFFS-9x 3.2 (TRUEFFS.PDR, TFORMAT 3.3.6) *cannot
format a blank SRAM card, on real hardware either*. TFORMAT only lays FTL onto media that one
of TrueFFS's *flash* MTDs has identified, and every flash MTD rejects RAM on purpose. The only
SRAM support in TrueFFS is a plain-FAT translation layer that mounts an SRAM card that
*already* holds a DOS boot sector. The emulated SRAM card behaves like a real one: blank →
"1024 KBytes, write-protected" (the read-only fallback). Nothing to fix there, and the owner
does not want pre-formatted cards.

What TFORMAT is made for is a **linear flash card**, so one was added: an
**Intel Series 2 flash card (28F008SA)**, `src/pcmcia/flash_card.c`. Its CIS gives Windows
`PCMCIA\MTD-A289`, which TRUEFFS.INF maps to TrueFFS, and its chips answer TrueFFS's Intel
MTD exactly as the real parts do (checked by unit tests that port the MTD's routines from
the disassembly). **In the guest (build 65, normal build): TFORMAT formats a blank 4 MB card
(3798 KB formatted), COPY/FC/TCHECK pass ("No errors were found"), and after a reboot the
card mounts with its files, reads back and takes new writes.** The two "open bugs" of the
second session were not emulator bugs (see "Resolution").

## How TrueFFS-9x 3.2 handles a card (TRUEFFS.PDR obj1 offsets)

TRUEFFS.PDR is C++ (thiscall, vtables). **Its LE pages are 512 bytes**: the old
`tools\le2.py` hardcoded 4096, so its fixup offsets were wrong (object data was right).
A fixed loader is `PS = U(h+0x28)` in place of 4096 (this session's copy:
scratchpad `le3.py`; worth copying into `flash-investigation\tools`).

- **MTD registration** (`0xd86b`): six factories, put on a list head-first, so they are
  tried in reverse: `0xc8f5` (ctor `0xc616`, something M-Systems/DiskOnChip-ish),
  `0xc370` (ctor `0xc103`, **CFI**: 0x98 at 0xAA, "QRY"), `0x7d00` (ctor `0x7d3d`, NAND,
  0x2048), `0x7cb9` (ctor `0x7120`, **Intel**), `0x70e0` (ctor `0x6987`, **AMD/Fujitsu**,
  JEDEC 01xx/04xx), `0x6814` (ctor `0x61e0`, **Card Services MTD**).
- **`isRAM`** (`0x4c99`) = OSAK's: read b, write `b ? 0 : FF`, read back, restore. The
  "00/FF at address 0, six times" pattern in the old traces is this, called by each MTD.
  The shared Intel/AMD ID probe (`0x4806`) and CFI return at once if `isRAM`.
- **`flIdentifyFlash`** (`0x4d0b`): first MTD whose constructor leaves status (+8) 0 wins;
  otherwise a bare base `Flash` (vtable 0x228) whose write/erase just fail: the read-only
  1 MB default. That is TFORMAT's "1024 KBytes ... write-protected".
- **Mount** (`0xec80`): identify; NFTL if flash+0x24 bit 0x10, else **FTL** on whatever was
  identified (the 64-byte reads every 64 KB over 1 MB in the old traces = FTL scanning unit
  headers on the 1 MB default). If FTL fails: **RAM/ROM FAT layer** (`0xef30`, vtable 0x318):
  `isRAM` → writable; needs `0xAA55` + partition table or a boot sector (E9/EB..90), size
  from the BPB. Else a dummy error layer (`0x4f40`). Media change (`0xd07f`): RAM counts as
  valid media even unmounted.
- **TFORMAT** formats from user mode through `\\.\TRUEFFS` DeviceIoControl (erase/write on
  the identified flash). No RAM path ("Erasing:", "Building unit map", "FAT12/16" strings).
- **Card Services MTD** (`0x61e0`): GetFirstRegion/GetNextRegion (CS fn 6/9; the old notes
  said TrueFFS never calls them because the function number is in a variable), then
  OpenMemory/ReadMemory/WriteMemory (0x18/0x19/0x24) through *another* MTD registered with
  Card Services. Needs region attribute bit **0x1000** (set by `RegisterMTD`: SRAMMTD.VXD
  registers with exactly 0x1000), **BlockSize > 1**, **PartMultiple ≠ 0**, **JEDEC ≠ 0**.
  SRAM (`MTD-0000`, JEDEC 0) can never pass. Also, once TrueFFS is installed it owns
  `MTD-0000`, so MTD.INF's SRAMMTD.VXD never loads for it.
- `Software\M-Systems\MTD` (`0xe3df`): maps the devnode's `MTD-xxxx` suffix to an extra MTD
  VxD to load (CM_Load_DLVxDs); empty by default.

### Intel MTD (what the flash card must satisfy)

- ctor `0x7120`: set 16-bit window (`0x566a(socket,1)`), **flIntelIdentify** (`0x4806`):
  `FF,FF` then `90` at byte 0, 1, 2… up to 0x3F; reads until the byte at offset `inlv`
  differs from the vendor byte → type = vendor<<8 | byte, interleave = inlv (power of 2).
  For 0x89A2 (28F008SA): chipSize 1 MB × interleave, erase block 64 KB × interleave.
  Also takes 89A0 (28F016SA), B088, 89A7/A6/AA (SC series, 5 V), 8989.
- **flIntelSize** (`0x499a`): chip 0 left in Read ID, then every chipSize: stop at a
  wraparound to chip 0's ID or a group that doesn't answer → number of groups.
- write `0x73e6`: socket WP check (`0x548c`) → 1; Vpp on (`0x799a` → `0x581d`: CS
  GetConfigurationInfo, RequestConfiguration if needed, **ModifyConfiguration Vpp1 = Vpp2 =
  12.0 V**); `40 40`+data per word, poll 0x8080; per chip status & 0x38 → error 11 + `50`;
  `FF`; memcmp.
- erase `0x75cf`: per chip `20`,`D0`; `70` poll bit 7; & 0x38 → 11; `50`; `FF`.

### Windows 9x side (PCCARD.VXD obj3)

- CS dispatch table at obj5+0x6d4, **12-byte entries** (handler, ?, arg length).
- Regions (`0xa4fc`): DEVICE (0x01) → one common region per device entry; JEDEC_C (0x18) →
  region JEDEC word; DEVICE_GEO (0x1E) → BlockSize = bus × erase block × interleave,
  PartMultiple. GetFirstRegion packet (`0xa7c1`): +2 attributes (`node[6] + devtype<<4`),
  +0x10 offset, +0x14 size, +0x18 block size, +0x1C part multiple, +0x1E JEDEC.
- Devnode name `PCMCIA\MTD-%04X` of the JEDEC word (`0xd08d`): bytes (89, A2) → `MTD-A289`.

## What changed in the code

- `src/pcmcia/flash_card.c` (new, "Flash memory card (Intel Series 2)", `pccard_flash`):
  2/4/10/20 MB of 28F008SA, two chips side by side (even/odd bytes), full command set
  (FF, 90, 70, 50, 40/10, 20+D0, B0/D0); program ANDs, erase sets a 64 KB chip block
  (128 KB card unit) to FF; **program/erase need 12 V on the chip's Vpp** (Vpp1 even chips,
  Vpp2 odd), else status VPPS; the **WP switch cuts Vpp** (commands and ID still work, so a
  protected card identifies at its real size, read-only) and shows on the WP pin. Contents in
  `nvr\pcmcia_flash_<a|b>.bin`. `flash_log` behind `ENABLE_FLASH_CARD_LOG`.
- `pccard_cis.c`: `pccard_cis_flash()` (DEVICE flash 150 ns, JEDEC_C 89 A2, DEVICE_GEO
  16-bit/128 KB, VERS_1 "86Box-Next","Flash Card", FUNCID memory, NO_LINK); shared
  `device_size()`.
- `pcmcia.h`: `pccard_t.write_protect` (WP pin), `pcmcia_socket_vpp(socket, pin)`,
  CISTPL_JEDEC_C / DEVICE_GEO.
- `pcic_pd6722.c`: status bit 4 (WP) from the card's pin (it was never set: the SRAM
  switch silently dropped writes); `pcmcia_socket_vpp()` from power register bits 1:0/3:2.
- `sram_card.c`: reports its switch on the WP pin.
- `pcmcia.c` card list, `src/device/CMakeLists.txt`.
- Tests: `tests/pcmcia/flash_test.c` (new: ports of TrueFFS's flIntelIdentify, flIntelSize,
  write and erase run against the card: ID 89A2, interleave 2, size for 2/4/10/20 MB, Vpp,
  WP, CFI probe harmless); `cis_test.c` (JEDEC/GEO parsing, `MTD-A289`); `pcic_test.c`
  (WP bit, Vpp). All pass: build them directly, e.g.
  `gcc -std=gnu11 -Isrc/include -Ibuild-static/src/include tests/pcmcia/flash_test.c src/pcmcia/flash_card.c src/pcmcia/pccard_cis.c`
  (UCRT64 gcc; `build-static` has BUILD_TESTING off).
- CLAUDE.md's PCMCIA paragraph has the flash card and its TrueFFS quirks.

## Resolution of the second session's open bugs (third session, 2026-10-05)

### "Writes after TFORMAT find Vpp back at 5 V" -- TrueFFS behaviour, not ours

Build 58's stack dump of the `F5` power write, matched to the VxDs with the new
`tools\stackmatch.py` (fixup bytes as wildcards): SOCKETSV obj1+0xCF (the port write) ←
SOCKETSV obj1+0x2AA (SS dispatcher) ← PCCARD obj3+0x8670 ← **PCCARD's Card Services entry**
(obj1+0x23b, function table obj5+0x644) running **CS function 0x1E, ReleaseConfiguration**
(handler obj3+0x7713) ← TrueFFS's CS wrapper (`0x5130`, `int 20h` PCCARD service 1).
Load addresses in that run: PCCARD obj1 C142CAE0, obj3 C1817AD0; SOCKETSV obj1 C1430920,
obj2 C1825ED0. (`tools\dumpobj.py` writes one LE object + its fixups for objdump.)

TrueFFS (obj1): Vpp-on `0x581d` increments the use counts (+0x2a on the socket and on the
shared `+0x3e` object) and calls ModifyConfiguration(Vpp 12 V) only if shared+0x2e is 0, then
sets it to 1. Vpp-off is lazy: the CS timer event (`0x529e` → `0x5329`, which calls `0x59cd`
for each socket and re-arms a 5000 ms timer with CS 0x28) needs two ticks with the use count 0
before it clears shared+0x2e and lowers Vpp. ReleaseConfiguration (`0x5ac8`, called from
`0x60b8` when TrueFFS shuts a socket down and `0xd5e3` when a handle closes -- TFORMAT exiting)
does **not** clear shared+0x2e. So after TFORMAT exits, PCCARD has reset Vpp to Vcc but
TrueFFS still believes it is at 12 V, until its timer has ticked twice (~10 s). The old batch
copied immediately. With `CHOICE /T:Y,20` between TFORMAT and COPY the copy works and the
trace shows TrueFFS raising Vpp (`FA`) again. A real 82365 + PCCARD would do the same.

### "Boot hangs with the formatted card" -- does not reproduce

`pcictest\flash_b_after_run1.bin` (the image from the run whose copy failed) boots fine on
build 58: E: mounts (empty, as run 1's copy never reached the card), a new file is written
and reads back identical. The earlier "hang" was most likely the boot waiting for a key (the
network login dialog; see below), not TrueFFS.

### FAT12 on a flash card (owner's question)

`tools\mkflashfat.py` makes a 4 MB card holding plain FAT12 + README.TXT
(`pcictest\flash_fat12_4m.bin`). TrueFFS mounts it **read-only**: DIR/TYPE/COPY from it work
and the file is byte-identical; TCHECK says "Could not recognize card format" (no FTL); a
write gives "Write protect error". As reversed: the FTL mount fails, then the RAM/ROM FAT layer
(`0xef30`) is writable only when `isRAM`, and flash is not RAM. Expected on real hardware too;
TFORMAT the card to write to it.

### Guest results (all on the `pcictest` copy)

| run | build | card | result |
| --- | --- | --- | --- |
| 4 | 58 | blank | TFORMAT, 20 s wait, COPY×2, FC, TCHECK: all OK (`flash_trace4.txt`) |
| 5 | 58 | run 4's | reboot: 2 files, FC OK, new copy OK (`flash_trace5.txt`) |
| 6 | 58 | FAT12 | read-only mount as above (`flash_trace6_fat12.txt`) |
| 7 | 58 | run 1's | boots, mounts, writes OK |
| 8 | 65 | blank | as run 4, all OK |
| 9 | 65 | run 8's | reboot: 3 files, FC OK, new copy OK; logged in by itself |

## How the guest test is run (unattended)

- `pcictest\86box.cfg`: `socket_b = pccard_flash`, `[Flash memory card (Intel Series 2) #2]
  size = 4` (the SRAM config is saved as `86box.cfg.sram-bak`). Card contents:
  `pcictest\nvr\pcmcia_flash_b.bin` (absent = blank card); saved images
  `flash_b_after_run1.bin`, `flash_b_after_run4.bin`, `flash_fat12_4m.bin`.
- The test copy **logs in automatically now**: `tools\AUTOLOG.REG` (imported by the batches
  with `REGEDIT /S C:\WINDOWS\AUTOLOG.REG`) sets `HKLM\Network\Logon\PrimaryProvider=""`
  (Windows Logon as the primary logon; blank password, so no prompt) plus the Winlogon
  auto-logon values (those alone were not enough). The owner's image in `Latest` still asks
  (Enter). After an exit without a Windows shutdown, Win98 runs ScanDisk at the next boot
  (Enter leaves it).
- Batches for the guest's StartUp folder, put there with
  `python tools\fatput.py IMAGE "WINDOWS/Start Menu/Programs/StartUp" NAME.BAT NAME.BAT`
  (`-` as the last argument removes it): `TFTEST.BAT` (TFORMAT, 20 s wait, DIR/COPY/FC,
  TCHECK), `TFREAD.BAT` (boot with a formatted card: DIR, FC, a new copy), `TFFAT.BAT` (the
  FAT12 card). All log to `C:\TFLOG.TXT` (read it with `fat32.py`) and shut Windows down.
  **TFREAD.BAT is in StartUp now**; remove it when done. Write them with CRLF line ends from
  Python (Git Bash's sed mangled backslashes and CRs).
- Runner: `tools\run_guest.ps1 -Seconds 300 [-Exe F:\...\pcictest\b65.exe]` (default exe
  `pcictest\trace.exe` = build 58, PCIC + flash logs on, plus the power-write stack dump).
  Starts the VM minimized, restores it without focus, PrintWindow screenshots into
  `tools\shots\`; every 5 s it presses Enter (`sendenter.cs`: brings the window to the front
  for a moment, then gives the focus back) if the network login dialog or ScanDisk is on
  screen; closes the VM (CloseMainWindow) once Windows has shut down (two more `power 40` in the
  trace than at boot, or the orange "It's now safe to turn off your computer" screen) or after
  `-Seconds`. ROMs come from `Latest\Windows 98 SE\roms` (read only). The guest disk is
  `hdd_01_speed = ramdisk`: never kill 86Box, or the guest's writes and the card file are lost.
- Trace builds: put `#define ENABLE_PCIC_LOG 1` / `#define ENABLE_FLASH_CARD_LOG 1` after
  `#define HAVE_STDARG_H` in `pcic_pd6722.c` / `flash_card.c`, build, copy the exe to
  `pcictest\trace.exe`, delete the lines again. For a stack dump per power write, re-add in
  `pcic_reg_write`'s REG_POWER logging: `ss + ESP`, `mmutranslate_noabrt()`,
  `mem_readl_phys()`/`mem_readb_phys()`, `#include "cpu.h"` (not in the sources).

### Code state

- `pcmcia-flash-card` (a5a2e2f46, 0c32ad062) merged into `master` (7ba664c38) on top of
  another session's modem commit; build 65 staged into every rig in `Latest`.
- Unit tests pass: flash 50, CIS 117, controller 59 checks.
- `F:\Claude\86Box-Next\pcic_pd6722.c.keep` (a scratch copy from the second session) can go.

### Possible follow-ups

- Other parts TrueFFS's MTDs accept (28F016SA Series 2+ `MTD-A089`; AMD Series D 29F016
  `MTD-AD01` with the AMD unlock sequence), if the owner wants them.

## Tools and artefacts (`F:\Claude\86Box-Next\flash-investigation`)

- `binaries\`: TRUEFFS.PDR, SRAMMTD.VXD, SOCKETSV.VXD, PCCARD.VXD, CONFIGMG.VXD;
  disassemblies (`tf1.txt` etc. from the first session; fixup annotations there are
  unreliable for TRUEFFS because of the page-size bug).
- `tools\`: `fat32.py` (read), `fatput.py` (write/remove one small file), `stackmatch.py`,
  `dumpobj.py`, `mkfat.py`/`mkflashfat.py`, `run_guest.ps1` + `sendenter.cs`, the batches,
  `infgrep.py`, `creg.py`, `pcmcia_devs2.py`, `emu_crc.py`, `le.py`/`le2.py` (4 KB pages
  only).
- `traces\`: the first session's SRAM traces.
- `C:\Users\xeon4\Desktop\ftl.zip`: TrueFFS package. Antivirus quarantines its .exe/.pdr when
  extracted; read them from the zip in memory (`zipfile`).

## First session (summary, corrected)

Card detection, data path and socket WP were verified then. Its conclusions "TrueFFS never
calls GetFirstRegion" and "the write-protect comes from a failed SRAM identification that
could be fixed" were wrong: see above.
