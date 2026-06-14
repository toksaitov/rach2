/*
 * timeprobe.c - Phase 7 probe: time, sleep-with-yield, and entropy for Rach2.
 *
 * Checks, from user space via direct int 0x80 syscalls:
 *   - clock_gettime(CLOCK_MONOTONIC) never goes backwards;
 *   - getrandom returns the requested count and varying bytes;
 *   - nanosleep(N) waits about N AND yields: a child forked just before the sleep
 *     runs *during* it (it creates a marker file the parent finds on waking),
 *     which only happens if the sleeping parent gives up the CPU.
 * Prints "TIMEPROBE OK".
 *
 * Freestanding; no 64-bit division (the user image has no libgcc either). Build:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 \
 *                -o timeprobe timeprobe.c
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT          1
#define SYS_FORK          2
#define SYS_WRITE         4
#define SYS_OPEN          5
#define SYS_CLOSE         6
#define SYS_WAITPID       7
#define SYS_CREAT         8
#define SYS_UNLINK        10
#define SYS_NANOSLEEP     162
#define SYS_CLOCK_GETTIME 265
#define SYS_GETRANDOM     355

#define O_RDONLY        0
#define CLOCK_MONOTONIC 1

struct timespec {
    int tv_sec;
    int tv_nsec;
};

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
    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)s, (int)strlen(s));
    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)"\n", 1);
}

static int fails = 0;

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)"  FAIL: ", 8);
        puts(msg);
    }
}

/* Milliseconds between two timespecs, all in 32-bit math. */
static int elapsed_ms(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000;
}

void _start(void)
{
    struct timespec t0, t1, before, after;
    unsigned char r1[16], r2[16];
    int i, differ, pid, status, slept_ms;

    /* 1. CLOCK_MONOTONIC must not run backwards. */
    check(syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (int)(size_t)&t0) == 0, "clock_gettime t0");
    check(syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (int)(size_t)&t1) == 0, "clock_gettime t1");
    check(t1.tv_sec > t0.tv_sec || (t1.tv_sec == t0.tv_sec && t1.tv_nsec >= t0.tv_nsec),
          "monotonic clock does not go backwards");
    check(t0.tv_nsec >= 0 && t0.tv_nsec < 1000000000, "tv_nsec in range");

    /* 2. getrandom returns the count and varying bytes. Pre-zero both buffers so
     * the difference test is honest (a broken getrandom that writes nothing leaves
     * them equal) and so they are visibly written. */
    for (i = 0; i < 16; i++) {
        r1[i] = 0;
        r2[i] = 0;
    }
    check(syscall3(SYS_GETRANDOM, (int)(size_t)r1, 16, 0) == 16, "getrandom r1 returns 16");
    check(syscall3(SYS_GETRANDOM, (int)(size_t)r2, 16, 0) == 16, "getrandom r2 returns 16");
    differ = 0;
    for (i = 0; i < 16; i++) {
        if (r1[i] != r2[i]) {
            differ = 1;
        }
    }
    check(differ, "two getrandom draws differ");

    /* 3. nanosleep waits ~N AND yields: a child forked right before the sleep
     *    creates /childran while the parent sleeps; the parent finds it on waking. */
    syscall1(SYS_UNLINK, (int)(size_t)"/childran");

    pid = syscall1(SYS_FORK, 0);
    check(pid >= 0, "fork");
    if (pid == 0) {
        /* Child: its first act is to drop the marker, then exit. If the parent's
         * sleep yields, this runs during the parent's nanosleep. */
        int fd = syscall2(SYS_CREAT, (int)(size_t)"/childran", 0644);
        if (fd >= 0) {
            syscall3(SYS_WRITE, fd, (int)(size_t)"x", 1);
            syscall1(SYS_CLOSE, fd);
        }
        sys_exit(0);
    }

    {
        struct timespec req;
        req.tv_sec = 0;
        req.tv_nsec = 150000000; /* 150 ms */

        syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (int)(size_t)&before);
        check(syscall2(SYS_NANOSLEEP, (int)(size_t)&req, 0) == 0, "nanosleep 150ms");
        syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (int)(size_t)&after);
    }

    slept_ms = elapsed_ms(&before, &after);
    check(slept_ms >= 130, "nanosleep waited at least ~150ms");
    check(slept_ms < 2000, "nanosleep did not overshoot wildly");

    {
        /* The marker exists only if the child ran during the parent's sleep. */
        int fd = syscall3(SYS_OPEN, (int)(size_t)"/childran", O_RDONLY, 0);
        check(fd >= 0, "child made progress during the sleep (nanosleep yields)");
        if (fd >= 0) {
            syscall1(SYS_CLOSE, fd);
        }
    }

    check(syscall3(SYS_WAITPID, pid, (int)(size_t)&status, 0) == pid, "reap child");
    syscall1(SYS_UNLINK, (int)(size_t)"/childran");

    if (fails == 0) {
        puts("TIMEPROBE OK");
    } else {
        puts("TIMEPROBE FAIL");
    }
    sys_exit(fails);
}
