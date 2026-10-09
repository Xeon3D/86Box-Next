#!/bin/bash
# NV3 rig test driver. Usage:
#   nvtest.sh start            launch NVIDIA-RIG minimized (no focus) with the debug command file
#   nvtest.sh cmd "<line>"...  send command lines and wait until the emulator ran them
#   nvtest.sh shot NAME        screenshot to $OUT/NAME.bmp and .png
#   nvtest.sh stop             kill the emulator
WORKU=${NV3_WORK:?set NV3_WORK to a work dir (POSIX path) holding rig/, a private copy of the NVIDIA-RIG}
WORKW=$(cygpath -w "$WORKU")
RIG="$WORKW\rig"
RIGU="$WORKU/rig"
OUT="$WORKU/t"
OUTW="$WORKW\t"
CMD="$OUT/cmd.txt"
PROCS="$(cygpath -w "$(dirname "$0")")\test-procs.ps1"
STRIP=/c/msys64/ucrt64/bin/strip.exe
CMDW="$OUTW\\cmd.txt"
mkdir -p "$OUT"

send() {
    printf '%s\n' "$@" > "$CMD.tmp" && mv "$CMD.tmp" "$CMD"
    for i in $(seq 1 100); do [ -f "$CMD" ] || return 0; sleep 0.2; done
    echo "command file not consumed" >&2; return 1
}

case "$1" in
start)
    n=$(powershell -NoProfile -ExecutionPolicy Bypass -File "$PROCS" | tr -dc 0-9); if [ "$n" != "0" ]; then echo "test VM already running ($n)" >&2; exit 1; fi
    rm -f "$CMD" "$RIGU/nv3.log"
    TEMP="$(cygpath -w "$OUT")" TMP="$(cygpath -w "$OUT")" powershell -NoProfile -ExecutionPolicy Bypass -Command "& '$(cygpath -w "$(dirname "$0")")\launch-noactivate.ps1' -Exe '$RIG\86Box-Next.exe' -ArgLine '-P \"$RIG\" --logfile \"$RIG\nv3.log\"' -Dir '$RIG' -CmdFile '$CMDW'"
    ;;
cmd)
    shift; send "$@" ;;
shot)
    send "shot $OUTW\\$2.bmp" && sleep 0.5 && python -I -c "
import sys,struct,zlib
d=open(sys.argv[1],'rb').read(); w,h=struct.unpack('<ii',d[18:26]); h=-h
px=d[54:]; rows=[]
for y in range(h):
    r=px[y*w*4:(y+1)*w*4]; rows.append(b'\0'+bytes(b for i in range(0,len(r),4) for b in (r[i+2],r[i+1],r[i])))
raw=b''.join(rows)
def ch(t,b): return struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b))
open(sys.argv[2],'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw,6))+ch(b'IEND',b''))
" "$OUT/$2.bmp" "$OUT/$2.png" && echo "$OUTW\\$2.png" ;;
stop)
    powershell -NoProfile -ExecutionPolicy Bypass -File "$PROCS" -Kill; sleep 2; echo stopped ;;
esac
