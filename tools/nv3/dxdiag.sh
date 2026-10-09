#!/bin/bash
# dxdiag.sh dd|d3d PREFIX: open dxdiag, Display tab, start the DirectDraw (dd) or Direct3D (d3d) test
S=$(dirname "$0"); T=$S/nvtest.sh
$T cmd "key 1d,01,81,9d"; sleep 1; $T cmd "type r"; sleep 2; $T cmd "type dxdiag" "key 1c,9c"; sleep 7
$T cmd "key 1c,9c"; sleep 6                      # bypass system info: Yes
$T cmd "key 1d,0f,8f,0f,8f,9d"; sleep 2          # Display tab
if [ "$1" = d3d ]; then $T cmd "key 0f,8f,0f,8f,0f,8f,0f,8f"; else $T cmd "key 0f,8f,0f,8f,0f,8f"; fi; sleep 1
$T cmd "key 39,b9"; sleep 2; $T cmd "key 1c,9c"; sleep 2   # test? yes
$T shot ${2}0 >/dev/null
