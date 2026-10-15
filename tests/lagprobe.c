/**
 * lagprobe: the sleeping block path, proved from user space.
 *
 * `lagprobe sleep`, under `rach2.blocklag=N`: a child counts time in 10 ms
 * steps and feeds a pipe while the parent reads 64 KiB from `/dev/disk0p1`,
 * which the lag device answers N ticks late per request. The read succeeding
 * proves the path; the bytes in the pipe prove the parent slept while the
 * child ran; the lag device's count of sixteen late answers proves the lag
 * was taken on each page, and the elapsed time is printed.
 *
 * `lagprobe write`, under the lag: a sector written to /dev/bootpart and
 * read back, each through the lag.
 *
 * `lagprobe deadline`, under a lag above the deadline: the read fails in
 * deadline time with EIO, the kernel's record says the slot was quarantined
 * and freed by the late completion, and a second read fails the same way,
 * which proves the pool was not exhausted by the first.
 *
 * `lagprobe mounted`, under a lag with the FAT32 partition on
 * /dev/disk0p2: the mounted half. A child's rmdir of the target while the
 * mount sleeps in its constructor is EBUSY; two children reading one cold
 * block make the cache count a busy-buffer wait; one description shared
 * through fork gives each sector of a file to exactly one of the two readers;
 * and the whole-disk writer, on the real disk under the lag, is refused over
 * the mounted range.
 *
 * `lagprobe root`, under the lag with the root on the disk: /proc/mounts
 * names a disk partition as the root's source, so the boot did not fall
 * back to the RAM image.
 *
 * `lagprobe blockfail`, under `rach2.blockfail=3`: the first write to a
 * mounted volume fails, the mount goes read-only, /proc/mounts says so,
 * and the next write answers EROFS.
 *
 * `lagprobe latefail [TYPE]`, under a later `rach2.blockfail`: a file is
 * created, written and synced until a write fails. The mount goes read-only
 * and unmounts with its dirty flag still set. The type is vfat unless TYPE
 * names another.
 *
 * `lagprobe writers [TYPE]`: two children write files on the lagged volume,
 * FAT32 or the type TYPE names, while the parent syncs it; each file reads
 * back as its own bytes.
 *
 * `lagprobe pool`, under `rach2.blockpool=2` with the lag: three readers
 * and a writer on two slots, of which the disk may hold one, so whoever
 * finds the share taken waits for it; every read succeeds, the write is
 * synced while they run, which is a flush through the pool, and the mount
 * is still writable.
 *
 * `lagprobe dead`, under `rach2.blockdead=1 rach2.blockpool=2` with the lag:
 * the first partition is a device that never answers. A child's raw read of
 * it holds the one slot that device may have; the parent's mount of the
 * same partition waits for that slot, and when the read's deadline
 * quarantines it the device is stalled, the waiter is released with EIO, and
 * the read answers EIO at that same deadline. Then the FAT32 volume on the
 * second partition, the lag device on the same pool, is mounted, written and
 * synced.
 *
 * `lagprobe queue`, under `rach2.blockpool=2` with a lag of half a second:
 * sixteen readers each read a block of their own from the mounted volume,
 * so sixteen requests queue for the one the disk may have in flight, and
 * the last waits eight seconds, past the five-second deadline; every read
 * must succeed, since the deadline of a request starts when it is taken
 * and the time spent waiting for the limit does not count.
 *
 * `lagprobe dirty`, under a lag of one tick: a megabyte is written to a
 * file on the mounted KRFFS volume, whose writes stay dirty in the buffer
 * cache until a sync where the FAT32 volume's go through, then the
 * probe takes all but a twentieth of the machine's memory in pages,
 * which makes the caches give their frames back, and then makes one more
 * mapping, the system call in which dirty buffers are written back; the
 * file's bytes must read back whole after a sync, the mount must still
 * take a write, and the buffer cache must have shrunk.
 *
 * With no argument the mode is `sleep`. Prints "LAGPROBE OK" and exits 0, or
 * "LAGPROBE FAIL: " and the reason and exits 1.
 */

#include "probe.h"

#define EIO     5
#define EROFS   30
#define O_CREAT 0100

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

/* The type `writers` and `latefail` mount, set by the second argument. */
static const char *volume_type = "vfat";

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

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

/** Print "LAGPROBE FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (long)"LAGPROBE FAIL: ", 15);
    puts(why);
    sys_exit(1);
}

/** Write `n` in decimal, without a newline. */
static void putnum(unsigned n)
{
    char digits[12];
    int i = sizeof(digits);

    digits[--i] = 0;
    do {
        digits[--i] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n);
    syscall3(SYS_WRITE, 1, (long)&digits[i], (int)strlen(&digits[i]));
}

static int disk_counter(const char *label);
static void say_numbers(unsigned a, unsigned b, unsigned c, unsigned d);

struct timeval {
    long sec;
    long usec;
};

/** The time of day in milliseconds, modulo 2^32, from gettimeofday. */
static unsigned now_ms(void)
{
    struct timeval tv;
    syscall2(SYS_GETTIMEOFDAY, (long)&tv, 0);
    return (unsigned)tv.sec * 1000u + (unsigned)tv.usec / 1000u;
}

static unsigned char sector_buf[64 * 1024];

/** One read of `len` bytes from the start of /dev/disk0p1 into the sector
 * buffer; bytes or -errno. */
static int read_part(unsigned len)
{
    int fd = syscall3(SYS_OPEN, (long)"/dev/disk0p1", O_RDONLY, 0);
    int r;

    if (fd < 0) {
        return fd;
    }
    r = syscall3(SYS_READ, fd, (long)sector_buf, (int)len);
    syscall1(SYS_CLOSE, fd);
    return r;
}

/**
 * The sleep row: a reader of sixteen pages through the lag device, each
 * answered after its lag, with a sibling that must run while the reader
 * sleeps. The lag device counts the answers, so the count is the proof and
 * the time is only printed. First, the MBR behind this boot has two
 * entries, so /dev/disk0p2 opens and /dev/disk0p3, a node for an empty
 * entry, does not.
 */
static void run_sleep(void)
{
    int fds[2];
    int pid;
    int fd;
    unsigned start, elapsed;
    int lagged;
    int got;

    /* Two MBR entries: a node for the second and none for a third. */
    fd = syscall3(SYS_OPEN, (long)"/dev/disk0p2", O_RDONLY, 0);
    if (fd < 0) {
        fail("the second partition node");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall3(SYS_OPEN, (long)"/dev/disk0p3", O_RDONLY, 0) >= 0) {
        fail("a node for an empty MBR entry");
    }

    fds[0] = -1;
    fds[1] = -1;
    if (syscall1(SYS_PIPE, (long)fds) != 0) {
        fail("pipe");
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        /* The sibling: one pipe byte per 10 ms of its own running time. */
        unsigned last = now_ms();
        for (;;) {
            unsigned t = now_ms();
            if (t - last >= 10u) {
                last = t;
                syscall3(SYS_WRITE, fds[1], (long)"x", 1);
            }
        }
    }
    start = now_ms();
    lagged = disk_counter("lag-completions ");
    if (read_part(sizeof(sector_buf)) != (int)sizeof(sector_buf)) {
        fail("the partition read");
    }
    elapsed = now_ms() - start;
    got = syscall3(SYS_READ, fds[0], (long)sector_buf, 64);
    syscall2(SYS_KILL, pid, 9);
    puts("LAGPROBE sleep ms:");
    putnum(elapsed);
    puts("");
    if (disk_counter("lag-completions ") - lagged != 16) {
        fail("the read did not take the lag on each of its sixteen pages");
    }
    if (got < 5) {
        fail("the sibling did not run while the reader slept");
    }
    puts("LAGPROBE OK");
}

#define DEADLINE_TICKS 500

/**
 * The deadline row. The lag is 700 ticks, the deadline 500: each read must
 * fail with EIO because the deadline quarantined its request, which the
 * block layer counts, and not because the lag answered, which would have
 * returned the sector; and each request must have been in flight for the
 * deadline's ticks, which the layer counts too, since a deadline cut short
 * would fail the read sooner with the same EIO and the same records. At
 * least one quarantine and one release must be the read's own; the boot's
 * own lagged requests and their late answers may land inside the window
 * too, so an exact count would depend on the timing. The second read comes
 * after the late completion has freed the quarantined slot, 200 ticks after
 * the deadline.
 */
static void run_deadline(void)
{
    int quarantines = disk_counter("disk-quarantines ");
    int releases = disk_counter("disk-releases ");
    int waited = disk_counter("disk-wait-ticks ");
    unsigned start;

    if (read_part(512) != -EIO) {
        fail("the first read past the deadline was not EIO");
    }
    /* In flight for the whole deadline, which the block layer counts. */
    puts("LAGPROBE first read: ticks in flight:");
    say_numbers((unsigned)(disk_counter("disk-wait-ticks ") - waited), 0, 0, 0);
    if (disk_counter("disk-wait-ticks ") - waited < DEADLINE_TICKS) {
        fail("the first read was failed before the deadline's ticks");
    }
    /* At least one of each, the read's own. */
    puts("LAGPROBE first read: quarantined, released:");
    say_numbers((unsigned)(disk_counter("disk-quarantines ") - quarantines),
                (unsigned)(disk_counter("disk-releases ") - releases), 0, 0);
    if (disk_counter("disk-quarantines ") - quarantines < 1 ||
        disk_counter("disk-releases ") - releases < 1) {
        fail("the first read was not quarantined and released");
    }
    /* The late completion frees the quarantined slot within the wait. */
    start = now_ms();
    while (now_ms() - start < 3000u) {
    }
    quarantines = disk_counter("disk-quarantines ");
    releases = disk_counter("disk-releases ");
    waited = disk_counter("disk-wait-ticks ");
    if (read_part(512) != -EIO) {
        fail("the second read past the deadline was not EIO");
    }
    if (disk_counter("disk-wait-ticks ") - waited < DEADLINE_TICKS) {
        fail("the second read was failed before the deadline's ticks");
    }
    puts("LAGPROBE second read: quarantined, released:");
    say_numbers((unsigned)(disk_counter("disk-quarantines ") - quarantines),
                (unsigned)(disk_counter("disk-releases ") - releases), 0, 0);
    if (disk_counter("disk-quarantines ") - quarantines < 1 ||
        disk_counter("disk-releases ") - releases < 1) {
        fail("the second read was not quarantined and released");
    }
    puts("LAGPROBE OK");
}

/** A sector written through the lag and read back: the kernel copies a write
 * into the request page before the device sleeps on it, and this is that
 * copy observed from user space. The offset is 256 KiB into the partition:
 * inside even the small fixture partition, and clear of what it reads. */
static void run_write(void)
{
    int fd = syscall3(SYS_OPEN, (long)"/dev/bootpart", O_RDWR, 0);
    unsigned char *pattern = sector_buf;
    unsigned char *back = sector_buf + 512;
    int lagged;
    int i;

    if (fd < 0) {
        fail("open /dev/bootpart");
    }
    if (syscall3(SYS_LSEEK, fd, 262144, 0) != 262144) {
        fail("seek to the test sector");
    }
    for (i = 0; i < 512; i++) {
        pattern[i] = (unsigned char)(0xa5 ^ i);
    }
    lagged = disk_counter("lag-completions ");
    if (syscall3(SYS_WRITE, fd, (long)pattern, 512) != 512) {
        fail("the write through the lag");
    }
    if (disk_counter("lag-completions ") - lagged < 1) {
        fail("the write did not take the lag");
    }
    if (syscall3(SYS_LSEEK, fd, 262144, 0) != 262144) {
        fail("seek back");
    }
    lagged = disk_counter("lag-completions ");
    if (syscall3(SYS_READ, fd, (long)back, 512) != 512) {
        fail("the read back");
    }
    if (disk_counter("lag-completions ") - lagged < 1) {
        fail("the read back did not take the lag");
    }
    for (i = 0; i < 512; i++) {
        if (back[i] != pattern[i]) {
            fail("the bytes read back");
        }
    }
    syscall1(SYS_CLOSE, fd);
    puts("LAGPROBE OK");
}

#define MS_RDONLY 1
#define EBUSY     16

/** Sleep for `ms` milliseconds. */
static void sleep_ms(int ms)
{
    long ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    syscall2(SYS_NANOSLEEP, (long)ts, 0);
}

/** The raw wait status of the child `pid`; fail when the wait fails. */
static int wait_status(int pid)
{
    int status = 0;

    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("waitpid");
    }
    return status;
}

/** The number after `label` in /proc/disk, or -1. */
static int disk_counter(const char *label)
{
    static char text[2048];
    int fd = syscall3(SYS_OPEN, (long)"/proc/disk", O_RDONLY, 0);
    int n, i, j;

    if (fd < 0) {
        return -1;
    }
    n = syscall3(SYS_READ, fd, (long)text, sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return -1;
    }
    text[n] = 0;
    for (i = 0; text[i]; i++) {
        for (j = 0; label[j] && text[i + j] == label[j]; j++) {
        }
        if (!label[j]) {
            int v = 0;

            i += j;
            while (text[i] == ' ') {
                i++;
            }
            while (text[i] >= '0' && text[i] <= '9') {
                v = v * 10 + (text[i] - '0');
                i++;
            }
            return v;
        }
    }
    return -1;
}

/** `lagprobe root`, under `rach2.fsdisk rach2.blocklag=N`: the root is the
 * disk's KRFFS partition, read through the real device, which /proc/mounts
 * says by naming the partition as the root's source. */
static void run_root(void)
{
    static char text[512];
    int fd = syscall3(SYS_OPEN, (long)"/proc/mounts", O_RDONLY, 0);
    int n;

    if (fd < 0) {
        fail("open /proc/mounts");
    }
    n = syscall3(SYS_READ, fd, (long)text, (int)sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        fail("read /proc/mounts");
    }
    text[n] = 0;
    if (text[0] != '/' || text[1] != 'd' || text[2] != 'e' || text[3] != 'v' || text[4] != '/' ||
        text[5] != 'd' || text[6] != 'i' || text[7] != 's' || text[8] != 'k') {
        fail("the root's source is not a disk partition");
    }
    puts("LAGPROBE OK");
}

/** Whether /proc/mounts says the mount on /lb is `ro`; fail when the file
 * cannot be read. */
static int lb_read_only(void)
{
    static char text[1024];
    int fd, n, i, ro = 0;

    fd = syscall3(SYS_OPEN, (long)"/proc/mounts", O_RDONLY, 0);
    n = fd < 0 ? -1 : syscall3(SYS_READ, fd, (long)text, (int)sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        fail("read /proc/mounts");
        return 0;
    }
    text[n] = 0;
    for (i = 0; i + 8 < n; i++) {
        if (text[i] == '/' && text[i + 1] == 'l' && text[i + 2] == 'b' && text[i + 3] == ' ') {
            int j = i + 4;

            while (j + 3 < n && text[j] != '\n') {
                if (text[j] == ' ' && text[j + 1] == 'r' && text[j + 2] == 'o' &&
                    (text[j + 3] == ',' || text[j + 3] == ' ')) {
                    ro = 1;
                }
                j++;
            }
        }
    }
    return ro;
}

/** `lagprobe blockfail`, under `rach2.blocklag=3 rach2.blockfail=3`: the
 * third write through the lag fails, which is inside the first write to
 * the mounted volume; the mount goes read-only with a record, /proc/mounts
 * says `ro`, and the next write answers EROFS. */
static void run_blockfail(void)
{
    int fd;

    if (syscall2(SYS_MKDIR, (long)"/lb", 0755) != 0) {
        fail("mkdir /lb");
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p2", (long)"/lb", (long)"vfat", 0, 0) != 0) {
        fail("the writable mount through the lag");
    }
    fd = syscall3(SYS_OPEN, (long)"/lb/FAIL.TXT", O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
        fail("the first write to the volume did not fail");
    }
    if (!lb_read_only()) {
        fail("the mount whose device failed a write is not read-only in /proc/mounts");
    }
    if (syscall3(SYS_OPEN, (long)"/lb/AGAIN.TXT", O_WRONLY | O_CREAT, 0644) != -EROFS) {
        fail("a write after the failure is not EROFS");
    }
    if (syscall2(SYS_UMOUNT2, (long)"/lb", 0) != 0) {
        fail("umount2 of the read-only mount");
    }
    puts("LAGPROBE OK");
}

/** `lagprobe latefail [TYPE]`, under the lag with a later write failing: the
 * create succeeds, so the dirty flag was written; then 4 KiB at a time is
 * written and synced until the device fails a write. The mount goes
 * read-only, and the unmount succeeds; the suite then reads the dirty flag
 * from the disk, which the driver does not clear on a read-only mount. */
static void run_latefail(void)
{
    static char block[4096];
    int fd, i, rc = 0;

    if (syscall2(SYS_MKDIR, (long)"/lb", 0755) != 0) {
        fail("mkdir /lb");
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p2", (long)"/lb", (long)volume_type, 0, 0) != 0) {
        fail("the writable mount through the lag");
    }
    fd = syscall3(SYS_OPEN, (long)"/lb/LATE.TXT", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fail("the create before the failure");
    }
    for (i = 0; i < (int)sizeof(block); i++) {
        block[i] = (char)('a' + i % 26);
    }
    for (i = 0; i < 64 && rc == 0; i++) {
        rc = syscall3(SYS_WRITE, fd, (long)block, (int)sizeof(block));
        rc = rc == (int)sizeof(block) ? syscall1(SYS_FSYNC, fd) : (rc < 0 ? rc : -EIO);
    }
    syscall1(SYS_CLOSE, fd);
    if (rc == 0) {
        fail("no write to the volume failed");
    }
    if (!lb_read_only()) {
        fail("the mount whose device failed a write is not read-only in /proc/mounts");
    }
    rc = syscall2(SYS_UMOUNT2, (long)"/lb", 0);
    if (rc != 0) {
        syscall3(SYS_WRITE, 1, (long)"lagprobe: umount2: errno ", 25);
        putnum((unsigned)-rc);
        puts("");
        fail("umount2 of the mount that failed after its dirty flag was set");
    }
    puts("LAGPROBE OK");
}

#define WRITER_CHUNK  4096
#define WRITER_ROUNDS 8

/** A writer of `lagprobe writers`: create `path` and write WRITER_ROUNDS
 * chunks of bytes counting up from `fill`; exit 0, 2 when the create fails
 * and 3 when a write is short. */
static void writer(const char *path, unsigned char fill)
{
    static unsigned char chunk[WRITER_CHUNK];
    int fd, i, r;

    for (i = 0; i < WRITER_CHUNK; i++) {
        chunk[i] = (unsigned char)(fill + i);
    }
    fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        sys_exit(2);
    }
    for (r = 0; r < WRITER_ROUNDS; r++) {
        if (syscall3(SYS_WRITE, fd, (long)chunk, WRITER_CHUNK) != WRITER_CHUNK) {
            sys_exit(3);
        }
    }
    syscall1(SYS_CLOSE, fd);
    sys_exit(0);
}

/** Fail unless `path` has exactly the bytes `writer` wrote with `fill`. */
static void check_written(const char *path, unsigned char fill)
{
    static unsigned char storage[WRITER_CHUNK];
    unsigned char *buf = storage; /* read into through the pointer */
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int r, i;

    if (fd < 0) {
        fail("a writer's file is missing");
    }
    for (r = 0; r < WRITER_ROUNDS; r++) {
        if (syscall3(SYS_READ, fd, (long)buf, WRITER_CHUNK) != WRITER_CHUNK) {
            fail("a writer's file is short");
        }
        for (i = 0; i < WRITER_CHUNK; i++) {
            if (buf[i] != (unsigned char)(fill + i)) {
                fail("a writer's file holds another writer's bytes");
            }
        }
    }
    if (syscall3(SYS_READ, fd, (long)buf, 16) != 0) {
        fail("a writer's file is long");
    }
    syscall1(SYS_CLOSE, fd);
}

/**
 * `lagprobe writers`, under the lag with the second partition mounted for
 * writing: two children each create a file and extend it eight times, and
 * the parent syncs the volume twenty times while they do; every write
 * sleeps in the lag, so the requests interleave. Each file must read back
 * as its own bytes, whole, every sync must return 0, and the volume must
 * unmount clean. The sync's turn under the lock cannot be seen from here:
 * the driver's assertion at every mutation is what catches a write outside
 * it.
 */
static void run_writers(void)
{
    int a, b, s;

    if (syscall2(SYS_MKDIR, (long)"/lw", 0755) != 0) {
        fail("mkdir /lw");
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p2", (long)"/lw", (long)volume_type, 0, 0) != 0) {
        fail("the writable mount through the lag");
    }
    a = syscall1(SYS_FORK, 0);
    if (a == 0) {
        writer("/lw/writer-a.bin", 0x11);
    }
    b = syscall1(SYS_FORK, 0);
    if (b == 0) {
        writer("/lw/writer-b.bin", 0x77);
    }
    for (s = 0; s < 20; s++) {
        if (syscall1(SYS_SYNC, 0) != 0) {
            fail("sync during the writers");
        }
        sleep_ms(30);
    }
    if (wait_status(a) != 0 || wait_status(b) != 0) {
        fail("a writer failed");
    }
    check_written("/lw/writer-a.bin", 0x11);
    check_written("/lw/writer-b.bin", 0x77);
    if (syscall2(SYS_UMOUNT2, (long)"/lw", 0) != 0) {
        fail("umount2 of the written volume");
    }
    puts("LAGPROBE OK");
}

#define PATTERN_SECTORS 128

static unsigned char parent_marks[PATTERN_SECTORS];
static unsigned char child_marks[PATTERN_SECTORS];
static int pipe_fds[2];

/** Read `fd` one sector at a time to the end and mark each sector received
 * in `marks`; a sector whose 128 fields do not all say the same number, or
 * a number out of range, or a short read, fails the probe. The reader that
 * finishes a sector keeps the processor and would take the description
 * again before the other wakes, so a tick's sleep after each lets the two
 * alternate, which is what the marks then show. */
static void mark_sectors(int fd, unsigned char *marks)
{
    int n, i, v;

    while ((n = syscall3(SYS_READ, fd, (long)sector_buf, 512)) > 0) {
        if (n != 512) {
            fail("a short read of pattern.bin");
        }
        v = (sector_buf[0] - '0') * 100 + (sector_buf[1] - '0') * 10 + (sector_buf[2] - '0');
        if (v < 0 || v >= PATTERN_SECTORS) {
            fail("a sector of pattern.bin names no sector");
        }
        for (i = 0; i < 512; i += 4) {
            if (sector_buf[i] != sector_buf[0] || sector_buf[i + 1] != sector_buf[1] ||
                sector_buf[i + 2] != sector_buf[2] || sector_buf[i + 3] != '.') {
                fail("a sector of pattern.bin is torn");
            }
        }
        marks[v]++;
        sleep_ms(10); /* so that the two readers alternate */
    }
    if (n < 0) {
        fail("a read of pattern.bin failed");
    }
}

/**
 * The mounted half, under `rach2.blocklag=N` with the FAT32 partition on
 * /dev/disk0p2. A child runs rmdir on the target while the parent's mount
 * sleeps in the constructor on the lag, and gets EBUSY, since the slot has
 * the covered node. Two children read the same cold block at once, and the
 * cache counts one waiting for the busy buffer. Two processes sharing one
 * description through fork read pattern.bin to its end a sector at a time,
 * each marking the sectors it received by the number every sector carries
 * in each of its 128 fields; the child's marks come back through a pipe,
 * and every sector must be marked by exactly one of the two. Last, the
 * whole-disk window is on the real disk and the mount is on the lag wrapper
 * over it, so the claim on the disk must compare the disk under both, and
 * a writer of /dev/disk0 is EBUSY.
 */
static void run_mounted(void)
{
    int pid, pid2, fd, before, after;
    int total;
    int hello_bytes;

    if (syscall2(SYS_MKDIR, (long)"/lm", 0755) != 0) {
        fail("mkdir /lm");
    }

    /* rmdir of the target while the constructor sleeps is EBUSY. */
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        sleep_ms(30);
        if (syscall1(SYS_RMDIR, (long)"/lm") != -EBUSY) {
            sys_exit(3);
        }
        sys_exit(0);
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p2", (long)"/lm", (long)"vfat", MS_RDONLY, 0) != 0) {
        fail("the mount through the lag");
    }
    if (wait_status(pid) != 0) {
        fail("rmdir of the target while the constructor slept was not EBUSY");
    }

    /* Two readers of one cold block: the cache counts the second's wait. */
    before = disk_counter("waits");
    pid = syscall1(SYS_FORK, 0);
    pid2 = pid == 0 ? 0 : syscall1(SYS_FORK, 0);
    if (pid == 0 || pid2 == 0) {
        fd = syscall3(SYS_OPEN, (long)"/lm/hello.txt", O_RDONLY, 0);
        if (fd < 0 || syscall3(SYS_READ, fd, (long)sector_buf, 512) <= 0) {
            sys_exit(4);
        }
        sys_exit(0);
    }
    if (wait_status(pid) != 0 || wait_status(pid2) != 0) {
        fail("a cold-block reader");
    }
    after = disk_counter("waits");
    if (before < 0 || after <= before) {
        fail("no busy-buffer wait was counted");
    }

    /* One description shared through fork gives each sector to one side. */
    fd = syscall3(SYS_OPEN, (long)"/lm/pattern.bin", O_RDONLY, 0);
    if (fd < 0) {
        fail("open the shared file");
    }
    hello_bytes = syscall3(SYS_LSEEK, fd, 0, 2);
    if (hello_bytes != PATTERN_SECTORS * 512 || syscall3(SYS_LSEEK, fd, 0, 0) != 0) {
        fail("the shared file's size");
    }
    if (syscall1(SYS_PIPE, (long)pipe_fds) != 0) {
        fail("pipe");
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        mark_sectors(fd, child_marks);
        if (syscall3(SYS_WRITE, pipe_fds[1], (long)child_marks, PATTERN_SECTORS) !=
            PATTERN_SECTORS) {
            sys_exit(5);
        }
        sys_exit(0);
    }
    mark_sectors(fd, parent_marks);
    if (wait_status(pid) != 0) {
        fail("the child reader");
    }
    if (syscall3(SYS_READ, pipe_fds[0], (long)child_marks, PATTERN_SECTORS) != PATTERN_SECTORS) {
        fail("the child's marks");
    }
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_CLOSE, pipe_fds[0]);
    syscall1(SYS_CLOSE, pipe_fds[1]);
    total = 0;
    for (fd = 0; fd < PATTERN_SECTORS; fd++) {
        if (parent_marks[fd] + child_marks[fd] != 1) {
            fail("a sector was read twice or not at all through the shared description");
        }
        total += parent_marks[fd];
    }
    if (total == 0 || total == PATTERN_SECTORS) {
        fail(total == 0 ? "the parent read nothing through the shared description"
                        : "the child read nothing through the shared description");
    }

    /* A whole-disk writer over the mounted range is EBUSY. */
    if (syscall3(SYS_OPEN, (long)"/dev/disk0", O_RDWR, 0) != -EBUSY) {
        fail("a whole-disk writer opened over a mounted range");
    }

    if (syscall2(SYS_UMOUNT2, (long)"/lm", 0) != 0) {
        fail("umount2 of the lag mount");
    }
    syscall1(SYS_RMDIR, (long)"/lm");
    puts("LAGPROBE OK");
}

/** Whether /proc/mounts lists `dir` with `ro` among its options; fail when
 * the file cannot be read. */
static int mount_is_ro(const char *dir)
{
    static char text[1024];
    int fd = syscall3(SYS_OPEN, (long)"/proc/mounts", O_RDONLY, 0);
    int n = fd < 0 ? -1 : syscall3(SYS_READ, fd, (long)text, (int)sizeof(text) - 1);
    int i, j, ro = 0;
    size_t dlen = strlen(dir);

    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        fail("read /proc/mounts");
        return 0;
    }
    text[n] = 0;
    for (i = 0; i + (int)dlen < n; i++) {
        if (text[i] != ' ' || text[i + 1 + (int)dlen] != ' ') {
            continue;
        }
        for (j = 0; j < (int)dlen && text[i + 1 + j] == dir[j]; j++) {
        }
        if (j != (int)dlen) {
            continue;
        }
        for (j = i + 1 + (int)dlen; j + 3 < n && text[j] != '\n'; j++) {
            if (text[j] == ' ' && text[j + 1] == 'r' && text[j + 2] == 'o' &&
                (text[j + 3] == ',' || text[j + 3] == ' ')) {
                ro = 1;
            }
        }
    }
    return ro;
}

/** On the volume mounted at `dir`: the file `first` written and synced, the
 * file `second` created after the sync, and the mount still writable; fail
 * otherwise. */
static void write_and_sync(const char *dir, const char *first, const char *second)
{
    int fd;

    fd = syscall3(SYS_OPEN, (long)first, O_WRONLY | O_CREAT, 0644);
    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)"pool", 4) != 4) {
        fail("the first write to the volume");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall1(SYS_SYNC, 0) != 0) {
        fail("sync of the volume");
    }
    fd = syscall3(SYS_OPEN, (long)second, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fail("a write after the sync");
    }
    syscall1(SYS_CLOSE, fd);
    if (mount_is_ro(dir)) {
        fail("the mount went read-only");
    }
}

/** Read the whole of `path` into the sector buffer, in 4 KiB pieces; bytes
 * or -errno. */
static int read_file(const char *path)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int total = 0, r;

    if (fd < 0) {
        return fd;
    }
    while ((r = syscall3(SYS_READ, fd, (long)sector_buf, 4096)) > 0) {
        total += r;
    }
    syscall1(SYS_CLOSE, fd);
    return r < 0 ? r : total;
}

/** End a child with 0 when `got` is `want`, and 1 otherwise. */
static int child_result(int got, int want)
{
    sys_exit(got == want ? 0 : 1);
    return 0;
}

/** Three readers and a writer on a pool of two slots, the device's share
 * one: the raw reads of the first partition go one at a time through the
 * raw window, the two readers of files on the mounted volume and the
 * parent's write and sync join them, and whoever finds the share taken
 * waits. The sync runs while the readers run, so its flush is submitted
 * against the contention and not after it. */
static void run_pool(void)
{
    int a, b, c;

    if (syscall2(SYS_MKDIR, (long)"/lp", 0755) != 0) {
        fail("mkdir /lp");
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p2", (long)"/lp", (long)"vfat", 0, 0) != 0) {
        fail("the writable mount of the second partition");
    }
    a = syscall1(SYS_FORK, 0);
    if (a == 0) {
        child_result(read_part(sizeof(sector_buf)), (int)sizeof(sector_buf));
    }
    b = syscall1(SYS_FORK, 0);
    if (b == 0) {
        child_result(read_file("/lp/pattern.bin"), PATTERN_SECTORS * 512);
    }
    c = syscall1(SYS_FORK, 0);
    if (c == 0) {
        child_result(read_file("/lp/again.bin") > 0 && read_file("/lp/hello") > 0, 1);
    }
    if (a < 0 || b < 0 || c < 0) {
        fail("fork of a reader");
    }
    write_and_sync("/lp", "/lp/POOL.TXT", "/lp/AGAIN.TXT");
    if (wait_status(a) != 0 || wait_status(b) != 0 || wait_status(c) != 0) {
        fail("a reader under the small pool failed");
    }
    if (syscall2(SYS_UMOUNT2, (long)"/lp", 0) != 0) {
        fail("umount2 of the volume");
    }
    puts("LAGPROBE OK");
}

/** One 4 KiB block of `path` at `offset`, into the sector buffer; bytes,
 * -errno, or -1 when the seek fails. */
static int read_block_at(const char *path, int offset)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int r;

    if (fd < 0) {
        return fd;
    }
    if (syscall3(SYS_LSEEK, fd, offset, 0) != offset) {
        syscall1(SYS_CLOSE, fd);
        return -1;
    }
    r = syscall3(SYS_READ, fd, (long)sector_buf, 4096);
    syscall1(SYS_CLOSE, fd);
    return r;
}

/** Sixteen readers of sixteen distinct blocks, queued for the one request
 * the disk may hold; each must get its block, however long it waited. */
static void run_queue(void)
{
    int pids[16];
    int i;

    if (syscall2(SYS_MKDIR, (long)"/lq", 0755) != 0) {
        fail("mkdir /lq");
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p2", (long)"/lq", (long)"vfat", 0, 0) != 0) {
        fail("the mount of the second partition");
    }
    for (i = 0; i < 16; i++) {
        pids[i] = syscall1(SYS_FORK, 0);
        if (pids[i] < 0) {
            fail("fork of a reader");
        }
        if (pids[i] == 0) {
            child_result(read_block_at("/lq/pattern.bin", i * 4096), 4096);
        }
    }
    for (i = 0; i < 16; i++) {
        if (wait_status(pids[i]) != 0) {
            fail("a queued reader failed");
        }
    }
    if (syscall2(SYS_UMOUNT2, (long)"/lq", 0) != 0) {
        fail("umount2 of the volume");
    }
    puts("LAGPROBE OK");
}

/** A field of /proc/meminfo, in kilobytes, or 0xFFFFFFFF when absent. */
static unsigned meminfo_kb(const char *key)
{
    static char text[2048];
    int fd = syscall3(SYS_OPEN, (long)"/proc/meminfo", O_RDONLY, 0);
    int n, i;

    if (fd < 0) {
        return 0xFFFFFFFFu;
    }
    n = syscall3(SYS_READ, fd, (long)text, (int)sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return 0xFFFFFFFFu;
    }
    text[n] = 0;
    for (i = 0; i < n; i++) {
        int k;

        for (k = 0; key[k] && text[i + k] == key[k]; k++) {
        }
        if (!key[k]) {
            unsigned v = 0;

            i += k;
            while (text[i] == ' ') {
                i++;
            }
            while (text[i] >= '0' && text[i] <= '9') {
                v = v * 10 + (unsigned)(text[i] - '0');
                i++;
            }
            return v;
        }
    }
    return 0xFFFFFFFFu;
}

#define ENOMEM 12

/** Four numbers on one line, for a row that fails to say what it saw. */
static void say_numbers(unsigned a, unsigned b, unsigned c, unsigned d)
{
    unsigned v[4];
    char line[64];
    int n = 0, k;

    v[0] = a;
    v[1] = b;
    v[2] = c;
    v[3] = d;
    for (k = 0; k < 4; k++) {
        char tmp[12];
        int t = 0;
        unsigned x = v[k];

        do {
            tmp[t++] = (char)('0' + x % 10);
            x /= 10;
        } while (x);
        while (t) {
            line[n++] = tmp[--t];
        }
        line[n++] = ' ';
    }
    line[n++] = '\n';
    syscall3(SYS_WRITE, 1, (long)line, n);
}

#define DIRTY_BYTES (1024 * 1024)

/** A megabyte left dirty in the buffer cache of the KRFFS volume, whose
 * writes stay dirty until a sync where the FAT32 volume's go through, then
 * the machine's memory taken a page at a time until the caches must give
 * their frames back, which a page fault does with clean frames only, and
 * one more mapping, the system call in which dirty buffers are written
 * back; the file whole and the mount writable after. The commands, the
 * flushes, the ticks requests spent in flight and the lag's answers are
 * printed at the end. */
static void run_dirty(void)
{
    unsigned total, buffers_before, buffers_after;
    unsigned long take, off;
    unsigned char *mem;
    int fd, i;

    if (syscall2(SYS_MKDIR, (long)"/ly", 0755) != 0) {
        fail("mkdir /ly");
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/ly", (long)"krffs", 0, 0) != 0) {
        fail("the writable mount of the first partition");
    }
    fd = syscall3(SYS_OPEN, (long)"/ly/dirty.bin", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        fail("creating the dirty file");
    }
    for (off = 0; off < DIRTY_BYTES; off += 4096) {
        for (i = 0; i < 4096; i++) {
            sector_buf[i] = (unsigned char)((off / 4096 + i) & 0xff);
        }
        if (syscall3(SYS_WRITE, fd, (long)sector_buf, 4096) != 4096) {
            fail("a write of the dirty file");
        }
    }
    syscall1(SYS_CLOSE, fd);
    total = meminfo_kb("MemTotal:");
    buffers_before = meminfo_kb("Buffers:");
    if (total == 0xFFFFFFFFu || buffers_before < 512) {
        fail("the buffer cache does not hold the file");
    }
    /* below the sixteenth that starts reclaim */
    take = ((unsigned long)total - total / 20) * 1024;
    mem = (unsigned char *)probe_mmap(0, (long)take, 3, 0x22, -1, 0);
    if ((long)mem < 0 && (long)mem > -4096) {
        fail("the anonymous mapping");
    }
    for (off = 0; off < take; off += 4096) {
        mem[off] = 1;
    }
    if (probe_mmap(0, 4096, 3, 0x22, -1, 0) == -ENOMEM) {
        fail("the mapping after the pressure");
    }
    buffers_after = meminfo_kb("Buffers:");
    if (buffers_after >= buffers_before) {
        say_numbers(total, buffers_before, buffers_after, meminfo_kb("MemFree:"));
        fail("the buffer cache did not give frames back under pressure");
    }
    if (syscall1(SYS_SYNC, 0) != 0) {
        fail("sync after the pressure");
    }
    fd = syscall3(SYS_OPEN, (long)"/ly/dirty.bin", O_RDONLY, 0);
    if (fd < 0) {
        say_numbers((unsigned)-fd, meminfo_kb("MemFree:"), 0, 0);
        fail("reopening the dirty file");
    }
    for (off = 0; off < DIRTY_BYTES; off += 4096) {
        if (syscall3(SYS_READ, fd, (long)sector_buf, 4096) != 4096) {
            fail("reading the file back");
        }
        for (i = 0; i < 4096; i++) {
            if (sector_buf[i] != (unsigned char)((off / 4096 + i) & 0xff)) {
                fail("the file's bytes after the pressure");
            }
        }
    }
    syscall1(SYS_CLOSE, fd);
    write_and_sync("/ly", "/ly/after.txt", "/ly/again.txt");
    if (syscall2(SYS_UMOUNT2, (long)"/ly", 0) != 0) {
        fail("umount2 of the volume");
    }
    puts("LAGPROBE dirty: commands, flushes, wait ticks, lag completions:");
    say_numbers((unsigned)disk_counter("disk-commands "), (unsigned)disk_counter("disk-flushes "),
                (unsigned)disk_counter("disk-wait-ticks "),
                (unsigned)disk_counter("lag-completions "));
    puts("LAGPROBE OK");
}

/** The raw reader's request takes the one slot the first partition's device
 * may have and is never answered; the mount of the same partition then
 * waits for that slot, and both must end at the reader's deadline, the one
 * quarantine releasing the reader and the waiter behind it. The second
 * partition is another device on the same pool and still writes. */
static void run_dead(void)
{
    int quarantines = disk_counter("disk-quarantines ");
    int releases = disk_counter("disk-releases ");
    int a, rc;

    a = syscall1(SYS_FORK, 0);
    if (a == 0) {
        child_result(read_part(512), -EIO);
    }
    if (a < 0) {
        fail("fork of the reader");
    }
    sleep_ms(200);
    if (syscall2(SYS_MKDIR, (long)"/ld1", 0755) != 0) {
        fail("mkdir /ld1");
    }
    rc = syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/ld1", (long)"krffs", 0, 0);
    if (rc != -EIO) {
        fail("the mount of the partition that never answers was not EIO");
    }
    if (wait_status(a) != 0) {
        fail("the raw reader of the partition that never answers was not EIO");
    }
    /* One quarantine, and two released by it. */
    if (disk_counter("disk-quarantines ") - quarantines != 1 ||
        disk_counter("disk-releases ") - releases != 2) {
        fail("the reader and the waiter were not the two released by the one deadline");
    }
    if (syscall2(SYS_MKDIR, (long)"/ld", 0755) != 0) {
        fail("mkdir /ld");
    }
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p2", (long)"/ld", (long)"vfat", 0, 0) != 0) {
        fail("the writable mount of the second partition");
    }
    write_and_sync("/ld", "/ld/DEAD.TXT", "/ld/AGAIN.TXT");
    if (syscall2(SYS_UMOUNT2, (long)"/ld", 0) != 0) {
        fail("umount2 of the volume");
    }
    puts("LAGPROBE OK");
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Whether the strings `a` and `b` are equal. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static const struct {
    const char *name;
    void (*run)(void);
} modes[] = {
    {"sleep", run_sleep}, {"deadline", run_deadline},   {"mounted", run_mounted},
    {"root", run_root},   {"blockfail", run_blockfail}, {"latefail", run_latefail},
    {"write", run_write}, {"writers", run_writers},     {"pool", run_pool},
    {"dead", run_dead},   {"queue", run_queue},         {"dirty", run_dirty},
};

/** The probe, entered with `sp` at argc: the mode the first argument names,
 * `sleep` with none; a second argument is the type `writers` or `latefail`
 * mounts. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    unsigned i;

    if (argc >= 3) {
        volume_type = argv[2];
    }
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (argc < 2 ? i == 0 : same(argv[1], modes[i].name)) {
            modes[i].run();
            sys_exit(0);
        }
    }
    fail("usage: lagprobe [sleep|deadline|mounted|root|blockfail|latefail [TYPE]|"
         "write|writers [TYPE]|pool|dead|queue|dirty]");
}
