#!/bin/sh
# Stage the newest 86Box-Next build in ../Latest (next to the repository).
# Latest holds one rig per guest OS ("Windows 98 SE", "MS-DOS", ...), each a
# folder with its own 86box.cfg, nvr/, disk images and roms/: the exe goes
# into every rig folder that has an 86box.cfg or an 86Box-Next.exe (folders
# with neither are left alone).  A rig without roms/ gets the 86Box ROM set
# and MegaPPBox's Megatouch ROMs; every rig gets whatever ../ROMS-EXTRA holds
# (ROMs in neither set, e.g. the NVIDIA BIOSes) that its roms/ lacks.
#
# The rigs are where the machines being tested live, so this only ever
# replaces the exe (and, for a dynamic build, its DLLs and Qt plugins) and
# never deletes anything: 86box.cfg, nvr/, disk images and whatever else is
# there are left alone.  roms/ is left alone too: it is only created when it
# is missing (and roms/megatouch only added when missing), unless
# --update-roms asks for both to be refreshed.
#
# The default is the static build (build-static: one self-contained exe).  A
# dynamic build directory also works; then every DLL and Qt plugin it needs
# from MSYS2 UCRT64 is copied next to it.
#
# Usage: tools/stage-rig.sh [--update-roms] [build-dir]
#        (run from the repo root, Git Bash)
set -e
UPDATE_ROMS=0
if [ "$1" = "--update-roms" ]; then
    UPDATE_ROMS=1
    shift
fi
BUILD=${1:-build-static}
OUT=../Latest
EXTRA=../ROMS-EXTRA
UCRT=/c/msys64/ucrt64
EXE="$BUILD/src/86Box-Next.exe"
export PATH="$UCRT/bin:/c/msys64/usr/bin:$PATH"

[ -f "$EXE" ] || { echo "no $EXE - build first" >&2; exit 1; }
if tasklist //FI "IMAGENAME eq 86Box-Next.exe" 2>/dev/null | grep -qi "86Box-Next.exe"; then
    echo "86Box-Next is running; close it and stage again" >&2
    exit 2
fi
# Every rig: a subfolder of Latest with a machine or an exe in it.  A rig
# holding a .stage-branch file belongs to that git branch: it gets builds of
# that branch only, and a build of that branch goes to such rigs only.
BRANCH=$(git branch --show-current)
OWNED=0
for d in "$OUT"/*/; do
    [ "$(cat "${d}.stage-branch" 2>/dev/null | tr -d '
')" = "$BRANCH" ] && OWNED=1
done
RIGS=""
for d in "$OUT"/*/; do
    d=${d%/}
    if [ -f "$d/86box.cfg" ] || [ -f "$d/86Box-Next.exe" ]; then
        OWNER=$(cat "$d/.stage-branch" 2>/dev/null | tr -d '
')
        if [ -n "$OWNER" ] && [ "$OWNER" != "$BRANCH" ]; then
            echo "Skipping $d (branch $OWNER's rig)"
            continue
        fi
        if [ -z "$OWNER" ] && [ "$OWNED" = 1 ]; then
            continue
        fi
        RIGS="$RIGS
$d"
    fi
done
[ -n "$RIGS" ] || { echo "no rig folders in $OUT" >&2; exit 1; }

stage_rig() {
    RIG=$1
    # The 86Box ROM set: cloned only if there is none; pulled only when asked.
    if [ ! -d "$RIG/roms" ]; then
        git clone -q --depth 1 https://github.com/86Box/roms.git "$RIG/roms"
    elif [ "$UPDATE_ROMS" = 1 ] && [ -d "$RIG/roms/.git" ]; then
        git -C "$RIG/roms" pull -q --ff-only || true
    fi

    # The Merit Megatouch boards' ROMs (board ROM, key password tables, CMOS
    # images) live in MegaPPBox's repository, not in the 86Box ROM set: added
    # only if missing, refreshed only when asked.
    if [ ! -d "$RIG/roms/megatouch" ] || [ "$UPDATE_ROMS" = 1 ]; then
        MEGA=../../MegaPPBox-src
        TMP=""
        if [ ! -d "$MEGA/roms/megatouch" ]; then
            TMP=$(mktemp -d)
            git clone -q --depth 1 --filter=blob:none --sparse https://github.com/Xeon3D/MegaPPBox.git "$TMP/m"
            git -C "$TMP/m" sparse-checkout set roms/megatouch
            MEGA="$TMP/m"
        fi
        mkdir -p "$RIG/roms/megatouch"
        cp -r "$MEGA/roms/megatouch/." "$RIG/roms/megatouch/"
        [ -n "$TMP" ] && rm -rf "$TMP"
    fi

    # ROMs that are in neither set (the NVIDIA RIVA 128 / 128 ZX BIOSes in
    # video/nvidia/nv3), kept outside the repository in ../ROMS-EXTRA with
    # the roms/ layout: missing files are added, existing ones are only
    # replaced when asked.
    if [ -d "$EXTRA" ]; then
        if [ "$UPDATE_ROMS" = 1 ]; then
            cp -r "$EXTRA/." "$RIG/roms/"
        else
            cp -rn "$EXTRA/." "$RIG/roms/"
        fi
    fi

    strip -o "$RIG/86Box-Next.exe" "$EXE"
    if ! ldd "$EXE" | grep -qi '/ucrt64/'; then
        echo "Staged $(git describe --always --dirty) in $RIG (static)"
        return
    fi

    # A dynamic build: its Qt plugins and every UCRT64 DLL it pulls in.
    windeployqt6 --no-translations --no-compiler-runtime --no-system-d3d-compiler         --no-opengl-sw --dir "$RIG" "$RIG/86Box-Next.exe" >/dev/null
    for i in 1 2 3; do
        find "$RIG" -maxdepth 2 \( -name '*.exe' -o -name '*.dll' \) |
            xargs ldd 2>/dev/null | awk '{print $3}' | grep -i '^/ucrt64/' | sort -u |
            while read -r dll; do
                cp -u "$UCRT/bin/$(basename "$dll")" "$RIG/"
            done
    done
    echo "Staged $(git describe --always --dirty) in $RIG (dynamic)"
}

echo "$RIGS" | while read -r rig; do
    [ -n "$rig" ] && stage_rig "$rig"
done

# isp-server (the virtual ISP and telephone exchange the rigs' modems dial)
# sits in Latest itself, shared by every rig.
# A branch build that owns a rig stages only that rig, not the shared isp-server.
ISP="$BUILD/isp-server/isp-server.exe"
if [ "$OWNED" != 1 ] && [ -f "$ISP" ]; then
    if tasklist //FI "IMAGENAME eq isp-server.exe" 2>/dev/null | grep -qi "isp-server.exe"; then
        echo "isp-server is running; not replaced (close it and stage again)" >&2
    else
        strip -o "$OUT/isp-server.exe" "$ISP"
        echo "Staged isp-server in $OUT"
    fi
fi
