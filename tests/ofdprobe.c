/**
 * Probe for refcounted open-file descriptions, from user space.
 *
 * Proves that a duplicated or forked descriptor shares one open-file
 * description (and its seek offset), the Unix way, not each carrying
 * its own:
 *
 *   (a) dup2 shares the offset: a read through the dup continues where the
 *       original left off, not from 0;
 *   (b) fork shares the offset: a child advances the offset and the parent's
 *       next read sees the advance;
 *   (c) reference counts: closing one fd leaves the dup working, and after
 *       many open and close cycles and many forks an open still succeeds;
 *   (d) FD_CLOEXEC is per-fd: it round-trips through fcntl, and dup2 leaves
 *       the new fd's flag clear.
 *
 * Prints "OFDPROBE OK" on success.
 *
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define O_RDONLY 0

#define F_GETFD    1
#define F_SETFD    2
#define FD_CLOEXEC 1

/* "hello, world\n": 0-4 hello, 5 ',', 6 ' ', 7 w... */
#define HELLO "/hello.txt"

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

/** Count a failure and print `msg` when `cond` is 0. */
static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, STDOUT_FILENO, (long)"  FAIL: ", 8);
        puts(msg);
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

/** Run cases (a) to (d) on /hello.txt, and exit with the count of the
 * checks that failed. */
PROBE_START void _start(void)
{
    char buf[16];
    int fd, fd2, pid, status, i, n;

    /* (a) dup2 shares the seek offset. */
    fd = syscall3(SYS_OPEN, (long)HELLO, O_RDONLY, 0);
    check(fd >= 0, "open hello.txt");
    check(rd(fd, buf, 5) == 5 && eqn(buf, "hello", 5), "read first 5 == hello");

    fd2 = syscall2(SYS_DUP2, fd, 9); /* dup onto fd 9 */
    check(fd2 == 9, "dup2 onto fd 9");
    n = rd(fd2, buf, 5); /* must continue from offset 5 */
    check(n == 5 && eqn(buf, ", wor", 5), "dup shares offset (reads ', wor')");

    /* (c) the dup works after the original is closed: the OFD is still held. */
    syscall1(SYS_CLOSE, fd);
    n = rd(fd2, buf, 3); /* offset is now 10: "ld\n" */
    check(n == 3 && eqn(buf, "ld\n", 3), "dup works after original closed");
    syscall1(SYS_CLOSE, fd2);

    /* (b) fork shares the seek offset. */
    fd = syscall3(SYS_OPEN, (long)HELLO, O_RDONLY, 0);
    check(fd >= 0, "reopen hello.txt");
    check(rd(fd, buf, 3) == 3 && eqn(buf, "hel", 3), "parent reads first 3 == hel");

    pid = syscall1(SYS_FORK, 0);
    check(pid >= 0, "fork");
    if (pid == 0) {
        char cb[8];
        rd(fd, cb, 3); /* child advances offset 3 -> 6 */
        sys_exit(0);
    }
    syscall3(SYS_WAITPID, pid, (long)&status, 0);
    n = rd(fd, buf, 3); /* must see the child's advance: " wo" */
    check(n == 3 && eqn(buf, " wo", 3), "fork shares offset (parent reads ' wo')");
    syscall1(SYS_CLOSE, fd);

    /* (c) open and close cycles leak no description. */
    for (i = 0; i < 300; i++) {
        int f = syscall3(SYS_OPEN, (long)HELLO, O_RDONLY, 0);
        if (f < 0) {
            check(0, "open/close cycle exhausted the OFD pool (leak)");
            break;
        }
        syscall1(SYS_CLOSE, f);
    }

    /* (c2) a child's exit releases the descriptions it inherited. */
    for (i = 0; i < 100; i++) {
        int p = syscall1(SYS_FORK, 0);
        if (p == 0) {
            sys_exit(0);
        }
        syscall3(SYS_WAITPID, p, (long)&status, 0);
    }
    fd = syscall3(SYS_OPEN, (long)HELLO, O_RDONLY, 0);
    check(fd >= 0, "open after 100 forks (no fork OFD leak)");
    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
    }

    /* (d) FD_CLOEXEC is per-fd: it round-trips, and dup2 leaves it clear. */
    fd = syscall3(SYS_OPEN, (long)HELLO, O_RDONLY, 0);
    check(fd >= 0, "open for cloexec test");
    syscall3(SYS_FCNTL, fd, F_SETFD, FD_CLOEXEC);
    check(syscall3(SYS_FCNTL, fd, F_GETFD, 0) == FD_CLOEXEC, "F_SETFD/F_GETFD CLOEXEC");
    fd2 = syscall2(SYS_DUP2, fd, 10);
    check(fd2 == 10, "dup2 onto fd 10");
    check(syscall3(SYS_FCNTL, fd2, F_GETFD, 0) == 0, "dup2 clears CLOEXEC on new fd");
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_CLOSE, fd2);

    if (fails == 0) {
        puts("OFDPROBE OK");
    } else {
        puts("OFDPROBE FAIL");
    }
    sys_exit(fails);
}
