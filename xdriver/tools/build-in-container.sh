#!/bin/sh
# Build the X test clients with the Buildroot toolchain.
# Runs inside the build container: sh /repo/xdriver/tools/build-in-container.sh [br-output-dir]
set -eu
BR=${1:-/work/br-buildroot_cfg_20240705}
CC=$BR/host/bin/m68k-buildroot-linux-gnu-gcc
PKGCONF=$BR/host/bin/pkg-config
OUT=/repo/output/xdriver
mkdir -p "$OUT"
$CC -O2 -Wall -o "$OUT/xdragbench" /repo/xdriver/tools/xdragbench.c $($PKGCONF --cflags --libs x11)
ls -l "$OUT/xdragbench"
