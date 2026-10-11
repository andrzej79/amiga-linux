#!/usr/bin/env bash
# Build a complete CF card image for the Warp: Amiga RDB + partitions.
# Runs inside the build container (./build.sh cfimage); see cfcard/make-cf.sh.
#
# Layout (Linux names assume the card is the first disk on the Warp IDE):
#   DH0    FFS DOS\3, bootable, "System": empty, or the
#          AmigaOS partition saved by save-dh0.sh       -> sda1
#   LBOOT  FFS DOS\3, amiboot + vmlinux + boot scripts -> sda2
#   SWAP   SWP\0 (0x53575000), Linux swap              -> sda3
#   LINUX  LNX\0 (0x4C4E5800), ext4 root, rest of card -> sda4
#
# Environment:
#   CF_BLOCKS   card size in 512-byte sectors (required)
#   DH0_MB      default 1024
#   LBOOT_MB    default 64
#   SWAP_MB     default 512
#   ROOT_DISK   Linux disk name in the boot scripts, default sda
#   VIDEO       amiwarpfb mode for the boot script, default 1280x1024@60,depth:16
#   DH0_IMAGE   saved DH0 (cfcard/save-dh0.sh), default output/cfcard/dh0.hdf.gz
#               if it exists; "none" = empty FFS volume
set -euo pipefail

REPO=/repo
OUT=$REPO/output/cfcard
W=/work/cfcard
HEADS=16
SECS=63
CYL_BLOCKS=$((HEADS * SECS))
RDB_CYLS=2

: "${CF_BLOCKS:?CF_BLOCKS (card size in 512-byte sectors) is required}"
DH0_MB=${DH0_MB:-1024}
LBOOT_MB=${LBOOT_MB:-64}
SWAP_MB=${SWAP_MB:-512}
ROOT_DISK=${ROOT_DISK:-sda}
# 1280x1024@60 (108 MHz): no pixel-clock harmonic in the 2.4 GHz WiFi band
VIDEO=${VIDEO:-1280x1024@60,depth:16}
DH0_IMAGE=${DH0_IMAGE:-$REPO/output/cfcard/dh0.hdf.gz}
case "$DH0_IMAGE" in /*|none) ;; *) DH0_IMAGE=$REPO/$DH0_IMAGE ;; esac
BR_CONFIG=${BR_CONFIG:-buildroot_2024.05/buildroot_cfg_20240705}

ROOTFS_TAR=$REPO/output/$(basename "$BR_CONFIG")/rootfs.tar
VMLINUX=$REPO/output/kernel/vmlinux
MODULES_TGZ=$(ls "$REPO"/output/kernel/modules-*.tar.gz 2>/dev/null | head -n1 || true)
AMIBOOT=$REPO/amiboot-5.6

log() { printf '\n=== %s\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

for f in "$ROOTFS_TAR" "$VMLINUX" "$AMIBOOT"; do
    [ -f "$f" ] || die "missing $f (run ./build.sh all first)"
done

# ---------------------------------------------------------------- geometry
CYLS=$((CF_BLOCKS / CYL_BLOCKS))
cyls_for_mb() { echo $(( ($1 * 1024 * 1024 + CYL_BLOCKS * 512 - 1) / (CYL_BLOCKS * 512) )); }

DH0_LO=$RDB_CYLS;              DH0_HI=$((DH0_LO + $(cyls_for_mb "$DH0_MB") - 1))
LBOOT_LO=$((DH0_HI + 1));      LBOOT_HI=$((LBOOT_LO + $(cyls_for_mb "$LBOOT_MB") - 1))
SWAP_LO=$((LBOOT_HI + 1));     SWAP_HI=$((SWAP_LO + $(cyls_for_mb "$SWAP_MB") - 1))
ROOT_LO=$((SWAP_HI + 1));      ROOT_HI=$((CYLS - 1))
[ "$ROOT_HI" -gt $((ROOT_LO + 100)) ] || die "card too small for this layout ($CYLS cylinders)"

part_bytes() { echo $(( ($2 - $1 + 1) * CYL_BLOCKS * 512 )); }
DH0_BYTES=$(part_bytes $DH0_LO $DH0_HI)
LBOOT_BYTES=$(part_bytes $LBOOT_LO $LBOOT_HI)
SWAP_BYTES=$(part_bytes $SWAP_LO $SWAP_HI)
ROOT_BYTES=$(part_bytes $ROOT_LO $ROOT_HI)

log "Card: $CF_BLOCKS sectors -> CHS $CYLS/$HEADS/$SECS ($(( CYLS * CYL_BLOCKS )) sectors used)"
printf '  %-6s cyl %5d-%5d  %6d MiB  %s\n' \
    RDB 0 $((RDB_CYLS - 1)) 0 "Rigid Disk Block" \
    DH0 $DH0_LO $DH0_HI $((DH0_BYTES >> 20)) "FFS DOS\\3 bootable  ${ROOT_DISK}1" \
    LBOOT $LBOOT_LO $LBOOT_HI $((LBOOT_BYTES >> 20)) "FFS DOS\\3           ${ROOT_DISK}2" \
    SWAP $SWAP_LO $SWAP_HI $((SWAP_BYTES >> 20)) "SWP\\0 Linux swap     ${ROOT_DISK}3" \
    LINUX $ROOT_LO $ROOT_HI $((ROOT_BYTES >> 20)) "LNX\\0 ext4 root      ${ROOT_DISK}4"

rm -rf "$W"
mkdir -p "$W" "$OUT"

# ---------------------------------------------------------------- DH0
# A saved AmigaOS partition (cfcard/save-dh0.sh) goes back as it was; it must
# have exactly the size of this layout's DH0.  DH0_IMAGE=none: empty volume.
if [ "$DH0_IMAGE" != none ] && [ -f "$DH0_IMAGE" ]; then
    log "DH0: saved partition ${DH0_IMAGE#$REPO/}"
    gzip -dc "$DH0_IMAGE" > "$W/dh0.hdf"
    got=$(stat -c %s "$W/dh0.hdf")
    [ "$got" -eq "$DH0_BYTES" ] ||
        die "saved DH0 is $got bytes, this layout's DH0 is $DH0_BYTES (set DH0_MB to match, or DH0_IMAGE=none)"
    xdftool "$W/dh0.hdf" open chs=$((DH0_HI - DH0_LO + 1)),$HEADS,$SECS + info | head -3
else
    log "DH0: empty FFS volume 'System'"
    xdftool -f "$W/dh0.hdf" create chs=$((DH0_HI - DH0_LO + 1)),$HEADS,$SECS + format System ffs+intl
fi

# ---------------------------------------------------------------- LBOOT
log "LBOOT: amiboot, kernel, boot scripts"
# The scripts take the Linux disk name as an argument: with a drive on the
# A1200's own IDE port (pata_gayle probes first) the Warp card becomes sdb.
OPTS="rootfstype=ext4 rw video=amiwarpfb:${VIDEO}"
cat > "$W/Linux" <<EOF
.KEY DISK/K
.BRA {
.KET }
.DEF DISK ${ROOT_DISK}
; Boot Linux from this card (Warp, firmware 3.000+)
;   Execute LBOOT:Linux            root on ${ROOT_DISK}4
;   Execute LBOOT:Linux DISK=sdb   when a drive is on the A1200 IDE port too
Stack 65536
LBOOT:amiboot-5.6 -k LBOOT:vmlinux "root=/dev/{DISK}4 $OPTS"
EOF
cat > "$W/Linux-debug" <<EOF
.KEY DISK/K
.BRA {
.KET }
.DEF DISK ${ROOT_DISK}
; Linux with all Warp driver debug output and without INT2 (polling),
; for bring-up and when the normal boot hangs.  DISK=sdb as for LBOOT:Linux
Stack 65536
LBOOT:amiboot-5.6 -k LBOOT:vmlinux "root=/dev/{DISK}4 $OPTS loglevel=8 cswarp_core.dbg=0x1ff cswarp_core.use_irq=0"
EOF
cat > "$W/ReadMe" <<EOF
Warp Linux boot partition
-------------------------
  Execute LBOOT:Linux              normal boot, root /dev/${ROOT_DISK}4
  Execute LBOOT:Linux-debug        verbose Warp driver messages, no INT2 (polling)
  ... DISK=sdb                     if a drive sits on the A1200 IDE port too
                                   (that one is probed first and becomes sda)

Kernel: LBOOT:vmlinux  (6.9, built $(date -u +%Y-%m-%d))
Partitions: 1 DH0, 2 LBOOT, 3 swap (label warp-swap), 4 root ext4
Under Linux this partition can be mounted as affs, e.g. to replace the kernel:
  mount -t affs /dev/${ROOT_DISK}2 /mnt
Kernel options: see kernel_6.9.0/WARP_DRIVERS.md in the amiga-linux repo.
EOF
xdftool -f "$W/lboot.hdf" create chs=$((LBOOT_HI - LBOOT_LO + 1)),$HEADS,$SECS + format LBOOT ffs+intl \
    + write "$AMIBOOT" amiboot-5.6 + write "$VMLINUX" vmlinux \
    + write "$W/Linux" Linux + write "$W/Linux-debug" Linux-debug + write "$W/ReadMe" ReadMe \
    + protect Linux srwed + protect Linux-debug srwed + protect amiboot-5.6 rwed
xdftool "$W/lboot.hdf" list

# ---------------------------------------------------------------- swap
# Big-endian swap header (the m68k's order), 4 KiB pages, version 1
log "SWAP: Linux swap area"
python3 - "$W/swap.img" "$SWAP_BYTES" <<'EOF'
import struct, sys, uuid
path, size = sys.argv[1], int(sys.argv[2])
page = 4096
hdr = bytearray(page)
last_page = size // page - 1
struct.pack_into(">III", hdr, 1024, 1, last_page, 0)   # version, last_page, nr_badpages
hdr[1036:1052] = uuid.uuid4().bytes
hdr[1052:1068] = b"warp-swap".ljust(16, b"\0")
hdr[page - 10:page] = b"SWAPSPACE2"
with open(path, "wb") as f:
    f.write(hdr)
    f.truncate(size)
EOF

# ---------------------------------------------------------------- root
# fakeroot keeps root ownership and device nodes from the tarball for mke2fs -d
log "LINUX: ext4 root filesystem from $(basename "$(dirname "$ROOTFS_TAR")")/rootfs.tar"
fakeroot -- bash -euo pipefail -c '
    W=$1; ROOTFS_TAR=$2; MODULES_TGZ=$3; ROOT_DISK=$4; ROOT_BYTES=$5
    mkdir "$W/tree"
    tar -xpf "$ROOTFS_TAR" -C "$W/tree"
    if [ -n "$MODULES_TGZ" ]; then
        # lib -> usr/lib in the merged-usr rootfs
        tar -xzf "$MODULES_TGZ" -C "$W/tree" --keep-directory-symlink
        echo "  modules: $(ls "$W/tree/lib/modules")"
    fi
    # Buildroot links /etc/dropbear to /var/run (RAM): the 68k would then
    # generate new host keys - slowly - on every boot.  Ship persistent ones.
    if [ -L "$W/tree/etc/dropbear" ]; then
        rm "$W/tree/etc/dropbear"
        mkdir -m 700 "$W/tree/etc/dropbear"
        for t in ed25519 ecdsa rsa; do
            dropbearkey -t $t -f "$W/tree/etc/dropbear/dropbear_${t}_host_key" 2>/dev/null |
                grep -i "fingerprint" | sed "s/^/  $t host key /"
        done
    fi
    # by label: the card is sda or sdb depending on the A1200 IDE port
    grep -q "warp-swap" "$W/tree/etc/fstab" ||
        printf "LABEL=warp-swap\tnone\tswap\tsw,nofail,x-systemd.device-timeout=10s\t0\t0\n" >> "$W/tree/etc/fstab"
    echo "  /etc/fstab:"; sed "s/^/    /" "$W/tree/etc/fstab"
    mke2fs -q -F -t ext4 -L warp-root -d "$W/tree" "$W/root.img" "$((ROOT_BYTES / 1024))k"
' _ "$W" "$ROOTFS_TAR" "$MODULES_TGZ" "$ROOT_DISK" "$ROOT_BYTES"
e2fsck -fn "$W/root.img" | tail -n 1
rm -rf "$W/tree"

# ---------------------------------------------------------------- RDB disk
log "RDB disk image"
rdbtool -f "$W/disk.img" create chs=$CYLS,$HEADS,$SECS + init rdb_cyls=$RDB_CYLS \
    + add start=$DH0_LO end=$DH0_HI name=DH0 dostype=DOS3 bootable=1 pri=0 \
          max_transfer=0x1fe00 mask=0x7ffffffe \
    + add start=$LBOOT_LO end=$LBOOT_HI name=LBOOT dostype=DOS3 bootable=0 \
          max_transfer=0x1fe00 mask=0x7ffffffe \
    + add start=$SWAP_LO end=$SWAP_HI name=SWAP dostype=0x53575000 automount=0 \
    + add start=$ROOT_LO end=$ROOT_HI name=LINUX dostype=0x4c4e5800 automount=0 \
    + import DH0 "$W/dh0.hdf" + import LBOOT "$W/lboot.hdf" \
    + import SWAP "$W/swap.img" + import LINUX "$W/root.img"
rdbtool -r "$W/disk.img" info
rm -f "$W/dh0.hdf" "$W/lboot.hdf" "$W/swap.img" "$W/root.img"

# ---------------------------------------------------------------- output
log "Compressing image"
IMG=$OUT/warp-cf-${CF_BLOCKS}.img.gz
gzip -1 -c "$W/disk.img" > "$IMG.part"
mv "$IMG.part" "$IMG"
{
    echo "CF_BLOCKS=$CF_BLOCKS"
    echo "IMAGE_BYTES=$(stat -c %s "$W/disk.img")"
    echo "CHS=$CYLS/$HEADS/$SECS"
    echo "BUILT=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "KERNEL=$(basename "$VMLINUX") $(stat -c %s "$VMLINUX") bytes"
    echo "ROOTFS=$ROOTFS_TAR"
} > "$IMG.info"
rm -f "$W/disk.img"
log "Image: ${IMG#$REPO/} ($(du -h "$IMG" | cut -f1) compressed)"
