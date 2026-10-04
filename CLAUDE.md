# 86Box-Next

A fork of [86Box/86Box](https://github.com/86Box/86Box) with extra features.

- `origin` is `Xeon3D/86Box-Next`; `upstream` is 86Box/86Box (fetch only, push disabled).
- `.github/workflows/sync-upstream.yml` merges upstream `master` into ours once daily;
  conflicts are reported as an issue in this repo.
- **Never open pull requests, issues or comments against 86Box/86Box** unless the owner
  explicitly asks for that specific submission. All PRs target `Xeon3D/86Box-Next`.

## Fork additions
- Elo TouchSystems SmartSet serial touchscreen (`src/device/mouse_elo_touchscreen.c`),
  ported from [PeepeeBox](https://github.com/Xeon3D/PeepeeBox).
- Branded "86Box-Next" via `EMU_DISPLAY_NAME`; `EMU_NAME` stays "86Box" for paths
  and guest-visible hardware IDs. Use `EMU_DISPLAY_NAME` for anything users read.
- No global config: every setting lives in the machine's own `86box.cfg`
  (`config.c`); the optional VM Manager's `vmm.ini` sits next to it.
- VM Manager is optional and off by default (Preferences > Emulator).
- Arcade I/O boards (`src/device/io_board.c`, one per machine, Settings > Other
  peripherals): funworld Photo Play (PeepeeBox `funworld_io.c`) and Merit
  Megatouch XL/MAXX (MegaPPBox `merit_io.c`; the MAXX's PC Card slots are the
  `src/pcmcia` controller). UI controls in `src/qt/qt_ioboard_controls.cpp`.
- PCMCIA (`src/pcmcia/`, Settings > Other peripherals > PCMCIA tab, ISA): MegaPPBox's Cirrus
  CL-PD6722 controller (`pcic_pd6722.c`, 0x3E0, two sockets, one per machine, shared with the
  MAXX I/O board): attribute and common memory windows, per-socket status changes and IRQs
  (every IRQ line worked out from all sources), card-detect events. Hot-pluggable: the PC Card
  status bar icon (`src/qt/qt_pccard_menu.cpp`, in `qt_machinestatus.cpp`) puts any card in or
  out of either socket; `pcmcia.c` makes and closes cards itself (device context instance
  socket+1) inside its "PC Card slots" device, not the device list, applying UI requests in
  the controller's 10 ms poll -- so a card must close everything it made (TE100: its DP8390).
  Cards (`pcmcia.c`'s list): 3Com 3C589D (`net_3c509b.c`'s PC Card mode), TRENDnet TE100-PC16
  (`net_te100pc16.c`), SRAM memory card (`sram_card.c`, contents in nvr/), Intel Series 2
  flash card (`flash_card.c`: 28F008SA pairs, JEDEC 89A2 so Windows 9x makes it
  `PCMCIA\MTD-A289` for TrueFFS; program/erase need 12 V on the socket's Vpp; WIP, see
  `flashhandoff.md`). A memory card's write-protect switch shows on the socket's WP bit. CIS in
  `pccard_cis.c`: built there, with NO_LINK, and a 2-byte CONFIG filler subtuple solved at
  build time so Windows 9x's ID matches the INF's (its checksum: ARC by nibble tables with one
  wrong entry, from PCCARD.VXD/CONFIGMG.VXD). Config in `[PCMCIA]`; a card's own settings are
  device instance #1/#2 (socket A/B); a PC network card links through
  `net_cards_conf[NET_CARD_MAX + socket]` (`NET_CONF_MAX`). Tests: `tests/pcmcia/` (controller;
  CIS parsed by pcmcia-cs cistpl.c's rules, Windows IDs pinned to ones Windows 98 produced).
- USB (`src/usb/`, Settings > Other peripherals, PCI): UHCI USB 1.1 cards (VIA
  VT83C572, Intel PIIX4) and USB 2.0 cards (EHCI + UHCI companion: VIA VT6202, Intel
  ICH4 layout with EHCI at function 7), ports in `usb_bus.c`, host passthrough via
  libusb (UsbDk on Windows when installed) in `usb_host.c` -- bulk/interrupt/control,
  isochronous streams (USB audio at full speed via UHCI; high speed via EHCI iTDs,
  high-bandwidth endpoints included; no siTDs -- full-speed devices use the companion),
  high-speed devices shown at full speed on USB 1.1 ports (`usb_speed.c`) -- the
  VMware-style connect prompt and USB menu in `src/qt/qt_usb_manager.cpp`, and an
  activity trace (USB menu / BOX86NEXT_USB_TRACE=1) to usb_trace.txt.
  Tests: `tests/usb/` (plain C, no framework). Upstream's `src/usb.c` is the
  southbridge stub and is untouched.
- Upstream's CI workflows and Dependabot are disabled/removed in this repo; only
  `sync-upstream.yml` runs.
- Build number in the title bar / About box: `.build-number` (git-ignored, repo root)
  holds the last successful build; `cmake/NextBuildNumber.cmake` bumps it on every
  successful link, so a no-change build still relinks with the next number.

## Layout and the staged rig
`F:\Claude\86Box-Next\86box` is this repository; `F:\Claude\86Box-Next\Latest` (outside
it) is the newest build, where the owner also keeps the machine they test with.
`tools/stage-rig.sh` replaces only the exe (plus DLLs for a dynamic build) there;
`roms/` is only created when missing (86Box ROM set + MegaPPBox's `roms/megatouch`) and
only refreshed with `--update-roms` -- leave the ROMs alone unless strictly necessary. **Never delete
anything else in Latest** -- `86box.cfg`, `nvr/` and disk images are theirs.
Restage after every change; it refuses while 86Box-Next is running.

## Building (Windows)
Static (the default, and what gets staged): MSYS2 UCRT64 with `qt6-static`, `libtiff`,
`libwebp` and `libusb` installed:
`MSYSTEM_PREFIX=C:/msys64/ucrt64 cmake -S . -B build-static -G Ninja -DCMAKE_BUILD_TYPE=Release -DQT=ON -DUSE_QT6=ON -DSTATIC_BUILD=ON && cmake --build build-static`
A dynamic `build/` (`-DSTATIC_BUILD=OFF`) links faster while iterating; run it with
`C:\msys64\ucrt64\bin` on PATH, or Windows loads a mismatched Qt6Core.
