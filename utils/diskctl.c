/**
 * The things a program that writes a whole disk needs: `diskctl reread DEV`,
 * `diskctl id DEV`, `diskctl relocate DEV`, `diskctl shrink DEV N SECTORS`
 * and `diskctl esp DEV`.
 *
 * A program that writes a whole disk changes the table under a kernel that
 * read the old one at boot, and the nodes for the partitions it just made do
 * not exist until the kernel is told. That is `BLKRRPART` on the whole-disk
 * node, which fails with EBUSY while anything is mounted from the disk or
 * another descriptor is open on it, and that is what keeps a reread from
 * moving the windows under a file system.
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
 * usable, and asks the kernel to read the table again. A table whose primary
 * does not check is read from its backup, so the same command writes the
 * primary again, which GRUB's BIOS core needs: it reads no backup.
 *
 * `shrink` gives entry N of a GUID table SECTORS sectors from its first, no
 * more than it has, and writes both copies as `relocate` does. The result is
 * the table of an image whose root is smaller than the disk, so that
 * `growroot` can be timed growing that root on the disk itself:
 * `tools/dev/machine_grow.sh` writes such a root and then this table over
 * sectors it has saved.
 *
 * `esp` gives entry 1 of an MBR the EFI system partition's type, 0xEF, which
 * UEFI firmware boots from on an MBR disk, and asks the kernel to read the
 * table again. It asks for the reread when the entry has the type already
 * too, since an earlier run may have written the type and failed to have
 * the table read. The entry must be a FAT one already, 0x0B, 0x0C or 0xEF:
 * this is the last step of converting an installed MBR disk's boot
 * partition, whose file system the installer has made one UEFI firmware
 * reads.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "gptdisk.h"

/* Linux's BLKRRPART: read the disk's partition table again. */
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

/** Ask the kernel to read the table of the disk open on `fd` again; 0, or 1
 *  having said why not. */
static int reread(int fd, const char *path)
{
    if (ioctl(fd, BLKRRPART, (void *)0) != 0) {
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

/**
 * Move the GUID table's backup of the disk open on `fd` to its end, writing
 * both copies, the primary from the backup when only the backup checked,
 * and have the table read again. Returns 0, or 1 having said why not.
 */
static int relocate(int fd, const char *path)
{
    const char *why = "";
    int kind = read_table(fd, path);
    int moved, repaired;

    if (kind < 0) {
        return 1;
    }
    if (kind == 0) {
        fprintf(stderr, "diskctl: %s has an MBR and no GUID table\n", path);
        return 1;
    }
    moved = !gptdisk_backup_at_end(&table);
    repaired = table.from_backup;
    if (gptdisk_write(fd, &table, &why) != 0) {
        fprintf(stderr, "diskctl: %s: %s\n", path, why);
        return 1;
    }
    if (reread(fd, path) != 0) {
        return 1;
    }
    if (repaired) {
        printf("diskctl: result: %s: the primary table was written again from the backup, which "
               "is in sectors %llu to %llu\n",
               path, (unsigned long long)gptdisk_le64(table.header + 48) + 1,
               (unsigned long long)table.sectors - 1);
    } else {
        printf("diskctl: result: %s: the backup table %s sectors %llu to %llu\n", path,
               moved ? "was moved to" : "is in",
               (unsigned long long)gptdisk_le64(table.header + 48) + 1,
               (unsigned long long)table.sectors - 1);
    }
    return 0;
}

/**
 * Give entry `text` of the GUID table of the disk open on `fd` a length of
 * `count` sectors, decimal text, from its first sector, writing both copies
 * with the backup at the disk's end, and have the table read again. Returns
 * 0, or 1 having said why not: the disk has no GUID table, the entry is not
 * a used one, or the length is zero or more than the entry has.
 */
static int shrink(int fd, const char *path, const char *text, const char *count)
{
    const char *why = "";
    unsigned char *e;
    unsigned long entry;
    unsigned long long sectors, first, length;
    char *end;
    int kind = read_table(fd, path);

    if (kind < 0) {
        return 1;
    }
    if (kind == 0) {
        fprintf(stderr, "diskctl: %s has an MBR and no GUID table\n", path);
        return 1;
    }
    entry = strtoul(text, &end, 10);
    e = *text >= '1' && *text <= '9' && *end == '\0' && entry <= table.entries
            ? gptdisk_entry(&table, (int)entry)
            : NULL;
    if (!e || !gptdisk_used(e) || gptdisk_le64(e + 40) < gptdisk_le64(e + 32)) {
        fprintf(stderr, "diskctl: %s: entry %s is not a used entry of its table\n", path, text);
        return 1;
    }
    first = gptdisk_le64(e + 32);
    length = gptdisk_le64(e + 40) - first + 1;
    errno = 0;
    sectors = strtoull(count, &end, 10);
    if (*count < '1' || *count > '9' || *end != '\0' || errno != 0 || sectors > length) {
        fprintf(stderr,
                "diskctl: %s: entry %lu has %llu sectors, and %s is not a length from 1 to that\n",
                path, entry, length, count);
        return 1;
    }
    gptdisk_put64(e + 40, first + sectors - 1);
    if (gptdisk_write(fd, &table, &why) != 0) {
        fprintf(stderr, "diskctl: %s: %s\n", path, why);
        return 1;
    }
    if (reread(fd, path) != 0) {
        return 1;
    }
    printf("diskctl: result: %s: entry %lu is sectors %llu to %llu\n", path, entry, first,
           first + sectors - 1);
    return 0;
}

/** Give entry 1 of the MBR of the disk open on `fd` the type 0xEF, and have
 *  the table read again; 0, or 1 having said why not. */
static int make_esp(int fd, const char *path)
{
    unsigned char sector[512];
    unsigned char *type = sector + 446 + 4;

    if (read(fd, sector, sizeof(sector)) != (ssize_t)sizeof(sector) || sector[510] != 0x55 ||
        sector[511] != 0xAA) {
        fprintf(stderr, "diskctl: %s: its first sector is not a table\n", path);
        return 1;
    }
    if (*type == 0xEE) {
        fprintf(stderr, "diskctl: %s has a GUID table, whose entries have no type byte\n", path);
        return 1;
    }
    if (*type != 0x0B && *type != 0x0C && *type != 0xEF) {
        fprintf(stderr, "diskctl: %s: entry 1 has type 0x%02x, which is not a FAT one\n", path,
                *type);
        return 1;
    }
    if (*type != 0xEF) {
        *type = 0xEF;
        if (lseek(fd, 0, SEEK_SET) != 0 ||
            write(fd, sector, sizeof(sector)) != (ssize_t)sizeof(sector)) {
            fprintf(stderr, "diskctl: %s: the table was not written: %s\n", path, strerror(errno));
            return 1;
        }
    }
    if (reread(fd, path) != 0) {
        return 1;
    }
    printf("diskctl: result: %s: entry 1 is the EFI system partition, type 0xef\n", path);
    return 0;
}

/** Run the command on DEVICE, opened for writing only for `relocate`,
 *  `shrink` and `esp`. Exits 0, 1 on a failure and 2 on a usage error. */
int main(int argc, char **argv)
{
    int fd, rc;
    int writing;
    int shrinking = argc == 5 && strcmp(argv[1], "shrink") == 0;

    if (!shrinking &&
        (argc != 3 || (strcmp(argv[1], "reread") != 0 && strcmp(argv[1], "id") != 0 &&
                       strcmp(argv[1], "relocate") != 0 && strcmp(argv[1], "esp") != 0))) {
        fprintf(stderr, "usage: diskctl reread DEVICE | diskctl id DEVICE | "
                        "diskctl relocate DEVICE | diskctl shrink DEVICE N SECTORS | "
                        "diskctl esp DEVICE\n");
        return 2;
    }
    writing = shrinking || strcmp(argv[1], "relocate") == 0 || strcmp(argv[1], "esp") == 0;
    fd = open(argv[2], writing ? O_RDWR : O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "diskctl: cannot open %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    if (strcmp(argv[1], "id") == 0) {
        rc = print_id(fd, argv[2]);
    } else if (strcmp(argv[1], "esp") == 0) {
        rc = make_esp(fd, argv[2]);
    } else if (shrinking) {
        rc = shrink(fd, argv[2], argv[3], argv[4]);
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
