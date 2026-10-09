#!/bin/bash
# run.sh PREFIX: run D:\OVLTEST.EXE in the rig (its CD set to ovl.iso; build it with build.sh) and
# take a shot in each of its three 8 s phases. The guest keyboard is Portuguese: ':' = Shift+period.
T=$(dirname "$0")/../nvtest.sh
$T cmd "key 1d,01,81,9d"; sleep 1; $T cmd "type r"; sleep 2
$T cmd "type d" "key 2a,34,b4,aa" "key 29,a9" "type ovltest.exe" "key 1c,9c"
sleep 5; $T shot ${1}1 >/dev/null; sleep 8; $T shot ${1}2 >/dev/null; sleep 8; $T shot ${1}3 >/dev/null
