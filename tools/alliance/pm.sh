#!/bin/bash
# pm.sh -- drive the Alliance ProMotion test VMs through the debug command file
# (BOX86NEXT_DEBUG_CMD), without touching the host's focus.
#   pm.sh start            start minimized (no focus)
#   pm.sh cmd "<lines>"    run debug commands (waits until consumed)
#   pm.sh shot NAME        screenshot to $PM_DIR/NAME.bmp, PNG copy in $PM_SHOTS
#   pm.sh stop             kill the VM (last resort: Win98 then boots to Safe Mode)
#   pm.sh running          print how many emulators run this VM (0 or 1)
# PM_DIR: the VM folder (default F:/Claude/86Box-Next/work-pmdos), with
#   86Box-Next.exe, 86box.cfg and romsx/ (the Alliance ROMs, passed with -R).
# PM_SHOTS: where PNGs go (default $PM_DIR/shots).
W=${PM_DIR:-/f/Claude/86Box-Next/work-pmdos}
WW=$(cygpath -w "$W")
SHOTS=${PM_SHOTS:-$W/shots}
CMDF="$W/dbg.cmd"
# the emulators running this VM: those started with -P "<its folder>"
vm_pids() {
    powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='86Box-Next.exe'\" | ForEach-Object { [string]\$_.ProcessId + ' ' + \$_.CommandLine }" |
        tr -d '\r' | grep -F -- "-P \"$WW\"" | cut -d' ' -f1
}
case "$1" in
start)
    # two instances would share the disk image and the command file
    if [ "$(vm_pids | wc -l)" != "0" ]; then
        echo "pm.sh: a VM in $W is already running" >&2
        exit 1
    fi
    rm -f "$CMDF" "$W/86box.log"
    powershell -NoProfile -Command "\$env:BOX86NEXT_DEBUG_CMD='$WW\dbg.cmd'; Start-Process -FilePath '$WW\86Box-Next.exe' -ArgumentList '-P','\"$WW\"','-R','\"$WW\romsx\"','-L','\"$WW\86box.log\"' -WindowStyle Minimized" ;;
cmd)
    printf '%s\n' "$2" > "$CMDF"
    for i in $(seq 1 100); do [ -f "$CMDF" ] || break; sleep 0.1; done ;;
shot)
    mkdir -p "$SHOTS"
    bash "$0" cmd "shot $WW\\$2.bmp"
    sleep 0.5
    py -c "from PIL import Image; Image.open(r'$WW\\$2.bmp').save(r'$(cygpath -w "$SHOTS")\\$2.png')" 2>/dev/null && rm -f "$W/$2.bmp" || echo "no PIL; bmp at $W/$2.bmp" ;;
running)
    vm_pids | wc -l ;;
stop)
    for pid in $(vm_pids); do powershell -NoProfile -Command "Stop-Process -Id $pid -Force"; done
    for i in $(seq 1 50); do [ "$(vm_pids | wc -l)" = "0" ] && break; sleep 0.2; done ;;
esac
