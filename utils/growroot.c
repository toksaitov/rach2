/**
 * Grow a root partition and its ext2 file system into the free space after
 * it: `growroot MBRID=ID,N`.
 *
 * The partition is named by identity, the disk's MBR signature and the
 * entry number, as the kernel's `rach2.root=` is, so a clone of the disk
 * behind it is not taken for it and a table entry is never grown by its
 * position alone. The entry is a primary one. The space it may grow into
 * is what lies between its end and the next entry's start, or the disk's
 * end, or the MBR's reach of 2^32 sectors, whichever comes first; a
 * partition that already fills that space is left as it is. Nothing is
 * moved and nothing else is touched.
 *
 * The steps, in order: the entry is rewritten with the new sector count,
 * the kernel is asked to read the table again, `e2fsck -f -p` checks the
 * volume, and `resize2fs` grows it to the partition. A run stopped after
 * the first step and started again finds the entry already at its size
 * and does the rest; a run stopped inside `resize2fs` leaves a volume the
 * checker on the next run repairs, and `resize2fs` then finishes. Writing
 * the table needs the recovery kernel, the one that lets a whole-disk
 * node take writes, and a table read again while the disk is in use is
 * refused; both refusals are reported as what they are.
 *
 * `resize2fs` and `e2fsck` keep the volume's bitmaps in memory, so a
 * machine below the memory the grow of a root to this disk's size was
 * measured to need is refused by name before anything is read.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* The memory a grow of a 1 GiB root to a partition the size of the
 * installed machine's disk was measured to need, as
 * dev-log/47-storage/Readme.md records: `MemTotal` of the smallest QEMU
 * machine, 16 MiB, on which the recovery system boots and resize2fs and
 * e2fsck then finish, 7232 kB. The two keep their bitmaps as runs, so
 * the volume's size hardly moves the need. */
#ifndef GROWROOT_MEMORY_MIB
#define GROWROOT_MEMORY_MIB 7u
#endif
#define GROWROOT_MEMORY_KIB (GROWROOT_MEMORY_MIB * 1024u)

#define BLKRRPART 0x125Fu
#define MBR_ENTRIES 446
/* One past the largest sector an entry can address: a count and an end are
 * both exclusive here, and the last addressable sector is 0xFFFFFFFF. */
#define MBR_REACH 0x100000000ull

struct part {
    int disk, number;
    unsigned long long start, count;
    unsigned type;
};

static struct part parts[128];
static int nparts;
static unsigned long long disk_sectors[16];
static char disk_table[16][12];
static int ndisks;

static int fail(const char *what, const char *detail)
{
    if (detail) {
        fprintf(stderr, "growroot: %s: %s\n", what, detail);
    } else {
        fprintf(stderr, "growroot: %s\n", what);
    }
    return 1;
}

/** `/proc/disk`: the disks and the partition entries the kernel found. */
static int read_disks(void)
{
    FILE *f = fopen("/proc/disk", "r");
    char line[256];

    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        int d, n;
        unsigned long long a, b;
        char table[12];
        char id[64];

        if (sscanf(line, "disk %d %*s %*d %llu %11s", &d, &a, table) == 3 && d >= 0 && d < 16) {
            disk_sectors[d] = a;
            strcpy(disk_table[d], table);
            if (d + 1 > ndisks) {
                ndisks = d + 1;
            }
        } else if (sscanf(line, "part %d %d %llu %llu %63s", &d, &n, &a, &b, id) == 5 &&
                   nparts < (int)(sizeof(parts) / sizeof(parts[0]))) {
            parts[nparts].disk = d;
            parts[nparts].number = n;
            parts[nparts].start = a;
            parts[nparts].count = b;
            parts[nparts].type = strncmp(id, "mbr:", 4) == 0 ? (unsigned)strtoul(id + 4, 0, 16) : 0;
            nparts++;
        }
    }
    fclose(f);
    return 0;
}

/** The MBR signature of `/dev/diskN`, read from its first sector. */
static int disk_id(int n, unsigned long *id)
{
    char node[32];
    unsigned char sector[512];
    int fd;

    snprintf(node, sizeof(node), "/dev/disk%d", n);
    fd = open(node, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    if (read(fd, sector, sizeof(sector)) != (ssize_t)sizeof(sector)) {
        close(fd);
        return -1;
    }
    close(fd);
    *id = (unsigned long)sector[440] | ((unsigned long)sector[441] << 8) |
          ((unsigned long)sector[442] << 16) | ((unsigned long)sector[443] << 24);
    return 0;
}

/** `MemTotal` of `/proc/meminfo` in KiB, or 0 when it cannot be read. */
static unsigned long memory_kib(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    unsigned long kib = 0;

    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %lu kB", &kib) == 1) {
            break;
        }
    }
    fclose(f);
    return kib;
}

/** Run `path` with `arg` and `node`, and answer its exit status or -1. */
static int run(const char *path, const char *arg, const char *node)
{
    pid_t pid = fork();
    int status = 0;

    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        char *argv[5];
        int i = 0;

        argv[i++] = (char *)strrchr(path, '/') + 1;
        if (arg) {
            argv[i++] = (char *)arg;
        }
        argv[i++] = (char *)node;
        argv[i] = 0;
        execve(path, argv, environ);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/** Whether the partition at `start` of `/dev/diskN` has the ext2 magic. */
static int is_ext2(const char *node)
{
    unsigned char sector[512];
    int fd = open(node, O_RDONLY);
    int ok;

    if (fd < 0) {
        return 0;
    }
    ok = lseek(fd, 1024, SEEK_SET) == 1024 && read(fd, sector, sizeof(sector)) == (ssize_t)sizeof(sector) &&
         sector[56] == 0x53 && sector[57] == 0xEF;
    close(fd);
    return ok;
}

int main(int argc, char **argv)
{
    unsigned long want_id, id, mem;
    int entry, disk = -1, found = 0, i, rc, fd;
    struct part *p = 0;
    unsigned long long end, limit, new_count;
    unsigned char sector[512];
    char dev[32], part_node[32];
    char text[128];

    if (argc != 2 || sscanf(argv[1], "MBRID=%lx,%d", &want_id, &entry) != 2) {
        fprintf(stderr, "usage: growroot MBRID=ID,N\n");
        return 2;
    }
    if (entry < 1 || entry > 4) {
        return fail(argv[1], "the entry is not a primary one; only entries 1 to 4 grow");
    }
    /* Before the disk is even looked for: a machine too small is told so
     * with nothing read and nothing written. */
    mem = memory_kib();
    if (mem < GROWROOT_MEMORY_KIB) {
        snprintf(text, sizeof(text), "%lu MiB of memory; growing a root needs %u MiB",
                 mem / 1024, GROWROOT_MEMORY_MIB);
        return fail("refused", text);
    }
    if (read_disks() != 0) {
        return fail("/proc/disk", strerror(errno));
    }
    for (i = 0; i < ndisks; i++) {
        if (disk_sectors[i] == 0 || strcmp(disk_table[i], "mbr") != 0 || disk_id(i, &id) != 0) {
            continue;
        }
        if (id == want_id) {
            found++;
            disk = i;
        }
    }
    if (found == 0) {
        snprintf(text, sizeof(text), "not found on any of %d disks", ndisks);
        return fail(argv[1], text);
    }
    if (found > 1) {
        return fail(argv[1], "ambiguous: more than one disk has that signature");
    }
    for (i = 0; i < nparts; i++) {
        if (parts[i].disk == disk && parts[i].number == entry) {
            p = &parts[i];
        }
    }
    if (!p) {
        snprintf(text, sizeof(text), "/dev/disk%d has no entry %d", disk, entry);
        return fail(argv[1], text);
    }
    snprintf(dev, sizeof(dev), "/dev/disk%d", disk);
    snprintf(part_node, sizeof(part_node), "/dev/disk%dp%d", disk, entry);
    if (p->type != 0x83 || !is_ext2(part_node)) {
        return fail(part_node, "not an ext2 partition");
    }
    /* The space after the entry: up to the next entry, the disk's end or
     * the MBR's reach. */
    end = p->start + p->count;
    limit = disk_sectors[disk];
    if (limit > MBR_REACH) {
        limit = MBR_REACH;
    }
    for (i = 0; i < nparts; i++) {
        if (parts[i].disk != disk || &parts[i] == p) {
            continue;
        }
        /* A logical partition is described by a record that lies outside
         * it, in the extended partition that holds it, and `/proc/disk`
         * does not name that container. Growing up to the logical's own
         * start would write over the record that names it, so a logical
         * partition after this entry stops the growth altogether. */
        if (parts[i].number >= 5 && parts[i].start >= end) {
            return fail(part_node, "an extended partition lies after it, and its records are "
                                   "not where this can see them");
        }
        if (parts[i].start >= end && parts[i].start < limit) {
            limit = parts[i].start;
        }
    }
    if (limit < end) {
        return fail(part_node, "the entry ends past the disk");
    }
    new_count = limit - p->start;
    if (new_count == p->count) {
        printf("growroot: entry: %s already fills the %llu sectors after it\n", part_node,
               new_count);
    } else {
        unsigned char *e;

        printf("growroot: entry: %s from %llu to %llu sectors\n", part_node, p->count,
               new_count);
        fd = open(dev, O_RDWR);
        if (fd < 0) {
            return fail(dev, strerror(errno));
        }
        /* The disk is judged again with the writer's descriptor open, since
         * the identity and the entry were read through descriptors that are
         * closed by now and another writer on this system could have put a
         * different table there in between. */
        if (read(fd, sector, sizeof(sector)) == (ssize_t)sizeof(sector)) {
            unsigned long now_id = (unsigned long)sector[440] |
                                   ((unsigned long)sector[441] << 8) |
                                   ((unsigned long)sector[442] << 16) |
                                   ((unsigned long)sector[443] << 24);
            const unsigned char *e0 = sector + MBR_ENTRIES + 16 * (entry - 1);
            unsigned long long was_start = (unsigned long long)e0[8] |
                                           ((unsigned long long)e0[9] << 8) |
                                           ((unsigned long long)e0[10] << 16) |
                                           ((unsigned long long)e0[11] << 24);

            int other;

            if (now_id != want_id || was_start != p->start) {
                close(fd);
                return fail(dev, "the table changed while this was deciding; nothing written");
            }
            /* The limit came from a table read through a descriptor that
             * is closed by now. Another writer can add an entry in the
             * space this is about to take without moving this entry or
             * the disk's signature, so the space itself is judged again
             * against the table the writer sees. */
            for (other = 0; other < 4; other++) {
                const unsigned char *o = sector + MBR_ENTRIES + 16 * other;
                unsigned long long o_start, o_count;

                if (other == entry - 1 || o[4] == 0) {
                    continue;
                }
                o_start = (unsigned long long)o[8] | ((unsigned long long)o[9] << 8) |
                          ((unsigned long long)o[10] << 16) | ((unsigned long long)o[11] << 24);
                o_count = (unsigned long long)o[12] | ((unsigned long long)o[13] << 8) |
                          ((unsigned long long)o[14] << 16) | ((unsigned long long)o[15] << 24);
                if (o_count != 0 && o_start < limit && o_start + o_count > end) {
                    close(fd);
                    return fail(dev, "another entry now lies in the space this would take; "
                                     "nothing written");
                }
            }
        }
        if (lseek(fd, 0, SEEK_SET) != 0 ||
            read(fd, sector, sizeof(sector)) != (ssize_t)sizeof(sector)) {
            close(fd);
            return fail(dev, "the first sector did not read");
        }
        e = sector + MBR_ENTRIES + 16 * (entry - 1);
        e[5] = 0xFE; /* the end in CHS: the "use the LBA" sentinel */
        e[6] = 0xFF;
        e[7] = 0xFF;
        e[12] = (unsigned char)new_count;
        e[13] = (unsigned char)(new_count >> 8);
        e[14] = (unsigned char)(new_count >> 16);
        e[15] = (unsigned char)(new_count >> 24);
        if (lseek(fd, 0, SEEK_SET) != 0 || write(fd, sector, sizeof(sector)) != (ssize_t)sizeof(sector)) {
            rc = errno;
            close(fd);
            return fail(dev, rc == EACCES || rc == EPERM || rc == EROFS
                                 ? "the table write was refused; only the recovery kernel writes a whole disk"
                                 : strerror(rc));
        }
        if (ioctl(fd, BLKRRPART, 0) != 0) {
            rc = errno;
            close(fd);
            return fail(dev, rc == EBUSY ? "the table was written but not read again: the disk is in use"
                                         : strerror(rc));
        }
        close(fd);
        nparts = 0;
        ndisks = 0;
        if (read_disks() != 0) {
            return fail("/proc/disk", strerror(errno));
        }
        for (i = 0; i < nparts; i++) {
            if (parts[i].disk == disk && parts[i].number == entry && parts[i].count == new_count) {
                found = 2;
            }
        }
        if (found != 2) {
            return fail(part_node, "the table read again does not show the new size");
        }
    }
    /* A repairing pass writes wherever the damage is, and `growset`
     * computes only the blocks a grow itself writes, so a volume that
     * needs repair cannot be undone from the set saved before this. It is
     * checked read-only first and refused rather than repaired. */
    printf("growroot: check: e2fsck -f -n %s\n", part_node);
    fflush(stdout);
    rc = run("/sbin/e2fsck", "-fn", part_node);
    if (rc != 0) {
        snprintf(text, sizeof(text), "e2fsck -fn status %d; repair it first, since a repair "
                                     "writes outside the saved set", rc);
        return fail(part_node, text);
    }
    printf("growroot: check: e2fsck -f -p %s\n", part_node);
    fflush(stdout);
    rc = run("/sbin/e2fsck", "-fp", part_node);
    if (rc < 0 || rc > 2) {
        snprintf(text, sizeof(text), "e2fsck status %d; the volume is not grown", rc);
        return fail(part_node, text);
    }
    printf("growroot: grow: resize2fs %s\n", part_node);
    fflush(stdout);
    rc = run("/sbin/resize2fs", 0, part_node);
    if (rc != 0) {
        snprintf(text, sizeof(text), "resize2fs status %d", rc);
        return fail(part_node, text);
    }
    printf("growroot: result: %s grown to %llu sectors\n", part_node, new_count);
    return 0;
}
