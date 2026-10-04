#!/bin/sh
# Stage a runnable 86Box-Next rig in ./Latest: the built exe and the 86Box
# ROM set (plus MegaPPBox's Megatouch ROMs).  Replaces whatever was there, so
# Latest only ever holds the newest build.
#
# The default is the static build (build-static: one self-contained exe).  A
# dynamic build directory also works; then every DLL and Qt plugin it needs
# from MSYS2 UCRT64 is copied next to it.
#
# Usage: tools/stage-rig.sh [build-dir]   (run from the repo root, Git Bash)
set -e
BUILD=${1:-build-static}
OUT=Latest
UCRT=/c/msys64/ucrt64
EXE="$BUILD/src/86Box-Next.exe"
export PATH="$UCRT/bin:/c/msys64/usr/bin:$PATH"

[ -f "$EXE" ] || { echo "no $EXE - build first" >&2; exit 1; }

# Keep the ROM clone across restages; it is large and rarely changes.
if [ -d "$OUT/roms/.git" ]; then
    git -C "$OUT/roms" pull -q --ff-only || true
    find "$OUT" -mindepth 1 -maxdepth 1 ! -name roms -exec rm -rf {} +
else
    rm -rf "$OUT"
    mkdir -p "$OUT"
    git clone -q --depth 1 https://github.com/86Box/roms.git "$OUT/roms"
fi

# The Merit Megatouch boards' ROMs (board ROM, key password tables, CMOS
# images) live in MegaPPBox's repository, not in the 86Box ROM set.
MEGA=../MegaPPBox-src
if [ ! -d "$MEGA/roms/megatouch" ]; then
    rm -rf "$OUT/.megappbox"
    git clone -q --depth 1 --filter=blob:none --sparse https://github.com/Xeon3D/MegaPPBox.git "$OUT/.megappbox"
    git -C "$OUT/.megappbox" sparse-checkout set roms/megatouch
    MEGA="$OUT/.megappbox"
fi
rm -rf "$OUT/roms/megatouch"
cp -r "$MEGA/roms/megatouch" "$OUT/roms/megatouch"
rm -rf "$OUT/.megappbox"

strip -o "$OUT/86Box-Next.exe" "$EXE"
if ! ldd "$EXE" | grep -qi '/ucrt64/'; then
    echo "Staged $(git describe --always --dirty) in $OUT (static)"
    exit 0
fi
windeployqt6 --no-translations --no-compiler-runtime --no-system-d3d-compiler \
    --no-opengl-sw --dir "$OUT" "$OUT/86Box-Next.exe" >/dev/null

# Copy every UCRT64 DLL the exe and the deployed Qt plugins pull in.
for i in 1 2 3; do
    find "$OUT" -name '*.exe' -o -name '*.dll' | grep -v "^$OUT/roms" |
        xargs ldd 2>/dev/null | awk '{print $3}' | grep -i '^/ucrt64/' | sort -u |
        while read -r dll; do
            name=$(basename "$dll")
            [ -f "$OUT/$name" ] || cp "$UCRT/bin/$name" "$OUT/"
        done
done

echo "Staged $(git describe --always --dirty) in $OUT"
