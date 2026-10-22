#!/usr/bin/env bash
# Golden gate — the CW and RTTY decoders and the RBN feed must not move by
# one byte.
#
#   run.sh <skimmer-replay> <skimmer-tap-replay>            check
#   run.sh <skimmer-replay> <skimmer-tap-replay> --update   new reference
#
# Inputs live OUTSIDE git (2.4 GB) in $SKIM_GOLDEN_DIR (default
# /var/tmp/skimmer-iq/golden): 10-minute 96 kHz IQ slices of real air
# (hd-*.cf32 recordings from skimmer-headless [record]), 1 h decode-tap
# slices of all 7 bands, a frozen MASTER.SCP and the learned feed gate.
# inputs.sha256 pins them; expected.sha256 pins every output. The full
# reference outputs sit beside the inputs (ref/) so a mismatch shows a diff.
#
# Runs with an EMPTY environment: no SKIM_* switch, the code's defaults only.
# Without the inputs the gate SKIPS (77) and says so — never a silent pass.
# A deliberate behaviour change regenerates the reference (--update) in its
# own commit, saying why.
set -euo pipefail

REPLAY=$1 TAPREPLAY=$2 MODE=${3:-check}
HERE=$(cd "$(dirname "$0")" && pwd)
G=${SKIM_GOLDEN_DIR:-/var/tmp/skimmer-iq/golden}

if [[ ! -f $G/scp/master.scp ]]; then
  echo "golden: SKIP — no inputs in $G (see tests/golden/README.md)"
  exit 77
fi
(cd "$G" && sha256sum --quiet -c "$HERE/inputs.sha256") || {
  echo "golden: FAIL — inputs in $G do not match inputs.sha256"; exit 1; }

OUT=$(mktemp -d "${TMPDIR:-/tmp}/skimmer-golden.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/cfg/skimmer-for-linux"
cp "$G/scp/master.scp" "$OUT/cfg/skimmer-for-linux/"
run() { env -i HOME="$OUT" PATH=/usr/bin:/bin XDG_CONFIG_HOME="$OUT/cfg" "$@"; }

# <output dir>:<slice>:<mode>[:pairs] — CW on every slice; RTTY on the
# contest slice (g40m-rtty, JARTS) and on a 20 m evening with an RTTY net
# (text, no spots); CW on the contest slice too: what CW makes of a band
# full of FSK — and, with "pairs", CW kept off the RTTY pairs an RTTY
# pipeline beside it copies ([decode] suppress_cw_on_rtty)
IQ=(g20m-day:g20m-day:cw g20m-eve:g20m-eve:cw g30m-day:g30m-day:cw
    g40m-night:g40m-night:cw g40m-rtty:g40m-rtty:cw
    rtty-g40m-rtty:g40m-rtty:rtty rtty-g20m-eve:g20m-eve:rtty
    pairs-g40m-rtty:g40m-rtty:cw:pairs)
BANDS=(160m 80m 40m 30m 20m 17m 15m 40m-rtty)

# IQ → channelizer → CW v2 / RTTY → extractor → station table → feed
# (stderr feed:)
for job in "${IQ[@]}"; do
  IFS=: read -r d s mode pairs <<< "$job"
  mkdir -p "$OUT/$d"
  ln -s "$G/$s.cf32" "$G/$s.cf32.meta" "$OUT/$d/"
  ( run SKIM_MODE=$mode SKIM_REPLAY_RTTY_PAIRS=${pairs:+1} SKIM_REPLAY_FEED=new \
      "$REPLAY" "$OUT/$d/$s.cf32" \
      > "$OUT/$d/out.raw" 2> "$OUT/$d/feed.txt"
    # paths and wall time are the only run-dependent bytes
    sed -e "s#$OUT/$d/##g" -e 's/ in [0-9.]* s wall ([0-9.]*x)//' \
      "$OUT/$d/out.raw" > "$OUT/$d/out.txt"
    mv "$OUT/$d/$s.cf32.decodes.log" "$OUT/$d/decodes.log" ) &
done
# decode tap → extractor → station table → feed policy + learned gate (shadow;
# an RTTY tap — engine=rtty in its header — replays without it)
for b in "${BANDS[@]}"; do
  mkdir -p "$OUT/tap-$b"
  ( run "$TAPREPLAY" --scp "$G/scp/master.scp" --gate-ini "$G/feed-gate-20261006.ini" \
      --band "${b%-rtty}" --rows "$OUT/tap-$b/rows.jsonl" --tap-out "$OUT/tap-$b/tap-out.log" \
      "$G/tap-$b.log" > "$OUT/tap-$b/out.txt" 2>&1
    cmp -s "$OUT/tap-$b/tap-out.log" "$G/tap-$b.log" \
      && echo faithful > "$OUT/tap-$b/faithful.txt" \
      || echo UNFAITHFUL > "$OUT/tap-$b/faithful.txt"
    rm "$OUT/tap-$b/tap-out.log" ) &
done
wait

files=()
for job in "${IQ[@]}"; do d=${job%%:*}; files+=("$d/out.txt" "$d/feed.txt" "$d/decodes.log"); done
for b in "${BANDS[@]}"; do files+=("tap-$b/out.txt" "tap-$b/rows.jsonl" "tap-$b/faithful.txt"); done
(cd "$OUT" && sha256sum "${files[@]}") > "$OUT/got.sha256"

if [[ $MODE == --update ]]; then
  cp "$OUT/got.sha256" "$HERE/expected.sha256"
  rm -rf "$G/ref" && mkdir -p "$G/ref"
  (cd "$OUT" && cp --parents "${files[@]}" "$G/ref/")
  echo "golden: reference updated (${#files[@]} outputs) — commit expected.sha256 and say why"
  exit 0
fi

if cmp -s "$OUT/got.sha256" "$HERE/expected.sha256"; then
  echo "golden: OK — ${#files[@]} outputs byte-identical"
  exit 0
fi
echo "golden: FAIL — outputs moved:"
diff "$HERE/expected.sha256" "$OUT/got.sha256" | grep '^>' | awk '{print "  " $3}' || true
for f in "${files[@]}"; do
  if [[ -f $G/ref/$f ]] && ! cmp -s "$G/ref/$f" "$OUT/$f"; then
    echo "--- $f (first lines of the diff against $G/ref)"
    diff "$G/ref/$f" "$OUT/$f" | head -20 || true
  fi
done
exit 1
