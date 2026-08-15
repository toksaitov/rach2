/*
 * mbr.c: the MBR partition-table parse and the boot-partition judgement.
 *
 * Pure functions over bytes and records: the caller reads the sector and
 * decides what to mount. tests/host/mbr_hosttest.c drives both functions
 * against crafted tables, the malformed beside the valid.
 */

#include "mbr.h"

/** Read a little-endian 32-bit field from an unaligned table entry. */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
           ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

int mbr_parse(const uint8_t *sector, struct mbr_part parts[MBR_PART_COUNT])
{
    int i;

    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return -1;
    }
    for (i = 0; i < MBR_PART_COUNT; i++) {
        const uint8_t *e = sector + 446 + i * 16;

        parts[i].type = e[4];
        parts[i].start_lba = le32(e + 8);
        parts[i].sector_count = le32(e + 12);
    }
    return 0;
}

/** Whether the half-open sector ranges of `a` and `b` share any sector. */
static int ranges_overlap(const struct mbr_part *a, const struct mbr_part *b)
{
    uint64_t a_end = (uint64_t) a->start_lba + a->sector_count;
    uint64_t b_end = (uint64_t) b->start_lba + b->sector_count;

    return a->start_lba < b_end && b->start_lba < a_end;
}

int mbr_bootpart_pick(const struct mbr_part parts[MBR_PART_COUNT],
                      int krffs_index, uint64_t disk_sectors)
{
    const struct mbr_part *p = &parts[0];

    if (krffs_index < 0 || krffs_index >= MBR_PART_COUNT || krffs_index == 0) {
        return -1;
    }
    if (p->type != 0x0b && p->type != 0x0c) {
        return -1;
    }
    if (p->start_lba == 0 || p->sector_count == 0) {
        return -1;
    }
    if ((uint64_t) p->start_lba >= disk_sectors ||
        (uint64_t) p->sector_count > disk_sectors - p->start_lba) {
        return -1;
    }
    if (ranges_overlap(p, &parts[krffs_index])) {
        return -1;
    }
    return 0;
}
