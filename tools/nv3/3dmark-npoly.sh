#!/bin/bash
# 3dmark-npoly.sh: start 3DMark 99 MAX (3dmark.sh), select only the "n Pixel Polygons" tests and run
# them. Keyboard only: Tab to the tests' Change..., Alt+C (Clear), Alt+N, Shift+Tab x2 from Help to OK.
D=$(dirname "$0"); T=$D/nvtest.sh
bash "$D/3dmark.sh"
$T cmd "key 0f,8f"; sleep 0.3; $T cmd "key 0f,8f"; sleep 0.3; $T cmd "key 1c,9c"; sleep 1.5
$T cmd "key 38,2e,ae,b8"; sleep 0.5; $T cmd "key 38,31,b1,b8"; sleep 0.5
for i in 1 2 3 4 5 6 7 8; do $T cmd "key 0f,8f"; sleep 0.2; done
$T cmd "key 2a,0f,8f,0f,8f,aa"; sleep 0.3; $T cmd "key 39,b9"; sleep 1
$T cmd "key 2a,0f,8f,0f,8f,aa"; sleep 0.3; $T cmd "key 1c,9c"
