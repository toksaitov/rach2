/* Read, write, flush, discard and zero virtio-blk disks through the driver's
 * queues, and check what the kernel publishes of them.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, or without it guest.vblk.read or guest.vblk.write by the mode;
 * each mode's function says what its assertions check.
 */

#include "probe.h"
#include "probe_report.h"

#define O_RDONLY 0
#define O_RDWR   2
#define SEEK_SET 0

#define READ_BYTES (1024 * 1024)
#define CHUNK      (32 * 1024)
#define WNOHANG    1
#define SIGKILL    9

#define BLKDISCARD       0x1277
#define BLKDISCARDZEROES 0x127C
#define BLKSECDISCARD    0x127D
#define BLKZEROOUT       0x127F

#define EBADF      9
#define EPERM      1
#define EINVAL     22
#define EOPNOTSUPP 95

/* The readers a failure must end, 0 when none. */
static int children[2];

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

/** Report the failure `why` of the planned assertion now checked, print
 * "VBLKPROBE FAIL: " and `why`, kill and reap each reader still running, and
 * exit 1. */
static void fail(const char *why)
{
    int i, status;

    report_stop(why);
    syscall3(SYS_WRITE, 1, (long)"VBLKPROBE FAIL: ", 16);
    puts(why);
    for (i = 0; i < 2; i++) {
        if (children[i] > 0 && syscall3(SYS_WAITPID, children[i], (long)&status, WNOHANG) == 0) {
            syscall2(SYS_KILL, children[i], SIGKILL);
            syscall3(SYS_WAITPID, children[i], (long)&status, 0);
        }
    }
    syscall2(SYS_EXIT, 1, 0);
}

/** Read `bytes` of `node` in chunks; 0, or -1 on a short read. */
static int read_node(const char *node, int bytes)
{
    static char buf[CHUNK];
    int fd = syscall3(SYS_OPEN, (long)node, O_RDONLY, 0);
    int done = 0;

    buf[0] = 0; /* the read fills it through a cast cppcheck cannot follow */
    if (fd < 0) {
        return -1;
    }
    while (done < bytes) {
        if (syscall3(SYS_READ, fd, (long)buf, CHUNK) != CHUNK) {
            syscall2(SYS_CLOSE, fd, 0);
            return -1;
        }
        done += CHUNK;
    }
    syscall2(SYS_CLOSE, fd, 0);
    return 0;
}

/** Read the file `path` into `buf`, at most `cap` - 1 bytes and a NUL: the
 * bytes read, or -1 when it does not open. */
static int slurp(const char *path, char *buf, int cap)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int n, total = 0;

    if (fd < 0) {
        return -1;
    }
    while (total < cap - 1) {
        n = syscall3(SYS_READ, fd, (long)(buf + total), cap - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    syscall2(SYS_CLOSE, fd, 0);
    buf[total] = 0;
    return total;
}

/** Whether `line`, up to its newline, ends with the word `name`. */
static int line_names(const char *line, const char *name)
{
    const char *end = line, *p;
    int m = (int)strlen(name), k;

    while (*end && *end != '\n') {
        end++;
    }
    p = end - m;
    if (p <= line || (p[-1] != ' ' && p[-1] != '\t')) {
        return 0;
    }
    for (k = 0; k < m && p[k] == name[k]; k++) {
    }
    return k == m;
}

/** The sum of the counts on every line of /proc/interrupts, `file`, that
 * ends with `name`, and the number of such lines in `*lines`. */
static long count_named(const char *file, const char *name, int *lines)
{
    const char *p = file;
    long total = 0;

    *lines = 0;
    while (*p) {
        const char *line = p;

        while (*p >= '0' && *p <= '9') {
            p++;
        }
        if (p != line && *p == ':' && line_names(line, name)) {
            ++*lines;
            for (p++;;) {
                long value = 0;

                while (*p == ' ' || *p == '\t') {
                    p++;
                }
                if (*p < '0' || *p > '9') {
                    break;
                }
                while (*p >= '0' && *p <= '9') {
                    value = value * 10 + (*p++ - '0');
                }
                total += value;
            }
        }
        while (*p && *p != '\n') {
            p++;
        }
        if (*p) {
            p++;
        }
    }
    return total;
}

/** The decimal number at the start of the file `path`, or -1. */
static long file_number(const char *path)
{
    char text[64];
    long v = 0;
    int i;

    if (slurp(path, text, sizeof(text)) <= 0 || text[0] < '0' || text[0] > '9') {
        return -1;
    }
    for (i = 0; text[i] >= '0' && text[i] <= '9'; i++) {
        v = v * 10 + (text[i] - '0');
    }
    return v;
}

/** Whether the file `path` holds `want` and a newline. */
static int file_is(const char *path, const char *want)
{
    char text[64];
    int n = (int)strlen(want), k;

    if (slurp(path, text, sizeof(text)) != n + 1 || text[n] != '\n') {
        return 0;
    }
    for (k = 0; k < n && text[k] == want[k]; k++) {
    }
    return k == n;
}

/** Make `ioctl` request `request` of `fd` with the byte range `start` and
 * `len`, and return its result. */
static long range_ioctl(int fd, long request, unsigned long start, unsigned long len)
{
    unsigned long long range[2];

    range[0] = start;
    range[1] = len;
    return syscall3(SYS_IOCTL, fd, request, (long)range);
}

/**
 * The ordinary kernel, two disks on virtio-blk: `both-read`, each disk read
 * 1 MiB in chunks of 32 KiB by a child of its own; `interrupts`, the first
 * device with two vectors or more in /proc/interrupts, a queue's for each
 * processor, and the two devices' vectors counting at least sixteen
 * completions; `serial`, the
 * second disk's serial in sysfs; `discard-limit`, the first disk's
 * discard_max_bytes not 0, since the device takes discards; and `refused`,
 * BLKDISCARD -EBADF on a descriptor opened for reading and -EPERM on one
 * opened for writing, since only the recovery kernel writes a raw disk.
 */
static void read_row(void)
{
    static char text[8192];
    long before, after;
    int i, status, lines, fd;

    report_plan("both-read");
    report_plan("interrupts");
    report_plan("serial");
    report_plan("discard-limit");
    report_plan("refused");
    if (slurp("/proc/interrupts", text, sizeof(text)) <= 0) {
        fail("/proc/interrupts does not read");
    }
    before = count_named(text, "virtio0", &lines) + count_named(text, "virtio1", &i);
    for (i = 0; i < 2; i++) {
        children[i] = syscall1(SYS_FORK, 0);
        if (children[i] < 0) {
            fail("fork");
        }
        if (children[i] == 0) {
            syscall2(SYS_EXIT, read_node(i == 0 ? "/dev/disk0" : "/dev/disk1", READ_BYTES) ? 3 : 0,
                     0);
        }
    }
    for (i = 0; i < 2; i++) {
        if (syscall3(SYS_WAITPID, children[i], (long)&status, 0) != children[i] || status != 0) {
            fail("a reader did not read its disk");
        }
        children[i] = 0;
    }
    report_held();
    if (slurp("/proc/interrupts", text, sizeof(text)) <= 0) {
        fail("/proc/interrupts does not read");
    }
    after = count_named(text, "virtio1", &i) + count_named(text, "virtio0", &lines);
    if (lines < 2 || after - before < 16) {
        fail("the first device has fewer than two vectors, or the two devices counted fewer "
             "than 16 completions");
    }
    report_held();
    if (!file_is("/sys/block/disk1/device/serial", "rach2vblk")) {
        fail("/sys/block/disk1/device/serial is not rach2vblk");
    }
    report_held();
    if (file_number("/sys/block/disk0/queue/discard_max_bytes") <= 0) {
        fail("/sys/block/disk0/queue/discard_max_bytes is 0");
    }
    report_held();
    fd = syscall3(SYS_OPEN, (long)"/dev/disk0", O_RDONLY, 0);
    if (fd < 0 || range_ioctl(fd, BLKDISCARD, 0, 4096) != -EBADF) {
        fail("BLKDISCARD on a descriptor opened for reading is not EBADF");
    }
    syscall2(SYS_CLOSE, fd, 0);
    fd = syscall3(SYS_OPEN, (long)"/dev/disk0", O_RDWR, 0);
    if (fd >= 0 && range_ioctl(fd, BLKDISCARD, 0, 4096) != -EPERM) {
        fail("BLKDISCARD on the ordinary kernel is not EPERM");
    }
    if (fd >= 0) {
        syscall2(SYS_CLOSE, fd, 0);
    }
    report_held();
    report_end();
    puts("VBLKPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}

/** The byte of the pattern at offset `i` of the disk. */
static char pattern(unsigned long i)
{
    return (char)((i * 7u + (i >> 9)) | 1u);
}

/** Write the pattern over `bytes` of `fd` from offset 0; 0, or -1. */
static int write_pattern(int fd, unsigned long bytes)
{
    static char buf[CHUNK];
    unsigned long off, k;

    if (syscall3(SYS_LSEEK, fd, 0, SEEK_SET) != 0) {
        return -1;
    }
    for (off = 0; off < bytes; off += CHUNK) {
        for (k = 0; k < CHUNK; k++) {
            buf[k] = pattern(off + k);
        }
        if (syscall3(SYS_WRITE, fd, (long)buf, CHUNK) != CHUNK) {
            return -1;
        }
    }
    return 0;
}

/** Read `bytes` of `fd` from offset 0 and require the pattern, but zeros in
 * [`zero`, `zero` + `zlen`) and anything in [`any`, `any` + `alen`): the
 * first offset that differs, or -1 when none does. */
static long check_pattern(int fd, unsigned long bytes, unsigned long zero, unsigned long zlen,
                          unsigned long any, unsigned long alen)
{
    static char buf[CHUNK];
    unsigned long off, k, at;

    buf[0] = 0; /* the read fills it through a cast cppcheck cannot follow */
    if (syscall3(SYS_LSEEK, fd, 0, SEEK_SET) != 0) {
        return 0;
    }
    for (off = 0; off < bytes; off += CHUNK) {
        if (syscall3(SYS_READ, fd, (long)buf, CHUNK) != CHUNK) {
            return (long)off;
        }
        for (k = 0; k < CHUNK; k++) {
            at = off + k;
            if (at >= any && at < any + alen) {
                continue;
            }
            if (buf[k] != (at >= zero && at < zero + zlen ? 0 : pattern(at))) {
                return (long)at;
            }
        }
    }
    return -1;
}

/**
 * The recovery kernel, one empty disk of 4 MiB on virtio-blk: `write-read`,
 * 1 MiB of a pattern written raw, flushed by fsync and read back; `zeroout`,
 * BLKZEROOUT of 128 KiB at 64 KiB, after which those bytes read as zeros and
 * the rest as the pattern; `discard`, BLKDISCARD of 256 KiB at 512 KiB, after
 * which the bytes around it still read as the pattern; `discardzeroes`,
 * BLKDISCARDZEROES answering 0; `secure`, BLKSECDISCARD -EOPNOTSUPP; and
 * `range`, BLKDISCARD of a range not of whole sectors, empty or past the
 * disk's end -EINVAL.
 */
static void write_row(void)
{
    int fd, zeroes = 1;

    report_plan("write-read");
    report_plan("zeroout");
    report_plan("discard");
    report_plan("discardzeroes");
    report_plan("secure");
    report_plan("range");
    fd = syscall3(SYS_OPEN, (long)"/dev/disk0", O_RDWR, 0);
    if (fd < 0 || write_pattern(fd, READ_BYTES) != 0 || syscall1(SYS_FSYNC, fd) != 0) {
        fail("the pattern was not written to /dev/disk0 and flushed");
    }
    if (check_pattern(fd, READ_BYTES, 0, 0, 0, 0) != -1) {
        fail("the pattern did not read back");
    }
    report_held();
    if (range_ioctl(fd, BLKZEROOUT, 65536, 131072) != 0 ||
        check_pattern(fd, READ_BYTES, 65536, 131072, 0, 0) != -1) {
        fail("BLKZEROOUT failed, or its range did not read as zeros beside the pattern");
    }
    report_held();
    if (range_ioctl(fd, BLKDISCARD, 524288, 262144) != 0 ||
        check_pattern(fd, READ_BYTES, 65536, 131072, 524288, 262144) != -1) {
        fail("BLKDISCARD failed, or it changed bytes outside its range");
    }
    report_held();
    if (syscall3(SYS_IOCTL, fd, BLKDISCARDZEROES, (long)&zeroes) != 0 || zeroes != 0) {
        fail("BLKDISCARDZEROES did not answer 0");
    }
    report_held();
    if (range_ioctl(fd, BLKSECDISCARD, 0, 4096) != -EOPNOTSUPP) {
        fail("BLKSECDISCARD is not EOPNOTSUPP");
    }
    report_held();
    if (range_ioctl(fd, BLKDISCARD, 100, 4096) != -EINVAL ||
        range_ioctl(fd, BLKDISCARD, 0, 0) != -EINVAL ||
        range_ioctl(fd, BLKDISCARD, 4194304 - 4096, 8192) != -EINVAL) {
        fail("BLKDISCARD of a range not of whole sectors, empty or past the end is not EINVAL");
    }
    syscall2(SYS_CLOSE, fd, 0);
    report_held();
    report_end();
    puts("VBLKPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run `write`, or the two-disk read when no mode is supplied. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;

    if (argc >= 2 && argv[1][0] == 'w') {
        report_begin(report_case(envp, "guest.vblk.write"));
        write_row();
    }
    report_begin(report_case(envp, "guest.vblk.read"));
    read_row();
}
