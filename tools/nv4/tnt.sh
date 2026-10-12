#!/bin/bash
# tnt.sh -- drive the RIVA TNT test VM through the debug command file
# (BOX86NEXT_DEBUG_CMD), without touching the host's focus.
#   tnt.sh start            start minimized (no focus); NV4_GPU / NV4_3D_VERIFY pass through
#   tnt.sh cmd "<lines>"    run debug commands (waits until consumed)
#   tnt.sh shot NAME        screenshot to $TNT_DIR/NAME.bmp, PNG copy in $TNT_SHOTS
#   tnt.sh stop             kill the VM (last resort: Win98 then boots to Safe Mode)
# TNT_DIR: the VM folder (default F:/Claude/86Box-Next/work-tnt), with
#   86Box-Next.exe, 86box.cfg and romsx/ (the NV4 ROMs, passed with -R).
# TNT_SHOTS: where PNGs go (default $TNT_DIR/shots).
W=${TNT_DIR:-/f/Claude/86Box-Next/work-tnt}
WW=$(cygpath -w "$W")
SHOTS=${TNT_SHOTS:-$W/shots}
CMDF="$W/dbg.cmd"
case "$1" in
start)
    rm -f "$CMDF" "$W/86box.log"
    powershell -NoProfile -Command "\$env:BOX86NEXT_DEBUG_CMD='$WW\dbg.cmd'; Start-Process -FilePath '$WW\86Box-Next.exe' -ArgumentList '-P','\"$WW\"','-R','\"$WW\romsx\"','-L','\"$WW\86box.log\"' -WindowStyle Minimized" ;;
cmd)
    printf '%s\n' "$2" > "$CMDF"
    for i in $(seq 1 100); do [ -f "$CMDF" ] || break; sleep 0.1; done ;;
shot)
    mkdir -p "$SHOTS"
    "$0" cmd "shot $WW\$2.bmp"
    sleep 0.5
    py -c "from PIL import Image; Image.open(r'$WW\$2.bmp').save(r'$(cygpath -w "$SHOTS")\$2.png')" 2>/dev/null || echo "no PIL; bmp at $W/$2.bmp" ;;
stop)
    powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='86Box-Next.exe'\" | Where-Object { \$_.CommandLine -like '*work-tnt*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" ;;
esac
