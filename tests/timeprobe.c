/**
 * timeprobe: the kernel's clock, its sleep and its random bytes, tested
 * from user space.
 *
 * Three checks run through direct system calls:
 *
 *   * clock_gettime(CLOCK_MONOTONIC) does not go backwards;
 *   * getrandom returns the requested count and varying bytes;
 *   * nanosleep(N) waits about N and yields: a child forked just before the
 *     sleep creates a marker file only once /proc shows the parent asleep,
 *     which happens only if the sleeping parent gives up the CPU, and the
 *     parent, which sleeps only in nanosleep until it finds the marker,
 *     requires it.
 *
 * It prints "TIMEPROBE OK" and exits 0 when all pass, and "TIMEPROBE FAIL"
 * with the count of failed checks as its status otherwise. It divides
 * nothing wider than a long, since the probes link without libgcc.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, an assertion for each check, in `part_ids`, the two reads of the
 * clock and the two draws of getrandom one assertion each; an assertion
 * fails with its first failed check.
 */

#include "probe.h"
#include "probe_report.h"

#define STDOUT_FILENO 1

#define O_RDONLY        0
#define CLOCK_MONOTONIC 1

/* The yield row's marker, in the working directory: the root as the test
 * image runs the probe, the workspace in a native run. */
#define MARKER "childran"

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

/* The assertions, one for each check or pair of checks made the same way,
 * and the first failed check of each, or NULL. */
enum {
    CLOCK_GETTIME,
    MONOTONIC,
    NSEC_RANGE,
    GETRANDOM_COUNT,
    GETRANDOM_DIFFER,
    FORK,
    NANOSLEEP,
    SLEEP_LENGTH,
    YIELD,
    REAP,
    PARTS
};

static const char *const part_ids[PARTS] = {
    "clock-gettime", "monotonic", "nsec-range",   "getrandom-count", "getrandom-differ",
    "fork",          "nanosleep", "sleep-length", "yield",           "reap"};
static const char *part_why[PARTS];

/** Count a failure of the assertion `part` and print `msg` when `cond` is
 * false; the first such `msg` of the assertion is the one reported. */
static void check(int part, int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, STDOUT_FILENO, (long)"  FAIL: ", 8);
        puts(msg);
        if (part_why[part] == 0) {
            part_why[part] = msg;
        }
    }
}

/** The state letter of process `pid`, the field after the name in
 * /proc/<pid>/stat, or 0 when it cannot be read. */
static char state_of(int pid)
{
    char path[32] = "/proc/";
    char digits[12];
    static char line[256];
    int i = 0, j = 6, fd, n;

    do {
        digits[i++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0);
    while (i > 0) {
        path[j++] = digits[--i];
    }
    for (i = 0; "/stat"[i] != '\0'; i++) {
        path[j++] = "/stat"[i];
    }
    path[j] = '\0';
    fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_READ, fd, (long)line, (int)sizeof(line));
    syscall1(SYS_CLOSE, fd);
    for (i = n - 1; i >= 0 && line[i] != ')'; i--) {
    }
    return i >= 0 && i + 2 < n && line[i + 1] == ' ' ? line[i + 2] : 0;
}

/** Sleep for `ms` milliseconds, which are below a second. */
static void nap(long ms)
{
    struct timespec req;

    req.tv_sec = 0;
    req.tv_nsec = ms * 1000000L;
    /* interval: naps between looks; the parent's are the sleep tested */
    syscall2(SYS_NANOSLEEP, (long)&req, 0);
}

/**
 * The child of the yield row: once /proc shows the parent `parent` asleep,
 * create the marker and exit 0. A parent that has ended, whose state is gone
 * or Z, ends the child with status 1 and no marker. Does not return.
 */
static void mark_when_asleep(int parent)
{
    char state;
    int fd;

    for (;;) {
        state = state_of(parent);
        if (state == 'S') {
            break;
        }
        if (state == 0 || state == 'Z') {
            sys_exit(1);
        }
        nap(1);
    }
    fd = syscall2(SYS_CREAT, (long)MARKER, 0644);
    if (fd >= 0) {
        syscall3(SYS_WRITE, fd, (long)"x", 1);
        syscall1(SYS_CLOSE, fd);
    }
    sys_exit(0);
}

/**
 * Whether the child `pid` created the marker. The parent naps 10 ms at a time,
 * so that it sleeps only in nanosleep, until the marker exists or the child
 * has ended, which its state Z shows. The child is left to be reaped.
 */
static int child_marked(int pid)
{
    int fd, ended = 0;

    for (;;) {
        fd = syscall3(SYS_OPEN, (long)MARKER, O_RDONLY, 0);
        if (fd >= 0) {
            syscall1(SYS_CLOSE, fd);
            return 1;
        }
        if (ended) {
            return 0;
        }
        ended = state_of(pid) == 'Z';
        nap(10);
    }
}

/** Milliseconds between two timespecs, in the width of their fields. */
static int elapsed_ms(const struct timespec *a, const struct timespec *b)
{
    return (int)((b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe: the three checks in order, each counted by `check`.
 *
 * Both getrandom buffers start zeroed, so a call that writes nothing fails
 * the test that the two differ.
 *
 * The sleep of 150 ms is 150 ticks; two reads of a clock that moves in
 * ticks can straddle two of them, and the bound of 130 leaves room past
 * that. How far the sleep overshot depends on the machine, and the probe
 * does not judge it.
 *
 * `_start` calls it with the initial stack, whose environment gives the
 * case's identity.
 */
void probe_main(unsigned long *sp)
{
    struct timespec t0, t1, before, after;
    unsigned char r1[16], r2[16];
    int i, differ, pid, status, slept_ms;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.timeprobe"));

    /* 1. CLOCK_MONOTONIC does not run backwards. */
    check(CLOCK_GETTIME, syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0) == 0,
          "clock_gettime t0");
    check(CLOCK_GETTIME, syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1) == 0,
          "clock_gettime t1");
    check(MONOTONIC, t1.tv_sec > t0.tv_sec || (t1.tv_sec == t0.tv_sec && t1.tv_nsec >= t0.tv_nsec),
          "monotonic clock does not go backwards");
    check(NSEC_RANGE, t0.tv_nsec >= 0 && t0.tv_nsec < 1000000000, "tv_nsec in range");

    /* 2. getrandom returns the count and bytes that differ. */
    for (i = 0; i < 16; i++) {
        r1[i] = 0;
        r2[i] = 0;
    }
    check(GETRANDOM_COUNT, syscall3(SYS_GETRANDOM, (long)r1, 16, 0) == 16,
          "getrandom r1 returns 16");
    check(GETRANDOM_COUNT, syscall3(SYS_GETRANDOM, (long)r2, 16, 0) == 16,
          "getrandom r2 returns 16");
    differ = 0;
    for (i = 0; i < 16; i++) {
        if (r1[i] != r2[i]) {
            differ = 1;
        }
    }
    check(GETRANDOM_DIFFER, differ, "two getrandom draws differ");

    /* 3. nanosleep waits about N and yields to the child. */
    syscall1(SYS_UNLINK, (long)MARKER);

    pid = syscall1(SYS_FORK, 0);
    check(FORK, pid >= 0, "fork");
    if (pid == 0) {
        mark_when_asleep((int)syscall0(SYS_GETPPID));
    }

    {
        struct timespec req;
        req.tv_sec = 0;
        req.tv_nsec = 150000000; /* 150 ms */

        syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
        /* interval: the sleep the row tests */
        check(NANOSLEEP, syscall2(SYS_NANOSLEEP, (long)&req, 0) == 0, "nanosleep 150ms");
        syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    }

    slept_ms = elapsed_ms(&before, &after);
    check(SLEEP_LENGTH, slept_ms >= 130, "nanosleep waited at least ~150ms");

    check(YIELD, pid > 0 && child_marked(pid),
          "child made progress during the sleep (nanosleep yields)");
    check(REAP, syscall3(SYS_WAITPID, pid, (long)&status, 0) == pid, "reap child");
    syscall1(SYS_UNLINK, (long)MARKER);

    for (i = 0; i < PARTS; i++) {
        if (part_why[i] != 0) {
            report_fail(part_ids[i], part_why[i]);
        } else {
            report_pass(part_ids[i]);
        }
    }
    report_end();

    if (fails == 0) {
        puts("TIMEPROBE OK");
    } else {
        puts("TIMEPROBE FAIL");
    }
    sys_exit(fails);
}
