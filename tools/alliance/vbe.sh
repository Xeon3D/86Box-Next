#!/bin/bash
# vbe.sh -- boot the DOS test VM, start VBETest and pick its menu entries:
#   vbe.sh "0 1 1" SHOTNAME   (interactive tests, 8 bpp list, entry 1)
D=$(dirname "$0")
P="bash $D/pm.sh"
W=${PM_DIR:-/f/Claude/86Box-Next/work-pmdos}
$P stop >/dev/null 2>&1
$P start
# DOS is at its prompt about 30 s after power-on
sleep 32
$P cmd "type vbetest"; $P cmd "key 1c,9c"; sleep 4
for k in $1; do $P cmd "type $k"; sleep 3; done
sleep 2
$P shot "$2"
