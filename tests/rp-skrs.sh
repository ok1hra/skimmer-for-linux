#!/usr/bin/env bash
# SKRS gate: skimmer-headless with [rp] stream=skrs on the skimmer's own
# bitstream — the real agent (skimmer-rp-host --sim-skrf: docs/FPGA.md) on
# 127.0.0.8: every band one stream at its own rate (rate=48…384) and input,
# a phased band two (IN1, IN2) lined up by the board's hardware index —
# the common noise coherent and nulled, as on two IPs; the P1 instance whose
# slots SKRS holds answers busy; SIGTERM stops SKRS (no stream left). A
# config past the slots is refused, rate= without stream=skrs too.
#
#   rp-skrs.sh <skimmer-headless> <skimmer-rp-host>
set -uo pipefail

HL=$1 AG=$2
T=$(mktemp -d "${TMPDIR:-/tmp}/skimmer-rp-skrs.XXXXXX")
PID= APID=
cleanup() { [[ -n $PID ]] && kill "$PID" 2>/dev/null; [[ -n $APID ]] && kill "$APID" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

fails=0
check() {  # what, condition-exit-code
  if [[ $2 == 0 ]]; then echo "  ok   $1"; else echo "  FAIL $1"; fails=$((fails + 1)); fi
}

P=$((35000 + RANDOM % 5000))
HTTP=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
"$AG" serve --sim-skrf --bind 127.0.0.8:$P,127.0.0.9:$P --ctl "$T/ctl.sock" > "$T/agent.log" 2>&1 &
APID=$!
for _ in $(seq 50); do [[ -S $T/ctl.sock ]] && break; sleep 0.1; done
st() { "$AG" ctl --ctl "$T/ctl.sock" status; }
py() { python3 -c "import json,sys; d=json.loads(sys.argv[1]); print($1)" "$2"; }

base() {
cat <<INI
[radio]
type=hpsdr-p1
host=127.0.0.8
rate=96000
[rp]
stream=skrs
[feed]
port=0
[status]
console_s=0
http_port=$HTTP
[web]
settings_from=127.0.0.1
INI
}
{ base; cat <<INI
[bands]
160m=1830000 rate=48
80m=3550000 rate=192
40m=7040000 ip=ab
30m=10120000
20m=14050000 rate=192 in=2
17m=18090000 rate=48
15m=21075000 rate=192
12m=24910000 rate=48
10m=28100000 rate=384
6m=50160000 rate=384
INI
} > "$T/headless.ini"
export XDG_CONFIG_HOME=$T/cfg XDG_DATA_HOME=$T/data XDG_STATE_HOME=$T/state
export SKIM_SCP_URL=http://127.0.0.1:9/ SKIM_HPSDR_DISCOVER=127.0.0.8:$P
export SKIM_DISCOVERY_PORT=$((40000 + RANDOM % 5000))
"$HL" --config "$T/headless.ini" > "$T/log" 2>&1 &
PID=$!
for _ in $(seq 100); do grep -q "radio: streaming" "$T/log" && break; sleep 0.1; done
grep -Eq "radio: streaming .{1,3} Red Pitaya 127.0.0.8 \(SKRS\), 11 streams of 24 slots \(10 bands, phased ones twice\)" "$T/log"
check "10 bands as 11 SKRS streams (40m phased: two)" $?
sleep 4
s=$(st)
want="[(1830000, 48000, 1), (3550000, 192000, 1), (7040000, 96000, 1), (7040000, 96000, 2), (10120000, 96000, 1), (14050000, 192000, 2), (18090000, 48000, 1), (21075000, 192000, 1), (24910000, 48000, 1), (28100000, 384000, 1), (50160000, 384000, 1)]"
[[ $(py '[(x["hz"], x["rate"], x["input"]) for x in d["skrs"]["streams"]]' "$s") == "$want" ]]
check "the agent: each band at its own rate and input (40m twice, IN1 and IN2)" $?
sj=$(curl -s "http://127.0.0.1:$HTTP/status.json")
ph() { curl -s "http://127.0.0.1:$HTTP/status.json" | python3 -c "import json,sys; d=json.load(sys.stdin); b=[x for x in d['bands'] if x['band']=='40m'][0]['phase']; print($1)"; }
[[ $(ph 'b is not None and b["mode"] == "mvdr" and not b["b_missing"] and b["updates"] >= 2') == True ]]
check "status.json: 40m phased (mvdr, IN2 there, the combiner updating)" $?
v=$(ph '"%.3f %.1f" % (b["coherence"] or 0, b["drop_db"] or 0)')
read -r coh drop <<< "$v"
python3 -c "import sys; sys.exit(0 if float(sys.argv[1]) > 0.9 and float(sys.argv[2]) > 10 else 1)" "$coh" "$drop"
check "IN1 and IN2 lined up by the hardware index: the common noise coherent (|γ|² $coh), nulled ($drop dB)" $?
[[ $(python3 -c "
import socket,sys
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(1)
s.sendto(bytes([0xEF,0xFE,2])+bytes(60),('127.0.0.8',$P))
d=s.recv(100); print(d[2])") == 3 ]]
check "instance A answers busy: SKRS holds its slots" $?
kill "$PID"; wait "$PID" 2>/dev/null; PID=
sleep 0.5
s=$(st)
[[ $(py 'd["skrs"]["streaming"]' "$s") == False ]]
check "SIGTERM stopped SKRS" $?

{ base; echo "[bands]"; for i in $(seq 1 13); do echo "b$i=$((1800000 + i * 1000000)) ip=ab"; done; } > "$T/many.ini"
"$HL" --config "$T/many.ini" --check-config > "$T/many.out" 2>&1
[[ $? == 2 ]] && grep -q "24 slots" "$T/many.out"
check "13 phased bands (26 slots) refused: the bitstream has 24" $?
{ base | sed 's/^stream=skrs$//'; printf '[bands]\n20m=14050000 rate=192\n'; } > "$T/rate.ini"
"$HL" --config "$T/rate.ini" --check-config > "$T/rate.out" 2>&1
[[ $? == 2 ]] && grep -q "stream=skrs" "$T/rate.out"
check "rate= without stream=skrs refused" $?

if [[ $fails -gt 0 ]]; then
  echo "--- headless log"; tail -20 "$T/log"; echo "--- agent log"; tail -5 "$T/agent.log"
fi
echo
[[ $fails == 0 ]] && echo PASS || echo "FAIL ($fails)"
exit $(( fails > 0 ))
