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

extern int printf(const char *fmt, ...);

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/* The named form, for cases where one failure among many needs telling apart. */
#define CHECKN(cond, what)                  \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
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

    /* ---- the index-free judgement a RAM-root boot uses ---- */
    {
        struct mbr_part t[MBR_PART_COUNT];
        int k;

        for (k = 0; k < MBR_PART_COUNT; k++) {
            t[k].type = 0;
            t[k].start_lba = 0;
            t[k].sector_count = 0;
        }
        t[0].type = 0x0c;
        t[0].start_lba = 2048;
        t[0].sector_count = 131072;
        CHECKN(mbr_bootpart_ok(t, 1290240) == 0,
               "a lone FAT boot partition qualifies with no root");

        t[1].type = 0x83;
        t[1].start_lba = 133120;
        t[1].sector_count = 1024000;
        CHECKN(mbr_bootpart_ok(t, 1290240) == 0,
               "a root partition beside it does not disqualify it");

        t[2].type = 0x0c;
        t[2].start_lba = 1157120;
        t[2].sector_count = 131072;
        CHECKN(mbr_bootpart_ok(t, 1290240) == 0, "nor does a third partition");

        t[2].start_lba = 200000; /* inside the root, clear of entry 0 */
        CHECKN(mbr_bootpart_ok(t, 1290240) == 0,
               "a third partition overlapping the root is not entry 0's business");
        t[2].start_lba = 2048 + 131071;
        CHECKN(mbr_bootpart_ok(t, 1290240) == -1,
               "a third partition overlapping entry 0 disqualifies it");
        t[2].start_lba = 1157120;

        t[0].start_lba = 2047;
        CHECKN(mbr_bootpart_ok(t, 1290240) == -1,
               "a boot partition reaching into the post-MBR gap is refused");
        t[0].start_lba = 1;
        CHECKN(mbr_bootpart_ok(t, 1290240) == -1, "and one starting at the gap's first sector too");
        t[0].start_lba = 2048;

        t[0].type = 0x83;
        CHECKN(mbr_bootpart_ok(t, 1290240) == -1, "a non-FAT type is refused");
        t[0].type = 0x0b;
        CHECKN(mbr_bootpart_ok(t, 1290240) == 0, "0x0b is the other accepted type");
        t[0].type = 0x0c;

        t[0].sector_count = 0;
        CHECKN(mbr_bootpart_ok(t, 1290240) == -1, "an empty entry 0 is refused");
        t[0].sector_count = 131072;

        CHECKN(mbr_bootpart_ok(t, 2048) == -1, "a partition starting at the disk's end is refused");
        CHECKN(mbr_bootpart_ok(t, 2048 + 131071) == -1,
               "a partition one sector past the disk's end is refused");
        CHECKN(mbr_bootpart_ok(t, 2048 + 131072) == 0, "one ending exactly at the end is accepted");

        t[0].sector_count = 0xFFFFFFFFu;
        CHECKN(mbr_bootpart_ok(t, 1290240) == -1,
               "a count that would wrap the 32-bit end is refused, not wrapped");
        t[0].sector_count = 131072;

        /* The boot loader's gap is nobody's partition. A table that puts one
           there describes a disk where writing GRUB would destroy a file
           system, and this project's own fixture did exactly that until
           milestone 25, so the case is real rather than theoretical. */
        CHECKN(mbr_gap_clear(t) == 0, "a table that leaves LBA 1 through 2047 alone is clear");
        t[3].type = 0x0c;
        t[3].start_lba = 64;
        t[3].sector_count = 1024;
        CHECKN(mbr_gap_clear(t) == -1, "a partition inside the gap is not clear");
        CHECKN(mbr_bootpart_ok(t, 1290240) == 0,
               "and entry 0 is still a fine boot partition, which is why the two are separate");
        t[3].start_lba = 2047;
        t[3].sector_count = 1;
        CHECKN(mbr_gap_clear(t) == -1, "a partition on the gap's last sector is not clear");
        t[3].start_lba = 2048;
        CHECKN(mbr_gap_clear(t) == 0, "one starting exactly after the gap is clear");
        t[3].start_lba = 0;
        t[3].sector_count = 0;
        t[3].type = 0;

        t[0].start_lba = 2048;
        t[0].sector_count = 131072;
    }

    return fails;
}
