/*
 * diskctl_hosttest.c: diskctl's `esp` over an image file, with the kernel's
 * reread answered here.
 *
 * host-deps: utils/gptdisk.c
 *
 * The source is included with `ioctl` replaced: a reread refused with EBUSY,
 * as the kernel refuses one while another descriptor is open on the disk,
 * comes after the type is written, so the disk has 0xEF and the kernel's
 * table does not. The next run must read the table again although the disk
 * has the type already, and a run on a disk the kernel has read must read it
 * again too, since nothing on the disk says which of the two it is.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int busy, rereads;

/** The kernel's BLKRRPART: refused with EBUSY while `busy`, else counted. */
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

#define ioctl counted_ioctl
#define main  diskctl_main
#include "diskctl.c"
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

/** The type byte of entry 1 of the image open on `fd`. */
static int entry_type(int fd)
{
    unsigned char sector[512];

    if (pread(fd, sector, sizeof(sector), 0) != (ssize_t)sizeof(sector)) {
        return -1;
    }
    return sector[446 + 4];
}

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

    if (fails == 0) {
        printf("diskctl_hosttest: PASS\n");
        return 0;
    }
    printf("diskctl_hosttest: %d failure(s)\n", fails);
    return 1;
}
