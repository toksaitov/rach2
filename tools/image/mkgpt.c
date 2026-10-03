/**
 * mkgpt: write a GUID partition table into a disk image file.
 *
 * Usage: `mkgpt [--seed N] [--guid GUID] [--damage-primary] DISK SPEC ...`,
 * with one SPEC of the form `START:SIZE:KIND[@INDEX][=LABEL]` per partition,
 * at most fifteen. START and SIZE are whole sectors of 512 bytes. KIND selects
 * the type GUID: `fat` an EFI system partition, `data` a basic data
 * partition, which is what a FAT or exFAT volume on a stick carries, `linux`
 * a Linux file system, and `bios` a BIOS boot partition, which holds GRUB's
 * BIOS core on a disk whose sectors after the MBR are the table's. `@INDEX`
 * puts the entry at that index of the array instead of the next free one,
 * which is how a table whose first used entry is not the first entry is
 * built, and `=LABEL` is the entry's name, `RACH2` and the entry's position
 * when absent. The partition GUIDs are made from the entry index and the
 * seed, so two disks built with one seed have the same identities, which
 * is what a test of a cloned disk wants, and two seeds give two sets. The
 * disk's GUID is made from the seed too, unless `--guid` gives it in the
 * 36-character text form, which is how a disk image is given the identity
 * its boot menu names. `--damage-primary` writes the primary header with a
 * wrong checksum after the table is complete, leaving the backup as the
 * table a reader finds.
 *
 * What is written: a protective MBR whose single entry is type 0xEE over
 * the whole medium, the primary header at sector 1 with the array at sector
 * 2, and the backup array and header at the end, which is where a table's
 * second copy belongs and what a checker looks for. Both headers carry the
 * CRC32 of the whole array and of themselves. The MBR's boot code is zeros;
 * `grubembed` puts GRUB's there afterwards.
 *
 * `mkgpt --list DISK` reads instead of writing: one line per used entry,
 * `INDEX KIND START SIZE`, with the index counted from 1, the kind by the
 * names above or `other`, and the extent in bytes, as `mkmbr --list` prints
 * an MBR's, from the primary table when its header and array check and from
 * the backup at the image's last sector otherwise.
 *
 * The kernel reads such tables (`rach2/sys/block/gpt.c`), the suites need
 * them to read, and the disk builder lays out an installed disk with one.
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

/* The type GUIDs this writes, in the mixed-endian form the standard stores
 * them in, by the name a SPEC gives them. */
static const struct kind {
    const char *name;
    unsigned char guid[16];
} kinds[] = {
    /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B, the EFI system partition */
    {"fat",
     {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9,
      0x3b}},
    /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, basic data */
    {"data",
     {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99,
      0xc7}},
    /* 0FC63DAF-8483-4772-8E79-3D69D8477DE4, a Linux file system */
    {"linux",
     {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d,
      0xe4}},
    /* 21686148-6449-6E6F-744E-656564454649, BIOS boot */
    {"bios",
     {0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6f, 0x6e, 0x74, 0x4e, 0x65, 0x65, 0x64, 0x45, 0x46,
      0x49}},
};

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

    /* The index and the seed go into different bytes, so that no pair of
     * them meets in one identity: with both folded into every byte, seed
     * 0 at index 7 and seed 16 at index 0 gave the same sixteen bytes. */
    for (i = 0; i < 16; i++) {
        p[i] = (unsigned char)(0x40 + i + (i < 8 ? index : seed));
    }
}

/* The text form of a GUID: the first three fields are little-endian on the
   disk and printed most significant byte first, the last two as stored. */
static const int text_order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};

/** The value of hexadecimal digit `c`, or -1. */
static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/** Parse the 36-character text form into `p`; 0 on success, -1 otherwise. */
static int parse_guid(const char *text, unsigned char *p)
{
    int i, n = 0;

    for (i = 0; i < 16; i++) {
        int hi, lo;

        if (i == 4 || i == 6 || i == 8 || i == 10) {
            if (text[n++] != '-') {
                return -1;
            }
        }
        hi = hex_digit((unsigned char)text[n]);
        lo = hi < 0 ? -1 : hex_digit((unsigned char)text[n + 1]);
        if (lo < 0) {
            return -1;
        }
        p[text_order[i]] = (unsigned char)(hi << 4 | lo);
        n += 2;
    }
    return text[n] == '\0' ? 0 : -1;
}

static int fail(const char *what)
{
    fprintf(stderr, "mkgpt: %s\n", what);
    return 1;
}

static unsigned long get32(const unsigned char *p)
{
    return (unsigned long)p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 |
           (unsigned long)p[3] << 24;
}

static unsigned long long get64(const unsigned char *p)
{
    return (unsigned long long)get32(p) | (unsigned long long)get32(p + 4) << 32;
}

/**
 * Read the header at sector `lba` of `f` into `header` and its array into
 * `array`, and check both checksums. Returns 0, or -1.
 */
static int read_table(FILE *f, unsigned long long lba, unsigned char *header, unsigned char *array)
{
    unsigned char copy[SECTOR];
    unsigned long size;

    if (fseek(f, (long)(lba * SECTOR), SEEK_SET) != 0 || fread(header, 1, SECTOR, f) != SECTOR ||
        memcmp(header, "EFI PART", 8) != 0 || get32(header + 80) != ENTRIES ||
        get32(header + 84) != ENTRY_SIZE) {
        return -1;
    }
    size = get32(header + 12);
    if (size < 92 || size > SECTOR) {
        return -1;
    }
    memcpy(copy, header, SECTOR);
    memset(copy + 16, 0, 4);
    if (crc32_of(copy, size) != get32(header + 16) ||
        fseek(f, (long)(get64(header + 72) * SECTOR), SEEK_SET) != 0 ||
        fread(array, 1, ARRAY_BYTES, f) != ARRAY_BYTES ||
        crc32_of(array, ARRAY_BYTES) != get32(header + 88)) {
        return -1;
    }
    return 0;
}

/** Print the used entries of the table on `path`, as the usage says. */
static int list_table(const char *path)
{
    static unsigned char array[ARRAY_BYTES];
    unsigned char header[SECTOR];
    FILE *f = fopen(path, "rb");
    long size;
    unsigned int i;

    if (!f || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < (long)(3 * SECTOR)) {
        return fail("the image would not open or is too small for a table");
    }
    if (read_table(f, 1, header, array) != 0 &&
        read_table(f, (unsigned long long)size / SECTOR - 1, header, array) != 0) {
        fclose(f);
        return fail("neither the primary nor the backup table checks");
    }
    fclose(f);
    for (i = 0; i < ENTRIES; i++) {
        const unsigned char *e = array + (unsigned long)i * ENTRY_SIZE;
        const char *name = "other";
        size_t k;

        for (k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
            if (memcmp(e, kinds[k].guid, 16) == 0) {
                name = kinds[k].name;
            }
        }
        for (k = 0; k < 16 && e[k] == 0; k++) {
        }
        if (k == 16) {
            continue;
        }
        printf("%u %s %llu %llu\n", i + 1, name, get64(e + 32) * SECTOR,
               (get64(e + 40) - get64(e + 32) + 1) * SECTOR);
    }
    return 0;
}

/**
 * Put the partition one SPEC argument describes into `array` as the entry
 * after the `used` already there, or at the index the SPEC names, inside
 * the usable range `[first_usable, last_usable]`. Returns 0, or 1 having
 * said why not.
 */
static int put_entry(unsigned char *array, const char *arg, int used,
                     unsigned long long first_usable, unsigned long long last_usable)
{
    unsigned long long start;
    unsigned long long count;
    char kind[16];
    char spec[128];
    char *label = 0;
    unsigned int at;
    int slot;
    int fields;
    unsigned char *e;
    size_t k;

    if (strlen(arg) >= sizeof(spec)) {
        return fail("a partition argument is too long");
    }
    strcpy(spec, arg);
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
    for (k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        if (strcmp(kind, kinds[k].name) == 0) {
            memcpy(e, kinds[k].guid, 16);
            break;
        }
    }
    if (k == sizeof(kinds) / sizeof(kinds[0])) {
        return fail("a partition's kind is not fat, data, linux or bios");
    }
    unique_guid(e + 16, at);
    put64(e + 32, start);
    put64(e + 40, start + count - 1u);
    /* The name, in UTF-16: "RACH2 N". */
    if (label) {
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
    return 0;
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
    unsigned char disk_guid[16];
    int given_guid = 0;
    int used = 0;
    int damage = 0;
    int i;

    if (argc == 3 && strcmp(argv[1], "--list") == 0) {
        return list_table(argv[2]);
    }
    while (argc >= 2 && argv[1][0] == '-') {
        if (strcmp(argv[1], "--seed") == 0 && argc >= 3) {
            seed = (unsigned int)strtoul(argv[2], 0, 10);
            argv += 2;
            argc -= 2;
        } else if (strcmp(argv[1], "--guid") == 0 && argc >= 3) {
            if (parse_guid(argv[2], disk_guid) != 0) {
                return fail("the disk GUID is not in the 36-character text form");
            }
            given_guid = 1;
            argv += 2;
            argc -= 2;
        } else if (strcmp(argv[1], "--damage-primary") == 0) {
            damage = 1;
            argv++;
            argc--;
        } else {
            return fail("usage: mkgpt [--seed N] [--guid GUID] [--damage-primary] DISK "
                        "START:SIZE:KIND[@INDEX][=LABEL] ...");
        }
    }
    if (argc < 3) {
        return fail("usage: mkgpt [--seed N] [--guid GUID] [--damage-primary] DISK "
                    "START:SIZE:KIND[@INDEX][=LABEL] ...");
    }
    if (!given_guid) {
        unique_guid(disk_guid, 99u);
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
        if (put_entry(array, argv[i], used, first_usable, last_usable) != 0) {
            return 1;
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
        memcpy(header + 56, disk_guid, 16);
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
        int k;

        if (e[0] == 0 && e[1] == 0) {
            continue;
        }
        printf("mkgpt: entry %d guid ", i + 1);
        for (k = 0; k < 16; k++) {
            if (k == 4 || k == 6 || k == 8 || k == 10) {
                printf("-");
            }
            printf("%02x", e[16 + text_order[k]]);
        }
        printf("\n");
    }
    printf("mkgpt: %s: %d partitions, %llu sectors\n", argv[1], used, sectors);
    return 0;
}
