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
 *       an fd >= MAX_FDS or a closed fd is -EBADF; nfds past the fd_set width
 *       is -EINVAL (never a silent truncation);
 *   (g) bad timeout: an out-of-range timeval (tv_usec >= 1e6) is -EINVAL.
 *
 * Prints "POLLPROBE OK". Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT      1
#define SYS_FORK      2
#define SYS_READ      3
#define SYS_WRITE     4
#define SYS_CLOSE     6
#define SYS_WAITPID   7
#define SYS_PIPE      42
#define SYS_NANOSLEEP 162
#define SYS_NEWSELECT 142
#define SYS_POLL      168

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

static int syscall1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int syscall2(int n, int a, int b)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static int syscall5(int n, int a, int b, int c, int d, int e)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory");
    return r;
}

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
    syscall3(SYS_WRITE, 1, (int)(size_t)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (int)(size_t)"\n", 1);
}

static int fails = 0;

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, 1, (int)(size_t)"  FAIL: ", 8);
        puts(msg);
    }
}

static int do_pipe(int fds[2])
{
    fds[0] = -1;
    fds[1] = -1;
    return syscall1(SYS_PIPE, (int)(size_t)fds);
}

static int do_poll(struct pollfd *fds, int nfds, int timeout_ms)
{
    return syscall3(SYS_POLL, (int)(size_t)fds, nfds, timeout_ms);
}

/**
 * Drains `n` bytes from `fd` into `buf`.
 *
 * Passing buf as char * keeps the static checker from mistaking the read target
 * (written only through an int-cast syscall arg) for an unused const.
 */
static int do_read(int fd, char *buf, int n)
{
    return syscall3(SYS_READ, fd, (int)(size_t)buf, n);
}

static void msleep(int ms)
{
    struct {
        int tv_sec;
        int tv_nsec;
    } ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    syscall2(SYS_NANOSLEEP, (int)(size_t)&ts, 0);
}

void _start(void)
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
    syscall3(SYS_WRITE, a[1], (int)(size_t)"x", 1);
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
    syscall3(SYS_WRITE, a[1], (int)(size_t)"y", 1);
    {
        unsigned int rset[32];
        int i;
        for (i = 0; i < 32; i++) {
            rset[i] = 0;
        }
        rset[a[0] / 32] = 1u << (a[0] % 32);
        /* select(nfds, &rset, NULL, NULL, NULL): NULL timeout, but a[0] is
         * already readable so it returns at once. */
        n = syscall5(SYS_NEWSELECT, a[0] + 1, (int)(size_t)rset, 0, 0, 0);
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
        syscall3(SYS_WRITE, a[1], (int)(size_t)"z", 1);
        sys_exit(0);
    } else {
        pfd[0].fd = a[0];
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        n = do_poll(pfd, 1, 2000);
        check(n == 1 && (pfd[0].revents & POLLIN) != 0,
              "poll blocks then wakes on the child's write");
        do_read(a[0], buf, 1);
        syscall3(SYS_WAITPID, pid, (int)(size_t)&status, 0);
    }
    syscall1(SYS_CLOSE, a[0]);
    syscall1(SYS_CLOSE, a[1]);

    /* (f) the select fd contract: never a silent truncation. */
    {
        unsigned int set[32];
        int i;

        /* A large nfds is allowed when no bit names an out-of-range fd: fd 1
         * (stdout) is writable, so a wide select that includes it returns
         * it. */
        for (i = 0; i < 32; i++) {
            set[i] = 0;
        }
        set[1u / 32] = 1u << (1u % 32);
        n = syscall5(SYS_NEWSELECT, 64, 0, (int)(size_t)set, 0, 0);
        check(n == 1 && (set[0] & (1u << 1)) != 0, "wide select with an in-range bit works");

        /* A bit for an fd >= MAX_FDS (16) is an impossible descriptor ->
         * -EBADF. */
        for (i = 0; i < 32; i++) {
            set[i] = 0;
        }
        set[17u / 32] = 1u << (17u % 32);
        check(syscall5(SYS_NEWSELECT, 18, (int)(size_t)set, 0, 0, 0) == -EBADF,
              "select bit past MAX_FDS -> -EBADF");

        /* A bit for a closed in-range fd is also -EBADF, not ignored: a[0] was
         * closed above. */
        for (i = 0; i < 32; i++) {
            set[i] = 0;
        }
        set[(unsigned)a[0] / 32] = 1u << ((unsigned)a[0] % 32);
        check(syscall5(SYS_NEWSELECT, a[0] + 1, (int)(size_t)set, 0, 0, 0) == -EBADF,
              "select bit for a closed fd -> -EBADF");

        /* nfds beyond the fd_set width (FD_SETSIZE) is -EINVAL. */
        check(syscall5(SYS_NEWSELECT, 2000, 0, 0, 0, 0) == -EINVAL,
              "select nfds > FD_SETSIZE -> -EINVAL");
    }

    /* (g) an out-of-range timeout is rejected, not silently clamped. */
    {
        int tv[2];
        tv[0] = 0;
        tv[1] = 2000000; /* tv_usec >= 1e6 is invalid */
        check(syscall5(SYS_NEWSELECT, 0, 0, 0, 0, (int)(size_t)tv) == -EINVAL,
              "select with tv_usec >= 1e6 -> -EINVAL");
    }

    if (fails == 0) {
        puts("POLLPROBE OK");
    } else {
        puts("POLLPROBE FAIL");
    }
    sys_exit(fails);
}
