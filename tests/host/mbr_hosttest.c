/**
 * Seam test for the MBR parse and the boot-partition judgement (sys/block/mbr.c).
 *
 * host-deps:
 *
 * The parser and the picker are pure, so every malformed table the kernel must
 * refuse is crafted here: a missing signature, empty and zero-start entries, a
 * wrong type byte, a range past the disk, a range whose 32-bit arithmetic would
 * wrap, and a boot partition overlapping the KRFFS one. The accepted case is
 * the GRUB disk's shape (FAT first, KRFFS second). Returns the failed-check
 * count (0 == pass).
 */

#include "block/mbr.c"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/** Write entry `i` of the table in `sector` as {type, start, count}. */
static void set_entry(uint8_t *sector, int i, uint8_t type, uint32_t start, uint32_t count)
{
    uint8_t *e = sector + 446 + i * 16;
    int b;

    for (b = 0; b < 16; b++) {
        e[b] = 0;
    }
    e[4] = type;
    for (b = 0; b < 4; b++) {
        e[8 + b] = (uint8_t)(start >> (8 * b));
        e[12 + b] = (uint8_t)(count >> (8 * b));
    }
}

int main(void)
{
    uint8_t sector[512];
    struct mbr_part parts[MBR_PART_COUNT];
    int i;

    for (i = 0; i < 512; i++) {
        sector[i] = 0;
    }

    /* No signature: the parse itself is refused. */
    CHECK(mbr_parse(sector, parts) == -1);

    sector[510] = 0x55;
    sector[511] = 0xAA;

    /* The GRUB disk's shape: FAT at 2048 for 32768, KRFFS at 34816. */
    set_entry(sector, 0, 0x0c, 2048, 32768);
    set_entry(sector, 1, 0x7f, 34816, 96256);
    CHECK(mbr_parse(sector, parts) == 0);
    CHECK(parts[0].type == 0x0c && parts[0].start_lba == 2048 && parts[0].sector_count == 32768);
    CHECK(parts[1].start_lba == 34816);
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == 0);
    parts[0].type = 0x0b; /* the other FAT32 type byte is accepted too */
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == 0);

    /* Rejections, each one flipped fact at a time from the accepted shape. */
    parts[0].type = 0x83;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == -1); /* not FAT-typed */
    parts[0].type = 0x0c;

    parts[0].start_lba = 0;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == -1); /* boot code lives at 0 */
    parts[0].start_lba = 2048;

    parts[0].sector_count = 0;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == -1); /* empty */
    parts[0].sector_count = 32768;

    CHECK(mbr_bootpart_pick(parts, 0, 131072) == -1); /* KRFFS is entry 0 */
    CHECK(mbr_bootpart_pick(parts, -1, 131072) == -1);
    CHECK(mbr_bootpart_pick(parts, 4, 131072) == -1);

    CHECK(mbr_bootpart_pick(parts, 1, 30000) == -1); /* runs past the disk */

    /* A count that would wrap 32-bit addition: subtraction must catch it. */
    parts[0].start_lba = 0xFFFFF000u;
    parts[0].sector_count = 0x2000u;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == -1);
    parts[0].start_lba = 2048;
    parts[0].sector_count = 32768;

    /* Partial overlap with the KRFFS range, from either side. */
    parts[1].start_lba = 30000;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == -1);
    parts[1].start_lba = 100;
    parts[1].sector_count = 4096;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == -1);
    parts[1].start_lba = 34816;
    parts[1].sector_count = 96256;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == 0);

    return fails;
}
