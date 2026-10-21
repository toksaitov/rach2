/**
 * vmprobe: the regions and demand paging of a process, tested from user
 * space: brk() (query, grow, lazy-zero, read/write, shrink), anonymous
 * mmap() (map, lazy-zero, read/write, munmap), brk refusing to grow over an
 * mmap region, and stack growth by deep recursion that faults pages well
 * past the initial one.
 *
 * Prints "VMPROBE OK" and exits 0 when every check passes, and otherwise
 * "VMPROBE FAIL count=N" and exits with N, the count of failed checks. The
 * wild-pointer case is a separate program (vmfault), since it kills the
 * process.
 *
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each check, which `check` names: `brk-grow`, `brk-zero`,
 * `brk-readback`, `brk-shrink`, `mmap`, `mmap-zero`, `mmap-readback`,
 * `munmap`, `collision-map`, `collision-above`, `collision-refused` and
 * `stack`. A check left out because the mapping it needs failed is reported
 * not run.
 */

#include "probe.h"
#include "probe_report.h"

#define STDOUT_FILENO 1

#define PAGE 4096u

/* mmap prot/flags (Linux's, on i386 and x86-64). */
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

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

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe: the four cases of the opening comment in order, each counted
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

    /* 4. Stack growth by about 64 KiB of recursion, far past the first page. */
    check(recurse(64) == 64 * 65 / 2, "stack", "stack grew under deep recursion");

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
