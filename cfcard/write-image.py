#!/usr/bin/env python3
"""Write a card image built by cfcard/mkimage.sh to a raw device.

    write-image.py [--verify] [--used-only] [--skip OFFSET:LENGTH] BASE /dev/rdiskN
    write-image.py --dump OFFSET:LENGTH BASE > file

BASE is output/cfcard/warp-cf-<sectors>; the image is stored sparse:
  BASE.extents   "offset length" of each data extent of the disk image, sorted
  BASE.data.gz   the bytes of those extents, one after the other, gzipped
  BASE.info      IMAGE_BYTES=... (everything outside the extents is zero)
  BASE.ranges    what a card needs (make-cf.sh --used-only), see mkimage.sh

Default: the whole image, zeros included.  --used-only: only BASE.ranges.
--skip leaves a byte range of the device alone (make-cf.sh --keep-dh0).
--verify reads each chunk back and compares it.  --dump writes part of the
image to stdout.  Run by make-cf.sh, as root for the device.
"""
import os
import subprocess
import sys
import time

CHUNK = 4 << 20


def read_pairs(path):
    pairs = []
    with open(path) as f:
        for line in f:
            if line.strip():
                off, n = map(int, line.split())
                if pairs and off < sum(pairs[-1]):
                    raise SystemExit(f"{path}: not sorted at {off}")
                pairs.append((off, n))
    return pairs


def cut(ranges, lo, hi):
    out = []
    for off, n in ranges:
        end = off + n
        if end <= lo or off >= hi:
            out.append((off, n))
            continue
        if off < lo:
            out.append((off, lo - off))
        if end > hi:
            out.append((hi, end - hi))
    return out


class Image:
    """Reads the sparse image in ascending, non-overlapping pieces."""

    def __init__(self, base):
        info = dict(l.strip().split("=", 1) for l in open(base + ".info") if "=" in l)
        self.size = int(info["IMAGE_BYTES"])
        self.extents = read_pairs(base + ".extents")
        self.gz = subprocess.Popen(["gzip", "-dc", base + ".data.gz"],
                                   stdout=subprocess.PIPE, bufsize=CHUNK)
        self.i = 0          # current extent
        self.stream = 0     # bytes consumed from the data stream
        self.start = []     # stream offset of each extent
        s = 0
        for _, n in self.extents:
            self.start.append(s)
            s += n
        self.data_bytes = s

    def _stream_read(self, at, n):
        skip = at - self.stream
        if skip < 0:
            raise SystemExit("image read out of order")
        while skip:
            b = self.gz.stdout.read(min(CHUNK, skip))
            if not b:
                raise SystemExit("image data ends early")
            skip -= len(b)
        buf = bytearray()
        while len(buf) < n:
            b = self.gz.stdout.read(n - len(buf))
            if not b:
                raise SystemExit("image data ends early")
            buf += b
        self.stream = at + n
        return buf

    def read(self, off, n):
        """Bytes [off, off+n) of the image; calls must ascend."""
        buf = bytearray(n)
        end = off + n
        while self.i < len(self.extents) and sum(self.extents[self.i]) <= off:
            self.i += 1
        j = self.i
        while j < len(self.extents) and self.extents[j][0] < end:
            es, el = self.extents[j]
            a, b = max(off, es), min(end, es + el)
            if a < b:
                buf[a - off:b - off] = self._stream_read(self.start[j] + a - es, b - a)
            j += 1
        return bytes(buf)

    def close(self):
        self.gz.stdout.close()
        self.gz.kill()
        self.gz.wait()


def main():
    args = sys.argv[1:]
    opt = lambda name: name in args and not args.remove(name)
    verify = opt("--verify")
    used_only = opt("--used-only")
    skips, dump = [], None
    while "--skip" in args or "--dump" in args:
        name = "--skip" if "--skip" in args else "--dump"
        i = args.index(name)
        off, n = map(int, args[i + 1].split(":"))
        del args[i:i + 2]
        if name == "--skip":
            skips.append((off, off + n))
        else:
            dump = (off, n)
    if dump:
        if len(args) != 1:
            raise SystemExit(__doc__)
        img = Image(args[0])
        out = sys.stdout.buffer
        off, n = dump
        while n:
            k = min(CHUNK, n)
            out.write(img.read(off, k))
            off += k
            n -= k
        img.close()
        return
    if len(args) != 2:
        raise SystemExit(__doc__)
    base, dev = args
    img = Image(base)
    ranges = read_pairs(base + ".ranges") if used_only else [(0, img.size)]
    for lo, hi in skips:
        ranges = cut(ranges, lo, hi)
    for off, n in ranges:
        if off % 512 or n % 512 or off + n > img.size:
            raise SystemExit(f"bad range {off} {n}")
    total = sum(n for _, n in ranges)
    print(f"{len(ranges)} ranges, {total >> 20} MiB to write")

    fd = os.open(dev, os.O_RDWR)
    done = bad = 0
    start = last = time.time()
    try:
        for off, n in ranges:
            pos, end = off, off + n
            while pos < end:
                k = min(CHUNK - pos % CHUNK, end - pos)   # 4 MiB-aligned pieces
                data = img.read(pos, k)
                os.pwrite(fd, data, pos)
                if verify:
                    os.fsync(fd)
                    if os.pread(fd, k, pos) != data:
                        print(f"\nverify: differs in {pos}..{pos + k}")
                        bad += 1
                pos += k
                done += k
                now = time.time()
                if now - last >= 2:
                    last = now
                    rate = done / (now - start) / 1e6
                    print(f"\r  {done >> 20} / {total >> 20} MiB  {rate:.1f} MB/s ", end="", flush=True)
        os.fsync(fd)
    finally:
        os.close(fd)
        img.close()
    print(f"\r  {done >> 20} / {total >> 20} MiB in {time.time() - start:.0f} s" + " " * 20)
    if verify:
        if bad:
            raise SystemExit(f"verify FAILED ({bad} chunks)")
        print("Verify OK")


if __name__ == "__main__":
    main()
