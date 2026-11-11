/**
 * vmprobe: the regions and demand paging of a process, tested from user
 * space: brk() (query, grow, lazy-zero, read/write, shrink), anonymous
 * mmap() (map, lazy-zero, read/write, munmap), brk refusing to grow over an
 * mmap region, a mapping placed below the break once the room above it is
 * taken, as Linux places it, the guard gap of 1 MiB below the stack's
 * growth window, which neither a hint nor a mapping that fills the address
 * space reaches, stack growth by deep recursion that faults pages well
 * past the initial one, and the 128 KiB the exec put in the stack's region
 * below the entry stack pointer, which a write reaches with a mapping right
 * under the region.
 *
 * Prints "VMPROBE OK" and exits 0 when every check passes, and otherwise
 * "VMPROBE FAIL count=N" and exits with N, the count of failed checks. The
 * wild-pointer case is a separate program (vmfault), since it kills the
 * process.
 *
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each check, which `check` names: `brk-grow`, `brk-zero`,
 * `brk-readback`, `brk-shrink`, `mmap`, `mmap-zero`, `mmap-readback`,
 * `munmap`, `collision-map`, `collision-above`, `collision-refused`,
 * `below-break`, `stack-gap-hint`, `stack-gap-fill`, `stack` and
 * `stack-expand`. A check left out because the mapping it needs failed, or
 * because RLIMIT_STACK is not the 8 MiB that places the window, is reported
 * not run.
 */

#include "probe.h"
#include "probe_report.h"

#define STDOUT_FILENO 1

#define PAGE 4096u

/* mmap prot/flags (Linux's, on i386 and x86-64). */
#define PROT_NONE           0x0
#define PROT_READ           0x1
#define PROT_WRITE          0x2
#define MAP_PRIVATE         0x02
#define MAP_ANONYMOUS       0x20
#define MAP_FIXED_NOREPLACE 0x100000

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

/** brk(addr): returns the resulting break (new on success, old on failure). */
static unsigned long sys_brk(unsigned long addr)
{
    return syscall1(SYS_BRK, (long)addr);
}

/** An anonymous private mapping of `len` bytes; its address, or a small
 * negative errno. */
static unsigned long mmap_anon(unsigned long len, int prot)
{
    return (unsigned long)probe_mmap(0, (long)len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/** The munmap call: 0, or a negated errno. */
static int sys_munmap(unsigned long addr, unsigned long len)
{
    return syscall2(SYS_MUNMAP, (long)addr, (long)len);
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

/** Write `s` to standard output. */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    print(s);
    sys_write(STDOUT_FILENO, "\n", 1);
}

/** Write `n` in decimal to standard output. */
static void print_num(int n)
{
    char buf[12];
    int i = 0;

    if (n == 0) {
        buf[i++] = '0';
    } else {
        if (n < 0) {
            print("-");
            n = -n;
        }
        while (n > 0) {
            buf[i++] = (char)('0' + (n % 10));
            n /= 10;
        }
    }
    while (i > 0) {
        sys_write(STDOUT_FILENO, &buf[--i], 1);
    }
}

static int fails = 0;

/** Begin the part of the assertion `id` (probe_report.h), unless it is the
 * part open. */
static void part(const char *id)
{
    static const char *open;
    int i = 0;

    if (open != 0) {
        while (id[i] != '\0' && open[i] == id[i]) {
            i++;
        }
        if (open[i] == id[i]) {
            return;
        }
    }
    report_part(id);
    open = id;
}

/** Count a failure of the assertion `id` and print `msg` when `cond` is
 * false. */
static void check(int cond, const char *id, const char *msg)
{
    part(id);
    if (!cond) {
        fails++;
        print("  FAIL: ");
        puts(msg);
        report_miss(msg);
    }
}

/**
 * Recurse `n` deep with a 1 KiB buffer per frame, touching every byte so
 * that each new stack page faults in. The buffer is checked again after the
 * deeper call returns, so a deeper stack fault that corrupted this frame is
 * caught on the way back up. Returns 1 + 2 + ... + n if every frame stayed
 * intact, else a large negative value.
 */
static int recurse(int n)
{
    volatile char buf[1024];
    int i, deeper;

    if (n == 0) {
        return 0;
    }
    for (i = 0; i < 1024; i++) {
        buf[i] = (char)(n + i);
    }

    deeper = recurse(n - 1); /* faults in more stack pages below this frame */

    for (i = 0; i < 1024; i++) {
        if (buf[i] != (char)(n + i)) {
            return -100000;
        }
    }
    return n + deeper;
}

/**
 * Take the room between the break and the mmap base with PROT_NONE
 * reservations, each the largest that still fits, and check that the next
 * one goes below the break, as Linux's top-down search places it and as an
 * i386 Wine, which reserves nearly all above its loader's heap, needs. The
 * reservations are removed before it returns.
 */
static void below_break(void)
{
    static unsigned long held[160][2];
    unsigned long old = sys_brk(0);
    unsigned long len = 1ul << (sizeof(long) == 8 ? 46 : 30);
    unsigned n = 0, i;

    while (len >= PAGE && n < 160) {
        unsigned long p = mmap_anon(len, PROT_NONE);

        if (sys_failed((long)p)) {
            len >>= 1;
            continue;
        }
        held[n][0] = p;
        held[n][1] = len;
        n++;
        if (p < old) {
            break;
        }
    }
    check(n > 0 && held[n - 1][0] < old, "below-break",
          "with the room above the break taken, mmap placed a range below it");
    for (i = 0; i < n; i++) {
        sys_munmap(held[i][0], held[i][1]);
    }
}

/* The stack's growth window under the default RLIMIT_STACK, and the guard
 * gap below it that no mapping without MAP_FIXED takes (VM_STACK_GUARD_GAP). */
#define STACK_WINDOW 0x800000ul
#define GUARD_GAP    0x100000ul

/** The value of the hexadecimal digits at `*p`, which moves past them. */
static unsigned long hex_at(const char **p)
{
    unsigned long v = 0;

    for (;; (*p)++) {
        char c = **p;

        if (c >= '0' && c <= '9') {
            v = v * 16 + (unsigned long)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v = v * 16 + (unsigned long)(c - 'a' + 10);
        } else {
            return v;
        }
    }
}

/** The end of the [stack] line of /proc/self/maps, the top of the stack's
 * window, or 0; its start, the bottom of the stack's region, in `*start`. */
static unsigned long stack_range(unsigned long *start)
{
    static char maps[8192];
    int fd = (int)syscall3(SYS_OPEN, (long)"/proc/self/maps", 0, 0);
    int n, i, line = 0;

    if (fd < 0) {
        return 0;
    }
    n = (int)syscall3(SYS_READ, fd, (long)maps, (long)sizeof(maps) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return 0;
    }
    maps[n] = 0;
    for (i = 0; i < n; i++) {
        if (maps[i] == '\n') {
            line = i + 1;
        } else if (maps[i] == '[' && maps[i + 1] == 's' && maps[i + 2] == 't' &&
                   maps[i + 3] == 'a' && maps[i + 4] == 'c' && maps[i + 5] == 'k') {
            const char *q = maps + line;

            *start = hex_at(&q);
            q++;
            return hex_at(&q);
        }
    }
    return 0;
}

/**
 * The guard gap below the stack's window, whose bottom, the stack's limit,
 * lies STACK_WINDOW below the stack's top under the default RLIMIT_STACK:
 * a hint whose page ends at the gap is taken and one in the gap is not
 * (`mmap_place`), and PROT_NONE reservations without a hint, each the
 * largest that still fits, take every free range there is, the highest of
 * them ending at the gap (`mmap_free`). The reservations are removed before
 * it returns.
 */
static void stack_gap(void)
{
    static unsigned long held[400][2];
    struct {
        unsigned long long cur, max;
    } rl;
    unsigned long bottom, top = stack_range(&bottom), gap, high = 0, p;
    unsigned long len = 1ul << (sizeof(long) == 8 ? 46 : 30);
    unsigned n = 0, i;

    if (syscall4(SYS_PRLIMIT64, 0, 3, 0, (long)&rl) != 0 || rl.cur != STACK_WINDOW ||
        rl.max < rl.cur || top < STACK_WINDOW + GUARD_GAP) {
        report_notrun("stack-gap-hint", "RLIMIT_STACK or the stack's top is not known");
        report_notrun("stack-gap-fill", "RLIMIT_STACK or the stack's top is not known");
        return;
    }
    gap = top - STACK_WINDOW - GUARD_GAP;
    p = (unsigned long)probe_mmap((long)(gap - PAGE), PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                                  -1, 0);
    check(p == gap - PAGE, "stack-gap-hint", "a hint whose page ends at the guard gap is taken");
    if (!sys_failed((long)p)) {
        sys_munmap(p, PAGE);
    }
    p = (unsigned long)probe_mmap((long)gap, PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(p != gap, "stack-gap-hint", "a hint in the guard gap is not taken");
    if (!sys_failed((long)p)) {
        sys_munmap(p, PAGE);
    }
    while (len >= PAGE && n < 400) {
        p = mmap_anon(len, PROT_NONE);
        if (sys_failed((long)p)) {
            len >>= 1;
            continue;
        }
        held[n][0] = p;
        held[n][1] = len;
        n++;
        high = p + len > high ? p + len : high;
    }
    check(len < PAGE && high == gap, "stack-gap-fill",
          "mappings that fill the address space end at the guard gap below the stack's limit");
    for (i = 0; i < n; i++) {
        sys_munmap(held[i][0], held[i][1]);
    }
}

/* The growth of the stack's region at an exec below the pages its arguments
 * took, and how far below the entry stack pointer the write goes. */
#define EXEC_STACK_EXPAND 0x20000ul
#define EXPAND_REACH      0x1e000ul

/**
 * The stack's region at the exec reaches EXEC_STACK_EXPAND below the pages
 * the arguments took, as Linux 6.12.115's setup_arg_pages expands it
 * (fs/exec.c:818-836), so that a write EXPAND_REACH below the entry stack
 * pointer `sp` lands in the region with no growth. A readable mapping is
 * placed on the page right under the region first, within the guard gap,
 * so a region that did not reach that far could not grow to the write
 * either (expand_downwards, mm/mmap.c:1167-1175). The write is made in a
 * child, whose death by the fault is the failure; the mapping is removed
 * before it returns.
 */
static void stack_expand(unsigned long sp)
{
    struct {
        unsigned long long cur, max;
    } rl;
    unsigned long bottom = 0, top = stack_range(&bottom), p;
    int pid, status = -1;

    if (syscall4(SYS_PRLIMIT64, 0, 3, 0, (long)&rl) != 0 ||
        rl.cur < top - sp + EXEC_STACK_EXPAND + PAGE || top == 0 || bottom < PAGE) {
        report_notrun("stack-expand", "RLIMIT_STACK or the stack's region is not known");
        return;
    }
    p = (unsigned long)probe_mmap((long)(bottom - PAGE), PAGE, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    check(p == bottom - PAGE, "stack-expand", "a page right under the stack's region is mapped");
    if (p != bottom - PAGE) {
        return;
    }
    pid = (int)syscall0(SYS_FORK);
    if (pid == 0) {
        *(volatile char *)(sp - EXPAND_REACH) = 1;
        sys_exit(0);
    }
    if (pid > 0) {
        syscall4(SYS_WAITPID, pid, (long)&status, 0, 0);
    }
    check(pid > 0 && status == 0, "stack-expand",
          "a write 120 KiB below the entry stack pointer lands in the stack's region");
    sys_munmap(p, PAGE);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe: the seven cases of the opening comment in order, each counted
 * by `check`; `_start` calls it with the initial stack. */
void probe_main(unsigned long *sp)
{
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.vmprobe"));
    puts("vmprobe: start");

    /* 1. brk: query, grow four pages, lazy-zero, read/write, shrink. */
    {
        unsigned long base = sys_brk(0);
        unsigned long want = base + 4 * PAGE;
        unsigned long got = sys_brk(want);
        volatile unsigned char *heap = (volatile unsigned char *)base;
        unsigned i;

        check(got == want, "brk-grow", "brk grow returns new break");
        check(heap[0] == 0 && heap[PAGE] == 0 && heap[3 * PAGE + 17] == 0, "brk-zero",
              "fresh heap pages read zero");
        for (i = 0; i < 4 * PAGE; i += 137) {
            heap[i] = (unsigned char)(i & 0xFF);
        }
        for (i = 0; i < 4 * PAGE; i += 137) {
            check(heap[i] == (unsigned char)(i & 0xFF), "brk-readback", "heap read-back matches");
        }
        check(sys_brk(base) == base, "brk-shrink", "brk shrink returns base");
    }

    /* 2. mmap anonymous: map eight pages, lazy-zero, read/write, munmap. */
    {
        unsigned len = 8 * PAGE;
        unsigned long p = mmap_anon(len, PROT_READ | PROT_WRITE);
        volatile unsigned char *m = (volatile unsigned char *)p;
        unsigned i;

        check(!sys_failed((long)p), "mmap", "mmap returns an address, not -errno");
        if (!sys_failed((long)p)) {
            check(m[0] == 0 && m[5 * PAGE] == 0 && m[len - 1] == 0, "mmap-zero",
                  "fresh mmap pages read zero");
            for (i = 0; i < len; i += 251) {
                m[i] = (unsigned char)((i * 7) & 0xFF);
            }
            for (i = 0; i < len; i += 251) {
                check(m[i] == (unsigned char)((i * 7) & 0xFF), "mmap-readback",
                      "mmap read-back matches");
            }
            check(sys_munmap(p, len) == 0, "munmap", "munmap succeeds");
        } else {
            report_notrun("mmap-zero", "the mapping failed");
            report_notrun("mmap-readback", "the mapping failed");
            report_notrun("munmap", "the mapping failed");
        }
    }

    /* 3. brk refuses to grow over a mapping, and returns the old break. */
    {
        unsigned long p = mmap_anon(PAGE, PROT_READ | PROT_WRITE);

        check(!sys_failed((long)p), "collision-map", "mmap for the brk collision succeeds");
        if (!sys_failed((long)p)) {
            unsigned long old = sys_brk(0);

            check(p >= old, "collision-above", "mmap placed the page above the break");
            if (p >= old) {
                /* A break at p + PAGE would overlap [p, p + PAGE). */
                unsigned long got = sys_brk(p + PAGE);

                check(got == old, "collision-refused", "brk refuses to grow over an mmap region");
                check(got != p + PAGE, "collision-refused",
                      "brk did not honor the colliding request");
            } else {
                report_notrun("collision-refused", "the page was not above the break");
            }
            sys_munmap(p, PAGE);
        } else {
            report_notrun("collision-above", "the mapping failed");
            report_notrun("collision-refused", "the mapping failed");
        }
    }

    /* 4. A mapping goes below the break once the room above it is taken. */
    below_break();

    /* 5. No mapping without MAP_FIXED reaches the guard gap below the stack. */
    stack_gap();

    /* 6. Stack growth by about 64 KiB of recursion, far past the first page. */
    check(recurse(64) == 64 * 65 / 2, "stack", "stack grew under deep recursion");

    /* 7. The exec put 128 KiB below the entry stack pointer in the region. */
    stack_expand((unsigned long)sp);

    report_finish();
    if (fails == 0) {
        puts("VMPROBE OK");
    } else {
        print("VMPROBE FAIL count=");
        print_num(fails);
        puts("");
    }
    sys_exit(fails);
}
