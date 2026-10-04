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
  Megatouch XL/MAXX (MegaPPBox `merit_io.c`, `merit_pcic.c`). UI controls in
  `src/qt/qt_ioboard_controls.cpp`.

## Staged rig
`tools/stage-rig.sh` rebuilds `Latest/` (git-ignored): exe, DLLs, 86Box ROM set,
plus MegaPPBox's `roms/megatouch`. Restage after every change.

## Building (Windows)
MSYS2 UCRT64 with Qt6:
`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DQT=ON -DUSE_QT6=ON && cmake --build build`
