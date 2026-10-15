/*
 * mbr.c: the MBR partition-table parse and the boot-partition judgements.
 *
 * Pure functions over bytes and records: the caller reads the sector and
 * decides what to publish. tests/host/mbr_hosttest.c drives them with
 * crafted tables, the malformed beside the valid.
 */

#include "mbr.h"

/** Read a little-endian 32-bit field from an unaligned table entry. */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

int mbr_take(const struct mbr_part parts[MBR_PART_COUNT], uint64_t disk_sectors,
             struct mbr_range taken[MBR_PART_COUNT], int *refused)
{
    int i;
    int count = 0;
    int passed_over = 0;

    for (i = 0; i < MBR_PART_COUNT; i++) {
        taken[i].start_lba = 0;
        taken[i].sector_count = 0;
    }
    for (i = 0; i < MBR_PART_COUNT; i++) {
        uint64_t first = parts[i].start_lba;
        uint64_t length = parts[i].sector_count;
        int j;
        int overlaps = 0;

        if (parts[i].type == 0 || length == 0) {
            continue;
        }
        if (first == 0 || first + length > disk_sectors) {
            passed_over++;
            continue;
        }
        for (j = 0; j < count; j++) {
            uint64_t other = taken[j].start_lba;

            if (first < other + taken[j].sector_count && other < first + length) {
                overlaps = 1;
            }
        }
        if (overlaps) {
            passed_over++;
            continue;
        }
        taken[count].start_lba = first;
        taken[count].sector_count = length;
        count++;
    }
    if (refused != 0) {
        *refused = passed_over;
    }
    return count;
}

/** Whether the half-open sector ranges of `a` and `b` share any sector. */
static int ranges_overlap(const struct mbr_part *a, const struct mbr_part *b)
{
    uint64_t a_end = (uint64_t)a->start_lba + a->sector_count;
    uint64_t b_end = (uint64_t)b->start_lba + b->sector_count;

    return a->start_lba < b_end && b->start_lba < a_end;
}

int mbr_gap_clear(const struct mbr_part parts[MBR_PART_COUNT])
{
    struct mbr_part gap;
    int i;

    gap.type = 0;
    gap.start_lba = MBR_GAP_FIRST_LBA;
    gap.sector_count = MBR_GAP_END_LBA - MBR_GAP_FIRST_LBA;

    for (i = 0; i < MBR_PART_COUNT; i++) {
        if (parts[i].sector_count == 0) {
            continue;
        }
        if (ranges_overlap(&parts[i], &gap)) {
            return -1;
        }
    }
    return 0;
}

int mbr_bootpart_ok(const struct mbr_part parts[MBR_PART_COUNT], uint64_t disk_sectors)
{
    const struct mbr_part *p = &parts[0];
    int i;

    if (p->type != 0x0b && p->type != 0x0c && p->type != MBR_TYPE_EFI_SYSTEM) {
        return -1;
    }
    if (p->start_lba == 0 || p->sector_count == 0) {
        return -1;
    }
    if (p->start_lba < MBR_GAP_END_LBA) {
        return -1;
    }
    if ((uint64_t)p->start_lba >= disk_sectors ||
        (uint64_t)p->sector_count > disk_sectors - p->start_lba) {
        return -1;
    }
    for (i = 1; i < MBR_PART_COUNT; i++) {
        if (parts[i].sector_count == 0) {
            continue;
        }
        if (ranges_overlap(p, &parts[i])) {
            return -1;
        }
    }
    return 0;
}

int mbr_bootpart_pick(const struct mbr_part parts[MBR_PART_COUNT], int krffs_index,
                      uint64_t disk_sectors)
{
    if (krffs_index < 0 || krffs_index >= MBR_PART_COUNT || krffs_index == 0) {
        return -1;
    }
    if (mbr_bootpart_ok(parts, disk_sectors) != 0) {
        return -1;
    }
    if (ranges_overlap(&parts[0], &parts[krffs_index])) {
        return -1;
    }
    return 0;
}
