/**
 * Exercises the anonymous-pipe contract from user space.
 *
 * Each case checks one guarantee:
 *   (a) round-trip + EOF: bytes written at one end read back in order at the
 *       other, and once the write end closes the drained read end returns 0;
 *   (b) O_NONBLOCK: a read of an empty pipe and a write to a full one return
 *       -EAGAIN instead of blocking, and the FIFO holds 64 KiB, Linux's
 *       capacity, which F_GETPIPE_SZ reports at both ends; F_SETPIPE_SZ
 *       refuses a size below what the pipe holds with EBUSY, rounds 300000
 *       up to 512 KiB, which F_GETPIPE_SZ then reports and a write fills
 *       further, and keeps the bytes held, in order;
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
 *       alignment that has it;
 *   (j) bytes held across the end of the ring survive F_SETPIPE_SZ: a pipe
 *       of 64 KiB holds 40000 bytes that wrap, grows to 128 KiB, shrinks to
 *       64 KiB, which wraps them again, gives up 10000 and shrinks to
 *       32 KiB, and then tee copies the 30000 left, wrapped in the source,
 *       into a pipe whose tail wraps, and both pipes read back the stream
 *       in order;
 *   (k) the limits of a caller without privilege, as on Linux: a write to
 *       /proc/sys/fs/pipe-max-size is rounded up to a power of two of a page
 *       or more and past 2^31 refused, a write of +64 to
 *       pipe-user-pages-soft or -hard is refused, as Linux's proc_get_long
 *       refuses a plus sign; a child that set its user to 65534
 *       may not grow a pipe past pipe-max-size, may grow one to 1 MiB, and,
 *       with pipe-user-pages-soft set to 64, grows its own pipes to 64
 *       pages whatever root's hold, is refused a growth past them, gets a
 *       new pipe of 8192 bytes, and may still shrink one;
 *   (l) a pipe holding bytes in more page buffers than a smaller size has
 *       refuses the shrink with EBUSY, however few bytes they are, as Linux
 *       counts buffers: a write of 4096, a read of 4095 and a write of 1
 *       leave two, since the byte does not fit in the first page after the
 *       4096 written to it; two writes of a byte share one, and the shrink
 *       to 4096 succeeds; a byte tee copied is a buffer no write joins; a
 *       byte a splice moved whole from another pipe's write is one a write
 *       joins, and the shrink succeeds; two bytes vmsplice took from the
 *       two sides of a page boundary are two buffers; a splice of 32769
 *       bytes from offset 1 of a memfd, longer than the kernel's copy
 *       chunk, makes one buffer of each page of the file, so after a read
 *       of 28671 the 4098 bytes left are two buffers, and a shrink to 8192
 *       succeeds where one to 4096 is refused; and a vmsplice of 128 KiB of
 *       page-aligned memory into an empty pipe that may wait takes the
 *       64 KiB that fit and returns, as Linux waits only before the first
 *       byte. A pipe of 64 KiB is full at sixteen buffers, as Linux's
 *       pipe_full counts them: sixteen vmsplices of a byte fill it and the
 *       seventeenth is EAGAIN, and a vmsplice of 128 KiB from byte 1 of a
 *       page takes 65535 bytes in sixteen buffers. A vmsplice from a
 *       mapping made with PROT_WRITE alone is EFAULT for a page never
 *       written, which Linux's get_user_pages refuses for want of
 *       VM_READ, and takes a byte of a page written first; a private
 *       mapping of a memfd made so is EFAULT for its untouched page too,
 *       though the file's page is in memory. A write stops at the same
 *       sixteen buffers, as Linux's pipe_write stops at pipe_full: after
 *       sixteen one-byte vmsplices a nonblocking write of a byte is EAGAIN,
 *       and after a write of a byte and fifteen of 4096 a sixteenth of 4096
 *       is EAGAIN. vmsplice without SPLICE_F_NONBLOCK waits on an empty
 *       pipe whose read end has O_NONBLOCK, until a child that saw the probe
 *       asleep writes a byte.
 *
 * Given `machine=native` (probe_args.h), the probe leaves out the heap's
 * comparison of (e), since the kernel's heap is shared with every process of
 * a machine that is already up; the pipes are still made and closed. It
 * leaves out (k)'s writes to /proc/sys and the rows that need them as well,
 * since the limits they set would bind every other user's pipes meanwhile.
 * Run without privilege, it leaves out all of (k) but its child's first
 * rows, which it runs as the user it has.
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
#define F_SETPIPE_SZ  1031
#define F_GETPIPE_SZ  1032

#define O_NONBLOCK 04000

#define CYCLES      100 /* pipes made and closed in one try of case (e) */
#define CYCLE_TRIES 5   /* tries of case (e) before its heap check fails */

#define EPERM             1
#define EAGAIN            11
#define EFAULT            14
#define EBUSY             16
#define EINVAL            22
#define EPIPE             32
#define O_WRONLY          1
#define SPLICE_F_NONBLOCK 2
#define NOBODY            65534 /* the user (k)'s child takes */
#define SIGPIPE           13
#define SA_SIGINFO        0x00000004
#define SA_RESTORER       0x04000000

#define PIPE_BUF_SIZE 65536

#if defined __x86_64__
#define SYS_MEMFD_CREATE 319
#else
#define SYS_MEMFD_CREATE 356
#endif

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
    SETPIPE_BUSY,
    SETPIPE_GROW,
    SETPIPE_KEPT,
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
    WRAP_PIPES,
    WRAP_HELD,
    WRAP_GROW,
    WRAP_SHRINK,
    WRAP_TEE,
    WRAP_ORDER,
    PAGES_PIPES,
    PAGES_BUSY,
    PAGES_MERGE,
    PAGES_TEE,
    PAGES_SPLICE,
    PAGES_VMSPLICE,
    PAGES_FILE,
    PAGES_VMSPLICE_WAIT,
    PAGES_SLOTS,
    PAGES_SLOTS_UNALIGNED,
    PAGES_WRONLY,
    PAGES_WRONLY_FILE,
    PAGES_WRITE_SLOTS,
    PAGES_WRITE_PAGES,
    VMSPLICE_WAIT_FORK,
    VMSPLICE_WAIT_NONBLOCK,
    MAX_ROUND,
    MAX_EINVAL,
    SYSCTL_PLUS,
    LIMIT_FORK,
    LIMIT_PAST_MAX,
    LIMIT_TO_MAX,
    SOFT_ROOT,
    SOFT_OWN,
    SOFT_PAST,
    SOFT_NEW_SMALL,
    SOFT_SHRINK,
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
    {"setpipe-busy", "F_SETPIPE_SZ below the bytes held -> -EBUSY"},
    {"setpipe-grow", "F_SETPIPE_SZ of 300000 gives 512 KiB, a 64 KiB write fits"},
    {"setpipe-kept", "the bytes held before the growth read back in order"},
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
    {"wrap-pipes", "two nonblocking pipes for the wrap test"},
    {"wrap-held", "40000 bytes held across the end of a 64 KiB ring"},
    {"wrap-grow", "F_SETPIPE_SZ of 128 KiB with the bytes held wrapped"},
    {"wrap-shrink", "F_SETPIPE_SZ back to 64 KiB, a read of 10000, and to 32 KiB"},
    {"wrap-tee", "tee of the 30000 bytes wrapped into a pipe whose tail wraps, read in order"},
    {"wrap-order", "the 30000 bytes read back in order from the resized pipe"},
    {"pages-pipes", "two nonblocking pipes for the page-buffer test"},
    {"pages-busy",
     "a write of 4096, a read of 4095 and a write of 1 hold two pages: a shrink to 4096 -> -EBUSY"},
    {"pages-merge", "two writes of 1 share a page: a shrink to 4096 succeeds"},
    {"pages-tee", "a byte tee copied takes no later write: a shrink to 4096 -> -EBUSY"},
    {"pages-splice", "a byte a splice moved whole takes a later write: a shrink to 4096 succeeds"},
    {"pages-vmsplice",
     "two bytes vmspliced across a page boundary hold two pages: a shrink to 4096 -> -EBUSY"},
    {"pages-file", "a splice of 32769 bytes from a memfd's offset 1 and a read of 28671 leave two "
                   "pages: a shrink to 8192 succeeds and to 4096 -> -EBUSY"},
    {"pages-vmsplice-wait", "a vmsplice of 128 KiB into an empty pipe that may wait returns 65536"},
    {"pages-slots", "16 vmsplices of a byte fill a pipe of 64 KiB: the 17th -> -EAGAIN"},
    {"pages-slots-unaligned", "a vmsplice of 128 KiB from byte 1 of a page returns 65535"},
    {"pages-wronly",
     "a vmsplice of an untouched PROT_WRITE page -> -EFAULT, and of a written one returns 1"},
    {"pages-wronly-file",
     "a vmsplice of an untouched PROT_WRITE private page of a memfd -> -EFAULT"},
    {"pages-write-slots", "after 16 vmsplices of a byte a nonblocking write of a byte -> -EAGAIN"},
    {"pages-write-pages", "after writes of 1 and 15 x 4096 a nonblocking write of 4096 -> -EAGAIN"},
    {"vmsplice-wait-fork", "fork for the vmsplice wait"},
    {"vmsplice-wait-nonblock",
     "vmsplice without SPLICE_F_NONBLOCK from an empty O_NONBLOCK pipe waits for the child's byte"},
    {"max-round", "pipe-max-size takes 600000 as 1048576, 1 as 4096 and 2^31 as written"},
    {"max-einval", "pipe-max-size refuses 2^31 + 1 with EINVAL"},
    {"sysctl-plus", "pipe-user-pages-soft and -hard refuse +64 with EINVAL and keep their values"},
    {"limit-fork", "fork for the limits' child"},
    {"limit-past-max", "an unprivileged F_SETPIPE_SZ of 2 MiB -> -EPERM"},
    {"limit-to-max", "an unprivileged F_SETPIPE_SZ of 600000 gives 1 MiB"},
    {"soft-root", "root's pipe grows to 512 KiB with pipe-user-pages-soft at 64"},
    {"soft-own", "the unprivileged child grows two pipes to 64 pages in all"},
    {"soft-past", "and its growth past 64 pages -> -EPERM"},
    {"soft-new-small", "and its new pipe past them has 8192 bytes"},
    {"soft-shrink", "and it may still shrink a pipe"},
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
 * tests, and one a pipeful is read back into. */
#define BIG 70000
static char big[BIG];
static char back[PIPE_BUF_SIZE];

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
    check(SETPIPE_BUSY, syscall3(SYS_FCNTL, p[1], F_SETPIPE_SZ, 4096) == -EBUSY);
    check(SETPIPE_GROW, syscall3(SYS_FCNTL, p[1], F_SETPIPE_SZ, 300000) == 524288 &&
                            syscall3(SYS_FCNTL, p[0], F_GETPIPE_SZ, 0) == 524288 &&
                            wr(p[1], big, PIPE_BUF_SIZE) == PIPE_BUF_SIZE);
    check(SETPIPE_KEPT, rd(p[0], back, PIPE_BUF_SIZE - 8) == PIPE_BUF_SIZE - 8 &&
                            eqn(back, big + 8, PIPE_BUF_SIZE - 8));
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

/* The byte at offset `i` of the stream case (j) writes. */
static char wrap_byte(unsigned int i)
{
    return (char)(i * 13u + (i >> 9));
}

/* One write's or one read's bytes of case (j). */
static char wrap_buf[PIPE_BUF_SIZE];

/** Write the `n` bytes of case (j)'s stream from offset `at` to `fd`:
 * whether all were taken. */
static int wrap_put(int fd, unsigned int at, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        wrap_buf[i] = wrap_byte(at + (unsigned int)i);
    }
    return wr(fd, wrap_buf, n) == n;
}

/** Read `n` bytes from `fd`: whether they are case (j)'s stream from offset
 * `at`, every one. */
static int wrap_get(int fd, unsigned int at, int n)
{
    int i;

    if (rd(fd, wrap_buf, n) != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (wrap_buf[i] != wrap_byte(at + (unsigned int)i)) {
            return 0;
        }
    }
    return 1;
}

/** F_SETPIPE_SZ of `size` on `fd`: the size given, or a negated errno. */
static int set_size(int fd, long size)
{
    return (int)syscall3(SYS_FCNTL, fd, F_SETPIPE_SZ, size);
}

/** Close the ends of `fds` that are open, those not -1. */
static void close_pair(const int fds[2])
{
    if (fds[0] >= 0) {
        syscall1(SYS_CLOSE, fds[0]);
    }
    if (fds[1] >= 0) {
        syscall1(SYS_CLOSE, fds[1]);
    }
}

/**
 * Case (j): bytes that wrap the ring survive a growth and two shrinks, each
 * of which copies them across the end of one ring or both, and tee copies
 * them from a wrapped source to a wrapped tail.
 */
static void case_wrap(void)
{
    int p[2] = {-1, -1}, q[2] = {-1, -1};
    int made = do_pipe2(p, O_NONBLOCK) == 0 && do_pipe2(q, O_NONBLOCK) == 0;

    check(WRAP_PIPES, made);
    if (!made) {
        close_pair(p);
        close_pair(q);
        return;
    }
    check(WRAP_HELD,
          wrap_put(p[1], 0, 60000) && wrap_get(p[0], 0, 50000) && wrap_put(p[1], 60000, 30000));
    check(WRAP_GROW, set_size(p[1], 131072) == 131072);
    check(WRAP_SHRINK, set_size(p[1], 65536) == 65536 && wrap_get(p[0], 50000, 10000) &&
                           set_size(p[1], 32768) == 32768);
    check(WRAP_TEE, wr(q[1], big, 50000) == 50000 && rd(q[0], back, 50000) == 50000 &&
                        syscall4(SYS_TEE, p[0], q[1], 30000, SPLICE_F_NONBLOCK) == 30000 &&
                        wrap_get(q[0], 60000, 30000));
    check(WRAP_ORDER, wrap_get(p[0], 60000, 30000));
    close_pair(p);
    close_pair(q);
}

/** vmsplice(2) of the `n` bytes at `addr` into `fd` with `flags`: the
 * bytes taken, or a negated errno. */
static long vmsplice_one(int fd, long addr, long n, int flags)
{
    long iov[2];

    iov[0] = addr;
    iov[1] = n;
    return syscall4(SYS_VMSPLICE, fd, (long)iov, 1, flags);
}

/**
 * The rows of case (l) that splice into the pipes `p` and `q`, which the
 * earlier rows left: `q` empty and `p` holding the byte tee copied. A byte
 * a splice moved whole from a write takes a later write, and two bytes
 * vmsplice read from two pages are two buffers.
 */
static void pages_splices(const int p[2], const int q[2])
{
    const char two[2] = {'a', 'b'};
    long map = mmap_anon(8192);
    int mapped = map != 0 && !sys_failed(map);

    check(PAGES_SPLICE, rd(p[0], back, 1) == 1 && wr(p[1], two, 1) == 1 &&
                            syscall6(SYS_SPLICE, p[0], 0, q[1], 0, 1, SPLICE_F_NONBLOCK) == 1 &&
                            wr(q[1], two + 1, 1) == 1 && set_size(q[1], 4096) == 4096 &&
                            rd(q[0], back, 2) == 2 && back[0] == 'a' && back[1] == 'b');
    if (mapped) {
        ((char *)map)[4095] = 'a';
        ((char *)map)[4096] = 'b';
    }
    check(PAGES_VMSPLICE, mapped && set_size(p[1], 65536) == 65536 &&
                              vmsplice_one(p[1], map + 4095, 2, SPLICE_F_NONBLOCK) == 2 &&
                              set_size(p[1], 4096) == -EBUSY && rd(p[0], back, 2) == 2 &&
                              back[0] == 'a' && back[1] == 'b');
    if (mapped) {
        syscall2(SYS_MUNMAP, map, 8192);
    }
}

/**
 * The rows of case (l) that splice a file's pages into `q`, empty and 4096
 * bytes after the earlier rows, and wait in vmsplice. The kernel copies a
 * file to a pipe 32 KiB at a time, so the splice's page at 32768 spans two
 * of its chunks; Linux links each of the nine pages once, and so the 4098
 * bytes left after the read are the last two pages' buffers. The vmsplice
 * may wait for room before its first byte only, so it returns the 64 KiB
 * that fit; a kernel that waits for the rest leaves the probe asleep, and
 * the harness times the row out.
 */
static void pages_file(const int q[2])
{
    long long off = 1;
    long fd = syscall2(SYS_MEMFD_CREATE, (long)"pipeprobe", 0);
    long map = mmap_anon(2 * PIPE_BUF_SIZE);
    int w[2] = {-1, -1};

    check(PAGES_FILE, !sys_failed(fd) && wr((int)fd, big, 40000) == 40000 &&
                          set_size(q[1], PIPE_BUF_SIZE) == PIPE_BUF_SIZE &&
                          syscall6(SYS_SPLICE, fd, (long)&off, q[1], 0, 32769, 0) == 32769 &&
                          rd(q[0], back, 28671) == 28671 && set_size(q[1], 8192) == 8192 &&
                          set_size(q[1], 4096) == -EBUSY && rd(q[0], back, 8192) == 4098 &&
                          back[0] == big[28672] && back[4097] == big[32769]);
    if (!sys_failed(fd)) {
        syscall1(SYS_CLOSE, fd);
    }
    check(PAGES_VMSPLICE_WAIT, map != 0 && !sys_failed(map) && do_pipe(w) == 0 &&
                                   vmsplice_one(w[1], map, 2 * PIPE_BUF_SIZE, 0) == PIPE_BUF_SIZE &&
                                   rd(w[0], back, PIPE_BUF_SIZE) == PIPE_BUF_SIZE);
    close_pair(w);
    if (map != 0 && !sys_failed(map)) {
        syscall2(SYS_MUNMAP, map, 2 * PIPE_BUF_SIZE);
    }
}

/**
 * The rows of case (l) that fill a pipe by its buffers, sixteen in a pipe of
 * 64 KiB however few bytes they hold, and vmsplice from memory mapped with
 * PROT_WRITE alone, whose page Linux's get_user_pages takes only once a
 * write has brought it in.
 */
static void pages_slots(void)
{
    long map = mmap_anon(2 * PIPE_BUF_SIZE + 4096);
    long wo = probe_mmap(0, 8192, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int mapped = map != 0 && !sys_failed(map), wmapped = wo != 0 && !sys_failed(wo);
    int w[2] = {-1, -1};
    int made = do_pipe(w) == 0, i, took = 0;

    for (i = 0; made && mapped && i < 16; i++) {
        took += vmsplice_one(w[1], map, 1, SPLICE_F_NONBLOCK) == 1;
    }
    check(PAGES_SLOTS, made && mapped && took == 16 &&
                           vmsplice_one(w[1], map, 1, SPLICE_F_NONBLOCK) == -EAGAIN &&
                           rd(w[0], back, 64) == 16);
    check(PAGES_SLOTS_UNALIGNED, made && mapped &&
                                     vmsplice_one(w[1], map + 1, 2 * PIPE_BUF_SIZE,
                                                  SPLICE_F_NONBLOCK) == PIPE_BUF_SIZE - 1 &&
                                     rd(w[0], back, PIPE_BUF_SIZE) == PIPE_BUF_SIZE - 1);
    if (wmapped) {
        ((char *)wo)[4096] = 'w';
    }
    check(PAGES_WRONLY, made && wmapped &&
                            vmsplice_one(w[1], wo, 1, SPLICE_F_NONBLOCK) == -EFAULT &&
                            vmsplice_one(w[1], wo + 4096, 1, SPLICE_F_NONBLOCK) == 1 &&
                            rd(w[0], back, 2) == 1 && back[0] == 'w');
    close_pair(w);
    if (mapped) {
        syscall2(SYS_MUNMAP, map, 2 * PIPE_BUF_SIZE + 4096);
    }
    if (wmapped) {
        syscall2(SYS_MUNMAP, wo, 8192);
    }
}

/**
 * The row of case (l) that vmsplices a byte of a private mapping of a memfd
 * made with PROT_WRITE alone and never touched: Linux's get_user_pages finds
 * no page in the mapping, though the file's page is in memory, and its slow
 * path refuses the region for want of VM_READ.
 */
static void pages_wronly_file(void)
{
    long fd = syscall2(SYS_MEMFD_CREATE, (long)"pipeprobe-wo", 0);
    long wo = -1;
    int w[2] = {-1, -1};
    int made = do_pipe(w) == 0, filled = !sys_failed(fd) && wr((int)fd, big, 4096) == 4096;

    if (filled) {
        wo = probe_mmap(0, 4096, PROT_WRITE, MAP_PRIVATE, fd, 0);
    }
    check(PAGES_WRONLY_FILE, made && filled && wo != 0 && !sys_failed(wo) &&
                                 vmsplice_one(w[1], wo, 1, SPLICE_F_NONBLOCK) == -EFAULT);
    close_pair(w);
    if (wo != 0 && !sys_failed(wo)) {
        syscall2(SYS_MUNMAP, wo, 4096);
    }
    if (!sys_failed(fd)) {
        syscall1(SYS_CLOSE, fd);
    }
}

/**
 * The rows of case (l) that fill a nonblocking pipe of 64 KiB by its sixteen
 * buffers and then write: a write opens a buffer only while one is free, as
 * Linux's pipe_write stops at pipe_full, so a byte after sixteen one-byte
 * vmsplices, whose buffers take no write, and 4096 bytes after a byte and
 * fifteen pages, which fill sixteen buffers with 61441 bytes, are EAGAIN.
 */
static void pages_write_slots(void)
{
    long map = mmap_anon(4096);
    int w[2] = {-1, -1};
    int made = do_pipe2(w, O_NONBLOCK) == 0, mapped = map != 0 && !sys_failed(map);
    int i, took = 0;

    for (i = 0; made && mapped && i < 16; i++) {
        took += vmsplice_one(w[1], map, 1, SPLICE_F_NONBLOCK) == 1;
    }
    check(PAGES_WRITE_SLOTS,
          made && mapped && took == 16 && wr(w[1], "x", 1) == -EAGAIN && rd(w[0], back, 64) == 16);
    took = made && wr(w[1], big, 1) == 1;
    for (i = 0; took && i < 15; i++) {
        took = wr(w[1], big, 4096) == 4096;
    }
    check(PAGES_WRITE_PAGES, took && wr(w[1], big, 4096) == -EAGAIN);
    close_pair(w);
    if (mapped) {
        syscall2(SYS_MUNMAP, map, 4096);
    }
}

/** Append the string `s` to `buf` at `*at`, moving `*at` past it. */
static void put_str(char *buf, int *at, const char *s)
{
    while (*s) {
        buf[(*at)++] = *s++;
    }
}

/** The state letter of process `pid` in /proc/PID/stat, the one after the
 * name's closing parenthesis, or 0 when the file cannot be read. */
static int proc_state(int pid)
{
    char path[32], line[256];
    int fd, n, i = 0, d = 1;

    put_str(path, &i, "/proc/");
    while (d <= pid / 10) {
        d *= 10;
    }
    for (; d > 0; d /= 10) {
        path[i++] = (char)('0' + pid / d % 10);
    }
    put_str(path, &i, "/stat");
    path[i] = '\0';
    fd = (int)syscall3(SYS_OPEN, (long)path, 0, 0);
    if (fd < 0) {
        return 0;
    }
    n = rd(fd, line, (int)sizeof(line) - 1);
    syscall1(SYS_CLOSE, fd);
    for (i = n - 1; i > 0 && line[i] != ')'; i--) {
    }
    return i > 0 && i + 2 < n && line[i + 1] == ' ' ? line[i + 2] : 0;
}

/** The child of `vmsplice_wait`: once `parent` is asleep, state S, looked
 * at every 10 ms, write a byte to `fd` and exit 0; exit 2 when the parent
 * ends first, and 1 when the write fails. Does not return. */
static void vmsplice_waker(int parent, int fd)
{
    static const long ts[2] = {0, 10000000};
    int state;

    for (;;) {
        state = proc_state(parent);
        if (state == 'S') {
            sys_exit(wr(fd, "v", 1) == 1 ? 0 : 1);
        }
        if (state == 0 || state == 'Z') {
            sys_exit(2);
        }
        /* interval: the parent's state is looked at again every 10 ms */
        syscall2(SYS_NANOSLEEP, (long)ts, 0);
    }
}

/**
 * The row of case (l) that vmsplices from the read end of an empty pipe made
 * with O_NONBLOCK, without SPLICE_F_NONBLOCK: Linux's vmsplice passes the
 * caller's flags alone, so the call waits, and a child that sees the probe
 * asleep writes the byte it returns. A kernel that took the description's
 * O_NONBLOCK answers EAGAIN.
 */
static void vmsplice_wait(void)
{
    long iov[2];
    int w[2] = {-1, -1};
    int made = do_pipe2(w, O_NONBLOCK) == 0, parent = (int)syscall1(SYS_GETPID, 0), pid = -1;
    long n = -1;

    if (made) {
        pid = (int)syscall1(SYS_FORK, 0);
    }
    if (pid == 0) {
        vmsplice_waker(parent, w[1]);
    }
    check(VMSPLICE_WAIT_FORK, made && pid > 0);
    back[0] = 0;
    if (pid > 0) {
        iov[0] = (long)back;
        iov[1] = 1;
        n = syscall4(SYS_VMSPLICE, w[0], (long)iov, 1, 0);
    }
    check(VMSPLICE_WAIT_NONBLOCK, pid > 0 && n == 1 && back[0] == 'v' && exited_zero(pid));
    close_pair(w);
}

/**
 * Case (l): a shrink is refused below the page buffers that hold bytes, as
 * Linux counts them, however few bytes they hold. A write's first `count`
 * mod 4096 bytes join the newest buffer only when they fit in its page after
 * the bytes written to it and it came from a write; a buffer tee made takes
 * no write, one splice moved whole from a write takes one, and vmsplice
 * makes a buffer of each page of the caller's it reads.
 */
static void case_pages(void)
{
    int p[2] = {-1, -1}, q[2] = {-1, -1};
    const char two[2] = {'a', 'b'};
    int made = do_pipe2(p, O_NONBLOCK) == 0 && do_pipe2(q, O_NONBLOCK) == 0;

    check(PAGES_PIPES, made);
    if (!made) {
        close_pair(p);
        close_pair(q);
        return;
    }
    check(PAGES_BUSY, wr(p[1], big, 4096) == 4096 && rd(p[0], back, 4095) == 4095 &&
                          wr(p[1], two, 1) == 1 && set_size(p[1], 4096) == -EBUSY &&
                          rd(p[0], back, 2) == 2);
    check(PAGES_MERGE, wr(p[1], two, 1) == 1 && wr(p[1], two + 1, 1) == 1 &&
                           set_size(p[1], 4096) == 4096 && rd(p[0], back, 2) == 2 &&
                           back[0] == 'a' && back[1] == 'b');
    check(PAGES_TEE, wr(p[1], two, 1) == 1 &&
                         syscall4(SYS_TEE, p[0], q[1], 1, SPLICE_F_NONBLOCK) == 1 &&
                         wr(q[1], two + 1, 1) == 1 && set_size(q[1], 4096) == -EBUSY &&
                         rd(q[0], back, 2) == 2 && back[0] == 'a' && back[1] == 'b');
    pages_splices(p, q);
    pages_file(q);
    pages_slots();
    pages_wronly_file();
    pages_write_slots();
    vmsplice_wait();
    close_pair(p);
    close_pair(q);
}

#define PIPE_MAX_FILE  "/proc/sys/fs/pipe-max-size"
#define PIPE_SOFT_FILE "/proc/sys/fs/pipe-user-pages-soft"
#define PIPE_HARD_FILE "/proc/sys/fs/pipe-user-pages-hard"

/* Why case (k) wrote no /proc/sys file, or NULL when it did. */
static const char *limits_skipped;

/** Write the text `s` to the /proc/sys file `path`: the count written, or a
 * negated errno. */
static int sysctl_put(const char *path, const char *s)
{
    int fd = (int)syscall3(SYS_OPEN, (long)path, O_WRONLY, 0), n;

    if (fd < 0) {
        return fd;
    }
    n = wr(fd, s, (int)strlen(s));
    syscall1(SYS_CLOSE, fd);
    return n;
}

/** Whether the /proc/sys file `path` reads exactly `s`. */
static int sysctl_is(const char *path, const char *s)
{
    char buf[32];
    int fd = (int)syscall3(SYS_OPEN, (long)path, 0, 0), n;

    if (fd < 0) {
        return 0;
    }
    n = rd(fd, buf, (int)sizeof(buf));
    syscall1(SYS_CLOSE, fd);
    return n == (int)strlen(s) && eqn(buf, s, n);
}

/** Wait for the child `pid`: its exit status, or 255 when it did not exit
 * by itself. */
static int exit_status(int pid)
{
    int status = -1;

    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || (status & 0x7f) != 0) {
        return 255;
    }
    return (status >> 8) & 0xff;
}

/* The bits a child of case (k) exits with, one for each row that failed. */
#define LIMIT_BAD_USER 0x01 /* setuid to NOBODY failed */
#define LIMIT_BAD_PAST 0x02
#define LIMIT_BAD_TO   0x04
#define SOFT_BAD_OWN   0x08
#define SOFT_BAD_PAST  0x10
#define SOFT_BAD_NEW   0x20
#define SOFT_BAD_SHR   0x40

/**
 * The child of case (k): as NOBODY when `become` is set, and otherwise as
 * the probe's user, it checks pipe-max-size's limit when `soft` is 0, and
 * pipe-user-pages-soft's, which the parent set to 64, when it is 1. Exits
 * with the bits of the rows that failed.
 */
static void limits_child(int become, int soft)
{
    int a[2], b[2], c[2], bad = 0;

    if (become && syscall1(SYS_SETUID, NOBODY) != 0) {
        sys_exit(LIMIT_BAD_USER);
    }
    if (!soft) {
        if (do_pipe(a) != 0) {
            sys_exit(LIMIT_BAD_PAST | LIMIT_BAD_TO);
        }
        if (set_size(a[1], 2097152) != -EPERM) {
            bad |= LIMIT_BAD_PAST;
        }
        if (set_size(a[1], 600000) != 1048576) {
            bad |= LIMIT_BAD_TO;
        }
        sys_exit(bad);
    }
    if (do_pipe(a) != 0 || do_pipe(b) != 0) {
        sys_exit(SOFT_BAD_OWN | SOFT_BAD_PAST | SOFT_BAD_NEW | SOFT_BAD_SHR);
    }
    if (set_size(a[1], 131072) != 131072 || set_size(b[1], 131072) != 131072) {
        bad |= SOFT_BAD_OWN;
    }
    if (set_size(b[1], 262144) != -EPERM) {
        bad |= SOFT_BAD_PAST;
    }
    if (do_pipe(c) != 0 || syscall3(SYS_FCNTL, c[0], F_GETPIPE_SZ, 0) != 8192) {
        bad |= SOFT_BAD_NEW;
    }
    if (set_size(b[1], 4096) != 4096) {
        bad |= SOFT_BAD_SHR;
    }
    sys_exit(bad);
}

/** Fork a child of case (k) for `limits_child`: its exit status, or 255
 * when the fork failed, which LIMIT_FORK records. */
static int limits_fork(int become, int soft)
{
    int pid = (int)syscall1(SYS_FORK, 0);

    if (pid == 0) {
        limits_child(become, soft);
    }
    check(LIMIT_FORK, pid > 0);
    return pid > 0 ? exit_status(pid) : 255;
}

/**
 * Case (k): the limits of a caller without privilege. The writes to
 * /proc/sys need root and are left out under `machine=native`; each file is
 * set back to Linux's value at boot whatever the rows found.
 */
static void case_limits(void)
{
    int root = syscall0(SYS_GETEUID) == 0, bits, r[2] = {-1, -1};

    limits_skipped = !root            ? "the probe runs without privilege"
                     : probe_native() ? "machine=native: a limit set binds every user's pipes"
                                      : 0;
    if (!limits_skipped) {
        check(MAX_ROUND,
              sysctl_put(PIPE_MAX_FILE, "600000\n") == 7 && sysctl_is(PIPE_MAX_FILE, "1048576\n") &&
                  sysctl_put(PIPE_MAX_FILE, "1\n") == 2 && sysctl_is(PIPE_MAX_FILE, "4096\n") &&
                  sysctl_put(PIPE_MAX_FILE, "2147483648\n") == 11 &&
                  sysctl_is(PIPE_MAX_FILE, "2147483648\n"));
        check(MAX_EINVAL, sysctl_put(PIPE_MAX_FILE, "2147483649\n") == -EINVAL);
        sysctl_put(PIPE_MAX_FILE, "1048576\n");
        check(SYSCTL_PLUS, sysctl_put(PIPE_SOFT_FILE, "+64\n") == -EINVAL &&
                               sysctl_is(PIPE_SOFT_FILE, "16384\n") &&
                               sysctl_put(PIPE_HARD_FILE, "+64\n") == -EINVAL &&
                               sysctl_is(PIPE_HARD_FILE, "0\n"));
    }
    bits = limits_fork(root, 0);
    check(LIMIT_PAST_MAX, (bits & (LIMIT_BAD_USER | LIMIT_BAD_PAST)) == 0);
    check(LIMIT_TO_MAX, (bits & (LIMIT_BAD_USER | LIMIT_BAD_TO)) == 0);
    if (limits_skipped) {
        return;
    }
    check(SOFT_ROOT, sysctl_put(PIPE_SOFT_FILE, "64\n") == 3 && do_pipe(r) == 0 &&
                         set_size(r[1], 524288) == 524288);
    bits = limits_fork(1, 1);
    check(SOFT_OWN, (bits & (LIMIT_BAD_USER | SOFT_BAD_OWN)) == 0);
    check(SOFT_PAST, (bits & (LIMIT_BAD_USER | SOFT_BAD_PAST)) == 0);
    check(SOFT_NEW_SMALL, (bits & (LIMIT_BAD_USER | SOFT_BAD_NEW)) == 0);
    check(SOFT_SHRINK, (bits & (LIMIT_BAD_USER | SOFT_BAD_SHR)) == 0);
    sysctl_put(PIPE_SOFT_FILE, "16384\n");
    close_pair(r);
}

/** Report each check: held, failed with its text, or not run, since a check
 * it depends on failed, for the heap's under `machine=native`, or for the
 * rows of case (k) that write /proc/sys when it wrote none; and end the
 * report. */
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
        } else if (limits_skipped &&
                   (i == MAX_ROUND || i == MAX_EINVAL || i == SYSCTL_PLUS || i >= SOFT_ROOT)) {
            report_notrun(checks[i].id, limits_skipped);
        } else {
            report_notrun(checks[i].id, "a check it depends on failed");
        }
    }
    report_end();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run cases (a) to (l) in order; a failed check is printed and counted,
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
    case_wrap();
    case_limits();
    case_pages();
    report_checks();
    if (fails == 0) {
        puts("PIPEPROBE OK");
    } else {
        puts("PIPEPROBE FAIL");
    }
    sys_exit(fails);
}
