#!/bin/bash
# build.sh ISO: build OVLTEST.EXE with WSL's i686 MinGW (no CRT, no CMOV: the rig is a Pentium MMX)
# and put it on an ISO for the rig's CD drive. ISO is a Windows-side path like /mnt/c/.../ovl.iso.
MSYS_NO_PATHCONV=1 wsl -- bash -c "cd '$(wslpath -a "$(dirname "$0")" 2>/dev/null || echo /mnt/f/Claude/86Box-Next/wt-nv3/tools/nv3/ovltest)' && \
  i686-w64-mingw32-gcc -O2 -march=pentium-mmx -fno-builtin -std=gnu99 -Wall -mwindows -nostdlib -e _entry@0 \
    -o /tmp/OVLTEST.EXE ovltest.c -lddraw -ldxguid -lgdi32 -luser32 -lkernel32 && \
  i686-w64-mingw32-strip /tmp/OVLTEST.EXE && rm -rf /tmp/ovliso && mkdir /tmp/ovliso && \
  cp /tmp/OVLTEST.EXE /tmp/ovliso/ && genisoimage -quiet -J -o '$1' /tmp/ovliso"
