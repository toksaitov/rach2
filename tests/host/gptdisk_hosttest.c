/*
 * gptdisk_hosttest.c: the user-space GUID table over an image file.
 *
 * An installed disk is an image written to a disk larger than itself, which
 * leaves the table's backup where the image ended. The image here is a file
 * of 2048 sectors with a table built byte by byte, made 4096 sectors long
 * the way a larger disk is; the table is read from its primary, rewritten
 * with the backup in the last sectors, read again from that backup alone,
 * and grown into the space, and a partition that would lie under the backup
 * is refused. The identity's text form is checked both ways.
 *
 * The source is included with its `write` counted, so that a write can be
 * stopped after any number of sectors' worth of calls: after every prefix of
 * a rewrite, from either copy, one copy must still read. A copy whose entry
 * size is a multiple of 8 and not of 128, which the kernel refuses, must not
 * be the one read.
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int writes_left = -1;

/** `write`, refused once `writes_left` calls have been allowed. */
static ssize_t counted_write(int fd, const void *buf, size_t n)
{
    if (writes_left == 0) {
        return -1;
    }
    if (writes_left > 0) {
        writes_left--;
    }
    return write(fd, buf, n);
}

#define write counted_write
#include "gptdisk.c"
#undef write

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

#define SECTOR      512u
#define IMAGE       2048u
#define DISK        4096u
#define ARRAY_BYTES 16384u
#define ARRAY_SECS  (ARRAY_BYTES / SECTOR)

static unsigned char image[IMAGE * SECTOR];

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** Entry `index` of the image's array: a type byte, a range and its GUID. */
static void entry(unsigned index, unsigned char type, uint64_t first, uint64_t last)
{
    unsigned char *e = image + 2 * SECTOR + index * 128;
    int i;

    for (i = 0; i < 16; i++) {
        e[i] = type;
        e[16 + i] = (unsigned char)(index + 1);
    }
    gptdisk_put64(e + 32, first);
    gptdisk_put64(e + 40, last);
}

/** The header at `lba` of the image, its array at `array`. */
static void header(uint64_t lba, uint64_t other, uint64_t array)
{
    unsigned char *h = image + lba * SECTOR;
    int i;

    memset(h, 0, SECTOR);
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x00010000u);
    put32(h + 12, 92);
    gptdisk_put64(h + 24, lba);
    gptdisk_put64(h + 32, other);
    gptdisk_put64(h + 40, 34);
    gptdisk_put64(h + 48, IMAGE - 34);
    for (i = 0; i < 16; i++) {
        h[56 + i] = (unsigned char)(0x10 + i);
    }
    gptdisk_put64(h + 72, array);
    put32(h + 80, 128);
    put32(h + 84, 128);
    put32(h + 88, gptdisk_crc32(image + 2 * SECTOR, ARRAY_BYTES));
    put32(h + 16, gptdisk_crc32(h, 92));
}

/**
 * Rewrite the table on `fd`, read from its backup when `from_backup` (its
 * primary header zeroed first), with entry 3 grown, and stop the writes
 * after each count of calls in turn: whatever the count, a copy must read,
 * since the old copy that checks is written last.
 */
static void check_prefixes(int fd, int from_backup)
{
    static unsigned char saved[DISK * SECTOR];
    static struct gptdisk g;
    unsigned char zero[SECTOR];
    const char *why = "";
    int stop, readable = 1, stopped = 0;

    memset(zero, 0, SECTOR);
    if (pread(fd, saved, sizeof(saved), 0) != (ssize_t)sizeof(saved)) {
        CHECK("the disk is saved", 0);
        return;
    }
    for (stop = 0; stop < 5; stop++) {
        if (pwrite(fd, saved, sizeof(saved), 0) != (ssize_t)sizeof(saved) ||
            (from_backup && pwrite(fd, zero, SECTOR, SECTOR) != SECTOR) ||
            gptdisk_read(fd, DISK, &g) != 1 || g.from_backup != from_backup) {
            CHECK("the disk is put back for a stopped rewrite", 0);
            break;
        }
        gptdisk_put64(gptdisk_entry(&g, 3) + 40, gptdisk_le64(gptdisk_entry(&g, 3) + 40) - 1);
        writes_left = stop;
        stopped += gptdisk_write(fd, &g, &why) != 0;
        writes_left = -1;
        if (gptdisk_read(fd, DISK, &g) != 1) {
            readable = 0;
        }
    }
    CHECK(from_backup ? "a rewrite from the backup is stopped after each of its writes"
                      : "a rewrite from the primary is stopped after each of its writes",
          stopped == 5);
    CHECK(from_backup ? "and a copy reads after each, the backup until the primary is whole"
                      : "and a copy reads after each, the primary until the backup is whole",
          readable);
    if (pwrite(fd, saved, sizeof(saved), 0) != (ssize_t)sizeof(saved)) {
        CHECK("the disk is put back", 0);
    }
}

/**
 * Give the primary of the table on `fd` 120 entries of 136 bytes with valid
 * checksums, the backup left at 128: the kernel refuses an entry size that is
 * not a multiple of 128, so the reader must take the backup.
 */
static void check_entry_size(int fd)
{
    static struct gptdisk g;
    unsigned char h[SECTOR];

    if (pread(fd, h, SECTOR, SECTOR) != SECTOR) {
        CHECK("the primary header reads", 0);
        return;
    }
    put32(h + 80, 120);
    put32(h + 84, 136);
    if (pread(fd, image, ARRAY_BYTES, 2 * SECTOR) != (ssize_t)ARRAY_BYTES) {
        CHECK("the primary array reads", 0);
        return;
    }
    put32(h + 88, gptdisk_crc32(image, 120 * 136));
    put32(h + 16, 0);
    put32(h + 16, gptdisk_crc32(h, 92));
    CHECK("a primary of 136-byte entries is written", pwrite(fd, h, SECTOR, SECTOR) == SECTOR);
    CHECK("and the table is read from its backup of 128-byte entries",
          gptdisk_read(fd, DISK, &g) == 1 && g.from_backup && g.entry_size == 128);
}

int main(void)
{
    static struct gptdisk g;
    char path[] = "/tmp/gptdisk-hosttest.XXXXXX";
    const char *why = "";
    unsigned char sector[SECTOR];
    unsigned char guid[16];
    char text[37];
    uint64_t backup_array = IMAGE - 1 - ARRAY_SECS;
    int fd;

    CHECK("the CRC32 of the check string",
          gptdisk_crc32((const unsigned char *)"123456789", 9) == 0xCBF43926u);

    image[446 + 4] = 0xEE;
    put32(image + 446 + 8, 1);
    put32(image + 446 + 12, IMAGE - 1);
    image[510] = 0x55;
    image[511] = 0xAA;
    entry(0, 0x28, 64, 127);
    entry(1, 0xA2, 128, 191);
    entry(2, 0xAF, 192, IMAGE - 40);
    header(1, IMAGE - 1, 2);
    memcpy(image + backup_array * SECTOR, image + 2 * SECTOR, ARRAY_BYTES);
    header(IMAGE - 1, 1, backup_array);

    fd = mkstemp(path);
    if (fd < 0 || pwrite(fd, image, sizeof(image), 0) != (ssize_t)sizeof(image) ||
        ftruncate(fd, (off_t)DISK * SECTOR) != 0) {
        printf("gptdisk_hosttest: the image file could not be made\n");
        return 2;
    }
    unlink(path);

    CHECK("the image on a larger disk reads", gptdisk_read(fd, DISK, &g) == 1);
    CHECK("from its primary", g.from_backup == 0 && g.entries == 128 && g.entry_size == 128);
    CHECK("with its backup short of the disk's end", !gptdisk_backup_at_end(&g));
    CHECK("entry 3 is the root's range", gptdisk_le64(gptdisk_entry(&g, 3) + 32) == 192);
    CHECK("entry 129 is past the array", gptdisk_entry(&g, 129) == NULL);
    CHECK("entry 4 is not used", !gptdisk_used(gptdisk_entry(&g, 4)));

    CHECK("the table is rewritten", gptdisk_write(fd, &g, &why) == 0);
    CHECK("with its backup at the end", gptdisk_backup_at_end(&g));
    CHECK("and the space before it usable", gptdisk_le64(g.header + 48) == DISK - 2 - ARRAY_SECS);
    CHECK("the protective entry covers the disk",
          pread(fd, sector, SECTOR, 0) == SECTOR && get32(sector + 446 + 12) == DISK - 1);
    CHECK("and the boot code around it is kept", sector[510] == 0x55 && sector[446 + 4] == 0xEE);

    memset(sector, 0, SECTOR);
    CHECK("the primary header is damaged", pwrite(fd, sector, SECTOR, SECTOR) == SECTOR);
    CHECK("the moved backup alone reads", gptdisk_read(fd, DISK, &g) == 1 && g.from_backup);
    CHECK("with the same disk GUID", g.header[56] == 0x10 && g.header[71] == 0x1F);
    CHECK("and the same entries", gptdisk_le64(gptdisk_entry(&g, 2) + 40) == 191);

    gptdisk_put64(gptdisk_entry(&g, 3) + 40, DISK - 2 - ARRAY_SECS);
    CHECK("the root grown to the backup is written", gptdisk_write(fd, &g, &why) == 0);
    CHECK("and reads from the repaired primary",
          gptdisk_read(fd, DISK, &g) == 1 && !g.from_backup &&
              gptdisk_le64(gptdisk_entry(&g, 3) + 40) == DISK - 2 - ARRAY_SECS);

    gptdisk_put64(gptdisk_entry(&g, 3) + 40, DISK - 2);
    CHECK("a partition under the backup is refused", gptdisk_write(fd, &g, &why) == -1);
    CHECK("by name", strstr(why, "past where the backup") != NULL);
    CHECK("and a disk larger than the file is refused", gptdisk_read(fd, DISK, &g) == 1 &&
                                                            (g.sectors = DISK + 1, 1) &&
                                                            gptdisk_write(fd, &g, &why) == -1);

    check_prefixes(fd, 0);
    check_prefixes(fd, 1);
    check_entry_size(fd);

    gptdisk_guid_text(g.header + 56, text);
    CHECK("the disk GUID's text", strcmp(text, "13121110-1514-1716-1819-1a1b1c1d1e1f") == 0);
    CHECK("parses back",
          gptdisk_guid_parse(text, guid) == 0 && memcmp(guid, g.header + 56, 16) == 0);
    CHECK("a short text is refused",
          gptdisk_guid_parse("13121110-1514-1716-1819-1a1b1c1d1e1", guid) == -1);
    CHECK("and a node name is a disk's number",
          gptdisk_number("/dev/disk12") == 12 && gptdisk_number("/dev/disk0p1") == -1);
    close(fd);

    if (fails == 0) {
        printf("gptdisk_hosttest: PASS\n");
        return 0;
    }
    printf("gptdisk_hosttest: %d failure(s)\n", fails);
    return 1;
}
