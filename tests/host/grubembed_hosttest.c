/*
 * grubembed_hosttest.c: the BIOS boot partition that tools/image/grubembed.c
 * writes the GRUB core over, found in a GUID table it is given.
 *
 * host-deps: utils/grubslot.c
 *
 * The source is included so that the table's reader is called directly on
 * an image file. A table this project builds, its partition over sectors 34
 * to 2047, is taken. A table whose checksums match and whose partition
 * starts at sector 2, over its own entry array, is refused, and so are one
 * whose partition starts before the first usable sector and one whose
 * partition ends past the last: the core is written over the whole
 * partition, and each would put it over the table.
 */
#define main grubembed_main
#include "../../tools/image/grubembed.c"
#undef main

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

static unsigned char disk[64 * SECTOR];

/** Store `v` little-endian in the four bytes at `p`. */
static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/** Store `v` little-endian in the eight bytes at `p`. */
static void put64(unsigned char *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

/**
 * Build a table of 128 entries of 128 bytes whose first usable sector is
 * `first_usable` and whose BIOS boot partition is `start` to `last`, with
 * its checksums, in a temporary file of 64 sectors, and read it back through
 * grubembed's reader. Returns what `bios_boot_partition` returns, with
 * `first` and `count` as it set them; exits with status 2 when the file
 * cannot be made.
 */
static int find(uint64_t first_usable, uint64_t start, uint64_t last, unsigned long *first,
                unsigned long *count)
{
    unsigned char *h = disk + SECTOR, *e = disk + 2 * SECTOR;
    FILE *f;
    int rc;

    memset(disk, 0, sizeof(disk));
    memcpy(e, bios_boot, 16);
    put64(e + 32, start);
    put64(e + 40, last);
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x00010000u);
    put32(h + 12, 92);
    put64(h + 24, 1);
    put64(h + 32, 4095);
    put64(h + 40, first_usable);
    put64(h + 48, 4062);
    put64(h + 72, 2);
    put32(h + 80, 128);
    put32(h + 84, 128);
    put32(h + 88, crc32_of(e, 128 * 128));
    put32(h + 16, crc32_of(h, 92));
    f = tmpfile();
    if (f == NULL || fwrite(disk, 1, sizeof(disk), f) != sizeof(disk)) {
        printf("grubembed_hosttest: the image file could not be made\n");
        exit(2);
    }
    rc = bios_boot_partition(f, first, count);
    fclose(f);
    return rc;
}

/** Run the four rows of the opening comment. Returns 0 when every check
 *  passed and 1 otherwise. */
int main(void)
{
    unsigned long first = 0, count = 0;

    CHECK("a partition over sectors 34 to 2047 is taken",
          find(34, 34, 2047, &first, &count) == 0 && first == 34 && count == 2014);
    CHECK("one starting at sector 2, over the entry array, is refused",
          find(2, 2, 2047, &first, &count) == 1);
    CHECK("one before the first usable sector is refused", find(40, 34, 2047, &first, &count) == 1);
    CHECK("one past the last usable sector is refused", find(34, 34, 4063, &first, &count) == 1);

    if (fails == 0) {
        printf("grubembed_hosttest: PASS\n");
        return 0;
    }
    printf("grubembed_hosttest: %d failure(s)\n", fails);
    return 1;
}
