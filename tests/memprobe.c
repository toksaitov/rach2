/**
 * Memory beyond the old 32 MiB window, proved from user space.
 *
 * `memprobe MIB`: the probe maps MIB megabytes anonymously, writes each
 * page a pattern of its own index, and reads every page back, so the
 * frames above the window the kernel used to stop at hold what was written
 * and no two pages share a frame. Then it forks: the child rewrites every
 * fourth page and checks the rest, and the parent, once the child has
 * exited, finds every page as it wrote it, which is the copy-on-write copy
 * through the kernel's temporary mappings. Then a child executes
 * `memprobe fresh`, which maps 16 MiB and requires every byte zero, since
 * a frame that held the pattern a moment ago must come back clean. Then
 * the mapping is unmapped, and the free memory `/proc/meminfo` reports must
 * be within 256 KiB of what it was before, the mapping's page tables aside,
 * which stay with the process until it exits, so the frames came back. The
 * mapping and the pattern come first and the fork after, so the fork's page
 * tables and directory, which the kernel takes from its heap, are taken
 * when the frames below the window are already in use. On x86-64 MIB may
 * pass 4096, and a mapping of more memory than lies below 4 GiB has pages in
 * the frames above it, which `tests/run bigmem` asks for.
 *
 * `memprobe fail`, under `rach2.framefail`: with a patterned mapping of its
 * own, the probe arms the count-th frame allocation to fail for each count
 * from one to thirty-two, and forks; then again, and forks a child that
 * execs `memprobe fresh`. A fork or an exec that meets the failure must
 * answer ENOMEM, or the child must exit as it would without it, or, for an
 * exec that met it after the old image was gone, end by the kernel's hand,
 * and after
 * every attempt the parent's pages read as it wrote them, take a write, and
 * the free memory is what it was, so nothing leaked and nothing that
 * survived was changed.
 *
 * `memprobe total` prints the MemTotal `/proc/meminfo` reports, in KiB, as
 * `MEMPROBE MemTotal N kB`, for a suite to judge against the machine's size.
 *
 * `memprobe hog`: the probe takes anonymous memory a page at a time, each
 * page backed through a `read` of `/dev/zero` into it, until the kernel
 * refuses a page, which it does with EFAULT from the read once the free
 * memory is inside the reserve kept for root; then a fork must be refused
 * with ENOMEM, the probe says so, and it sleeps holding what it took until
 * it is killed, ignoring the hangup its shell's exit sends. What the row
 * then shows is that root can still log in beside it.
 *
 * Prints "MEMPROBE OK" or "MEMPROBE FAIL: reason". Freestanding; direct system
 * calls (probe.h).
 */

#include "probe.h"

#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define O_RDONLY      0
#define O_WRONLY      1
#define ENOMEM        12

#define PAGE 4096u

/* The KiB of page tables a mapping of `pages` pages keeps until the process
 * exits: on i386 a table for each 4 MiB, and on x86-64 a table for each
 * 2 MiB, a directory for each GiB and a pointer table for the 512 GiB the
 * mapping is in, each a page and one more of each for a mapping across a
 * boundary. */
#if defined __x86_64__
#define TABLE_KIB(pages) (((pages) / 512 + (pages) / (512ul * 512) + 4) * 4)
#else
#define TABLE_KIB(pages) (((pages) / 1024 + 1) * 4)
#endif

/* An anonymous mapping of `len` bytes; its address, or a negated errno. */
static unsigned long mmap_anon(unsigned long len)
{
    return (unsigned long)probe_mmap(0, (long)len, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

static void print(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
}

static void print_num(unsigned long v)
{
    char tmp[21];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        syscall3(SYS_WRITE, 1, (long)&tmp[--n], 1);
    }
}

static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (long)"MEMPROBE FAIL: ", 15);
    puts(why);
    sys_exit(1);
}

/* The value page `i` of a mapping at `base` holds in its word `w`. */
static unsigned pattern(unsigned i, unsigned w)
{
    return (i * 0x9E3779B1u) ^ (w * 0x01000193u) ^ 0xA5A5A5A5u;
}

static void write_page(unsigned *p, unsigned i)
{
    unsigned w;

    for (w = 0; w < PAGE / 4; w += 64) {
        p[w] = pattern(i, w);
    }
}

static int check_page(const unsigned *p, unsigned i)
{
    unsigned w;

    for (w = 0; w < PAGE / 4; w += 64) {
        if (p[w] != pattern(i, w)) {
            return 0;
        }
    }
    return 1;
}

/* The field `key` of /proc/meminfo, in KiB, or 0. */
static unsigned long meminfo_kib(const char *key)
{
    static char buf[1024];
    int fd = syscall3(SYS_OPEN, (long)"/proc/meminfo", O_RDONLY, 0);
    int n, i;

    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_READ, fd, (long)buf, (int)sizeof(buf) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return 0;
    }
    buf[n] = 0;
    for (i = 0; i + 8 < n; i++) {
        int k;

        for (k = 0; key[k] && buf[i + k] == key[k]; k++) {
        }
        if (!key[k]) {
            unsigned long v = 0;

            i += k;
            while (buf[i] == ' ') {
                i++;
            }
            while (buf[i] >= '0' && buf[i] <= '9') {
                v = v * 10 + (unsigned long)(buf[i] - '0');
                i++;
            }
            return v;
        }
    }
    return 0;
}

/* MemFree from /proc/meminfo, in KiB, or 0. */
static unsigned long mem_free_kib(void)
{
    return meminfo_kib("MemFree:");
}

/* `memprobe total`: the MemTotal /proc/meminfo reports, printed. */
static void run_total(void)
{
    unsigned long total = meminfo_kib("MemTotal:");

    if (total == 0) {
        fail("MemTotal");
    }
    print("MEMPROBE MemTotal ");
    print_num(total);
    print(" kB\n");
    sys_exit(0);
}

static unsigned parse_mib(const char *s)
{
    unsigned v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned)(*s - '0');
        s++;
    }
    return v;
}

static int wait_status(int pid)
{
    int status = -1;

    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        return -1;
    }
    return status;
}

/* `memprobe fresh`: 16 MiB mapped, every word zero. */
static void run_fresh(void)
{
    unsigned len = 16u << 20;
    unsigned long base = mmap_anon(len);
    unsigned i, w;

    if (sys_failed((long)base)) {
        sys_exit(2);
    }
    for (i = 0; i < len / PAGE; i++) {
        const unsigned *p = (const unsigned *)(base + i * PAGE);

        for (w = 0; w < PAGE / 4; w++) {
            if (p[w] != 0) {
                sys_exit(3);
            }
        }
    }
    sys_exit(0);
}

/* `/dev/framefail`, which `run_fail` opens before it arms the first count. */
static int framefail = -1;

/**
 * Arm the frame allocator to fail its `n`-th allocation in a system call, or
 * disarm it with 0. The count goes through the descriptor `run_fail` opened
 * once: an open allocates, and a count a fork or an exec left unspent would
 * fail the open that was to disarm it. The write allocates nothing.
 */
static void arm(unsigned n)
{
    char digits[12];
    int i = 0;
    unsigned v = n;

    do {
        digits[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (v = 0; v < (unsigned)i / 2; v++) {
        char t = digits[v];
        digits[v] = digits[i - 1 - (int)v];
        digits[i - 1 - (int)v] = t;
    }
    if (syscall3(SYS_WRITE, framefail, (long)digits, i) != i) {
        fail("rach2.framefail is not on this boot's line");
    }
}

/* The parent's own pages after an attempt: as written, then rewritten. */
static void survivors(unsigned long base, unsigned pages, unsigned round)
{
    unsigned i;

    for (i = 0; i < pages; i++) {
        if (!check_page((const unsigned *)(base + i * PAGE), i + round * 0x1000u)) {
            fail("a surviving page changed under a failed attempt");
        }
        write_page((unsigned *)(base + i * PAGE), i + (round + 1) * 0x1000u);
    }
}

static void run_fail(void)
{
    unsigned pages = 256;
    unsigned long base = mmap_anon(pages * PAGE);
    unsigned long baseline;
    unsigned n, i, round = 0;

    if (sys_failed((long)base)) {
        fail("the mapping");
    }
    for (i = 0; i < pages; i++) {
        write_page((unsigned *)(base + i * PAGE), i);
    }
    framefail = syscall3(SYS_OPEN, (long)"/dev/framefail", O_WRONLY, 0);
    if (framefail < 0) {
        fail("/dev/framefail did not open");
    }
    baseline = mem_free_kib();
    for (n = 1; n <= 32; n++) {
        int pid, status;
        unsigned long free_now;

        arm(n);
        pid = syscall1(SYS_FORK, 0);
        if (pid == 0) {
            sys_exit(0);
        }
        arm(0);
        if (pid < 0 && pid != -ENOMEM) {
            fail("a fork that met the failure did not answer ENOMEM");
        }
        if (pid > 0 && wait_status(pid) != 0) {
            fail("a child that was forked did not run");
        }
        survivors(base, pages, round++);
        free_now = mem_free_kib();
        if (free_now + 64 < baseline || free_now > baseline + 64) {
            fail("memory leaked or was freed twice around a fork");
        }
        pid = syscall1(SYS_FORK, 0);
        if (pid == 0) {
            char *args[3];

            /* Armed by the child itself, just before its exec: armed by the
               parent, the count would be the parent's to disarm, and whether
               the exec saw it would depend on which of the two ran first. */
            arm(n);
            args[0] = (char *)"memprobe";
            args[1] = (char *)"fresh";
            args[2] = 0;
            status = syscall3(SYS_EXECVE, (long)"/memprobe", (long)args, 0);
            sys_exit(status == -ENOMEM ? 7 : 5);
        }
        if (pid < 0) {
            fail("the fork before an exec failed");
        }
        if (pid > 0) {
            status = wait_status(pid);
            /* A count the exec did not reach would fire on the next
               allocation here; the child armed it, the parent disarms it. */
            arm(0);
            /* Ran whole, answered ENOMEM, or ended by the kernel: an exec
               that fails after the old image is gone has nothing to return
               to and the process ends, which is its contract. */
            if (status != 0 && status != (7 << 8) && (status & 0x7f) == 0) {
                fail("an exec that met the failure neither ran nor answered ENOMEM nor ended");
            }
        }
        survivors(base, pages, round++);
        free_now = mem_free_kib();
        if (free_now + 64 < baseline || free_now > baseline + 64) {
            print("MEMPROBE count ");
            print_num(n);
            print(" free ");
            print_num(free_now);
            print(" baseline ");
            print_num(baseline);
            print(" status ");
            print_num((unsigned)status);
            print("\n");
            fail("memory leaked or was freed twice around an exec");
        }
    }
    puts("MEMPROBE OK");
    sys_exit(0);
}

#define SIGHUP 1
#define EFAULT 14

static void run_hog(void)
{
    int zero = syscall3(SYS_OPEN, (long)"/dev/zero", 0, 0);
    struct {
        unsigned long handler, flags, restorer;
        unsigned long long mask;
    } act;
    unsigned taken = 0;
    int pid;

    if (zero < 0) {
        fail("open /dev/zero");
    }
    act.handler = 1; /* SIG_IGN */
    act.flags = 0;
    act.restorer = 0;
    act.mask = 0;
    if (syscall4(SYS_RT_SIGACTION, SIGHUP, (long)&act, 0, 8) != 0) {
        fail("ignoring SIGHUP");
    }
    for (;;) {
        unsigned long base = mmap_anon(1u << 20);
        unsigned off;
        int refused = 0;

        if (sys_failed((long)base)) {
            break; /* no room for the region: the reserve reached another way */
        }
        for (off = 0; off < (1u << 20); off += PAGE) {
            int r = syscall3(SYS_READ, zero, (long)(base + off), 16);

            if (r == -EFAULT) {
                refused = 1;
                break;
            }
            if (r != 16) {
                fail("a read of /dev/zero into a fresh page");
            }
            taken++;
        }
        if (refused) {
            break;
        }
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        sys_exit(0);
    }
    if (pid != -ENOMEM) {
        if (pid > 0) {
            wait_status(pid);
        }
        fail("the fork at the reserve was not refused with ENOMEM");
    }
    puts("MEMPROBE HOG: fork refused with ENOMEM");
    for (;;) {
        long ts[2];

        ts[0] = 1;
        ts[1] = 0;
        syscall2(SYS_NANOSLEEP, (long)ts, 0);
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    unsigned mib = argc >= 2 ? parse_mib(argv[1]) : 64;
    unsigned long len, base, pages, i, before, after;
    int pid, status;

    if (argc >= 2 && argv[1][0] == 'f' && argv[1][1] == 'r') {
        run_fresh();
    }
    if (argc >= 2 && argv[1][0] == 'h') {
        run_hog();
    }
    if (argc >= 2 && argv[1][0] == 't') {
        run_total();
    }
    if (argc >= 2 && argv[1][0] == 'f') {
        run_fail();
    }
    if (mib == 0) {
        fail("a size in MiB");
    }
    len = (unsigned long)mib << 20;
    pages = len / PAGE;
    before = mem_free_kib();
    if (before == 0) {
        fail("MemFree");
    }
    base = mmap_anon(len);
    if (sys_failed((long)base)) {
        fail("the mapping");
    }
    for (i = 0; i < pages; i++) {
        write_page((unsigned *)(base + i * PAGE), (unsigned)i);
    }
    for (i = 0; i < pages; i++) {
        if (!check_page((const unsigned *)(base + i * PAGE), (unsigned)i)) {
            fail("a page did not read back what was written");
        }
    }

    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork with the frames below the window in use");
    }
    if (pid == 0) {
        for (i = 0; i < pages; i += 4) {
            write_page((unsigned *)(base + i * PAGE), (unsigned)i + 0x100000u);
        }
        for (i = 0; i < pages; i++) {
            unsigned want = (i % 4 == 0) ? (unsigned)i + 0x100000u : (unsigned)i;

            if (!check_page((const unsigned *)(base + i * PAGE), want)) {
                sys_exit(4);
            }
        }
        sys_exit(0);
    }
    status = wait_status(pid);
    if (status != 0) {
        fail("the child's copies");
    }
    for (i = 0; i < pages; i++) {
        if (!check_page((const unsigned *)(base + i * PAGE), (unsigned)i)) {
            fail("a page changed under the parent when the child wrote its copy");
        }
    }

    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork for exec");
    }
    if (pid == 0) {
        char *args[3];

        args[0] = (char *)"memprobe";
        args[1] = (char *)"fresh";
        args[2] = 0;
        syscall3(SYS_EXECVE, (long)"/memprobe", (long)args, 0);
        sys_exit(5);
    }
    status = wait_status(pid);
    if (status != 0) {
        fail("a fresh image did not see zeroed pages");
    }

    if (syscall2(SYS_MUNMAP, (long)base, (long)len) != 0) {
        fail("munmap");
    }
    /* The page tables of the mapping stay with the process until it exits
     * (TABLE_KIB); everything else must be back. */
    after = mem_free_kib();
    if (after + 256 + TABLE_KIB(pages) < before) {
        fail("the frames did not come back");
    }
    puts("MEMPROBE OK");
    sys_exit(0);
}
