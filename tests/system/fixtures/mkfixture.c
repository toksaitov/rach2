/**
 * mkfixture: build the two disk fixtures the probe suites need.
 *
 * `mkfixture boot DISK` rewrites a mkdisk.sh disk into the bootprobe
 * fixture. mkdisk builds an MBR whose entry 1 is the KRFFS partition at
 * LBA 2048; this moves that entry to slot 2 and writes a FAT-typed (0x0c)
 * entry 1 after it at LBA 102400, 1024 sectors, filled with the pattern
 * bootprobe expects (byte j of partition sector k is (k * 7 + j) & 0xff).
 * The kernel's mount still finds KRFFS, since it probes entry contents
 * rather than order, and the MBR then qualifies for /dev/bootpart: a
 * FAT-typed entry 1, disjoint from KRFFS.
 *
 * That partition sat at LBA 64 until milestone 25, inside the 1 MiB
 * post-MBR gap where GRUB's core image lives. It was harmless while
 * nothing else reached those sectors and stopped being harmless when
 * /dev/grubcore was published over the gap, so the fixture moved into the
 * free space after the KRFFS partition instead.
 *
 * `mkfixture wide DISK` builds a sparse disk as large as the one milestone
 * 25 seals into the laptop. The raw disk windows seek and transfer through
 * a 64-bit offset, and nothing else in the tree is big enough to tell that
 * apart from the old 32-bit one: every test image is tens of megabytes.
 * Size alone is not enough either. A fixture of a few gigabytes would be
 * satisfied by an implementation keeping 33 bits of offset, so this one is
 * 260 GB, larger than the 250 GB drive being sealed: its far marker needs
 * 38 bits, and an offset truncated anywhere below that misses it. Two
 * 512-byte patterns are written, one just past 4 GiB and one near the end
 * (byte j of the marker at offset k is (j * 7 + 3 + k / GiB) & 0xff), plus
 * an MBR signature with no partitions, so discovery runs and publishes no
 * boot partition. The file is sparse, so all of it costs kilobytes.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR            512
#define BOOT_PART_LBA     102400
#define BOOT_PART_SECTORS 1024

#define GIB       ((uint64_t)1024 * 1024 * 1024)
#define WIDE_SIZE (260 * GIB)
#define WIDE_NEAR (5 * GIB)
#define WIDE_FAR  (258 * GIB)

static void w32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static uint32_t r32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int make_boot(const char *path)
{
    unsigned char mbr[SECTOR], sector[SECTOR];
    unsigned char *entry1 = mbr + 446, *entry2 = mbr + 462;
    FILE *f = fopen(path, "r+b");
    unsigned long k;
    int j;

    if (f == NULL || fread(mbr, 1, SECTOR, f) != SECTOR) {
        fprintf(stderr, "mkbootfixture: cannot read %s\n", path);
        return 1;
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        fprintf(stderr, "mkbootfixture: no MBR signature in %s\n", path);
        fclose(f);
        return 1;
    }
    if (r32(entry1 + 12) == 0) {
        fprintf(stderr, "mkbootfixture: entry 1 is empty; not a mkdisk "
                        "image\n");
        fclose(f);
        return 1;
    }
    /* Entry 2 takes the KRFFS partition entry as mkdisk wrote it; entry 1
       becomes the FAT-typed fixture partition. */
    memcpy(entry2, entry1, 16);
    memset(entry1, 0, 16);
    entry1[4] = 0x0C;
    w32(entry1 + 8, BOOT_PART_LBA);
    w32(entry1 + 12, BOOT_PART_SECTORS);
    if (fseek(f, 0, SEEK_SET) != 0 || fwrite(mbr, 1, SECTOR, f) != SECTOR) {
        fprintf(stderr, "mkbootfixture: cannot write the MBR\n");
        fclose(f);
        return 1;
    }
    if (fseek(f, (long)BOOT_PART_LBA * SECTOR, SEEK_SET) != 0) {
        fprintf(stderr, "mkbootfixture: cannot seek to the partition\n");
        fclose(f);
        return 1;
    }
    for (k = 0; k < BOOT_PART_SECTORS; k++) {
        for (j = 0; j < SECTOR; j++)
            sector[j] = (unsigned char)((k * 7 + (unsigned long)j) & 0xFF);
        if (fwrite(sector, 1, SECTOR, f) != SECTOR) {
            fprintf(stderr, "mkbootfixture: cannot write the pattern\n");
            fclose(f);
            return 1;
        }
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "mkbootfixture: cannot write %s\n", path);
        return 1;
    }
    printf("mkbootfixture: %s: FAT-typed partition 1 at LBA %d, %d sectors "
           "of pattern\n",
           path, BOOT_PART_LBA, BOOT_PART_SECTORS);
    return 0;
}

static int make_wide(const char *path)
{
    unsigned char marker[SECTOR];
    const uint64_t at[2] = {WIDE_NEAR, WIDE_FAR};
    FILE *f = fopen(path, "wb");
    int i, j;

    if (f == NULL) {
        fprintf(stderr, "mkwidefixture: cannot create %s\n", path);
        return 1;
    }
    /* The size comes from a seek past the end and one byte written there,
       which leaves the file sparse on every host this builds on. */
    if (fseeko(f, (off_t)WIDE_SIZE - 1, SEEK_SET) != 0 || fputc(0, f) == EOF) {
        fprintf(stderr, "mkwidefixture: cannot size %s\n", path);
        fclose(f);
        return 1;
    }
    for (i = 0; i < 2; i++) {
        unsigned long tag = (unsigned long)(at[i] / GIB);

        for (j = 0; j < SECTOR; j++)
            marker[j] = (unsigned char)(((unsigned long)j * 7 + 3 + tag) & 0xFF);
        if (fseeko(f, (off_t)at[i], SEEK_SET) != 0 || fwrite(marker, 1, SECTOR, f) != SECTOR) {
            fprintf(stderr, "mkwidefixture: cannot write a marker\n");
            fclose(f);
            return 1;
        }
    }
    if (fseeko(f, 510, SEEK_SET) != 0 || fputc(0x55, f) == EOF || fputc(0xAA, f) == EOF ||
        fclose(f) != 0) {
        fprintf(stderr, "mkwidefixture: cannot write %s\n", path);
        return 1;
    }
    printf("mkwidefixture: %s: %llu bytes, patterns at %llu and %llu\n", path,
           (unsigned long long)WIDE_SIZE, (unsigned long long)WIDE_NEAR,
           (unsigned long long)WIDE_FAR);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "boot") == 0)
        return make_boot(argv[2]);
    if (argc == 3 && strcmp(argv[1], "wide") == 0)
        return make_wide(argv[2]);
    fprintf(stderr, "usage: mkfixture {boot|wide} <disk-image>\n");
    return 2;
}
