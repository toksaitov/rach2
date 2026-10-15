/**
 * mbr_hosttest.c: the MBR parse and the boot partition judgements of
 * rach2/sys/block/mbr.c, over tables built in a buffer.
 *
 * host-deps:
 *
 * The functions read no device, so every malformed table the kernel must
 * refuse is built here: a missing signature, empty and zero-start entries, a
 * wrong type byte, a range past the disk, a range whose 32-bit end would
 * wrap, and a boot partition overlapping the KRFFS one. The accepted case is
 * the GRUB disk's shape, FAT first and KRFFS second.
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

/* CHECK that also prints `what` when the check fails. */
#define CHECKN(cond, what)                  \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/** Write entry `i` of the table in `sector`: the type byte, the first sector
 *  and the sector count, little-endian at offsets 4, 8 and 12 of the 16-byte
 *  entry, and zeros in the rest. */
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

/**
 * Run the rows. Returns the count of failed checks, at most 255.
 *
 * `mbr_parse` refuses a sector without the 0x55AA signature and reads the
 * GRUB disk's shape: FAT at 2048 for 32768 sectors, KRFFS at 34816.
 * `mbr_bootpart_pick` takes that shape with entry 0 of type 0x0C, 0x0B or
 * 0xEF (an EFI system partition, as on a disk set up to boot by UEFI), and
 * then refuses it with one fact changed at a time: type 0x83; a start at
 * sector 0, which has the boot code; an empty entry; a KRFFS index of 0,
 * which is the boot partition's own, or outside the table; a disk too
 * short for the entry; a start and count whose 32-bit sum wraps, which the
 * check must catch by subtraction; and a KRFFS range overlapping entry 0
 * from either side.
 *
 * `mbr_bootpart_ok`, which a boot from a RAM image uses with no root to
 * compare against, takes a lone FAT entry 0 and one beside a root and a
 * third partition, even when the third overlaps the root and not entry 0.
 * It refuses an overlap with entry 0, a start inside the post-MBR gap, a
 * type that is not FAT, an empty entry, an entry past the disk's end, and a
 * count whose 32-bit end would wrap; an entry that ends at the disk's last
 * sector is taken.
 *
 * `mbr_gap_clear` refuses a partition inside LBA 1 through 2047 or on its
 * last sector, and takes one that starts at 2048. A table with a partition
 * in the gap describes a disk where writing GRUB destroys a file system;
 * entry 0 of such a table is still a boot partition by `mbr_bootpart_ok`,
 * which is why the two judgements are separate.
 *
 * `mbr_take` passes over an entry at sector 0, one past a 10000-sector
 * medium and one overlapping an entry already taken, takes the rest in
 * table order, and reports how many it passed over.
 */
int main(void)
{
    uint8_t sector[512];
    struct mbr_part parts[MBR_PART_COUNT];
    int i;

    for (i = 0; i < 512; i++) {
        sector[i] = 0;
    }

    CHECK(mbr_parse(sector, parts) == -1);

    sector[510] = 0x55;
    sector[511] = 0xAA;

    set_entry(sector, 0, 0x0c, 2048, 32768);
    set_entry(sector, 1, 0x7f, 34816, 96256);
    CHECK(mbr_parse(sector, parts) == 0);
    CHECK(parts[0].type == 0x0c && parts[0].start_lba == 2048 && parts[0].sector_count == 32768);
    CHECK(parts[1].start_lba == 34816);
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == 0);
    parts[0].type = 0x0b;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == 0);
    parts[0].type = 0xef;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == 0);

    parts[0].type = 0x83;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == -1);
    parts[0].type = 0x0c;

    parts[0].start_lba = 0;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == -1);
    parts[0].start_lba = 2048;

    parts[0].sector_count = 0;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == -1);
    parts[0].sector_count = 32768;

    CHECK(mbr_bootpart_pick(parts, 0, 131072) == -1);
    CHECK(mbr_bootpart_pick(parts, -1, 131072) == -1);
    CHECK(mbr_bootpart_pick(parts, 4, 131072) == -1);

    CHECK(mbr_bootpart_pick(parts, 1, 30000) == -1);

    parts[0].start_lba = 0xFFFFF000u;
    parts[0].sector_count = 0x2000u;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == -1);
    parts[0].start_lba = 2048;
    parts[0].sector_count = 32768;

    parts[1].start_lba = 30000;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == -1);
    parts[1].start_lba = 100;
    parts[1].sector_count = 4096;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == -1);
    parts[1].start_lba = 34816;
    parts[1].sector_count = 96256;
    CHECK(mbr_bootpart_pick(parts, 1, 131072) == 0);

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

        t[2].start_lba = 200000;
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

    {
        struct mbr_part four[MBR_PART_COUNT];
        struct mbr_range taken[MBR_PART_COUNT];
        int refused = -1;

        four[0].type = 0x0c;
        four[0].start_lba = 2048;
        four[0].sector_count = 4096;
        four[1].type = 0x07;
        four[1].start_lba = 0;
        four[1].sector_count = 100;
        four[2].type = 0x83;
        four[2].start_lba = 3000;
        four[2].sector_count = 100;
        four[3].type = 0x83;
        four[3].start_lba = 8000;
        four[3].sector_count = 4096;
        CHECK(mbr_take(four, 10000, taken, &refused) == 1);
        CHECK(taken[0].start_lba == 2048 && taken[0].sector_count == 4096);
        CHECK(refused == 3);
        four[0].type = 0;
        four[3].sector_count = 2000;
        CHECK(mbr_take(four, 10000, taken, &refused) == 2);
        CHECK(taken[0].start_lba == 3000 && taken[1].start_lba == 8000 && refused == 1);
    }

    return fails > 255 ? 255 : fails;
}
