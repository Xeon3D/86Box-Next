#!/bin/bash
# rmbp.sh FILEOFF... : arm breakpoints at NV3RM.VXD file offsets, using this boot's load address
# (derived from the first logged "cache1 ctx[0]" write, which is at file offset 0x1e639 + ... calibrated:
#  eip c14682c9 <-> base c144d090 in a known boot)
S=$(dirname "$0")
x=$(grep -m1 "cache1 ctx\[0\]" $NV3_WORK/rig/nv3.log | sed 's/.*0028:\(........\).*/\1/')
args=()
for f in "$@"; do
  a=$(python -c "print('%x'%(0xC144D090+(0x$x-0xc14682c9)+0x$f-0xac00))")
  args+=("bp $a ${N:-4}")
done
$S/nvtest.sh cmd "${args[@]}"
echo "${args[@]}"
