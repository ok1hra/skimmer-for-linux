#!/usr/bin/env bash
# Headless gate — the whole skimmer-headless with no radio: [radio] type=mock
# keys "CQ CQ DE OK1MCK OK1MCK K" on two bands in real time. The gate checks
# that the config is read (band modes, per-band engine), the source streams,
# the spot reaches the telnet feed at the right frequency, /status.json says
# so (with the built-in gate and the SKIM_* switches), SIGTERM stops it
# cleanly, and a bad config stops the start with exit 2.
#
#   headless-mock.sh <skimmer-headless>
set -uo pipefail

HL=$1
T=$(mktemp -d "${TMPDIR:-/tmp}/skimmer-headless-mock.XXXXXX")
PID=
cleanup() { [[ -n $PID ]] && kill "$PID" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

fails=0
check() {  # what, condition-exit-code
  if [[ $2 == 0 ]]; then echo "  ok   $1"; else echo "  FAIL $1"; fails=$((fails + 1)); fi
}

FEED=$((20000 + RANDOM % 5000))
HTTP=$((25000 + RANDOM % 5000))
cat > "$T/headless.ini" <<EOF
[radio]
type=mock
rate=96000
[bands]
20m=14045500
40m=7045500 cw:v2,rtty
[feed]
call=TEST
port=$FEED
settle_nodict_s=0
[status]
console_s=0
http_port=$HTTP
EOF

# no network in the gate: the MASTER.SCP updater gets a dead URL
export XDG_CONFIG_HOME=$T/cfg XDG_DATA_HOME=$T/data SKIM_SCP_URL=http://127.0.0.1:9/
"$HL" --config "$T/headless.ini" > "$T/log" 2>&1 &
PID=$!

# the telnet feed: log in, wait for the mock's call (≤ 60 s: two readings,
# 8 s settle, one 8 s message per round)
got=
for _ in $(seq 50); do
  if { exec 3<>/dev/tcp/127.0.0.1/$FEED; } 2>/dev/null; then break; fi
  sleep 0.2
done
if { true >&3; } 2>/dev/null; then
  printf 'TEST\r\n' >&3
  end=$((SECONDS + 60))
  while ((SECONDS < end)); do
    if IFS= read -r -t 5 line <&3; then
      if [[ $line == *OK1MCK* ]]; then got=$line; break; fi
    fi
  done
  exec 3>&-
fi
echo "  feed: ${got:-nothing}"
[[ $got == "DX de TEST-#:"*OK1MCK*CW*CQ* ]]; check "the mock's CQ is spotted on the telnet feed" $?
[[ $got == *" 14046.5 "* || $got == *" 7046.5 "* ]]; check "… at centre + 1 kHz" $?

sleep 1.5                                    # the status snapshot is 1 Hz
json=$(curl -s "http://127.0.0.1:$HTTP/status.json" || true)
py() { python3 -c "import json,sys; d=json.loads(sys.argv[1]); print($1)" "$json" 2>/dev/null; }
[[ $(py 'd["streaming"]') == True ]]; check "status: streaming" $?
[[ $(py 'd["radio"]') == *mock* ]]; check "status: the radio is the mock" $?
[[ $(py '",".join(b["band"] + ":" + b["engine"] for b in d["bands"])') == "20m:cw-v2,40m:cw-v2" ]]
check "status: both bands, each with its engine" $?
[[ $(py 'd["feed_gate"]["id"]') == builtin:feed-gate.ini#* && $(py 'd["feed_gate"]["mode"]') == shadow ]]
check "status: the built-in gate in shadow" $?
[[ $(py 'd["switches"]["SKIM_SCP_URL"]') == http://127.0.0.1:9/ ]]
check "status: the SKIM_* switches of the start" $?
[[ $(py 'sum(b["spots"] for b in d["bands"]) > 0') == True ]]; check "status: spots counted" $?
page=$(curl -s "http://127.0.0.1:$HTTP/")
[[ $page == *"<title>Skimmer for Linux</title>"* ]]; check "web: the page" $?
[[ $(curl -s -o /dev/null -w '%{http_code} %{content_type}' "http://127.0.0.1:$HTTP/app.js") == "200 text/javascript"* ]]
check "web: app.js" $?
[[ $(curl -s "http://127.0.0.1:$HTTP/api/log") == '[{"t":'*'"text":"skimmer-headless '* ]]; check "web: the log" $?
[[ $(curl -s -o /dev/null -w '%{http_code}' --path-as-is "http://127.0.0.1:$HTTP/../headless.ini") == 404 ]]
check "web: no path out of the page" $?
grep -q "band 40m: RTTY decoding is not in skimmer-headless yet" "$T/log"
check "log: RTTY asked for but not there yet — said so" $?
grep -q "switch: SKIM_SCP_URL=http://127.0.0.1:9/ (system)" "$T/log"
check "log: the switch with its kind" $?

kill -TERM "$PID"
rc=0; wait "$PID" || rc=$?
PID=
[[ $rc == 0 ]]; check "SIGTERM stops it cleanly (exit 0, got $rc)" $?
grep -q "stopped" "$T/log"; check "log: stopped" $?

printf '[bands]\n20m=14045500 psk\n' > "$T/bad.ini"
rc=0; "$HL" --config "$T/bad.ini" > "$T/bad.log" 2>&1 || rc=$?
[[ $rc == 2 ]] && grep -q '"psk"' "$T/bad.log"; check "a bad config stops the start: exit 2, the reason named" $?

"$HL" --list-switches | grep -q "^SKIM_V2_EDGE behaviour$"; check "--list-switches" $?
out=$("$HL" --check-config --config "$T/headless.ini" 2>&1); rc=$?
[[ $rc == 0 && $out == *"RX2    40m   7045.5 kHz  cw,rtty"* ]]; check "--check-config: fine, the bands listed" $?
rc=0; "$HL" --check-config --config "$T/none.ini" > /dev/null 2>&1 || rc=$?
[[ $rc == 2 && ! -e $T/none.ini ]]; check "--check-config on a missing file: exit 2, nothing written" $?

if ((fails)); then
  echo "--- log"; cat "$T/log"
  echo "FAIL ($fails)"; exit 1
fi
echo "PASS"
