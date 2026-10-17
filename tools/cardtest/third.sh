#!/bin/bash
# third.sh CDTREE -- copy the third-party benchmarks (the owner's local
# copies; nothing is downloaded) into CDTREE/THIRD.
set -e
t=$1/THIRD
dl=/f/Claude/86Box-Next/dl
old="/g/Old OS"
rm -rf "$t"
mkdir -p "$t"
python3 - "$old/DOS Apps/System/Benchmarks/DOSBENCH/dosbench_v1.6.zip" "$t/DOSBENCH" <<'PY'
import sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
PY
cp -r "$dl/games/x-uvbe" "$t/UNIVBE"
cp -r "$old/DOS Apps/System/System Info/SPEEDSYS" "$t/SPEEDSYS"
cp -r "$dl/games/iso/QUAKE1" "$t/QUAKE1"
cp -r "$dl/games/iso/GLQUAKE" "$t/GLQUAKE"
cp -r "$dl/games/iso/QUAKE2" "$t/QUAKE2"
cp -r "$dl/iso-extra/3DM2000" "$t/3DM2000"
cp -r "$dl/iso-extra/UTDEMO" "$t/UTDEMO"
cp -r "$dl/iso-extra/DX7" "$t/DX7"
mkdir -p "$t/DX81" && cp "$dl/dx81/dx81eng.exe" "$t/DX81/"
# glean 1.0 (OpenGL conformance) is small: also on disc 1, so RUNALL can always run it
python3 - "$dl/cardtest-extra/glean/glean-win-exe-1-0.zip" "$t/GLEAN" <<'PY'
import sys, zipfile, os
z = zipfile.ZipFile(sys.argv[1])
for n in z.namelist():
    if n.endswith('/'):
        continue
    dest = os.path.join(sys.argv[2], n.split('/', 1)[1].upper())
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    open(dest, 'wb').write(z.read(n))
PY
cp -r "$dl/iso-extra/VIDEO" "$t/VIDEO"
# 3DMark 99 Max from the TNT test ISO (installer only, not Serial.txt)
mkdir -p "$t/3DMARK99"
MSYS_NO_PATHCONV=1 wsl -u root -- xorriso -osirrox on -indev /mnt/f/Claude/86Box-Next/work-tnt/nv4test.iso \
    -extract /3DMARK99/3dmark99max.exe "/mnt/$(cygpath -m "$t" | sed 's|^\(.\):|\L\1|')/3DMARK99/3DMARK99.EXE" 2>/dev/null
# 3DMark 99's WinZip self-extractor unpacked, so AUTOGUI drives the plain InstallShield setup
# (autogui session)
python3 - "$t/3DMARK99/3DMARK99.EXE" "$t/3DM99SET" <<'PY'
import sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
PY
# DOS/Windows text files want CRLF
for f in "$1"/*.TXT "$1"/*.BAT "$1"/*.TAG "$1"/GUI/*.BAT "$1"/GUI/*.AGS "$1"/RUNGUI.BAT; do
    python3 -c "import sys;p=sys.argv[1];d=open(p,'rb').read().replace(b'\r\n',b'\n').replace(b'\n',b'\r\n');open(p,'wb').write(d)" "$f"
done
du -sh "$t"/* | sort -h
