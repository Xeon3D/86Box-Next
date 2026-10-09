#!/bin/bash
# 3dmark.sh: start 3DMark 99 MAX on the rig and open a new benchmark (Benchmark button focused;
# send "key 1c,9c" to run it). The guest keyboard is Portuguese: '"' = Shift+2, ':' = Shift+period,
# '\' = the key left of 1 (0x29) -- tools/nv3 "type" only knows the US map.
T=$(dirname "$0")/nvtest.sh
Q="key 2a,03,83,aa"; C="key 2a,34,b4,aa"; B="key 29,a9"
$T cmd "key 1d,01,81,9d"; sleep 1; $T cmd "type r"; sleep 2
$T cmd "$Q" "type C" "$C" "$B" "type Program Files" "$B" "type 3DMark 99 Max" "$B" "type 3dmark.exe" "$Q" "key 1c,9c"
sleep 12; $T cmd "key 1c,9c"; sleep 4
