/**
 * timeprobe: the kernel's clock, its sleep and its random bytes, tested
 * from user space.
 *
 * Three checks run through direct system calls:
 *
 *   * clock_gettime(CLOCK_MONOTONIC) does not go backwards;
 *   * getrandom returns the requested count and varying bytes;
 *   * nanosleep(N) waits about N and yields: a child forked just before the
 *     sleep runs during it, creating a marker file the parent finds on
 *     waking, which happens only if the sleeping parent gives up the CPU.
 *
 * It prints "TIMEPROBE OK" and exits 0 when all pass, and "TIMEPROBE FAIL"
 * with the count of failed checks as its status otherwise. It does no
 * 64-bit division, since the probes link without libgcc.
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define O_RDONLY        0
#define CLOCK_MONOTONIC 1

/* The kernel's timespec, whose fields are longs. */
struct timespec {
    long tv_sec;
    long tv_nsec;
};

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
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"\n", 1);
}

static int fails = 0;

/** Count a failure and print `msg` when `cond` is false. */
static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, STDOUT_FILENO, (long)"  FAIL: ", 8);
        puts(msg);
    }
}

/** Milliseconds between two timespecs, in the width of their fields. */
static int elapsed_ms(const struct timespec *a, const struct timespec *b)
{
    return (int)((b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000);
}

/**
 * The probe: the three checks in order, each counted by `check`.
 *
 * The sleep of 150 ms is fifteen ticks; two reads of a clock that moves in
 * ticks can straddle two of them, so the bound is 130. How far the sleep
 * overshot depends on the machine, and the probe does not judge it.
 */
PROBE_START void _start(void)
{
    struct timespec t0, t1, before, after;
    unsigned char r1[16], r2[16];
    int i, differ, pid, status, slept_ms;

    /* 1. CLOCK_MONOTONIC does not run backwards. */
    check(syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0) == 0, "clock_gettime t0");
    check(syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1) == 0, "clock_gettime t1");
    check(t1.tv_sec > t0.tv_sec || (t1.tv_sec == t0.tv_sec && t1.tv_nsec >= t0.tv_nsec),
          "monotonic clock does not go backwards");
    check(t0.tv_nsec >= 0 && t0.tv_nsec < 1000000000, "tv_nsec in range");

    /* 2. getrandom: both buffers start equal, so a call that writes nothing
     * fails the difference test. */
    for (i = 0; i < 16; i++) {
        r1[i] = 0;
        r2[i] = 0;
    }
    check(syscall3(SYS_GETRANDOM, (long)r1, 16, 0) == 16, "getrandom r1 returns 16");
    check(syscall3(SYS_GETRANDOM, (long)r2, 16, 0) == 16, "getrandom r2 returns 16");
    differ = 0;
    for (i = 0; i < 16; i++) {
        if (r1[i] != r2[i]) {
            differ = 1;
        }
    }
    check(differ, "two getrandom draws differ");

    /* 3. nanosleep waits about N and yields to the child. */
    syscall1(SYS_UNLINK, (long)"/childran");

    pid = syscall1(SYS_FORK, 0);
    check(pid >= 0, "fork");
    if (pid == 0) {
        int fd = syscall2(SYS_CREAT, (long)"/childran", 0644);
        if (fd >= 0) {
            syscall3(SYS_WRITE, fd, (long)"x", 1);
            syscall1(SYS_CLOSE, fd);
        }
        sys_exit(0);
    }

    {
        struct timespec req;
        req.tv_sec = 0;
        req.tv_nsec = 150000000; /* 150 ms */

        syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
        check(syscall2(SYS_NANOSLEEP, (long)&req, 0) == 0, "nanosleep 150ms");
        syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    }

    slept_ms = elapsed_ms(&before, &after);
    check(slept_ms >= 130, "nanosleep waited at least ~150ms");

    {
        int fd = syscall3(SYS_OPEN, (long)"/childran", O_RDONLY, 0);
        check(fd >= 0, "child made progress during the sleep (nanosleep yields)");
        if (fd >= 0) {
            syscall1(SYS_CLOSE, fd);
        }
    }

    check(syscall3(SYS_WAITPID, pid, (long)&status, 0) == pid, "reap child");
    syscall1(SYS_UNLINK, (long)"/childran");

    if (fails == 0) {
        puts("TIMEPROBE OK");
    } else {
        puts("TIMEPROBE FAIL");
    }
    sys_exit(fails);
}
