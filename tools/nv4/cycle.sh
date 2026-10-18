#!/bin/bash
# cycle.sh -- shut the test VM's Win98 down cleanly (Start > Shut Down),
# wait for the emulator to exit, copy the newest build into the VM folder
# and boot it again (minimized), waiting for the desktop.
#   cycle.sh [WAIT]   WAIT: seconds to wait after the start (default 60)
# TNT_DIR as in tnt.sh. Skips the shutdown when no VM runs.
here=$(cd "$(dirname "$0")" && pwd)
T="$here/tnt.sh"
W=${TNT_DIR:-/f/Claude/86Box-Next/work-tnt}
if [ "$(bash "$T" running)" != "0" ]; then
    bash "$T" cmd "key 01,81"
    sleep 1
    bash "$T" cmd "key 1d,01,81,9d"
    sleep 1
    bash "$T" cmd "type u"
    sleep 2
    bash "$T" cmd "key 38,1f,9f,b8"
    sleep 1
    bash "$T" cmd "key 1c,9c"
    for i in $(seq 1 45); do
        sleep 2
        [ "$(bash "$T" running)" = "0" ] && break
    done
    if [ "$(bash "$T" running)" != "0" ]; then
        echo "cycle.sh: the VM did not shut down; look at it (tnt.sh shot)" >&2
        exit 1
    fi
fi
cp "$here/../../build-static/src/86Box-Next.exe" "$W/" || exit 1
bash "$T" start || exit 1
sleep "${1:-60}"
