#!/bin/sh
# Build warp_drv.so with the Buildroot toolchain against its Xorg SDK.
# Runs inside the build container: sh /repo/xdriver/build-in-container.sh [br-output-dir]
set -eu
BR=${1:-/work/br-buildroot_cfg_20240705}
CC=$BR/host/bin/m68k-buildroot-linux-gnu-gcc
PKGCONF=$BR/host/bin/pkg-config
OUT=/repo/output/xdriver
mkdir -p "$OUT"
CFLAGS=$($PKGCONF --cflags xorg-server pixman-1)
$CC -O2 -m68040 -fPIC -shared -fvisibility=hidden -Wall -Wno-unused-parameter \
    $CFLAGS -o "$OUT/warp_drv.so" /repo/xdriver/warp_drv.c
ls -l "$OUT/warp_drv.so"
