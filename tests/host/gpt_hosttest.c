/*
 * gpt_hosttest.c: the GUID partition table reader, against tables no disk
 * would carry.
 *
 * host-deps: rach2/sys/block/gpt.c
 *
 * A table describes where its own entry array is, how many entries it holds
 * and how large each one is, and a reader that believed those numbers would
 * compute an offset a stick chose. A real stick's table is sound, so the
 * cases that matter here are the ones no formatting tool produces: an entry
 * array whose CRC is wrong past the fourth entry, a count that overflows the
 * arithmetic, an entry outside the usable range, two entries that overlap,
 * and a backup header that disagrees with the primary.
 *
 * The tables are built here byte by byte and read in buffers of their exact
 * length under AddressSanitizer, so a read past the end is a failure and not
 * a byte of whatever follows.
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

static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void wr64(unsigned char *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

/** An entry of the usual kind at `index`, covering `[first, last]`. */
static void put_entry(unsigned index, uint64_t first, uint64_t last)
{
    unsigned char *e = array + index * ENTRY_SIZE;
    int i;

    for (i = 0; i < 16; i++) {
        e[i] = (unsigned char)(0xA0 + i); /* a type GUID that is not zero */
    }
    for (i = 0; i < 16; i++) {
        e[16 + i] = (unsigned char)(0x10 + i);
    }
    wr64(e + 32, first);
    wr64(e + 40, last);
    wr64(e + 48, 0);
}

/** Build a header for the array as it stands, on `my_lba`. */
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
    wr64(header + 48, DISK_SECTORS - 34u); /* and the last */
    wr64(header + 72, array_lba);
    wr32(header + 80, ENTRIES);
    wr32(header + 84, ENTRY_SIZE);
    wr32(header + 88, gpt_crc32(array, ARRAY_BYTES));
    wr32(header + 16, gpt_crc32(header, 92));
}

static void clear_array(void)
{
    int i;

    for (i = 0; i < (int)ARRAY_BYTES; i++) {
        array[i] = 0;
    }
}

int main(void)
{
    struct gpt_part parts[GPT_PART_COUNT];
    unsigned char mbr[SECTOR];
    int i;
    int refused;

    /* The protective MBR, and an ordinary one left alone. */
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

    /* A sound table with two partitions, the first used entry at index six,
       which is where a reader that took the first four entries of the array
       would find nothing. */
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

    /* The whole array is covered, so a broken entry past the fourth changes
       the checksum: a reader that hashed the first four entries would take
       this table for sound. */
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

    /* A header whose own checksum is wrong. */
    build_header(1, DISK_SECTORS - 1u, 2);
    header[90] ^= 0xFF;
    CHECK("a header whose checksum does not cover it is refused",
          gpt_header_ok(header, 1, DISK_SECTORS) != 0);

    /* A header read from the wrong sector, which is how a copy of the
       primary left at the end of the disk is told from a real backup. */
    build_header(1, DISK_SECTORS - 1u, 2);
    CHECK("a primary header read from the backup's sector is refused",
          gpt_header_ok(header, DISK_SECTORS - 1u, DISK_SECTORS) != 0);
    build_header(DISK_SECTORS - 1u, 1, DISK_SECTORS - 33u);
    CHECK("and a backup header on its own sector is taken",
          gpt_header_ok(header, DISK_SECTORS - 1u, DISK_SECTORS) == 0);
    CHECK("with the primary named as its alternate", gpt_alternate_lba(header) == 1);

    /* The numbers a reader would compute an offset from. */
    build_header(1, DISK_SECTORS - 1u, 2);
    wr32(header + 80, 0xFFFFFFFFu); /* an entry count that overflows */
    wr32(header + 16, 0);
    wr32(header + 16, gpt_crc32(header, 92));
    CHECK("a count that would overflow the array's size is refused",
          gpt_header_ok(header, 1, DISK_SECTORS) != 0);

    build_header(1, DISK_SECTORS - 1u, 2);
    wr32(header + 84, 64); /* an entry smaller than the standard's minimum */
    wr32(header + 16, 0);
    wr32(header + 16, gpt_crc32(header, 92));
    CHECK("an entry size below the minimum is refused",
          gpt_header_ok(header, 1, DISK_SECTORS) != 0);

    build_header(1, DISK_SECTORS - 1u, 2);
    wr32(header + 84, 256); /* one larger than the minimum is allowed */
    wr32(header + 80, 64);
    wr32(header + 16, 0);
    wr32(header + 16, gpt_crc32(header, 92));
    CHECK("an entry size above the minimum is taken", gpt_header_ok(header, 1, DISK_SECTORS) == 0);

    build_header(1, DISK_SECTORS - 1u, 2);
    wr64(header + 48, DISK_SECTORS + 100u); /* a usable range past the disk */
    wr32(header + 16, 0);
    wr32(header + 16, gpt_crc32(header, 92));
    CHECK("a usable range that runs past the device is refused",
          gpt_header_ok(header, 1, DISK_SECTORS) != 0);

    /* The array's first sector on the device and its last one past it. */
    build_header(1, DISK_SECTORS - 1u, 2);
    wr64(header + 72, DISK_SECTORS - 8u); /* 128 entries need 32 sectors */
    wr32(header + 16, 0);
    wr32(header + 16, gpt_crc32(header, 92));
    CHECK("an entry array that runs past the device is refused",
          gpt_header_ok(header, 1, DISK_SECTORS) != 0);

    /* Entries the array holds and this reader will not publish. */
    clear_array();
    put_entry(0, 2048, 4095);
    put_entry(1, 3000, 5000); /* overlapping the first */
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
    put_entry(0, 10, 2000); /* before the first usable sector */
    put_entry(1, 2048, 4095);
    build_header(1, DISK_SECTORS - 1u, 2);
    refused = -1;
    CHECK("an entry outside the usable range is passed over",
          gpt_parse_entries(array, ENTRIES, ENTRY_SIZE, gpt_first_usable(header),
                            gpt_last_usable(header), parts, &refused) == 1);
    CHECK("and counted", refused == 1);

    clear_array();
    put_entry(0, 4000, 2048); /* a last sector before its first */
    build_header(1, DISK_SECTORS - 1u, 2);
    refused = -1;
    CHECK("an entry that ends before it begins is passed over",
          gpt_parse_entries(array, ENTRIES, ENTRY_SIZE, gpt_first_usable(header),
                            gpt_last_usable(header), parts, &refused) == 0);
    CHECK("and counted", refused == 1);

    /* More partitions than this kernel publishes: the first four are taken
       and the rest are counted. */
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

    /* An entry past what a ten-byte read can address. */
    clear_array();
    put_entry(0, 2048, 0x100000000ull);
    build_header(1, DISK_SECTORS - 1u, 2);
    refused = -1;
    CHECK("an entry past the reach of a ten-byte read is passed over",
          gpt_parse_entries(array, ENTRIES, ENTRY_SIZE, 34u, 0x200000000ull, parts, &refused) == 0);
    CHECK("and counted", refused == 1);

    /* The array read in a buffer of its exact length, so that a reader that
       walked one entry too far is a failure here. */
    {
        unsigned char *exact = (unsigned char *)malloc(ARRAY_BYTES);

        clear_array();
        put_entry(127, 2048, 4095); /* the very last entry of the array */
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
