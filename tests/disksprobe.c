/**
 * Two disks on one AHCI controller, both in the registry and both usable.
 *
 * The suite attaches a 64 MiB KRFFS disk on port 0 and a 20 MiB disk with
 * one FAT32 partition on port 1, and boots with `rach2.diskdiag` so the
 * whole-disk nodes exist. This requires `/proc/disk` to list both disks with
 * their ports and sizes, the nodes of both to exist and the missing
 * partitions' nodes not to, and a partition of each to mount and take a
 * write that reads back: the first disk's KRFFS volume at `/mnt0`, the
 * second's FAT32 volume at `/mnt1`. The suite then reads the FAT32 file from
 * the host with mtools, so the write reached the second disk and not the
 * first.
 *
 * Prints "DISKSPROBE OK" or "DISKSPROBE FAIL: reason". Freestanding; direct
 * system calls (probe.h).
 */

#include "probe.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000

#define DISK1_SECTORS 40960u /* the suite's second disk, 20 MiB */

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

/** Print "DISKSPROBE FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (long)"DISKSPROBE FAIL: ", 17);
    puts(why);
    syscall2(SYS_EXIT, 1, 0);
}

/** stat(2) of `path`: 0 when it exists, or a negated errno. */
static int xstat(const char *path)
{
    char st[KSTAT_SIZE];

    st[0] = 0;
    return syscall2(SYS_STAT, (long)path, (long)st);
}

/** Whether `s` begins with `prefix`. */
static int starts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

/** The sector count on the `/proc/disk` line `disk N TRANSPORT PORT SECTORS`
 * for the disk numbered `n` on `transport` port `port`, each number a single
 * digit, or 0 when there is no such line. */
static unsigned disk_sectors(const char *text, int n, const char *transport, int port)
{
    char want[24];
    int k = 0;
    const char *p;

    want[k++] = 'd';
    want[k++] = 'i';
    want[k++] = 's';
    want[k++] = 'k';
    want[k++] = ' ';
    want[k++] = (char)('0' + n);
    want[k++] = ' ';
    while (*transport) {
        want[k++] = *transport++;
    }
    want[k++] = ' ';
    want[k++] = (char)('0' + port);
    want[k++] = ' ';
    want[k] = 0;
    for (p = text; *p; p++) {
        if ((p == text || p[-1] == '\n') && starts(p, want)) {
            unsigned v = 0;

            p += k;
            while (*p >= '0' && *p <= '9') {
                v = v * 10u + (unsigned)(*p - '0');
                p++;
            }
            return v;
        }
    }
    return 0;
}

/** Write `text` to the new file `path`, read it back and compare; fail,
 * naming `path`, at the first step that does not hold. */
static void write_and_read_back(const char *path, const char *text)
{
    static char back[64];
    int fd, n = (int)strlen(text), got;

    fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail(path);
    }
    if (syscall3(SYS_WRITE, fd, (long)text, n) != n) {
        fail(path);
    }
    syscall2(SYS_CLOSE, fd, 0);
    fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        fail(path);
    }
    got = syscall3(SYS_READ, fd, (long)back, (int)sizeof(back));
    syscall2(SYS_CLOSE, fd, 0);
    if (got != n) {
        fail(path);
    }
    for (fd = 0; fd < n; fd++) {
        if (back[fd] != text[fd]) {
            fail(path);
        }
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Check the two disks' lines in /proc/disk and their nodes, then mount a
 * partition of each, write a file and read it back, and unmount both. */
void probe_main(unsigned long *sp)
{
    static char text[2048];
    int fd, n;
    unsigned s0, s1;

    (void)sp;
    fd = syscall3(SYS_OPEN, (long)"/proc/disk", O_RDONLY, 0);
    if (fd < 0) {
        fail("/proc/disk");
    }
    n = syscall3(SYS_READ, fd, (long)text, (int)sizeof(text) - 1);
    syscall2(SYS_CLOSE, fd, 0);
    if (n <= 0) {
        fail("reading /proc/disk");
    }
    text[n] = 0;
    s0 = disk_sectors(text, 0, "ahci", 0);
    s1 = disk_sectors(text, 1, "ahci", 1);
    if (s0 == 0) {
        fail("/proc/disk has no line for disk 0 on AHCI port 0");
    }
    if (s1 != DISK1_SECTORS) {
        fail("/proc/disk has no line for disk 1 on AHCI port 1 with its 40960 sectors");
    }
    if (s0 <= s1) {
        fail("disk 0 is not the larger disk");
    }
    if (disk_sectors(text, 2, "ahci", 2) != 0) {
        fail("/proc/disk lists a third disk");
    }

    if (xstat("/dev/disk0") != 0 || xstat("/dev/disk0p1") != 0) {
        fail("the nodes of disk 0");
    }
    if (xstat("/dev/disk1") != 0 || xstat("/dev/disk1p1") != 0) {
        fail("the nodes of disk 1");
    }
    if (xstat("/dev/disk1p2") == 0 || xstat("/dev/disk2") == 0) {
        fail("a node for a partition or a disk that is not there");
    }

    syscall2(SYS_MKDIR, (long)"/mnt0", 0755);
    syscall2(SYS_MKDIR, (long)"/mnt1", 0755);
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/mnt0", (long)"krffs", 0, 0) != 0) {
        fail("mounting disk 0's KRFFS partition");
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/disk1p1", (long)"/mnt1", (long)"vfat", 0, 0) != 0) {
        fail("mounting disk 1's FAT32 partition");
    }
    write_and_read_back("/mnt0/zero.txt", "disk zero says hello\n");
    write_and_read_back("/mnt1/one.txt", "disk one says hello\n");
    if (syscall2(SYS_UMOUNT2, (long)"/mnt1", 0) != 0 ||
        syscall2(SYS_UMOUNT2, (long)"/mnt0", 0) != 0) {
        fail("unmounting");
    }
    puts("DISKSPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}
