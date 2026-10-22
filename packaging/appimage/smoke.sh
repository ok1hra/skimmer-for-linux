#!/usr/bin/env bash
# Smoke test of a built AppImage — on this machine, or with --container in a
# clean debian:12 (nothing installed beyond the base image: the AppImage
# must bring everything it needs but glibc).
#
#   packaging/appimage/smoke.sh <AppImage> [--container] [--fetch]
#
# --fetch also fetches the DeepCW model from its upstream (network; TLS
# through the bundled libcurl) and has the BUNDLED ONNX Runtime load it.
# Uses only bash and coreutils: no curl, no python in the clean image (with
# --fetch it gets ca-certificates — a TLS trust store every real system has).
set -uo pipefail

AI=$(readlink -f "$1"); shift
CONTAINER= FETCH=
for a in "$@"; do
  case $a in --container) CONTAINER=1 ;; --fetch) FETCH=1 ;; esac
done
if [[ -n $CONTAINER ]]; then
  HERE=$(cd "$(dirname "$0")" && pwd)
  exec "${SKIM_CONTAINER:-podman}" run --rm -v "$AI:/app/$(basename "$AI"):ro" \
    -v "$HERE/smoke.sh:/smoke.sh:ro" docker.io/library/debian:12 bash -c \
    "${FETCH:+apt-get update -qq && apt-get install -y -qq ca-certificates >/dev/null && }
     bash /smoke.sh '/app/$(basename "$AI")' ${FETCH:+--fetch}"
fi

export APPIMAGE_EXTRACT_AND_RUN=1             # no FUSE needed (containers, CI)
T=$(mktemp -d "${TMPDIR:-/tmp}/skimmer-smoke.XXXXXX")
PID=
cleanup() { [[ -n $PID ]] && kill "$PID" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
# TMPDIR too: the runtime keeps its extraction (appimage_extracted_<md5>) there
export HOME=$T XDG_CONFIG_HOME=$T/cfg XDG_DATA_HOME=$T/data XDG_STATE_HOME=$T/state TMPDIR=$T
export SKIM_SCP_URL=http://127.0.0.1:9/
unset SKIM_ORT_LIB SKIM_DEEPCW_MODEL
fails=0
check() { if [[ $2 == 0 ]]; then echo "  ok   $1"; else echo "  FAIL $1"; fails=$((fails + 1)); fi; }
get() {  # port path → body (HTTP/1.0 over bash's /dev/tcp)
  exec 3<>"/dev/tcp/127.0.0.1/$1" || return 1
  printf 'GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n' "$2" >&3
  sed '1,/^\r$/d' <&3
  exec 3>&-
}
echo "smoke: $(basename "$AI") on $(. /etc/os-release && echo "$PRETTY_NAME"), glibc $(ldd --version | head -1 | grep -o '[0-9.]*$')"

out=$("$AI" --help 2>&1); rc=$?
[[ $rc == 0 && $out == *--install-service* && $out == *--fetch-deepcw-model* ]]; check "--help" $?

(cd "$T" && "$AI" --appimage-extract >/dev/null 2>&1)
R=$T/squashfs-root
miss=$(ldd "$R/usr/bin/skimmer-headless" "$R/usr/lib/libonnxruntime.so.1" 2>&1 | grep "not found")
[[ -x $R/usr/bin/skimmer-headless && -z $miss ]]; check "every library found (ldd) ${miss:+— $miss}" $?
[[ -f $R/usr/share/doc/onnxruntime/LICENSE && -f $R/usr/share/doc/skimmer-for-linux/LICENSE ]]
check "licences inside" $?

# the whole skimmer on the mock source: streaming, the web page answers
HTTP=$((30000 + RANDOM % 5000))
mkdir -p "$T/cfg/skimmer-for-linux"
printf '[radio]\ntype=mock\n[bands]\n20m=14045500\n40m=7045500 cw,rtty\n[feed]\nport=0\n[status]\nconsole_s=0\nhttp_port=%s\n' \
  "$HTTP" > "$T/h.ini"
# from the extracted tree (what the runtime runs): without FUSE the runtime
# keeps the program as its child and does not pass SIGTERM on, so a signal
# to it would orphan the skimmer — systemd signals the whole unit, a shell
# the whole foreground group, this test the skimmer itself
"$R/AppRun" --config "$T/h.ini" > "$T/log" 2>&1 &
PID=$!
st=
for _ in $(seq 60); do
  st=$(get "$HTTP" /status.json 2>/dev/null) && [[ $st == *'"streaming":true'* ]] && break
  sleep 0.5
done
[[ $st == *'"streaming":true'* && $st == *'"engine":"cw-v2"'* && $st == *'"engine":"rtty"'* ]]
check "mock source: streaming, CW and RTTY, /status.json" $?
page=$(get "$HTTP" / 2>/dev/null)
[[ $page == *"<title>Skimmer for Linux</title>"* ]]; check "the web page (built in)" $?
kill -TERM "$PID"; rc=0; wait "$PID" || rc=$?; PID=
[[ $rc == 0 ]] && grep -q "stopped$" "$T/log"; check "SIGTERM: a clean stop (exit 0)" $?

# the service unit names the AppImage itself
mkdir -p "$T/bin"; printf '#!/bin/sh\nexit 0\n' > "$T/bin/systemctl"; chmod +x "$T/bin/systemctl"
PATH=$T/bin:$PATH "$AI" --install-service > /dev/null 2>&1
grep -qx "ExecStart=\"$AI\"" "$T/cfg/systemd/user/skimmer-headless.service"
check "--install-service: ExecStart is the AppImage" $?

printf '[bands]\n20m=14045500 cw:deepcw\n' > "$T/d.ini"
if [[ -n $FETCH ]]; then
  "$AI" --fetch-deepcw-model > "$T/f.log" 2>&1; check "--fetch-deepcw-model from upstream (TLS)" $?
  out=$("$AI" --check-config --config "$T/d.ini" 2>/dev/null)
  [[ $out == *"deepcw "*"ONNX Runtime 1.21.0 via libonnxruntime.so.1, CPU"* ]]
  check "DeepCW: the bundled ONNX Runtime loads the model" $?
else
  out=$("$AI" --check-config --config "$T/d.ini" 2>/dev/null)
  [[ $out == *"deepcw NOT AVAILABLE, those bands run v2: no model"* ]]
  check "DeepCW without the model: v2, says why (--fetch: the runtime too)" $?
fi

if ((fails)); then echo "--- log"; cat "$T/log"; echo "FAIL ($fails)"; exit 1; fi
echo "PASS"
