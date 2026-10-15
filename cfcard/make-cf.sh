#!/usr/bin/env bash
# Prepare a Warp CF card from scratch on macOS: build the card image for the
# card's exact size (in the build container), then write it to the card.
#
#   cfcard/make-cf.sh [--image-only] [--used-only [--keep-dh0]] [--verify] disk39   (or /dev/disk39, /dev/rdisk39)
#
# Layout and options: see cfcard/mkimage.sh (DH0_MB, LBOOT_MB, SWAP_MB,
# ROOT_DISK, VIDEO are passed through).  Needs ./build.sh all to have run.
# Writing needs sudo and ERASES THE WHOLE CARD.  --used-only writes just what
# the new card needs (RDB, DH0, LBOOT, swap header, the blocks the ext4 root
# uses) and leaves the rest of the card as it was: much faster on a big card.
# --keep-dh0 also leaves the RDB and DH0 (AmigaOS) as they are on the card;
# only if the card's partition layout is the new image's (cfcard/rdb-layout.py).
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
image_only=0
verify=0
used_only=0
keep_dh0=0
disk=""

for arg in "$@"; do
    case "$arg" in
        --image-only) image_only=1 ;;
        --verify) verify=1 ;;
        --used-only) used_only=1 ;;
        --keep-dh0) keep_dh0=1 ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        -*) echo "unknown option: $arg" >&2; exit 1 ;;
        *) disk=$arg ;;
    esac
done
[ -n "$disk" ] || { sed -n '2,14p' "$0"; exit 1; }
[ "$keep_dh0" = 0 ] || [ "$used_only" = 1 ] || { echo "--keep-dh0 needs --used-only" >&2; exit 1; }

die() { echo "ERROR: $*" >&2; exit 1; }

# disk39 | /dev/disk39 | /dev/rdisk39 -> disk39
id=${disk#/dev/}
id=${id#r}
[[ "$id" =~ ^disk[0-9]+$ ]] || die "not a whole-disk identifier: $disk"

plist=$(diskutil info -plist "/dev/$id" 2>/dev/null) || die "/dev/$id not found"
key() { plutil -extract "$1" raw -o - - <<<"$plist" 2>/dev/null || echo ""; }

whole=$(key WholeDisk)
internal=$(key Internal)
removable=$(key RemovableMedia)
external=$(key RemovableMediaOrExternalDevice)
size=$(key TotalSize)
bsize=$(key DeviceBlockSize)
media=$(key MediaName)
proto=$(key BusProtocol)

# ---- what the image needs: a whole disk with 512-byte sectors and a sane size
[ "$whole" = "true" ] || die "/dev/$id is not a whole disk"
[ "$bsize" = "512" ] || die "/dev/$id has $bsize-byte blocks, expected 512"
[ -n "$size" ] && [ "$size" -gt $((512 * 1024 * 1024)) ] || die "/dev/$id: implausible size '$size'"

# ---- safety checks before writing: only a removable medium, never the system.
# A card in a built-in reader (Mac Studio SDXC) is Internal but RemovableMedia;
# an internal SSD never has removable media.
write_checks() {
    if [ "$internal" = "true" ]; then
        [ "$removable" = "true" ] || die "/dev/$id is an internal disk - refusing"
    else
        [ "$external" = "true" ] || die "/dev/$id is not removable/external - refusing"
    fi
    [ "$size" -le $((256 * 1024 * 1024 * 1024)) ] || die "/dev/$id is larger than 256 GB - refusing (not a CF card?)"
    local boot_disk
    boot_disk=$(diskutil info -plist / | plutil -extract ParentWholeDisk raw -o - - 2>/dev/null || true)
    [ "$id" != "$boot_disk" ] || die "/dev/$id holds the running system - refusing"
    if diskutil apfs list 2>/dev/null | grep -qE "Physical Store Disk:[[:space:]]+${id}(s[0-9]+)?$"; then
        die "/dev/$id is an APFS physical store - refusing"
    fi
}
# fail early, before a long image build, unless only the image is wanted
[ "$image_only" = 1 ] || write_checks

blocks=$((size / 512))
echo "Card: /dev/$id  \"$media\"  $proto  $size bytes ($blocks sectors)"

# ---- build the image for exactly this card
for f in "$repo/output/kernel/vmlinux"; do
    [ -f "$f" ] || die "missing $f - run ./build.sh all first"
done
CF_BLOCKS=$blocks "$repo/build.sh" cfimage
# sparse image: .data.gz + .extents + .ranges + .info (see cfcard/write-image.py)
img=$repo/output/cfcard/warp-cf-$blocks
for f in "$img.info" "$img.data.gz" "$img.extents" "$img.ranges"; do
    [ -f "$f" ] || die "image not built: $f missing"
done
img_bytes=$(sed -n 's/^IMAGE_BYTES=//p' "$img.info")
[ "$img_bytes" -le "$size" ] || die "image ($img_bytes) larger than the card ($size)"
writer=("$repo/cfcard/write-image.py")

if [ "$image_only" = 1 ]; then
    echo "Image ready: ${img#$repo/} (not written, --image-only)"
    exit 0
fi

# ---- confirm (the checks again: the disk may have changed during the build)
write_checks
echo
echo "Current contents of /dev/$id:"
diskutil list "/dev/$id" | sed 's/^/  /'
echo
echo "This ERASES the whole card /dev/$id (\"$media\", $((size / 1000000)) MB) and writes"
skip=()
if [ "$keep_dh0" = 1 ]; then
    rdb_bytes=$(sed -n 's/^RDB_BYTES=//p' "$img.info")
    dh0_off=$(sed -n 's/^DH0_OFFSET=//p' "$img.info")
    dh0_bytes=$(sed -n 's/^DH0_BYTES=//p' "$img.info")
    [ -n "$rdb_bytes" ] && [ -n "$dh0_off" ] && [ -n "$dh0_bytes" ] || die "$img.info has no RDB/DH0 layout (rebuild the image)"
    sudo -v || die "sudo failed"
    # Same partition layout (geometry, names, cylinders, DOS types; boot
    # priority and flags may differ): DH0 on the card is where the image has
    # it.  The card's RDB stays too, with whatever was changed there.
    tmp=$(mktemp -d)
    sudo dd if="/dev/r$id" of="$tmp/card.rdb" bs=$rdb_bytes count=1 2>/dev/null
    python3 -I "${writer[@]}" --dump "0:$rdb_bytes" "$img" > "$tmp/image.rdb"
    python3 -I "$repo/cfcard/rdb-layout.py" "$tmp/card.rdb" > "$tmp/card.txt" 2>&1 || true
    python3 -I "$repo/cfcard/rdb-layout.py" "$tmp/image.rdb" > "$tmp/image.txt"
    if ! diff -u "$tmp/card.txt" "$tmp/image.txt" | sed 's/^/  /'; then
        rm -rf "$tmp"
        die "the card's partition layout differs from the new image's - DH0 cannot be kept (drop --keep-dh0)"
    fi
    rm -rf "$tmp"
    skip=(--skip "0:$((dh0_off + dh0_bytes))")
    echo "The card's partition layout matches the image: its RDB and DH0 stay as they are."
fi
if [ "$used_only" = 1 ]; then
    used=$(awk '{ s += $2 } END { print s }' "$img.ranges")
    [ "$keep_dh0" = 1 ] && used=$((used - dh0_off - dh0_bytes))
    echo "  ${img#$repo/}  (only the used parts: about $((used >> 20)) of $((img_bytes >> 20)) MiB)"
else
    echo "  ${img#$repo/}  ($img_bytes bytes)"
fi
read -r -p "Type the disk identifier ($id) to continue: " answer
[ "$answer" = "$id" ] || die "not confirmed, nothing written"

# ---- write
sudo -v || die "sudo failed - run this script from a Terminal window so it can ask for the password"
diskutil unmountDisk "/dev/$id" >/dev/null || die "cannot unmount /dev/$id"
echo "Writing (sudo) ..."
start=$(date +%s)
flags=()
[ "$used_only" = 1 ] && flags+=(--used-only)
[ "$verify" = 1 ] && flags+=(--verify)
sudo /usr/bin/python3 -I "${writer[@]}" ${flags[@]+"${flags[@]}"} ${skip[@]+"${skip[@]}"} "$img" "/dev/r$id"
sync
echo "Written in $(( $(date +%s) - start )) s"

diskutil eject "/dev/$id" >/dev/null && echo "Card ejected."
cat <<EOF

Next, on the Amiga (card on the Warp IDE):
  - DH0: "System" (bootable, empty FFS)  -> install AmigaOS here (no format needed)
  - LBOOT:Linux  (Execute LBOOT:Linux, or LBOOT:Linux-debug for bring-up output)
  - Linux sees the card as /dev/sda: sda1 DH0, sda2 LBOOT, sda3 swap, sda4 root
EOF
