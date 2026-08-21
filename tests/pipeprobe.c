/**
 * Exercises the anonymous-pipe contract from user space.
 *
 * Each case checks one guarantee:
 *   (a) round-trip + EOF: bytes written at one end read back in order at the
 *       other, and once the write end closes the drained read end returns 0;
 *   (b) O_NONBLOCK: a read of an empty pipe and a write to a full one return
 *       -EAGAIN instead of blocking, and the FIFO has a full page;
 *   (c) SIGPIPE / -EPIPE: a write with the read end closed raises SIGPIPE and
 *       (with the signal caught) returns -EPIPE;
 *   (d) blocking across a fork: a writer of more than one buffer blocks until
 *       the forked reader drains it, and every byte arrives intact.
 *
 * Prints "PIPEPROBE OK". Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT         1
#define SYS_FORK         2
#define SYS_READ         3
#define SYS_WRITE        4
#define SYS_CLOSE        6
#define SYS_WAITPID      7
#define SYS_PIPE         42
#define SYS_PIPE2        331
#define SYS_RT_SIGACTION 174

#define O_NONBLOCK 04000

#define EAGAIN      11
#define EPIPE       32
#define SIGPIPE     13
#define SA_RESTORER 0x04000000

#define PIPE_BUF_SIZE 4096

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

static int syscall4(int n, int a, int b, int c, int d)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
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

/**
 * Reads n bytes from fd into buf and returns the byte count. The char* lets
 * the analyzer see buf written.
 */
static int rd(int fd, char *buf, int n)
{
    return syscall3(SYS_READ, fd, (int)(size_t)buf, n);
}

static int wr(int fd, const char *buf, int n)
{
    return syscall3(SYS_WRITE, fd, (int)(size_t)buf, n);
}

/**
 * Calls pipe() into the two-element fds array. fds is pre-cleared so the
 * analyzer sees the output written even though the kernel fills it through the
 * cast-to-int address.
 */
static int do_pipe(int fds[2])
{
    fds[0] = -1;
    fds[1] = -1;
    return syscall1(SYS_PIPE, (int)(size_t)fds);
}

static int do_pipe2(int fds[2], int flags)
{
    fds[0] = -1;
    fds[1] = -1;
    return syscall2(SYS_PIPE2, (int)(size_t)fds, flags);
}

/* Signal restorer trampoline (musl-style): invokes rt_sigreturn (173). */
extern void sig_restorer(void);
__asm__(".globl sig_restorer\n"
        "sig_restorer:\n"
        "    movl $173, %eax\n"
        "    int $0x80\n");

struct k_sigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long long mask;
};

static volatile int got_sigpipe;

static void sigpipe_handler(int sig)
{
    (void)sig;
    got_sigpipe = 1;
}

/* A 5 KB pattern buffer (> one pipe page) for the fill and blocking tests. */
#define BIG 5000
static char big[BIG];

void _start(void)
{
    int p[2];
    char buf[64];
    struct k_sigaction ka;
    int i, n, pid, status;

    for (i = 0; i < BIG; i++) {
        big[i] = (char)(i & 0x7f);
    }

    /* (a) round-trip + EOF, in one process (the page-sized buffer has it). */
    check(do_pipe(p) == 0, "pipe()");
    check(wr(p[1], "hello pipe", 10) == 10, "write 10 bytes");
    n = rd(p[0], buf, 10);
    check(n == 10 && eqn(buf, "hello pipe", 10), "read back the 10 bytes in order");
    syscall1(SYS_CLOSE, p[1]); /* close the write end */
    check(rd(p[0], buf, 10) == 0, "read returns 0 (EOF) after write end closed");
    syscall1(SYS_CLOSE, p[0]);

    /* (b) O_NONBLOCK: empty read -> EAGAIN; the FIFO has a full page; a write
     * past full -> EAGAIN. */
    check(do_pipe2(p, O_NONBLOCK) == 0, "pipe2(O_NONBLOCK)");
    check(rd(p[0], buf, 8) == -EAGAIN, "nonblocking read of empty pipe -> -EAGAIN");
    check(wr(p[1], big, PIPE_BUF_SIZE) == PIPE_BUF_SIZE, "write fills a full page");
    check(wr(p[1], "x", 1) == -EAGAIN, "nonblocking write of a full pipe -> -EAGAIN");
    check(rd(p[0], buf, 8) == 8, "drain 8 bytes from the full pipe");
    syscall1(SYS_CLOSE, p[0]);
    syscall1(SYS_CLOSE, p[1]);

    /* (c) SIGPIPE + -EPIPE: write with the read end closed. */
    ka.handler = sigpipe_handler;
    ka.flags = SA_RESTORER;
    ka.restorer = sig_restorer;
    ka.mask = 0;
    syscall4(SYS_RT_SIGACTION, SIGPIPE, (int)(size_t)&ka, 0, 8);
    got_sigpipe = 0;
    check(do_pipe(p) == 0, "pipe() for SIGPIPE test");
    syscall1(SYS_CLOSE, p[0]); /* close the read end */
    check(wr(p[1], "data", 4) == -EPIPE, "write to reader-closed pipe -> -EPIPE");
    check(got_sigpipe == 1, "SIGPIPE was raised");
    syscall1(SYS_CLOSE, p[1]);

    /* (d) blocking across a fork: the parent writes 5000 bytes (> one page) so
     * it must block until the forked reader drains, and the reader gets every
     * byte. */
    check(do_pipe(p) == 0, "pipe() for blocking test");
    pid = syscall1(SYS_FORK, 0);
    check(pid >= 0, "fork");
    if (pid == 0) {
        int total = 0, got;
        char cb[256];
        syscall1(SYS_CLOSE, p[1]); /* child reads only */
        while ((got = rd(p[0], cb, sizeof(cb))) > 0) {
            int j;
            for (j = 0; j < got; j++) {
                if (cb[j] != (char)((total + j) & 0x7f)) {
                    sys_exit(1); /* corrupted stream */
                }
            }
            total += got;
        }
        sys_exit(total == BIG ? 0 : 2); /* 0 only if every byte arrived */
    } else {
        int off = 0;
        syscall1(SYS_CLOSE, p[0]); /* parent writes only */
        while (off < BIG) {
            int w = wr(p[1], big + off, BIG - off);
            if (w <= 0) {
                break;
            }
            off += w;
        }
        syscall1(SYS_CLOSE, p[1]); /* signal EOF to the reader */
        syscall3(SYS_WAITPID, pid, (int)(size_t)&status, 0);
        check(off == BIG, "parent wrote all 5000 bytes (blocked through full)");
        check(((status >> 8) & 0xff) == 0, "child read every byte intact");
    }

    /* (e) no pool leak: many create/close cycles past the pool size must keep
     * succeeding (each closed pipe returns to the pool). */
    for (i = 0; i < 100; i++) {
        int q[2];
        if (do_pipe(q) != 0) {
            check(0, "pipe() exhausted the pool (leak)");
            break;
        }
        syscall1(SYS_CLOSE, q[0]);
        syscall1(SYS_CLOSE, q[1]);
    }

    if (fails == 0) {
        puts("PIPEPROBE OK");
    } else {
        puts("PIPEPROBE FAIL");
    }
    sys_exit(fails);
}
