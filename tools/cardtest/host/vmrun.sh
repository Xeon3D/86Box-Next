#!/bin/bash
# vmrun.sh VMDIR CMD... -- type commands into a Windows 9x/2000 guest's
# Start > Run, one after another, through the emulator's debug command
# file (pm.sh; the VM window never takes focus). Each CMD is "WAIT|text":
# the text is typed and Enter pressed, then WAIT seconds pass and a
# screenshot run_<n>.png is taken in VMDIR/shots.
#   vmrun.sh ../work-tnt "40|d:\win\gdichk.exe /q" "60|d:\win\ddchk.exe /q"
# Use run_in_background for long lists (never stop it midway: it sends keys).
# VMRUN_KBD=pt: the guest has a Portuguese layout; : \ / are sent as
# that layout's scancodes (the debug "type" command assumes US).
vm=$(cd "$1" && pwd)
shift
P="bash /f/Claude/86Box-Next/wt-alliance/tools/alliance/pm.sh"
export PM_DIR=$vm
# type text, mapping the characters the guest layout moves
send_text() {
    local t=$1 run="" c i
    for ((i = 0; i < ${#t}; i++)); do
        c=${t:i:1}
        if [ "$VMRUN_KBD" = pt ] && [[ "$c" == ":" || "$c" == "\\" || "$c" == "/" ]]; then
            [ -n "$run" ] && $P cmd "type $run"
            run=""
            case $c in
                :) $P cmd "key 2a,34,b4,aa" ;;
                /) $P cmd "key 2a,08,88,aa" ;;
                *) $P cmd "key 29,a9" ;;
            esac
        else
            run+=$c
        fi
    done
    [ -n "$run" ] && $P cmd "type $run"
}
n=0
for item in "$@"; do
    wait=${item%%|*}
    text=${item#*|}
    n=$((n + 1))
    $P cmd "key 1d,01,81,9d"
    sleep 1
    $P cmd "key 13,93"
    sleep 1
    send_text "$text"
    sleep 1
    $P cmd "key 1c,9c"
    sleep "$wait"
    $P shot "run_$n"
    echo "$n: $text"
done
