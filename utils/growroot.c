/**
 * growroot: grow a primary ext2 partition and its file system into the free
 * space after it, from the recovery system.
 *
 * `growroot MBRID=ID,N [--measure]` names the disk by its MBR signature and
 * the partition by its entry, 1 to 4. The entry is rewritten to reach the next
 * partition, the disk's end or the MBR's reach, whichever comes first, the
 * kernel reads the table again, and `e2fsck` and `resize2fs` grow the file
 * system to fill the entry. `--measure` prints the clock and the disk
 * counters at the edges of each phase. The procedure and its refusals are in
 * `growroot(8)`.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* The memory a grow of a 1 GiB root to a partition the size of the
 * installed machine's disk was measured to need, as
 * dev-log/47-storage/notes.md records: resize2fs and e2fsck finished on the
 * smallest QEMU machine the recovery system boots on, 16 MiB, whose
 * `MemTotal` is 7232 kB. The two keep their bitmaps as runs, so the volume's
 * size hardly moves the need. */
#ifndef GROWROOT_MEMORY_MIB
#define GROWROOT_MEMORY_MIB 7u
#endif
#define GROWROOT_MEMORY_KIB (GROWROOT_MEMORY_MIB * 1024u)

#define BLKRRPART   0x125Fu
/* The byte offset of the four partition entries in the first sector. */
#define MBR_ENTRIES 446
/* One past the largest sector an entry can address: a count and an end are
 * both exclusive here, and the last addressable sector is 0xFFFFFFFF. */
#define MBR_REACH   0x100000000ull

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
static int measure;

static int fail(const char *what, const char *detail)
{
    if (detail) {
        fprintf(stderr, "growroot: %s: %s\n", what, detail);
    } else {
        fprintf(stderr, "growroot: %s\n", what);
    }
    return 1;
}

/**
 * Record each phase against the same monotonic clock and disk counters.
 * The counters cover the machine, so recovery must have no other disk work
 * running for their differences to describe this grow alone. Rach2 time
 * counts delivered 10 ms ticks; masked interrupts can lose ticks, so these
 * timestamps do not independently establish wall time. Does nothing without
 * `--measure`; returns 0, or 1 having said why.
 */
static int measured(const char *phase, const char *edge)
{
    FILE *f;
    struct timespec t;
    char line[256];
    unsigned long long commands = 0, flushes = 0, us;
    int seen = 0, error;

    if (!measure) {
        return 0;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        return fail("measurement clock", strerror(errno));
    }
    f = fopen("/proc/disk", "r");
    if (!f) {
        return fail("measurement counters", strerror(errno));
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "disk-commands %llu", &commands) == 1) {
            seen |= 1;
        } else if (sscanf(line, "disk-flushes %llu", &flushes) == 1) {
            seen |= 2;
        }
    }
    error = ferror(f);
    fclose(f);
    if (error || seen != 3) {
        return fail("measurement counters", "commands or flushes did not read");
    }
    us = (unsigned long long)t.tv_sec * 1000000 + (unsigned long)t.tv_nsec / 1000;
    printf("growroot: measure: %s %s monotonic-us %llu disk-commands %llu disk-flushes %llu\n",
           phase, edge, us, commands, flushes);
    fflush(stdout);
    return 0;
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

/**
 * Run `path` with `arg`, when it is not NULL, and `node`. Returns its exit
 * status, or -1 when it could not be started or did not exit.
 */
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

/**
 * Return whether partition node `node` has the ext2 superblock magic.
 * Return zero if the node cannot be opened or read.
 */
static int is_ext2(const char *node)
{
    unsigned char sector[512];
    int fd = open(node, O_RDONLY);
    int ok;

    if (fd < 0) {
        return 0;
    }
    ok = lseek(fd, 1024, SEEK_SET) == 1024 &&
         read(fd, sector, sizeof(sector)) == (ssize_t)sizeof(sector) && sector[56] == 0x53 &&
         sector[57] == 0xEF;
    close(fd);
    return ok;
}

/**
 * Grow an unmounted primary ext2 partition from recovery.
 *
 * Check the memory threshold before reading disks. Bound growth by the
 * next partition, the disk's end and the MBR address limit. Reject a later
 * logical partition because its extended-partition records are absent
 * from `/proc/disk`. Before writing, reread the MBR through the writer's
 * descriptor and recheck the identity, target entry and proposed free range.
 *
 * After updating and rereading the partition table, run `e2fsck -fn`.
 * Continue with `e2fsck -fp` and `resize2fs` only when the read-only check
 * succeeds. A volume requiring repair must be repaired separately before
 * retrying. Repairs can write outside the ranges saved for the grow.
 * Writing the table requires the recovery kernel; rereading the table
 * fails while the disk is in use. An entry that already fills its space is
 * left as it is, and its file system is still checked and grown.
 */
int main(int argc, char **argv)
{
    unsigned long want_id, id, mem;
    int entry, disk = -1, found = 0, i, rc, fd;
    struct part *p = 0;
    unsigned long long end, limit, new_count;
    unsigned char sector[512];
    char dev[32], part_node[32];
    char text[128];

    measure = argc == 3 && strcmp(argv[2], "--measure") == 0;
    if ((argc != 2 && !measure) || sscanf(argv[1], "MBRID=%lx,%d", &want_id, &entry) != 2) {
        fprintf(stderr, "usage: growroot MBRID=ID,N [--measure]\n");
        return 2;
    }
    if (entry < 1 || entry > 4) {
        return fail(argv[1], "the entry is not a primary one; only entries 1 to 4 grow");
    }
    if (measure) {
        puts("growroot: clock: CLOCK_MONOTONIC uses delivered 10 ms ticks on Rach2; "
             "lost ticks can undercount elapsed time");
    }
    mem = memory_kib();
    if (mem < GROWROOT_MEMORY_KIB) {
        snprintf(text, sizeof(text), "%lu MiB of memory; growing a root needs %u MiB", mem / 1024,
                 GROWROOT_MEMORY_MIB);
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
    end = p->start + p->count;
    limit = disk_sectors[disk];
    if (limit > MBR_REACH) {
        limit = MBR_REACH;
    }
    for (i = 0; i < nparts; i++) {
        if (parts[i].disk != disk || &parts[i] == p) {
            continue;
        }
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
    if (measured("table", "before")) {
        return 1;
    }
    if (new_count == p->count) {
        printf("growroot: entry: %s already fills the %llu sectors after it\n", part_node,
               new_count);
    } else {
        unsigned char *e;

        printf("growroot: entry: %s from %llu to %llu sectors\n", part_node, p->count, new_count);
        fd = open(dev, O_RDWR);
        if (fd < 0) {
            return fail(dev, strerror(errno));
        }
        if (read(fd, sector, sizeof(sector)) != (ssize_t)sizeof(sector)) {
            close(fd);
            return fail(dev, "the first sector did not read; nothing written");
        }
        {
            unsigned long now_id = (unsigned long)sector[440] | ((unsigned long)sector[441] << 8) |
                                   ((unsigned long)sector[442] << 16) |
                                   ((unsigned long)sector[443] << 24);
            const unsigned char *e0 = sector + MBR_ENTRIES + 16 * (entry - 1);
            unsigned long long was_start =
                (unsigned long long)e0[8] | ((unsigned long long)e0[9] << 8) |
                ((unsigned long long)e0[10] << 16) | ((unsigned long long)e0[11] << 24);
            unsigned long long was_count =
                (unsigned long long)e0[12] | ((unsigned long long)e0[13] << 8) |
                ((unsigned long long)e0[14] << 16) | ((unsigned long long)e0[15] << 24);

            int other;

            if (sector[510] != 0x55 || sector[511] != 0xAA || now_id != want_id ||
                e0[4] != p->type || was_start != p->start || was_count != p->count) {
                close(fd);
                return fail(dev, "the table changed while this was deciding; nothing written");
            }
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
        e = sector + MBR_ENTRIES + 16 * (entry - 1);
        e[5] = 0xFE; /* the end in CHS: the "use the LBA" sentinel */
        e[6] = 0xFF;
        e[7] = 0xFF;
        e[12] = (unsigned char)new_count;
        e[13] = (unsigned char)(new_count >> 8);
        e[14] = (unsigned char)(new_count >> 16);
        e[15] = (unsigned char)(new_count >> 24);
        if (lseek(fd, 0, SEEK_SET) != 0 ||
            write(fd, sector, sizeof(sector)) != (ssize_t)sizeof(sector)) {
            rc = errno;
            close(fd);
            return fail(
                dev,
                rc == EACCES || rc == EPERM || rc == EROFS
                    ? "the table write was refused; only the recovery kernel writes a whole disk"
                    : strerror(rc));
        }
        if (ioctl(fd, BLKRRPART, 0) != 0) {
            rc = errno;
            close(fd);
            return fail(dev, rc == EBUSY
                                 ? "the table was written but not read again: the disk is in use"
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
    if (measured("table", "after")) {
        return 1;
    }
    printf("growroot: check: e2fsck -f -n %s\n", part_node);
    fflush(stdout);
    if (measured("check-readonly", "before")) {
        return 1;
    }
    rc = run("/sbin/e2fsck", "-fn", part_node);
    if (measured("check-readonly", "after")) {
        return 1;
    }
    if (rc != 0) {
        snprintf(text, sizeof(text),
                 "e2fsck -fn status %d; repair it first, since a repair "
                 "writes outside the saved set",
                 rc);
        return fail(part_node, text);
    }
    printf("growroot: check: e2fsck -f -p %s\n", part_node);
    fflush(stdout);
    if (measured("check-repair", "before")) {
        return 1;
    }
    rc = run("/sbin/e2fsck", "-fp", part_node);
    if (measured("check-repair", "after")) {
        return 1;
    }
    if (rc < 0 || rc > 2) {
        snprintf(text, sizeof(text), "e2fsck status %d; the volume is not grown", rc);
        return fail(part_node, text);
    }
    printf("growroot: grow: resize2fs %s\n", part_node);
    fflush(stdout);
    if (measured("resize", "before")) {
        return 1;
    }
    rc = run("/sbin/resize2fs", 0, part_node);
    if (measured("resize", "after")) {
        return 1;
    }
    if (rc != 0) {
        snprintf(text, sizeof(text), "resize2fs status %d", rc);
        return fail(part_node, text);
    }
    printf("growroot: result: %s grown to %llu sectors\n", part_node, new_count);
    return 0;
}
