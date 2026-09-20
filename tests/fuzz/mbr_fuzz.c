/**
 * Fuzz harness for the MBR reader (sys/block/mbr.c): the input is a
 * sector, padded with zeros when shorter, and the table it parses is put
 * through every judgement the kernel makes of one.
 *
 * fuzz-deps:
 */
#include "block/mbr.c"
#include "../fuzz/fuzz.h"

int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
    uint8_t sector[512];
    struct mbr_part parts[MBR_PART_COUNT];
    struct mbr_range taken[MBR_PART_COUNT];
    uint64_t disk_sectors = 1u << 20;
    int refused = 0;
    unsigned i;

    for (i = 0; i < 512; i++) {
        sector[i] = i < size ? data[i] : 0;
    }
    if (mbr_parse(sector, parts) != 0) {
        return 0;
    }
    (void)mbr_take(parts, disk_sectors, taken, &refused);
    (void)mbr_bootpart_pick(parts, 1, disk_sectors);
    (void)mbr_bootpart_ok(parts, disk_sectors);
    (void)mbr_gap_clear(parts);
    return 0;
}
