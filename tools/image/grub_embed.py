#!/usr/bin/env python3
"""Embed GRUB's i386-pc boot.img and core.img into a disk image by hand.

grub-bios-setup cannot run on a Docker overlay filesystem (it fails to resolve
the disk's host device), so this performs the same install directly: it writes
boot.img to the MBR (keeping the partition table), places core.img in the
post-MBR gap at LBA 1, and patches the two pointers grub-bios-setup would patch.

  boot.img[0x5c]  the 8-byte LBA of core.img's first sector (1)
  boot.img[0x64]  the boot drive (0xff: use the BIOS-provided drive)
  core.img[0x1f4] the diskboot blocklist: the LBA (2) and count of the sectors
                  after core.img's first, since core.img runs contiguously
                  from LBA 1

The gap is whatever lies between LBA 1 and the first partition; this aborts if
core.img would run past it into a partition.

Usage: grub_embed.py <disk.img> <boot.img> <core.img>
"""
import struct
import sys

SECTOR = 512

disk, boot_path, core_path = sys.argv[1], sys.argv[2], sys.argv[3]

with open(boot_path, "rb") as f:
    boot = bytearray(f.read())
with open(core_path, "rb") as f:
    core = bytearray(f.read())

# Pad core.img to a whole number of sectors so the diskboot count and the write
# are sector-exact.
core_sectors = (len(core) + SECTOR - 1) // SECTOR
core += b"\x00" * (core_sectors * SECTOR - len(core))

with open(disk, "rb") as f:
    mbr = f.read(SECTOR)

# The post-MBR gap ends at the first partition. core.img occupies LBA 1 through
# core_sectors, so the next free LBA is core_sectors + 1; it must not reach the
# first partition's start.
starts = [struct.unpack_from("<I", mbr, 0x1be + i * 16 + 8)[0]
          for i in range(4)]
first_lba = min((s for s in starts if s), default=0)
if first_lba and core_sectors >= first_lba:
    sys.stderr.write(
        "grub_embed: core.img needs %d sectors but the gap before LBA %d "
        "has only %d\n" % (core_sectors, first_lba, first_lba - 1))
    sys.exit(1)

# The gap is two tail slots of 1023 sectors, LBA 2 through 1024 and LBA 1025
# through 2047, so that the machine can replace its own boot loader by writing
# the slot it is not running from (utils/grubslot.h). An image whose tail ran
# past LBA 1024 would leave no free slot, and the tool would refuse to install
# over it. Fail here, where a disk built wrong is cheap to rebuild.
MAX_IMAGE_SECTORS = 1024
if core_sectors > MAX_IMAGE_SECTORS:
    sys.stderr.write(
        "grub_embed: core.img is %d sectors and a tail slot has %d; the disk "
        "would boot but could not have its boot loader replaced\n"
        % (core_sectors, MAX_IMAGE_SECTORS - 1))
    sys.exit(1)

# core.img lives at LBA 1; boot.img must load it from there.
struct.pack_into("<Q", boot, 0x5c, 1)
boot[0x64] = 0xff

# The diskboot blocklist (end of core.img's first sector) names the rest of
# core.img: starting LBA 2, count = all sectors but the first.
struct.pack_into("<Q", core, 0x1f4, 2)
struct.pack_into("<H", core, 0x1fc, core_sectors - 1)

with open(disk, "r+b") as f:
    f.seek(0)
    f.write(bytes(boot[:446]))  # MBR boot code, leaving the partition table
    f.seek(SECTOR)
    f.write(bytes(core))        # core.img in the gap before the first partition
