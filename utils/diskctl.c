/**
 * The three things a program that has just written a whole disk needs:
 * `diskctl reread DEV`, `diskctl id DEV` and `diskctl relocate DEV`.
 *
 * A program that writes a whole disk changes the table under a kernel that
 * read the old one at boot, and the nodes for the partitions it just made do
 * not exist until the kernel is told. That is `BLKRRPART` on the whole-disk
 * node, and the kernel refuses it while anything is mounted from the disk or
 * another descriptor is open on it, which is what keeps a reread from moving
 * the windows under a file system.
 *
 * `growroot` does this as one of its steps, and an installer that does not
 * grow needs it on its own; `reread` is that step by itself.
 *
 * `id` prints the disk's identity: on an MBR disk its signature, the four
 * bytes at offset 440 of its first sector, as eight lowercase hexadecimal
 * digits, and on a disk with a GUID table the table's disk GUID in its
 * 36-character text form. That is how every program here names a disk that
 * has just been written: `growroot` takes `MBRID=ID,N` or `GPTID=GUID,N`, and
 * the identity is the image's own rather than a position that another disk
 * of the machine could also have.
 *
 * `relocate` moves a GUID table's backup to the disk's last sectors and
 * rewrites both copies with their checksums. An image written to a disk
 * larger than itself leaves its backup where the image ended, and a reader
 * looks for it at the disk's end; this puts it there, makes the space between
 * usable, and asks the kernel to read the table again.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "gptdisk.h"

#define BLKRRPART 0x125Fu

static struct gptdisk table;

/**
 * Read the GUID table of the disk open on `fd`, named `path`, into `table`.
 * Returns 1 for a table read, 0 when the disk has an MBR and no GUID table,
 * and -1 having said why when it does not read.
 */
static int read_table(int fd, const char *path)
{
    uint64_t sectors;
    int number = gptdisk_number(path);
    int kind;

    if (number < 0 || gptdisk_sectors(number, &sectors) != 0) {
        fprintf(stderr, "diskctl: %s: not a disk /proc/disk lists\n", path);
        return -1;
    }
    kind = gptdisk_read(fd, sectors, &table);
    if (kind < 0) {
        fprintf(stderr, "diskctl: %s: its GUID table does not read or check\n", path);
    }
    return kind;
}

/**
 * Print the identity of the disk open on `fd`, or report why not.
 * Returns 0, or 1 when the first sector does not read, lacks a table's
 * 0x55AA mark, or is a protective MBR whose GUID table does not read.
 */
static int print_id(int fd, const char *path)
{
    unsigned char sector[512];
    ssize_t n;
    unsigned long id;
    char guid[37];

    n = read(fd, sector, sizeof(sector));
    if (n != (ssize_t)sizeof(sector)) {
        fprintf(stderr, "diskctl: %s: its first sector did not read\n", path);
        return 1;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        fprintf(stderr, "diskctl: %s: its first sector is not a table\n", path);
        return 1;
    }
    if (sector[446 + 4] == 0xEE) {
        if (read_table(fd, path) != 1) {
            return 1;
        }
        gptdisk_guid_text(table.header + 56, guid);
        printf("%s\n", guid);
        return 0;
    }
    id = (unsigned long)sector[440] | ((unsigned long)sector[441] << 8) |
         ((unsigned long)sector[442] << 16) | ((unsigned long)sector[443] << 24);
    printf("%08lx\n", id);
    return 0;
}

/** Ask the kernel to read the table of the disk open on `fd` again. */
static int reread(int fd, const char *path)
{
    if (ioctl(fd, BLKRRPART, 0) != 0) {
        int e = errno;

        if (e == EBUSY) {
            fprintf(stderr,
                    "diskctl: %s is in use: unmount everything on it and close "
                    "every program holding it\n",
                    path);
        } else if (e == ENOTTY) {
            fprintf(stderr, "diskctl: %s is not a whole disk\n", path);
        } else {
            fprintf(stderr, "diskctl: %s: %s\n", path, strerror(e));
        }
        return 1;
    }
    return 0;
}

/** Move the GUID table's backup of the disk open on `fd` to its end. */
static int relocate(int fd, const char *path)
{
    const char *why = "";
    int kind = read_table(fd, path);
    int moved;

    if (kind < 0) {
        return 1;
    }
    if (kind == 0) {
        fprintf(stderr, "diskctl: %s has an MBR and no GUID table\n", path);
        return 1;
    }
    moved = !gptdisk_backup_at_end(&table);
    if (gptdisk_write(fd, &table, &why) != 0) {
        fprintf(stderr, "diskctl: %s: %s\n", path, why);
        return 1;
    }
    if (reread(fd, path) != 0) {
        return 1;
    }
    printf("diskctl: result: %s: the backup table %s sectors %llu to %llu\n", path,
           moved ? "was moved to" : "is in",
           (unsigned long long)gptdisk_le64(table.header + 48) + 1,
           (unsigned long long)table.sectors - 1);
    return 0;
}

int main(int argc, char **argv)
{
    int fd, rc;
    int writing;

    if (argc != 3 || (strcmp(argv[1], "reread") != 0 && strcmp(argv[1], "id") != 0 &&
                      strcmp(argv[1], "relocate") != 0)) {
        fprintf(stderr,
                "usage: diskctl reread DEVICE | diskctl id DEVICE | diskctl relocate DEVICE\n");
        return 2;
    }
    writing = strcmp(argv[1], "relocate") == 0;
    fd = open(argv[2], writing ? O_RDWR : O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "diskctl: cannot open %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    if (strcmp(argv[1], "id") == 0) {
        rc = print_id(fd, argv[2]);
    } else if (writing) {
        rc = relocate(fd, argv[2]);
    } else {
        rc = reread(fd, argv[2]);
        if (rc == 0) {
            printf("diskctl: %s: the table was read again\n", argv[2]);
        }
    }
    close(fd);
    return rc;
}
