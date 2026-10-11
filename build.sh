#!/usr/bin/env bash
# Build the Amiga/Warp kernel and Buildroot rootfs inside a Linux container.
#
# On the host this script builds the docker/ image and re-runs itself inside
# the container. Sources and build trees live in a named Docker volume (Linux,
# case-sensitive, fast); only this repo is bind-mounted, at /repo.
set -euo pipefail

IMAGE=amiga-linux-build
VOLUME=amiga-linux-work
KERNEL_VERSION=6.9
KERNEL_CONFIG=kernel_6.9.0/kernel_6.9_m68k_amiga_cfg
KERNEL_PATCH=kernel_6.9.0/sellen_mods_and_fixes.patch
DEFAULT_BR_CONFIG=buildroot_2024.05/buildroot_cfg_20240705

usage() {
    cat <<EOF
Usage: ./build.sh <command>

  all                 build rootfs, then kernel (default)
  rootfs              build the Buildroot rootfs  -> output/<br config>/rootfs.tar
  kernel              build the kernel            -> output/kernel/
  rootfs-menuconfig   Buildroot menuconfig, saved back to the repo config
  kernel-menuconfig   kernel menuconfig, saved back to $KERNEL_CONFIG
  shell               interactive shell in the build container
  image               (re)build the container image
  clean               delete the work volume (sources, build trees, downloads)

Environment:
  BR_CONFIG   Buildroot config to use (default: $DEFAULT_BR_CONFIG)
  JOBS        parallel jobs for the kernel build (default: all CPUs)

The kernel is built with the m68k toolchain of the selected Buildroot config.
EOF
}

# ---------------------------------------------------------------- host side
if [ -z "${AMIGA_IN_CONTAINER:-}" ]; then
    repo=$(cd "$(dirname "$0")" && pwd)
    cmd=${1:-all}
    case "$cmd" in
        -h|--help|help) usage; exit 0 ;;
        image) exec docker build -t "$IMAGE" "$repo/docker" ;;
        clean) exec docker volume rm "$VOLUME" ;;
    esac
    docker build -q -t "$IMAGE" "$repo/docker" >/dev/null
    tty=()
    if [ -t 0 ] && [ -t 1 ]; then tty=(-it); fi
    exec docker run --rm ${tty[@]+"${tty[@]}"} \
        -v "$VOLUME:/work" -v "$repo:/repo" \
        -e AMIGA_IN_CONTAINER=1 -e BR_CONFIG -e JOBS \
        "$IMAGE" /repo/build.sh "$@"
fi

# ----------------------------------------------------------- container side
REPO=/repo
WORK=/work
OUT=$REPO/output
JOBS=${JOBS:-$(nproc)}
BR_CONFIG=${BR_CONFIG:-$DEFAULT_BR_CONFIG}

[ -f "$REPO/$BR_CONFIG" ] || { echo "No such Buildroot config: $BR_CONFIG" >&2; exit 1; }

# Referenced from the Buildroot configs, e.g. BR2_ROOTFS_OVERLAY.
export AMIGA_LINUX_REPO=$REPO
# Shared download cache for every Buildroot version/config.
export BR2_DL_DIR=$WORK/dl

BR_VERSION=$(sed -n 's/^# Buildroot \(.*\) Configuration$/\1/p' "$REPO/$BR_CONFIG")
BR_SRC=$WORK/buildroot-$BR_VERSION
BR_OUT=$WORK/br-$(basename "$BR_CONFIG")
K_SRC=$WORK/linux-$KERNEL_VERSION
K_OUT=$WORK/linux-build

log() { printf '\n=== %s\n' "$*"; }

# fetch <url> <dir>: download and unpack a release tarball into $WORK
fetch() {
    local url=$1 dir=$2 tarball=$WORK/tarballs/${1##*/}
    [ -d "$dir" ] && return
    mkdir -p "$WORK/tarballs"
    if [ ! -f "$tarball" ]; then
        wget -nv -O "$tarball.part" "$url"
        mv "$tarball.part" "$tarball"
    fi
    tar -xf "$tarball" -C "$WORK"
}

br_make() { make -C "$BR_SRC" O="$BR_OUT" "$@"; }

br_prepare() {
    log "Buildroot $BR_VERSION, config $BR_CONFIG"
    fetch "https://buildroot.org/downloads/buildroot-$BR_VERSION.tar.xz" "$BR_SRC"
    # Extra/updated package patches kept next to the configs
    local pkgdir=$REPO/$(dirname "$BR_CONFIG")/package
    [ -d "$pkgdir" ] && rsync -a --exclude .DS_Store "$pkgdir/" "$BR_SRC/package/"
    mkdir -p "$BR_OUT"
    cp "$REPO/$BR_CONFIG" "$BR_OUT/.config"
    br_make olddefconfig >/dev/null
}

cross_compile() {
    local gcc
    gcc=$(ls "$BR_OUT"/host/bin/m68k-*linux-gnu-gcc 2>/dev/null | head -n1)
    echo "${gcc%gcc}"
}

kernel_make() {
    # Buildroot's GCC is configured --with-cpu=68040. For a multi-CPU kernel
    # (020/030 enabled) the kernel passes no -m680x0 and relies on the
    # compiler default, so restore the generic 68020 baseline; otherwise the
    # 030 MMU instructions (pmove/ptest/pload) are rejected by the assembler.
    local cpu=()
    if grep -qE '^CONFIG_M680[23]0=y' "$K_OUT/.config" 2>/dev/null; then
        cpu=(KCFLAGS=-m68020 KAFLAGS=-m68020)
    fi
    make -C "$K_SRC" O="$K_OUT" ARCH=m68k CROSS_COMPILE="$(cross_compile)" -j"$JOBS" \
        ${cpu[@]+"${cpu[@]}"} "$@"
}

kernel_prepare() {
    br_prepare
    if [ -z "$(cross_compile)" ]; then
        log "Building Buildroot toolchain for the kernel"
        br_make toolchain
    fi
    # Re-extract a pristine tree whenever the patch changes
    local stamp
    stamp=$(sha256sum "$REPO/$KERNEL_PATCH" | cut -d' ' -f1)
    if [ "$(cat "$K_SRC/.amiga-patch" 2>/dev/null)" != "$stamp" ]; then
        log "Preparing linux-$KERNEL_VERSION source tree"
        rm -rf "$K_SRC"
        fetch "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KERNEL_VERSION.tar.xz" "$K_SRC"
        patch -d "$K_SRC" -p1 < "$REPO/$KERNEL_PATCH"
        echo "$stamp" > "$K_SRC/.amiga-patch"
    fi
    # Warp driver sources go in at the same relative paths
    rsync -a --exclude .DS_Store "$REPO/kernel_6.9.0/drivers" "$REPO/kernel_6.9.0/arch" "$K_SRC/"
    mkdir -p "$K_OUT"
    cp "$REPO/$KERNEL_CONFIG" "$K_OUT/.config"
    kernel_make olddefconfig >/dev/null
}

build_rootfs() {
    br_prepare
    log "Building rootfs"
    br_make
    local dest=$OUT/$(basename "$BR_CONFIG")
    mkdir -p "$dest"
    cp "$BR_OUT/images/rootfs.tar" "$dest/"
    log "rootfs: ${dest#$REPO/}/rootfs.tar"
}

build_kernel() {
    kernel_prepare
    log "Building kernel"
    kernel_make vmlinux modules
    local release staging=$WORK/modules-staging
    release=$(kernel_make -s kernelrelease)
    rm -rf "$staging"
    kernel_make INSTALL_MOD_PATH="$staging" INSTALL_MOD_STRIP=1 modules_install >/dev/null
    mkdir -p "$OUT/kernel"
    cp "$K_OUT/vmlinux" "$OUT/kernel/vmlinux"
    gzip -9c "$K_OUT/vmlinux" > "$OUT/kernel/vmlinux.gz"
    tar -czf "$OUT/kernel/modules-$release.tar.gz" -C "$staging" lib
    log "kernel $release: output/kernel/{vmlinux,vmlinux.gz,modules-$release.tar.gz}"
}

case "${1:-all}" in
    all) build_rootfs; build_kernel ;;
    rootfs) build_rootfs ;;
    kernel) build_kernel ;;
    rootfs-menuconfig)
        br_prepare
        br_make menuconfig
        cp "$BR_OUT/.config" "$REPO/$BR_CONFIG"
        log "Saved to $BR_CONFIG" ;;
    kernel-menuconfig)
        kernel_prepare
        kernel_make menuconfig
        cp "$K_OUT/.config" "$REPO/$KERNEL_CONFIG"
        log "Saved to $KERNEL_CONFIG" ;;
    shell) exec bash ;;
    *) usage; exit 1 ;;
esac
