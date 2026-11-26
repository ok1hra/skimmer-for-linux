#!/bin/sh
# skimmer-phase-bench on a synthetic pair (skimmer-phase-test --write-pair:
# a wideband interferer 30 dB over the noise, four stations): the report is
# the same byte for byte on a second run (the CPU line aside), the lag it
# measures is 0, and MVDR's floor drop and station gain are what the
# phase gate proves of the combiner (≥ 20 dB, ≥ 15 dB).
# Usage: phase-bench.sh BUILDDIR
set -eu
B=$1
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
"$B/skimmer-phase-test" --write-pair "$T/a.cf32" "$T/b.cf32" 30
"$B/skimmer-phase-bench" "$T/a.cf32" "$T/b.cf32" --csv "$T/1.csv" > "$T/1.txt"
"$B/skimmer-phase-bench" "$T/a.cf32" "$T/b.cf32" --csv "$T/2.csv" > "$T/2.txt"
cat "$T/1.txt"
grep -v '^CPU' "$T/1.txt" > "$T/1n.txt"
grep -v '^CPU' "$T/2.txt" > "$T/2n.txt"
fail=0
cmp -s "$T/1n.txt" "$T/2n.txt" && echo "ok   the report repeats byte for byte" || { echo "FAIL the report differs between runs"; fail=1; }
cmp -s "$T/1.csv" "$T/2.csv" && echo "ok   the CSV repeats byte for byte" || { echo "FAIL the CSV differs"; fail=1; }
grep -q 'B lag 0 samples (measured' "$T/1.txt" && echo "ok   the measured lag is 0" || { echo "FAIL lag"; fail=1; }
# the last mvdr summary line: columns t mode stns G_best G_A drop ...
last=$(grep -E '^ +[0-9]+  mvdr ' "$T/1.txt" | tail -1)
echo "last mvdr line: $last"
echo "$last" | awk '{ g=$4+0; d=$6+0; exit !(g >= 15 && d >= 20) }' \
  && echo "ok   MVDR: station gain ≥ 15 dB, floor drop ≥ 20 dB" \
  || { echo "FAIL MVDR numbers"; fail=1; }
n=$(wc -l < "$T/1.csv")
[ "$n" -gt 1000 ] && echo "ok   the CSV holds $n lines" || { echo "FAIL CSV too short ($n)"; fail=1; }
exit $fail
