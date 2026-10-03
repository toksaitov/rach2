/*
 * gptdisk_hosttest.c: the user-space GUID table over an image file.
 *
 * host-deps: utils/gptdisk.c
 *
 * An installed disk is an image written to a disk larger than itself, which
 * leaves the table's backup where the image ended. The image here is a file
 * of 2048 sectors with a table built byte by byte, made 4096 sectors long
 * the way a larger disk is; the table is read from its primary, rewritten
 * with the backup in the last sectors, read again from that backup alone,
 * and grown into the space, and a partition that would lie under the backup
 * is refused. The identity's text form is checked both ways.
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gptdisk.h"

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

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

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
    if (fd < 0 || write(fd, image, sizeof(image)) != (ssize_t)sizeof(image) ||
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
