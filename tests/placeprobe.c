/*
 * placeprobe: entry 26's placement experiment, the emulator's cost of a
 * read buffer that shares a page with executed code.
 *
 * Two page-aligned buffers of 64 KiB in `.bss`, `near` and `far`. Before
 * the runs the probe writes one `ret` instruction at `near[0]` and calls
 * it, so the page holding `near`'s first bytes is code the emulator has
 * translated; `far` is never executed. The same work for both: a forked
 * child writes 32 MiB of a pattern into a pipe in 4 KiB writes, and the
 * parent reads it into the buffer from `buf + 16`, asking for the rest of
 * the buffer and getting at most a page, since the pipe holds one, so
 * every read of the near buffer writes into the page with the `ret`, which
 * the parent calls once per read to keep it code. The first and last byte
 * of each read's extent are checked and then cleared, so a read that
 * reported bytes it did not copy would be caught by the next; only an end
 * of file before 32 MiB is short. Each run is timed by the monotonic
 * clock; three of each, near and far alternating.
 *
 * Prints, per run, `PLACEPROBE NEAR N ms KiB/s` or `PLACEPROBE FAR N ms
 * KiB/s`, and first the mappings: the code's pages from `_start` to
 * `etext` and each buffer's pages, which must not meet. Then `PLACEPROBE
 * OK`, or `PLACEPROBE FAIL: reason`. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"

#define CLOCK_MONOTONIC 1
#define PAGE            4096u
#define TOTAL           (32u * 1024u * 1024u)
#define BUFFER          65536u
#define READ_AT         16u

static void put(const char *s)
{
    int n = 0;

    while (s[n]) {
        n++;
    }
    syscall3(SYS_WRITE, 1, (long)s, n);
}

static void putnum(unsigned v)
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    put(&buf[i]);
}

static void puthex(unsigned long v)
{
    char buf[20];
    int i = 19;

    buf[i] = 0;
    do {
        buf[--i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v != 0);
    put("0x");
    put(&buf[i]);
}

static void fail(const char *why)
{
    put("PLACEPROBE FAIL: ");
    put(why);
    put("\n");
    syscall1(SYS_EXIT, 1);
}

extern char _start[];
extern char etext[];

static unsigned char near_buf[BUFFER] __attribute__((aligned(4096)));
static unsigned char far_buf[BUFFER] __attribute__((aligned(4096)));
static unsigned char page_out[PAGE];

struct stamp {
    long sec;
    long nsec;
};

static unsigned ms_between(const struct stamp *a, const struct stamp *b)
{
    return (unsigned)((b->sec - a->sec) * 1000 + (b->nsec - a->nsec) / 1000000);
}

/** One run: the child writes, the parent reads into `buf`; milliseconds. */
static unsigned run(unsigned char *buf, int near)
{
    int p[2];
    int pid;
    int status;
    unsigned total = 0;
    struct stamp before, after;
    void (*code)(void) = (void (*)(void))near_buf;

    p[0] = -1;
    p[1] = -1;
    if (syscall1(SYS_PIPE, (long)p) != 0) {
        fail("pipe");
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        unsigned written = 0;

        syscall1(SYS_CLOSE, p[0]);
        while (written < TOTAL) {
            int w = syscall3(SYS_WRITE, p[1], (long)page_out, (int)PAGE);

            if (w <= 0) {
                syscall1(SYS_EXIT, 2);
            }
            written += (unsigned)w;
        }
        syscall1(SYS_CLOSE, p[1]);
        syscall1(SYS_EXIT, 0);
    }
    syscall1(SYS_CLOSE, p[1]);
    syscall3(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before, 0);
    for (;;) {
        int n = syscall3(SYS_READ, p[0], (long)(buf + READ_AT), (int)(BUFFER - READ_AT));

        if (n < 0) {
            fail("a read of the pipe failed");
        }
        if (n == 0) {
            break;
        }
        if (buf[READ_AT] != 0x5A || buf[READ_AT + (unsigned)n - 1] != 0x5A) {
            fail("a read reported bytes it did not copy");
        }
        buf[READ_AT] = 0;
        buf[READ_AT + (unsigned)n - 1] = 0;
        total += (unsigned)n;
        if (near) {
            code(); /* the page's code, run again after the write into it */
        }
    }
    syscall3(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after, 0);
    syscall1(SYS_CLOSE, p[0]);
    syscall3(SYS_WAITPID, pid, (long)&status, 0);
    if (total != TOTAL) {
        fail("the end of file came before 32 MiB");
    }
    if (((status >> 8) & 0xff) != 0) {
        fail("the writer did not write it all");
    }
    return ms_between(&before, &after);
}

static void report(const char *which, unsigned ms)
{
    put("PLACEPROBE ");
    put(which);
    put(" ");
    putnum(ms);
    put(" ms ");
    putnum(ms > 0 ? (TOTAL / 1024u) * 1000u / ms : 0);
    put(" KiB/s\n");
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

void probe_main(unsigned long *sp)
{
    unsigned long code_lo = (unsigned long)_start & ~(unsigned long)(PAGE - 1);
    unsigned long code_hi = ((unsigned long)etext + PAGE - 1) & ~(unsigned long)(PAGE - 1);
    unsigned i;

    (void)sp;
    for (i = 0; i < PAGE; i++) {
        page_out[i] = 0x5A;
    }
    put("PLACEPROBE code ");
    puthex(code_lo);
    put("-");
    puthex(code_hi);
    put(" near ");
    puthex((unsigned long)near_buf);
    put("-");
    puthex((unsigned long)near_buf + BUFFER);
    put(" far ");
    puthex((unsigned long)far_buf);
    put("-");
    puthex((unsigned long)far_buf + BUFFER);
    put("\n");
    if (((unsigned long)near_buf < code_hi && (unsigned long)near_buf + BUFFER > code_lo) ||
        ((unsigned long)far_buf < code_hi && (unsigned long)far_buf + BUFFER > code_lo)) {
        fail("a buffer shares a page with the program's own code");
    }
    /* The near buffer's first page becomes code: one `ret`, called. x86-64
       maps `.bss` without execute permission, so the page is given it
       first; i386 pages have no such bit. */
#if defined __x86_64__
    if (syscall3(SYS_MPROTECT, (long)near_buf, PAGE, 7) != 0) {
        fail("the near buffer's first page could not be made executable");
    }
#endif
    near_buf[0] = 0xC3;
    ((void (*)(void))near_buf)();

    for (i = 0; i < 3; i++) {
        report("NEAR", run(near_buf, 1));
        report("FAR", run(far_buf, 0));
    }
    put("PLACEPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
