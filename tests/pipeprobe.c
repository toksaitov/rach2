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
#define SYS_READLINK     85
#define SYS_MUNMAP       91
#define SYS_MMAP2        192
#define PROT_READ        0x1
#define PROT_WRITE       0x2
#define MAP_PRIVATE      0x02
#define MAP_ANONYMOUS    0x20
#define SYS_FSTAT64      197

/* i386 struct stat64: st_ino is the 64-bit field at offset 88. */
#define ST_INO64 88

#define O_NONBLOCK 04000

#define EAGAIN      11
#define EPIPE       32
#define SIGPIPE     13
#define SA_SIGINFO  0x00000004
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

/** Build "/proc/self/fd/<fd>" for a small descriptor. */
static void fd_path_of(char *dst, int fd)
{
    const char *head = "/proc/self/fd/";
    int i = 0, k = 0;
    char digits[8];

    while (head[i]) {
        dst[i] = head[i];
        i++;
    }
    do {
        digits[k++] = (char)('0' + fd % 10);
        fd /= 10;
    } while (fd > 0);
    while (k > 0) {
        dst[i++] = digits[--k];
    }
    dst[i] = '\0';
}

/**
 * Parse a pipe link, which is exactly `pipe:[<digits>]` and nothing else.
 *
 * Stores the inode through `out` and returns 1, or returns 0 for any other
 * text: a loose parse would accept a link that named the wrong thing, which is
 * what this case exists to catch. `n` is the length readlink reported, so no
 * byte past it is read.
 */
static int parse_pipe_link(const char *s, int n, unsigned int *out)
{
    const char *head = "pipe:[";
    unsigned int v = 0;
    int i = 0, digits = 0;

    while (head[i]) {
        if (i >= n || s[i] != head[i]) {
            return 0;
        }
        i++;
    }
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        unsigned int d = (unsigned int)(s[i] - '0');
        if (v > (0xffffffffu - d) / 10u) {
            return 0; /* wider than any number the kernel handed out */
        }
        v = v * 10u + d;
        digits++;
        i++;
    }
    if (digits == 0 || i != n - 1 || s[i] != ']') {
        return 0;
    }
    *out = v;
    return 1;
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

static unsigned mmap2_anon(unsigned len)
{
    unsigned r;

    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_MMAP2), "b"(0), "c"(len), "d"(PROT_READ | PROT_WRITE),
                           "S"(MAP_PRIVATE | MAP_ANONYMOUS), "D"(-1)
                         : "memory");
    return r;
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
    ka.flags = SA_SIGINFO | SA_RESTORER; /* the rt frame its restorer returns from */
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
        check(off == BIG, "parent wrote all 5000 bytes (blocked through full)");
        status = -1;
        check(syscall3(SYS_WAITPID, pid, (int)(size_t)&status, 0) == pid && (status & 0x7f) == 0 &&
                  ((status >> 8) & 0xff) == 0,
              "child read every byte intact");
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

    /* (f) identity: each pipe is one object with two ends, and the link a
     * descriptor resolves to carries its inode. The two ends of one pipe must
     * give the same number, two pipes must give different ones, and fstat must
     * agree with the link. Every pipe here read as `pipe:[1]` before. */
    {
        int a[2], b[2];
        unsigned int ino[4];
        char link[64];
        char fdpath[32];
        char st[128] = {0}; /* fstat64 fills it through an int-cast pointer */
        int k, ends[4];

        check(do_pipe(a) == 0 && do_pipe(b) == 0, "two pipes open");
        ends[0] = a[0];
        ends[1] = a[1];
        ends[2] = b[0];
        ends[3] = b[1];
        for (k = 0; k < 4; k++) {
            int ln;

            ino[k] = 0;
            fd_path_of(fdpath, ends[k]);
            ln = syscall3(SYS_READLINK, (int)(size_t)fdpath, (int)(size_t)link,
                          (int)sizeof(link) - 1);
            check(ln > 0, "a pipe end has a link to read");
            if (ln > 0) {
                link[ln] = '\0';
                check(parse_pipe_link(link, ln, &ino[k]),
                      "and it is a pipe link, spelled exactly as one");
            }
            check(syscall3(SYS_FSTAT64, ends[k], (int)(size_t)st, 0) == 0, "fstat a pipe end");
            /* Both words of the 64-bit inode: a match in the low one alone is
               a different number. */
            check(*(unsigned int *)(st + ST_INO64) == ino[k] &&
                      *(unsigned int *)(st + ST_INO64 + 4) == 0u && ino[k] != 0,
                  "and fstat reports the number the link does");
        }
        check(ino[0] == ino[1] && ino[2] == ino[3], "the two ends of one pipe name one pipe");
        check(ino[0] != ino[2], "and two pipes are not the same pipe");
        for (k = 0; k < 4; k++) {
            syscall1(SYS_CLOSE, ends[k]);
        }
    }

    /* (g) a read whose buffer's second page is unmapped: the first page's
     * bytes arrive and are counted, the rest stay in the pipe for the next
     * read, since the kernel copies a user page at a time and a page that
     * faults is left in the ring. The hole is an anonymous mapping of two
     * pages with the second unmapped. */
    {
        unsigned m = mmap2_anon(8192);
        char *first = (char *)(size_t)m;
        char rest[2048];
        int got;
        int k;

        check(m != 0 && m < 0xfffff000u, "an anonymous mapping of two pages");
        check(syscall2(SYS_MUNMAP, (int)(m + 4096), 4096) == 0, "with its second page unmapped");
        check(do_pipe(p) == 0, "pipe() for the fault test");
        /* A pattern with no period of 2048, so the second half cannot pass
           as the first read again: the byte's 128-byte block enters it. */
        for (i = 0; i < 4096; i++) {
            big[i] = (char)(((i * 7 + 3) ^ (i >> 7)) & 0x7f);
        }
        check(wr(p[1], big, 4096) == 4096, "a page of bytes written");
        got = rd(p[0], first + 2048, 4096);
        check(got == 2048, "a read across the hole returns the bytes before it");
        for (k = 0; k < 2048 && got == 2048; k++) {
            if (first[2048 + k] != big[k]) {
                check(0, "and they are the first bytes");
                break;
            }
        }
        got = rd(p[0], rest, sizeof(rest));
        check(got == 2048, "and the bytes after the hole are still in the pipe");
        for (k = 0; k < 2048 && got == 2048; k++) {
            if (rest[k] != big[2048 + k]) {
                check(0, "in order, every one");
                break;
            }
        }
        syscall1(SYS_CLOSE, p[0]);
        syscall1(SYS_CLOSE, p[1]);
    }

    /* (h) one write of three pages and a byte, which fills the pipe and
     * waits three times while the reader drains: the reader, which sleeps
     * on the empty pipe between the refills, is woken at every refill and not
     * once per write call. The write end stays open until the reader says
     * through a second pipe that every byte arrived, so a reader left
     * asleep is not rescued by the close; a kernel that wakes it only once
     * leaves both ends waiting, and the harness times the row out. */
    {
        int q[2];

        check(do_pipe(p) == 0 && do_pipe(q) == 0, "two pipes for the refill test");
        pid = syscall1(SYS_FORK, 0);
        check(pid >= 0, "fork for the refill test");
        if (pid == 0) {
            int total = 0, got;
            char cb[4096];

            syscall1(SYS_CLOSE, p[1]);
            syscall1(SYS_CLOSE, q[0]);
            while (total < 3 * 4096 + 1) {
                got = rd(p[0], cb, sizeof(cb));
                if (got <= 0) {
                    sys_exit(3);
                }
                total += got;
            }
            wr(q[1], "ok", 2);
            sys_exit(0);
        } else {
            static char threefull[3 * 4096 + 1];
            char ack[2];

            syscall1(SYS_CLOSE, p[0]);
            syscall1(SYS_CLOSE, q[1]);
            check(wr(p[1], threefull, (int)sizeof(threefull)) == (int)sizeof(threefull),
                  "three pages and a byte written in one call through three full waits");
            check(rd(q[0], ack, 2) == 2, "and the reader, woken at every refill, read them all");
            syscall1(SYS_CLOSE, p[1]);
            syscall1(SYS_CLOSE, q[0]);
            status = -1;
            check(syscall3(SYS_WAITPID, pid, (int)(size_t)&status, 0) == pid &&
                      (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0,
                  "and said so");
        }
    }

    /* (h) every length up to 67 bytes at every alignment of the writer's and
     * the reader's buffers, through the pipe and compared byte for byte, with
     * a sentinel on each side of the reader's bytes. The kernel's copies move
     * words with a tail of bytes, so a wrong tail, a wrong start or a byte
     * written past the end shows at the length and alignment that has it. */
    {
        static char pat[80], got[80];
        int len, sa, da, bad = 0;

        for (i = 0; i < 80; i++) {
            pat[i] = (char)(0x21 + (i * 7) % 90);
        }
        check(do_pipe(p) == 0, "pipe() for the alignment sweep");
        for (len = 1; len <= 67 && bad == 0; len++) {
            for (sa = 0; sa < 4 && bad == 0; sa++) {
                for (da = 1; da <= 4 && bad == 0; da++) {
                    for (i = 0; i < 80; i++) {
                        got[i] = (char)0x5a;
                    }
                    if (wr(p[1], pat + sa, len) != len) {
                        bad = 1;
                    } else if (rd(p[0], got + da, len) != len) {
                        bad = 2;
                    } else if (!eqn(got + da, pat + sa, len)) {
                        bad = 3;
                    } else if (got[da + len] != (char)0x5a || got[da - 1] != (char)0x5a) {
                        bad = 4;
                    }
                }
            }
        }
        check(bad == 0, "every length to 67 at every alignment of both buffers arrives intact");
        syscall1(SYS_CLOSE, p[0]);
        syscall1(SYS_CLOSE, p[1]);
    }

    if (fails == 0) {
        puts("PIPEPROBE OK");
    } else {
        puts("PIPEPROBE FAIL");
    }
    sys_exit(fails);
}
