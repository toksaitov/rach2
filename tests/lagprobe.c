/**
 * The sleeping block path, proved from user space.
 *
 * `lagprobe sleep`, under `rach2.blocklag=N`: a child counts time in 10 ms
 * steps and feeds a pipe while the parent reads 64 KiB from `/dev/disk0p1`,
 * which the lag device answers N ticks late per request. The read succeeding
 * proves the path; the pipe holding counts proves the parent slept while the
 * child ran; the elapsed time proves the lag was real.
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
 * `lagprobe writers`: two children write files on the lagged volume while
 * the parent syncs it; each file reads back as its own bytes.
 *
 * Prints "LAGPROBE OK" or "LAGPROBE FAIL: reason". Freestanding; int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT         1
#define SYS_FORK         2
#define SYS_READ         3
#define SYS_WRITE        4
#define SYS_OPEN         5
#define SYS_CLOSE        6
#define SYS_LSEEK        19
#define SYS_KILL         37
#define SYS_PIPE         42
#define SYS_SYNC         36
#define EIO              5
#define EROFS            30
#define O_CREAT          0100
#define SYS_GETTIMEOFDAY 78

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

static int syscall1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int syscall2(int n, int a, int b)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

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

static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (int)(size_t)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (int)(size_t)"\n", 1);
}

static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (int)(size_t)"LAGPROBE FAIL: ", 15);
    puts(why);
    sys_exit(1);
}

struct timeval {
    int sec;
    int usec;
};

/* Milliseconds since boot, from gettimeofday. */
static unsigned now_ms(void)
{
    struct timeval tv;
    syscall2(SYS_GETTIMEOFDAY, (int)(size_t)&tv, 0);
    return (unsigned)tv.sec * 1000u + (unsigned)tv.usec / 1000u;
}

static unsigned char sector_buf[64 * 1024];

/* One read of `len` bytes from the start of /dev/disk0p1; bytes or -errno. */
static int read_part(unsigned len)
{
    int fd = syscall3(SYS_OPEN, (int)(size_t)"/dev/disk0p1", O_RDONLY, 0);
    int r;

    if (fd < 0) {
        return fd;
    }
    r = syscall3(SYS_READ, fd, (int)(size_t)sector_buf, (int)len);
    syscall1(SYS_CLOSE, fd);
    return r;
}

static void run_sleep(void)
{
    int fds[2];
    int pid;
    int fd;
    unsigned start, elapsed;
    int got;

    /* The MBR behind this boot has two entries; a third node would be a
       node for an empty entry. */
    fd = syscall3(SYS_OPEN, (int)(size_t)"/dev/disk0p2", O_RDONLY, 0);
    if (fd < 0) {
        fail("the second partition node");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall3(SYS_OPEN, (int)(size_t)"/dev/disk0p3", O_RDONLY, 0) >= 0) {
        fail("a node for an empty MBR entry");
    }

    fds[0] = -1;
    fds[1] = -1;
    if (syscall1(SYS_PIPE, (int)(size_t)fds) != 0) {
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
                syscall3(SYS_WRITE, fds[1], (int)(size_t)"x", 1);
            }
        }
    }
    start = now_ms();
    if (read_part(sizeof(sector_buf)) != (int)sizeof(sector_buf)) {
        fail("the partition read");
    }
    elapsed = now_ms() - start;
    got = syscall3(SYS_READ, fds[0], (int)(size_t)sector_buf, 64);
    syscall2(SYS_KILL, pid, 9);
    /* 16 page requests, 10 ticks each: 1.6 s in the lag alone. */
    if (elapsed < 1200u) {
        fail("the read did not take the lag");
    }
    if (got < 5) {
        fail("the sibling did not run while the reader slept");
    }
    puts("LAGPROBE OK");
}

/* The lag is 700 ticks, the deadline 500: each read must fail with EIO
 * after the deadline and before the lag would have answered. */
static void run_deadline(void)
{
    unsigned start = now_ms();
    unsigned elapsed;

    if (read_part(512) != -EIO) {
        fail("the first read past the deadline was not EIO");
    }
    elapsed = now_ms() - start;
    if (elapsed < 5000u || elapsed > 6900u) {
        fail("the first read did not end at the deadline");
    }
    /* Let the late completion land and free the quarantined slot. */
    start = now_ms();
    while (now_ms() - start < 3000u) {
    }
    start = now_ms();
    if (read_part(512) != -EIO) {
        fail("the second read past the deadline was not EIO");
    }
    elapsed = now_ms() - start;
    if (elapsed < 5000u || elapsed > 6900u) {
        fail("the second read did not end at the deadline");
    }
    puts("LAGPROBE OK");
}

/* A sector written through the lag and read back: the kernel copies a write
 * into the request page before the device sleeps on it, and this is that
 * copy observed from user space. The offset is 256 KiB into the partition:
 * inside even the small fixture partition, and clear of what it reads. */
static void run_write(void)
{
    int fd = syscall3(SYS_OPEN, (int)(size_t)"/dev/bootpart", O_RDWR, 0);
    unsigned char *pattern = sector_buf;
    unsigned char *back = sector_buf + 512;
    unsigned start;
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
    start = now_ms();
    if (syscall3(SYS_WRITE, fd, (int)(size_t)pattern, 512) != 512) {
        fail("the write through the lag");
    }
    if (now_ms() - start < 80u) {
        fail("the write did not take the lag");
    }
    if (syscall3(SYS_LSEEK, fd, 262144, 0) != 262144) {
        fail("seek back");
    }
    start = now_ms();
    if (syscall3(SYS_READ, fd, (int)(size_t)back, 512) != 512) {
        fail("the read back");
    }
    if (now_ms() - start < 80u) {
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

/* The mounted half, under `rach2.blocklag=N` with the FAT32 partition on
 * /dev/disk0p2: two children read the same cold block at once and the
 * cache counts one waiting for the busy buffer; two processes sharing one
 * description through fork read a file to its end and between them see
 * every byte once; and a child runs rmdir on the target while the parent's
 * mount sleeps in the constructor and gets EBUSY. */

#define SYS_WAITPID   7
#define SYS_MKDIR     39
#define SYS_RMDIR     40
#define SYS_MOUNT     21
#define SYS_UMOUNT2   52
#define SYS_NANOSLEEP 162

#define MS_RDONLY 1
#define EBUSY     16

static int sys5(int n, int a, int b, int c, int d, int e)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory");
    return r;
}

static void sleep_ms(int ms)
{
    int ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    syscall2(SYS_NANOSLEEP, (int)ts, 0);
}

static int wait_status(int pid)
{
    int status = 0;

    if (syscall3(SYS_WAITPID, pid, (int)&status, 0) != pid) {
        fail("waitpid");
    }
    return status;
}

/* The number after `label` in /proc/disk, or -1. */
static int disk_counter(const char *label)
{
    static char text[2048];
    int fd = syscall3(SYS_OPEN, (int)"/proc/disk", O_RDONLY, 0);
    int n, i, j;

    if (fd < 0) {
        return -1;
    }
    n = syscall3(SYS_READ, fd, (int)text, sizeof(text) - 1);
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

/* `lagprobe root`, under `rach2.fsdisk rach2.blocklag=N`: the root is the
   disk's KRFFS partition, read through the real device, which /proc/mounts
   says by naming the partition as the root's source. */
static void run_root(void)
{
    static char text[512];
    int fd = syscall3(SYS_OPEN, (int)"/proc/mounts", O_RDONLY, 0);
    int n;

    if (fd < 0) {
        fail("open /proc/mounts");
    }
    n = syscall3(SYS_READ, fd, (int)text, (int)sizeof(text) - 1);
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

/* `lagprobe blockfail`, under `rach2.blocklag=3 rach2.blockfail=3`: the
   third write through the lag fails, which is inside the first write to
   the mounted volume; the mount goes read-only with a record, /proc/mounts
   says `ro`, and the next write answers EROFS. */
static void run_blockfail(void)
{
    static char text[1024];
    int fd, n, i, ro = 0;

    if (syscall2(SYS_MKDIR, (int)"/lb", 0755) != 0) {
        fail("mkdir /lb");
    }
    if (sys5(SYS_MOUNT, (int)"/dev/disk0p2", (int)"/lb", (int)"vfat", 0, 0) != 0) {
        fail("the writable mount through the lag");
    }
    fd = syscall3(SYS_OPEN, (int)"/lb/FAIL.TXT", O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
        fail("the first write to the volume did not fail");
    }
    fd = syscall3(SYS_OPEN, (int)"/proc/mounts", O_RDONLY, 0);
    n = fd < 0 ? -1 : syscall3(SYS_READ, fd, (int)text, (int)sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        fail("read /proc/mounts");
        return;
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
    if (!ro) {
        fail("the mount whose device failed a write is not read-only in /proc/mounts");
    }
    if (syscall3(SYS_OPEN, (int)"/lb/AGAIN.TXT", O_WRONLY | O_CREAT, 0644) != -EROFS) {
        fail("a write after the failure is not EROFS");
    }
    if (syscall2(SYS_UMOUNT2, (int)"/lb", 0) != 0) {
        fail("umount2 of the read-only mount");
    }
    puts("LAGPROBE OK");
}

/* `lagprobe writers`, under the lag with the FAT32 partition mounted for
   writing: two children each create a file and extend it eight times, and
   the parent syncs the volume twenty times while they do; every write
   sleeps in the lag, so the requests interleave. Each file must read back
   as its own bytes, whole, every sync must return 0, and the volume must
   unmount clean. The sync's turn under the lock is not observable from
   here: the driver's assertion at every mutation is what catches a write
   outside it. */
#define WRITER_CHUNK  4096
#define WRITER_ROUNDS 8

static void writer(const char *path, unsigned char fill)
{
    static unsigned char chunk[WRITER_CHUNK];
    int fd, i, r;

    for (i = 0; i < WRITER_CHUNK; i++) {
        chunk[i] = (unsigned char)(fill + i);
    }
    fd = syscall3(SYS_OPEN, (int)path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        sys_exit(2);
    }
    for (r = 0; r < WRITER_ROUNDS; r++) {
        if (syscall3(SYS_WRITE, fd, (int)chunk, WRITER_CHUNK) != WRITER_CHUNK) {
            sys_exit(3);
        }
    }
    syscall1(SYS_CLOSE, fd);
    sys_exit(0);
}

static void check_written(const char *path, unsigned char fill)
{
    static unsigned char storage[WRITER_CHUNK];
    unsigned char *buf = storage; /* read into through the pointer */
    int fd = syscall3(SYS_OPEN, (int)path, O_RDONLY, 0);
    int r, i;

    if (fd < 0) {
        fail("a writer's file is missing");
    }
    for (r = 0; r < WRITER_ROUNDS; r++) {
        if (syscall3(SYS_READ, fd, (int)buf, WRITER_CHUNK) != WRITER_CHUNK) {
            fail("a writer's file is short");
        }
        for (i = 0; i < WRITER_CHUNK; i++) {
            if (buf[i] != (unsigned char)(fill + i)) {
                fail("a writer's file holds another writer's bytes");
            }
        }
    }
    if (syscall3(SYS_READ, fd, (int)buf, 16) != 0) {
        fail("a writer's file is long");
    }
    syscall1(SYS_CLOSE, fd);
}

static void run_writers(void)
{
    int a, b, s;

    if (syscall2(SYS_MKDIR, (int)"/lw", 0755) != 0) {
        fail("mkdir /lw");
    }
    if (sys5(SYS_MOUNT, (int)"/dev/disk0p2", (int)"/lw", (int)"vfat", 0, 0) != 0) {
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
    if (syscall2(SYS_UMOUNT2, (int)"/lw", 0) != 0) {
        fail("umount2 of the written volume");
    }
    puts("LAGPROBE OK");
}

#define PATTERN_SECTORS 128

static unsigned char parent_marks[PATTERN_SECTORS];
static unsigned char child_marks[PATTERN_SECTORS];
static int pipe_fds[2];

/* Read `fd` one sector at a time to the end and mark each sector received
   in `marks`; a sector whose 128 fields do not all say the same number, or
   a number out of range, or a short read, fails the probe. */
static void mark_sectors(int fd, unsigned char *marks)
{
    int n, i, v;

    while ((n = syscall3(SYS_READ, fd, (int)sector_buf, 512)) > 0) {
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
        /* The reader that finishes a sector keeps the CPU and would take
           the description again before the other wakes; a tick's sleep
           lets the two alternate, which is what the marks then show. */
        sleep_ms(10);
    }
    if (n < 0) {
        fail("a read of pattern.bin failed");
    }
}

static void run_mounted(void)
{
    int pid, pid2, fd, before, after;
    int total;
    int hello_bytes;

    if (syscall2(SYS_MKDIR, (int)"/lm", 0755) != 0) {
        fail("mkdir /lm");
    }

    /* The constructor sleeps on the lag; a child that removes the target
       meanwhile must be refused, because the slot holds the covered node. */
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        sleep_ms(30);
        if (syscall1(SYS_RMDIR, (int)"/lm") != -EBUSY) {
            sys_exit(3);
        }
        sys_exit(0);
    }
    if (sys5(SYS_MOUNT, (int)"/dev/disk0p2", (int)"/lm", (int)"vfat", MS_RDONLY, 0) != 0) {
        fail("the mount through the lag");
    }
    if (wait_status(pid) != 0) {
        fail("rmdir of the target while the constructor slept was not EBUSY");
    }

    /* Two readers of one cold block: the second sleeps on the busy buffer
       and the cache counts the wait. */
    before = disk_counter("waits");
    pid = syscall1(SYS_FORK, 0);
    pid2 = pid == 0 ? 0 : syscall1(SYS_FORK, 0);
    if (pid == 0 || pid2 == 0) {
        fd = syscall3(SYS_OPEN, (int)"/lm/hello.txt", O_RDONLY, 0);
        if (fd < 0 || syscall3(SYS_READ, fd, (int)sector_buf, 512) <= 0) {
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

    /* One description shared through fork: each side reads one sector at
       a time until the end and marks which sectors it received, by the
       number every sector of pattern.bin carries in each of its 128 fields;
       the child's marks come back through a pipe, and every sector must
       be marked by exactly one of the two. */
    fd = syscall3(SYS_OPEN, (int)"/lm/pattern.bin", O_RDONLY, 0);
    if (fd < 0) {
        fail("open the shared file");
    }
    hello_bytes = syscall3(SYS_LSEEK, fd, 0, 2);
    if (hello_bytes != PATTERN_SECTORS * 512 || syscall3(SYS_LSEEK, fd, 0, 0) != 0) {
        fail("the shared file's size");
    }
    if (syscall1(SYS_PIPE, (int)pipe_fds) != 0) {
        fail("pipe");
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        mark_sectors(fd, child_marks);
        if (syscall3(SYS_WRITE, pipe_fds[1], (int)child_marks, PATTERN_SECTORS) !=
            PATTERN_SECTORS) {
            sys_exit(5);
        }
        sys_exit(0);
    }
    mark_sectors(fd, parent_marks);
    if (wait_status(pid) != 0) {
        fail("the child reader");
    }
    if (syscall3(SYS_READ, pipe_fds[0], (int)child_marks, PATTERN_SECTORS) != PATTERN_SECTORS) {
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

    /* The whole-disk window is on the real disk and the mount is on the lag
       wrapper over it; the claim must compare the disk under both. */
    if (syscall3(SYS_OPEN, (int)"/dev/disk0", O_RDWR, 0) != -EBUSY) {
        fail("a whole-disk writer opened over a mounted range");
    }

    if (syscall2(SYS_UMOUNT2, (int)"/lm", 0) != 0) {
        fail("umount2 of the lag mount");
    }
    syscall1(SYS_RMDIR, (int)"/lm");
    puts("LAGPROBE OK");
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

    if (argc >= 2 && argv[1][0] == 'd') {
        run_deadline();
    } else if (argc >= 2 && argv[1][0] == 'w' && argv[1][5] == 'r') {
        run_writers();
    } else if (argc >= 2 && argv[1][0] == 'w') {
        run_write();
    } else if (argc >= 2 && argv[1][0] == 'm') {
        run_mounted();
    } else if (argc >= 2 && argv[1][0] == 'r') {
        run_root();
    } else if (argc >= 2 && argv[1][0] == 'b') {
        run_blockfail();
    } else {
        run_sleep();
    }
    sys_exit(0);
}
