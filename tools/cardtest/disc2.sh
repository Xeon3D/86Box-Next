#!/bin/bash
# disc2.sh -- master CARDTEST2.ISO (the big extras that do not fit on disc 1):
#   root      3D WinBench 2000 CD as published (3D WinBench 2000, WinBench 99
#             1.1, Audio WinBench 99, DirectX 7, Install.exe / autorun)
#   DX7SDK    DirectX 7.0a SDK: bin/dxutil (DDTest, DX caps viewer, DxDiag) and
#             the prebuilt samples (D3DIM, D3DRM, D3DX, DirectDraw) with media
#   FINALRLT  Final Reality 1.01 (InstallShield setup)
#   GLEAN     glean 1.0 for Win32
#   SPECVP    SPECviewperf 6.1.2 InstallShield set (vogonsdrivers.com fileid 2136)
#   DCT53     WHQL Display Compatibility Test 5.3 (owner's MSDN 1846-4/5 discs), XP SP2 only
#   DCT130    WHQL DCT 1.30.0.300 from HCT 9.5 (MSDN X05-44899): Win95/98/2000/NT4
#   DCT135    WHQL DCT 1.35.0.350 from HCT 9.6 (MSDN X05-97897): Windows Me only
# Sources: F:\Claude\86Box-Next\dl\cardtest-extra (archive.org, SourceForge).
set -e
cd "$(dirname "$0")"
here=$(pwd)
# --- build lock (runall session): two builds at once corrupt out/; a second one waits here.
mkdir -p "$here/out"
until mkdir "$here/out/.buildlock" 2>/dev/null; do echo "waiting for another build (out/.buildlock)"; sleep 5; done
trap 'rmdir "$here/out/.buildlock"' EXIT
# --- end build lock
wp() { local w; w=$(cygpath -m "$1"); echo "/mnt/$(echo "${w:0:1}" | tr A-Z a-z)${w:2}"; }
src=$(wp /f/Claude/86Box-Next/dl/cardtest-extra)
out=$here/out/CD2
mkdir -p "$out" && find "$out" -mindepth 1 -delete
mkdir -p "$out"
o=$(wp "$out")
MSYS_NO_PATHCONV=1 wsl -u root -- bash -c "set -e
    xorriso -osirrox on -indev $src/zd3dwb2000/3DWB2000.iso -extract / $o >/dev/null 2>&1
    chmod -R u+w $o
    mkdir -p $o/DX7SDK $o/FINALRLT $o/GLEAN $o/SPECVP
    7z x -y -o$o/SPECVP $src/specviewperf/specviewperf612is01.exe >/dev/null
    mkdir -p $o/DCT53 /tmp/dct53
    xorriso -osirrox on -indev $src/dct/DCT53.iso -extract /DCT5.3.EXE /tmp/dct53/dct.exe >/dev/null 2>&1
    7z x -y -o$o/DCT53 /tmp/dct53/dct.exe >/dev/null
    rm -rf /tmp/dct53
    mkdir -p $o/DCT130 $o/DCT135
    xorriso -osirrox on -indev $src/hct95/HCT95.iso -extract /hct/devkits/DCT_300 $o/DCT130 >/dev/null 2>&1
    xorriso -osirrox on -indev $src/hct96/HCT96.iso -extract /devkits $o/DCT135 >/dev/null 2>&1
    rm -f $o/DCT130/vssver.scc $o/DCT130/foo.txt
    chmod -R u+w $o/DCT130 $o/DCT135
    7z x -y -o$o/DX7SDK $src/dx7sdk/dx7sdk-7001.exe 'bin/dxutil/*' 'samples/multimedia/d3dim/bin/*' \
        'samples/multimedia/d3drm/bin/*' 'samples/multimedia/d3dx/bin/*' 'samples/multimedia/ddraw/bin/*' \
        'samples/multimedia/d3dim/media/*' 'samples/multimedia/d3drm/media/*' 'samples/multimedia/d3dx/media/*' 'samples/multimedia/ddraw/media/*' -r >/dev/null
    cd $o/FINALRLT && python3 -c \"import zipfile; z=zipfile.ZipFile('$src/finalreality/fr101.zip'); z.extractall('.')\"
    mv 'Final Reality.ver.1.0.1'/* . && rmdir 'Final Reality.ver.1.0.1'
    cd $o/GLEAN && python3 -c \"import zipfile; zipfile.ZipFile('$src/glean/glean-win-exe-1-0.zip').extractall('.')\"
    mv glean_1_0/* . && rmdir glean_1_0"
printf 'CARDTEST disc 2\r\n' > "$out/CARDTST2.TAG"
python3 -c "import sys;p=sys.argv[1];d=open(p,'rb').read().replace(b'\r\n',b'\n').replace(b'\n',b'\r\n');open(p,'wb').write(d)" "$here/cd2/README2.TXT"
cp "$here/cd2/README2.TXT" "$out/"
du -sh "$out"/* | sort -h | tail -8
MSYS_NO_PATHCONV=1 wsl -e genisoimage -quiet -J -joliet-long -r -V CARDTST2 -o "$(wp "$here/out/CARDTEST2.ISO")" "$o"
ls -l "$here/out/CARDTEST2.ISO"
