#!/usr/bin/env bash
# capture-spots.sh — record two skimmer telnet feeds side by side for a later
# comparison: the local Skimmer for Linux (skimmer-headless) and the remote
# CW Skimmer Server — plus the Reverse Beacon Network feed as the referee
# that tells a real callsign from a busted one. Every received line is
# written with its UTC receive time, one file per feed, all files named
# after the same start time.
#
#   capture-spots.sh [-c CALL] [-o DIR] [-t SECONDS] [-l HOST:PORT]
#                    [-r HOST:PORT] [-b HOST:PORT] [-w URL] [-k SECONDS]
#                    [-S SECONDS] [-n]
#
#   -c CALL       login callsign for all feeds           (default OK1HRA)
#   -o DIR        output directory          (default <script dir>/logs)
#   -t SECONDS    stop after this long             (default: until Ctrl+C)
#   -l HOST:PORT  local feed                        (default 127.0.0.1:7300)
#   -r HOST:PORT  remote feed                   (default 192.168.1.201:7301)
#   -b HOST:PORT  RBN feed, "" = off      (default telnet.reversebeacon.net:7000)
#   -w URL        local skimmer status JSON, "" = off
#                                   (default http://127.0.0.1:8073/status.json)
#   -k SECONDS    keepalive: an empty line to each feed this often (300)
#   -S SECONDS    status JSON snapshot this often (300)
#   -n            do not hold a sleep inhibitor (by default the machine is
#                 kept from suspending while recording)
#
# Output in DIR, <T> = UTC start time:
#   local-<T>.log    remote-<T>.log   rbn-<T>.log   every received line:
#       2026-09-29T21:30:05Z<TAB>DX de OK1HRA-#:  7027.0  G5ACF  CW ...
#     lines starting with "#" are connection events
#   events-<T>.log   what the recorder did: connects, drops, a heartbeat
#                    every minute with per-feed counts and the WRITE CHECK
#                    (lines in the file vs. lines the recorder wrote),
#                    write errors, suspend/freeze detection, disk space
#   status-<T>.jsonl local skimmer status (/status.json) every -S seconds
#
# A dropped feed reconnects every 10 s; the others keep recording.

set -u

CALL=OK1HRA
OUT="$(cd "$(dirname "$0")" && pwd)/logs"
DURATION=0
LOCAL=127.0.0.1:7300
REMOTE=192.168.1.201:7301
RBN=telnet.reversebeacon.net:7000
STATUS_URL=http://127.0.0.1:8073/status.json
KEEPALIVE=300
SNAP_EVERY=300
INHIBIT=1
TICK=${CAPTURE_TICK:-60}          # heartbeat period (tests shorten it)
READ_T=${CAPTURE_READ_T:-10}      # socket read wake-up (keepalive, state)
IDLE_NOTE=1800                    # heartbeat mentions a feed silent this long

while getopts "c:o:t:l:r:b:w:k:S:nh" opt; do
  case $opt in
    c) CALL=$OPTARG ;;
    o) OUT=$OPTARG ;;
    t) DURATION=$OPTARG ;;
    l) LOCAL=$OPTARG ;;
    r) REMOTE=$OPTARG ;;
    b) RBN=$OPTARG ;;
    w) STATUS_URL=$OPTARG ;;
    k) KEEPALIVE=$OPTARG ;;
    S) SNAP_EVERY=$OPTARG ;;
    n) INHIBIT=0 ;;
    *) sed -n '2,37p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
  esac
done

export TZ=UTC
mkdir -p "$OUT" || { echo "cannot create $OUT" >&2; exit 1; }
[[ -w $OUT ]] || { echo "$OUT is not writable" >&2; exit 1; }
START=$(date -u +%Y%m%d-%H%M%S)
LOCAL_FILE="$OUT/local-$START.log"
REMOTE_FILE="$OUT/remote-$START.log"
RBN_FILE="$OUT/rbn-$START.log"
EVENTS="$OUT/events-$START.log"
SNAPS="$OUT/status-$START.jsonl"
STATE="$OUT/.state-$START"            # recorder counters, one file per feed
mkdir -p "$STATE" || exit 1

ts() { printf '%(%Y-%m-%dT%H:%M:%S)TZ' -1; }

# The events log also goes to the terminal. If even it cannot be written,
# the terminal still gets the line.
note() {
  local line
  line="$(ts) $*"
  printf '%s\n' "$line"
  printf '%s\n' "$line" 2>/dev/null >>"$EVENTS" ||
    printf '%s WRITE ERROR: events log %s not writable\n' "$(ts)" "$EVENTS" >&2
}

# ---- one feed: connect, log in, record every line, reconnect on drop -----------

capture() {           # name host port file
  local name=$1 host=$2 port=$3 file=$4
  local rx=0 wr=0 spots=0 werr=0 werr_noted=0 last_rx=0 connected=0
  local line part rc now last_ka last_state=$EPOCHSECONDS

  trap '' PIPE        # a write to a dead socket must fail, not kill us
  trap 'exit 0' TERM

  put() {             # line → data file; count; report failures (throttled)
    # the timestamp comes from printf itself: no fork per line (RBN is busy)
    if printf '%(%Y-%m-%dT%H:%M:%S)TZ\t%s\n' -1 "$1" 2>/dev/null >>"$file"; then
      ((wr++))
    else
      ((werr++))
      if ((EPOCHSECONDS - werr_noted >= 60)); then
        note "$name: WRITE ERROR — $file not writable ($werr lines lost so far)"
        werr_noted=$EPOCHSECONDS
      fi
    fi
  }
  mark() {            # connection event → data file (not counted as a line)
    printf '#%s\t%s\n' "$(ts)" "$1" 2>/dev/null >>"$file"
    note "$name: $1"
  }
  state() {           # builtin only — runs after every line
    printf '%d %d %d %d %d %d\n' "$rx" "$wr" "$spots" "$werr" "$last_rx" \
      "$connected" 2>/dev/null >"$STATE/$name"
  }
  got() {             # one complete received line
    local l=${1//$'\r'/}
    [[ -z $l ]] && return
    ((rx++))
    last_rx=$EPOCHSECONDS
    [[ $l == "DX de "* ]] && ((spots++))
    put "$l"
    state
  }

  while :; do
    if exec 3<>"/dev/tcp/$host/$port" 2>/dev/null; then
      connected=1
      part=""
      last_ka=$EPOCHSECONDS
      mark "connected to $host:$port"
      # Both servers prompt for a callsign without a newline; answer at once.
      printf '%s\r\n' "$CALL" >&3 2>/dev/null
      state
      while :; do
        if IFS= read -r -t "$READ_T" line <&3; then
          got "$part$line"
          part=""
        else
          rc=$?
          if ((rc > 128)); then
            part+=$line          # timeout: keep the partial line, go on
          else
            [[ -n $part$line ]] && got "$part$line"   # EOF: flush the tail
            break
          fi
        fi
        now=$EPOCHSECONDS
        if ((KEEPALIVE > 0 && now - last_ka >= KEEPALIVE)); then
          last_ka=$now
          if ! printf '\r\n' >&3 2>/dev/null; then
            mark "keepalive write failed — connection is dead"
            break
          fi
        fi
        if ((now - last_state >= READ_T)); then state; last_state=$now; fi
      done
      exec 3<&- 3>&- 2>/dev/null
      connected=0
      mark "disconnected — retry in 10 s"
    else
      connected=0
      mark "cannot connect to $host:$port — retry in 10 s"
    fi
    state
    sleep 10
  done
}

# ---- supervisor -----------------------------------------------------------------

PIDS=()
INHIBIT_PID=
declare -A PREV_SPOTS=()

data_lines() { # lines in a data file that the recorder counts (not "#" events)
  [[ -f $1 ]] || { echo -1; return; }
  grep -vc '^#' "$1"
}

# One heartbeat field for a feed → $FIELD; clears WRITE_OK on a failed check:
# every line the recorder wrote must still be in the file (fewer = the file
# was truncated, replaced or deleted), and no append may have failed.
check_feed() {        # name file
  local name=$1 file=$2 rx wr spots werr last_rx connected in_file age st
  if ! read -r rx wr spots werr last_rx connected <"$STATE/$name" 2>/dev/null ||
     [[ -z ${connected:-} ]]; then
    FIELD="$name: starting"
    return
  fi
  in_file=$(data_lines "$file")
  if ((in_file < wr)); then
    note "$name: WRITE CHECK FAILED — $file holds $in_file lines, the recorder wrote $wr"
    WRITE_OK=0
  fi
  if ((werr > 0)); then
    note "$name: WRITE CHECK FAILED — $werr received lines could not be written"
    WRITE_OK=0
  fi
  st=$([[ $connected == 1 ]] && echo up || echo DOWN)
  if ((last_rx > 0)); then
    age=$((EPOCHSECONDS - last_rx))
    ((age >= IDLE_NOTE)) && st+=", silent $((age / 60)) min"
  else
    st+=", nothing received yet"
  fi
  FIELD=$(printf '%s %s: %d spots (+%d), %d lines' "$name" "$st" "$spots" \
    "$((spots - ${PREV_SPOTS[$name]:-0}))" "$in_file")
  PREV_SPOTS[$name]=$spots
}

snapshot() {
  [[ -z $STATUS_URL ]] && return
  local j
  if j=$(curl -s --max-time 5 "$STATUS_URL" 2>/dev/null) && [[ $j == \{* ]]; then
    printf '%s\t%s\n' "$(ts)" "$j" 2>/dev/null >>"$SNAPS" ||
      note "WRITE ERROR — $SNAPS not writable"
    SNAP_FAILED=0
  elif ((!SNAP_FAILED)); then
    note "local skimmer status $STATUS_URL unavailable (is skimmer-headless running?)"
    SNAP_FAILED=1
  fi
}

summary() {
  local f n b
  for f in "$LOCAL_FILE" "$REMOTE_FILE" ${RBN:+"$RBN_FILE"}; do
    n=$(grep -c $'\tDX de ' "$f" 2>/dev/null)
    b=$(basename "$f")
    note "$(printf '%-7s %6d spots  %s' "${b%%-*}" "${n:-0}" "$f")"
  done
  note "events: $EVENTS"
  [[ -n $STATUS_URL ]] && note "status snapshots: $SNAPS"
}

stop() {
  trap - INT TERM
  kill "${PIDS[@]}" 2>/dev/null
  wait "${PIDS[@]}" 2>/dev/null
  [[ -n $INHIBIT_PID ]] && kill "$INHIBIT_PID" 2>/dev/null
  WRITE_OK=1
  check_feed local "$LOCAL_FILE"
  check_feed remote "$REMOTE_FILE"
  [[ -n $RBN ]] && check_feed rbn "$RBN_FILE"
  note "stopped — write check $([[ $WRITE_OK == 1 ]] && echo OK || echo FAILED)"
  summary
  rm -rf "$STATE"
  exit 0
}
trap stop INT TERM HUP

note "recording as $CALL since $START UTC$( ((DURATION > 0)) && echo " for $DURATION s")"
note "  local  $LOCAL  → $LOCAL_FILE"
note "  remote $REMOTE → $REMOTE_FILE"
note "  rbn    ${RBN:-off}$([[ -n $RBN ]] && echo " → $RBN_FILE")"
note "  events → $EVENTS"

if ((INHIBIT)) && command -v systemd-inhibit >/dev/null; then
  systemd-inhibit --what=sleep:idle --who=capture-spots \
    --why="recording skimmer telnet feeds" --mode=block sleep infinity &
  INHIBIT_PID=$!
  sleep 0.5
  if kill -0 "$INHIBIT_PID" 2>/dev/null; then
    note "sleep inhibitor held — the machine will not suspend while recording"
  else
    note "WARNING: could not hold a sleep inhibitor — a suspend will stop the recording"
    INHIBIT_PID=
  fi
else
  note "no sleep inhibitor (-n) — a suspend will stop the recording"
fi

capture local "${LOCAL%:*}" "${LOCAL##*:}" "$LOCAL_FILE" & PIDS+=($!)
capture remote "${REMOTE%:*}" "${REMOTE##*:}" "$REMOTE_FILE" & PIDS+=($!)
[[ -n $RBN ]] && { capture rbn "${RBN%:*}" "${RBN##*:}" "$RBN_FILE" & PIDS+=($!); }

SNAP_FAILED=0
T0=$EPOCHSECONDS
last_tick=$EPOCHSECONDS
last_snap=0
while :; do
  step=$TICK
  if ((DURATION > 0)); then
    left=$((DURATION - (EPOCHSECONDS - T0)))
    ((left <= 0)) && stop
    ((left < step)) && step=$left
  fi
  sleep "$step" & wait $!
  now=$EPOCHSECONDS
  # sleep counts only running time: a tick that took far longer in wall-clock
  # time means the machine was suspended (or this process was stopped).
  gap=$((now - last_tick - step))
  if ((gap > 30)); then
    note "WARNING: $((gap / 60)) min $((gap % 60)) s missing — the machine was suspended or this recorder was frozen; nothing was recorded in that time"
  fi
  last_tick=$now
  for p in "${PIDS[@]}"; do
    kill -0 "$p" 2>/dev/null || note "ERROR: a recorder process ($p) died"
  done
  WRITE_OK=1
  check_feed local "$LOCAL_FILE"; line=$FIELD
  check_feed remote "$REMOTE_FILE"; line+=" · $FIELD"
  if [[ -n $RBN ]]; then check_feed rbn "$RBN_FILE"; line+=" · $FIELD"; fi
  free_mb=$(df -Pm "$OUT" 2>/dev/null | awk 'NR==2 {print $4}')
  ((${free_mb:-0} < 100)) && note "WARNING: only ${free_mb:-?} MB free in $OUT"
  note "$line · write $([[ $WRITE_OK == 1 ]] && echo OK || echo FAILED) · ${free_mb:-?} MB free"
  if ((SNAP_EVERY > 0 && now - last_snap >= SNAP_EVERY)); then
    snapshot
    last_snap=$now
  fi
  if ((DURATION > 0 && EPOCHSECONDS - T0 >= DURATION)); then stop; fi
done
