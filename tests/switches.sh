#!/usr/bin/env bash
# Switches gate — skimmer-headless's table of known SKIM_* switches
# (src/headless/switches.c) must name exactly the switches the engine and the
# product read: a new getenv("SKIM_…") without a table entry would be warned
# about as a typo, a stale entry would hide one.
#
#   switches.sh <skimmer-headless> <source root>
set -euo pipefail

HL=$1 ROOT=$2
known=$("$HL" --list-switches | awk '{print $1}' | sort -u)
read=$(grep -rhoE '"SKIM_[A-Z0-9_]+"' "$ROOT/src/engine" "$ROOT/src/headless" \
         --include='*.c' --exclude=switches.c | tr -d '"' | sort -u)
missing=$(comm -13 <(echo "$known") <(echo "$read"))
stale=$(comm -23 <(echo "$known") <(echo "$read"))
if [[ -n $missing || -n $stale ]]; then
  [[ -n $missing ]] && echo "read but not in the table: $missing"
  [[ -n $stale ]] && echo "in the table but read nowhere: $stale"
  echo "FAIL"; exit 1
fi
echo "PASS — $(echo "$known" | wc -l) switches, table = sources"
