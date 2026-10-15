/*
 * gpt_hosttest.c: the GUID partition table reader, rach2/sys/block/gpt.c,
 * against tables no disk would have.
 *
 * host-deps: rach2/sys/block/gpt.c
 *
 * A table gives the place of its own entry array, the number of entries and
 * the size of each, and a reader that used those numbers unchecked would
 * compute an offset the medium chose. A formatting tool writes a sound
 * table, so the rows build the tables no tool writes: an array changed past
 * its fourth entry, a count that overflows the arithmetic, an entry outside
 * the usable range, two entries that overlap, and a primary header read
 * from the backup's sector.
 *
 * The tables are built byte by byte, and the array is read once in a buffer
 * of its exact length under AddressSanitizer, so a read past its end is a
 * failure and not a byte of whatever follows.
 */

extern int printf(const char *fmt, ...);
extern void *malloc(unsigned long size);
extern void free(void *p);

#include "../../rach2/sys/block/gpt.h"

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

#define SECTOR       512u
#define DISK_SECTORS 131072u
#define ENTRIES      128u
#define ENTRY_SIZE   128u
#define ARRAY_BYTES  (ENTRIES * ENTRY_SIZE)

static unsigned char header[SECTOR];
static unsigned char array[ARRAY_BYTES];

/** Store `v` little-endian in the four bytes at `p`. */
static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/** Store `v` little-endian in the eight bytes at `p`. */
static void wr64(unsigned char *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

/** Write a used entry at `index` of `array`, covering sectors `first`
 *  through `last`, with a type GUID that is not zero and no attributes. */
static void put_entry(unsigned index, uint64_t first, uint64_t last)
{
    unsigned char *e = array + index * ENTRY_SIZE;
    int i;

    for (i = 0; i < 16; i++) {
        e[i] = (unsigned char)(0xA0 + i);
    }
    for (i = 0; i < 16; i++) {
        e[16 + i] = (unsigned char)(0x10 + i);
    }
    wr64(e + 32, first);
    wr64(e + 40, last);
    wr64(e + 48, 0);
}

/** Build in `header` a sound header of revision 1.0 for `array` as it
 *  stands: on sector `my_lba`, naming `alternate` as the other copy and
 *  `array_lba` as the array's first sector, with the usable range 34 through
 *  `DISK_SECTORS - 34` and both checksums set. */
static void build_header(uint64_t my_lba, uint64_t alternate, uint64_t array_lba)
{
    int i;

    for (i = 0; i < (int)SECTOR; i++) {
        header[i] = 0;
    }
    header[0] = 'E';
    header[1] = 'F';
    header[2] = 'I';
    header[3] = ' ';
    header[4] = 'P';
    header[5] = 'A';
    header[6] = 'R';
    header[7] = 'T';
    wr32(header + 8, 0x00010000u);
    wr32(header + 12, 92);
    wr64(header + 24, my_lba);
    wr64(header + 32, alternate);
    wr64(header + 40, 34);                 /* the first usable sector */
    wr64(header + 48, DISK_SECTORS - 34u); /* the last usable sector */
    wr64(header + 72, array_lba);
    wr32(header + 80, ENTRIES);
    wr32(header + 84, ENTRY_SIZE);
    wr32(header + 88, gpt_crc32(array, ARRAY_BYTES));
    wr32(header + 16, gpt_crc32(header, 92));
}

/** Zero the whole entry array, leaving every entry unused. */
static void clear_array(void)
{
    int i;

    for (i = 0; i < (int)ARRAY_BYTES; i++) {
        array[i] = 0;
    }
}

/**
 * Run the rows. Returns 0 when every check passed and 1 otherwise.
 *
 * - `gpt_protective` takes an MBR whose entry has type 0xEE, and refuses an
 *   ordinary MBR and a sector without the signature.
 * - A sound table whose used entries are at indexes six and seven, where a
 *   reader that took only the first four entries finds nothing: the header
 *   is taken, and both partitions are published in order.
 * - The checksum covers the whole array: a byte changed in the hundredth
 *   entry changes it, and leaves the checksum of the first four entries as
 *   it was, so a reader that hashed only those would take the table.
 * - `gpt_header_ok` refuses a header with a bad checksum; a primary header
 *   read from the backup's sector, which is how a copy of the primary left
 *   at the disk's end is told from a backup; an entry count that overflows
 *   the array's size; an entry size below the 128-byte minimum; a usable
 *   range past the device; and an array of 128 entries, 32 sectors, that
 *   starts eight sectors before the end. It takes a backup on its own
 *   sector, naming the primary as its alternate, and entries of 256 bytes.
 * - `gpt_parse_entries` passes over and counts an entry that overlaps one
 *   already taken, one before the first usable sector, one that ends before
 *   it begins, and one whose last sector is past 2^32, the reach of a SCSI
 *   ten-byte read; of six partitions it publishes four and counts two.
 * - An entry in the array's last slot, read from a buffer of the array's
 *   exact length, is published.
 */
int main(void)
{
    struct gpt_part parts[GPT_PART_COUNT];
    unsigned char mbr[SECTOR];
    int i;
    int refused;

    for (i = 0; i < (int)SECTOR; i++) {
        mbr[i] = 0;
    }
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    mbr[446 + 4] = 0xEE;
    CHECK("a protective MBR says a GUID table is behind it", gpt_protective(mbr));
    mbr[446 + 4] = 0x0C;
    CHECK("an ordinary MBR does not", !gpt_protective(mbr));
    mbr[446 + 4] = 0xEE;
    mbr[510] = 0;
    CHECK("and a sector with no signature is not a table at all", !gpt_protective(mbr));

    clear_array();
    put_entry(6, 2048, 67583);
    put_entry(7, 67584, 100351);
    build_header(1, DISK_SECTORS - 1u, 2);
    CHECK("a sound header is taken", gpt_header_ok(header, 1, DISK_SECTORS) == 0);
    CHECK("with its array where it says", gpt_array_lba(header) == 2);
    CHECK("and its entries counted", gpt_entry_count(header) == ENTRIES);
    CHECK("the array's checksum is the one the header carries",
          gpt_crc32(array, ARRAY_BYTES) == gpt_array_crc(header));
    refused = -1;
    CHECK("two partitions are published",
          gpt_parse_entries(array, ENTRIES, ENTRY_SIZE, gpt_first_usable(header),
                            gpt_last_usable(header), parts, &refused) == 2);
    CHECK("the first is the one at index six",
          parts[0].start_lba == 2048 && parts[0].sector_count == 65536);
    CHECK("the second follows it", parts[1].start_lba == 67584 && parts[1].sector_count == 32768);
    CHECK("and none was passed over", refused == 0);

    {
        uint32_t whole = gpt_crc32(array, ARRAY_BYTES);
        uint32_t first_four = gpt_crc32(array, 4u * ENTRY_SIZE);

        array[100 * ENTRY_SIZE + 33] ^= 0xFF;
        CHECK("a byte changed in the hundredth entry changes the array's checksum",
              gpt_crc32(array, ARRAY_BYTES) != whole);
        CHECK("and does not change the checksum of the first four",
              gpt_crc32(array, 4u * ENTRY_SIZE) == first_four);
        array[100 * ENTRY_SIZE + 33] ^= 0xFF;
    }

    build_header(1, DISK_SECTORS - 1u, 2);
    header[90] ^= 0xFF;
    CHECK("a header whose checksum does not cover it is refused",
          gpt_header_ok(header, 1, DISK_SECTORS) != 0);

    build_header(1, DISK_SECTORS - 1u, 2);
    CHECK("a primary header read from the backup's sector is refused",
          gpt_header_ok(header, DISK_SECTORS - 1u, DISK_SECTORS) != 0);
    build_header(DISK_SECTORS - 1u, 1, DISK_SECTORS - 33u);
    CHECK("and a backup header on its own sector is taken",
          gpt_header_ok(header, DISK_SECTORS - 1u, DISK_SECTORS) == 0);
    CHECK("with the primary named as its alternate", gpt_alternate_lba(header) == 1);

    build_header(1, DISK_SECTORS - 1u, 2);
    wr32(header + 80, 0xFFFFFFFFu);
    wr32(header + 16, 0);
    wr32(header + 16, gpt_crc32(header, 92));
    CHECK("a count that would overflow the array's size is refused",
          gpt_header_ok(header, 1, DISK_SECTORS) != 0);

    build_header(1, DISK_SECTORS - 1u, 2);
    wr32(header + 84, 64);
    wr32(header + 16, 0);
    wr32(header + 16, gpt_crc32(header, 92));
    CHECK("an entry size below the minimum is refused",
          gpt_header_ok(header, 1, DISK_SECTORS) != 0);

    build_header(1, DISK_SECTORS - 1u, 2);
    wr32(header + 84, 256);
    wr32(header + 80, 64);
    wr32(header + 16, 0);
    wr32(header + 16, gpt_crc32(header, 92));
    CHECK("an entry size above the minimum is taken", gpt_header_ok(header, 1, DISK_SECTORS) == 0);

    build_header(1, DISK_SECTORS - 1u, 2);
    wr64(header + 48, DISK_SECTORS + 100u);
    wr32(header + 16, 0);
    wr32(header + 16, gpt_crc32(header, 92));
    CHECK("a usable range that runs past the device is refused",
          gpt_header_ok(header, 1, DISK_SECTORS) != 0);

    build_header(1, DISK_SECTORS - 1u, 2);
    wr64(header + 72, DISK_SECTORS - 8u); /* 128 entries need 32 sectors */
    wr32(header + 16, 0);
    wr32(header + 16, gpt_crc32(header, 92));
    CHECK("an entry array that runs past the device is refused",
          gpt_header_ok(header, 1, DISK_SECTORS) != 0);

    clear_array();
    put_entry(0, 2048, 4095);
    put_entry(1, 3000, 5000);
    put_entry(2, 5001, 6000);
    build_header(1, DISK_SECTORS - 1u, 2);
    refused = -1;
    CHECK("an entry that overlaps one already taken is passed over",
          gpt_parse_entries(array, ENTRIES, ENTRY_SIZE, gpt_first_usable(header),
                            gpt_last_usable(header), parts, &refused) == 2);
    CHECK("and counted", refused == 1);
    CHECK("with the two that do not overlap published",
          parts[0].start_lba == 2048 && parts[1].start_lba == 5001);

    clear_array();
    put_entry(0, 10, 2000);
    put_entry(1, 2048, 4095);
    build_header(1, DISK_SECTORS - 1u, 2);
    refused = -1;
    CHECK("an entry outside the usable range is passed over",
          gpt_parse_entries(array, ENTRIES, ENTRY_SIZE, gpt_first_usable(header),
                            gpt_last_usable(header), parts, &refused) == 1);
    CHECK("and counted", refused == 1);

    clear_array();
    put_entry(0, 4000, 2048);
    build_header(1, DISK_SECTORS - 1u, 2);
    refused = -1;
    CHECK("an entry that ends before it begins is passed over",
          gpt_parse_entries(array, ENTRIES, ENTRY_SIZE, gpt_first_usable(header),
                            gpt_last_usable(header), parts, &refused) == 0);
    CHECK("and counted", refused == 1);

    clear_array();
    for (i = 0; i < 6; i++) {
        put_entry((unsigned)i, 2048u + (uint64_t)i * 100u, 2048u + (uint64_t)i * 100u + 99u);
    }
    build_header(1, DISK_SECTORS - 1u, 2);
    refused = -1;
    CHECK("a table with six partitions publishes four",
          gpt_parse_entries(array, ENTRIES, ENTRY_SIZE, gpt_first_usable(header),
                            gpt_last_usable(header), parts, &refused) == GPT_PART_COUNT);
    CHECK("and says how many it did not", refused == 2);

    clear_array();
    put_entry(0, 2048, 0x100000000ull);
    build_header(1, DISK_SECTORS - 1u, 2);
    refused = -1;
    CHECK("an entry past the reach of a ten-byte read is passed over",
          gpt_parse_entries(array, ENTRIES, ENTRY_SIZE, 34u, 0x200000000ull, parts, &refused) == 0);
    CHECK("and counted", refused == 1);

    {
        unsigned char *exact = (unsigned char *)malloc(ARRAY_BYTES);

        clear_array();
        put_entry(127, 2048, 4095);
        for (i = 0; i < (int)ARRAY_BYTES; i++) {
            exact[i] = array[i];
        }
        build_header(1, DISK_SECTORS - 1u, 2);
        CHECK("an entry in the last slot of the array is published",
              gpt_parse_entries(exact, ENTRIES, ENTRY_SIZE, gpt_first_usable(header),
                                gpt_last_usable(header), parts, &refused) == 1);
        free(exact);
    }

    if (fails == 0) {
        printf("gpt_hosttest: OK\n");
        return 0;
    }
    printf("gpt_hosttest: %d failure(s)\n", fails);
    return 1;
}
