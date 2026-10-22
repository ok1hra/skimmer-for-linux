#!/usr/bin/env bash
# Headless gate — the whole skimmer-headless with no radio: [radio] type=mock
# keys "CQ CQ DE OK1MCK OK1MCK K" on two bands in real time, and sends "CQ CQ
# DE OK1RTY OK1RTY K" in RTTY on the band that decodes RTTY. The gate checks
# that the config is read (band modes, per-band engine), the source streams,
# both spots reach the telnet feed at the right frequency, /status.json says
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
[decode]
suppress_cw_on_rtty=true
[feed]
call=TEST
port=$FEED
settle_nodict_s=0
[status]
console_s=0
http_port=$HTTP
EOF

# no network in the gate: the MASTER.SCP updater gets a dead URL
export XDG_CONFIG_HOME=$T/cfg XDG_DATA_HOME=$T/data XDG_STATE_HOME=$T/state
export SKIM_SCP_URL=http://127.0.0.1:9/
"$HL" --config "$T/headless.ini" > "$T/log" 2>&1 &
PID=$!

# the telnet feed: log in, wait for the mock's calls (≤ 90 s: two readings,
# 8 s settle, one 8 s message per round)
got= rgot=
for _ in $(seq 50); do
  if { exec 3<>/dev/tcp/127.0.0.1/$FEED; } 2>/dev/null; then break; fi
  sleep 0.2
done
if { true >&3; } 2>/dev/null; then
  printf 'TEST\r\n' >&3
  end=$((SECONDS + 90))
  while ((SECONDS < end)) && [[ -z $got || -z $rgot ]]; do
    if IFS= read -r -t 5 line <&3; then
      [[ $line == *OK1MCK* && -z $got ]] && got=$line
      [[ $line == *OK1RTY* && -z $rgot ]] && rgot=$line
    fi
  done
  exec 3>&-
fi
echo "  feed: ${got:-nothing}"
echo "  feed: ${rgot:-nothing}"
[[ $got == "DX de TEST-#:"*OK1MCK*CW*CQ* ]]; check "the mock's CQ is spotted on the telnet feed" $?
[[ $got == *" 14046.5 "* || $got == *" 7046.5 "* ]]; check "… at centre + 1 kHz" $?
[[ $rgot == "DX de TEST-#:"*" 7035.5 "*OK1RTY*RTTY*"45 BPS"*CQ* ]]
check "the RTTY CQ is spotted too, 40m only, at centre - 10 kHz, 45 BPS" $?

sleep 1.5                                    # the status snapshot is 1 Hz
json=$(curl -s "http://127.0.0.1:$HTTP/status.json" || true)
py() { python3 -c "import json,sys; d=json.loads(sys.argv[1]); print($1)" "$json" 2>/dev/null; }
[[ $(py 'd["streaming"]') == True ]]; check "status: streaming" $?
[[ $(py 'd["radio"]') == *mock* ]]; check "status: the radio is the mock" $?
[[ $(py '",".join(b["band"] + ":" + b["engine"] for b in d["bands"])') == "20m:cw-v2,40m:cw-v2" ]]
check "status: both bands, each with its engine" $?
[[ $(py 'd["bands"][0]["rtty"], d["bands"][1]["rtty"]["engine"], d["bands"][1]["rtty"]["spots"] > 0, d["feed_rtty"]') == "None rtty True True" ]]
check "status: RTTY on 40m only, with its spots" $?
[[ $(py 'd["suppress_cw_on_rtty"], d["bands"][1]["rtty_suppressed"] > 0, d["bands"][0]["rtty_suppressed"]') == "True True 0" ]]
check "status: 40m CW kept off the mock's RTTY (decodes dropped), 20m untouched" $?
[[ $(py 'd["feed_gate"]["id"]') == builtin:feed-gate.ini#* && $(py 'd["feed_gate"]["mode"]') == shadow ]]
check "status: the built-in gate in shadow" $?
[[ $(py 'd["switches"]["SKIM_SCP_URL"]') == http://127.0.0.1:9/ ]]
check "status: the SKIM_* switches of the start" $?
[[ $(py 'sum(b["spots"] for b in d["bands"]) > 0') == True ]]; check "status: spots counted" $?
page=$(curl -s "http://127.0.0.1:$HTTP/")
[[ $page == *"<title>Skimmer for Linux</title>"* ]]; check "web: the page" $?
[[ $(curl -s -o /dev/null -w '%{http_code} %{content_type}' "http://127.0.0.1:$HTTP/app.js") == "200 text/javascript"* ]]
check "web: app.js" $?
stj=$(curl -s "http://127.0.0.1:$HTTP/stats.json?hours=6")
# (rtty: null until a minute with RTTY was recorded, and then never on 20m)
[[ $(python3 -c "import json,sys; d=json.loads(sys.argv[1]); r=d['rtty']; print(d['bands'], d['n'], d['step_min'], r is None or all(v is None for v in r['spots']['20m']), len(d['cw']['spots']['20m']), sorted(d['global']))" "$stj" 2>&1) == "['20m', '40m'] 360 1 True 360 ['clients', 'cpu', 'lost', 'radio', 'streaming']" ]]
check "web: stats.json (6 h, both bands, the global series)" $?
[[ $(curl -s "http://127.0.0.1:$HTTP/api/log") == '[{"t":'*'"text":"skimmer-headless '* ]]; check "web: the log" $?
[[ $(curl -s -o /dev/null -w '%{http_code}' --path-as-is "http://127.0.0.1:$HTTP/../headless.ini") == 404 ]]
check "web: no path out of the page" $?
grep -q "decode: 40m CW, engine cw-v2 + RTTY$" "$T/log" && grep -q "decode: 20m CW, engine cw-v2$" "$T/log" &&
  grep -q "decode: 40m CW stays off the RTTY stations RTTY copies" "$T/log"
check "log: what each band decodes" $?
grep -q "switch: SKIM_SCP_URL=http://127.0.0.1:9/ (system)" "$T/log"
check "log: the switch with its kind" $?

# ---- settings: read, refuse, save + restart, restore -----------------------------
U=http://127.0.0.1:$HTTP
started() { curl -s "$U/status.json" | python3 -c "import json,sys; print(json.load(sys.stdin)['started_us'])" 2>/dev/null || echo -1; }
nbands() { curl -s "$U/status.json" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['bands']))" 2>/dev/null || echo -1; }
restarted() {  # wait for a new start: started_us moves on
  local before=$1
  for _ in $(seq 40); do
    sleep 0.5; local u; u=$(started)
    [[ $u != -1 && $u != "$before" ]] && return 0
  done
  return 1
}
cj=$(curl -s "$U/api/config")
[[ $(python3 -c "import json,sys; d=json.loads(sys.argv[1]); print(d['may_change'], len(d['bands']), d['values']['feed.port'], d['readonly'].get('feed.settle_nodict_s'))" "$cj") == "True 2 $FEED 0" ]]
check "settings: read (may change from 127.0.0.1, bands, values, the read-only rest)" $?
code=$(curl -s -o /dev/null -w '%{http_code}' -H "Origin: http://evil.example" -d 'feed.call=X' "$U/api/config")
[[ $code == 403 ]]; check "settings: a page from another origin is refused (403)" $?
before=$(md5sum < "$T/headless.ini")
code=$(curl -s -o "$T/r.json" -w '%{http_code}' --data-urlencode 'bands=20m=14045500 psk' "$U/api/config")
[[ $code == 400 && $(md5sum < "$T/headless.ini") == "$before" ]] && grep -q psk "$T/r.json"
check "settings: a bad band is refused (400, the reason, the file untouched)" $?
u0=$(started)
code=$(curl -s -o "$T/r.json" -w '%{http_code}' -H "Origin: $U" \
       --data-urlencode $'bands=20m=14045500\n40m=7045500 cw:v2,rtty\n17m=18113500' "$U/api/config")
[[ $code == 200 ]] && grep -q '"restart":true' "$T/r.json"; check "settings: saved (200, restart)" $?
restarted "$u0"; check "settings: the skimmer restarted itself (exec, no supervisor)" $?
[[ $(nbands) == 3 ]]; check "settings: … and runs the new config (3 bands)" $?
bak=$(cd "$T" && ls headless.ini.bak-* 2>/dev/null | head -1)
[[ -n $bak ]] && ! grep -q 17m "$T/$bak"; check "settings: the previous file kept as $bak" $?
u0=$(started)
code=$(curl -s -o /dev/null -w '%{http_code}' --data-urlencode "name=$bak" "$U/api/rollback")
[[ $code == 200 ]] && restarted "$u0" && [[ $(nbands) == 2 ]]
check "settings: restoring it puts 2 bands back (restart)" $?
code=$(curl -s -o /dev/null -w '%{http_code}' --data-urlencode "name=../../headless.ini" "$U/api/rollback")
[[ $code == 400 ]]; check "settings: restore refuses a name that is no backup" $?

kill -TERM "$PID"
rc=0; wait "$PID" || rc=$?
PID=
[[ $rc == 0 ]]; check "SIGTERM stops it cleanly (exit 0, got $rc)" $?
grep -q "stopped" "$T/log"; check "log: stopped" $?

# settings_from that does not hold this machine: read-only
H2=$((HTTP + 1))
printf '[radio]\ntype=mock\n[bands]\n20m=14045500\n[feed]\nport=0\n[status]\nconsole_s=0\nhttp_port=%s\n[web]\nsettings_from=10.0.0.0/8\n' "$H2" > "$T/ro.ini"
"$HL" --config "$T/ro.ini" > "$T/ro.log" 2>&1 &
P2=$!
for _ in $(seq 20); do curl -s -o /dev/null "http://127.0.0.1:$H2/status.json" && break; sleep 0.2; done
code=$(curl -s -o "$T/r.json" -w '%{http_code}' -d 'feed.call=X' "http://127.0.0.1:$H2/api/config")
may=$(curl -s "http://127.0.0.1:$H2/api/config" | python3 -c "import json,sys; print(json.load(sys.stdin)['may_change'])")
kill -TERM $P2; wait $P2 2>/dev/null
[[ $code == 403 && $may == False ]] && grep -q settings_from "$T/r.json"
check "settings: an address outside settings_from reads only (403, may_change false)" $?

printf '[bands]\n20m=14045500 psk\n' > "$T/bad.ini"
rc=0; "$HL" --config "$T/bad.ini" > "$T/bad.log" 2>&1 || rc=$?
[[ $rc == 2 ]] && grep -q '"psk"' "$T/bad.log"; check "a bad config stops the start: exit 2, the reason named" $?

"$HL" --list-switches | grep -q "^SKIM_V2_EDGE behaviour$"; check "--list-switches" $?
out=$("$HL" --check-config --config "$T/headless.ini" 2>&1); rc=$?
[[ $rc == 0 && $out == *"RX2    40m   7045.5 kHz  cw,rtty"* && $out == *"CW and RTTY"* ]]
check "--check-config: fine, the bands listed" $?
printf '[bands]\n40m=7045500 rtty\n[feed]\nrtty=false\n' > "$T/cwonly.ini"
out=$("$HL" --check-config --config "$T/cwonly.ini" 2>&1); rc=$?
[[ $rc == 0 && $out == *"40m   7045.5 kHz  rtty"* && $out == *"CW only"* ]]
check "--check-config: an RTTY-only band, [feed] rtty=false" $?
printf '[bands]\n40m=7045500\n[feed]\nrtty=maybe\n' > "$T/badbool.ini"
rc=0; "$HL" --check-config --config "$T/badbool.ini" > "$T/bad.log" 2>&1 || rc=$?
[[ $rc == 2 ]] && grep -q "rtty: true or false" "$T/bad.log"; check "[feed] rtty=maybe: exit 2" $?
rc=0; "$HL" --check-config --config "$T/none.ini" > /dev/null 2>&1 || rc=$?
[[ $rc == 2 && ! -e $T/none.ini ]]; check "--check-config on a missing file: exit 2, nothing written" $?

if ((fails)); then
  echo "--- log"; cat "$T/log"
  echo "FAIL ($fails)"; exit 1
fi
echo "PASS"
