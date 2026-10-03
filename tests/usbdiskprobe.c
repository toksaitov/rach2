/**
 * A flash drive as a disk: its nodes, its volumes, and what happens when it
 * is pulled out.
 *
 * The program takes one word saying which part to run, because the harness
 * has to pull the stick out between some of them and a pulled stick cannot
 * be arranged from inside the machine:
 *
 * * `report`: `/dev/disk0` exists, is a block device, and `/proc/usb` names
 *   it with a size.
 * * `volumes`: the two volumes on the stick mount, read and unmount, and a
 *   raw read of `/dev/disk0` gives the table their partitions came from.
 * * `write`: a file written to the FAT volume, synced, unmounted, and read
 *   back after the volume is mounted again; the unmount must have flushed
 *   the stick's own cache, which the stick's line in `/proc/usb` counts.
 * * `hold` and `holdexfat`: warm FAT32 or exFAT caches, then signal the pull
 *   with `USBDISKPROBE HOLDING` and read the uncached disk until `EIO`.
 *   File access and cached lookup must then fail; the listings omit the disk
 *   and mount. Unmount is busy while files are open and succeeds after close.
 * * `holdnested` and `holdnestedexfat`: the same with the other volume mounted
 *   below the first. The child must unmount before the parent can be released.
 * * `replug`: the volume mounts again and the file is still there, on a
 *   stick that inherits nothing from the one before it.
 * * `free`: the number of free names `/proc/usb` reports, printed, so the
 *   harness can compare it across a run of plugs and pulls.
 * * `super`: a stick with no partition table mounts through `/dev/disk0`.
 * * `pullread`: `/dev/disk0` read in 32 KiB pieces, `USBDISKPROBE READING`
 *   printed first, until a read answers `EIO`, then `USBDISKPROBE READ
 *   FAILED`; the harness pulls the stick during the reads and reads the
 *   order of that line and the kernel's `given back` line.
 * * `throughput`: 16 MiB read from `/dev/disk0` in 32 KiB pieces, timed by
 *   the monotonic clock and printed as `USBDISKPROBE THROUGHPUT N KB/s`.
 *   The number is recorded, not judged: it is what this milestone's
 *   page-sized request gives under the emulator.
 *
 * Prints "USBDISKPROBE OK" or "USBDISKPROBE FAIL: reason". Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"

#define CLOCK_MONOTONIC 1

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000

#define MS_RDONLY 1

#define EIO   5
#define EBUSY 16

#define MNT "/mnt"

static void sys_exit(int status) __attribute__((noreturn));
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static void put(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
}

static void puts(const char *s)
{
    put(s);
    put("\n");
}

static void putnum(int v)
{
    char digits[12];
    int n = 0;

    if (v < 0) {
        put("-");
        v = -v;
    }
    if (v == 0) {
        digits[n++] = '0';
    }
    while (v > 0) {
        digits[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        char c = digits[--n];

        syscall3(SYS_WRITE, 1, (long)&c, 1);
    }
}

static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why)
{
    put("USBDISKPROBE FAIL: ");
    puts(why);
    sys_exit(1);
}

static void ok(void) __attribute__((noreturn));
static void ok(void)
{
    puts("USBDISKPROBE OK");
    sys_exit(0);
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int contains(const char *hay, const char *needle)
{
    const char *h;

    for (h = hay; *h; h++) {
        const char *a = h;
        const char *b = needle;

        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b) {
            return 1;
        }
    }
    return 0;
}

static char text[2048];

/** Read a whole small file into `text`; the length, or a negative errno. */
static int slurp(const char *path);

/* A counter of /proc/disk by its label, or 0 when it is not there. */
static int disk_counter(const char *label)
{
    int i, j;

    if (slurp("/proc/disk") <= 0) {
        return 0;
    }
    for (i = 0; text[i]; i++) {
        for (j = 0; label[j] && text[i + j] == label[j]; j++) {
        }
        if (!label[j]) {
            int v = 0;

            i += j;
            while (text[i] >= '0' && text[i] <= '9') {
                v = v * 10 + (text[i] - '0');
                i++;
            }
            return v;
        }
    }
    return 0;
}

static int slurp(const char *path)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int n;

    if (fd < 0) {
        return fd;
    }
    n = syscall3(SYS_READ, fd, (long)text, (int)sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n < 0) {
        return n;
    }
    text[n] = '\0';
    return n;
}

static void sleep_ms(int ms)
{
    struct {
        long sec;
        long nsec;
    } ts;

    ts.sec = ms / 1000;
    ts.nsec = (ms % 1000) * 1000000;
    syscall2(SYS_NANOSLEEP, (long)&ts, 0);
}

static int mount_at(const char *source, const char *type, int flags)
{
    syscall2(SYS_MKDIR, (long)MNT, 0755);
    return syscall5(SYS_MOUNT, (long)source, (long)MNT, (long)type, flags, 0);
}

/* stat's mode, at KSTAT_MODE in the kernel's stat, which is where the other
   probes read it. S_IFBLK is 0060000. */
static unsigned int mode_of(const char *path)
{
    static unsigned char st[KSTAT_SIZE];
    int i;

    /* Cleared first, so a field the kernel does not fill reads as zero
       rather than as what the last call left there. */
    for (i = 0; i < (int)sizeof(st); i++) {
        st[i] = 0;
    }
    if (syscall2(SYS_STAT, (long)path, (long)st) != 0) {
        return 0;
    }
    return (unsigned int)st[KSTAT_MODE] | ((unsigned int)st[KSTAT_MODE + 1] << 8) |
           ((unsigned int)st[KSTAT_MODE + 2] << 16) | ((unsigned int)st[KSTAT_MODE + 3] << 24);
}

/** The number after `disks ` in /proc/usb, or -1 when there is no such line. */
static int free_names(void)
{
    int at;
    int n = slurp("/proc/usb");

    if (n < 0) {
        return -1;
    }
    for (at = 0; at + 6 < n; at++) {
        if (text[at] == 'd' && text[at + 1] == 'i' && text[at + 2] == 's' && text[at + 3] == 'k' &&
            text[at + 4] == 's' && text[at + 5] == ' ') {
            int value = 0;
            int i = at + 6;

            if (text[i] < '0' || text[i] > '9') {
                return -1;
            }
            while (text[i] >= '0' && text[i] <= '9') {
                value = value * 10 + (text[i] - '0');
                i++;
            }
            return value;
        }
    }
    return -1;
}

static void part_report(void)
{
    if (mode_of("/dev/disk0") == 0) {
        fail("/dev/disk0 does not stat");
    }
    if ((mode_of("/dev/disk0") & 0170000u) != 0060000u) {
        fail("/dev/disk0 is not a block device");
    }
    if (slurp("/proc/usb") < 0) {
        fail("/proc/usb did not read");
    }
    if (!contains(text, "disk0 ")) {
        fail("/proc/usb does not name the stick");
    }
    if (!contains(text, " sectors ")) {
        fail("/proc/usb gives the stick no size");
    }
    if (free_names() < 0) {
        fail("/proc/usb does not count the names");
    }
    put("USBDISKPROBE FREE ");
    putnum(free_names());
    put("\n");
    ok();
}

/* The two volumes' nodes: the table's numbering, given by the suite when
   the entries are not the first two. */
static const char *fat_node = "/dev/disk0p1";
static const char *exfat_node = "/dev/disk0p2";

static void part_volumes(void)
{
    int fd;
    int n;

    if (mount_at(fat_node, "vfat", MS_RDONLY) != 0) {
        fail("the FAT volume would not mount");
    }
    n = slurp(MNT "/hello.txt");
    if (n <= 0) {
        fail("the file on the FAT volume did not read");
    }
    if (!contains(text, "hello from the stick")) {
        fail("the file on the FAT volume holds something else");
    }
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("the FAT volume would not unmount");
    }
    if (mount_at(exfat_node, "exfat", MS_RDONLY) != 0) {
        fail("the exFAT volume would not mount");
    }
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("the exFAT volume would not unmount");
    }

    /* The whole node is the table the two partitions came from. */
    fd = syscall3(SYS_OPEN, (long)"/dev/disk0", O_RDONLY, 0);
    if (fd < 0) {
        fail("/dev/disk0 would not open");
    }
    n = syscall3(SYS_READ, fd, (long)text, 512);
    syscall1(SYS_CLOSE, fd);
    if (n != 512) {
        fail("a sector of /dev/disk0 did not read");
    }
    if ((unsigned char)text[510] != 0x55 || (unsigned char)text[511] != 0xAA) {
        fail("the first sector of /dev/disk0 is not a table");
    }
    /* And it is the table the two partitions came from: either the MBR with
       the FAT32 and exFAT entries the fixture wrote, or the protective entry
       a GUID table sits behind, which is the other fixture. */
    {
        unsigned char type0 = (unsigned char)text[446 + 4];
        unsigned char type1 = (unsigned char)text[446 + 16 + 4];

        if (!((type0 == 0x0C && type1 == 0x07) || type0 == 0xEE)) {
            fail("the table on /dev/disk0 is not the one the partitions came from");
        }
    }
    ok();
}

/** The number after `flushes ` on the stick's line of /proc/usb, or -1. */
static int stick_flushes(void)
{
    int n = slurp("/proc/usb");
    int at;

    if (n <= 0) {
        return -1;
    }
    for (at = 0; at + 8 < n; at++) {
        if (text[at] == 'f' && text[at + 1] == 'l' && text[at + 2] == 'u' && text[at + 3] == 's' &&
            text[at + 4] == 'h' && text[at + 5] == 'e' && text[at + 6] == 's' &&
            text[at + 7] == ' ') {
            int value = 0;
            int i = at + 8;

            if (text[i] < '0' || text[i] > '9') {
                return -1;
            }
            while (text[i] >= '0' && text[i] <= '9') {
                value = value * 10 + (text[i] - '0');
                i++;
            }
            return value;
        }
    }
    return -1;
}

static void part_write(void)
{
    int fd;
    int n;
    int flushes_before;

    if (mount_at("/dev/disk0p1", "vfat", 0) != 0) {
        fail("the FAT volume would not mount for writing");
    }
    fd = syscall3(SYS_OPEN, (long)MNT "/written.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail("the file would not be created");
    }
    if (syscall3(SYS_WRITE, fd, (long)"a stick takes a write\n", 22) != 22) {
        fail("the write was short");
    }
    if (syscall1(SYS_FSYNC, fd) != 0) {
        fail("fsync failed");
    }
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_SYNC, 0);
    /* The unmount tells the stick to put its own cache on the medium, which
       its line in /proc/usb counts. A volume whole on the wire is not yet
       whole on the medium. */
    flushes_before = stick_flushes();
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("the volume would not unmount after the write");
    }
    if (flushes_before < 0 || stick_flushes() <= flushes_before) {
        fail("the unmount did not flush the stick's cache");
    }
    if (mount_at("/dev/disk0p1", "vfat", MS_RDONLY) != 0) {
        fail("the volume would not mount again");
    }
    n = slurp(MNT "/written.txt");
    if (n != 22 || !contains(text, "a stick takes a write")) {
        fail("what was written did not come back");
    }
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("the volume would not unmount at the end");
    }
    ok();
}

/** Keep file and directory cache entries across a pull on either volume.
 * `USBDISKPROBE HOLDING` is said after the first read has completed, so
 * the pull the suite makes on the word lands on a process already reading
 * the stick.
 */
static void part_hold(const char *source, const char *type, int children)
{
    int fd;
    int dir;
    int raw;
    int rw;
    int nested = -1;
    int reads = 0;
    int gone = 0;
    char buf[512];

    if (mount_at(source, type, 0) != 0) {
        fail("the volume would not mount");
    }
    if (children) {
        if (syscall2(SYS_MKDIR, (long)MNT "/nested", 0755) != 0 ||
            syscall5(SYS_MOUNT, (long)(source == fat_node ? exfat_node : fat_node),
                     (long)MNT "/nested", (long)(source == fat_node ? "exfat" : "vfat"), MS_RDONLY,
                     0) != 0) {
            fail("the child volume would not mount");
        }
        nested = syscall3(SYS_OPEN, (long)MNT "/nested", O_RDONLY, 0);
        if (nested < 0) {
            fail("the child mount would not open");
        }
    }
    rw = syscall3(SYS_OPEN, (long)MNT "/held.txt", O_RDWR | O_CREAT, 0644);
    if (rw < 0) {
        fail("a file for writing would not be created on the volume");
    }
    if (syscall3(SYS_WRITE, rw, (long)"kept\n", 5) != 5 || syscall1(SYS_FSYNC, rw) != 0) {
        fail("the held file would not write and sync");
    }
    fd = syscall3(SYS_OPEN, (long)MNT "/held.txt", O_RDONLY, 0);
    if (fd < 0 || syscall3(SYS_READ, fd, (long)buf, 5) != 5 || syscall3(SYS_LSEEK, fd, 0, 0) != 0) {
        fail("the held file would not read before removal");
    }
    dir = syscall3(SYS_OPEN, (long)MNT, O_RDONLY, 0);
    if (dir < 0 || syscall3(SYS_GETDENTS64, dir, (long)buf, (int)sizeof(buf)) <= 0) {
        fail("the directory would not list before removal");
    }
    /* The reads that meet the pull go to the whole node, which has no cache
       in front of it: every one of them is a request on the stick, so the
       process is inside such a request when the stick goes. The file stays
       open on the mount meanwhile, which is what the unmount must refuse. */
    raw = syscall3(SYS_OPEN, (long)source, O_RDONLY, 0);
    if (raw < 0) {
        fail("the partition would not open");
    }
    while (reads < 2000000) {
        int n = syscall3(SYS_READ, raw, (long)buf, (int)sizeof(buf));

        reads++;
        if (n == -EIO) {
            gone = 1;
            break;
        }
        if (n < 0) {
            fail("a read of the stick failed with something other than EIO");
        }
        if (reads == 1) {
            puts("USBDISKPROBE HOLDING");
        }
        if (n == 0) {
            syscall1(SYS_CLOSE, raw);
            raw = syscall3(SYS_OPEN, (long)source, O_RDONLY, 0);
            if (raw < 0) {
                gone = 1; /* the node went with the stick */
                break;
            }
        }
    }
    if (!gone) {
        fail("the stick was never pulled out");
    }
    if (syscall3(SYS_GETDENTS64, dir, (long)buf, (int)sizeof(buf)) != -EIO) {
        fail("a cached directory listed after removal");
    }
    if (syscall3(SYS_OPEN, (long)MNT, O_RDONLY, 0) != -EIO ||
        syscall2(SYS_STAT, (long)MNT, (long)buf) != -EIO) {
        fail("the removed mount still resolves");
    }
    if (slurp("/proc/disk") < 0 || contains(text, "disk 0 ") || contains(text, "identity 0 ") ||
        contains(text, "part 0 ")) {
        fail("the removed stick is still listed as a disk");
    }
    if (slurp("/proc/mounts") < 0 || contains(text, " " MNT " ")) {
        fail("the removed volume is still listed as mounted");
    }
    if (syscall3(SYS_READ, fd, (long)buf, (int)sizeof(buf)) != -EIO) {
        fail("a read of the mounted file after the pull did not answer EIO");
    }
    if (syscall3(SYS_WRITE, rw, (long)"gone\n", 5) != -EIO) {
        fail("a write to the mounted file after the pull did not answer EIO");
    }
    if (syscall3(SYS_OPEN, (long)MNT "/held.txt", O_RDONLY, 0) != -EIO) {
        fail("an open on the pulled stick's mount did not answer EIO");
    }
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != -EBUSY) {
        fail("the mount unmounted with a description still open on it");
    }
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_CLOSE, rw);
    syscall1(SYS_CLOSE, dir);
    if (raw >= 0) {
        syscall1(SYS_CLOSE, raw);
    }
    if (children) {
        if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != -EBUSY) {
            fail("the parent unmounted while its child mount remained");
        }
        if (syscall2(SYS_UMOUNT2, (long)MNT "/nested", 0) != -EBUSY) {
            fail("the removed child mount did not retain its open description");
        }
        syscall1(SYS_CLOSE, nested);
        if (syscall2(SYS_UMOUNT2, (long)MNT "/nested", 0) != 0) {
            fail("the child mount would not unmount below its removed parent");
        }
    }
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("the mount would not unmount after its files were closed");
    }
    ok();
}

static void part_replug(void)
{
    if (mount_at("/dev/disk0p1", "vfat", MS_RDONLY) != 0) {
        fail("the volume on the stick plugged back in would not mount");
    }
    if (slurp(MNT "/hello.txt") <= 0) {
        fail("the file on the volume did not read after the replug");
    }
    if (!contains(text, "hello from the stick")) {
        fail("the volume holds something else after the replug");
    }
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("the volume would not unmount after the replug");
    }
    ok();
}

static void part_free(void)
{
    int free_count = free_names();

    put("USBDISKPROBE FREE ");
    putnum(free_count);
    put("\n");
    if (free_count < 0) {
        fail("/proc/usb does not count the names");
    }
    ok();
}

static char big[32768];

static void part_throughput(void)
{
    struct {
        long sec;
        long nsec;
    } before, after;
    int fd = syscall3(SYS_OPEN, (long)"/dev/disk0", O_RDONLY, 0);
    int total = 0;
    int ms;

    if (fd < 0) {
        fail("/dev/disk0 would not open");
    }
    syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    while (total < 16 * 1024 * 1024) {
        int n = syscall3(SYS_READ, fd, (long)big, (int)sizeof(big));

        if (n <= 0) {
            fail("a read of /dev/disk0 failed before 16 MiB");
        }
        total += n;
    }
    syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    syscall1(SYS_CLOSE, fd);
    ms = (after.sec - before.sec) * 1000 + (after.nsec - before.nsec) / 1000000;
    if (ms <= 0) {
        ms = 1;
    }
    put("USBDISKPROBE THROUGHPUT ");
    putnum((int)((16 * 1024 * 1000) / ms));
    put(" KB/s over ");
    putnum(ms);
    put(" ms\n");
    ok();
}

/*
 * The pull during a transfer: a child reads the stick in large calls until
 * one fails, and `USBDISKPROBE READING` is said once the block layer counts
 * a request in flight, `disk-in-flight` in /proc/disk, so the pull the
 * suite makes on the word lands on a data stage and not between two calls.
 */
static void part_pullread(void)
{
    int fd = syscall3(SYS_OPEN, (long)"/dev/disk0", O_RDONLY, 0);
    int reads = 0;
    int child, status, tries;

    if (fd < 0) {
        fail("/dev/disk0 would not open");
    }
    child = syscall1(SYS_FORK, 0);
    if (child < 0) {
        fail("no child to read the stick");
    }
    if (child == 0) {
        while (reads < 2000000) {
            int n = syscall3(SYS_READ, fd, (long)big, (int)sizeof(big));

            reads++;
            if (n == -EIO) {
                syscall1(SYS_EXIT, 0);
            }
            if (n < 0) {
                syscall1(SYS_EXIT, 3);
            }
            if (n == 0) {
                syscall1(SYS_CLOSE, fd);
                fd = syscall3(SYS_OPEN, (long)"/dev/disk0", O_RDONLY, 0);
                if (fd < 0) {
                    syscall1(SYS_EXIT, 4);
                }
            }
        }
        syscall1(SYS_EXIT, 5);
    }
    syscall1(SYS_CLOSE, fd);
    for (tries = 0; tries < 2000000; tries++) {
        if (disk_counter("disk-in-flight ") > 0) {
            break;
        }
    }
    if (tries == 2000000) {
        fail("no request of the child's was in flight to be pulled");
    }
    puts("USBDISKPROBE READING");
    if (syscall3(SYS_WAITPID, child, (long)&status, 0) != child) {
        fail("waiting for the reading child");
    }
    switch ((status >> 8) & 0xFF) {
    case 0:
        puts("USBDISKPROBE READ FAILED");
        ok();
    case 3:
        fail("a read of the stick failed with something other than EIO");
    case 4:
        fail("the node went before a read in flight failed");
    default:
        fail("the stick was never pulled out");
    }
}

static void part_super(void)
{
    if (mount_at("/dev/disk0", "exfat", MS_RDONLY) != 0) {
        fail("the stick with no table would not mount through its whole node");
    }
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("the stick with no table would not unmount");
    }
    ok();
}

/*
 * `three`: three sticks at once, each with a FAT32 first partition holding
 * the same file, mounted and read one after the other.
 */
static void part_three(void)
{
    static const char *const nodes[3] = {"/dev/disk0p1", "/dev/disk1p1", "/dev/disk2p1"};
    int i;

    for (i = 0; i < 3; i++) {
        if (mount_at(nodes[i], "vfat", MS_RDONLY) != 0) {
            fail("one of the three sticks would not mount");
        }
        if (slurp(MNT "/hello.txt") <= 0 || !contains(text, "hello from the stick")) {
            fail("one of the three sticks did not read its file");
        }
        if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
            fail("one of the three sticks would not unmount");
        }
    }
    ok();
}

/*
 * `pullone`: with three sticks in, keep reading the middle one's whole node
 * through the queue and say so; the suite pulls it, the read in flight
 * answers EIO, and the two beside it still mount and read.
 *
 * `which` is the disk held and pulled, 0, 1 or 2. It is 1 unless a caller
 * names another: a machine with two controllers numbers its disks in the order
 * they were enumerated across both, so which disk is on which controller is
 * the caller's to say.
 * The pull of one stick among three: the reads run until the stick is
 * gone, and `USBDISKPROBE HOLDING` is said after the eighth successful
 * read, which is the fewest the row requires before the pull, so the suite
 * pulls on the word and the count holds whenever the pull lands.
 */
static void part_pullone(int which)
{
    static char sector[512];
    static char node[16] = "/dev/disk1";
    static char part[16] = "/dev/disk1p1";
    static char other[2][16] = {"/dev/disk0p1", "/dev/disk0p1"};
    int fd;
    int reads = 0;
    int n, i, o = 0;

    node[9] = (char)('0' + which);
    part[9] = (char)('0' + which);
    for (i = 0; i < 3; i++) {
        if (i == which) {
            continue;
        }
        other[o][9] = (char)('0' + i);
        o++;
    }

    fd = syscall3(SYS_OPEN, (long)node, O_RDONLY, 0);
    sector[0] = 0;
    if (fd < 0) {
        fail("the stick to be pulled would not open");
    }
    for (;;) {
        n = syscall3(SYS_READ, fd, (long)sector, 512);
        if (n == 512) {
            reads++;
            if (reads == 8) {
                puts("USBDISKPROBE HOLDING");
            }
            continue;
        }
        if (n == 0) {
            syscall3(SYS_LSEEK, fd, 0, 0);
            continue;
        }
        if (n != -EIO) {
            fail("the read of the pulled stick failed with something other than EIO");
        }
        break;
    }
    syscall1(SYS_CLOSE, fd);
    if (reads < 8) {
        fail("too few reads ran before the pull to have met it");
    }
    for (i = 0; i < 2; i++) {
        if (mount_at(other[i], "vfat", MS_RDONLY) != 0 || slurp(MNT "/hello.txt") <= 0 ||
            !contains(text, "hello from the stick") || syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
            fail("a stick beside the pulled one was harmed by the pull");
        }
    }
    if (mount_at(part, "vfat", MS_RDONLY) == 0) {
        fail("the pulled stick still mounts");
    }
    ok();
}

/*
 * `big`: a stick past two terabytes, its partition near the end, listed at
 * its size, mounted, written and read back.
 */
static void part_big(void)
{
    int fd;

    if (slurp("/proc/usb") <= 0 || !contains(text, " 4823449600 sectors ")) {
        fail("/proc/usb does not list the stick at 4823449600 sectors");
    }
    if (slurp("/proc/disk") <= 0 || !contains(text, "part 0 1 4294969344 65536 ")) {
        fail("/proc/disk does not place the partition past sector 2^32");
    }
    if (mount_at("/dev/disk0p1", "vfat", 0) != 0) {
        fail("the far partition would not mount");
    }
    fd = syscall3(SYS_OPEN, (long)MNT "/far.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)"written past two terabytes\n", 27) != 27) {
        fail("the file past two terabytes would not write");
    }
    syscall1(SYS_CLOSE, fd);
    if (slurp(MNT "/far.txt") != 27 || !contains(text, "written past two terabytes")) {
        fail("the file past two terabytes did not read back");
    }
    if (syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("the far partition would not unmount");
    }
    ok();
}

/*
 * `luns`: one device with two logical units, each a disk with a FAT32
 * partition, both listed and both mounted.
 */
static void part_luns(void)
{
    if (slurp("/proc/usb") <= 0 || !contains(text, "disk0 ") || !contains(text, "disk1 ")) {
        fail("/proc/usb does not list both units");
    }
    if (mount_at("/dev/disk0p1", "vfat", MS_RDONLY) != 0 || slurp(MNT "/hello.txt") <= 0 ||
        syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("the first unit would not mount and read");
    }
    if (mount_at("/dev/disk1p1", "vfat", MS_RDONLY) != 0 || slurp(MNT "/hello.txt") <= 0 ||
        syscall2(SYS_UMOUNT2, (long)MNT, 0) != 0) {
        fail("the second unit would not mount and read");
    }
    ok();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    const char *what = argc > 1 ? argv[1] : "report";

    if (argc > 3) {
        fat_node = argv[2];
        exfat_node = argv[3];
    }
    if (same(what, "report")) {
        part_report();
    }
    if (same(what, "three")) {
        part_three();
    }
    if (same(what, "pullone")) {
        /* `pullone [N]`: N is the disk held and pulled, 1 by default. */
        int which = 1;

        if (argc > 2 && argv[2][0] >= '0' && argv[2][0] <= '2' && argv[2][1] == '\0') {
            which = argv[2][0] - '0';
        }
        part_pullone(which);
    }
    if (same(what, "big")) {
        part_big();
    }
    if (same(what, "luns")) {
        part_luns();
    }
    if (same(what, "volumes")) {
        part_volumes();
    }
    if (same(what, "write")) {
        part_write();
    }
    if (same(what, "hold")) {
        part_hold(fat_node, "vfat", 0);
    }
    if (same(what, "holdexfat")) {
        part_hold(exfat_node, "exfat", 0);
    }
    if (same(what, "holdnested")) {
        part_hold(fat_node, "vfat", 1);
    }
    if (same(what, "holdnestedexfat")) {
        part_hold(exfat_node, "exfat", 1);
    }
    if (same(what, "replug")) {
        part_replug();
    }
    if (same(what, "free")) {
        part_free();
    }
    if (same(what, "super")) {
        part_super();
    }
    if (same(what, "throughput")) {
        part_throughput();
    }
    if (same(what, "pullread")) {
        part_pullread();
    }
    fail("no such part");
}
