/**
 * Exercises I/O multiplexing (poll and select) over pipes from user space.
 *
 * Each case runs against pipes:
 *   (a) timeout: poll of two empty pipes returns 0 when nothing is ready;
 *   (b) readiness: after a write, poll returns that fd with POLLIN and leaves
 *       the other fd's revents clear;
 *   (c) hangup: polling a read end whose writer has closed reports POLLHUP;
 *   (d) select: the same readable fd is found through an fd_set (proving the
 *       fd_set<->pollfd translation and the _newselect path);
 *   (e) block + wake: a parent that polls with a long timeout is woken the
 *       moment a forked child writes (it does NOT wait out the timeout),
 *       proving poll truly blocks on the pipe wait queue;
 *   (f) fd contract: a wide nfds with only in-range bits works; a bit naming
 *       a closed fd, past the table or not, is -EBADF; nfds past the fd_set
 *       width is -EINVAL (never a silent truncation);
 *   (g) bad timeout: an out-of-range timeval (tv_usec >= 1e6) is -EINVAL;
 *   (h) on x86-64, a ppoll whose timeout is 2^32 seconds waits for its pipe,
 *       which a child writes after 50 ms, and does not end at once, as a
 *       timeout narrowed to 32 bits would.
 *
 * Prints "POLLPROBE OK". Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLHUP 0x010

#define EBADF  9
#define EINVAL 22

struct pollfd {
    int fd;
    short events;
    short revents;
};

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
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

static int fails = 0;

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, 1, (long)"  FAIL: ", 8);
        puts(msg);
    }
}

static int do_pipe(int fds[2])
{
    fds[0] = -1;
    fds[1] = -1;
    return syscall1(SYS_PIPE, (long)fds);
}

static int do_poll(struct pollfd *fds, int nfds, int timeout_ms)
{
    return syscall3(SYS_POLL, (long)fds, nfds, timeout_ms);
}

/**
 * Drains `n` bytes from `fd` into `buf`.
 *
 * Passing buf as char * keeps the static checker from mistaking the read target
 * (written only through an int-cast syscall arg) for an unused const.
 */
static int do_read(int fd, char *buf, int n)
{
    return syscall3(SYS_READ, fd, (long)buf, n);
}

static void msleep(int ms)
{
    struct {
        long tv_sec;
        long tv_nsec;
    } ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    syscall2(SYS_NANOSLEEP, (long)&ts, 0);
}

#if defined __x86_64__
/* The state letter of process `pid`, from the field after the name in
   /proc/<pid>/stat, or 0 when it cannot be read. */
static char state_of(int pid)
{
    static char path[32], text[256];
    char digits[12];
    int n = 0, i = 0, fd, len;

    do {
        digits[n++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0 && n < 11);
    path[i++] = '/';
    path[i++] = 'p';
    path[i++] = 'r';
    path[i++] = 'o';
    path[i++] = 'c';
    path[i++] = '/';
    while (n > 0) {
        path[i++] = digits[--n];
    }
    path[i++] = '/';
    path[i++] = 's';
    path[i++] = 't';
    path[i++] = 'a';
    path[i++] = 't';
    path[i] = '\0';
    fd = (int)syscall3(SYS_OPEN, (long)path, 0, 0);
    if (fd < 0) {
        return 0;
    }
    len = (int)syscall3(SYS_READ, fd, (long)text, (int)sizeof(text));
    syscall1(SYS_CLOSE, fd);
    for (i = 0; i + 2 < len; i++) {
        if (text[i] == ')' && text[i + 1] == ' ') {
            return text[i + 2];
        }
    }
    return 0;
}
#endif

/**
 * Case (f), the select fd contract, which is never a silent truncation: a
 * wide nfds with only an in-range bit works, a bit for a closed descriptor
 * is -EBADF whether past the table or not, and an nfds past FD_SETSIZE is
 * -EINVAL. `closed` is a descriptor in the table's range that was closed.
 */
static void select_contract(int closed)
{
    unsigned int set[32];
    int i, n;

    /* A large nfds is allowed when no bit names an out-of-range fd: fd 1
     * (stdout) is writable, so a wide select that includes it returns it. */
    for (i = 0; i < 32; i++) {
        set[i] = 0;
    }
    set[1u / 32] = 1u << (1u % 32);
    n = syscall5(SYS_SELECT, 64, 0, (long)set, 0, 0);
    check(n == 1 && (set[0] & (1u << 1)) != 0, "wide select with an in-range bit works");

    /* A bit for an fd past the table's sixteen, which nothing has opened,
     * is a closed descriptor -> -EBADF. */
    for (i = 0; i < 32; i++) {
        set[i] = 0;
    }
    set[17u / 32] = 1u << (17u % 32);
    check(syscall5(SYS_SELECT, 18, (long)set, 0, 0, 0) == -EBADF,
          "select bit past the table -> -EBADF");

    /* A bit for a closed in-range fd is also -EBADF, not ignored. */
    for (i = 0; i < 32; i++) {
        set[i] = 0;
    }
    set[(unsigned)closed / 32] = 1u << ((unsigned)closed % 32);
    check(syscall5(SYS_SELECT, closed + 1, (long)set, 0, 0, 0) == -EBADF,
          "select bit for a closed fd -> -EBADF");

    /* nfds beyond the fd_set width (FD_SETSIZE) is -EINVAL. */
    check(syscall5(SYS_SELECT, 2000, 0, 0, 0, 0) == -EINVAL,
          "select nfds > FD_SETSIZE -> -EINVAL");
}

#if defined __x86_64__
/**
 * Case (h), a ppoll timeout past 32 bits of seconds. The child writes only
 * once the parent is asleep, in the call: a timeout cut to zero would return
 * at once and never sleep, and the child writes after two seconds anyway, so
 * that such a parent is not left waiting for it.
 */
static void wide_ppoll(void)
{
    long ts[2] = {4294967296L, 0};
    struct pollfd pfd[1];
    int c[2], parent, pid, status, tries;

    check(do_pipe(c) == 0, "pipe for the ppoll row");
    pid = syscall0(SYS_FORK);
    if (pid == 0) {
        parent = (int)syscall0(SYS_GETPPID);
        for (tries = 0; tries < 200 && state_of(parent) != 'S'; tries++) {
            msleep(10);
        }
        syscall3(SYS_WRITE, c[1], (long)"x", 1);
        sys_exit(0);
    }
    pfd[0].fd = c[0];
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    check(syscall5(SYS_PPOLL, (long)pfd, 1, (long)ts, 0, 8) == 1 && (pfd[0].revents & POLLIN),
          "ppoll with a timeout of 2^32 seconds waits for its pipe");
    syscall3(SYS_WAITPID, pid, (long)&status, 0);
    syscall1(SYS_CLOSE, c[0]);
    syscall1(SYS_CLOSE, c[1]);
}
#endif

PROBE_START void _start(void)
{
    int a[2], b[2];
    struct pollfd pfd[2];
    char buf[8];
    int n, pid, status;

    /* (a) timeout: two empty pipes, nothing ready -> 0. */
    check(do_pipe(a) == 0 && do_pipe(b) == 0, "pipe x2");
    pfd[0].fd = a[0];
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    pfd[1].fd = b[0];
    pfd[1].events = POLLIN;
    pfd[1].revents = 0;
    check(do_poll(pfd, 2, 30) == 0, "poll of empty pipes times out -> 0");

    /* (b) readiness: write one end, poll returns it with POLLIN, the other
     * clear. */
    syscall3(SYS_WRITE, a[1], (long)"x", 1);
    pfd[0].revents = 0;
    pfd[1].revents = 0;
    n = do_poll(pfd, 2, 100);
    check(n == 1, "poll reports exactly one ready fd");
    check((pfd[0].revents & POLLIN) != 0, "the written pipe is POLLIN");
    check(pfd[1].revents == 0, "the idle pipe stays clear");
    do_read(a[0], buf, 1); /* drain */

    /* (c) hangup: close b's write end, poll b[0] -> POLLHUP. */
    syscall1(SYS_CLOSE, b[1]);
    pfd[0].fd = b[0];
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    n = do_poll(pfd, 1, 100);
    check(n == 1 && (pfd[0].revents & POLLHUP) != 0, "closed writer -> POLLHUP");
    syscall1(SYS_CLOSE, b[0]);

    /* (d) select via _newselect: a[0] is drained/empty, write then select
     * it. */
    syscall3(SYS_WRITE, a[1], (long)"y", 1);
    {
        unsigned int rset[32];
        int i;
        for (i = 0; i < 32; i++) {
            rset[i] = 0;
        }
        rset[a[0] / 32] = 1u << (a[0] % 32);
        /* select(nfds, &rset, NULL, NULL, NULL): NULL timeout, but a[0] is
         * already readable so it returns at once. */
        n = syscall5(SYS_SELECT, a[0] + 1, (long)rset, 0, 0, 0);
        check(n == 1, "select finds the readable fd");
        check((rset[a[0] / 32] & (1u << (a[0] % 32))) != 0, "select set the ready bit");
    }
    do_read(a[0], buf, 1); /* drain */

    /* (e) block + wake: child writes after a delay; the parent's poll must
     * return woken (well under the 2s timeout), proving it blocked on the wait
     * queue. */
    pid = syscall1(SYS_FORK, 0);
    check(pid >= 0, "fork");
    if (pid == 0) {
        syscall1(SYS_CLOSE, a[0]);
        msleep(60);
        syscall3(SYS_WRITE, a[1], (long)"z", 1);
        sys_exit(0);
    } else {
        pfd[0].fd = a[0];
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        n = do_poll(pfd, 1, 2000);
        check(n == 1 && (pfd[0].revents & POLLIN) != 0,
              "poll blocks then wakes on the child's write");
        do_read(a[0], buf, 1);
        syscall3(SYS_WAITPID, pid, (long)&status, 0);
    }
    syscall1(SYS_CLOSE, a[0]);
    syscall1(SYS_CLOSE, a[1]);

    select_contract(a[0]);

    /* (g) an out-of-range timeout is rejected, not silently clamped. */
    {
        long tv[2];
        tv[0] = 0;
        tv[1] = 2000000; /* tv_usec >= 1e6 is invalid */
        check(syscall5(SYS_SELECT, 0, 0, 0, 0, (long)tv) == -EINVAL,
              "select with tv_usec >= 1e6 -> -EINVAL");
    }

#if defined __x86_64__
    wide_ppoll();
#endif

    if (fails == 0) {
        puts("POLLPROBE OK");
    } else {
        puts("POLLPROBE FAIL");
    }
    sys_exit(fails);
}
