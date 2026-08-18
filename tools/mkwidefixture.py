#!/usr/bin/env python3
"""Build a sparse disk as large as the one milestone 25 seals into the laptop.

The raw disk windows seek and transfer through a 64-bit offset since milestone
25, and nothing else in the tree is big enough to tell that apart from the old
32-bit one: every test image is tens of megabytes. Size alone is not enough
either. A fixture of a few gigabytes would be satisfied by an implementation
that kept only 33 bits of offset, which is why this one is 260 GB, larger than
the 250 GB drive being sealed: its far marker needs 38 bits, so an offset
truncated anywhere below that misses it.

Two 512-byte patterns are written, one just past 4 GiB and one near the end
(byte j of the marker at offset k is (j * 7 + 3 + k // GIB) & 0xff), plus an
MBR signature with no partitions, so discovery runs and publishes no boot
partition. The file is sparse, so all of it costs kilobytes on disk.

Usage:
    mkwidefixture.py <disk-image>
"""
import sys

SECTOR = 512
GIB = 1024 * 1024 * 1024
SIZE = 260 * GIB
NEAR_AT = 5 * GIB
FAR_AT = 258 * GIB


def main(argv):
    if len(argv) != 2:
        print("usage: mkwidefixture.py <disk-image>", file=sys.stderr)
        return 2
    with open(argv[1], "wb") as f:
        f.truncate(SIZE)
        for at in (NEAR_AT, FAR_AT):
            f.seek(at)
            tag = at // GIB
            f.write(bytes((j * 7 + 3 + tag) & 0xFF for j in range(SECTOR)))
        f.seek(510)
        f.write(b"\x55\xaa")
    print("mkwidefixture: %s: %d bytes, patterns at %d and %d" %
          (argv[1], SIZE, NEAR_AT, FAR_AT))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
