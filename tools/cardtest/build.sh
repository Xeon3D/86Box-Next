#!/bin/bash
# build.sh -- build CARDTEST (DOS tools with nasm, Win32 tools with
# i686-w64-mingw32-gcc, both in WSL) and master CARDTEST.ISO.
#   build.sh [--tools-only]
# Output: out/CD (the CD tree) and out/CARDTEST.ISO.
set -e
shopt -s nullglob
cd "$(dirname "$0")"
here=$(pwd)
# --- build lock (runall session): two builds at once corrupt out/; a second one waits here.
mkdir -p "$here/out"
until mkdir "$here/out/.buildlock" 2>/dev/null; do echo "waiting for another build (out/.buildlock)"; sleep 5; done
trap 'rmdir "$here/out/.buildlock"' EXIT
# --- end build lock
wp() { local w; w=$(cygpath -m "$1"); echo "/mnt/$(echo "${w:0:1}" | tr A-Z a-z)${w:2}"; }
wsl_() { MSYS_NO_PATHCONV=1 wsl -e "$@"; }
cd_=$here/out/CD
mkdir -p "$cd_/DOS" "$cd_/WIN"
# Win32: freestanding, Pentium (no CMOV), runs on Win95/NT4 and later.
for src in win/*.c; do
    exe=$(basename "$src" .c | tr a-z A-Z).EXE
    libs="-luser32 -lgdi32 -lkernel32 -ladvapi32 -lshell32"
    sub=-mwindows
    case $exe in
        DDCHK.EXE|D3DCHK.EXE) libs="-ldxguid $libs" ;;
        DEPTH.EXE|CTRUN.EXE) sub=-mconsole ;; # RUNALL.BAT's helpers: command.com waits for them
    esac
    wsl_ i686-w64-mingw32-gcc -Os -s -march=i586 -ffreestanding -fno-builtin -fno-stack-protector \
        -mno-stack-arg-probe -nostdlib $sub -Wall -Wno-unused-function -Wl,-e,_entry@0 \
        -Wl,--major-subsystem-version,4,--minor-subsystem-version,0,--major-os-version,4,--minor-os-version,0 \
        -o "$(wp "$cd_/WIN/$exe")" "$(wp "$here/$src")" $libs -lgcc
    echo "built WIN/$exe"
done
for src in dos/*.asm; do
    com=$(basename "$src" .asm | tr a-z A-Z).COM
    wsl_ nasm -f bin -I "$(wp "$here/dos")/" -o "$(wp "$cd_/DOS/$com")" "$(wp "$here/$src")"
    echo "built DOS/$com"
done
[ "$1" = --tools-only ] && exit 0
cp -r "$here/cd"/. "$cd_"/
# AUTOGUI scripts (GUI benchmark automation, autogui session)
rm -rf "$cd_/GUI" && cp -r "$here/gui" "$cd_/GUI" && cp "$here/gui/RUNGUI.BAT" "$cd_/RUNGUI.BAT"
[ -x "$here/third.sh" ] && "$here/third.sh" "$cd_"
wsl_ genisoimage -quiet -J -joliet-long -r -V CARDTEST -o "$(wp "$here/out/CARDTEST.ISO")" "$(wp "$cd_")"
ls -l "$here/out/CARDTEST.ISO"
