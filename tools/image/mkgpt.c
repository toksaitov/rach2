/**
 * mkgpt: write a GUID partition table into a disk image file.
 *
 * Usage: `mkgpt [--seed N] [--damage-primary] DISK SPEC ...`, with one
 * SPEC of the form `START:SIZE:KIND[@INDEX][=LABEL]` per partition, at
 * most fifteen. START and SIZE are whole
 * sectors of 512 bytes, KIND is `fat` or `data` and selects the type GUID,
 * `@INDEX` puts the entry at that index of the array instead of the next
 * free one, which is how a table whose first used entry is not the first
 * entry is built, and `=LABEL` is the entry's name, `RACH2` and the entry's
 * position when absent. The GUIDs are made from the entry index and the
 * seed, so two disks built with one seed have the same identities, which
 * is what a test of a cloned disk wants, and two seeds give two sets.
 * `--damage-primary` writes the primary header with a wrong checksum after
 * the table is complete, leaving the backup as the table a reader finds.
 *
 * What is written: a protective MBR whose single entry is type 0xEE over
 * the whole medium, the primary header at sector 1 with the array at sector
 * 2, and the backup array and header at the end, which is where a table's
 * second copy belongs and what a checker looks for. Both headers carry the
 * CRC32 of the whole array and of themselves.
 *
 * The tree had no GUID writer before this. The kernel reads such tables
 * (`rach2/sys/block/gpt.c`) and the suite needs one to read, so this builds
 * it; nothing in the kernel is built from this file, and nothing here reads
 * a table back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR      512u
#define ENTRIES     128u
#define ENTRY_SIZE  128u
#define ARRAY_LBA   2u
#define ARRAY_BYTES (ENTRIES * ENTRY_SIZE)
#define ARRAY_SECS  (ARRAY_BYTES / SECTOR)
#define MAX_PARTS   15

/* The two type GUIDs this writes, in the mixed-endian form the standard
 * stores them in: the EFI system partition and a basic data partition,
 * which is what a FAT or exFAT volume on a stick carries. */
static const unsigned char type_fat[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                           0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
static const unsigned char type_data[16] = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                            0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};

static unsigned long crc32_of(const unsigned char *data, unsigned long length)
{
    unsigned long crc = 0xFFFFFFFFul;
    unsigned long i;

    for (i = 0; i < length; i++) {
        int bit;

        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            unsigned long mask = (unsigned long)0 - (crc & 1u);

            crc = (crc >> 1) ^ (0xEDB88320ul & mask);
        }
    }
    return (~crc) & 0xFFFFFFFFul;
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static void put64(unsigned char *p, unsigned long long v)
{
    put32(p, (unsigned long)(v & 0xFFFFFFFFull));
    put32(p + 4, (unsigned long)((v >> 32) & 0xFFFFFFFFull));
}

/* A partition GUID that differs per entry without a random source, so two
 * runs of this program on one image produce the same bytes. */
static unsigned int seed;

static void unique_guid(unsigned char *p, unsigned int index)
{
    int i;

    for (i = 0; i < 16; i++) {
        p[i] = (unsigned char)(0x40 + i + index * 16u + seed * 7u);
    }
}

static int fail(const char *what)
{
    fprintf(stderr, "mkgpt: %s\n", what);
    return 1;
}

int main(int argc, char **argv)
{
    unsigned char array[ARRAY_BYTES];
    unsigned char header[SECTOR];
    unsigned char mbr[SECTOR];
    unsigned long long sectors;
    unsigned long long first_usable;
    unsigned long long last_usable;
    unsigned long long backup_array;
    unsigned long array_crc;
    FILE *f;
    long size;
    int used = 0;
    int damage = 0;
    int i;

    while (argc >= 2 && argv[1][0] == '-') {
        if (strcmp(argv[1], "--seed") == 0 && argc >= 3) {
            seed = (unsigned int)strtoul(argv[2], 0, 10);
            argv += 2;
            argc -= 2;
        } else if (strcmp(argv[1], "--damage-primary") == 0) {
            damage = 1;
            argv++;
            argc--;
        } else {
            return fail("usage: mkgpt [--seed N] [--damage-primary] DISK "
                        "START:SIZE:KIND[@INDEX][=LABEL] ...");
        }
    }
    if (argc < 3) {
        return fail("usage: mkgpt [--seed N] [--damage-primary] DISK "
                    "START:SIZE:KIND[@INDEX][=LABEL] ...");
    }
    f = fopen(argv[1], "r+b");
    if (!f) {
        return fail("the image would not open");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        return fail("the image would not seek");
    }
    size = ftell(f);
    if (size <= 0 || (size % (long)SECTOR) != 0) {
        return fail("the image is not a whole number of sectors");
    }
    sectors = (unsigned long long)size / SECTOR;
    if (sectors < 2u * ARRAY_SECS + 4u) {
        return fail("the image is too small for a table and its backup");
    }
    backup_array = sectors - 1u - ARRAY_SECS;
    first_usable = ARRAY_LBA + ARRAY_SECS;
    last_usable = backup_array - 1u;

    memset(array, 0, sizeof(array));
    for (i = 2; i < argc; i++) {
        unsigned long long start;
        unsigned long long count;
        char kind[16];
        char spec[128];
        char *label = 0;
        unsigned int at;
        int slot;
        int fields;
        unsigned char *e;

        if (strlen(argv[i]) >= sizeof(spec)) {
            return fail("a partition argument is too long");
        }
        strcpy(spec, argv[i]);
        label = strchr(spec, '=');
        if (label) {
            *label++ = '\0';
            if (strlen(label) > 36) {
                return fail("a label is longer than 36 characters");
            }
        }
        fields = sscanf(spec, "%llu:%llu:%15[^@]@%u", &start, &count, kind, &at);
        if (fields == 3) {
            at = (unsigned int)used;
        } else if (fields != 4) {
            return fail("a partition is not START:SIZE:KIND[@INDEX][=LABEL]");
        }
        if (at >= ENTRIES) {
            return fail("an entry index is past the array");
        }
        if (used >= MAX_PARTS) {
            return fail("more partitions than this writes");
        }
        if (count == 0 || start < first_usable || start + count - 1u > last_usable) {
            return fail("a partition is outside the usable range");
        }
        slot = (int)at;
        e = array + (unsigned long)slot * ENTRY_SIZE;
        if (e[0] != 0 || e[1] != 0) {
            return fail("two partitions were given the same entry");
        }
        if (strcmp(kind, "fat") == 0) {
            memcpy(e, type_fat, 16);
        } else if (strcmp(kind, "data") == 0) {
            memcpy(e, type_data, 16);
        } else {
            return fail("a partition's kind is neither fat nor data");
        }
        unique_guid(e + 16, at);
        put64(e + 32, start);
        put64(e + 40, start + count - 1u);
        /* The name, in UTF-16: "RACH2 N". */
        if (label) {
            int k;

            for (k = 0; label[k]; k++) {
                e[56 + k * 2] = (unsigned char)label[k];
            }
        } else {
            e[56] = 'R';
            e[58] = 'A';
            e[60] = 'C';
            e[62] = 'H';
            e[64] = '2';
            e[66] = (unsigned char)('0' + used);
        }
        used++;
    }
    if (used == 0) {
        return fail("no partitions were given");
    }
    array_crc = crc32_of(array, ARRAY_BYTES);

    /* The protective MBR: one entry of type 0xEE over everything past
     * sector zero, with the CHS fields saturated as every writer sets
     * them. */
    memset(mbr, 0, sizeof(mbr));
    mbr[446 + 1] = 0x00;
    mbr[446 + 2] = 0x02;
    mbr[446 + 3] = 0x00;
    mbr[446 + 4] = 0xEE;
    mbr[446 + 5] = 0xFF;
    mbr[446 + 6] = 0xFF;
    mbr[446 + 7] = 0xFF;
    put32(mbr + 446 + 8, 1u);
    put32(mbr + 446 + 12,
          sectors - 1u > 0xFFFFFFFFull ? 0xFFFFFFFFul : (unsigned long)(sectors - 1u));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    for (i = 0; i < 2; i++) {
        int primary = i == 0;

        memset(header, 0, sizeof(header));
        memcpy(header, "EFI PART", 8);
        put32(header + 8, 0x00010000ul);
        put32(header + 12, 92u);
        put64(header + 24, primary ? 1ull : sectors - 1u);
        put64(header + 32, primary ? sectors - 1u : 1ull);
        put64(header + 40, first_usable);
        put64(header + 48, last_usable);
        unique_guid(header + 56, 99u);
        put64(header + 72, primary ? (unsigned long long)ARRAY_LBA : backup_array);
        put32(header + 80, ENTRIES);
        put32(header + 84, ENTRY_SIZE);
        put32(header + 88, array_crc);
        put32(header + 16, crc32_of(header, 92u));
        if (primary && damage) {
            header[20] ^= 0xFF; /* the checksum no longer covers the header */
        }
        if (fseek(f, (long)((primary ? 1u : sectors - 1u) * SECTOR), SEEK_SET) != 0 ||
            fwrite(header, 1, SECTOR, f) != SECTOR) {
            return fail("a header would not be written");
        }
        if (fseek(f, (long)((primary ? (unsigned long long)ARRAY_LBA : backup_array) * SECTOR),
                  SEEK_SET) != 0 ||
            fwrite(array, 1, ARRAY_BYTES, f) != ARRAY_BYTES) {
            return fail("an entry array would not be written");
        }
    }
    if (fseek(f, 0, SEEK_SET) != 0 || fwrite(mbr, 1, SECTOR, f) != SECTOR) {
        return fail("the protective MBR would not be written");
    }
    if (fclose(f) != 0) {
        return fail("the image would not close");
    }
    for (i = 0; i < (int)ENTRIES; i++) {
        const unsigned char *e = array + (unsigned long)i * ENTRY_SIZE;
        /* The text form: the first three fields little-endian on the disk,
           printed most significant byte first, the last two as stored. */
        static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
        int k;

        if (e[0] == 0 && e[1] == 0) {
            continue;
        }
        printf("mkgpt: entry %d guid ", i + 1);
        for (k = 0; k < 16; k++) {
            if (k == 4 || k == 6 || k == 8 || k == 10) {
                printf("-");
            }
            printf("%02x", e[16 + order[k]]);
        }
        printf("\n");
    }
    printf("mkgpt: %s: %d partitions, %llu sectors\n", argv[1], used, sectors);
    return 0;
}
