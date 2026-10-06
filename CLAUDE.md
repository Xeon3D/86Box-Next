# 86Box-Next

A fork of [86Box/86Box](https://github.com/86Box/86Box) with extra features.

- `origin` is `Xeon3D/86Box-Next`; `upstream` is 86Box/86Box (fetch only, push disabled).
- `.github/workflows/sync-upstream.yml` merges upstream `master` into ours once a week (Mondays);
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
  receive ring is not a hangup. Lines are backends (`modem_line_ops_t`: TCP, phone) behind the
  unchanged call state machine (CONNECT before DCD). Line 2, "Telephone network (isp-server)",
  with `phone_number` (blank: the exchange assigns) and `exchange` (127.0.0.1:2323) settings: the
  modem holds a REGISTER connection to isp-server's exchange (reconnects every 3 s, polled every
  32nd `modem_read`), gets RING/CANCEL on it -- RING result every 6 s, RI 2 s, S1 counts, S0
  auto-answers, ATA, caller ID with `AT#CID=1`/`AT+VCID=1` -- and opens a connection per call
  (DIAL/ANSWER, the reply read a byte at a time until CONNECT; BUSY plays the busy tone). Modem
  menu: "Telephone network (isp-server)", "Set phone number...", the number in the status line.
  (Line 2 was the built-in ISP for a day; the exchange still reaches the ISP on any unknown
  number.)
- Voice calls (Supra and ELSA, not the 3C562): Rockwell's `#` voice set, as Windows 9x's
  Unimodem/V uses it (the Diamond INF: `#CLS=8`, `#VLS=0`, `#VBS=4`, `#VSR=7200`, `#VTX`/`#VRX`,
  `#VTS`) and vgetty's Rockwell driver. `AT#CLS=8` then ATD/ATA give VCON (`#VRN=0`: once
  dialled); ATD...; is a voice call too (OK once dialled). `#VTX` plays the DTE's audio
  (DLE-shielded, DLE ETX ends it once played out, DLE CAN flushes; CTS paces it) and `#VRX`
  records the line until any byte (DLE ETX, OK; the rest up to the next `A` is swallowed); both
  report DLE events: the far end's digits, `b` once it hung up, `s` after `#VSP` of silence,
  `a`/`e` for a modem's answer/calling tone. `#VLS` 0/4 the line, 1/2/3 handset/speaker/mic,
  6 speakerphone. `modem_voice.c`: Rockwell ADPCM 2/3/4-bit, a reentrant port of vgetty's
  rockwell.c, bit-exact with it (and so with Rockwell's coder, which Windows' SERWAVE.VXD
  decodes) -- the tests pin CRCs computed with vgetty's own code; mu-law, 7200<->8000 Hz, DTMF,
  silence. On the exchange DIAL/ANSWER take ` VOICE` and each end's CONNECT says what the other
  is (`CONNECT VOICE`); two voice ends exchange 20 ms frames (`'A'` 8000 Hz mu-law, `'D'` a
  digit), paced by the wall clock in `modem_voice_poll()` (from `modem_read`); a voice end facing
  a modem hears its tones and sends nothing; a data modem answered by a voice waits out S7: NO
  CARRIER. The handset (status bar modem menu: pick up / answer / call a number / hang up;
  `char_modem_handset()`) is the phone beside the modem: the host's speakers
  (`modem_sound_voice()`) and microphone (`src/sound/snd_mic.c`, OpenAL capture at 8000 Hz,
  static `AL_LIBTYPE_STATIC` like openal.c); it answers or dials as a voice call of its own (no
  result codes, ATH does not end it), joins the guest's voice call, hears dial/ring/busy tones;
  an incoming RING also rings the phone (`MODEM_SOUND_BELL`, heard whatever ATM says when the
  Speaker option is on). Tests: `tests/modem/voice_test.c` (Modem.voice), `exchange_test.c`.
  Also the ITU's set, V.253 (`modem_v253_command()`): `+FCLASS=8`, `+VLS` (V.253 numbering: 0 on
  hook, 1 the line -- off hook, answering a call ringing -- 2 handset, 4 speaker, 6 mic, 7/13
  speakerphone, mapped onto Rockwell's `vls`), `+VSM` (0/1 signed/unsigned 8-bit, 2/129 16-bit,
  4 mu-law, 5 A-law, 128 8-bit; `+VSM=?` lists names, which vgetty matches), `+VTX`/`+VRX`
  (`<DLE>!` stops), `+VTR` full duplex (`<DLE>^`), `+VTS` (10 ms units, `{d,len}`, `+VTD`),
  `+VSD`, `+VGT/+VGR/+VRA/+VRN`, `+VIP`; others accepted. `vset` says which set the DTE spoke last:
  V.253 answers OK where Rockwell says VCON, and reports silence as `q` after a voice, `s` before.
  The voice tests' guests must keep real time: `timeBeginPeriod(1)` (Windows' 15.6 ms Sleep starves
  mu-law at 8 KB/s).
- The status page's phone (`src/network/isp/isp_phone.c`, the "Phone" section): `/api/phone` is a
  WebSocket (Host and Origin must be the page's own; SHA-1/base64 handshake in-file) that
  `http_serve()` hands over; each is a thread registering on the exchange over loopback as any
  modem (asks for 555-0100, label "Status page (browser)"), dialling/answering as VOICE, relaying
  JSON commands/events and 8000 Hz 16-bit audio (binary) to and from the browser, which resamples
  in an AudioWorklet (the page's audio runs at the browser's rate: Firefox will not mix rates) and
  plays the ring/ringback/busy tones itself. The microphone needs the page as 127.0.0.1/localhost
  (a secure context). `isp_srv` links `src/char/modem_voice.c` for it. Test:
  `tests/isp/web_phone_test.c` (Isp.web_phone: the test is the browser and a modem).
- isp-server (`src/network/isp/`, its own exe; the emulator does not link it): the virtual ISP
  and telephone exchange modems reach over the network. `isp_srv.c` is the server (listeners,
  exchange, .ini; start/stop for in-process tests), `isp_server.c` only its command line. A
  connection opening with `86BOX-EXCHANGE 1 ` is the exchange's (REGISTER / DIAL / ANSWER, see
  `isp_srv.h`): numbers from 555-0101 (the one asked for if free), a dial rings the line whose
  number it is or ends with (prefixes), RINGING/CONNECT/BUSY/NOANSWER/UNKNOWN, the answer's
  connection handed to the caller's thread and bridged; other numbers reach the ISP (switchable;
  explicit ISP numbers); dialling oneself is BUSY. Anything else is a plain TCP line: PPP from the
  first byte. The ISP: one session per call: PPP server
  (`ppp_framing.c` RFC 1662, `ppp_session.c` RFC 1661 LCP passive until the guest's first request,
  optional PAP accepting anything, IPCP with address and DNS; rejects PFC/ACFC/callback/VJ/NBNS,
  Protocol-Rejects CCP/IPX/NBF) and a libslirp instance per session (`isp_nat_slirp.c`: synthetic
  Ethernet/ARP, wall-clock timers, select()/poll() -- not WSAEventSelect, which loses FD_CONNECT --
  and no "readable" fallback, which makes libslirp send the guest ICMP unreachables). Session N
  gets 10.86.N.0/24: guest .15, gateway .2 (= host loopback), DNS .3; guests reach each other
  (guest LAN, `isp.c` routes between sessions). Each session has its own thread; other threads only
  copy bytes or snapshots under its lock (global lock before session lock). Its GUI is a web page
  on 127.0.0.1:2324 (`isp_web.c`, `isp_web_page.html` embedded by `embed.cmake`), opened at start
  (`--no-open` to skip -- always use it when testing): calls, the exchange's phone book and
  modem-to-modem calls (hang-up), the exchange's settings, hang-up, per-call-number port
  forwards (libslirp hostfwd), settings (guest LAN, modem-speed throttle, PAP, keepalive, range);
  Host-header check and an `X-ISP-Request` header on changes against rebinding/CSRF. Settings and
  forwards persist in `isp-server.ini` next to the exe (`--config`). Tests: `tests/isp/` (framing,
  PPP automaton, sessions and controls through libslirp to host sockets; `isp_web_test.c` the
  page's API; `isp_probe` drives a running isp-server, `--internet NAME` adds DNS + HTTP, `--hold
  SECS` keeps a call up; `exchange_test.c` the exchange in-process with sockets as modems),
  `tests/modem/` (transport, two modems, calls to isp-server's core behind the fake sockets;
  `phone_test.c`: two real modems with real sockets ringing each other through the real exchange)
  and `tests/network/` (`net_slirp.c` behind a stub card).
- Roland Sound Canvas MIDI out (`src/sound/midi_soundcanvas.c`, replacing upstream's CLAP-plugin
  device and `src/sound/clap/`; sync-upstream.yml keeps both ours (KEEP_OURS / KEEP_DELETED),
  upstream changes to the SOUNDCANVAS blocks of the CMake files or midi.h can still conflict):
  88emu from gearmulator (GPLv3, so builds with it are GPLv3), vendored in
  `src/sound/emu88/gearmulator/` (`VENDORED.md`: commits, MinGW patches, how to update), built
  as the `emu88` static library (`src/sound/emu88/CMakeLists.txt`; framework sources C++17, the
  rest C++20, `-ffast-math`, asmjit JIT). `emu88_host.h/.cpp` is the C glue: catalogue (88emu's
  24 boards, config key = 88emu's CLI id), per-slot ROM status from `RomInventory`, boards
  created powered and booted in the first `emu88h_render()` on the MIDI thread (never the
  emulation/UI threads), power/standby, panel buttons/encoder/LEDs, LCD textures
  (`emu88_lcd.cpp`, ported from 88emuPlayer). ROMs: `roms/soundcanvas` under every ROM path,
  recursive, identified by MD5+size. One device ("Roland Sound Canvas", internal name
  `soundcanvas`; config `model` = key, legacy numeric values map to SC-55/SC-55mkII,
  `factory_reset`, `fast_boot`, `output_gain` = the panel's knob, `panel_x/y/width`).
  The board outlives a device close for 3 s (hard resets re-create devices): the next init with
  the same options takes it back, so the synth keeps running through a PC reset.
  Qt (`src/qt/qt_soundcanvas.cpp`): Settings > Sound > MIDI Out > Configure opens
  `SoundCanvasConfigDialog` (synth list with availability lamps, ROM table with lamps);
  `SoundCanvasPanelManager` (Tools > Sound > "Roland Sound Canvas panel") polls
  `soundcanvas_get_board()` and opens `SoundCanvasPanel` once per new board (no focus steal):
  88emuPlayer's artwork without the playlist (`src/qt/soundcanvas/*.png`, 2x, generated from
  88emu's assets; SVG faces pre-rendered to PNG -- the dynamic Qt has no Svg module), its
  controls and key bindings in the skin's 612 x 187 dp. Tests: `tests/soundcanvas/`
  (SoundCanvas.emu88; board tests need `BOX86NEXT_SC_ROMS`).
- Upstream's CI workflows and Dependabot are disabled/removed in this repo; only
  `sync-upstream.yml` runs.
- Build number in the title bar / About box: `.build-number` (git-ignored, repo root)
  holds the last successful build; `cmake/NextBuildNumber.cmake` bumps it on every
  successful link, so a no-change build still relinks with the next number.
  A release build: `NEXT_RELEASE=1 cmake --build build-static` -- the title bar reads
  "86Box-Next 7.0 - Release 1" (the About box and the log keep the build number too). It is
  read from the environment on every build, so the next build without it is a plain build
  again. Every release: `tools/make-release.sh N` builds Release N, keeps its zip in
  `F:\Claude\86Box-Next\RELEASES-ARCHIVE` (one per release, never overwritten) and stages
  exactly those exes in the rigs (`isp-server.exe` in `Latest`); then tag `release-N`, push it,
  and `gh release create` with the zip (the script prints both).

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
Tests live in their own tree, `build-tests` (`-DBUILD_TESTING=ON -DQT=OFF -DSTATIC_BUILD=ON`,
GTest from `mingw-w64-ucrt-x86_64-gtest`): build a suite's target there, then
`ctest --test-dir build-tests -R <Name>` (ours: `Isp|Modem|Network`). Keep build-static's
BUILD_TESTING off: some upstream tests (pcjx, fdc_read_id) do not build, which breaks a full build.
