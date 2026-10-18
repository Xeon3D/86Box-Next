#!/bin/bash
# fullrun.sh THREADS GPU [SHOTS] -- boot the VM, run 3DMark 99 Max's whole
# benchmark with render_threads = THREADS and NV4_GPU = GPU, take a shot
# (full_N) every 30 s, SHOTS of them (default 14: the run and its score
# box), log "dev 3d" at the end, then shut Windows down.
D=$(dirname "$0")
T="$D/tnt.sh"
W=${TNT_DIR:-/f/Claude/86Box-Next/work-tnt}
N=${3:-14}
sed -i "s/^render_threads = .*/render_threads = $1/" "$W/86box.cfg"
NV4_GPU=$2 bash "$T" start || exit 1
sleep 80
bash "$T" cmd "key 1d,01,81,9d"; sleep 1; bash "$T" cmd "type p"; sleep 1.5
bash "$T" cmd "key 1c,9c"; sleep 1; bash "$T" cmd "key 1c,9c"; sleep 40
bash "$T" cmd "key 1c,9c"; sleep 4; bash "$T" cmd "key 1c,9c"
for i in $(seq 1 "$N"); do sleep 30; bash "$T" shot "full_$i"; done
bash "$T" cmd "dev 3d"; sleep 1
grep "nv4 3D:" "$W/86box.log" | tail -8
# close the score box and 3DMark's window, then shut down
bash "$T" cmd "key 01,81"; sleep 1; bash "$T" cmd "key 01,81"; sleep 1
bash "$T" cmd "key 1d,01,81,9d"; sleep 1; bash "$T" cmd "type u"; sleep 2; bash "$T" cmd "key 1c,9c"
for i in $(seq 1 90); do
  n=$(bash "$T" running)
  [ "$n" = "0" ] && break
  [ "$i" = "45" ] && bash "$T" cmd "key 12,92"
  sleep 2
done
echo "exited: $n"
