#!/bin/sh
# Build 86Box Guest Tools and pack them as a floppy image and a CD image.
#
# Needs (Debian/Ubuntu, e.g. WSL): gcc-mingw-w64-i686, binutils-mingw-w64-i686,
# mtools, xorriso, python3-pil, and Open Watcom (WATCOM, default ~/ow).
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

# ---- Windows 3.x agent (Open Watcom; WATCOM defaults to ~/ow) ----
WATCOM=${WATCOM:-$HOME/ow}
export WATCOM
PATH="$WATCOM/binl64:$WATCOM/binl:$PATH"
INCLUDE="$WATCOM/h:$WATCOM/h/win"
export PATH INCLUDE
python3 "$here/windows/mkicon.py" "$icon" "$obj/gtwin.ico" --win16
cp "$here/win16/gtwin.rc" "$obj/"
wcc -q -bt=windows -zW -ms -2 -os -s -wx -we -i="$proto" -fo="$obj/gtwin.obj" "$here/win16/gtwin.c"
(cd "$obj" && wrc -q -r -bt=windows -fo=gtwin.res gtwin.rc)
wlink option quiet system windows name "$out/GTWIN.EXE" file "$obj/gtwin.obj" \
    option resource="$obj/gtwin.res" option stack=8k option heapsize=4k

# ---- Disk images ----
readme="$obj/README.TXT"
sed 's/$/\r/' "$here/README.TXT" > "$readme"
files="$out/86BOXGT.EXE $out/GTWIN.EXE $readme"

rm -f "$out/86box-guest-tools.img"
mformat -C -f 1440 -v GUESTTOOLS -i "$out/86box-guest-tools.img" ::
mcopy -i "$out/86box-guest-tools.img" $files ::

rm -f "$out/86box-guest-tools.iso"
rm -rf "$obj/iso"
mkdir -p "$obj/iso"
cp $files "$obj/iso/"
xorriso -as mkisofs -quiet -iso-level 1 -J -V GUESTTOOLS \
    -o "$out/86box-guest-tools.iso" "$obj/iso"

ls -l "$out/86BOXGT.EXE" "$out/GTWIN.EXE" "$out/86box-guest-tools.img" "$out/86box-guest-tools.iso"
