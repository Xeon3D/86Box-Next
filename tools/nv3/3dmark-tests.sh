#!/bin/bash
# 3dmark-tests.sh SC...: start 3DMark 99 MAX (3dmark.sh), select only the tests whose Select Tests
# accelerators have the scancodes SC (Alt+letter; default 31 = n Pixel Polygons) and run them.
# Accelerators: 22 G Game 1 Race, 32 m Game 2, 1f S CPU 3D, 21 F fill rate, 12 e texture rendering,
# 30 B bump mapping, 14 T texture filtering, 31 n n Pixel Polygons, 17 I image quality.
# Keyboard only: Tab to the tests' Change..., Alt+C (Clear), the accelerators, Tab to Help,
# Shift+Tab x2 to OK, Space; then Shift+Tab x2 from the tests' Change... to Benchmark, Enter.
D=$(dirname "$0"); T=$D/nvtest.sh
[ $# -eq 0 ] && set -- 31
bash "$D/3dmark.sh"
$T cmd "key 0f,8f"; sleep 0.3; $T cmd "key 0f,8f"; sleep 0.3; $T cmd "key 1c,9c"; sleep 1.5
$T cmd "key 38,2e,ae,b8"; sleep 0.5
for sc in "$@"; do $T cmd "key 38,$sc,$(printf %x $((0x$sc | 0x80))),b8"; sleep 0.5; done
# Alt+N twice: n Pixel Polygons unchanged and focused; from there Help is 8 Tabs away
$T cmd "key 38,31,b1,b8"; sleep 0.3; $T cmd "key 38,31,b1,b8"; sleep 0.3
for i in 1 2 3 4 5 6 7 8; do $T cmd "key 0f,8f"; sleep 0.2; done
$T cmd "key 2a,0f,8f,0f,8f,aa"; sleep 0.3; $T cmd "key 39,b9"; sleep 1
$T cmd "key 2a,0f,8f,0f,8f,aa"; sleep 0.3; $T cmd "key 1c,9c"
