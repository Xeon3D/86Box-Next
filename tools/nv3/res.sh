#!/bin/bash
# res.sh left|right N PREFIX: open display settings, move the screen-area slider N steps, apply, keep
S=$(dirname "$0"); T=$S/nvtest.sh
$T cmd "key 1d,01,81,9d"; sleep 1; $T cmd "type r"; sleep 2; $T cmd "type control desk.cpl,,3" "key 1c,9c"; sleep 5
$T cmd "key 0f,8f"; sleep 0.5
k=4d; [ "$1" = left ] && k=4b
for i in $(seq 1 $2); do $T cmd "key e0,$k,e0,$(printf %x $((0x$k|0x80)))"; sleep 0.4; done
$T shot ${3}a >/dev/null
$T cmd "key 1c,9c"; sleep 3; $T cmd "key 1c,9c"; sleep 6; $T shot ${3}b >/dev/null
$T cmd "type y"; sleep 3; $T shot ${3}c >/dev/null
