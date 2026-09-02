/**
 * A flash drive as a disk: its nodes, its volumes, and what happens when it
 * is pulled out.
 *
 * The program takes one word saying which part to run, because the harness
 * has to pull the stick out between some of them and a pulled stick cannot
 * be arranged from inside the machine:
 *
 * * `report`: `/dev/usb0` exists, is a block device, and `/proc/usb` names
 *   it with a size.
 * * `volumes`: the two volumes on the stick mount, read and unmount, and a
 *   raw read of `/dev/usb0` gives the table their partitions came from.
 * * `write`: a file written to the FAT volume, synced, unmounted, and read
 *   back after the volume is mounted again; the unmount must have flushed
 *   the stick's own cache, which the stick's line in `/proc/usb` counts.
 * * `hold`: the volume is mounted, a file is opened and read in a loop, and
 *   `USBDISKPROBE HOLDING` is printed so the harness can pull the stick.
 *   The read that meets the pull answers `EIO`; so does a write; `umount2`
 *   is `EBUSY` while the file is open and succeeds once it is closed.
 * * `replug`: the volume mounts again and the file is still there, on a
 *   stick that inherits nothing from the one before it.
 * * `free`: the number of free names `/proc/usb` reports, printed, so the
 *   harness can compare it across a run of plugs and pulls.
 * * `super`: a stick with no partition table mounts through `/dev/usb0`.
 * * `throughput`: 16 MiB read from `/dev/usb0` in 32 KiB pieces, timed by
 *   the monotonic clock and printed as `USBDISKPROBE THROUGHPUT N KB/s`.
 *   The number is recorded, not judged: it is what this milestone's
 *   page-sized request gives under the emulator.
 *
 * Prints "USBDISKPROBE OK" or "USBDISKPROBE FAIL: reason". Freestanding;
 * direct int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT          1
#define SYS_READ          3
#define SYS_WRITE         4
#define SYS_OPEN          5
#define SYS_CLOSE         6
#define SYS_UNLINK        10
#define SYS_MOUNT         21
#define SYS_MKDIR         39
#define SYS_UMOUNT2       52
#define SYS_SYNC          36
#define SYS_FSYNC         118
#define SYS_STAT64        195
#define SYS_NANOSLEEP     162
#define SYS_CLOCK_GETTIME 265

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

static int sys1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int sys2(int n, int a, int b)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static int sys5(int n, int a, int b, int c, int d, int e)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory");
    return r;
}

static void sys_exit(int status) __attribute__((noreturn));
static void sys_exit(int status)
{
    sys1(SYS_EXIT, status);
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
    sys3(SYS_WRITE, 1, (int)s, (int)strlen(s));
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

        sys3(SYS_WRITE, 1, (int)&c, 1);
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
static int slurp(const char *path)
{
    int fd = sys3(SYS_OPEN, (int)path, O_RDONLY, 0);
    int n;

    if (fd < 0) {
        return fd;
    }
    n = sys3(SYS_READ, fd, (int)text, (int)sizeof(text) - 1);
    sys1(SYS_CLOSE, fd);
    if (n < 0) {
        return n;
    }
    text[n] = '\0';
    return n;
}

static void sleep_ms(int ms)
{
    struct {
        int sec;
        int nsec;
    } ts;

    ts.sec = ms / 1000;
    ts.nsec = (ms % 1000) * 1000000;
    sys2(SYS_NANOSLEEP, (int)&ts, 0);
}

static int mount_at(const char *source, const char *type, int flags)
{
    sys2(SYS_MKDIR, (int)MNT, 0755);
    return sys5(SYS_MOUNT, (int)source, (int)MNT, (int)type, flags, 0);
}

/* stat's mode is at offset 16 of the kernel's stat64, which is the layout
   the other probes read it at. S_IFBLK is 0060000. */
static unsigned int mode_of(const char *path)
{
    static unsigned char st[144];
    int i;

    /* Cleared first, so a field the kernel does not fill reads as zero
       rather than as what the last call left there. */
    for (i = 0; i < (int)sizeof(st); i++) {
        st[i] = 0;
    }
    if (sys2(SYS_STAT64, (int)path, (int)st) != 0) {
        return 0;
    }
    return (unsigned int)st[16] | ((unsigned int)st[17] << 8) | ((unsigned int)st[18] << 16) |
           ((unsigned int)st[19] << 24);
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
    if (mode_of("/dev/usb0") == 0) {
        fail("/dev/usb0 does not stat");
    }
    if ((mode_of("/dev/usb0") & 0170000u) != 0060000u) {
        fail("/dev/usb0 is not a block device");
    }
    if (slurp("/proc/usb") < 0) {
        fail("/proc/usb did not read");
    }
    if (!contains(text, "usb0 ")) {
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

static void part_volumes(void)
{
    int fd;
    int n;

    if (mount_at("/dev/usb0p1", "vfat", MS_RDONLY) != 0) {
        fail("the FAT volume would not mount");
    }
    n = slurp(MNT "/hello.txt");
    if (n <= 0) {
        fail("the file on the FAT volume did not read");
    }
    if (!contains(text, "hello from the stick")) {
        fail("the file on the FAT volume holds something else");
    }
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
        fail("the FAT volume would not unmount");
    }
    if (mount_at("/dev/usb0p2", "exfat", MS_RDONLY) != 0) {
        fail("the exFAT volume would not mount");
    }
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
        fail("the exFAT volume would not unmount");
    }

    /* The whole node is the table the two partitions came from. */
    fd = sys3(SYS_OPEN, (int)"/dev/usb0", O_RDONLY, 0);
    if (fd < 0) {
        fail("/dev/usb0 would not open");
    }
    n = sys3(SYS_READ, fd, (int)text, 512);
    sys1(SYS_CLOSE, fd);
    if (n != 512) {
        fail("a sector of /dev/usb0 did not read");
    }
    if ((unsigned char)text[510] != 0x55 || (unsigned char)text[511] != 0xAA) {
        fail("the first sector of /dev/usb0 is not a table");
    }
    /* And it is the table the two partitions came from: either the MBR with
       the FAT32 and exFAT entries the fixture wrote, or the protective entry
       a GUID table sits behind, which is the other fixture. */
    {
        unsigned char type0 = (unsigned char)text[446 + 4];
        unsigned char type1 = (unsigned char)text[446 + 16 + 4];

        if (!((type0 == 0x0C && type1 == 0x07) || type0 == 0xEE)) {
            fail("the table on /dev/usb0 is not the one the partitions came from");
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

    if (mount_at("/dev/usb0p1", "vfat", 0) != 0) {
        fail("the FAT volume would not mount for writing");
    }
    fd = sys3(SYS_OPEN, (int)MNT "/written.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail("the file would not be created");
    }
    if (sys3(SYS_WRITE, fd, (int)"a stick takes a write\n", 22) != 22) {
        fail("the write was short");
    }
    if (sys1(SYS_FSYNC, fd) != 0) {
        fail("fsync failed");
    }
    sys1(SYS_CLOSE, fd);
    sys1(SYS_SYNC, 0);
    /* The unmount tells the stick to put its own cache on the medium, which
       its line in /proc/usb counts. A volume whole on the wire is not yet
       whole on the medium. */
    flushes_before = stick_flushes();
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
        fail("the volume would not unmount after the write");
    }
    if (flushes_before < 0 || stick_flushes() <= flushes_before) {
        fail("the unmount did not flush the stick's cache");
    }
    if (mount_at("/dev/usb0p1", "vfat", MS_RDONLY) != 0) {
        fail("the volume would not mount again");
    }
    n = slurp(MNT "/written.txt");
    if (n != 22 || !contains(text, "a stick takes a write")) {
        fail("what was written did not come back");
    }
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
        fail("the volume would not unmount at the end");
    }
    ok();
}

static void part_hold(void)
{
    int fd;
    int reads = 0;
    int gone = 0;
    char buf[512];

    if (mount_at("/dev/usb0p1", "vfat", 0) != 0) {
        fail("the volume would not mount");
    }
    fd = sys3(SYS_OPEN, (int)MNT "/pattern.bin", O_RDONLY, 0);
    if (fd < 0) {
        fail("the file on the volume would not open");
    }
    puts("USBDISKPROBE HOLDING");

    /* Read until the stick is pulled out from under the reader: the file is
       opened again at its end, so it is read over and over, and the read
       that is in flight when the stick is pulled is the one that must
       answer EIO. */
    while (reads < 20000) {
        int n = sys3(SYS_READ, fd, (int)buf, (int)sizeof(buf));

        reads++;
        if (n == -EIO) {
            gone = 1;
            break;
        }
        if (n < 0) {
            fail("a read on the mounted stick failed with something other than EIO");
        }
        if (n == 0) {
            /* Back to the beginning: the descriptor cannot seek here, so
               the file is opened again. */
            sys1(SYS_CLOSE, fd);
            fd = sys3(SYS_OPEN, (int)MNT "/pattern.bin", O_RDONLY, 0);
            if (fd < 0) {
                /* The pull landed between the close and the open; the read
                   that meets it is made on a descriptor opened before the
                   pull, which the shell's own copy of the directory gives. */
                if (fd != -EIO) {
                    fail("the file would not open again, and not for EIO");
                }
                fd = sys3(SYS_OPEN, (int)MNT, O_RDONLY, 0);
                if (fd < 0) {
                    fail("the mount point would not open after the pull");
                }
                gone = 1;
                break;
            }
        }
        sleep_ms(1);
    }
    if (!gone) {
        fail("the stick was never pulled out");
    }
    if (sys3(SYS_READ, fd, (int)buf, (int)sizeof(buf)) != -EIO) {
        fail("a read after the pull did not answer EIO");
    }
    if (sys3(SYS_OPEN, (int)MNT "/hello.txt", O_RDONLY, 0) != -EIO) {
        fail("an open on the pulled stick's mount did not answer EIO");
    }
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != -EBUSY) {
        fail("the mount unmounted with a description still open on it");
    }
    sys1(SYS_CLOSE, fd);
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
        fail("the mount would not unmount after its file was closed");
    }
    ok();
}

static void part_replug(void)
{
    if (mount_at("/dev/usb0p1", "vfat", MS_RDONLY) != 0) {
        fail("the volume on the stick plugged back in would not mount");
    }
    if (slurp(MNT "/hello.txt") <= 0) {
        fail("the file on the volume did not read after the replug");
    }
    if (!contains(text, "hello from the stick")) {
        fail("the volume holds something else after the replug");
    }
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
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
        int sec;
        int nsec;
    } before, after;
    int fd = sys3(SYS_OPEN, (int)"/dev/usb0", O_RDONLY, 0);
    int total = 0;
    int ms;

    if (fd < 0) {
        fail("/dev/usb0 would not open");
    }
    sys2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (int)&before);
    while (total < 16 * 1024 * 1024) {
        int n = sys3(SYS_READ, fd, (int)big, (int)sizeof(big));

        if (n <= 0) {
            fail("a read of /dev/usb0 failed before 16 MiB");
        }
        total += n;
    }
    sys2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (int)&after);
    sys1(SYS_CLOSE, fd);
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

static void part_super(void)
{
    if (mount_at("/dev/usb0", "exfat", MS_RDONLY) != 0) {
        fail("the stick with no table would not mount through its whole node");
    }
    if (sys2(SYS_UMOUNT2, (int)MNT, 0) != 0) {
        fail("the stick with no table would not unmount");
    }
    ok();
}

void probe_main(unsigned int *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned int *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    const char *what = argc > 1 ? argv[1] : "report";

    if (same(what, "report")) {
        part_report();
    }
    if (same(what, "volumes")) {
        part_volumes();
    }
    if (same(what, "write")) {
        part_write();
    }
    if (same(what, "hold")) {
        part_hold();
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
    fail("no such part");
}
