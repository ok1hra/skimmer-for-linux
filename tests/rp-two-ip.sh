#!/usr/bin/env bash
# Two-IP gate: skimmer-headless takes both IPs of a Red Pitaya that runs the
# skimmer's agent — the real agent (skimmer-rp-host --sim) on 127.0.0.6 and
# 127.0.0.7. IP A is [radio] host; IP B's address comes from the agent's
# discovery; the ip=b band streams from B at [rp] b_rate, its in=2 puts it
# on IN2 through Protocol 1; SIGTERM stops both streams. A config with every
# band on ip=b is refused, and so are nine bands on one IP. A phased band
# (ip=ab, docs/PHASING.md) streams paired from both IPs (one epoch, A on
# IN1, B on IN2) into a phased pipeline; when another client takes IP B the
# band goes on from A alone and says so, and phasing comes back with B.
#
#   rp-two-ip.sh <skimmer-headless> <skimmer-rp-host> [skimmer-phase-bench]
set -uo pipefail

HL=$1 AG=$2 PB=${3:-}
T=$(mktemp -d "${TMPDIR:-/tmp}/skimmer-rp-two-ip.XXXXXX")
PID= APID=
cleanup() { [[ -n $PID ]] && kill "$PID" 2>/dev/null; [[ -n $APID ]] && kill "$APID" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

fails=0
check() {  # what, condition-exit-code
  if [[ $2 == 0 ]]; then echo "  ok   $1"; else echo "  FAIL $1"; fails=$((fails + 1)); fi
}

P=$((30000 + RANDOM % 5000))
HTTP=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
"$AG" serve --sim --bind 127.0.0.6:$P,127.0.0.7:$P --ctl "$T/ctl.sock" --orphan-s 2 > "$T/agent.log" 2>&1 &
APID=$!
for _ in $(seq 50); do [[ -S $T/ctl.sock ]] && break; sleep 0.1; done
st() { "$AG" ctl --ctl "$T/ctl.sock" status; }
py() { python3 -c "import json,sys; d=json.loads(sys.argv[1]); print($1)" "$2"; }

cat > "$T/headless.ini" <<INI
[radio]
type=hpsdr-p1
host=127.0.0.6
rate=48000
[rp]
b_rate=96000
[bands]
20m=14045500
40m=7025000 ip=b in=2
[feed]
port=0
[status]
console_s=0
http_port=$HTTP
[web]
settings_from=127.0.0.1
INI
export XDG_CONFIG_HOME=$T/cfg XDG_DATA_HOME=$T/data XDG_STATE_HOME=$T/state
export SKIM_SCP_URL=http://127.0.0.1:9/ SKIM_HPSDR_DISCOVER=127.0.0.6:$P
export SKIM_DISCOVERY_PORT=$((40000 + RANDOM % 5000))
"$HL" --config "$T/headless.ini" > "$T/log" 2>&1 &
PID=$!
for _ in $(seq 100); do grep -q "IP B streaming" "$T/log" && break; sleep 0.1; done
grep -Eq "radio: streaming .{1,3} Red Pitaya 127.0.0.6 \(HPSDR P1\), 1 RX .{1,3} 48 kHz" "$T/log"
check "IP A streams the ip=a band at [radio] rate" $?
grep -Eq "radio: IP B streaming .{1,3} Red Pitaya 127.0.0.7 \(HPSDR P1\), 1 RX .{1,3} 96 kHz" "$T/log"
check "IP B (named by the agent) streams the ip=b band at [rp] b_rate" $?
sleep 1
s=$(st)
[[ $(py '[(i["streaming"], i["rate"], i["receivers"], i["sel"]) for i in d["inst"]]' "$s") == "[(True, 48000, 1, 0), (True, 96000, 1, 1)]" ]]
check "the agent: both streams, B's RX1 on IN2 (in=2 through Protocol 1)" $?
sj=$(curl -s "http://127.0.0.1:$HTTP/status.json")
[[ $sj == *"IP B streaming"* ]]
check "status.json: the radio's state names IP B" $?
kill "$PID"; wait "$PID" 2>/dev/null; PID=
sleep 0.5
s=$(st)
[[ $(py '[(i["streaming"], i["sel"]) for i in d["inst"]]' "$s") == "[(False, 0), (False, 0)]" ]]
check "SIGTERM stopped both streams; B's inputs back to the manager's" $?
[[ ! -s $T/state/skimmer-for-linux/hpsdr-streams ]]
check "no stream left noted" $?

# a phased band: 20m from both inputs, 40m from IP A alone
cat > "$T/phased.ini" <<INI
[radio]
type=hpsdr-p1
host=127.0.0.6
rate=48000
[bands]
20m=14045500 ip=ab
40m=7025000
[feed]
port=0
[status]
console_s=0
http_port=$HTTP
[web]
settings_from=127.0.0.1
[record]
bands=20m
minutes=0.05
dir=$T/rec
INI
"$HL" --config "$T/phased.ini" > "$T/logp" 2>&1 &
PID=$!
for _ in $(seq 100); do grep -q "IP B streaming" "$T/logp" && break; sleep 0.1; done
sleep 4
s=$(st)
[[ $(py '[(i["streaming"], i["receivers"], i["sel"], i["paired"]) for i in d["inst"]]' "$s") == "[(True, 2, 0, True), (True, 1, 1, True)]" ]] &&
  [[ $(py 'd["inst"][0]["epoch"] == d["inst"][1]["epoch"]' "$s") == True ]]
check "phased: both IPs stream paired, one epoch; 20m on IN1 at A and on IN2 at B" $?
ph() { curl -s "http://127.0.0.1:$HTTP/status.json" | python3 -c "import json,sys; d=json.load(sys.stdin); b=[x for x in d['bands'] if x['band']=='20m'][0]['phase']; print($1)"; }
[[ $(ph 'b is not None and b["mode"] == "mvdr" and not b["b_missing"] and b["updates"] >= 2') == True ]] &&
  [[ $(curl -s "http://127.0.0.1:$HTTP/status.json" | python3 -c "import json,sys; d=json.load(sys.stdin); print([x['phase'] for x in d['bands'] if x['band']=='40m'][0])") == None ]]
check "status.json: 20m phased (mvdr, B there, the combiner updating), 40m not" $?
# the sim's noise is the same function of the sample number on both inputs:
# only blocks paired at the same instants keep it coherent, and can null it
v=$(ph '"%.3f %.1f" % (b["coherence"] or 0, b["drop_db"] or 0)')
read -r coh drop <<< "$v"
python3 -c "import sys; sys.exit(0 if float(sys.argv[1]) > 0.9 and float(sys.argv[2]) > 10 else 1)" "$coh" "$drop"
check "A and B paired at the same instants: the common noise coherent (|γ|² $coh), nulled ($drop dB)" $?
# the waterfall's Φ: POST /api/phase switches the band live; GET gives its line
code=$(curl -s -o "$T/pa.json" -w '%{http_code}' -d 'band=20m&mode=a' "http://127.0.0.1:$HTTP/api/phase")
ok=1
for _ in $(seq 40); do [[ $(ph 'b["mode"] == "a" and b["est"]') == True ]] && { ok=0; break; }; sleep 0.1; done
[[ $code == 200 && $ok == 0 ]]
check "POST /api/phase mode=a: 20m runs on antenna A, the line shows what phasing would give" $?
c1=$(curl -s -o /dev/null -w '%{http_code}' -d 'band=40m&mode=a' "http://127.0.0.1:$HTTP/api/phase")
c2=$(curl -s -o /dev/null -w '%{http_code}' -d 'band=20m&mode=best' "http://127.0.0.1:$HTTP/api/phase")
c3=$(curl -s -o /dev/null -w '%{http_code}' -H 'Origin: http://evil.example' -d 'band=20m&mode=a' "http://127.0.0.1:$HTTP/api/phase")
[[ $c1 == 400 && $c2 == 400 && $c3 == 403 ]]
check "… refused for a band not phased (400), an unknown mode (400), another origin (403)" $?
curl -s -d 'band=20m&mode=mvdr' "http://127.0.0.1:$HTTP/api/phase" > /dev/null
ok=1
for _ in $(seq 40); do [[ $(ph 'b["mode"] == "mvdr" and not b["est"]') == True ]] && { ok=0; break; }; sleep 0.1; done
n=$(curl -s "http://127.0.0.1:$HTTP/api/phase?band=20m" | python3 -c "import json,sys; d=json.load(sys.stdin); print(len(d['drop']) if d.get('phased') else 0)")
[[ $ok == 0 && $n == 384 ]]
check "… back to mvdr; GET /api/phase: the line, one value a channel ($n)" $?
# the phasing panel: a fixed weight, a memory, Save into the config (no restart)
curl -s -d 'band=20m&mode=fixed&gain_db=6&phase_deg=180&m2=6,180' "http://127.0.0.1:$HTTP/api/phase" > /dev/null
code=$(curl -s -o "$T/save.json" -w '%{http_code}' -d 'band=20m&mode=fixed&gain_db=6&phase_deg=180&action=save' "http://127.0.0.1:$HTTP/api/phase")
pj=$(curl -s "http://127.0.0.1:$HTTP/api/phase?band=20m")
[[ $code == 200 && $(cat "$T/save.json") == *'"saved":true'* ]] && grep -q '^20m=14045500 ip=ab phase=fixed$' "$T/phased.ini" &&
  grep -q '^fixed_gain_db=6.00$' "$T/phased.ini" && grep -q '^m2=6.00,180.0$' "$T/phased.ini" &&
  [[ $pj == *'"fixed_gain_db":6.00'* && $pj == *'"mem":[null,[6.00,180.0],null]'* ]] && pgrep -f "$T/phased.ini" > /dev/null
check "the panel: a fixed weight and M2, Save → phase=fixed and [phase.20m] in the config, no restart" $?
curl -s -d 'band=20m&mode=mvdr' "http://127.0.0.1:$HTTP/api/phase" > /dev/null
for _ in $(seq 40); do [[ $(ph 'b["mode"] == "mvdr"') == True ]] && break; sleep 0.1; done
# another client takes IP B: its start from a socket that closes (the agent stops it as an orphan)
python3 -c "import socket,sys; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.sendto(bytes([0xef,0xfe,4,1])+bytes(60),('127.0.0.7',int(sys.argv[1])))" $P
lost=1
for _ in $(seq 60); do
  [[ $(ph 'b["b_missing"]') == True ]] && { lost=0; break; }
  sleep 0.1
done
sj=$(curl -s "http://127.0.0.1:$HTTP/status.json")
[[ $lost == 0 && $sj == *"20m phased: IN1 alone (IP B missing)"* ]]
check "IP B taken: 20m goes on from IN1 alone, and the radio's state says so" $?
back=1
for _ in $(seq 200); do
  [[ $(ph 'not b["b_missing"]') == True ]] && { back=0; break; }
  sleep 0.1
done
[[ $back == 0 ]] && grep -q "IP B streaming" "$T/logp"
check "IP B free again: phasing comes back by itself" $?
kill "$PID"; wait "$PID" 2>/dev/null; PID=
# [record] of the phased band: A and B side by side, each .meta naming the other
ra=$(ls "$T"/rec/hd-20m-*Z.cf32 2>/dev/null | head -1); rb=${ra%.cf32}-b.cf32
[[ -n $ra && -s $ra && -s $rb && $(stat -c %s "$ra") == $(stat -c %s "$rb") ]] &&
  grep -q "^pair_b: $(basename "$rb")$" "$ra.meta" && grep -q "^pair_a: $(basename "$ra")$" "$rb.meta"
check "[record] of a phased band: A and B as long as each other, each .meta naming the other" $?
if [[ -n $PB && -n $ra ]]; then
  out=$("$PB" "$ra" 2>&1)
  [[ $out == *"$(basename "$rb")"* && $out == *"B lag 0 samples (measured"* ]]
  check "skimmer-phase-bench finds the pair from A's file, B lag 0" $?
fi
# in=12: phased on IP A alone (two of its receivers, IN1 and IN2); IP B free
sed -e 's/^20m=14045500 ip=ab.*/20m=14045500 in=12/' -e '/^\[record\]/,/^dir=/d' "$T/phased.ini" > "$T/same.ini"
"$HL" --config "$T/same.ini" > "$T/logs" 2>&1 &
PID=$!
for _ in $(seq 100); do grep -q "radio: streaming" "$T/logs" && break; sleep 0.1; done
sleep 4
s=$(st)
v=$(ph '"%.3f %s" % (b["coherence"] or 0, b["b_missing"])')
read -r coh miss <<< "$v"
got=$(py '[(i["streaming"], i["receivers"], i["sel"]) for i in d["inst"]]' "$s")
[[ $got == "[(True, 3, 4), (False, 1, 0)]" ]] &&
  [[ $miss == False ]] && python3 -c "import sys; sys.exit(0 if float(sys.argv[1]) > 0.9 else 1)" "$coh"
check "in=12: 20m phased on IP A alone (RX1 IN1, RX3 IN2: $got), IP B untouched, the common noise coherent (|γ|² $coh)" $?
kill "$PID"; wait "$PID" 2>/dev/null; PID=
printf '[radio]\nhost=127.0.0.6\nrate=48000\n[rp]\nb_rate=96000\n[bands]\n20m=14045500 ip=ab\n' > "$T/prate.ini"
out=$("$HL" --config "$T/prate.ini" --check-config 2>&1); rc=$?
[[ $rc == 2 && $out == *"ip=ab needs one rate on both IPs"* ]]
check "ip=ab with two rates refused (exit 2)" $?

# 384 kHz on IP B: the stream, the channelizer at that rate
sed -e 's/^b_rate=96000/b_rate=384000/' -e 's/^40m=7025000 ip=b in=2/40m=7100000 ip=b\n15m=21100000 ip=b/' "$T/headless.ini" > "$T/h384.ini"
"$HL" --config "$T/h384.ini" > "$T/log384" 2>&1 &
PID=$!
for _ in $(seq 100); do grep -q "IP B streaming" "$T/log384" && break; sleep 0.1; done
sleep 1.5
grep -Eq "radio: IP B streaming .{1,3} Red Pitaya 127.0.0.7 \(HPSDR P1\), 2 RX .{1,3} 384 kHz" "$T/log384" &&
  grep -q "40m cw: 3072 channels" "$T/log384"
check "IP B at 384 kHz: 2 RX streaming, 3072 CW channels a band" $?
s=$(st)
[[ $(py '[(i["streaming"], i["rate"], i["receivers"]) for i in d["inst"]]' "$s") == "[(True, 48000, 1), (True, 384000, 2)]" ]]
check "the agent: B at 384 kHz with 2 receivers" $?
kill "$PID"; wait "$PID" 2>/dev/null; PID=
{ printf '[radio]\nhost=127.0.0.6\nrate=384000\n[bands]\n'; for i in 1 2 3 4 5; do printf 'b%d=%d000000\n' $i $((i + 2)); done; } > "$T/five384.ini"
out=$("$HL" --config "$T/five384.ini" --check-config 2>&1); rc=$?
[[ $rc == 2 && $out == *"IP A at 384 kHz: 4 receivers at most"* ]]
check "five receivers at 384 kHz on one IP refused (exit 2)" $?

printf '[radio]\nhost=127.0.0.6\n[bands]\n40m=7025000 ip=b\n' > "$T/bonly.ini"
out=$("$HL" --config "$T/bonly.ini" --check-config 2>&1); rc=$?
[[ $rc == 2 && $out == *"every band is ip=b"* ]]
check "every band on ip=b refused (exit 2)" $?
{ printf '[radio]\nhost=127.0.0.6\n[bands]\n'; for i in 1 2 3 4 5 6 7 8 9; do printf 'b%d=%d000000 ip=b\n' $i $((i + 1)); done; printf 'a=1830000\n'; } > "$T/nine.ini"
out=$("$HL" --config "$T/nine.ini" --check-config 2>&1); rc=$?
[[ $rc == 2 && $out == *"8 receivers an IP at most (IP B)"* ]]
check "nine bands on one IP refused (exit 2)" $?
{ printf '[radio]\nhost=127.0.0.6\n[bands]\n'; for i in 1 2 3 4 5 6 7 8; do printf 'b%d=%d000000 ip=b\na%d=%d100000\n' $i $((i + 1)) $i $((i + 1)); done; } > "$T/sixteen.ini"
out=$("$HL" --config "$T/sixteen.ini" --check-config 2>&1); rc=$?
[[ $rc == 0 ]]
check "sixteen bands, eight an IP: fine" $?

if ((fails)); then echo "rp-two-ip: $fails FAILED"; cat "$T/log" | tail -n 30; exit 1; fi
echo "rp-two-ip: all passed"
