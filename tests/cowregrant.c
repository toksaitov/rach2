/**
 * Stresses the copy-on-write sole-owner re-grant arm for leaks and corruption.
 *
 * When a write faults on a COW page the kernel (`cow_fault`, mm/paging.c)
 * takes one of two paths: if this address space holds the frame's last
 * reference, the page is made writable in place with no copy (the re-grant
 * arm); otherwise a private copy takes its place and the shared frame's
 * reference is dropped (the shared-copy arm). This probe drives the re-grant
 * arm every cycle: the child writes first (its write takes the shared-copy
 * arm and leaves the parent the sole owner), and the parent reaps it and then
 * writes the same page (that write takes the re-grant arm). Over many cycles
 * it catches the faults that arm can have: a leak of the re-granted frame
 * shows as MemFree in /proc/meminfo fallen by more than SLACK across the
 * cycles after a warm-up, a double free as a fork that runs out of frames or
 * a kernel fault, and a re-grant of a frame still shared as the child's
 * write changing the parent's bytes. It cannot tell a correct re-grant in
 * place from a correct but wasteful copy every time, since the two look the
 * same from user space; telling them apart needs a host test or a count of
 * the kernel's copies.
 *
 * Prints "COWREGRANT OK" on success. etude's report (probe_report.h), for
 * the case ETUDE_CASE names, has an assertion for each check of the cycles,
 * `fork`, `child`, `parent-unchanged` and `regrant-write`, reported once
 * the cycles end, and `memfree`, the reads of MemFree and its fall. The
 * first failure fails its assertion and leaves those not yet reported not
 * run. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define STDOUT_FILENO 1

#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

#define WARMUP 8
#define CYCLES 200
/* The fall in MemFree allowed, in KiB: well below the 800 KiB a leak of a
 * frame a cycle would cost, and above the jitter of the kernel heap. */
#define SLACK  64u

/* A global lives in the writable data segment, so after fork it is a COW
 * page. */
static volatile unsigned shared_val;

/** write(2): the bytes written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** exit(2) with `status`; does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** fork(2): the child's pid, 0 in the child, or a negated errno. */
static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

/** waitpid(2): the pid reaped, or a negated errno. */
static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
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
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/* The assertions, in the order they are reported, and how many of them
 * have held. */
enum { FORK, CHILD, PARENT_UNCHANGED, REGRANT_WRITE, MEMFREE, ASSERTIONS };
static const char *const ids[ASSERTIONS] = {"fork", "child", "parent-unchanged", "regrant-write",
                                            "memfree"};
static int held;

/** Print "COWREGRANT FAIL: " and `why`, report the assertion `which` failed
 * and those after the held ones not run, and exit 1. */
static void fail(int which, const char *why)
{
    int i;

    sys_write(STDOUT_FILENO, "COWREGRANT FAIL: ", 17);
    puts(why);
    for (i = held; i < ASSERTIONS; i++) {
        if (i == which) {
            report_fail(ids[i], why);
        } else {
            report_notrun(ids[i], "a failed check ended the cycles");
        }
    }
    report_end();
    sys_exit(1);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * Run WARMUP and then CYCLES cycles of fork, the child's write, the reap and
 * the parent's write, and fail at the first wrong value or failed call, or
 * when MemFree after the cycles is more than SLACK below where it was after
 * the warm-up. The child exits 7 when its copy held the parent's value and
 * took its own write, 11 when it did not inherit the value and 12 when its
 * write did not stick. `_start` calls it with the initial stack.
 */
void probe_main(unsigned long *sp)
{
    int pid, status, i;
    unsigned free_kb = 0, end_kb;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.cowregrant"));
    for (i = 0; i < WARMUP + CYCLES; i++) {
        unsigned base = 0x10000u + (unsigned)i;

        if (i == WARMUP) {
            free_kb = probe_meminfo("MemFree:");
            if (free_kb == 0xFFFFFFFFu) {
                fail(MEMFREE, "MemFree in /proc/meminfo");
            }
        }
        shared_val = base;

        pid = sys_fork();
        if (pid < 0) {
            fail(FORK, "frames exhausted: the re-grant arm leaks a frame");
        }
        if (pid == 0) {
            if (shared_val != base) {
                sys_exit(11); /* did not inherit the shared value */
            }
            shared_val = base ^ 0xFFFFu; /* shared-copy arm: a private copy */
            if (shared_val != (base ^ 0xFFFFu)) {
                sys_exit(12);
            }
            sys_exit(7);
        }

        if (sys_waitpid(pid, &status, 0) != pid) {
            fail(CHILD, "waitpid");
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 7) {
            fail(CHILD, "child split path did not run cleanly");
        }
        if (shared_val != base) {
            fail(PARENT_UNCHANGED, "parent bytes changed: re-grant of a still-shared frame");
        }

        shared_val = base + 0x20000u; /* re-grant arm: sole owner, in place */
        if (shared_val != base + 0x20000u) {
            fail(REGRANT_WRITE, "re-grant write did not stick");
        }
    }

    for (; held < MEMFREE; held++) {
        report_pass(ids[held]);
    }
    end_kb = probe_meminfo("MemFree:");
    if (end_kb == 0xFFFFFFFFu || (free_kb > end_kb && free_kb - end_kb > SLACK)) {
        fail(MEMFREE, "MemFree fell across the cycles: the re-grant arm leaks a frame");
    }

    report_pass(ids[MEMFREE]);
    report_end();
    puts("COWREGRANT OK");
    sys_exit(0);
}
