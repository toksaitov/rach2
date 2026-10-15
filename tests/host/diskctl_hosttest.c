/*
 * diskctl_hosttest.c: the `esp` and `shrink` commands of utils/diskctl.c
 * over image files, with the kernel's table reread answered by the test.
 *
 * host-deps: utils/gptdisk.c
 *
 * The source is included with `ioctl` replaced. A reread refused with EBUSY,
 * as the kernel refuses one while another descriptor is open on the disk,
 * comes after the type is written, so the disk has 0xEF and the kernel's
 * table does not. The next run must read the table again although the disk
 * has the type already, and a run on a disk the kernel has read must read it
 * again too, since nothing on the disk says which of the two it is.
 *
 * `shrink` runs on `/dev/disk0`, which the test opens as a GUID table in an
 * image file of 4096 sectors, the size the test gives for disk 0 in place
 * of /proc/disk. Entry 3 shrunk must end where it was told in both copies,
 * the other entries and the backup's place unchanged; a length past the
 * entry's, past the last usable sector or into the next entry, a length of
 * zero or with other characters, an unused entry and an MBR disk must be
 * refused with nothing written and nothing reread.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int busy, rereads;
static char image_path[] = "/tmp/diskctl-hosttest-gpt.XXXXXX";

#define DISK 4096u /* the sectors of disk 0, the GUID table's image */

/** Open `path`, with `/dev/disk0` standing for the GUID table's image. */
static int image_open(const char *path, int flags)
{
    return open(strcmp(path, "/dev/disk0") == 0 ? image_path : path, flags);
}

/** The size of disk `number` in place of its /proc/disk line: DISK
 *  sectors for disk 0, and -1 for any other. */
int fake_sectors(int number, uint64_t *sectors)
{
    if (number != 0) {
        return -1;
    }
    *sectors = DISK;
    return 0;
}

/** The kernel's BLKRRPART (0x125F): refused with EBUSY while `busy`, else
 *  counted in `rereads`. Every other request fails with ENOTTY. */
static int counted_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd;
    (void)arg;
    if (request != 0x125Fu) {
        errno = ENOTTY;
        return -1;
    }
    if (busy) {
        errno = EBUSY;
        return -1;
    }
    rereads++;
    return 0;
}

#define ioctl           counted_ioctl
#define main            diskctl_main
#define open            image_open
#define gptdisk_sectors fake_sectors
#include "diskctl.c"
#undef gptdisk_sectors
#undef open
#undef main
#undef ioctl

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

/** The type byte of entry 1 of the image open on `fd`, or -1 when sector 0
 *  does not read. */
static int entry_type(int fd)
{
    unsigned char sector[512];

    if (pread(fd, sector, sizeof(sector), 0) != (ssize_t)sizeof(sector)) {
        return -1;
    }
    return sector[446 + 4];
}

/**
 * Write a GUID table over all of the image open on `fd` with gptdisk_write:
 * entry 1 from sector 40 to 999, entry 2 from 1000 to 1999, entry 3 from
 * 2000 to the last usable sector, 4062, and entry 4, the BIOS boot
 * partition's place, from 34 to 39. Returns 0, or -1.
 */
static int make_gpt(int fd)
{
    static struct gptdisk g;
    static const uint64_t first[4] = {40, 1000, 2000, 34}, last[4] = {999, 1999, 4062, 39};
    const char *why = "";
    int i, k;

    memset(&g, 0, sizeof(g));
    g.sectors = DISK;
    g.entries = 128;
    g.entry_size = 128;
    g.mbr[446 + 4] = 0xEE;
    g.mbr[510] = 0x55;
    g.mbr[511] = 0xAA;
    memcpy(g.header, "EFI PART", 8);
    g.header[10] = 1;
    g.header[12] = 92;
    gptdisk_put64(g.header + 40, 34);
    for (i = 0; i < 16; i++) {
        g.header[56 + i] = (unsigned char)(0x10 + i);
    }
    g.header[80] = 128;
    g.header[84] = 128;
    for (i = 0; i < 4; i++) {
        unsigned char *e = gptdisk_entry(&g, i + 1);

        for (k = 0; k < 16; k++) {
            e[k] = (unsigned char)(0xA0 + i);
            e[16 + k] = (unsigned char)(i + 1);
        }
        gptdisk_put64(e + 32, first[i]);
        gptdisk_put64(e + 40, last[i]);
    }
    if (ftruncate(fd, 0) != 0 || ftruncate(fd, (off_t)DISK * 512) != 0 ||
        gptdisk_write(fd, &g, &why) != 0) {
        return -1;
    }
    return 0;
}

/** The last sector of entry `number` of the table on `fd`, read from the
 *  backup alone when `backup`, or 0 when the table does not read. */
static uint64_t entry_end(int fd, int number, int backup)
{
    static struct gptdisk g;
    unsigned char saved[512], zero[512];
    uint64_t end = 0;

    memset(zero, 0, sizeof(zero));
    if (pread(fd, saved, sizeof(saved), 512) != (ssize_t)sizeof(saved) ||
        (backup && pwrite(fd, zero, sizeof(zero), 512) != (ssize_t)sizeof(zero))) {
        return 0;
    }
    if (gptdisk_read(fd, DISK, &g) == 1 && g.from_backup == backup &&
        gptdisk_entry(&g, number) != NULL) {
        end = gptdisk_le64(gptdisk_entry(&g, number) + 40);
        if (!backup && !gptdisk_backup_at_end(&g)) {
            end = 0;
        }
    }
    if (backup && pwrite(fd, saved, sizeof(saved), 512) != (ssize_t)sizeof(saved)) {
        return 0;
    }
    return end;
}

/** Whether the image open on `fd` is byte for byte the copy `before`. */
static int unchanged(int fd, const unsigned char *before)
{
    static unsigned char now[DISK * 512];

    return pread(fd, now, sizeof(now), 0) == (ssize_t)sizeof(now) &&
           memcmp(now, before, sizeof(now)) == 0;
}

/** Run `diskctl shrink /dev/disk0 ENTRY SECTORS` on a fresh table, which must
 *  be refused with the image unchanged and no reread. */
static void refused(int fd, const char *what, char *entry, char *sectors)
{
    static unsigned char before[DISK * 512];
    char *argv[] = {"diskctl", "shrink", "/dev/disk0", NULL, NULL, NULL};
    int had = rereads;

    argv[3] = entry;
    argv[4] = sectors;
    if (make_gpt(fd) != 0 || pread(fd, before, sizeof(before), 0) != (ssize_t)sizeof(before)) {
        CHECK("the table is made", 0);
        return;
    }
    CHECK(what, diskctl_main(5, argv) == 1 && rereads == had && unchanged(fd, before));
}

/** The `shrink` rows on the GUID table's image. */
static void check_shrink(void)
{
    static unsigned char before[DISK * 512];
    char *argv[] = {"diskctl", "shrink", "/dev/disk0", "3", "512", NULL};
    unsigned char sector[512];
    int fd = mkstemp(image_path);
    int had;

    if (fd < 0 || make_gpt(fd) != 0) {
        CHECK("the GUID table's image is made", 0);
        return;
    }
    had = rereads;
    CHECK("shrink of entry 3 to 512 sectors succeeds", diskctl_main(5, argv) == 0);
    CHECK("and has the table read again", rereads == had + 1);
    CHECK("the primary ends entry 3 at 2511, the backup at the disk's end",
          entry_end(fd, 3, 0) == 2511);
    CHECK("the backup alone ends it there too", entry_end(fd, 3, 1) == 2511);
    CHECK("the other entries keep their ends",
          entry_end(fd, 1, 0) == 999 && entry_end(fd, 2, 0) == 1999 && entry_end(fd, 4, 0) == 39);
    argv[4] = "2063";
    CHECK("a table made again shrinks by nothing to its whole length",
          make_gpt(fd) == 0 && diskctl_main(5, argv) == 0 && entry_end(fd, 3, 0) == 4062);

    refused(fd, "a length past the entry's is refused", "3", "2064");
    refused(fd, "a length into the next entry is refused", "2", "1001");
    refused(fd, "a length of zero is refused", "3", "0");
    refused(fd, "a length with a leading zero is refused", "3", "0512");
    refused(fd, "a length with other characters is refused", "3", "512x");
    refused(fd, "an unused entry is refused", "5", "512");
    refused(fd, "an entry past the array is refused", "129", "512");
    refused(fd, "entry 0 is refused", "0", "512");

    memset(sector, 0, sizeof(sector));
    sector[446 + 4] = 0x83;
    sector[510] = 0x55;
    sector[511] = 0xAA;
    if (make_gpt(fd) != 0 || pwrite(fd, sector, sizeof(sector), 0) != (ssize_t)sizeof(sector) ||
        pread(fd, before, sizeof(before), 0) != (ssize_t)sizeof(before)) {
        CHECK("the MBR disk is made", 0);
    } else {
        had = rereads;
        argv[4] = "512";
        CHECK("an MBR disk is refused, unread and unchanged",
              diskctl_main(5, argv) == 1 && rereads == had && unchanged(fd, before));
    }
    close(fd);
    unlink(image_path);
}

/**
 * Run `diskctl esp` on a 128-sector image whose MBR entry 1 has type 0x0C:
 * once with the reread refused, then twice with it answered, and once more
 * with the entry's type changed to 0x83, which must be refused before the
 * table is reread or the entry written. Returns 0 when every check passed, 1
 * when one failed, and 2 when the image file could not be made.
 */
int main(void)
{
    char path[] = "/tmp/diskctl-hosttest.XXXXXX";
    char *argv[] = {"diskctl", "esp", path, NULL};
    unsigned char sector[512];
    int fd;

    memset(sector, 0, sizeof(sector));
    sector[446 + 4] = 0x0C;
    sector[446 + 8] = 1;
    sector[446 + 12] = 0x7F;
    sector[510] = 0x55;
    sector[511] = 0xAA;
    fd = mkstemp(path);
    if (fd < 0 || pwrite(fd, sector, sizeof(sector), 0) != (ssize_t)sizeof(sector) ||
        ftruncate(fd, 128 * 512) != 0) {
        printf("diskctl_hosttest: the image file could not be made\n");
        return 2;
    }

    busy = 1;
    CHECK("esp with the disk in use fails", diskctl_main(3, argv) == 1);
    CHECK("after it wrote the type", entry_type(fd) == 0xEF && rereads == 0);
    busy = 0;
    CHECK("esp run again succeeds", diskctl_main(3, argv) == 0);
    CHECK("and has the kernel read the table, the type on the disk already",
          rereads == 1 && entry_type(fd) == 0xEF);
    CHECK("a third run reads it again", diskctl_main(3, argv) == 0 && rereads == 2);

    sector[446 + 4] = 0x83;
    CHECK("an entry of another type is written",
          pwrite(fd, sector, sizeof(sector), 0) == (ssize_t)sizeof(sector));
    CHECK("and refused, unread and unchanged",
          diskctl_main(3, argv) == 1 && rereads == 2 && entry_type(fd) == 0x83);
    close(fd);
    unlink(path);
    check_shrink();

    if (fails == 0) {
        printf("diskctl_hosttest: PASS\n");
        return 0;
    }
    printf("diskctl_hosttest: %d failure(s)\n", fails);
    return 1;
}
