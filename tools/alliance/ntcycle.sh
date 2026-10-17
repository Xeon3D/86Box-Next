#!/bin/bash
# ntcycle.sh -- shut a running NT 4 test VM down (Start > Shut Down), put the
# freshly built exe in, start it, log on as Administrator (blank password)
# and screenshot the desktop.
#   PM_DIR=/f/Claude/86Box-Next/work-pmnt4 ntcycle.sh [SHOTNAME] [BOOT_SECONDS]
D=$(cd "$(dirname "$0")" && pwd)
P="bash $D/pm.sh"
W=${PM_DIR:-/f/Claude/86Box-Next/work-pmnt4}
export PM_DIR=$W
EXE="$D/../../build-static/src/86Box-Next.exe"
if [ "$($P running)" != "0" ]; then
    $P cmd "key 1d,01,81,9d"; sleep 2      # Ctrl+Esc
    $P cmd "key 16,96"; sleep 3            # U: Shut Down...
    $P cmd "key 1c"; sleep 0.3; $P cmd "key 9c"
    sleep 20
    $P stop
fi
for i in 1 2 3 4 5 6 7 8 9 10; do cp "$EXE" "$W/" 2>/dev/null && break; sleep 2; done
$P start
sleep "${2:-90}"
$P cmd "key 1d,38,e0,53,e0,d3,b8,9d"; sleep 3   # Ctrl+Alt+Del
$P cmd "key 1c"; sleep 0.3; $P cmd "key 9c"
sleep 20
$P shot "${1:-ntcycle}"
