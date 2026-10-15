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
 * it catches the faults that arm can have: a leak or double free of the
 * re-granted frame shows as a fork that runs out of frames, and a re-grant of
 * a frame still shared shows as the child's write changing the parent's
 * bytes. It cannot tell a correct re-grant in place from a correct but
 * wasteful copy every time, since the two look the same from user space;
 * telling them apart needs a host test or a count of the kernel's copies.
 *
 * Prints "COWREGRANT OK" on success. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

#define CYCLES 200

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

/** Print "COWREGRANT FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    sys_write(STDOUT_FILENO, "COWREGRANT FAIL: ", 17);
    puts(why);
    sys_exit(1);
}

/**
 * Run CYCLES cycles of fork, the child's write, the reap and the parent's
 * write, and fail at the first wrong value or failed call. The child exits
 * 7 when its copy held the parent's value and took its own write, 11 when
 * it did not inherit the value and 12 when its write did not stick.
 */
PROBE_START void _start(void)
{
    int pid, status, i;

    for (i = 0; i < CYCLES; i++) {
        unsigned base = 0x10000u + (unsigned)i;

        shared_val = base;

        pid = sys_fork();
        if (pid < 0) {
            fail("frames exhausted: the re-grant arm leaks a frame");
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
            fail("waitpid");
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 7) {
            fail("child split path did not run cleanly");
        }
        if (shared_val != base) {
            fail("parent bytes changed: re-grant of a still-shared frame");
        }

        shared_val = base + 0x20000u; /* re-grant arm: sole owner, in place */
        if (shared_val != base + 0x20000u) {
            fail("re-grant write did not stick");
        }
    }

    puts("COWREGRANT OK");
    sys_exit(0);
}
