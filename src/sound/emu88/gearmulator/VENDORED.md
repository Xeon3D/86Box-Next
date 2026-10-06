# gearmulator (88emu), vendored

The Roland Sound Canvas MIDI device runs **88emu**, the Sound Canvas / MT-32 / CM
board emulator of [gearmulator](https://github.com/dsp56300/gearmulator) by The
Usual Suspects (GPLv3, see `LICENSE.md`; `88emu_readme.md` is its user manual).
This folder holds the part of its source tree that `88lib` needs, laid out as in
gearmulator's `source/`:

| Folder | What |
| --- | --- |
| `ronaldo/88emu/88lib` | the boards, ROM registry and loader, `HardwareDevice` |
| `ronaldo/custom_chips/{gp,xp,lsp,la32,lp,rcc,mt32reverb}` | the sound chips |
| `ronaldo/common/romDescramble.h` | |
| `cpu/{common,h8500,mcs96,sh2}` | the CPU cores |
| `cpu/dsp56300/source/{dsp56kBase,asmjit}` | threads/logging, and asmjit for the chip JITs |
| `framework/{baseLib,synthLib,hardwareLib}` | shared framework (hardwareLib without its 68k-only am29f, sciMidi, i2c) |
| `3rdparty/libresample` | |

Taken from:

* gearmulator `567ee1ff16db00240ebc1db9a30810f74fd599ac`
* its `source/cpu/dsp56300` submodule `90d3af6661727f01ce98b6c023c7b1e57bbd4fe6`
* asmjit `3577608cab0bc509f856ebf6e41b2f9d9f71acc4`

Tests, the JUCE/RmlUi player (`88emuplayer`) and everything else are left out. The
player's front-panel artwork (with the playlist painted out), sprites, key bindings
and LCD renderer were ported to Qt in `src/qt/qt_soundcanvas.cpp`,
`src/qt/soundcanvas/` and `src/sound/emu88/emu88_lcd.cpp` / `lcd_font.cpp`.

Local changes, for MinGW-w64 GCC (gearmulator builds with MSVC on Windows):

* `cpu/dsp56300/source/dsp56kBase/threadtools.cpp`: the thread-naming `__try` /
  `__except` only under `_MSC_VER`.
* `framework/baseLib/filesystem.cpp`, `framework/synthLib/os.cpp`: `<shlobj.h>`
  where MSVC has `<shlobj_core.h>`.
* `framework/synthLib/deviceException.cpp`: `#include <cstdint>`.

`framework/synthLib/buildconfig.h` is the file gearmulator's CMake generates from
`buildconfig.h.in` (demo mode off). The build is `../CMakeLists.txt`.

To update: copy the same folders from a newer gearmulator checkout (code files only,
without `tests/`), reapply the changes above, and update the commits here.
