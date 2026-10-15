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
#   SWAP_AUTO   1 = activate the swap partition at boot (default: noauto)
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

# ---------------------------------------------------------------- up to date?
# The inputs (files by size and time, this script, the settings) are hashed
# into the .info: the same inputs again reuse the image instead of an
# identical rebuild.  Delete the .info to force one.
BASE=$OUT/warp-cf-${CF_BLOCKS}
inputs_id() {
    {
        local f
        for f in "$ROOTFS_TAR" "$VMLINUX" "$MODULES_TGZ" "$AMIBOOT" "$DH0_IMAGE"; do
            if [ -f "$f" ]; then stat -c '%n %s %Y' "$f"; else echo "$f -"; fi
        done
        sha256sum < "$REPO/cfcard/mkimage.sh"
        echo "$CF_BLOCKS $DH0_MB $LBOOT_MB $SWAP_MB $ROOT_DISK $VIDEO ${SWAP_AUTO:-0}"
    } | sha256sum | cut -c1-32
}
INPUTS=$(inputs_id)
if [ -f "$BASE.info" ] && [ -f "$BASE.data.gz" ] && [ -f "$BASE.extents" ] && [ -f "$BASE.ranges" ] &&
   [ "$(sed -n 's/^INPUTS=//p' "$BASE.info")" = "$INPUTS" ]; then
    log "Image ${BASE#$REPO/} is up to date (same rootfs, kernel, modules, DH0 and settings)"
    exit 0
fi
rm -f "$BASE.info"   # rebuilt below; without it a half-built image is never used

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
; One-shot test kernel: LBOOT:vmlinux.try is renamed to vmlinux.trying and
; booted once; the next boot (after a hang and reset) is LBOOT:vmlinux again.
Stack 65536
If EXISTS LBOOT:vmlinux.trying
  Delete LBOOT:vmlinux.trying QUIET
EndIf
If EXISTS LBOOT:vmlinux.try
  Rename LBOOT:vmlinux.try LBOOT:vmlinux.trying
  LBOOT:amiboot-5.6 -k LBOOT:vmlinux.trying "root=/dev/{DISK}4 $OPTS"
EndIf
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
                                   (LBOOT:vmlinux.try, if there, is booted once)
  Execute LBOOT:Linux-debug        verbose Warp driver messages, no INT2 (polling)
  ... DISK=sdb                     if a drive sits on the A1200 IDE port too
                                   (that one is probed first and becomes sda)

Kernel: LBOOT:vmlinux  (6.9, built $(date -u +%Y-%m-%d))
Partitions: 1 DH0, 2 LBOOT, 3 swap (label warp-swap, off by default:
            swapon LABEL=warp-swap), 4 root ext4
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
if [ "${SWAP_AUTO:-0}" = 1 ]; then
    SWAP_OPTS="sw,nofail,x-systemd.device-timeout=5min"
else
    SWAP_OPTS="sw,noauto"
fi
export SWAP_OPTS
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
    # by label: the card is sda or sdb depending on the A1200 IDE port.
    # noauto: swap to the CF over PIO is slower than going without, so it is
    # not activated by default - "swapon LABEL=warp-swap" when wanted
    # (SWAP_AUTO=1: activate at boot; udev reaches the disks minutes into a
    # 68k boot, hence the long timeout, and nofail keeps the boot from waiting).
    grep -q "warp-swap" "$W/tree/etc/fstab" ||
        printf "LABEL=warp-swap\tnone\tswap\t%s\t0\t0\n" "$SWAP_OPTS" >> "$W/tree/etc/fstab"
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
    + add start=$ROOT_LO end=$ROOT_HI name=LINUX dostype=0x4c4e5800 automount=0
# The partition contents go in with dd conv=sparse rather than rdbtool import:
# import writes every block, zeros too, so a big card needs the whole card's
# size free in the build volume (a 64 GB card did not fit).  Sparse, disk.img
# only takes what the partitions really hold.
put_part() {  # put_part <name> <image> <first cyl> <last cyl>
    local want got
    want=$(part_bytes "$3" "$4")
    got=$(stat -c %s "$2")
    [ "$got" = "$want" ] || die "$1: image is $got bytes, the partition $want"
    local off=$(( $3 * CYL_BLOCKS * 512 )) mib=1048576
    dd if="$2" of="$W/disk.img" bs=4M oflag=seek_bytes seek=$off conv=notrunc,sparse status=none
    # read back the first and the last MiB of the partition
    cmp -n $mib "$2" <(dd if="$W/disk.img" bs=$mib iflag=skip_bytes,count_bytes skip=$off count=$mib status=none) ||
        die "$1: first MiB differs after writing"
    cmp -n $mib <(dd if="$2" bs=$mib iflag=skip_bytes,count_bytes skip=$(( got - mib )) count=$mib status=none) \
        <(dd if="$W/disk.img" bs=$mib iflag=skip_bytes,count_bytes skip=$(( off + got - mib )) count=$mib status=none) ||
        die "$1: last MiB differs after writing"
    echo "wrote $1 ($(( got >> 20 )) MiB at cylinder $3)"
}
put_part DH0 "$W/dh0.hdf" $DH0_LO $DH0_HI
put_part LBOOT "$W/lboot.hdf" $LBOOT_LO $LBOOT_HI
put_part SWAP "$W/swap.img" $SWAP_LO $SWAP_HI
put_part LINUX "$W/root.img" $ROOT_LO $ROOT_HI
echo "disk.img: $(du -h "$W/disk.img" | cut -f1) allocated of $(( $(stat -c %s "$W/disk.img") >> 20 )) MiB"

# What a card write must cover (make-cf.sh --used-only): the RDB, DH0 and LBOOT
# whole, the swap header, and the blocks the ext4 root uses (its block bitmaps:
# superblocks, descriptors, bitmaps, inode tables, journal, data - zeros
# included).  Free ext4 blocks are never read, so whatever the card holds
# there can stay.  "offset length" in bytes, MiB-aligned, merged.
log "Write ranges for --used-only"
dumpe2fs "$W/root.img" > "$W/dumpe2fs.txt" 2>/dev/null
python3 - "$W/dumpe2fs.txt" "$W/ranges" "$(stat -c %s "$W/disk.img")" \
    $(( DH0_LO * CYL_BLOCKS * 512 )) $(( SWAP_LO * CYL_BLOCKS * 512 )) $(( ROOT_LO * CYL_BLOCKS * 512 )) <<'PYEOF'
import re, sys
dump, out = sys.argv[1:3]
disk_bytes, dh0_off, swap_off, root_off = map(int, sys.argv[3:])
bsize = blocks = None
free = []
for line in open(dump):
    if line.startswith("Block size:"):
        bsize = int(line.split()[-1])
    elif line.startswith("Block count:"):
        blocks = int(line.split()[-1])
    else:
        m = re.match(r"\s+Free blocks: (.*)", line)
        if m:
            for r in filter(None, (x.strip() for x in m.group(1).split(","))):
                a, _, b = r.partition("-")
                free.append((int(a), int(b or a)))
assert bsize and blocks, "dumpe2fs: no block size/count"
free.sort()
want = [(0, swap_off), (swap_off, 1 << 20)]   # RDB + DH0 + LBOOT, swap header
pos = 0
for a, b in free + [(blocks, blocks)]:
    if a > pos:
        want.append((root_off + pos * bsize, (a - pos) * bsize))
    pos = max(pos, b + 1)
MIB = 1 << 20
merged = []
for off, n in sorted(want):
    lo, hi = off // MIB * MIB, min(-(-(off + n) // MIB) * MIB, disk_bytes)
    if merged and lo <= merged[-1][1]:
        merged[-1][1] = max(merged[-1][1], hi)
    else:
        merged.append([lo, hi])
with open(out, "w") as f:
    for lo, hi in merged:
        f.write(f"{lo} {hi - lo}\n")
total = sum(hi - lo for lo, hi in merged)
print(f"  {len(merged)} ranges, {total >> 20} MiB of {disk_bytes >> 20} MiB "
      f"(ext4: {sum(b - a + 1 for a, b in free) * bsize >> 20} MiB free, skipped)")
PYEOF
rdbtool -r "$W/disk.img" info
rm -f "$W/dh0.hdf" "$W/lboot.hdf" "$W/swap.img" "$W/root.img" "$W/dumpe2fs.txt"

# ---------------------------------------------------------------- output
# Stored sparse: the data extents of disk.img ("offset length", what
# SEEK_DATA/SEEK_HOLE report) and their bytes gzipped one after the other;
# everything else is zero.  cfcard/write-image.py writes it to a card.
log "Image data"
rm -f "$OUT/warp-cf-${CF_BLOCKS}.img.gz" "$OUT/warp-cf-${CF_BLOCKS}.img.gz".*   # older format
python3 - "$W/disk.img" "$BASE.extents.part" <<'PYEOF' | gzip -1 > "$BASE.data.gz.part"
import os, sys
src, ext = sys.argv[1:]
fd = os.open(src, os.O_RDONLY)
size = os.fstat(fd).st_size
out = sys.stdout.buffer
pos = total = 0
with open(ext, "w") as e:
    while pos < size:
        try:
            start = os.lseek(fd, pos, os.SEEK_DATA)
        except OSError:          # ENXIO: only a hole is left
            break
        end = os.lseek(fd, start, os.SEEK_HOLE)
        e.write(f"{start} {end - start}\n")
        n = end - start
        os.lseek(fd, start, os.SEEK_SET)
        while n:
            b = os.read(fd, min(n, 4 << 20))
            out.write(b)
            n -= len(b)
        total += end - start
        pos = end
print(f"  {total >> 20} MiB of data in the {size >> 20} MiB image", file=sys.stderr)
PYEOF
{
    echo "CF_BLOCKS=$CF_BLOCKS"
    echo "IMAGE_BYTES=$(stat -c %s "$W/disk.img")"
    echo "CHS=$CYLS/$HEADS/$SECS"
    echo "RDB_BYTES=$(( DH0_LO * CYL_BLOCKS * 512 ))"
    echo "DH0_OFFSET=$(( DH0_LO * CYL_BLOCKS * 512 ))"
    echo "DH0_BYTES=$DH0_BYTES"
    echo "BUILT=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "KERNEL=$(basename "$VMLINUX") $(stat -c %s "$VMLINUX") bytes"
    echo "ROOTFS=$ROOTFS_TAR"
    echo "INPUTS=$INPUTS"
} > "$BASE.info.part"
cp "$W/ranges" "$BASE.ranges"
mv "$BASE.extents.part" "$BASE.extents"
mv "$BASE.data.gz.part" "$BASE.data.gz"
mv "$BASE.info.part" "$BASE.info"     # last: the .info marks a complete image
rm -f "$W/disk.img"
log "Image: ${BASE#$REPO/}.{data.gz,extents,ranges,info} ($(du -h "$BASE.data.gz" | cut -f1) compressed data)"
