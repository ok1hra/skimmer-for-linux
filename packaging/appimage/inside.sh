#!/usr/bin/env bash
# Runs INSIDE the debian:12 container (build.sh starts it): /in/src.tar.gz
# the tree, /dl the pinned downloads, /out the AppImage, /pkg this
# directory. Not meant to be run by hand.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive APPIMAGE_EXTRACT_AND_RUN=1

apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential meson ninja-build pkg-config file ca-certificates \
  libglib2.0-dev libwebsockets-dev libfftw3-dev libcurl4-openssl-dev libsoup-3.0-dev \
  curl python3 >/dev/null                            # (the last two: the gates)

# build and gates as an ordinary user: root writes where a gate proves a
# write must fail (the MASTER.SCP updater's read-only state file)
useradd -m builder
mkdir -p /build/src && tar -xzf /in/src.tar.gz -C /build/src && chown -R builder /build
cd /build
runuser -u builder -- meson setup b src --buildtype=debugoptimized >/dev/null
runuser -u builder -- meson compile -C b
runuser -u builder -- meson test -C b --print-errorlogs

# AppDir: the program, ONNX Runtime beside it (the binary's RUNPATH finds
# libonnxruntime.so.1 there; SKIM_ORT_LIB still wins — a system runtime
# with CUDA), the licences
A=/build/AppDir
mkdir -p $A/usr/bin $A/usr/lib $A/usr/share/doc/skimmer-for-linux $A/usr/share/doc/onnxruntime
cp b/skimmer-headless $A/usr/bin/
tar -xzf /dl/onnxruntime-linux-x64-1.21.0.tgz -C /build
ORT=/build/onnxruntime-linux-x64-1.21.0
cp $ORT/lib/libonnxruntime.so.1.21.0 $A/usr/lib/
cp $ORT/LICENSE $ORT/ThirdPartyNotices.txt $A/usr/share/doc/onnxruntime/
cp src/LICENSE src/README.md $A/usr/share/doc/skimmer-for-linux/

cp /dl/linuxdeploy-x86_64.AppImage /build/linuxdeploy && chmod +x /build/linuxdeploy
/build/linuxdeploy --appdir $A \
  -e $A/usr/bin/skimmer-headless \
  -l $A/usr/lib/libonnxruntime.so.1.21.0 \
  -d /pkg/skimmer-headless.desktop -i /pkg/skimmer-headless.svg
ln -sf libonnxruntime.so.1.21.0 $A/usr/lib/libonnxruntime.so.1
LINUXDEPLOY_OUTPUT_VERSION=$VERSION /build/linuxdeploy --appdir $A --output appimage
mv Skimmer_for_Linux*-x86_64.AppImage "/out/skimmer-headless-$VERSION-x86_64.AppImage"
