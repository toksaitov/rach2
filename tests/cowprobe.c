/**
 * Exercises copy-on-write fork semantics from user space.
 *
 * Three properties are checked:
 *   1. SHARE: a value written before fork is visible to the child (the page is
 *      shared, not re-zeroed).
 *   2. SPLIT: the child writing the page gets a private copy; the parent's copy
 *      is unchanged, and the parent can then write its own copy. Isolation both
 *      ways means the shared frame was split on write.
 *   3. NO LEAK: 64 fork/exit cycles do not exhaust frames: each COW share is
 *      reference-counted and freed when the last owner exits.
 *
 * Prints "COWPROBE OK" on success. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

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
    sys_write(STDOUT_FILENO, "COWPROBE FAIL: ", 15);
    puts(why);
    sys_exit(1);
}

PROBE_START void _start(void)
{
    int pid, status, i;

    /* 1 + 2: share then split. */
    shared_val = 0xAAAAu;

    pid = sys_fork();
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        /* Child: must see the shared value, then split it with a write. */
        if (shared_val != 0xAAAAu) {
            sys_exit(10); /* did not inherit the shared value */
        }
        shared_val = 0xBBBBu; /* COW: this write makes a private copy */
        if (shared_val != 0xBBBBu) {
            sys_exit(11); /* the private write did not stick */
        }
        sys_exit(42); /* success marker */
    }

    if (sys_waitpid(pid, &status, 0) != pid) {
        fail("waitpid");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) {
        fail("child did not observe share-then-split");
    }

    /* The child's write must NOT have touched the parent's copy. */
    if (shared_val != 0xAAAAu) {
        fail("parent copy changed (isolation broken)");
    }

    /* The parent can now write its own copy (its COW page splits in place). */
    shared_val = 0xCCCCu;
    if (shared_val != 0xCCCCu) {
        fail("parent write did not stick");
    }

    /* 3: many fork/exit cycles must not exhaust frames. */
    for (i = 0; i < 64; i++) {
        int p = sys_fork();
        if (p < 0) {
            fail("frames exhausted across repeated forks");
        }
        if (p == 0) {
            sys_exit(7);
        }
        if (sys_waitpid(p, &status, 0) != p || !WIFEXITED(status) || WEXITSTATUS(status) != 7) {
            fail("repeated fork/reap");
        }
    }

    puts("COWPROBE OK");
    sys_exit(0);
}
