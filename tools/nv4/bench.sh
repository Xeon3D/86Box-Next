#!/bin/bash
# bench.sh THREADS GPU -- boot the TNT VM with render_threads = THREADS and
# NV4_GPU = GPU (0/1), start 3DMark 99 Max's benchmark from the Start menu
# (Programs > 3DMark 99 Max is the first entry; the welcome dialog's first
# button is New Benchmark, whose Benchmark button has focus), time the 3D
# shading over 40 s ("dev 3d"), then quit 3DMark and shut Windows down.
# Compare us per span between runs: 3DMark's own score is in guest time.
D=$(dirname "$0")
T="$D/tnt.sh"
W=${TNT_DIR:-/f/Claude/86Box-Next/work-tnt}
CFG=$W/86box.cfg
sed -i "s/^render_threads = .*/render_threads = $1/" "$CFG"
NV4_GPU=$2 bash "$T" start
sleep 58
bash "$T" cmd "key 1d,01,81,9d"; sleep 1; bash "$T" cmd "type p"; sleep 1.5
bash "$T" cmd "key 1c,9c"; sleep 1; bash "$T" cmd "key 1c,9c"; sleep 12
bash "$T" cmd "key 1c,9c"; sleep 4; bash "$T" cmd "key 1c,9c"
sleep 40
bash "$T" cmd "dev 3d"; sleep 40; bash "$T" cmd "dev 3d"; sleep 1
bash "$T" shot "bench_$1_$2"
grep "nv4 3D:" "$W/86box.log" | tail -2
bash "$T" cmd "key 01,81"; sleep 5; bash "$T" cmd "key 1c,9c"; sleep 1
bash "$T" cmd "key 1d,01,81,9d"; sleep 1; bash "$T" cmd "type u"; sleep 2; bash "$T" cmd "key 1c,9c"
for i in $(seq 1 60); do
  n=$(powershell -NoProfile -Command "(Get-CimInstance Win32_Process -Filter \"Name='86Box-Next.exe'\" | Measure-Object).Count")
  [ "$n" = "0" ] && break; sleep 2
done
echo "exited: $n"
