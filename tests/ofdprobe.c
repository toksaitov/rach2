/**
 * Probe for refcounted open file descriptions, from user space.
 *
 * Proves that a duplicated or forked descriptor shares one open file
 * description (and its seek offset), the Unix way, not each carrying
 * its own:
 *
 *   (a) dup2 shares the offset: a read through the dup continues where the
 *       original left off, not from 0;
 *   (b) fork shares the offset: a child advances the offset and the parent's
 *       next read sees the advance;
 *   (c) reference counts: closing one fd leaves the dup working, and the
 *       kernel heap's HeapUsed in /proc/meminfo is what it was before three
 *       hundred open and close cycles, and before an open held across a
 *       hundred forks and then closed, so no cycle and no fork leaves a
 *       description behind;
 *   (d) FD_CLOEXEC is per-fd: it round-trips through fcntl, and dup2 leaves
 *       the new fd's flag clear.
 *
 * The probe reads `hello.txt` in its working directory, or the file the
 * argument `hello=PATH` (probe_args.h) names, which holds "hello, world\n".
 * Given `machine=native`, it leaves out the two comparisons of HeapUsed,
 * since the kernel's heap is shared with every process of a machine that is
 * already up; the cycles and the forks still run.
 *
 * Prints "OFDPROBE OK" on success. Writes etude's report records
 * (probe_report.h) for the case ETUDE_CASE names, an assertion for each
 * check below, all of them, `begin` too, after the last check, so that no
 * record, and no work of the reader it wakes, moves the heap between two
 * readings of HeapUsed; `heap-cycles` and `heap-forks` are not run under
 * `machine=native`.
 *
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

#define STDOUT_FILENO 1

#define O_RDONLY 0

#define F_GETFD    1
#define F_SETFD    2
#define FD_CLOEXEC 1

/* The file of "hello, world\n": 0-4 hello, 5 ',', 6 ' ', 7 w..., by default
 * in the working directory, which is the root as the test image runs the
 * probe. */
static const char *hello;

/* The assertions, one for each check, in the order the probe makes them:
 * (a) from `open` to `dup-after-close`, (b) from `reopen` to `fork-offset`,
 * (c) from `meminfo` to `heap-forks`, and (d) from `cloexec-open` on. */
enum {
    OPEN,
    READ_HELLO,
    DUP2,
    DUP_OFFSET,
    DUP_AFTER_CLOSE,
    REOPEN,
    READ_HEL,
    FORK,
    FORK_OFFSET,
    MEMINFO,
    CYCLES,
    HEAP_CYCLES,
    HELD_OPEN,
    FORKS,
    HEAP_FORKS,
    CLOEXEC_OPEN,
    CLOEXEC,
    DUP2_CLOEXEC,
    DUP2_CLEARS,
    PARTS
};

static const char *const part_ids[PARTS] = {
    "open",         "read-hello",  "dup2",         "dup-offset",  "dup-after-close",
    "reopen",       "read-hel",    "fork",         "fork-offset", "meminfo",
    "cycles",       "heap-cycles", "held-open",    "forks",       "heap-forks",
    "cloexec-open", "cloexec",     "dup2-cloexec", "dup2-clears"};

/* The first failed check of each assertion, or NULL. */
static const char *part_why[PARTS];

/** exit(2) with `status`; does not return. */
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

/** Count a failure of the assertion `part` and print `msg` when `cond` is 0;
 * the first such `msg` of the assertion is the one reported. */
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

/** Check `cond` for `part` as `check` does, unless given `machine=native`. */
static void check_heap(int part, int cond, const char *msg)
{
    if (!probe_native()) {
        check(part, cond, msg);
    }
}

/** Read up to `n` bytes from `fd` into `buf`: the byte count, or a negated
 * errno. */
static int rd(int fd, char *buf, int n)
{
    return syscall3(SYS_READ, fd, (long)buf, n);
}

/** Whether the first `n` bytes of `a` and `b` are equal. */
static int eqn(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/** Run case (c): the heap after 300 open and close cycles, and after an open
 * held across 100 forks and then closed. No report record is written here,
 * since a record is a write that may move the heap. */
static void count_heap(void)
{
    int fd, status, i;
    unsigned used;

    used = probe_meminfo("HeapUsed:");
    check(MEMINFO, used != 0xFFFFFFFFu, "HeapUsed in /proc/meminfo");
    for (i = 0; i < 300; i++) {
        int f = syscall3(SYS_OPEN, (long)hello, O_RDONLY, 0);
        if (f < 0) {
            check(CYCLES, 0, "open in an open/close cycle");
            break;
        }
        syscall1(SYS_CLOSE, f);
    }
    check_heap(HEAP_CYCLES, probe_meminfo("HeapUsed:") == used,
               "300 open/close cycles left the heap as it was (no OFD leak)");

    used = probe_meminfo("HeapUsed:");
    fd = syscall3(SYS_OPEN, (long)hello, O_RDONLY, 0);
    check(HELD_OPEN, fd >= 0, "open held across the forks");
    for (i = 0; i < 100; i++) {
        int p = syscall1(SYS_FORK, 0);
        if (p == 0) {
            sys_exit(0);
        }
        if (p < 0 || syscall3(SYS_WAITPID, p, (long)&status, 0) != p || status != 0) {
            check(FORKS, 0, "a fork and reap with the open held");
            break;
        }
    }
    syscall1(SYS_CLOSE, fd);
    check_heap(HEAP_FORKS, probe_meminfo("HeapUsed:") == used,
               "100 forks and the close left the heap as it was (no fork OFD leak)");
}

/** Report each assertion: failed with its first failed check, held, or not
 * run for a comparison of HeapUsed that `machine=native` leaves out. */
static void report_parts(void)
{
    int i;

    for (i = 0; i < PARTS; i++) {
        if ((i == HEAP_CYCLES || i == HEAP_FORKS) && probe_native()) {
            report_notrun(part_ids[i], "machine=native leaves out the counts of the kernel heap");
        } else if (part_why[i] != 0) {
            report_fail(part_ids[i], part_why[i]);
        } else {
            report_pass(part_ids[i]);
        }
    }
    report_end();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run cases (a) to (d) on hello.txt, and exit with the count of the
 * checks that failed. */
void probe_main(unsigned long *sp)
{
    char buf[16];
    int fd, fd2, pid, status, n;
    const char *case_id;

    probe_args((int)sp[0], (char **)(sp + 1));
    hello = probe_arg("hello", "hello.txt");
    case_id = report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.ofdprobe");

    fd = syscall3(SYS_OPEN, (long)hello, O_RDONLY, 0);
    check(OPEN, fd >= 0, "open hello.txt");
    check(READ_HELLO, rd(fd, buf, 5) == 5 && eqn(buf, "hello", 5), "read first 5 == hello");

    fd2 = syscall2(SYS_DUP2, fd, 9);
    check(DUP2, fd2 == 9, "dup2 onto fd 9");
    n = rd(fd2, buf, 5); /* must continue from offset 5 */
    check(DUP_OFFSET, n == 5 && eqn(buf, ", wor", 5), "dup shares offset (reads ', wor')");

    syscall1(SYS_CLOSE, fd);
    n = rd(fd2, buf, 3); /* offset is now 10: "ld\n" */
    check(DUP_AFTER_CLOSE, n == 3 && eqn(buf, "ld\n", 3), "dup works after original closed");
    syscall1(SYS_CLOSE, fd2);

    fd = syscall3(SYS_OPEN, (long)hello, O_RDONLY, 0);
    check(REOPEN, fd >= 0, "reopen hello.txt");
    check(READ_HEL, rd(fd, buf, 3) == 3 && eqn(buf, "hel", 3), "parent reads first 3 == hel");

    pid = syscall1(SYS_FORK, 0);
    check(FORK, pid >= 0, "fork");
    if (pid == 0) {
        char cb[8];
        rd(fd, cb, 3); /* child advances offset 3 -> 6 */
        sys_exit(0);
    }
    syscall3(SYS_WAITPID, pid, (long)&status, 0);
    n = rd(fd, buf, 3); /* must see the child's advance: " wo" */
    check(FORK_OFFSET, n == 3 && eqn(buf, " wo", 3), "fork shares offset (parent reads ' wo')");
    syscall1(SYS_CLOSE, fd);

    count_heap();

    fd = syscall3(SYS_OPEN, (long)hello, O_RDONLY, 0);
    check(CLOEXEC_OPEN, fd >= 0, "open for cloexec test");
    syscall3(SYS_FCNTL, fd, F_SETFD, FD_CLOEXEC);
    check(CLOEXEC, syscall3(SYS_FCNTL, fd, F_GETFD, 0) == FD_CLOEXEC, "F_SETFD/F_GETFD CLOEXEC");
    fd2 = syscall2(SYS_DUP2, fd, 10);
    check(DUP2_CLOEXEC, fd2 == 10, "dup2 onto fd 10");
    check(DUP2_CLEARS, syscall3(SYS_FCNTL, fd2, F_GETFD, 0) == 0, "dup2 clears CLOEXEC on new fd");
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_CLOSE, fd2);

    report_begin(case_id);
    report_parts();
    if (fails == 0) {
        puts("OFDPROBE OK");
    } else {
        puts("OFDPROBE FAIL");
    }
    sys_exit(fails);
}
