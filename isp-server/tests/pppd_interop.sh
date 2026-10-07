#!/bin/bash
# 86Box-Next: isp-server against Linux's own PPP -- pppd and the kernel's
# MPPE, Deflate, BSD-Compress and Multilink -- as the guest.  Not run by
# ctest: it needs root (pppd makes ppp interfaces), pppd, socat, curl,
# python3 and the kernel's ppp modules.
#
#   sudo isp-server/tests/pppd_interop.sh path/to/isp-server
#
# Each case: isp-server configured by an .ini, pppd dialling it over a pty
# and socat, then pings to the ISP's gateway, a 2 MB download (text, then
# noise) and a 1 MB upload through the link, checked by SHA-256.  Non-zero
# if any case fails.
set -u
ISP=${1:?usage: pppd_interop.sh path/to/isp-server}
ISP=$(readlink -f "$ISP")
WORK=$(mktemp -d /tmp/isp-interop.XXXXXX)
PORT=2390
HTTP=8399
UP=8398
pass=0
fail=0
failed=""

modprobe -a ppp_async ppp_deflate bsd_comp ppp_mppe 2>/dev/null

# Data to move: compressible text and incompressible noise.
python3 - "$WORK" <<'EOF'
import os, sys, random
w = sys.argv[1]
random.seed(86)
words = "the quick brown fox jumps over the lazy dog dial up modem isp ppp".split()
with open(os.path.join(w, "text.bin"), "w") as f:
    while f.tell() < 2 * 1024 * 1024:
        f.write(" ".join(random.choice(words) for _ in range(12)) + "\n")
with open(os.path.join(w, "noise.bin"), "wb") as f:
    f.write(os.urandom(1024 * 1024))
EOF
(cd "$WORK" && exec python3 -m http.server $HTTP --bind 127.0.0.1 >/dev/null 2>&1) &
WEB=$!
sleep 1

cleanup() {
    kill $WEB 2>/dev/null
    pkill -f "isp-server.*--port $PORT" 2>/dev/null
    pkill -f "socat - TCP:127.0.0.1:$PORT" 2>/dev/null
}
trap cleanup EXIT

# run_case NAME "ini lines" expect(up|fail) pppd options...
run_case() {
    local name=$1 ini=$2 expect=$3
    shift 3
    local ifc=isp0 ok=1 why=""

    printf '%s\nport = %d\nhttp_port = 0\n' "$ini" $PORT > "$WORK/isp.ini"
    "$ISP" --no-open --config "$WORK/isp.ini" > "$WORK/isp.log" 2>&1 &
    local sp=$!
    sleep 0.7
    timeout 40 pppd pty "socat - TCP:127.0.0.1:$PORT" nodetach noauth nodefaultroute noipdefault \
        ifname $ifc logfile "$WORK/pppd.log" debug maxfail 1 "$@" >/dev/null 2>&1 &
    local pp=$!

    for _ in $(seq 1 40); do
        ip -4 addr show $ifc 2>/dev/null | grep -q 'inet ' && break
        kill -0 $pp 2>/dev/null || break
        sleep 0.25
    done
    if [ "$expect" = fail ]; then
        if ip -4 addr show $ifc 2>/dev/null | grep -q 'inet '; then
            ok=0
            why="the link came up"
        fi
    elif ! ip -4 addr show $ifc 2>/dev/null | grep -q 'inet '; then
        ok=0
        why="no link"
    else
        local me gw
        me=$(ip -4 addr show $ifc | awk '/inet /{print $2}' | cut -d/ -f1)
        gw=$(ip -4 addr show $ifc | awk '/inet /{print $4}' | cut -d/ -f1)
        ping -c 3 -W 3 -I $ifc "$gw" >/dev/null 2>&1 || { ok=0; why="no ping"; }
        for f in text.bin noise.bin; do
            if [ $ok = 1 ]; then
                local got
                got=$(curl -s --max-time 60 --interface $ifc "http://$gw:$HTTP/$f" | sha256sum | cut -d' ' -f1)
                [ "$got" = "$(sha256sum < "$WORK/$f" | cut -d' ' -f1)" ] || { ok=0; why="download of $f damaged"; }
            fi
        done
        if [ $ok = 1 ]; then
            rm -f "$WORK/up.bin"
            timeout 30 socat -u TCP-LISTEN:$UP,bind=127.0.0.1,reuseaddr OPEN:"$WORK/up.bin",creat,trunc &
            local ls=$!
            sleep 0.3
            head -c 1048576 "$WORK/text.bin" > "$WORK/up.src"
            timeout 60 socat -u FILE:"$WORK/up.src" TCP:"$gw":$UP,bind="$me" 2>/dev/null
            wait $ls 2>/dev/null
            cmp -s "$WORK/up.src" "$WORK/up.bin" || { ok=0; why="upload damaged"; }
        fi
        if [ -n "${CHECK_LOG:-}" ] && ! grep -q -- "$CHECK_LOG" "$WORK/pppd.log" "$WORK/isp.log"; then
            ok=0
            why="the logs do not say \"$CHECK_LOG\""
        fi
    fi
    kill $pp 2>/dev/null
    wait $pp 2>/dev/null
    kill $sp 2>/dev/null
    wait $sp 2>/dev/null
    if [ $ok = 1 ]; then
        pass=$((pass + 1))
        printf '  %-58s ok   %s\n' "$name" "$(grep -h -o -E 'MPPE [0-9]+-bit[^"]*|(Deflate|BSD-Compress) [^;]*|CCP: up; [^"]*' "$WORK/isp.log" | head -1)"
    else
        fail=$((fail + 1))
        failed="$failed\n  $name: $why"
        printf '  %-58s FAIL (%s)\n' "$name" "$why"
        echo "    --- isp-server" ; grep -v "^$" "$WORK/isp.log" | tail -15 | sed 's/^/    /'
        echo "    --- pppd" ; tail -15 "$WORK/pppd.log" | sed 's/^/    /'
    fi
    unset CHECK_LOG
    sleep 0.5
}

ACC=$'auth = accounts\naccount = alice Secret123\nmultilink = 0'
NOCOMP=$'compression = \nmppe = off'

echo "== authentication =="
CHECK_LOG='PAP: "alice" let in' run_case "PAP" "$ACC
$NOCOMP" up user alice password Secret123 refuse-chap refuse-mschap refuse-mschap-v2 refuse-eap noccp
run_case "PAP, wrong password" "$ACC
$NOCOMP" fail user alice password nope refuse-chap refuse-mschap refuse-mschap-v2 refuse-eap noccp
CHECK_LOG='CHAP-MD5: "alice" let in' run_case "CHAP-MD5" "$ACC
$NOCOMP" up user alice password Secret123 refuse-pap refuse-mschap refuse-mschap-v2 refuse-eap noccp
run_case "CHAP-MD5, wrong password" "$ACC
$NOCOMP" fail user alice password nope refuse-pap refuse-mschap refuse-mschap-v2 refuse-eap noccp
CHECK_LOG='MS-CHAP: "alice" let in' run_case "MS-CHAP" "$ACC
$NOCOMP" up user alice password Secret123 refuse-pap refuse-chap refuse-mschap-v2 refuse-eap noccp
CHECK_LOG='MS-CHAP-2: "alice" let in' run_case "MS-CHAP-2 (pppd checks our authenticator response)" "$ACC
$NOCOMP" up user alice password Secret123 refuse-pap refuse-chap refuse-mschap refuse-eap noccp
run_case "MS-CHAP-2, wrong password" "$ACC
$NOCOMP" fail user alice password nope refuse-pap refuse-chap refuse-mschap refuse-eap noccp
run_case "an unknown account" "$ACC
$NOCOMP" fail user mallory password Secret123 refuse-pap refuse-chap refuse-mschap refuse-eap noccp
CHECK_LOG='PAP: "anybody" let in' run_case "anyone, by PAP" $'auth = any\nauth_methods = pap\ncompression = \nmppe = off' up \
    user anybody password whatever noccp
CHECK_LOG='WINS 10.0.0.5' run_case "WINS servers told (usepeerwins)" "$ACC
$NOCOMP
wins = 10.0.0.5 10.0.0.6" up user alice password Secret123 usepeerwins noccp \
    ipparam x connect-delay 0
echo
echo "== encryption (MPPE) =="
for s in 128 40; do
    CHECK_LOG="MPPE $s-bit" run_case "MS-CHAP-2, MPPE $s-bit stateless" "$ACC
compression =
mppe = required
mppe_strengths = 40 56 128" up user alice password Secret123 refuse-pap refuse-chap refuse-mschap refuse-eap \
        require-mppe-$s nobsdcomp nodeflate
done
# (No MS-CHAP + MPPE case: pppd 2.5's mppe_set_chapv1() hands one buffer to
# mppe_set_keys() as both keys, which zeroes it after taking the first, so
# its receive key is all zeroes.  The ISP follows RFC 3079, as Windows does;
# crypto_test.c checks those keys against the RFC's worked example.)
CHECK_LOG="stateful" run_case "MS-CHAP-2, MPPE 128-bit stateful" "$ACC
compression =
mppe = required
mppe_stateless = 0" up user alice password Secret123 refuse-pap refuse-chap refuse-mschap refuse-eap \
    require-mppe-128 mppe-stateful nobsdcomp nodeflate
run_case "MPPE required, the guest will not" "$ACC
compression =
mppe = required" fail user alice password Secret123 refuse-pap refuse-chap refuse-mschap refuse-eap nomppe
echo
echo "== compression =="
for w in 15 12 9; do
    CHECK_LOG="Deflate, $w-bit window" run_case "Deflate $w" $'auth = none\nmppe = off\ncompression = deflate' up \
        deflate $w nobsdcomp nopredictor1 nomppe
done
for b in 15 12 9; do
    CHECK_LOG="BSD-Compress, $b bits" run_case "BSD-Compress $b" $'auth = none\nmppe = off\ncompression = bsd' up \
        bsdcomp $b nodeflate nopredictor1 nomppe
done
CHECK_LOG="Deflate" run_case "all offered, pppd picks" $'auth = none\nmppe = off' up nomppe
echo
echo "== Multilink =="
multilink_case() {
    local ok=1 why=""
    printf '%s\nport = %d\nhttp_port = 0\n' $'auth = accounts\naccount = alice Secret123\ncompression = \nmppe = off\nmultilink = 1' \
        $PORT > "$WORK/isp.ini"
    "$ISP" --no-open --config "$WORK/isp.ini" > "$WORK/isp.log" 2>&1 &
    local sp=$!
    sleep 0.7
    local opts=(nodetach noauth nodefaultroute noipdefault debug maxfail 1 multilink mrru 1500
        endpoint magic:0011223344556677 user alice password Secret123 refuse-pap refuse-chap refuse-mschap noccp)
    timeout 60 pppd pty "socat - TCP:127.0.0.1:$PORT" ifname isp0 logfile "$WORK/pppd.log" "${opts[@]}" >/dev/null 2>&1 &
    local p1=$!
    for _ in $(seq 1 40); do ip -4 addr show isp0 2>/dev/null | grep -q 'inet ' && break; sleep 0.25; done
    timeout 60 pppd pty "socat - TCP:127.0.0.1:$PORT" logfile "$WORK/pppd2.log" "${opts[@]}" >/dev/null 2>&1 &
    local p2=$!
    for _ in $(seq 1 40); do grep -q "joined call 1's bundle" "$WORK/isp.log" && break; sleep 0.25; done
    grep -q "joined call 1's bundle" "$WORK/isp.log" || { ok=0; why="the second link did not join the bundle"; }
    if [ $ok = 1 ]; then
        local gw
        gw=$(ip -4 addr show isp0 | awk '/inet /{print $4}' | cut -d/ -f1)
        sleep 1
        local got
        got=$(curl -s --max-time 60 --interface isp0 "http://$gw:$HTTP/text.bin" | sha256sum | cut -d' ' -f1)
        [ "$got" = "$(sha256sum < "$WORK/text.bin" | cut -d' ' -f1)" ] || { ok=0; why="download over the bundle damaged"; }
    fi
    kill $p2 $p1 2>/dev/null
    wait $p2 $p1 2>/dev/null
    kill $sp 2>/dev/null
    wait $sp 2>/dev/null
    if [ $ok = 1 ]; then
        pass=$((pass + 1))
        printf '  %-58s ok\n' "two links, one bundle, 2 MB across it"
    else
        fail=$((fail + 1))
        failed="$failed\n  multilink: $why"
        printf '  %-58s FAIL (%s)\n' "two links, one bundle" "$why"
        echo "    --- isp-server" ; tail -25 "$WORK/isp.log" | sed 's/^/    /'
        echo "    --- pppd 1" ; tail -12 "$WORK/pppd.log" | sed 's/^/    /'
        echo "    --- pppd 2" ; tail -12 "$WORK/pppd2.log" | sed 's/^/    /'
    fi
}
multilink_case

echo
echo "$pass passed, $fail failed"
[ $fail = 0 ] || { printf "$failed\n"; exit 1; }
