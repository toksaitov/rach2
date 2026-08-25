#!/usr/bin/env python3
"""Rewrite a mkdisk.sh disk into the bootprobe fixture.

mkdisk.sh builds an MBR whose entry 1 is the KRFFS partition at LBA 2048.
This tool moves that entry to slot 2 and writes a FAT-typed (0x0c) entry 1
after it: LBA 102400, 1024 sectors, filled with the pattern bootprobe expects
(byte j of partition sector k is (k * 7 + j) & 0xff). The kernel's mount still
finds KRFFS (it probes entry contents, not order), and the MBR then qualifies
for /dev/bootpart: FAT-typed entry 1, disjoint from KRFFS.

The partition sat at LBA 64 until milestone 25, inside the 1 MiB post-MBR gap
where GRUB's core image lives. That was harmless while nothing else reached
those sectors, and stopped being harmless when /dev/grubcore was published over
the gap: a boot partition overlapping the boot loader is a disk this kernel now
refuses to offer one for, so the fixture moved into the free space after the
KRFFS partition instead.

Usage:
    mkbootfixture.py <disk-image>
"""
import struct
import sys

PART_LBA = 102400
PART_SECTORS = 1024
SECTOR = 512


def main(argv):
    if len(argv) != 2:
        print("usage: mkbootfixture.py <disk-image>", file=sys.stderr)
        return 2
    path = argv[1]
    with open(path, "r+b") as f:
        mbr = bytearray(f.read(SECTOR))
        if mbr[510] != 0x55 or mbr[511] != 0xAA:
            print("mkbootfixture: no MBR signature in %s" % path,
                  file=sys.stderr)
            return 1
        entry1 = bytes(mbr[446:462])
        if struct.unpack_from("<I", entry1, 12)[0] == 0:
            print("mkbootfixture: entry 1 is empty; not a mkdisk image",
                  file=sys.stderr)
            return 1
        # Entry 2 takes the KRFFS partition entry as mkdisk wrote it.
        mbr[462:478] = entry1
        # Entry 1 becomes the FAT-typed fixture partition in the gap.
        fixture = bytearray(16)
        fixture[4] = 0x0C
        struct.pack_into("<I", fixture, 8, PART_LBA)
        struct.pack_into("<I", fixture, 12, PART_SECTORS)
        mbr[446:462] = fixture
        f.seek(0)
        f.write(mbr)

        f.seek(PART_LBA * SECTOR)
        for k in range(PART_SECTORS):
            f.write(bytes((k * 7 + j) & 0xFF for j in range(SECTOR)))
    print("mkbootfixture: %s: FAT-typed partition 1 at LBA %d, %d sectors of "
          "pattern" % (path, PART_LBA, PART_SECTORS))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
