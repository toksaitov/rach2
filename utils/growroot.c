/**
 * growroot: grow an ext2 partition and its file system into the free space
 * after it, from the recovery system.
 *
 * `growroot MBRID=ID,N [--measure]` names an MBR disk by its signature and
 * the partition by its primary entry, 1 to 4, and `growroot GPTID=GUID,N
 * [--measure]` names a disk with a GUID table by the table's disk GUID and
 * the partition by its entry. The entry is rewritten to reach the next
 * partition, the disk's end or the MBR's reach, whichever comes first; on a
 * GUID table the end is the sector before the backup table, which is moved
 * to the disk's last sectors in the same write, and both copies are written
 * with their checksums. The kernel reads the table again, and `e2fsck` and
 * `resize2fs` grow the file system to fill the entry. `--measure` prints the
 * clock and the disk counters at the edges of each phase. The procedure and
 * its refusals are in `growroot(8)`.
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

#include "gptdisk.h"

extern char **environ;

/* The memory a grow of a 1 GiB root to a partition the size of the
 * installed machine's disk was measured to need, as
 * dev-log/47-storage/notes.md records: resize2fs and e2fsck finished on a
 * QEMU machine of 16 MiB, whose `MemTotal` was then 7232 kB. The two keep
 * their bitmaps as runs, so the volume's size hardly moves the need. */
#ifndef GROWROOT_MEMORY_MIB
#define GROWROOT_MEMORY_MIB 7u
#endif
#define GROWROOT_MEMORY_KIB (GROWROOT_MEMORY_MIB * 1024u)

#define BLKRRPART     0x125Fu
/* The byte offset of the four partition entries in the first sector. */
#define MBR_ENTRIES   446
/* One past the largest sector an entry can address: a count and an end are
 * both exclusive here, and the last addressable sector is 0xFFFFFFFF. */
#define MBR_REACH     0x100000000ull
/* The entries a GUID table may name in `GPTID=GUID,N`: the kernel publishes
 * a partition node for the first fifteen. */
#define GPT_ENTRY_MAX 15

/* A Linux file system's type GUID, 0FC63DAF-8483-4772-8E79-3D69D8477DE4, as
 * the table stores it: the root's type on a disk this system installs. */
static const unsigned char type_linux[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                             0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};

struct part {
    int disk, number;
    unsigned long long start, count;
    unsigned type;
};

static struct part parts[128];
static struct gptdisk gpt_table;
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
 * Whether disk `n` has the GUID table whose disk GUID is `guid`. The table
 * is read into `table`.
 */
static int gpt_disk_is(int n, const unsigned char *guid)
{
    char node[32];
    int fd, kind;

    snprintf(node, sizeof(node), "/dev/disk%d", n);
    fd = open(node, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    kind = gptdisk_read(fd, disk_sectors[n], &gpt_table);
    close(fd);
    return kind == 1 && memcmp(gpt_table.header + 56, guid, 16) == 0;
}

/**
 * Find the one disk the identity names: an MBR disk with signature `id`, or
 * with `guid` not NULL, a disk whose GUID table has that disk GUID. Returns
 * its number, or -1 having said why not.
 */
static int find_disk(const char *spec, unsigned long id, const unsigned char *guid)
{
    unsigned long on_disk;
    char text[64];
    int found = 0, disk = -1, i;

    for (i = 0; i < ndisks; i++) {
        if (disk_sectors[i] == 0) {
            continue;
        }
        if (guid
                ? strcmp(disk_table[i], "gpt") == 0 && gpt_disk_is(i, guid)
                : strcmp(disk_table[i], "mbr") == 0 && disk_id(i, &on_disk) == 0 && on_disk == id) {
            found++;
            disk = i;
        }
    }
    if (found == 0) {
        snprintf(text, sizeof(text), "not found on any of %d disks", ndisks);
        fail(spec, text);
        return -1;
    }
    if (found > 1) {
        fail(spec, guid ? "ambiguous: more than one disk has that GUID"
                        : "ambiguous: more than one disk has that signature");
        return -1;
    }
    return disk;
}

/** Read `/proc/disk` again and require entry `entry` of `disk` at `count`. */
static int published(int disk, int entry, unsigned long long count, const char *part_node)
{
    int i;

    nparts = 0;
    ndisks = 0;
    if (read_disks() != 0) {
        return fail("/proc/disk", strerror(errno));
    }
    for (i = 0; i < nparts; i++) {
        if (parts[i].disk == disk && parts[i].number == entry && parts[i].count == count) {
            return 0;
        }
    }
    return fail(part_node, "the table read again does not show the new size");
}

/** Ask the kernel to read the table on `fd` again, and close `fd`. */
static int reread_table(int fd, const char *dev)
{
    int rc;

    if (ioctl(fd, BLKRRPART, 0) != 0) {
        rc = errno;
        close(fd);
        return fail(dev, rc == EBUSY
                             ? "the table was written but not read again: the disk is in use"
                             : strerror(rc));
    }
    close(fd);
    return 0;
}

/**
 * Rewrite primary entry `entry` of the MBR of `disk` to reach `limit`, the
 * next partition, the disk's end or the MBR's reach. Before writing, the
 * MBR is read again through the writer's descriptor, and its signature,
 * identity, the entry and the free range are checked against what was
 * decided. Returns 0, or 1 having said why not.
 */
static int grow_mbr(int disk, int entry, unsigned long want_id, const struct part *p,
                    unsigned long long limit)
{
    unsigned long long end = p->start + p->count;
    unsigned long long new_count = limit - p->start;
    unsigned char sector[512];
    unsigned char *e;
    char dev[32], part_node[32];
    int fd, rc, other;

    snprintf(dev, sizeof(dev), "/dev/disk%d", disk);
    snprintf(part_node, sizeof(part_node), "/dev/disk%dp%d", disk, entry);
    if (new_count == p->count) {
        printf("growroot: entry: %s already fills the %llu sectors after it\n", part_node,
               new_count);
        return 0;
    }
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

        if (sector[510] != 0x55 || sector[511] != 0xAA || now_id != want_id || e0[4] != p->type ||
            was_start != p->start || was_count != p->count) {
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
        return fail(dev, rc == EACCES || rc == EPERM || rc == EROFS
                             ? "the table write was refused; only the recovery kernel writes a "
                               "whole disk"
                             : strerror(rc));
    }
    if (reread_table(fd, dev) != 0) {
        return 1;
    }
    return published(disk, entry, new_count, part_node);
}

/**
 * Rewrite entry `entry` of the GUID table of `disk` to reach the next
 * partition or the sector before the backup table, with the backup moved to
 * the disk's last sectors. The table is read again through the writer's
 * descriptor, and its disk GUID, the entry's type and range, and every other
 * used entry are checked against what was decided. Returns 0, or 1 having
 * said why not, with the new length in `*grown`.
 */
static int grow_gpt(int disk, int entry, const unsigned char *want_guid, const struct part *p,
                    unsigned long long *grown)
{
    unsigned long long end = p->start + p->count;
    unsigned long long limit;
    const char *why = "";
    unsigned char *e;
    char dev[32], part_node[32];
    int fd, n;

    snprintf(dev, sizeof(dev), "/dev/disk%d", disk);
    snprintf(part_node, sizeof(part_node), "/dev/disk%dp%d", disk, entry);
    fd = open(dev, O_RDWR);
    if (fd < 0) {
        return fail(dev, strerror(errno));
    }
    e = gptdisk_read(fd, disk_sectors[disk], &gpt_table) == 1 ? gptdisk_entry(&gpt_table, entry)
                                                              : NULL;
    if (!e || memcmp(gpt_table.header + 56, want_guid, 16) != 0 || !gptdisk_used(e) ||
        gptdisk_le64(e + 32) != p->start || gptdisk_le64(e + 40) + 1 != end) {
        close(fd);
        return fail(dev, "the table changed while this was deciding; nothing written");
    }
    if (memcmp(e, type_linux, 16) != 0) {
        close(fd);
        return fail(part_node, "not a Linux file system's entry");
    }
    limit = disk_sectors[disk] - 1 -
            (gpt_table.entries * gpt_table.entry_size + GPTDISK_SECTOR - 1) / GPTDISK_SECTOR;
    for (n = 1; n <= (int)gpt_table.entries; n++) {
        const unsigned char *o = gptdisk_entry(&gpt_table, n);

        if (n != entry && gptdisk_used(o) && gptdisk_le64(o + 32) >= end &&
            gptdisk_le64(o + 32) < limit) {
            limit = gptdisk_le64(o + 32);
        }
    }
    if (limit < end) {
        close(fd);
        return fail(part_node, "the entry ends past the disk");
    }
    *grown = limit - p->start;
    if (*grown == p->count && gptdisk_backup_at_end(&gpt_table)) {
        close(fd);
        printf("growroot: entry: %s already fills the %llu sectors after it\n", part_node, *grown);
        return 0;
    }
    printf("growroot: entry: %s from %llu to %llu sectors\n", part_node, p->count, *grown);
    gptdisk_put64(e + 40, limit - 1);
    if (gptdisk_write(fd, &gpt_table, &why) != 0) {
        close(fd);
        return fail(dev, why);
    }
    if (reread_table(fd, dev) != 0) {
        return 1;
    }
    return published(disk, entry, *grown, part_node);
}

/**
 * Bound an MBR partition's growth by the next partition, the disk's end and
 * the MBR's reach, into `*limit`. Returns 0, or 1 having said why not: a
 * later logical partition, whose extended-partition records are absent from
 * `/proc/disk`, or an entry that already ends past the disk.
 */
static int mbr_limit(int disk, const struct part *p, unsigned long long *limit)
{
    unsigned long long end = p->start + p->count;
    char part_node[32];
    int i;

    snprintf(part_node, sizeof(part_node), "/dev/disk%dp%d", disk, p->number);
    *limit = disk_sectors[disk];
    if (*limit > MBR_REACH) {
        *limit = MBR_REACH;
    }
    for (i = 0; i < nparts; i++) {
        if (parts[i].disk != disk || &parts[i] == p) {
            continue;
        }
        if (parts[i].number >= 5 && parts[i].start >= end) {
            return fail(part_node, "an extended partition lies after it, and its records are "
                                   "not where this can see them");
        }
        if (parts[i].start >= end && parts[i].start < *limit) {
            *limit = parts[i].start;
        }
    }
    if (*limit < end) {
        return fail(part_node, "the entry ends past the disk");
    }
    return 0;
}

/** Run `tool` with `arg` on `part_node`, measured as `phase`; its status. */
static int check_or_grow(const char *phase, const char *tool, const char *arg,
                         const char *part_node)
{
    int rc;

    if (measured(phase, "before")) {
        return -2;
    }
    rc = run(tool, arg, part_node);
    if (measured(phase, "after")) {
        return -2;
    }
    return rc;
}

/**
 * Grow an unmounted ext2 partition from recovery.
 *
 * Check the memory threshold before reading disks, then find the one disk
 * the identity names and the entry on it. The table step is the MBR's or
 * the GUID table's, each of which reads its table again through the
 * writer's descriptor and checks it against what was decided before it
 * writes.
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
    unsigned long want_id = 0, mem;
    unsigned char want_guid[16];
    int entry = 0, disk, i, rc, gpt;
    struct part *p = 0;
    unsigned long long limit, new_count = 0;
    char part_node[32], guid_text[37];
    char text[128];

    measure = argc == 3 && strcmp(argv[2], "--measure") == 0;
    gpt = strncmp(argv[1] ? argv[1] : "", "GPTID=", 6) == 0;
    if (gpt && strlen(argv[1]) > 43 && strlen(argv[1]) < 46 && argv[1][42] == ',') {
        char *rest;

        memcpy(guid_text, argv[1] + 6, 36);
        guid_text[36] = '\0';
        entry = (int)strtol(argv[1] + 43, &rest, 10);
        if (gptdisk_guid_parse(guid_text, want_guid) != 0 || *rest != '\0' || entry < 1) {
            entry = 0;
        }
    }
    if ((argc != 2 && !measure) ||
        (gpt ? entry == 0 : sscanf(argv[1], "MBRID=%lx,%d", &want_id, &entry) != 2)) {
        fprintf(stderr, "usage: growroot MBRID=ID,N | GPTID=GUID,N [--measure]\n");
        return 2;
    }
    if (entry < 1 || entry > (gpt ? GPT_ENTRY_MAX : 4)) {
        return fail(argv[1], gpt ? "the entry is past the fifteen partitions the kernel publishes"
                                 : "the entry is not a primary one; only entries 1 to 4 grow");
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
    disk = find_disk(argv[1], want_id, gpt ? want_guid : NULL);
    if (disk < 0) {
        return 1;
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
    snprintf(part_node, sizeof(part_node), "/dev/disk%dp%d", disk, entry);
    if ((!gpt && p->type != 0x83) || !is_ext2(part_node)) {
        return fail(part_node, "not an ext2 partition");
    }
    if (!gpt && mbr_limit(disk, p, &limit) != 0) {
        return 1;
    }
    if (measured("table", "before")) {
        return 1;
    }
    if (gpt ? grow_gpt(disk, entry, want_guid, p, &new_count) != 0
            : grow_mbr(disk, entry, want_id, p, limit) != 0) {
        return 1;
    }
    if (!gpt) {
        new_count = limit - p->start;
    }
    if (measured("table", "after")) {
        return 1;
    }
    printf("growroot: check: e2fsck -f -n %s\n", part_node);
    fflush(stdout);
    rc = check_or_grow("check-readonly", "/sbin/e2fsck", "-fn", part_node);
    if (rc == -2) {
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
    rc = check_or_grow("check-repair", "/sbin/e2fsck", "-fp", part_node);
    if (rc == -2) {
        return 1;
    }
    if (rc < 0 || rc > 2) {
        snprintf(text, sizeof(text), "e2fsck status %d; the volume is not grown", rc);
        return fail(part_node, text);
    }
    printf("growroot: grow: resize2fs %s\n", part_node);
    fflush(stdout);
    rc = check_or_grow("resize", "/sbin/resize2fs", 0, part_node);
    if (rc == -2) {
        return 1;
    }
    if (rc != 0) {
        snprintf(text, sizeof(text), "resize2fs status %d", rc);
        return fail(part_node, text);
    }
    printf("growroot: result: %s grown to %llu sectors\n", part_node, new_count);
    return 0;
}
