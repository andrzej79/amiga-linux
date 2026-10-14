#!/usr/bin/env bash
# Prepare a Warp CF card from scratch on macOS: build the card image for the
# card's exact size (in the build container), then write it to the card.
#
#   cfcard/make-cf.sh [--image-only] [--verify] disk39        (or /dev/disk39, /dev/rdisk39)
#
# Layout and options: see cfcard/mkimage.sh (DH0_MB, LBOOT_MB, SWAP_MB,
# ROOT_DISK, VIDEO are passed through).  Needs ./build.sh all to have run.
# Writing needs sudo and ERASES THE WHOLE CARD.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
image_only=0
verify=0
disk=""

for arg in "$@"; do
    case "$arg" in
        --image-only) image_only=1 ;;
        --verify) verify=1 ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        -*) echo "unknown option: $arg" >&2; exit 1 ;;
        *) disk=$arg ;;
    esac
done
[ -n "$disk" ] || { sed -n '2,10p' "$0"; exit 1; }

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
img=$repo/output/cfcard/warp-cf-$blocks.img.gz
[ -f "$img" ] || die "image not built: $img"
img_bytes=$(sed -n 's/^IMAGE_BYTES=//p' "$img.info")
[ "$img_bytes" -le "$size" ] || die "image ($img_bytes) larger than the card ($size)"

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
echo "  ${img#$repo/}  ($img_bytes bytes)"
read -r -p "Type the disk identifier ($id) to continue: " answer
[ "$answer" = "$id" ] || die "not confirmed, nothing written"

# ---- write
sudo -v || die "sudo failed - run this script from a Terminal window so it can ask for the password"
diskutil unmountDisk "/dev/$id" >/dev/null || die "cannot unmount /dev/$id"
progress=""
if dd if=/dev/zero of=/dev/null count=1 status=progress 2>/dev/null; then
    progress="status=progress"
fi
echo "Writing (sudo) ..."
start=$(date +%s)
gzip -dc "$img" | sudo dd of="/dev/r$id" bs=4m $progress
sync
echo "Written in $(( $(date +%s) - start )) s"

if [ "$verify" = 1 ]; then
    echo "Verifying ..."
    if gzip -dc "$img" | sudo cmp -n "$img_bytes" - "/dev/r$id"; then
        echo "Verify OK"
    else
        die "verify FAILED"
    fi
fi

diskutil eject "/dev/$id" >/dev/null && echo "Card ejected."
cat <<EOF

Next, on the Amiga (card on the Warp IDE):
  - DH0: "System" (bootable, empty FFS)  -> install AmigaOS here (no format needed)
  - LBOOT:Linux  (Execute LBOOT:Linux, or LBOOT:Linux-debug for bring-up output)
  - Linux sees the card as /dev/sda: sda1 DH0, sda2 LBOOT, sda3 swap, sda4 root
EOF
