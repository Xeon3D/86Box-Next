#!/bin/bash
# vmlock.sh -- one test VM at a time across all sessions (the owner's rule:
# two running VMs slow the owner's own VM down).
#   vmlock.sh acquire NAME VM [WAIT_SECONDS=3600]  blocks until the lock is free
#   vmlock.sh release NAME
#   vmlock.sh status
# The lock is a directory (mkdir is atomic) in the shared notes folder holding
# a file "owner" with "NAME VM since". A lock older than 45 minutes whose VM
# is not running any more is considered stale and taken over.
L="C:/Users/xeon4/AppData/Roaming/Claude/side-session-notes/local_76d07661-93a5-449d-b186-1ce851f4f0b5/VM-LOCK"
running() { powershell -NoProfile -Command "(Get-CimInstance Win32_Process -Filter \"Name='86Box-Next.exe'\" | Where-Object { \$_.CommandLine -like '*$1*' } | Measure-Object).Count" | tr -d '\r'; }
case "$1" in
acquire)
    end=$(( $(date +%s) + ${4:-3600} ))
    while ! mkdir "$L" 2>/dev/null; do
        read -r who vm since < "$L/owner" 2>/dev/null
        age=$(( $(date +%s) - ${since:-0} ))
        if [ "$age" -gt 2700 ] && [ "$(running "$vm")" = "0" ]; then
            echo "vmlock: taking over stale lock of $who ($vm, ${age}s)"; rm -rf "$L"; continue
        fi
        [ "$(date +%s)" -gt "$end" ] && { echo "vmlock: still held by $who ($vm)"; exit 1; }
        sleep 20
    done
    echo "$2 $3 $(date +%s)" > "$L/owner"
    echo "vmlock: acquired by $2 for $3" ;;
release)
    read -r who vm since < "$L/owner" 2>/dev/null
    [ "$who" = "$2" ] && rm -rf "$L" && echo "vmlock: released" || echo "vmlock: not held by $2 (held by ${who:-nobody})" ;;
status)
    [ -d "$L" ] && { read -r who vm since < "$L/owner"; echo "held by $who ($vm) for $(( $(date +%s) - since ))s"; } || echo free ;;
esac
