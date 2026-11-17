#!/usr/bin/env bash
# Hub gate — a hub and a worker on this machine, no radio. The hub runs the
# mock source with 40m decoded here and 20m passed to the worker "pc2"; the
# worker ([radio] type=hub) gets its band and policy from the hub, decodes
# the IQ it is sent and its spot leaves on the HUB's feed under the hub's
# call. The worker's own telnet feed is also one of the hub's peers, so its
# spot reaches the hub twice — once goes out, the other is a counted
# duplicate. Then the hub assigns 40m to pc2 too: the worker restarts by
# itself and decodes both. Discovery lists the hub and the worker in their
# roles; SIGTERM stops both cleanly.
#
#   hub-mock.sh <skimmer-headless>
set -uo pipefail

HL=$1
T=$(mktemp -d "${TMPDIR:-/tmp}/skimmer-hub-mock.XXXXXX")
HPID= WPID=
cleanup() { for p in $HPID $WPID; do kill "$p" 2>/dev/null; done; rm -rf "$T"; }
trap cleanup EXIT

fails=0
check() {
  if [[ $2 == 0 ]]; then echo "  ok   $1"; else echo "  FAIL $1${3:+ (got: $3)}"; fails=$((fails + 1)); fi
}

R=$((20000 + RANDOM % 9000))
HFEED=$R HHTTP=$((R + 1)) HUB=$((R + 2)) WFEED=$((R + 3)) WHTTP=$((R + 4))
export SKIM_DISCOVERY_PORT=$((R + 5)) SKIM_SCP_URL=http://127.0.0.1:9/
unset SKIM_CW_ENGINE SKIM_DEEPCW_MODEL

mkdir -p "$T/h" "$T/w"
cat > "$T/h/headless.ini" <<EOF
[radio]
type=mock
[bands]
40m=7045500 cw
20m=14045500 cw @pc2
[feed]
call=OK1HUB
port=$HFEED
settle_nodict_s=0
[hub]
name=central
port=$HUB
allow=127.0.0.0/8
[peers]
hosts=127.0.0.1:$WFEED
[status]
console_s=0
http_port=$HHTTP
EOF
cat > "$T/w/headless.ini" <<EOF
[radio]
type=hub
host=127.0.0.1:$HUB
[feed]
port=$WFEED
[hub]
name=pc2
port=0
[status]
console_s=0
http_port=$WHTTP
EOF

start() {  # dir — in the background, $! = its pid
  XDG_CONFIG_HOME=$T/$1/cfg XDG_DATA_HOME=$T/$1/data XDG_STATE_HOME=$T/$1/state \
    "$HL" --config "$T/$1/headless.ini" > "$T/$1/log" 2>&1 &
}
start h; HPID=$!
sleep 1
start w; WPID=$!

# the hub's feed: log in, wait for both calls (the hub's own 40m, pc2's 20m)
for _ in $(seq 50); do { exec 3<>/dev/tcp/127.0.0.1/$HFEED; } 2>/dev/null && break; sleep 0.2; done
got40= got20=
if { true >&3; } 2>/dev/null; then
  IFS= read -r -t 3 -d ':' _ <&3
  printf 'TEST\r\n' >&3
  end=$((SECONDS + 100))
  while (( SECONDS < end )) && [[ -z $got40 || -z $got20 ]]; do
    IFS= read -r -t 5 line <&3 || continue
    [[ $line == *"OK1HUB-#:"*" 7046.5 "*"OK1MCK"* ]] && got40=1
    [[ $line == *"OK1HUB-#:"*"14046.5 "*"OK1MCK"* ]] && got20=1
    echo "$line" >> "$T/feed"
  done
fi
[[ -n $got40 ]]; check "the hub's own band (40m) is spotted on its feed" $?
[[ -n $got20 ]]; check "pc2's band (20m) is spotted on the hub's feed, under the hub's call" $?
sleep 3
[[ $(grep -c "14046.5" "$T/feed") == 1 ]]; check "… once: the same spot through the peer feed was a duplicate" $?

sh=$(curl -s "http://127.0.0.1:$HHTTP/status.json")
py() { python3 -c "import json,sys; d=json.loads(sys.argv[1]); print($2)" "$1" 2>&1; }
v=$(py "$sh" 'd["hub"]["workers"][0]["name"], d["hub"]["workers"][0]["bands"]')
[[ $v == "pc2 [1]" ]]; check "hub status: the worker pc2 with band 20m" $? "$v"
[[ $(py "$sh" 'd["hub"]["workers"][0]["mbps"] > 3 and d["hub"]["workers"][0]["dropped"] == 0') == True ]]
check "… IQ at ~4.6 Mb/s, nothing dropped" $?
v=$(py "$sh" '[b.get("worker") for b in d["bands"]], d["bands"][1]["remote_spots"] >= 1')
[[ $v == "[None, 'pc2'] True" ]]; check "… 20m decoded by pc2, its spots counted" $? "$v"
[[ $(py "$sh" 'd["external_dups"] >= 1 and d["peers"][0]["connected"]') == True ]]
check "… the worker's feed is a peer, its copy a counted duplicate" $?
[[ $(py "$sh" 'd["hub"]["workers"][0]["status"]["bands"][0]["band"]') == 20m ]]
check "… the worker's own status comes with it" $?
ws=$(curl -s "http://127.0.0.1:$WHTTP/status.json")
v=$(py "$ws" '"streaming from the hub" in d["radio"], d["streaming"], [b["band"] for b in d["bands"]]')
[[ $v == "True True ['20m']" ]]; check "worker status: streaming from the hub, the one band" $? "$v"
grep -q "hub link: 127.0.0.1:$HUB assigns 1 band to pc2" "$T/w/log"; check "worker log: the band assigned" $?

v=$(py "$(curl -s "http://127.0.0.1:$HHTTP/api/spots")" '[(x["band"], x["src"]) for x in d["spots"] if x["call"] == "OK1MCK"]')
# 20m comes from pc2 — over the link or through its feed (the peer), whichever is first
[[ ( $v == *"('20m', 'pc2')"* || $v == *"('20m', '127.0.0.1:$WFEED')"* ) && $v == *"('40m', '')"* ]]
check "hub /api/spots: 20m made by pc2 (link or peer), 40m here" $? "$v"

out=$("$HL" --find-skimmers 2>&1)
[[ $out == *"central"*" hub "*"OK1HUB"* && $out == *"pc2"*" worker "* ]]
check "--find-skimmers: the hub and the worker in their roles" $?

# the hub assigns 40m to pc2 too: the hub restarts, the worker follows
code=$(curl -s -o "$T/post" -w '%{http_code}' --data-urlencode $'bands=40m=7045500 cw @pc2\n20m=14045500 cw @pc2' \
       "http://127.0.0.1:$HHTTP/api/config")
[[ $code == 200 ]]; check "hub: 40m moved to pc2 (saved, restart)" $?
two=
for _ in $(seq 40); do
  sleep 1
  n=$(py "$(curl -s "http://127.0.0.1:$WHTTP/status.json")" 'len(d["bands"])')
  [[ $n == 2 ]] && { two=1; break; }
done
[[ -n $two ]]; check "the worker restarted by itself and decodes both bands" $?
grep -q "restart: the hub assigns other bands" "$T/w/log"; check "… and its log says why" $?

kill -TERM "$WPID"; wait "$WPID"; wrc=$?
kill -TERM "$HPID"; wait "$HPID"; hrc=$?
[[ $wrc == 0 && $hrc == 0 ]]; check "SIGTERM stops both cleanly (worker $wrc, hub $hrc)" $?
HPID= WPID=

if (( fails )); then
  echo "--- hub log"; tail -30 "$T/h/log"; echo "--- worker log"; tail -30 "$T/w/log"
fi
echo
(( fails )) && { echo "FAIL ($fails)"; exit 1; }
echo PASS
