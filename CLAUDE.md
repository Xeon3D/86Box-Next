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
  (`net_te100pc16.c`), 3Com 3C562D LAN+33.6 Modem (`pccard_3c562.c`), the first
  multi-function card: `pccard_mfc.c` is a PC Card 95 MFC framework (per-function COR/CCSR/
  I/O base/limit registers, I/O routed by each function's decode, IREQ = OR of the functions),
  CIS with LONGLINK_MFC chains (Windows 9x's ID walk follows function 0's chain); its LAN is
  `net_3c509b.c`'s `threec562_lan_device`, its modem the modem engine on a detached
  16550 (`serial_init_detached()`, no COM slot) opened with `char_modem_attach()` (its own
  `char_modem_model_t`, label "PC Card A/B") in the card's device context, so its line is saved in
  the card's section. Verified in Win98 (NET3C562.INF + MDMGATEW.INF, ping, ATI over ReadFile); caveat:
  Windows tends to give its modem COM4 at 2E8h, which the S3 cards' 8514/A register shadows
  (`vid_s3.c`, as on real hardware) -- reads come back 00h; move the modem's I/O in Device
  Manager; see `mfchandoff.md`. Hayes Accura 56K PC Card (`pccard_accura56k.c`): a V.90
  data/fax modem, single function -- a detached 16550 with the modem engine on it
  (`char_modem_attach()`), COR/CCSR at 200h (index 0 memory-only, any other I/O; the UART decodes
  A2-A0), CIS (`pccard_cis_accura56k()`): FUNCID serial, COM1-COM4 on ten lines (indexes 1-4) or
  anywhere on three (5), matched to MDMHAY2.INF's `PCMCIA\Hayes-Accura_56K_PC_CARD-EBBB`; ATI
  answers plausible, not dumped; not yet tried in a guest. Adaptec APA-1460 SlimSCSI (`scsi_apa1460.c`): the AIC-6360 of
  `scsi_aic6360.c` (`aic6360_chip_init()`: the chip without ISA I/O or IRQ), COR at 2000h as its
  Technical Reference has it (SRESET/IOEN/PRIMARY, so CIS indexes 09h = 340h, 08h = 140h), PORTA/B
  read FFh; Windows 98's SCSI.INF ID `...-BE89` (SPARROW.MPD). Its SCSI bus is its socket's
  (`pcmcia_scsi_bus()`), kept from the hard reset (or first insertion) to the next, after every
  other device's -- `scsi_plan()` lists it, so Settings > Hard disks names it. Verified in Win98: a
  disk on its bus as E:, read and written. No memory cards (the SRAM and Intel flash cards
  were removed, 2026-10-07). CIS in `pccard_cis.c`: built there, with NO_LINK, and a 2-byte CONFIG filler subtuple solved at
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
- COM port modems (`src/char/char_modem.c`, PeepeeBox's): Diamond SupraExpress 56e PRO,
  ELSA MicroLink 56k and a Standard Hayes-compatible 56k Modem (`modem_hayes`: generic ATI
  answers matching none of funworld's rows, data/fax, no voice), chosen as a COM port's device
  in Settings > Ports. Upstream's network-card modem (`net_modem.c`) is removed; a config with it
  gets `modem_hayes` on that COM port (`load_network()`). Serial Plug and Play (a model's `pnp_id` /
  `pnp_name`; only the Supra: `SUP2311` "SupraExpress 56e PRO", Diamond's SUPIV92.INF): DTR rising with
  RTS off, then RTS rising at least 50 ms later (Windows' serenum sequence) queues the PnP string
  (7-bit, rev 1.00, `\MODEM\<name>`, checksum); `ATI9` answers it too. AT engine with ATI
  answers that funworld's modem table resolves to exactly one part; the line is dead or dials
  a TCP host (any number), with a real call's timing and sounds (`modem_sound.c`, a pool of
  speakers whose sound handlers are registered once per sound reset: `sound_has_handler()`).
  `DEVICE_HOTPLUG`, so attaching, removing or reconfiguring one is a soft change (the soft
  settings path refreshes the status bar). Status bar modem icon (`src/qt/qt_modem_menu.cpp`,
  shown while a COM port is configured with a modem or a device carries one -- the status bar's
  75 ms poll notices a carried modem come or go; `char_modem.h` slots: the COM ports, then the
  carried modems) sets each modem's line live through
  `char_modem.h` (applied on the emulation thread; a call in progress gets NO CARRIER) and saves
  it in the modem's section (`<name> #<COM+1>`). Tests: `tests/modem/` (FN_SYS's tables).
  The modem engine and the PC Card code know nothing of each other: another device carries a
  modem with `char_modem_attach(port, model, label)` (a `char_modem_model_t`: ATI answers and
  `voice`; settings from `CHAR_MODEM_CONFIG_LINE` / `CHAR_MODEM_CONFIG_SPEAKER` in its own
  config), and `pcmcia.c` knows a modem card only as a device in its list. Only the modem cards
  themselves (`pccard_3c562.c`, `pccard_accura56k.c`) include both.
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
- Internal modem card (`src/char/modem_card.c`, Settings > Other peripherals > Internal modem, one
  per machine, `modem_card` in `[Other peripherals]`, ISA, hard reset; the `io_board.c` pattern):
  Diamond SupraExpress 56i Sp (`supra56i`) -- the modem engine (`char_modem_attach()`, label
  "Internal (ISA)", voice) on a detached 16550 decoded on the ISA bus, IRQ via `picint_common()` as
  `serial_do_irq()` does; config `mode` 0 = ISA PnP card `SUP2171` (built-in ROM: IRQ 3/4/5/7/9-12/15,
  3E8h/2E8h/3F8h/2F8h or 100h-3F8h; inactive until configured), 1-4 = jumpered COM1-COM4 address
  with `irq`. Windows 9x: Diamond's SUPIV90.INF / MDMISUPV.INF (`*SUP2171`); Linux 8250_pnp knows
  the ID. (Diamond's "56i PRO" is a PCI HCF soft modem, not this.)
- Voice calls (models with `voice`: Supra and ELSA, not the PC Cards' data/fax modems): Rockwell's `#` voice set, as Windows 9x's
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
- The status page's phone (`isp-server/isp_phone.c`, the "Phone" tab): `/api/phone` is a
  WebSocket (an admin's request from the page's own Origin, `isp_web_phone_allowed()`; SHA-1/base64
  handshake in-file) that `http_serve()` hands over; each is a thread registering on the exchange over loopback as any
  modem (asks for 555-0100, label "Status page (browser)"), dialling/answering as VOICE, relaying
  JSON commands/events and 8000 Hz 16-bit audio (binary) to and from the browser, which resamples
  in an AudioWorklet (the page's audio runs at the browser's rate: Firefox will not mix rates) and
  plays the ring/ringback/busy tones itself. The microphone needs a secure context (127.0.0.1,
  localhost, or HTTPS through a proxy). `isp_srv` links `src/char/modem_voice.c` for it. Test:
  `isp-server/tests/web_phone_test.c` (Isp.web_phone: the test is the browser and a modem).
- isp-server (`isp-server/`, a **git submodule**: its own repository, `Xeon3D/ISP-Server`, public,
  with the history from src/network/isp on; its own exe and tests in `isp-server/tests/`; the
  emulator does not link it): the virtual ISP and telephone exchange modems reach over the network.
  Work on it in `isp-server/` as in any clone of that repo -- commit and push there first, then
  commit the new submodule pointer here. Fresh clones need `git submodule update --init`. It
  carries its own copies of `modem_voice.c` (`voice/`) and `modem_voice.h` (`include/86box/`):
  keep them in step with `src/char/modem_voice.c` and its header.
  `isp_srv.c` is the server (listeners, exchange, .ini; start/stop for in-process tests),
  `isp_server.c` only its command line, `isp_ui.h` its face: on Windows (`isp_ui_win.c`, a WIN32
  GUI exe, icon/manifest in `isp-server.rc`) a log window instead of a console -- minimizing hides
  it to a notification-area icon (click: the log; right-click: status page / Exit), closing it or
  WM_CLOSE (plain `taskkill`) stops the server cleanly, `--minimized` (or a "Run: Minimized"
  shortcut) starts in the tray, usage/errors go to the parent console or a message box, and with
  stdout redirected to a file the log goes there too; elsewhere (`isp_ui_term.c`) the terminal. A
  connection opening with `86BOX-EXCHANGE 1 ` is the exchange's (REGISTER / DIAL / ANSWER, see
  `isp_srv.h`): numbers from 555-0101 (the one asked for if free), a dial rings the line whose
  number it is or ends with (prefixes), RINGING/CONNECT/BUSY/NOANSWER/UNKNOWN, the answer's
  connection handed to the caller's thread and bridged; other numbers reach the ISP (switchable;
  explicit ISP numbers); dialling oneself is BUSY. Anything else is a plain TCP line: PPP from the
  first byte. The ISP: one session per call: PPP server
  (`ppp_framing.c` RFC 1662, `ppp_session.c` RFC 1661 LCP passive until the guest's first request;
  rejects PFC/ACFC/callback/VJ, Protocol-Rejects IPX/NBF) with: authentication none / anyone /
  accounts (`isp_settings_t.auth`, `auth_protos`; accounts in `isp.c`, `isp_account_password()`,
  names case-blind, `DOMAIN\` dropped as a fallback) by PAP, CHAP-MD5, MS-CHAP, MS-CHAP-2 (with
  the S= authenticator response; only with accounts) and CHAP-SHA1/256/384/512/SHA3-256/384/512
  (IANA 6-12), strongest asked first, the guest's NAK'd choice taken if allowed (`ppp_auth.c`: the
  arithmetic; `isp_crypto.c`: MD4/MD5/SHA-1/2/3, DES, RC4, HMAC, PBKDF2, OS random -- no library);
  IPCP DNS and WINS (`wins[2]`); CCP (`ppp_comp.c`) offering one method at a time (MPPE/MPPC option
  18, Deflate via zlib, BSD-Compress ported from RFC 1977's code, Predictor-1) and taking the
  guest's first allowed one; MPPE 40/56/128, stateless/stateful, keys from MS-CHAP (RFC 3079: NT
  hash for 128, LM hash for 40/56) or MS-CHAP-2, off/allowed/required (required: no IP until CCP
  encrypts both ways). Stateful MPPE follows Linux's kernel (proven against Windows): FLUSHED on
  flag packets and on the answer to a Reset-Request means a key change; an MPPC flush's FLUSHED
  only re-inits RC4 (RFC 3078) -- see the comments in `mppx_compress()`. Multilink (RFC 1990,
  `ppp_mp.c` reassembly/fragmenting; on by default): a link with MRRU asks `link_ready()`, and joins
  the call whose guest has the same name + endpoint discriminator (`isp.c` bundle_*): the member
  keeps LCP/auth and hands packets to the owner's `mpq` ring; the owner's IPCP/CCP/NAT serve the
  bundle and write MP fragments into members' output rings; the owner's end hangs up its members.
  pppd 2.5 bug to remember: its MS-CHAP (v1) MPPE receive key is zeroed (mppe_set_chapv1 passes one
  buffer as both keys), so v1+MPPE cannot be tested against pppd. And a libslirp instance per session (`isp_nat_slirp.c`: synthetic
  Ethernet/ARP, wall-clock timers, select()/poll() -- not WSAEventSelect, which loses FD_CONNECT --
  and no "readable" fallback, which makes libslirp send the guest ICMP unreachables). Session N
  gets 10.86.N.0/24: guest .15, gateway .2 (= host loopback), DNS .3; guests reach each other
  (guest LAN, `isp.c` routes between sessions). Each session has its own thread; other threads only
  copy bytes or snapshots under its lock (global lock before session lock). Its GUI is a web page
  (`isp_web.c`, `isp_web_page.html` embedded by `embed.cmake`) on `http_listen` (127.0.0.1 by
  default, `--http-listen`), port 2324, a thread per request (16 at most), opened at start
  (`--no-open` to skip -- always use it when testing, with `--minimized`, so no window takes
  focus). Tabs: Status (calls with auth/CCP/encryption/Multilink, a summary), Phone (the browser
  phone, the exchange's lines/calls/settings), Port forwards (libslirp hostfwd per call number),
  Settings (network, who gets in + dial-in accounts, MPPE, compression, WINS, Multilink), Log
  (`/api/log?after=N`, a 2000-line ring in `isp_srv.c`), Users. Logins (`isp_users.c`): with no users
  the page is for loopback only (Host must be a loopback name: anti-rebinding), role "local";
  the first user is the super admin (from loopback, or remotely with the setup token logged at
  start); super adds admins (all but users) and viewers (status + log); PBKDF2-SHA-256 hashes, a
  SameSite=Strict HttpOnly cookie token (sha256 kept), 12 h idle / 7 days max, 5 wrong in a minute
  locks logins for 30 s. Changes need `X-ISP-Request` (CSRF). Settings, forwards, accounts
  (plain passwords, pct-encoded: CHAP needs them) and users persist in `isp-server.ini` next to
  the exe (`--config`; `isp_config.c` has the words). The ISP-Server repo builds on its own
  (`cmake -S .`; Linux, macOS, Windows/MSYS2), its `Dockerfile` (context: its root; Alpine,
  libslirp built static, tests run, a static musl `--target linux-binary` too; image
  `xeon3d/86box-next-isp`, amd64+arm64) and its `.github/workflows/build.yml` (manual: macOS
  arm64/x86_64 with dylibbundler, Linux static amd64/arm64, optional release upload). WSL's Ubuntu
  has docker, pppd and the build deps; root via `wsl -u root`. Tests: `isp-server/tests/` (framing,
  PPP automaton, sessions and controls through libslirp to host sockets, every auth method end to
  end (`test_auth`); `crypto_test.c` RFC vectors; `comp_test.c` every codec with itself, lossy too;
  `isp_web_test.c` the page's API incl. roles; `isp_probe` drives a running isp-server, `--internet NAME` adds DNS + HTTP, `--hold
  SECS` keeps a call up; `exchange_test.c` the exchange in-process with sockets as modems;
  `pppd_interop.sh`, run as root on Linux, Linux's pppd as the guest for every method, MPPE,
  Deflate, BSD-Compress and Multilink with checked transfers),
  `tests/modem/` (transport, two modems, calls to isp-server's core behind the fake sockets;
  `phone_test.c`: two real modems with real sockets ringing each other through the real exchange)
  and `tests/network/` (`net_slirp.c` behind a stub card).
- Roland Sound Canvas MIDI out (`src/sound/midi_soundcanvas.c`, replacing upstream's CLAP-plugin
  device and `src/sound/clap/`, and MUNT's MT-32/CM-32L devices (removed with `src/sound/munt`,
  the MUNT options; `load_sound()` moves `mt32`/`mt32_new`/`cm32l`/`cm32ln` to `soundcanvas`
  with the same board); sync-upstream.yml keeps both ours (KEEP_OURS / KEEP_DELETED),
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
  `factory_reset`, `fast_boot`, `output_gain` = the panel's knob, `panel_x/y/width`, `panel_scale` =
  the size the panel opens at, 25-200 % of the 612 x 187 dp skin, 0 = last size; set in the
  Configure dialog, applied to an open panel at once, no board restart).
  The board outlives a device close for 3 s (hard resets re-create devices): the next init with
  the same options takes it back, so the synth keeps running through a PC reset.
  Qt (`src/qt/qt_soundcanvas.cpp`): Settings > Sound > MIDI Out > Configure opens
  `SoundCanvasConfigDialog` (synth list with availability lamps, ROM table with lamps and each
  file's MD5, the known dumps' in its tooltip); `SoundCanvasPanelManager` polls
  `soundcanvas_get_board()` and opens `SoundCanvasPanel` once per new board (no focus steal);
  the status bar's first icon, a piano (`midi.ico`, while MIDI out is the Sound Canvas, lit while
  its board runs, `MachineStatus::setSoundCanvas()`), opens `SoundCanvasPanelManager::menu()`:
  Show front panel, "Show the panel when the synth starts" (= Preferences > Emulator's "Don't
  show the Roland Sound Canvas panel...", global `soundcanvas_hide_panel`), Configure... (the
  same dialog, applied paused with `midi_config_changed()` as Settings' soft path does). The panel:
  88emuPlayer's artwork without the playlist (`src/qt/soundcanvas/*.png`, 2x, generated from
  88emu's assets; SVG faces pre-rendered to PNG -- the dynamic Qt has no Svg module), its
  controls and key bindings in the skin's 612 x 187 dp. Tests: `tests/soundcanvas/`
  (SoundCanvas.emu88; board tests need `BOX86NEXT_SC_ROMS`).
- Settings > Machine shows each board's BIOS vendor (Award, AMI, Phoenix, MR BIOS, IBM, Compaq,
  in-house OEM BIOSes...; boards with selectable BIOSes list the default first) from
  `src/machine/machine_bios_vendor.c`, generated by `tools/machine-bios-vendors.py <rig dir>`
  from the ROM images' strings plus its hand-checked OVERRIDES; rerun it when upstream adds
  machines (it names any it cannot place; those show "Unknown").
- Soft settings changes (no PC reset, `Settings::accept()`'s soft path): a PC Card put in, taken
  out, swapped or reconfigured (settings page hands it to `pcmcia_request_card()` /
  `pcmcia_request_reinsert()`; only fitting/removing the controller is hard; `tests/pcmcia/slots_test.c`);
  every COM/LPT port device and its options, unless a non-hotplug device (a serial mouse) holds
  the port (the LPT sound devices are `DEVICE_HOTPLUG`: their
  close unregisters handlers with `sound_remove_handler()`/`music_remove_handler()`, OPL2/3LPT
  closes its OPL with `device_close_priv()`); MIDI out/in and the standalone MPU-401 on ISA
  (`mpu401_standalone_hotplug()`; MCA stays hard). Port enable/disable and the ECP DMA jumper
  stay hard (the UARTs/ports are made at the hard reset).
- Status bar icon order (Preferences > Status bar, `qt_preferencesstatusbar.cpp`: a list of the icon groups --
  Sound Canvas, cassette, cartridges, floppy, CD-ROM, removable disk, MO, tape, network, hard
  disks, PC Card, USB, modems, sound, dynarec -- drag or Move up/down, Default order): saved as
  `status_icon_order` (comma-separated keys from `MachineStatus::iconGroups()`, blank = default;
  unknown keys dropped, missing groups follow in default order) with the other emulator
  preferences; `MachineStatus::refresh()` collects each group's widgets, then adds them in
  `iconOrder()`; `Preferences::save()` saves that page before the emulator page, whose save
  rebuilds the status bar. A new icon group needs a key there and a name in `iconGroupName()`.
  Preferences OK replaces `MachineStatus`; `adoptMenus()` carries the PC Card/USB/modem/Sound
  Canvas hooks over (they were lost before).
- 86Box Guest Tools (Settings > Other peripherals, `guest_tools_enabled` in `[Other peripherals]`,
  any machine, hard reset; device `guest_tools` with `pointer` and `clipboard` options): our own
  VMware Tools-like integration -- no VMware backdoor. `src/device/guest_tools.c` is a two-port channel
  at 86B0h (`guest_tools_proto.h`, shared with the agents: command port reads "86BX", the data port is
  a byte stream, any access width so 8088 DOS code can use it too; every reply starts with a status;
  HELLO / POLL / CLIP_GET / CLIP_SET / BYE). `check_io_perm` (`386_common.h`) lets every privilege
  level reach those ports while the device is in (`guest_tools_io_bypass()`), so agents need no
  kernel driver, NT included. Pointer integration: the agent polls the host pointer (0-65535 across
  the guest display, from `RendererStack::guestToolsPointerMoved()`) and moves the guest's own
  pointer; while it does (`guest_tools_pointer_active()`: features on, the agent's last POLL under
  2 s old, mouse input mode 0) the UI never captures (`GuestTools::poll()` lets go of a capture),
  hides the host pointer over the display and feeds only buttons and the wheel to the machine's
  mouse (PS/2, serial...); `guest_tools_keyboard()` counts as capture for "Keyboard requires
  capture". The keyboard follows the pointer: off the guest's display (`guest_tools_keyboard_outside()`)
  `keyboard_input()` drops new presses (releases of keys down in the guest still pass), the Windows
  low-level hook and the Qt key filter let the host have the keys, and `GuestTools::poll()` hands
  the focus to another 86Box-Next window (same exe name) under the pointer -- only between machines,
  and without raising it: it posts that window a registered message ("86Box-Next.GuestTools.TakeFocus")
  after AllowSetForegroundWindow, and the window's `FocusFilter` (a native event filter) takes the
  foreground itself while adding SWP_NOZORDER to its WM_WINDOWPOSCHANGING for 250 ms. Clipboard: UTF-16 text, LF line ends, 1 MB; `src/qt/qt_guesttools.cpp` keeps a
  snapshot of the host clipboard for the emulation thread and drops its own echoes. Agents in
  `guest-tools/` (built in WSL: `wsl -e sh guest-tools/build.sh` -> `guest-tools/out/`
  86BOXGT.EXE, a 1.44 MB floppy image and an ISO): `windows/gtagent.c`, one Win32 exe for
  Windows 95 / NT 4.0 and later, no C runtime (KERNEL32/USER32/ADVAPI32/SHELL32/WINMM only,
  subsystem 4.0), tray icon, `/install` (copies itself to the Windows directory + Run key) /
  `/uninstall` / `/quiet`; stops following when a program confines the pointer (ClipCursor).
  Tools > "Install 86Box Guest Tools..." (`GuestTools::insertMedia()`): the images are built in
  (`src/qt/guesttools.qrc` from `guest-tools/images/`, committed: build.sh writes them with fixed
  dates/serial so they only change with the tools; Q_INIT_RESOURCE is needed -- `ui` is a static
  library), written to `<global data dir>/guest-tools/86box-guest-tools-<md5 prefix>.iso|img`
  and mounted in the first CD-ROM drive (else the first floppy, write-protected); off tools get
  turned on first (hard reset, as Settings does). The CD carries AUTORUN.INF (open=86BOXGT.EXE).
  Installing from a disk: the Win32 agent stops a running agent and waits for its process, clears
  the read-only bit a CD copy carries, copies, then starts the installed copy and exits; GTWIN keeps
  running itself (Windows 3.x would take the installed copy for a second instance of the disk's
  module), with code and icon PRELOAD FIXED so the disk can go. debug_cmd `gt insert` runs it.
  `mkicon.py` makes its BMP-format icon (Windows 95 reads no PNG icons; `--win16`: one 32x32
  16-colour image). `win16/gtwin.c`: GTWIN.EXE for Windows 3.1 / WfW 3.11 (Open Watcom, WATCOM
  defaults to WSL's ~/ow), 16-bit port I/O, an icon window with the toggles in its window menu,
  Windows-1252 clipboard text (16000 chars), `/install` copies it to the Windows directory and adds
  it to WIN.INI `load=`; polls on a 55 ms timer and from the message loop (PeekMessage) for 1 s
  after a move. Tested in Windows 95, 98 SE, NT 4.0, 2000 and WfW 3.11 (pointer, clicks, both
  clipboard directions, autostart). A DOS agent was tried and shelved at the owner's request
  (source kept outside the repo, `F:\Claude\86Box-Next\guest-tools-dos-wip`; notes: INT 33h
  function 04h raises no motion event, so event-driven programs like EDIT need a real motion
  packet; a hook must `iret`, not `retf 2`, or the caller keeps interrupts off). debug_cmd
  `gt pointer x y` / `gt buttons mask` / `gt motion dx dy` / `gt clip text` (`|` = new line;
  switches to a stand-in host clipboard and logs the guest's) / `gt state`.
  Files (device option `files`, on; follows the clipboard's directions; `GT_FEAT_FILES`, agent cap
  `GT_CAP_FILES`): host to guest, CLIP_GET answers `GT_CLIP_FILES` (u32 count, then flags/u64
  size/name per entry, names relative with '\', folders before their contents) when the host
  clipboard holds local files (`hostFiles`, from `QMimeData::urls()`); `FILE_READ` reads 64 KB
  chunks from a snapshot taken at that moment (`GuestTools::filesList()`, emulation thread). The
  Win32 agent offers CF_HDROP with delayed rendering and only copies the files into
  `%TEMP%\86BOXGT\<tick>` on WM_RENDERFORMAT (the previous copy is deleted then; the folder is
  emptied at agent start); names are made safe there (no `..`, bad characters -> `_`). Guest to
  host: a CF_HDROP on the guest clipboard starts an upload thread (FILES_BEGIN / FILE_ADD / FILE_WRITE
  with REP OUTSD / FILES_END) into `<global data dir>/guest-tools/clipboard/<pid>-<n>` (the UI checks
  every name, `safeName()`), and FILES_END `GT_FILES_CLIPBOARD` puts the top-level entries on the
  host clipboard as URLs + "Preferred DropEffect" copy; `ownFiles` keeps that from coming back to the
  same guest, other machines see it as host files (machine to machine). Limits 20000 entries / 2 GB.
  The agent uses the W file APIs only through GetProcAddress on NT (Windows 95 has stubs), ANSI on
  9x. Drag and drop (same option): host to guest, `RendererStack` accepts drops of local files from
  outside the program while integrated (`guestToolsDragFiles()`; the OpenGL/Vulkan windows forward
  drag events in `eventDelegate`), feeds the pointer from the drag positions and sets
  `guest_tools_host_drag()` 1/2/0; POLL says `GT_POLL_DRAG`/`GT_POLL_DROP` with a drag seq, the agent
  takes the list with DRAG_GET and runs `DoDragDrop` with its own IDataObject (CF_HDROP, rendered on
  GetData with `GT_FILE_DRAG_INDEX` reads) and an IDropSource whose QueryContinueDrag follows the
  host (drop / cancel); the poll thread wiggles the cursor when the host's state changes so the drag
  loop sees it. Guest to host: POLL says `GT_POLL_DRAG_OUT` while the host pointer is off the display
  with the guest's left button down; the agent opens a 3x3 topmost drop target under the guest
  pointer ("catcher"), a guest drag of files entering it uploads them (FILES_END `GT_FILES_DRAG`) and
  reports `GT_STATE_DRAG_OUT` once FILES_BEGIN went through; the UI (`GuestTools::poll()`, 50 ms)
  then lets go of the guest's left button (the guest drops on the catcher, copy) and starts a QDrag
  whose `GuestDragData` waits in `retrieveData()` (`waitDragOutFiles()`, a nested event loop) until
  the upload is done. OLE32 + uuid are linked into the agent. `GT_CMD_LOG` writes an agent's line to
  the emulator log (`gt_log()` in the agent). debug_cmd `gt files a|b` (stand-in host clipboard
  files; the guest's files are logged, not put on the host clipboard), `gt drag a|b` / `gt drop` /
  `gt dragleave`; a drag out in debug mode logs the files instead of starting a QDrag.
  Versions: `src/include/86box/guest_tools_version.h` (1.1.<revision>, shared by both agents and the
  emulator); `guest-tools/bump-revision.py`, run by build.sh, raises the revision when the agents'
  sources hash differently from `guest-tools/images/SOURCES` (commit both with the images). The
  version is in each EXE's VERSIONINFO, the About box and the tray tooltip, and goes in HELLO (u32
  after the caps; older agents send none -> 1); `guest_tools_agent_version()` gives it to the UI,
  which names the Tools menu item "Install 86Box Guest Tools 1.1.N..." or, for an older agent,
  "Update 86Box Guest Tools (old -> new)..." (`GuestTools::updateInstallAction()`); `gt state`
  logs it. Raise the minor version by hand for new features.
- STB video cards from STB's own release BIOSes (in `ROMS-EXTRA`, not the 86Box ROM set):
  - S3 ViRGE/MX (86C260, 5333:8C01), STB Symmetry 3D (`vid_s3_virge.c`, `S3_VIRGEMX`: the GX2
    core, 4 MB SGRAM, ROM `video/s3virge/stb_symmetry3d_m5_17_14.rom`). The datasheet's paired
    CRTC registers (`s3_virge_mx_paired`) exist per controller: SR26 bit 3 blocks Controller 1
    writes, bit 2 writes Controller 2 (40h C1, 44h both, 4Eh/4Fh C2 = the panel/TV CRTC), bits 1/0
    read from C2. C2 is a read-back shadow (`crtc_iga2`), only C1 is displayed -- without it
    Windows' panel timings overwrote the CRT's.
    The ROM (1.0B0) holds its sign-on 20-25 s by itself. Also from TheRetroWeb, AGP: an unbranded
    ViRGE/MX (`virge_mx_agp`, S3's BIOS 3.05.01-26, `video/s3virge/s3_virge_mx_3.05.01-26.rom`)
    and the ViRGE/MX+ Spectrah LD-S500 (8C03h, `virge_mx_plus_spectrah_ld_s500`, BIOS A1.3A,
    `video/s3virge/spectrah_ld-s500_a1.3a.bin`); S3's BIOS waits for SR46 bit 6 (panel powered
    down), and SR01 bit 3 (half dot clock: 320x200-512x384) doubles pixels. Windows, 2026-10-16
    (both AGP cards, 440 boards): MX+ with S3VMX 4.11.01.3009 on Win95/98 SE, NT4 SP6a (S3 3.28.05;
    SP1 has no AGP) and Win2000 SP4's inbox driver all run 1024x768. The generic MX on Win2000 and
    NT4 is fine too, but on Win95/98 (the Win98 CD's `drivers\display\s3\86c260m5`) only a
    640x480 window of a larger desktop shows: S3's BIOS (a laptop one) always reports an LCD
    to 4F14h, the BIOS boots CRT-only (CR6B=01) and the driver then programs C1 with 640x480
    timings. The STB BIOS boots in duoview (CR6B=83) and is fine. Genuine pairing, not emulation.
  - C&T 65554, STB Monocle (`vid_chips_69000.c`, `CHIPS_65554`: the 69000 code with 1/2/4 MB
    external DRAM that wraps (the BIOS sizes it so), ROM `video/chips/stb_monocle_65554_110.bin`).
    The BIOS and Windows run the panel path (FR01 = 2) with no panel: the 65554 shows the CRTC
    timing, not the picture cut to the panel. Interlaced modes halve all vertical timings.
    Its packed 16-colour modes (20h-2Ah: GR05 bit 6 without AR10 bit 6) show each nibble as a
    pixel. SciTech VBETest draws those modes with planar writes (it resets GR05), so its 4-bit
    "Banked+Linear" tests look wrong -- VBETest's doing, not the card's. Win98 SE's built-in
    "Chips & Technologies 65554 PCI" driver works; its 1024x600 entry fails.
  - `debug_cmd dev` dumps both cards' registers.
- S3 Savage3D, the STB Nitro 3200 (`src/video/vid_s3_savage.c`, AGP and PCI, 5333:8A21, 8 MB;
  BIOS `roms/video/s3savage/n32_110.rom` from STB's SourceSafe dump, in ROMS-EXTRA): ViRGE-style VGA
  core, 128 MB BAR0 (LFB, MMIO at +16 MB with the VGA ports at 8000h + port, six tiled apertures at
  +32 MB), primary stream via MM81C0/81C8 with 16/32 bpp tiled scan-out (2 KB tiles of 8x8-pixel
  blocks, a block being two 4-pixel columns of 8 rows: S3's DDSURF.C/DDBLTTEX.C), and the BCI 2D engine (rectangles with ROP3, patterns, host and screen blits, text,
  lines, bitmap descriptors, shadow status in system memory; a dword with bits 31:30 set where a
  position or size is expected starts the next command). GX3 3D engine (`savage_3d_*`):
  DrawPrimitive and DrawIndexedPrimitive (vertex buffer at register 3Eh) lists/strips/fans,
  textures tiled in 2 KB tiles (8 bpp 64x32, 16 bpp 64x16, 32 bpp 32x16) in video or AGP memory,
  S3TC (DXT1), mipmaps (levels in whole 8x8 blocks, 4x4 and smaller sharing one), point/bilinear/
  trilinear sampling with LOD bias, wrap/clamp/mirror (TexCtrl 15:14), texture blend, transparency (TexCtrl bit 20), specular,
  vertex/table fog, alpha test/blend, 16- and 32-bit Z, scissors; the scan-out latches MM81C0 page flips
  per frame. The Win9x driver also draws palettized DIBs at 16 bpp that way (DIB32.ASM). Direct3D
  verified: DxDiag 6.1 cube, Tomb Raider 2 (matches its software renderer; with the
  driver's HKCC\Display\Settings AC=ON and AT=ON it draws with S3TC and trilinear). Win98 SE (S3
  4.11.01) and NT4 (S3 6.10.04) run accelerated; NT4's s3mini does not load on the AGP card --
  VideoPortVerifyAccessRanges rejects its legacy VGA/ROM ranges on bus 1 because NT4's HAL only
  accepts the AGP bridge's windows and the BIOS leaves its I/O window empty (as on real
  hardware); use the PCI card. Secondary stream (overlay, `savage_overlay_*`, composed by a
  wrapper around the primary line renderer): GX3 register meanings from DDOVER.C -- scale
  factors are 1.15 source steps (MM8198/81E0), MM81E8 is the source line count (bit 15 vertical
  filter), MM81EC bit 0 horizontal interpolation, MM8190 source width + format (YVYU, YUY2
  CCIR/full range, Y211, RGB 555/565/888/8888), MM819C colour adjust, MM81A0 compose modes and
  keys (dest key compares the primary's raw bits, YUV source keys whole bytes). Interrupts:
  MM8504 status 7:0 (bit 0 vsync) / enables 15:8, CR32 bit 4 master, INTA
  (KMVT.ASM's scheme). DDC2 on CRAA, as the BIOS's VBE 4F15 and both drivers use it. Tests:
  OVTEST.EXE modes 1-7 and IRQTEST.COM (savage3d-tools in the notes folder). "dev" in the debug
  command file dumps the BCI parser, 3D, overlay and interrupt state; "dev bcilog 1|0" logs every BCI dword and MMIO register write; "dev texdump <TexAddr>
  <TexDescr> <file.bmp>" decodes a texture as the 3D engine reads it.
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
only refreshed with `--update-roms`; ROMs in neither set (the NVIDIA RIVA 128/128 ZX BIOSes in
`video/nvidia/nv3/`) live in `F:\Claude\86Box-Next\ROMS-EXTRA` (roms/ layout, outside the repo) and are
added to every rig that lacks them -- leave the ROMs alone unless strictly necessary. **Never delete
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
Qt 5 compatibility check only: `build-qt5` (dynamic, `-DUSE_QT6=OFF -DSTATIC_BUILD=OFF`, MSYS2
`qt5-base` + `qt5-tools`). Never release or stage it: releases are Qt 6 only, and
`tools/make-release.sh` refuses a build-static that is not `USE_QT6=ON`. Qt 6-only APIs
(e.g. `QMouseEvent::position()`) need a `QT_VERSION_CHECK(6, 0, 0)` fallback.
