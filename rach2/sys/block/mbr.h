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
 * Entry 0 qualifies only when it is FAT-typed (0x0b or 0x0c), non-empty,
 * contained in the `disk_sectors`-sector disk without wrapping, a different
 * entry from the KRFFS one, and disjoint from the KRFFS partition's half-open
 * sector range. The containment and overlap arithmetic subtracts rather than
 * adds, so a crafted range near the 32-bit edge cannot wrap past a check.
 *
 * @return 0 (the accepted entry's index) or -1.
 */
int mbr_bootpart_pick(const struct mbr_part parts[MBR_PART_COUNT],
                      int krffs_index, uint64_t disk_sectors);

#endif /* _MBR_H_ */
