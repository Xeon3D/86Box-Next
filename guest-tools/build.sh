#!/bin/sh
# Build 86Box Guest Tools and pack them as a floppy image and a CD image.
#
# Needs (Debian/Ubuntu, e.g. WSL): gcc-mingw-w64-i686, binutils-mingw-w64-i686,
# mtools, xorriso, python3-pil.
#
#   guest-tools/build.sh            -> guest-tools/out/
#
# From Windows: wsl -e sh guest-tools/build.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out="$here/out"
obj="$out/obj"
proto="$here/../src/include/86box"
icon="$here/../src/qt/icons/86Box-green.ico"
cross=${CROSS:-i686-w64-mingw32-}

mkdir -p "$out" "$obj"

# ---- Windows agent (Windows 95 / NT 4.0 and later) ----
python3 "$here/windows/mkicon.py" "$icon" "$obj/gtagent.ico"
cp "$here/windows/gtagent.rc" "$here/windows/gtagent.h" "$obj/"
"${cross}windres" -I"$obj" -O coff -o "$obj/gtagent_res.o" "$obj/gtagent.rc"

"${cross}gcc" -std=gnu99 -Os -march=i386 -mtune=i686 -Wall -Wextra -Wno-unused-parameter \
    -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
    -fno-stack-protector -fno-asynchronous-unwind-tables -fno-ident \
    -I"$proto" -I"$here/windows" \
    -nostdlib -mwindows \
    -Wl,-e,_entry -Wl,--subsystem,windows:4.0 \
    -Wl,--major-os-version,4 -Wl,--minor-os-version,0 \
    -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--no-insert-timestamp \
    -s -o "$out/86BOXGT.EXE" \
    "$here/windows/gtagent.c" "$obj/gtagent_res.o" \
    -lkernel32 -luser32 -ladvapi32 -lshell32 -lwinmm -lgcc

# ---- Disk images ----
readme="$obj/README.TXT"
sed 's/$/\r/' "$here/README.TXT" > "$readme"

rm -f "$out/86box-guest-tools.img"
mformat -C -f 1440 -v GUESTTOOLS -i "$out/86box-guest-tools.img" ::
mcopy -i "$out/86box-guest-tools.img" "$out/86BOXGT.EXE" "$readme" ::

rm -f "$out/86box-guest-tools.iso"
mkdir -p "$obj/iso"
cp "$out/86BOXGT.EXE" "$readme" "$obj/iso/"
xorriso -as mkisofs -quiet -iso-level 1 -J -V GUESTTOOLS \
    -o "$out/86box-guest-tools.iso" "$obj/iso"

ls -l "$out/86BOXGT.EXE" "$out/86box-guest-tools.img" "$out/86box-guest-tools.iso"
