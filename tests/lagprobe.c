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
    } else {
        run_sleep();
    }
    sys_exit(0);
}
