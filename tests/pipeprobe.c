/**
 * Exercises the anonymous-pipe contract from user space.
 *
 * Each case checks one guarantee:
 *   (a) round-trip + EOF: bytes written at one end read back in order at the
 *       other, and once the write end closes the drained read end returns 0;
 *   (b) O_NONBLOCK: a read of an empty pipe and a write to a full one return
 *       -EAGAIN instead of blocking, and the FIFO holds 64 KiB, Linux's
 *       capacity, which F_GETPIPE_SZ reports at both ends;
 *   (c) SIGPIPE / -EPIPE: a write with the read end closed raises SIGPIPE and
 *       (with the signal caught) returns -EPIPE;
 *   (d) blocking across a fork: a writer of more than one buffer blocks until
 *       the forked reader drains it, and every byte arrives intact;
 *   (e) a hundred pipes made, written a byte and closed in turn each
 *       succeed, and leave the kernel heap's HeapUsed in /proc/meminfo as
 *       it was before them. The heap is shared with etude's guest agent and
 *       the kernel's own work, whose allocations may be live at either
 *       reading, so the hundred cycles are tried up to five times and the
 *       check holds when one try leaves the heap exactly as it was; a leak
 *       of a pipe or of its ring on close changes the heap at every try;
 *   (f) identity: the link /proc/self/fd gives for a pipe end is exactly
 *       `pipe:[N]`, the two ends of one pipe give the same N and two pipes
 *       different ones, and fstat reports N as the inode, the high word of
 *       its 64-bit field zero;
 *   (g) a read into a buffer whose second page is unmapped returns the bytes
 *       of the first page and leaves the rest in the pipe for the next read,
 *       since the kernel copies to user memory a page at a time and a page
 *       that faults leaves its bytes in the ring;
 *   (h) one write of three pipefuls and a byte fills the pipe and waits three
 *       times, and the reader, asleep on the empty pipe between refills, is
 *       woken at every refill and not once per write call. The write end
 *       stays open until the reader says through a second pipe that every
 *       byte arrived, so a reader left asleep is not rescued by the close; a
 *       kernel that wakes it only once leaves both ends waiting, and the
 *       harness times the row out;
 *   (i) every length up to 67 bytes at every alignment of the writer's and
 *       the reader's buffers, over a long, arrives byte for byte with a
 *       sentinel on each side of the reader's bytes untouched. The kernel's
 *       copies move words with a tail of bytes, so a wrong tail, a wrong
 *       start or a byte written past the end shows at the length and
 *       alignment that has it.
 *
 * Given `machine=native` (probe_args.h), the probe leaves out the heap's
 * comparison of (e), since the kernel's heap is shared with every process of
 * a machine that is already up; the pipes are still made and closed.
 *
 * Prints "PIPEPROBE OK" and exits with the count of the checks that failed.
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, an assertion for each check `checks` lists; a check that a failed
 * one guards is not run, as is the heap's under `machine=native`.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define F_GETPIPE_SZ  1032

#define O_NONBLOCK 04000

#define CYCLES      100 /* pipes made and closed in one try of case (e) */
#define CYCLE_TRIES 5   /* tries of case (e) before its heap check fails */

#define EAGAIN      11
#define EPIPE       32
#define SIGPIPE     13
#define SA_SIGINFO  0x00000004
#define SA_RESTORER 0x04000000

#define PIPE_BUF_SIZE 65536

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
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
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

/* The checks, one assertion each, in the order the probe makes them, and the
 * text it prints for each that fails. */
enum {
    PIPE_A,
    WRITE_10,
    READ_BACK,
    EOF_AFTER_CLOSE,
    PIPE2_NONBLOCK,
    GETPIPE_SZ,
    READ_EAGAIN,
    FILL_64K,
    WRITE_EAGAIN,
    DRAIN,
    SIGPIPE_PIPE,
    EPIPE_WRITE,
    SIGPIPE_RAISED,
    BLOCKING_PIPE,
    BLOCKING_FORK,
    WROTE_WHOLE,
    CHILD_INTACT,
    HEAPUSED,
    CYCLE_PIPES,
    HEAP_KEPT,
    TWO_PIPES,
    LINK_READ,
    LINK_EXACT,
    FSTAT_END,
    FSTAT_INO,
    SAME_PIPE,
    DISTINCT_PIPES,
    TWO_PAGE_MAP,
    UNMAP_SECOND,
    FAULT_PIPE,
    FAULT_WRITE,
    READ_BEFORE_HOLE,
    FIRST_BYTES,
    REST_IN_PIPE,
    REST_IN_ORDER,
    REFILL_PIPES,
    REFILL_FORK,
    REFILL_WRITE,
    REFILL_READ,
    REFILL_CHILD,
    SWEEP_PIPE,
    SWEEP,
    CHECKS
};

static const struct {
    const char *id;
    const char *text;
} checks[CHECKS] = {
    {"pipe", "pipe()"},
    {"write-10", "write 10 bytes"},
    {"read-back", "read back the 10 bytes in order"},
    {"eof-after-close", "read returns 0 (EOF) after write end closed"},
    {"pipe2-nonblock", "pipe2(O_NONBLOCK)"},
    {"getpipe-sz", "F_GETPIPE_SZ reports 64 KiB at both ends"},
    {"read-eagain", "nonblocking read of empty pipe -> -EAGAIN"},
    {"fill-64k", "a write of 64 KiB fills the pipe"},
    {"write-eagain", "nonblocking write of a full pipe -> -EAGAIN"},
    {"drain", "drain 8 bytes from the full pipe"},
    {"sigpipe-pipe", "pipe() for SIGPIPE test"},
    {"epipe-write", "write to reader-closed pipe -> -EPIPE"},
    {"sigpipe-raised", "SIGPIPE was raised"},
    {"blocking-pipe", "pipe() for blocking test"},
    {"blocking-fork", "fork"},
    {"wrote-whole", "parent wrote the whole buffer (blocked through full)"},
    {"child-intact", "child read every byte intact"},
    {"heapused", "HeapUsed in /proc/meminfo"},
    {"cycle-pipes", "pipe() and a one-byte write in a pipe/close cycle"},
    {"heap-kept", "100 pipe/close cycles left the heap as it was (no pipe leak)"},
    {"two-pipes", "two pipes open"},
    {"link-read", "a pipe end has a link to read"},
    {"link-exact", "and it is a pipe link, spelled exactly as one"},
    {"fstat-end", "fstat a pipe end"},
    {"fstat-ino", "and fstat reports the number the link does"},
    {"same-pipe", "the two ends of one pipe name one pipe"},
    {"distinct-pipes", "and two pipes are not the same pipe"},
    {"two-page-map", "an anonymous mapping of two pages"},
    {"unmap-second", "with its second page unmapped"},
    {"fault-pipe", "pipe() for the fault test"},
    {"fault-write", "a page of bytes written"},
    {"read-before-hole", "a read across the hole returns the bytes before it"},
    {"first-bytes", "and they are the first bytes"},
    {"rest-in-pipe", "and the bytes after the hole are still in the pipe"},
    {"rest-in-order", "in order, every one"},
    {"refill-pipes", "two pipes for the refill test"},
    {"refill-fork", "fork for the refill test"},
    {"refill-write", "three pipefuls and a byte written in one call through three full waits"},
    {"refill-read", "and the reader, woken at every refill, read them all"},
    {"refill-child", "and said so"},
    {"sweep-pipe", "pipe() for the alignment sweep"},
    {"sweep", "every length to 67 at every alignment of both buffers arrives intact"},
};

/* Each check's outcome: 0 not made, 1 held, 2 failed at least once. */
static unsigned char outcome[CHECKS];

/** Record the outcome of the check `id`, and count a failure and print its
 * text when `cond` is 0. */
static void check(int id, int cond)
{
    if (outcome[id] != 2) {
        outcome[id] = cond ? 1 : 2;
    }
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, 1, (long)"  FAIL: ", 8);
        puts(checks[id].text);
    }
}

/** Check `cond` as `check` does, unless given `machine=native`. */
static void check_heap(int id, int cond)
{
    if (!probe_native()) {
        check(id, cond);
    }
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

/**
 * Read up to `n` bytes from `fd` into `buf`: the byte count, or a negated
 * errno. The char* lets the analyzer see buf written.
 */
static int rd(int fd, char *buf, int n)
{
    return syscall3(SYS_READ, fd, (long)buf, n);
}

/** Write `n` bytes of `buf` to `fd`: the count written, or a negated errno. */
static int wr(int fd, const char *buf, int n)
{
    return syscall3(SYS_WRITE, fd, (long)buf, n);
}

/** An anonymous read-write mapping of `len` bytes: its address, or a negated
 * errno. */
static long mmap_anon(long len)
{
    return probe_mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/**
 * Calls pipe() into the two-element fds array: 0, or a negated errno. fds is
 * pre-cleared so the analyzer sees the output written even though the kernel
 * fills it through the long-cast address.
 */
static int do_pipe(int fds[2])
{
    fds[0] = -1;
    fds[1] = -1;
    return syscall1(SYS_PIPE, (long)fds);
}

/** pipe2() into `fds` with `flags`, as `do_pipe` makes pipe(). */
static int do_pipe2(int fds[2], int flags)
{
    fds[0] = -1;
    fds[1] = -1;
    return syscall2(SYS_PIPE2, (long)fds, flags);
}

/* Signal restorer trampoline (musl-style): invokes rt_sigreturn. */
extern void sig_restorer(void);
PROBE_RT_RESTORER(sig_restorer);

/* The sigaction rt_sigaction takes, with a 64-bit mask. */
struct k_sigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long long mask;
};

static volatile int got_sigpipe;

/** The SIGPIPE handler: record that it ran. */
static void sigpipe_handler(int sig)
{
    (void)sig;
    got_sigpipe = 1;
}

/* A pattern buffer of more than a pipe holds, for the fill and blocking
 * tests. */
#define BIG 70000
static char big[BIG];

/** Wait for the child `pid` and return whether it exited with status 0. */
static int exited_zero(int pid)
{
    int status = -1;

    return syscall3(SYS_WAITPID, pid, (long)&status, 0) == pid && (status & 0x7f) == 0 &&
           ((status >> 8) & 0xff) == 0;
}

/** Case (a): a round trip, and EOF once the write end closes. */
static void case_round_trip(void)
{
    int p[2], n;
    char buf[64];

    check(PIPE_A, do_pipe(p) == 0);
    check(WRITE_10, wr(p[1], "hello pipe", 10) == 10);
    n = rd(p[0], buf, 10);
    check(READ_BACK, n == 10 && eqn(buf, "hello pipe", 10));
    syscall1(SYS_CLOSE, p[1]);
    check(EOF_AFTER_CLOSE, rd(p[0], buf, 10) == 0);
    syscall1(SYS_CLOSE, p[0]);
}

/** Case (b): O_NONBLOCK at both ends, and the capacity of 64 KiB. */
static void case_nonblock(void)
{
    int p[2];
    char buf[64];

    check(PIPE2_NONBLOCK, do_pipe2(p, O_NONBLOCK) == 0);
    check(GETPIPE_SZ, syscall3(SYS_FCNTL, p[0], F_GETPIPE_SZ, 0) == PIPE_BUF_SIZE &&
                          syscall3(SYS_FCNTL, p[1], F_GETPIPE_SZ, 0) == PIPE_BUF_SIZE);
    check(READ_EAGAIN, rd(p[0], buf, 8) == -EAGAIN);
    check(FILL_64K, wr(p[1], big, PIPE_BUF_SIZE) == PIPE_BUF_SIZE);
    check(WRITE_EAGAIN, wr(p[1], "x", 1) == -EAGAIN);
    check(DRAIN, rd(p[0], buf, 8) == 8);
    syscall1(SYS_CLOSE, p[0]);
    syscall1(SYS_CLOSE, p[1]);
}

/** Case (c): SIGPIPE and EPIPE for a write with the read end closed. */
static void case_sigpipe(void)
{
    struct k_sigaction ka;
    int p[2];

    ka.handler = sigpipe_handler;
    ka.flags = SA_SIGINFO | SA_RESTORER; /* an rt frame, for rt_sigreturn */
    ka.restorer = sig_restorer;
    ka.mask = 0;
    syscall4(SYS_RT_SIGACTION, SIGPIPE, (long)&ka, 0, 8);
    got_sigpipe = 0;
    check(SIGPIPE_PIPE, do_pipe(p) == 0);
    syscall1(SYS_CLOSE, p[0]);
    check(EPIPE_WRITE, wr(p[1], "data", 4) == -EPIPE);
    check(SIGPIPE_RAISED, got_sigpipe == 1);
    syscall1(SYS_CLOSE, p[1]);
}

/** Case (d)'s reader, in the child: read the pipe `fd` to its end and exit 0
 * only when every byte of `big` arrived in order. */
static void blocking_reader(int fd)
{
    int total = 0, got;
    char cb[256];

    while ((got = rd(fd, cb, sizeof(cb))) > 0) {
        int j;
        for (j = 0; j < got; j++) {
            if (cb[j] != (char)((total + j) & 0x7f)) {
                sys_exit(1); /* corrupted stream */
            }
        }
        total += got;
    }
    sys_exit(total == BIG ? 0 : 2);
}

/** Case (d): a writer of more than one buffer blocks until the forked reader
 * drains it. */
static void case_blocking(void)
{
    int p[2], pid, off = 0;

    check(BLOCKING_PIPE, do_pipe(p) == 0);
    pid = syscall1(SYS_FORK, 0);
    check(BLOCKING_FORK, pid >= 0);
    if (pid == 0) {
        syscall1(SYS_CLOSE, p[1]);
        blocking_reader(p[0]);
    }
    syscall1(SYS_CLOSE, p[0]);
    while (off < BIG) {
        int w = wr(p[1], big + off, BIG - off);
        if (w <= 0) {
            break;
        }
        off += w;
    }
    syscall1(SYS_CLOSE, p[1]); /* signal EOF to the reader */
    check(WROTE_WHOLE, off == BIG);
    check(CHILD_INTACT, exited_zero(pid));
}

/** Sleep for `ms` milliseconds (nanosleep(2)). */
static void nap(long ms)
{
    long ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000L;
    /* interval: a try that changed the heap is followed by a pause */
    (void)syscall2(SYS_NANOSLEEP, (long)ts, 0);
}

/**
 * One try of case (e): CYCLES pipes made in turn, each written one byte, so
 * that it has a ring, and closed. HeapUsed is read into `before` and `after`
 * around them. Returns 1 when every pipe was made and written.
 */
static int cycle_once(unsigned *before, unsigned *after)
{
    int i;

    *before = probe_meminfo("HeapUsed:");
    for (i = 0; i < CYCLES; i++) {
        int q[2];
        int w;

        if (do_pipe(q) != 0) {
            break;
        }
        w = wr(q[1], "x", 1);
        syscall1(SYS_CLOSE, q[0]);
        syscall1(SYS_CLOSE, q[1]);
        if (w != 1) {
            break;
        }
    }
    *after = probe_meminfo("HeapUsed:");
    return i == CYCLES;
}

/**
 * Case (e): a hundred pipes made, written and closed leave HeapUsed as it
 * was, in one of CYCLE_TRIES tries. A try whose readings differ is followed,
 * after a pause of 10 ms for work in flight to finish, by the next.
 */
static void case_cycles(void)
{
    unsigned before = 0xFFFFFFFFu;
    unsigned after = 0;
    int made = 1;
    int tries;

    for (tries = 0; tries < CYCLE_TRIES && made && before != after; tries++) {
        if (tries > 0) {
            nap(10);
        }
        made = cycle_once(&before, &after);
    }
    check(HEAPUSED, before != 0xFFFFFFFFu);
    check(CYCLE_PIPES, made);
    check_heap(HEAP_KEPT, before != 0xFFFFFFFFu && before == after);
}

/** Case (f) for one pipe end `fd`: its link is exactly `pipe:[N]` and fstat
 * reports N as its inode. Returns N, or 0 when the link could not be read. */
static unsigned int end_identity(int fd)
{
    unsigned int ino = 0;
    char link[64];
    char fdpath[32];
    static char st[KSTAT_SIZE]; /* fstat fills it through a cast */
    int ln;

    fd_path_of(fdpath, fd);
    ln = syscall3(SYS_READLINK, (long)fdpath, (long)link, (int)sizeof(link) - 1);
    check(LINK_READ, ln > 0);
    if (ln > 0) {
        link[ln] = '\0';
        check(LINK_EXACT, parse_pipe_link(link, ln, &ino));
    }
    check(FSTAT_END, syscall3(SYS_FSTAT, fd, (long)st, 0) == 0);
    check(FSTAT_INO, *(unsigned int *)(st + KSTAT_INO) == ino &&
                         *(unsigned int *)(st + KSTAT_INO + 4) == 0u && ino != 0);
    return ino;
}

/** Case (f): the identity of a pipe in /proc/self/fd and in fstat. */
static void case_identity(void)
{
    int a[2], b[2];
    unsigned int ino[4];
    int k, ends[4];

    check(TWO_PIPES, do_pipe(a) == 0 && do_pipe(b) == 0);
    ends[0] = a[0];
    ends[1] = a[1];
    ends[2] = b[0];
    ends[3] = b[1];
    for (k = 0; k < 4; k++) {
        ino[k] = end_identity(ends[k]);
    }
    check(SAME_PIPE, ino[0] == ino[1] && ino[2] == ino[3]);
    check(DISTINCT_PIPES, ino[0] != ino[2]);
    for (k = 0; k < 4; k++) {
        syscall1(SYS_CLOSE, ends[k]);
    }
}

/** Whether the `n` bytes at `a` equal those at `b`, for case (g)'s reads. */
static int same_bytes(const char *a, const char *b, int n)
{
    int k;

    for (k = 0; k < n; k++) {
        if (a[k] != b[k]) {
            return 0;
        }
    }
    return 1;
}

/** Case (g): a read into a buffer that runs into an unmapped page returns the
 * bytes before it and leaves the rest in the pipe. */
static void case_fault(void)
{
    long m = mmap_anon(8192);
    char *first = (char *)m;
    char rest[2048];
    int p[2], got, i;

    check(TWO_PAGE_MAP, m != 0 && !sys_failed(m));
    check(UNMAP_SECOND, syscall2(SYS_MUNMAP, (long)(first + 4096), 4096) == 0);
    check(FAULT_PIPE, do_pipe(p) == 0);
    /* No period of 2048, so the second half cannot pass for the first. */
    for (i = 0; i < 4096; i++) {
        big[i] = (char)(((i * 7 + 3) ^ (i >> 7)) & 0x7f);
    }
    check(FAULT_WRITE, wr(p[1], big, 4096) == 4096);
    got = rd(p[0], first + 2048, 4096);
    check(READ_BEFORE_HOLE, got == 2048);
    if (got == 2048) {
        check(FIRST_BYTES, same_bytes(first + 2048, big, 2048));
    }
    got = rd(p[0], rest, sizeof(rest));
    check(REST_IN_PIPE, got == 2048);
    if (got == 2048) {
        check(REST_IN_ORDER, same_bytes(rest, big + 2048, 2048));
    }
    syscall1(SYS_CLOSE, p[0]);
    syscall1(SYS_CLOSE, p[1]);
}

/** Case (h)'s reader, in the child: read three pipefuls and a byte from `fd`,
 * say so on `ack`, and exit 0, or exit 3 at an early end. */
static void refill_reader(int fd, int ack)
{
    int total = 0, got;
    char cb[4096];

    while (total < 3 * PIPE_BUF_SIZE + 1) {
        got = rd(fd, cb, sizeof(cb));
        if (got <= 0) {
            sys_exit(3);
        }
        total += got;
    }
    wr(ack, "ok", 2);
    sys_exit(0);
}

/** Case (h): one write of three pipefuls and a byte, and a reader woken at
 * every refill. */
static void case_refill(void)
{
    static char threefull[3 * PIPE_BUF_SIZE + 1];
    char ack[2];
    int p[2], q[2], pid;

    check(REFILL_PIPES, do_pipe(p) == 0 && do_pipe(q) == 0);
    pid = syscall1(SYS_FORK, 0);
    check(REFILL_FORK, pid >= 0);
    if (pid == 0) {
        syscall1(SYS_CLOSE, p[1]);
        syscall1(SYS_CLOSE, q[0]);
        refill_reader(p[0], q[1]);
    }
    syscall1(SYS_CLOSE, p[0]);
    syscall1(SYS_CLOSE, q[1]);
    check(REFILL_WRITE, wr(p[1], threefull, (int)sizeof(threefull)) == (int)sizeof(threefull));
    check(REFILL_READ, rd(q[0], ack, 2) == 2);
    syscall1(SYS_CLOSE, p[1]);
    syscall1(SYS_CLOSE, q[0]);
    check(REFILL_CHILD, exited_zero(pid));
}

/** One step of case (i): write `len` bytes of `pat` at `sa` and read them at
 * `da` of `got`. Returns 0, or the stage that failed, as case (i) counts. */
static int sweep_one(const int p[2], const char *pat, char *got, int len, int sa, int da)
{
    int i;

    for (i = 0; i < 80; i++) {
        got[i] = (char)0x5a;
    }
    if (wr(p[1], pat + sa, len) != len) {
        return 1;
    }
    if (rd(p[0], got + da, len) != len) {
        return 2;
    }
    if (!eqn(got + da, pat + sa, len)) {
        return 3;
    }
    return got[da + len] != (char)0x5a || got[da - 1] != (char)0x5a ? 4 : 0;
}

/** Case (i): every length up to 67 at every alignment of both buffers. */
static void case_sweep(void)
{
    static char pat[80], got[80];
    int p[2], i, len, sa, da, bad = 0;

    for (i = 0; i < 80; i++) {
        pat[i] = (char)(0x21 + (i * 7) % 90);
    }
    check(SWEEP_PIPE, do_pipe(p) == 0);
    for (len = 1; len <= 67 && bad == 0; len++) {
        for (sa = 0; sa < (int)sizeof(long) && bad == 0; sa++) {
            for (da = 1; da <= (int)sizeof(long) && bad == 0; da++) {
                bad = sweep_one(p, pat, got, len, sa, da);
            }
        }
    }
    check(SWEEP, bad == 0);
    syscall1(SYS_CLOSE, p[0]);
    syscall1(SYS_CLOSE, p[1]);
}

/** Report each check: held, failed with its text, or not run, since a check
 * it depends on failed or, for the heap's, under `machine=native`; and end
 * the report. */
static void report_checks(void)
{
    int i;

    for (i = 0; i < CHECKS; i++) {
        if (outcome[i] == 1) {
            report_pass(checks[i].id);
        } else if (outcome[i] == 2) {
            report_fail(checks[i].id, checks[i].text);
        } else if (i == HEAP_KEPT && probe_native()) {
            report_notrun(checks[i].id, "machine=native shares the kernel heap");
        } else {
            report_notrun(checks[i].id, "a check it depends on failed");
        }
    }
    report_end();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run cases (a) to (i) in order; a failed check is printed and counted,
 * and the next case runs. */
void probe_main(unsigned long *sp)
{
    int i;

    probe_args((int)sp[0], (char **)(sp + 1));
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.pipeprobe"));
    for (i = 0; i < BIG; i++) {
        big[i] = (char)(i & 0x7f);
    }
    case_round_trip();
    case_nonblock();
    case_sigpipe();
    case_blocking();
    case_cycles();
    case_identity();
    case_fault();
    case_refill();
    case_sweep();
    report_checks();
    if (fails == 0) {
        puts("PIPEPROBE OK");
    } else {
        puts("PIPEPROBE FAIL");
    }
    sys_exit(fails);
}
