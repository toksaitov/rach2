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
 * OK`, or `PLACEPROBE FAIL: reason`. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT          1
#define SYS_FORK          2
#define SYS_READ          3
#define SYS_WRITE         4
#define SYS_CLOSE         6
#define SYS_WAITPID       7
#define SYS_PIPE          42
#define SYS_CLOCK_GETTIME 265

#define CLOCK_MONOTONIC 1
#define PAGE            4096u
#define TOTAL           (32u * 1024u * 1024u)
#define BUFFER          65536u
#define READ_AT         16u

static int sys3(int n, int a, int b, int c)
{
    int r;

    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static int sys1(int n, int a)
{
    return sys3(n, a, 0, 0);
}

static void put(const char *s)
{
    int n = 0;

    while (s[n]) {
        n++;
    }
    sys3(SYS_WRITE, 1, (int)s, n);
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

static void puthex(unsigned v)
{
    char buf[12];
    int i = 11;

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
    sys1(SYS_EXIT, 1);
}

extern char _start[];
extern char etext[];

static unsigned char near_buf[BUFFER] __attribute__((aligned(4096)));
static unsigned char far_buf[BUFFER] __attribute__((aligned(4096)));
static unsigned char page_out[PAGE];

struct stamp {
    int sec;
    int nsec;
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
    if (sys1(SYS_PIPE, (int)p) != 0) {
        fail("pipe");
    }
    pid = sys1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        unsigned written = 0;

        sys1(SYS_CLOSE, p[0]);
        while (written < TOTAL) {
            int w = sys3(SYS_WRITE, p[1], (int)page_out, (int)PAGE);

            if (w <= 0) {
                sys1(SYS_EXIT, 2);
            }
            written += (unsigned)w;
        }
        sys1(SYS_CLOSE, p[1]);
        sys1(SYS_EXIT, 0);
    }
    sys1(SYS_CLOSE, p[1]);
    sys3(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (int)&before, 0);
    for (;;) {
        int n = sys3(SYS_READ, p[0], (int)(buf + READ_AT), (int)(BUFFER - READ_AT));

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
    sys3(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (int)&after, 0);
    sys1(SYS_CLOSE, p[0]);
    sys3(SYS_WAITPID, pid, (int)&status, 0);
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

void probe_main(unsigned int *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned int *sp)
{
    unsigned code_lo = (unsigned)_start & ~(PAGE - 1);
    unsigned code_hi = ((unsigned)etext + PAGE - 1) & ~(PAGE - 1);
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
    puthex((unsigned)near_buf);
    put("-");
    puthex((unsigned)near_buf + BUFFER);
    put(" far ");
    puthex((unsigned)far_buf);
    put("-");
    puthex((unsigned)far_buf + BUFFER);
    put("\n");
    if (((unsigned)near_buf < code_hi && (unsigned)near_buf + BUFFER > code_lo) ||
        ((unsigned)far_buf < code_hi && (unsigned)far_buf + BUFFER > code_lo)) {
        fail("a buffer shares a page with the program's own code");
    }
    /* The near buffer's first page becomes code: one `ret`, called. */
    near_buf[0] = 0xC3;
    ((void (*)(void))near_buf)();

    for (i = 0; i < 3; i++) {
        report("NEAR", run(near_buf, 1));
        report("FAR", run(far_buf, 0));
    }
    put("PLACEPROBE OK\n");
    sys1(SYS_EXIT, 0);
}
