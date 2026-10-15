/**
 * cowprobe: copy-on-write fork, tested from user space.
 *
 * Three properties are checked:
 *   1. Share: a value written before fork is visible to the child (the page
 *      is shared, not zeroed).
 *   2. Split: the child writing the page gets a private copy; the parent's
 *      copy is unchanged, and the parent can then write its own copy.
 *      Isolation both ways means the shared frame was split on write.
 *   3. No leak: 64 fork/exit cycles do not exhaust frames: each shared frame
 *      is reference-counted and freed when the last owner exits.
 *
 * Prints "COWPROBE OK" and exits 0 on success.
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

/* A global lives in the writable data segment, so after fork it is a COW
 * page. */
static volatile unsigned shared_val;

/** The write call: the count written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** The fork call: the child's pid, 0 in the child, or a negated errno. */
static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

/** Wait for the child `pid`: the pid reaped, or a negated errno. */
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

/** Print "COWPROBE FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    sys_write(STDOUT_FILENO, "COWPROBE FAIL: ", 15);
    puts(why);
    sys_exit(1);
}

/**
 * The probe: the three cases in order. The child of cases 1 and 2 exits 42
 * when it saw the parent's value and its own write took, 10 when it did not
 * see the value and 11 when its write did not take.
 */
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
        if (shared_val != 0xAAAAu) {
            sys_exit(10);
        }
        shared_val = 0xBBBBu; /* COW: this write makes a private copy */
        if (shared_val != 0xBBBBu) {
            sys_exit(11);
        }
        sys_exit(42);
    }

    if (sys_waitpid(pid, &status, 0) != pid) {
        fail("waitpid");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) {
        fail("child did not observe share-then-split");
    }

    /* The child's write leaves the parent's copy unchanged. */
    if (shared_val != 0xAAAAu) {
        fail("parent copy changed (isolation broken)");
    }

    /* The parent has the frame's last reference: the write keeps the page. */
    shared_val = 0xCCCCu;
    if (shared_val != 0xCCCCu) {
        fail("parent write did not stick");
    }

    /* 3: many fork/exit cycles leave frames for the next fork. */
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
