# Handoff: PC Card memory cards under Windows 98 + TrueFFS

Second session, 2026-10-04/05 (builds 55-58, uncommitted work on `master`). Read `CLAUDE.md` first
(repo layout, build, staging rules: never delete anything in `F:\Claude\86Box-Next\Latest`).
The first session's notes are summarised at the end; several of its conclusions were wrong
and are corrected here.

## Bottom line

**The SRAM card was never the bug.** TrueFFS-9x 3.2 (TRUEFFS.PDR, TFORMAT 3.3.6) *cannot
format a blank SRAM card, on real hardware either*. TFORMAT only lays FTL onto media that one
of TrueFFS's *flash* MTDs has identified, and every flash MTD rejects RAM on purpose. The only
SRAM support in TrueFFS is a plain-FAT translation layer that mounts an SRAM card that
*already* holds a DOS boot sector. The emulated SRAM card behaves like a real one: blank →
"1024 KBytes, write-protected" (the read-only fallback). Nothing to fix there, and the owner
does not want pre-formatted cards.

What TFORMAT is made for is a **linear flash card**, so this session added one: an
**Intel Series 2 flash card (28F008SA)**, `src/pcmcia/flash_card.c`. Its CIS gives Windows
`PCMCIA\MTD-A289`, which TRUEFFS.INF maps to TrueFFS, and its chips answer TrueFFS's Intel
MTD exactly as the real parts do (checked by unit tests that port the MTD's routines from
the disassembly). **In the guest, TFORMAT now formats it (4096 KB physical, 3798 KB formatted).
Two bugs left: writes after TFORMAT find Vpp back at 5 V, and a boot with the formatted
card hangs. See "Status".**

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

## What changed in the code (uncommitted)

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
- CLAUDE.md's PCMCIA paragraph should get the flash card added once it's verified.

## Status / next steps

### Guest results (2026-10-05, 4 MB card in socket B, trace builds 56-58)

Run 1 (build 56, blank card) and run 3 (build 57, blank card, power log) behave the same:

- Windows creates `MTD-A289`, TrueFFS loads; CFI/NAND probes harmless; Intel identify and
  flIntelSize run exactly as reversed (pairs at 0 and 2 MB found, nothing at 4 MB).
- **TFORMAT succeeds**: `Medium physical size is 4096 KBytes` ... `Format complete.
  Formatted size is 3798 KBytes.` (blank card, so no erases, ~74 K programs, all at 12 V).
  `DIR E:` after it: empty drive, 3,847,680 bytes free. The FTL on the card is well formed
  (32 units of 128 KB, 31 logical + 1 transfer unit, formatted size 3,889,152, BAM at 0x44).
- **Open bug 1: writes after TFORMAT fail.** `COPY ... E:\` gives "General failure reading
  drive E" (Abort/Retry/Fail; the batch waits there). Power-register log (`flash_trace3.txt`):
  - power-up: socket B `40` -> `55` -> `F5` (Vcc on, outputs on, Vpp1 = Vpp2 = Vcc);
  - TFORMAT's first write: `FA` (Vpp1 = Vpp2 = 12 V): TrueFFS's ModifyConfiguration works;
  - right after TFORMAT's last write (BAM at 0x270), **before** the remount: back to `F5`;
  - then the remount (identify again) and the copy's writes at 0x274.. **without Vpp**, and
    no further power write at all: TrueFFS never raises Vpp again.

  TrueFFS's Vpp-on (`0x581d`) calls ModifyConfiguration(12 V) only when the shared flag
  `socket->+0x3e->+0x2e` is 0. Its own Vpp-off (`0x59cd`, called from `0x54ac(0)` and from
  the CS TIMER_EXPIRED event 0x15 -> `0x529e`; it needs two calls, `+0x48` is the delay)
  clears that flag *before* lowering Vpp, so it can't leave the flag stale. Its
  ReleaseConfiguration (`0x5ac8`, CS 0x1E) runs only on CARD_REMOVAL (event 0x05 ->
  `0x5220`; 0x40 CARD_INSERTION -> `0x51fa`), and there was none. So the drop to 5 V most
  likely came from **outside TrueFFS**: PCCARD/SOCKETSV re-applying the socket's configured
  power (on TFORMAT closing `\\.\TRUEFFS`, a power-management call, or a status change our
  controller reported). Whether a real 82365 + PCCARD does the same is the open question.

  **Next step (prepared, not run):** `pcictest\trace.exe` is now **build 58** = build 57
  plus, on every power-register write, the guest CS:EIP and every stack dword in VxD space
  (0xC0000000-0xC2000000) with the 12 bytes before it. Match those bytes against
  PCCARD.VXD / SOCKETSV.VXD / TRUEFFS.PDR / CONFIGMG.VXD to name the caller of the `F5`
  write. The owner stopped that run before it started (they asked for the docs first); ask
  before starting it. The temporary stack-dump code is **not** in the sources (added, built,
  removed). To re-add: in `pcic_reg_write`'s REG_POWER logging, `ss + ESP`,
  `mmutranslate_noabrt()`, `mem_readl_phys()`/`mem_readb_phys()`, `#include "cpu.h"`.

  Then: if the caller is SOCKETSV/PCCARD reacting to something our PCIC reports (ready or
  status change, a CSC interrupt, the WP bit), fix the PCIC. If it is ordinary Card Services
  behaviour, look for the CS event TrueFFS should have got for it.
- **Open bug 2: boot hang with the formatted card** (run 2, build 57, card as run 1 left it:
  `pcictest\flash_b_after_run1.bin`, log `flash_trace2_hang.txt`). Windows reaches the
  desktop, TrueFFS identifies the card, then an hourglass and the tray clock stops; the
  StartUp batch never runs; no flash commands or power writes after the identify. Maybe the
  FTL mount loops (reads are not logged). Unit 0's BAM: entries 3-15 read `40 xx FF FF`
  (low word programmed, high word still FFFF). Check whether TrueFFS writes BAM entries
  that way or it is a leftover of the failed copy, and whether the mount hangs on it.
  Repro: copy `flash_b_after_run1.bin` to `pcictest\nvr\pcmcia_flash_b.bin`.
  (Every run also logs `Illegal instruction 00008B55 (FF)` once at 0147:B9BD during boot,
  run 1 included; probably unrelated.)
- The guest disk is `hdd_01_speed = ramdisk`: guest writes and the card's nvr file only
  reach disk when 86Box exits cleanly. Never kill it; close the main window
  (`$p.CloseMainWindow()`, `confirm_exit = 0` in the cfg). The runner does that.

### How the guest test is run (unattended)

- `pcictest\86box.cfg`: `socket_b = pccard_flash`, `[Flash memory card (Intel Series 2) #2]
  size = 4` (the SRAM config is saved as `86box.cfg.sram-bak`). Card contents:
  `pcictest\nvr\pcmcia_flash_b.bin`, **currently absent (= blank card)**.
- Trace builds: put `#define ENABLE_PCIC_LOG 1` / `#define ENABLE_FLASH_CARD_LOG 1` after
  `#define HAVE_STDARG_H` in `pcic_pd6722.c` / `flash_card.c`, build, copy
  `build-static\src\86Box-Next.exe` to `pcictest\trace.exe`, delete the lines again (the
  normal build has both logs compiled out). Log: `-L pcictest\flash_trace.txt`; older ones:
  `flash_trace1.txt` (run 1), `flash_trace2_hang.txt` (run 2), `flash_trace3.txt` (run 3).
- Batch in the guest's StartUp folder (`flash-investigation\tools\TFTEST.BAT`), put there with
  `python flash-investigation\tools\fatput.py IMAGE "WINDOWS/Start Menu/Programs/StartUp" TFTEST.BAT TFTEST.BAT`
  (`-` as the last argument removes it). It is **still there** (remove it when done):
  TFORMAT E: /Y /S:! /VERBOSE > C:\TFLOG.TXT, DIR/COPY/FC on E:, TCHECK E:, shutdown. Read
  `C:\TFLOG.TXT` with `fat32.py`.
- Runner: `flash-investigation\tools\run_guest.ps1 -Seconds 240 -Every 60`: starts
  trace.exe minimized, restores it with SW_SHOWNOACTIVATE (no focus steal), PrintWindow
  screenshots into `tools\shots\`, then CloseMainWindow. The owner sees the window; **ask
  them first**.
- Loader with the right LE page size for TRUEFFS.PDR: `flash-investigation\tools\le3.py`.

### Code state

- Nothing committed. `Latest` was restaged on 2026-10-05 with **build 59** (the normal
  build of this work: flash card, WP pin, Vpp; logs compiled out). Builds 56-58 were trace
  builds for `pcictest` only. The sources
  hold the changes listed above plus a `pcic_log` line for power-register writes (compiled
  out normally). `F:\Claude\86Box-Next\pcic_pd6722.c.keep` is a scratch copy of the current
  `pcic_pd6722.c`; delete it.
- Unit tests (flash, cis, pcic) pass.

### After the open bugs

1. Restage `Latest` again with the fixes (`tools/stage-rig.sh`; refuses while 86Box-Next
   runs), add the flash card to CLAUDE.md's PCMCIA paragraph, commit (memory: never commit
   from a shell whose PATH starts with `/c/msys64/usr/bin`).
2. Optional: other parts the MTDs accept (28F016SA Series 2+ `MTD-A089`, AMD Series D 29F016
   `MTD-AD01` with the AMD unlock sequence), if the owner wants them.

## Tools and artefacts (`F:\Claude\86Box-Next\flash-investigation`)

- `binaries\`: TRUEFFS.PDR, SRAMMTD.VXD, SOCKETSV.VXD, PCCARD.VXD, CONFIGMG.VXD;
  disassemblies (`tf1.txt` etc. from the first session; fixup annotations there are
  unreliable for TRUEFFS because of the page-size bug).
- `tools\`: `fat32.py` (read), `fatput.py` (new: write/remove one small file),
  `infgrep.py`, `creg.py`, `pcmcia_devs2.py`, `emu_crc.py`, `le.py`/`le2.py` (4 KB pages
  only).
- `traces\`: the first session's SRAM traces.
- `C:\Users\xeon4\Desktop\ftl.zip`: TrueFFS package. Antivirus quarantines its .exe/.pdr when
  extracted; read them from the zip in memory (`zipfile`).

## First session (summary, corrected)

Card detection, data path and socket WP were verified then. Its conclusions "TrueFFS never
calls GetFirstRegion" and "the write-protect comes from a failed SRAM identification that
could be fixed" were wrong: see above.
