#!/usr/bin/env bash
# Two-IP gate: skimmer-headless takes both IPs of a Red Pitaya that runs the
# skimmer's agent — the real agent (skimmer-rp-host --sim) on 127.0.0.6 and
# 127.0.0.7. IP A is [radio] host; IP B's address comes from the agent's
# discovery; the ip=b band streams from B at [rp] b_rate, its in=2 puts it
# on IN2 through Protocol 1; SIGTERM stops both streams. A config with every
# band on ip=b is refused, and so are nine bands on one IP.
#
#   rp-two-ip.sh <skimmer-headless> <skimmer-rp-host>
set -uo pipefail

HL=$1 AG=$2
T=$(mktemp -d "${TMPDIR:-/tmp}/skimmer-rp-two-ip.XXXXXX")
PID= APID=
cleanup() { [[ -n $PID ]] && kill "$PID" 2>/dev/null; [[ -n $APID ]] && kill "$APID" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

fails=0
check() {  # what, condition-exit-code
  if [[ $2 == 0 ]]; then echo "  ok   $1"; else echo "  FAIL $1"; fails=$((fails + 1)); fi
}

P=$((30000 + RANDOM % 5000))
HTTP=$((35000 + RANDOM % 5000))
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
