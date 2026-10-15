#!/usr/bin/env python3
"""Print the partition layout of an Amiga RDB, one line per item.

    rdb-layout.py FILE        (the first cylinders of a disk or image)

Only what decides where the partitions are: the disk geometry and each
partition's name, cylinders, block size, surfaces, blocks per track and DOS
type.  Boot priority, bootable/automount flags, buffers and the like are left
out, so two RDBs that print the same have their partitions in the same place
(make-cf.sh --keep-dh0).
"""
import struct
import sys


def main():
    data = open(sys.argv[1], "rb").read()
    blk = lambda n: data[n * 512:(n + 1) * 512]
    for n in range(16):
        if blk(n)[:4] == b"RDSK":
            break
    else:
        raise SystemExit("no RDSK block in the first 16 blocks")
    rdsk = blk(n)
    block_bytes = struct.unpack_from(">I", rdsk, 16)[0]
    part = struct.unpack_from(">I", rdsk, 28)[0]
    cyls, sectors, heads = struct.unpack_from(">III", rdsk, 64)
    print(f"disk block={block_bytes} cyls={cyls} heads={heads} sectors={sectors}")
    seen = set()
    while part != 0xFFFFFFFF:
        if part in seen or (part + 1) * 512 > len(data):
            raise SystemExit(f"partition list broken at block {part}")
        seen.add(part)
        pb = blk(part)
        if pb[:4] != b"PART":
            raise SystemExit(f"block {part} is not a PART block")
        name = pb[37:37 + pb[36]].decode("latin-1")
        env = struct.unpack_from(">17I", pb, 128)
        size_block, surfaces, blocks_per_track = env[1], env[3], env[5]
        low, high, dostype = env[9], env[10], env[16]
        print(f"part {name} cyl={low}-{high} longs/block={size_block} surfaces={surfaces} "
              f"blocks/track={blocks_per_track} dostype=0x{dostype:08x}")
        part = struct.unpack_from(">I", pb, 16)[0]


if __name__ == "__main__":
    main()
