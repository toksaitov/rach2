/*
 * mbr.h: the four-entry partition table in a disk's first sector, parsed
 * into plain records, and the judgements of a boot disk's table. The code is
 * pure (bytes in, records out, no device access), so the host test drives it
 * with crafted tables; the kernel's boot partition and boot loader windows
 * and the host's stick check read a table through it.
 */

#ifndef _MBR_H_
#define _MBR_H_

#include "../lib/kint.h"

#define MBR_PART_COUNT 4

/* The post-MBR gap: LBA 1 up to the first partition's conventional start at
 * 2048. GRUB's core image is here, outside every partition, so this kernel
 * publishes no boot partition for a disk whose table reaches into it. */
#define MBR_GAP_FIRST_LBA 1u
#define MBR_GAP_END_LBA   2048u

/* The type of an EFI system partition, which UEFI firmware boots from on an
 * MBR disk. It is a FAT volume like types 0x0B and 0x0C, and it is the type
 * of the boot partition of a disk set up to boot by UEFI. */
#define MBR_TYPE_EFI_SYSTEM 0xEF

/** One partition entry: the type byte and the half-open sector range
 *  `[start_lba, start_lba + sector_count)`. An empty slot has both zero. */
struct mbr_part {
    uint8_t type;
    uint32_t start_lba;
    uint32_t sector_count;
};

/**
 * Parse a 512-byte MBR sector into `parts`.
 *
 * Checks the 0x55AA signature and copies the four entries as stored, judging
 * none of them. Returns 0 on success, -1 when the signature is absent (the
 * entries are then untouched).
 */
int mbr_parse(const uint8_t *sector, struct mbr_part parts[MBR_PART_COUNT]);

/** One partition `mbr_take` took: the half-open sector range it covers. */
struct mbr_range {
    uint64_t start_lba;
    uint64_t sector_count;
};

/**
 * Take the entries of a parsed table that can be published on a medium of
 * `disk_sectors` sectors.
 *
 * An entry that is empty is skipped; one that starts at the table's own
 * sector, runs past the medium, or overlaps one already taken is passed
 * over with the rest still read, and how many were passed over is put in
 * `refused` when it is not null. Returns how many were taken, packed in
 * table order. `tools/image/stickcheck.c` checks a stick image with it; the
 * kernel reads tables with `part_scan`, which applies the same rules.
 */
int mbr_take(const struct mbr_part parts[MBR_PART_COUNT], uint64_t disk_sectors,
             struct mbr_range taken[MBR_PART_COUNT], int *refused);

/**
 * Judge whether entry 0 is an acceptable boot partition beside the mounted
 * KRFFS partition at `krffs_index`.
 *
 * Everything `mbr_bootpart_ok` requires, and in addition a KRFFS entry that is
 * a different entry from entry 0 and disjoint from it. Sums are taken in 64
 * bits and the containment check subtracts, so a crafted range near the
 * 32-bit edge cannot wrap past a check.
 *
 * @return 0 (the accepted entry's index) or -1.
 */
int mbr_bootpart_pick(const struct mbr_part parts[MBR_PART_COUNT], int krffs_index,
                      uint64_t disk_sectors);

/**
 * Judge entry 0 as a boot partition with no mounted root to compare against.
 *
 * `mbr_bootpart_pick` answers the same question for a machine that has
 * mounted its root and knows the entry it is on. A machine booting a RAM
 * image mounts no root and still has a boot partition to manage, so entry 0
 * is compared with the whole table instead: it must be FAT-typed (0x0B, 0x0C
 * or the EFI system partition's 0xEF), non-empty, contained in the disk,
 * start at or after the end of the post-MBR gap so it cannot overlap the
 * embedded boot loader, and share no sector with any other non-empty entry.
 *
 * @return 0 when entry 0 qualifies, -1 otherwise.
 */
int mbr_bootpart_ok(const struct mbr_part parts[MBR_PART_COUNT], uint64_t disk_sectors);

/**
 * Whether every non-empty entry leaves the post-MBR gap alone.
 *
 * GRUB's core image is in LBA 1 through 2047, outside every partition by
 * convention and not by anything the table records. A table that puts a
 * partition there describes a disk where the boot loader's sectors and a file
 * system's sectors are the same sectors, and a device published over the gap
 * would let a write to one destroy the other.
 *
 * @return 0 when the gap is clear, -1 when any non-empty entry reaches into it.
 */
int mbr_gap_clear(const struct mbr_part parts[MBR_PART_COUNT]);

#endif /* _MBR_H_ */
