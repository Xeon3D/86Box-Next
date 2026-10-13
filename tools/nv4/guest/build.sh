#!/bin/bash
# build.sh -- build the guest test programs in this folder (Windows 9x,
# no C runtime) with WSL's i686-w64-mingw32-gcc, and put them on an ISO.
#   build.sh [ISO]    default ISO: ../../../../work-tnt/nv4tools.iso
# Extra files for the ISO go in $NV4_ISO_EXTRA (a folder; copied as is).
set -e
cd "$(dirname "$0")"
here=$(pwd)
out=${1:-$here/../../../../work-tnt/nv4tools.iso}
stage=$(mktemp -d)
# a Windows path as WSL sees it
wp() { local w; w=$(cygpath -m "$1"); echo "/mnt/$(echo "${w:0:1}" | tr A-Z a-z)${w:2}"; }
for src in *.c; do
    exe=$(basename "$src" .c | tr a-z A-Z).EXE
    MSYS_NO_PATHCONV=1 wsl -e i686-w64-mingw32-gcc -Os -ffreestanding -fno-builtin -fno-stack-protector \
        -mno-stack-arg-probe -nostdlib -mwindows -Wl,-e,_entry \
        -Wl,--major-subsystem-version,4,--minor-subsystem-version,0,--major-os-version,4,--minor-os-version,0 \
        -o "$(wp "$stage/$exe")" "$src" -luser32 -lgdi32 -lkernel32
    echo "built $exe"
done
[ -n "$NV4_ISO_EXTRA" ] && cp -r "$NV4_ISO_EXTRA"/. "$stage"/
MSYS_NO_PATHCONV=1 wsl -e genisoimage -quiet -J -r -V NV4TOOLS -o "$(wp "$out")" \
    "$(wp "$stage")"
rm -rf "$stage"
echo "iso: $out"
