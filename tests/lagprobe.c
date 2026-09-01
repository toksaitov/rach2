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
 * block make the cache count a busy-buffer wait; and one description shared
 * through fork hands every byte of a file out once between the two readers.
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
#define SYS_GETTIMEOFDAY 78

#define O_RDONLY 0
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

/* One read of `len` bytes from the start of /dev/disk0p1; bytes or -1. */
static int read_part(unsigned len)
{
    int fd = syscall3(SYS_OPEN, (int)(size_t)"/dev/disk0p1", O_RDONLY, 0);
    int r;

    if (fd < 0) {
        return -1;
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
    /* 16 page requests, 3 ticks each: at least ~240 ms in requests alone. */
    if (elapsed < 100u) {
        fail("the read did not take the lag");
    }
    if (got < 5) {
        fail("the sibling did not run while the reader slept");
    }
    puts("LAGPROBE OK");
}

static void run_deadline(void)
{
    unsigned start = now_ms();

    if (read_part(512) >= 0) {
        fail("a read succeeded past the deadline");
    }
    if (now_ms() - start > 8000u) {
        fail("the deadline took longer than the deadline");
    }
    /* Let the late completion land and free the quarantined slot. */
    start = now_ms();
    while (now_ms() - start < 3000u) {
    }
    if (read_part(512) >= 0) {
        fail("a second read succeeded past the deadline");
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
    if (syscall3(SYS_WRITE, fd, (int)(size_t)pattern, 512) != 512) {
        fail("the write through the lag");
    }
    if (syscall3(SYS_LSEEK, fd, 262144, 0) != 262144) {
        fail("seek back");
    }
    if (syscall3(SYS_READ, fd, (int)(size_t)back, 512) != 512) {
        fail("the read back");
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

    /* One description shared through fork: each side reads chunks until
       the end and reports its byte count; the two counts sum to the file's
       size, so no byte was read twice and none was skipped. */
    fd = syscall3(SYS_OPEN, (int)"/lm/hello", O_RDONLY, 0);
    if (fd < 0) {
        fail("open the shared file");
    }
    hello_bytes = syscall3(SYS_LSEEK, fd, 0, 2);
    if (hello_bytes <= 0 || syscall3(SYS_LSEEK, fd, 0, 0) != 0) {
        fail("the shared file's size");
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        int mine = 0;
        int n;

        while ((n = syscall3(SYS_READ, fd, (int)sector_buf, 512)) > 0) {
            mine += n;
        }
        sys_exit(mine / 512);
    }
    total = 0;
    {
        int n;

        while ((n = syscall3(SYS_READ, fd, (int)sector_buf, 512)) > 0) {
            total += n;
        }
    }
    total += (wait_status(pid) >> 8) * 512;
    syscall1(SYS_CLOSE, fd);
    if (total != hello_bytes) {
        fail("the shared description did not hand out every byte once");
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
    } else if (argc >= 2 && argv[1][0] == 'w') {
        run_write();
    } else if (argc >= 2 && argv[1][0] == 'm') {
        run_mounted();
    } else {
        run_sleep();
    }
    sys_exit(0);
}
