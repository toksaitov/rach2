#ifndef _MBR_H_
#define _MBR_H_

#include "../lib/kint.h"

/*
 * mbr: the classic four-entry partition table in the disk's first sector,
 * parsed into plain records. The parser is pure (bytes in, records out, no
 * device access), so the host seam test exercises it against crafted tables;
 * the kernel's disk mount and the boot-partition device both read the disk
 * through this one parse.
 */

#define MBR_PART_COUNT 4

/* The post-MBR gap: LBA 1 through the first partition's conventional start at
 * 2048. GRUB's core image lives here, outside every partition, which is why a
 * partition table that reached into it would be describing a disk this kernel
 * must not publish a boot partition for. */
#define MBR_GAP_FIRST_LBA 1u
#define MBR_GAP_END_LBA   2048u

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
 * Validates the 0x55AA signature and extracts the four entries as stored;
 * no per-entry judgement happens here. Returns 0 on success, -1 when the
 * signature is absent (the entries are then untouched).
 */
int mbr_parse(const uint8_t *sector, struct mbr_part parts[MBR_PART_COUNT]);

/**
 * Judge whether entry 0 is an acceptable boot partition beside the mounted
 * KRFFS partition at `krffs_index`.
 *
 * Everything `mbr_bootpart_ok` requires, and in addition a KRFFS entry that is
 * a different entry from entry 0 and disjoint from it. The containment and
 * overlap arithmetic subtracts rather than adds, so a crafted range near the
 * 32-bit edge cannot wrap past a check.
 *
 * @return 0 (the accepted entry's index) or -1.
 */
int mbr_bootpart_pick(const struct mbr_part parts[MBR_PART_COUNT],
                      int krffs_index, uint64_t disk_sectors);

/**
 * Judge entry 0 as a boot partition with no mounted root to compare against.
 *
 * `mbr_bootpart_pick` answers the same question for a machine that has just
 * mounted its root and can name the entry holding it. A machine booting a RAM
 * image never mounts one and still has a boot partition to manage, so the
 * judgement here compares entry 0 against the whole table instead: it must be
 * FAT-typed, non-empty, contained in the disk, start at or after the end of the
 * post-MBR gap so it cannot overlap the embedded boot loader, and share no
 * sector with any other non-empty entry.
 *
 * @return 0 when entry 0 qualifies, -1 otherwise.
 */
int mbr_bootpart_ok(const struct mbr_part parts[MBR_PART_COUNT], uint64_t disk_sectors);

/**
 * Whether every non-empty entry leaves the post-MBR gap alone.
 *
 * GRUB's core image lives in LBA 1 through 2047, outside every partition by
 * convention rather than by anything the table records. A table that puts a
 * partition there describes a disk where the boot loader's sectors and a file
 * system's sectors are the same sectors, and a device published over the gap
 * would let a write to one destroy the other. This project's own test fixture
 * did exactly that until milestone 25, so the case is real rather than
 * theoretical.
 *
 * @return 0 when the gap is clear, -1 when any non-empty entry reaches into it.
 */
int mbr_gap_clear(const struct mbr_part parts[MBR_PART_COUNT]);

#endif /* _MBR_H_ */
