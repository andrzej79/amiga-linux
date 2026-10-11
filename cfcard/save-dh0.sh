#!/usr/bin/env bash
# Save the AmigaOS system partition (DH0) of a Warp CF card, so the next
# cfcard/make-cf.sh writes it back instead of an empty volume.
#
#   cfcard/save-dh0.sh [--part NAME] disk39        (or /dev/disk39, /dev/rdisk39)
#
# Reads the card's Rigid Disk Block, finds the partition (default DH0), copies
# exactly its cylinders and stores them as output/cfcard/dh0.hdf.gz (plus a
# dated copy).  output/ is git-ignored: the AmigaOS files never enter the repo.
# Needs sudo for the raw reads; only reads the card.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
part=DH0
disk=""
while [ $# -gt 0 ]; do
    case "$1" in
        --part) part=$2; shift ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 1 ;;
        *) disk=$1 ;;
    esac
    shift
done
[ -n "$disk" ] || { sed -n '2,11p' "$0"; exit 1; }

die() { echo "ERROR: $*" >&2; exit 1; }
trap 'rc=$?; [ $rc -eq 0 ] || echo "ERROR: save-dh0.sh failed (exit $rc) at line $LINENO" >&2' ERR

id=${disk#/dev/}
id=${id#r}
[[ "$id" =~ ^disk[0-9]+$ ]] || die "not a whole-disk identifier: $disk"
plist=$(diskutil info -plist "/dev/$id" 2>/dev/null) || die "/dev/$id not found"
key() { plutil -extract "$1" raw -o - - <<<"$plist" 2>/dev/null || echo ""; }
[ "$(key WholeDisk)" = "true" ] || die "/dev/$id is not a whole disk"
[ "$(key Internal)" = "false" ] || die "/dev/$id is an internal disk - refusing"
[ "$(key RemovableMediaOrExternalDevice)" = "true" ] || die "/dev/$id is not removable/external - refusing"
echo "Card: /dev/$id  \"$(key MediaName)\"  $(key TotalSize) bytes"

out=$repo/output/cfcard
mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# ---- RDB: the first 16 blocks hold RDSK, the partition list follows within the RDB cylinders
# raw reads need root; ask now, visibly (a shell without a terminal cannot prompt)
sudo -v || die "sudo failed - run this script from a Terminal window so it can ask for the password"
diskutil unmountDisk "/dev/$id" >/dev/null 2>&1 || true
echo "Reading the partition table (sudo) ..."
sudo dd if="/dev/r$id" of="$tmp/rdb.bin" bs=512 count=4096 2>/dev/null
sudo chown "$(id -u)" "$tmp/rdb.bin"

# prints: lowCyl highCyl surfaces blocksPerTrack dostype(hex) name
info=$(python3 - "$tmp/rdb.bin" "$part" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
want = sys.argv[2]
def blk(n):
    return d[n * 512:(n + 1) * 512]
rdsk = None
for n in range(16):
    if blk(n)[:4] == b'RDSK':
        rdsk = blk(n)
        break
if rdsk is None:
    sys.exit("no RDSK block in the first 16 blocks - not an Amiga RDB disk")
nxt = struct.unpack('>I', rdsk[28:32])[0]
names = []
while nxt != 0xFFFFFFFF and nxt * 512 + 512 <= len(d):
    p = blk(nxt)
    if p[:4] != b'PART':
        sys.exit("broken partition list at block %d" % nxt)
    summed = struct.unpack('>I', p[4:8])[0]
    if summed > 128 or sum(struct.unpack('>%di' % summed, p[:summed * 4])) & 0xFFFFFFFF:
        sys.exit("bad PART checksum at block %d" % nxt)
    name = p[37:37 + p[36]].decode('latin-1')
    env = struct.unpack('>17I', p[128:196])
    names.append(name)
    if name == want:
        print(env[9], env[10], env[3], env[5], '%08x' % env[16], name)
        sys.exit(0)
    nxt = struct.unpack('>I', p[16:20])[0]
sys.exit("partition %s not found (have: %s)" % (want, ', '.join(names)))
EOF
) || die "$info"
read -r lo hi heads secs dostype pname <<<"$info"
cyl_bytes=$((heads * secs * 512))
cyls=$((hi - lo + 1))
echo "Partition $pname: cylinders $lo-$hi ($cyls x $heads x $secs), DosType 0x$dostype, $((cyls * cyl_bytes >> 20)) MiB"

# ---- copy the partition, cylinder-aligned
echo "Reading $pname (sudo) ..."
start=$(date +%s)
sudo dd if="/dev/r$id" bs="$cyl_bytes" skip="$lo" count="$cyls" 2>/dev/null | gzip -1 > "$tmp/part.hdf.gz"
echo "Read in $(( $(date +%s) - start )) s"

# sanity: the partition must start with the boot block DosType from the RDB.
# Read straight from the device: "gzip -dc | head -c 4" would SIGPIPE gzip,
# which pipefail + set -e turn into a silent exit.
sudo dd if="/dev/r$id" of="$tmp/first.bin" bs=512 skip=$((lo * heads * secs)) count=1 2>/dev/null
sudo chown "$(id -u)" "$tmp/first.bin"
first=$(od -A n -N 4 -t x1 "$tmp/first.bin" | tr -d ' \n')
[ "$first" = "$dostype" ] || die "partition does not start with DosType $dostype (found $first) - not saved"
size=$(gzip -dc "$tmp/part.hdf.gz" | wc -c | tr -d ' ')
[ "$size" -eq $((cyls * cyl_bytes)) ] || die "read $size bytes, expected $((cyls * cyl_bytes)) - not saved"

stamp=$(date +%Y%m%d-%H%M%S)
dated=$out/dh0-$stamp.hdf.gz
mv "$tmp/part.hdf.gz" "$dated"
cp "$dated" "$out/dh0.hdf.gz"
{
    echo "PART=$pname"
    echo "CYLS=$cyls"
    echo "CHS=$cyls/$heads/$secs"
    echo "BYTES=$((cyls * cyl_bytes))"
    echo "DOSTYPE=$dostype"
    echo "SAVED=$stamp"
} > "$out/dh0.hdf.gz.info"

# show what was saved (container: amitools)
docker run --rm -v "$out:/out:ro" amiga-linux-build bash -c \
    "gzip -dc /out/dh0.hdf.gz > /tmp/p.hdf && xdftool /tmp/p.hdf open chs=$cyls,$heads,$secs + list 2>/dev/null | head -25" ||
    echo "(listing skipped)"

echo
echo "Saved: ${dated#$repo/}  ($(du -h "$dated" | cut -f1) compressed)"
echo "Default for the next image: output/cfcard/dh0.hdf.gz"
