#!/bin/bash
# cycle.sh -- shut a running Win9x test VM down cleanly (Start > Shut Down),
# put the freshly built exe in, start it again and screenshot the desktop.
#   PM_DIR=/f/Claude/86Box-Next/work-pmw98 cycle.sh [SHOTNAME] [WAIT_SECONDS]
# A Windows killed without shutting down comes back in Safe Mode.
D=$(cd "$(dirname "$0")" && pwd)
P="bash $D/pm.sh"
W=${PM_DIR:-/f/Claude/86Box-Next/work-pmw98}
EXE="$D/../../build-static/src/86Box-Next.exe"
if [ "$($P running)" != "0" ]; then
    $P cmd "key 1d,01,81,9d"; sleep 2      # Ctrl+Esc
    $P cmd "key 16,96"; sleep 3            # U: Shut Down...
    $P cmd "key 1f,9f"; sleep 1            # S: Shut down
    $P cmd "key 1c,9c"; sleep 15
    $P stop
fi
for i in 1 2 3 4 5 6 7 8 9 10; do cp "$EXE" "$W/" 2>/dev/null && break; sleep 2; done
$P start
sleep "${2:-90}"
$P shot "${1:-cycle}"
