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
  `PCMCIA\MTD-A289` for TrueFFS; program/erase need 12 V on the socket's Vpp; verified in
  Win98 + TrueFFS-9x: TFORMAT, copy, TCHECK, reboot. TrueFFS quirks that are not bugs: a blank
  SRAM card can't be formatted (TFORMAT is flash-only), a FAT image on flash mounts read-only,
  writes fail for ~10 s after TFORMAT exits (its lazy Vpp-off); see `flashhandoff.md`), 3Com 3C562D LAN+33.6 Modem (`pccard_3c562.c`), the first
  multi-function card: `pccard_mfc.c` is a PC Card 95 MFC framework (per-function COR/CCSR/
  I/O base/limit registers, I/O routed by each function's decode, IREQ = OR of the functions),
  CIS with LONGLINK_MFC chains (Windows 9x's ID walk follows function 0's chain); its LAN is
  `net_3c509b.c`'s `threec562_lan_device`, its modem `char_modem.c`'s engine on a detached
  16550 (`serial_init_detached()`, no COM slot) opened with `char_open_unlisted()` in the card's
  device context; the modem icon lists it as "PC Card A/B" and saves its line in the card's
  section. Verified in Win98 (NET3C562.INF + MDMGATEW.INF, ping, ATI over ReadFile); caveat:
  Windows tends to give its modem COM4 at 2E8h, which the S3 cards' 8514/A register shadows
  (`vid_s3.c`, as on real hardware) -- reads come back 00h; move the modem's I/O in Device
  Manager; see `mfchandoff.md`. Adaptec APA-1460 SlimSCSI (`scsi_apa1460.c`): the AIC-6360 of
  `scsi_aic6360.c` (`aic6360_chip_init()`: the chip without ISA I/O or IRQ), COR at 2000h as its
  Technical Reference has it (SRESET/IOEN/PRIMARY, so CIS indexes 09h = 340h, 08h = 140h), PORTA/B
  read FFh; Windows 98's SCSI.INF ID `...-BE89` (SPARROW.MPD). Its SCSI bus is its socket's
  (`pcmcia_scsi_bus()`), kept from the hard reset (or first insertion) to the next, after every
  other device's -- `scsi_plan()` lists it, so Settings > Hard disks names it. Verified in Win98: a
  disk on its bus as E:, read and written. A memory card's write-protect switch shows on the socket's WP
  bit. CIS in
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
- COM port modems (`src/char/char_modem.c`, PeepeeBox's): Diamond SupraExpress 56e PRO and
  ELSA MicroLink 56k, chosen as a COM port's device in Settings > Ports. AT engine with ATI
  answers that funworld's modem table resolves to exactly one part; the line is dead or dials
  a TCP host (any number), with a real call's timing and sounds (`modem_sound.c`, a pool of
  speakers whose sound handlers are registered once per sound reset: `sound_has_handler()`).
  `DEVICE_HOTPLUG`, so attaching, removing or reconfiguring one is a soft change (the soft
  settings path refreshes the status bar). Status bar modem icon (`src/qt/qt_modem_menu.cpp`,
  shown while a COM port is configured with a modem or a socket holds a PC Card with one;
  `char_modem.h` slots: the COM ports, then one per socket) sets each modem's line live through
  `char_modem.h` (applied on the emulation thread; a call in progress gets NO CARRIER) and saves
  it in the modem's section (`<name> #<COM+1>`). Tests: `tests/modem/` (FN_SYS's tables).
  The modem's bytes to the line go through a queue (`MODEM_TXQ_*`): short sends and would-block
  lose nothing, CTS drops at the high-water mark, an overrun past CTS ends the call; a full
  receive ring is not a hangup. Lines are backends (`modem_line_ops_t`: TCP, ISP) behind the
  unchanged call state machine (CONNECT before DCD).
- Virtual ISP (`src/network/isp/`, `86box/isp.h`): line 2, "Internet (built-in ISP)", on the
  COM modems and the 3C562D (Settings, status bar modem menu). Any number reaches it; one session
  per call: PPP server (`ppp_framing.c` RFC 1662, `ppp_session.c` RFC 1661 LCP passive until the
  guest's first request, optional PAP accepting anything, IPCP with address and DNS; rejects
  PFC/ACFC/callback/VJ/NBNS, Protocol-Rejects CCP/IPX/NBF) and a libslirp instance per session
  (`isp_nat_slirp.c`: synthetic Ethernet/ARP, wall-clock timers, select()/poll() -- not
  WSAEventSelect, which loses FD_CONNECT -- and no "readable" fallback, which makes libslirp send
  the guest ICMP unreachables). Session N gets 10.86.N.0/24: guest .15, gateway .2 (= host
  loopback), DNS .3. Each session has its own thread; the modem only copies bytes through locked
  queues. `isp-server.exe` (same dir, console) is the same ISP on a TCP port (default
  127.0.0.1:2323) for the modem's TCP line. Tests: `tests/isp/` (framing, PPP automaton, sessions
  through libslirp to a host UDP socket; `isp_probe` drives a running isp-server, `--internet
  NAME` adds DNS + HTTP) and `tests/modem/` (transport, two modems, ISP calls through the UART).
  `cmake --build build-static --target isp_tests modem_tests isp_probe`, then
  `ctest --test-dir build-static -R "Isp|Modem"` (build-static is configured with BUILD_TESTING=ON).
- Upstream's CI workflows and Dependabot are disabled/removed in this repo; only
  `sync-upstream.yml` runs.
- Build number in the title bar / About box: `.build-number` (git-ignored, repo root)
  holds the last successful build; `cmake/NextBuildNumber.cmake` bumps it on every
  successful link, so a no-change build still relinks with the next number.

## Layout and the staged rig
`F:\Claude\86Box-Next\86box` is this repository; `F:\Claude\86Box-Next\Latest` (outside
it) holds the owner's test rigs, one folder per guest OS (`Windows 98 SE` -- the PCMCIA/USB
machine; its Windows asks for a network login at boot, and runs ScanDisk at boot after
an exit without a Windows shutdown: Enter for both -- `MS-DOS`, `OS2 Warp 3`, ...),
each with the newest build. `tools/stage-rig.sh` replaces only the exe (plus DLLs for a
dynamic build) in every rig folder holding an `86box.cfg` or an exe; a rig's
`roms/` is only created when missing (86Box ROM set + MegaPPBox's `roms/megatouch`) and
only refreshed with `--update-roms` -- leave the ROMs alone unless strictly necessary. **Never delete
anything else in the rigs** -- `86box.cfg`, `nvr/` and disk images are theirs.
Restage after every change; it refuses while 86Box-Next is running.

## Building (Windows)
Static (the default, and what gets staged): MSYS2 UCRT64 with `qt6-static`, `libtiff`,
`libwebp` and `libusb` installed:
`MSYSTEM_PREFIX=C:/msys64/ucrt64 cmake -S . -B build-static -G Ninja -DCMAKE_BUILD_TYPE=Release -DQT=ON -DUSE_QT6=ON -DSTATIC_BUILD=ON && cmake --build build-static`
A dynamic `build/` (`-DSTATIC_BUILD=OFF`) links faster while iterating; run it with
`C:\msys64\ucrt64\bin` on PATH, or Windows loads a mismatched Qt6Core.
Tests: `mingw-w64-ucrt-x86_64-gtest` is installed and build-static has `-DBUILD_TESTING=ON`;
build a suite's target, then `ctest --test-dir build-static -R <Name>`.
