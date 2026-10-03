/**
 * stickcheck: judge a stick image whole, by the kernel's own partition rules.
 *
 * Usage: `stickcheck IMAGE VOLUME-START VOLUME-SECTORS`, the two numbers in
 * sectors of 512 bytes. The image is the hybrid ISO followed by an exFAT
 * volume, and what is checked is the finished image and not its parts:
 *
 * * sector 0 carries boot code and the `0x55AA` signature;
 * * the table's entries, judged by `mbr_take`, which is the function the
 *   kernel publishes a stick's partitions with, give exactly three
 *   partitions and pass none over;
 * * the second is the volume, of type `0x07`, at `VOLUME-START` for
 *   `VOLUME-SECTORS`, and the first ends before it;
 * * the third is the ISO's EFI boot image, of type `0xEF`, between the
 *   first's end and the volume's start, which UEFI firmware boots a USB
 *   device from;
 * * the volume's boot sector is exFAT's, and its `PartitionOffset` and
 *   `VolumeLength` are the entry's start and length, which is what a stick
 *   formatted by any other system carries and what a reader that compares
 *   the two would refuse otherwise;
 * * the image ends where the volume ends.
 *
 * The volume's contents are walked by `mkexfatfixture --check` afterwards;
 * this program reads two sectors and the table, and nothing else.
 */
#include "block/mbr.h"

/* No hosted headers: the kernel's headers define the fixed-width types, and
   the host's <stdio.h> defines them again, differently. The few calls used
   are declared by hand, as the host tests declare them. */
extern int printf(const char *format, ...);
extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern long read(int fd, void *buffer, unsigned long count);
extern long long lseek(int fd, long long offset, int whence);
extern unsigned long long strtoull(const char *text, char **end, int base);

#define O_RDONLY 0
#define SEEK_SET 0
#define SEEK_END 2

#define SECTOR 512u

static int fail(const char *what)
{
    printf("stickcheck: %s\n", what);
    return 1;
}

static unsigned long long rd64(const unsigned char *p)
{
    unsigned long long v = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

/**
 * Whether the third entry is an EFI boot image, of type 0xEF, between the
 * first entry's end and the volume's start.
 */
static int efi_entry_ok(const struct mbr_part *parts, const struct mbr_range *taken,
                        unsigned long long start)
{
    return parts[2].type == 0xEF &&
           taken[2].start_lba >= taken[0].start_lba + taken[0].sector_count &&
           taken[2].start_lba + taken[2].sector_count <= start;
}

int main(int argc, char **argv)
{
    unsigned char table[SECTOR];
    unsigned char boot[SECTOR];
    struct mbr_part parts[MBR_PART_COUNT];
    struct mbr_range taken[MBR_PART_COUNT];
    unsigned long long start, length, image_sectors;
    long long size;
    int refused = -1;
    int count;
    int fd;

    if (argc != 4) {
        return fail("usage: stickcheck IMAGE VOLUME-START VOLUME-SECTORS");
    }
    start = strtoull(argv[2], 0, 10);
    length = strtoull(argv[3], 0, 10);
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        return fail("the image would not open");
    }
    size = lseek(fd, 0, SEEK_END);
    if (size <= 0 || (size % (long long)SECTOR) != 0) {
        return fail("the image is not a whole number of sectors");
    }
    image_sectors = (unsigned long long)size / SECTOR;
    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, table, SECTOR) != (long)SECTOR) {
        return fail("sector 0 would not read");
    }
    if (table[0] == 0 && table[1] == 0 && table[2] == 0) {
        return fail("sector 0 has no boot code");
    }
    if (mbr_parse(table, parts) != 0) {
        return fail("sector 0 has no partition table signature");
    }
    count = mbr_take(parts, image_sectors, taken, &refused);
    if (refused != 0) {
        return fail(
            "the kernel would pass an entry over: at sector zero, past the image, or overlapping");
    }
    if (count != 3) {
        return fail("the table does not give exactly three partitions");
    }
    /* `mbr_take` packs what it takes, so the three must be the first three
     * entries for the type read from an entry to be the range's own: with
     * three taken and the fourth empty, the third is not. */
    if (parts[0].type == 0 || parts[1].type == 0 || parts[3].type != 0) {
        return fail("the three partitions are not the table's first three entries");
    }
    if (parts[1].type != 0x07 || taken[1].start_lba != start || taken[1].sector_count != length) {
        return fail("the second entry is not the volume at the start and length given");
    }
    if (taken[0].start_lba + taken[0].sector_count > start) {
        return fail("the first entry runs into the volume");
    }
    if (!efi_entry_ok(parts, taken, start)) {
        return fail("the third entry is not an EFI boot image between the ISO and the volume");
    }
    if (start % 2048u != 0) {
        return fail("the volume does not start on a 1 MiB boundary");
    }
    if (start + length != image_sectors) {
        return fail("the image does not end where the volume ends");
    }
    if (lseek(fd, (long long)(start * SECTOR), SEEK_SET) != (long long)(start * SECTOR) ||
        read(fd, boot, SECTOR) != (long)SECTOR) {
        return fail("the volume's boot sector would not read");
    }
    {
        const char *name = "EXFAT   ";
        int i;

        for (i = 0; i < 8; i++) {
            if (boot[3 + i] != (unsigned char)name[i]) {
                return fail("the volume's boot sector is not exFAT's");
            }
        }
    }
    if (rd64(boot + 64) != start) {
        return fail("the volume's PartitionOffset is not its start on the image");
    }
    if (rd64(boot + 72) != length) {
        return fail("the volume's VolumeLength is not the entry's length");
    }
    close(fd);
    printf("stickcheck: %s: ok: iso at %llu for %llu, efi at %llu for %llu, volume at %llu for "
           "%llu, %llu sectors\n",
           argv[1], taken[0].start_lba, taken[0].sector_count, taken[2].start_lba,
           taken[2].sector_count, start, length, image_sectors);
    return 0;
}
