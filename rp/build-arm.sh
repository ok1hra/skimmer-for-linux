#!/bin/sh
# build-arm.sh SRC.c OUT — a static armhf binary for the Red Pitaya (Alpine,
# musl — so static glibc, no shared libs) in a debian:12 podman container.
# The toolchain image is built once and cached as skimmer-rp-cross.
set -eu
src=$(realpath "$1"); out=$(realpath -m "$2")
if ! podman image exists localhost/skimmer-rp-cross; then
  podman build -t skimmer-rp-cross - <<'DOCKER'
FROM docker.io/library/debian:12
RUN apt-get update && apt-get install -y --no-install-recommends gcc-arm-linux-gnueabihf libc6-dev-armhf-cross && rm -rf /var/lib/apt/lists/*
DOCKER
fi
podman run --rm -v "$(dirname "$src")":/src:ro,Z -v "$(dirname "$out")":/out:Z skimmer-rp-cross \
  arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -static -march=armv7-a -mfpu=neon -mfloat-abi=hard \
  -o "/out/$(basename "$out")" "/src/$(basename "$src")" -lm
