#!/usr/bin/env bash
# skimmer-headless AppImage — built in a Debian 12 container (glibc 2.36:
# the oldest base it runs on), x86_64.
#
#   packaging/appimage/build.sh [OUTDIR]          (default dist/)
#
# Needs podman (or SKIM_CONTAINER=docker) and the network: the debian:12
# image, its packages, and two pinned downloads (sha256-checked, cached in
# ~/.cache/skimmer-appimage): linuxdeploy, and ONNX Runtime 1.21.0 (CPU) —
# the DeepCW runtime, bundled; the DeepCW MODEL is never bundled (AGPL,
# --fetch-deepcw-model). The tracked files of this tree go in as they are
# on disk (uncommitted edits included, untracked files not).
#
# In the container: a build with the same flags as the gates
# (debugoptimized), ALL offline gates — the golden one too when its inputs
# are on this machine ($SKIM_GOLDEN_DIR, default /var/tmp/skimmer-iq/golden):
# the decoder built on Debian 12 must copy real air byte for byte as the
# development build does — then the AppImage. smoke.sh checks the result.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$(mkdir -p "${1:-$ROOT/dist}" && cd "${1:-$ROOT/dist}" && pwd)
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/skimmer-appimage
GOLDEN=${SKIM_GOLDEN_DIR:-/var/tmp/skimmer-iq/golden}
CT=${SKIM_CONTAINER:-podman}
IMAGE=docker.io/library/debian:12

LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
LINUXDEPLOY_SHA=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
ORT_URL=https://github.com/microsoft/onnxruntime/releases/download/v1.21.0/onnxruntime-linux-x64-1.21.0.tgz
ORT_SHA=7485c7e7aac6501b27e353dcbe068e45c61ab51fbaf598d13970dfae669d20bf

fetch() {  # url sha256 → $CACHE/<name>, checked
  local f=$CACHE/${1##*/}
  if [[ ! -f $f ]] || ! echo "$2  $f" | sha256sum --quiet -c - 2>/dev/null; then
    echo "appimage: fetching ${1##*/}"
    curl -sSfL -o "$f.part" "$1"
    echo "$2  $f.part" | sha256sum --quiet -c - || { echo "appimage: ${1##*/}: sha256 mismatch"; exit 1; }
    mv "$f.part" "$f"
  fi
}
mkdir -p "$CACHE"
fetch "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA"
fetch "$ORT_URL" "$ORT_SHA"

# the tree as tracked, as it is on disk
SRC=$(mktemp -d "${TMPDIR:-/tmp}/skimmer-appimage.XXXXXX")
trap 'rm -rf "$SRC"' EXIT
(cd "$ROOT" && git ls-files -z | tar --null -T - -czf "$SRC/src.tar.gz")
VERSION=$(sed -n "s/^  version : '\(.*\)',$/\1/p" "$ROOT/meson.build")
if ! (cd "$ROOT" && git describe --exact-match --tags HEAD >/dev/null 2>&1); then
  VERSION+="-g$(cd "$ROOT" && git rev-parse --short=7 HEAD)"
  (cd "$ROOT" && git diff --quiet HEAD) || VERSION+="-dirty"
fi

vols=(-v "$SRC:/in:ro" -v "$CACHE:/dl:ro" -v "$OUT:/out" -v "$HERE:/pkg:ro")
[[ -f $GOLDEN/scp/master.scp ]] && vols+=(-v "$GOLDEN:/var/tmp/skimmer-iq/golden:ro")
echo "appimage: skimmer-headless $VERSION in $IMAGE"
"$CT" run --rm "${vols[@]}" -e VERSION="$VERSION" "$IMAGE" bash /pkg/inside.sh
echo "appimage: $OUT/skimmer-headless-$VERSION-x86_64.AppImage"
