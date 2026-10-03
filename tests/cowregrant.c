/**
 * Stresses the copy-on-write sole-owner re-grant arm for leaks and corruption.
 *
 * When a write faults on a COW page the kernel takes one of two paths: if this
 * process is the frame's last owner (refcount 1) the page is made writable in
 * place with no copy (the re-grant arm); otherwise a private copy is allocated
 * and the shared frame's reference dropped (the shared-copy arm). This probe
 * drives the re-grant arm every iteration: the child writes first (its write
 * takes the shared-copy arm and drops the parent to sole owner), the parent
 * reaps it and then writes the same page (that write must take the re-grant
 * arm). Over many cycles it catches the bugs that arm can have: a leak or
 * double-free of the re-granted frame shows up as a fork that runs out of
 * frames, and a re-grant of a still-shared frame shows up as the parent's bytes
 * changing under the child. It cannot tell a correct in-place re-grant from a
 * correct but wasteful always-copy, since the two are identical from user
 * space; pinning that distinction needs a host test or a kernel copy counter.
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

static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
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
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

static void fail(const char *why)
{
    sys_write(STDOUT_FILENO, "COWREGRANT FAIL: ", 17);
    puts(why);
    sys_exit(1);
}

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
