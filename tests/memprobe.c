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
 * when the frames below the window are already in use.
 *
 * `memprobe fail`, under `rach2.framefail`: with a patterned mapping of its
 * own, the probe arms the count-th frame allocation to fail for each count
 * from one to thirty-two, and forks; then again, and forks a child that
 * execs `memprobe fresh`. A fork or an exec that meets the failure must
 * answer ENOMEM, or the child must exit as it would without it, and after
 * every attempt the parent's pages read as it wrote them, take a write, and
 * the free memory is what it was, so nothing leaked and nothing that
 * survived was changed.
 *
 * `memprobe hog`: the probe takes anonymous memory a page at a time, each
 * page backed through a `read` of `/dev/zero` into it, until the kernel
 * refuses a page, which it does with EFAULT from the read once the free
 * memory is inside the reserve kept for root; then a fork must be refused
 * with ENOMEM, the probe says so, and it sleeps holding what it took until
 * it is killed, ignoring the hangup its shell's exit sends. What the row
 * then shows is that root can still log in beside it.
 *
 * Prints "MEMPROBE OK" or "MEMPROBE FAIL: reason". Freestanding; int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT    1
#define SYS_FORK    2
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_WAITPID 7
#define SYS_EXECVE  11
#define SYS_MUNMAP  91
#define SYS_MMAP2   192

#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define O_RDONLY      0
#define O_WRONLY      1
#define ENOMEM        12

#define PAGE 4096u

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

static unsigned sys_mmap2_anon(unsigned len)
{
    unsigned result;
    __asm__ __volatile__("push %%ebp\n\t"
                         "xor %%ebp, %%ebp\n\t"
                         "int $0x80\n\t"
                         "pop %%ebp\n\t"
                         : "=a"(result)
                         : "0"(SYS_MMAP2), "b"(0), "c"(len), "d"(PROT_READ | PROT_WRITE),
                           "S"(MAP_PRIVATE | MAP_ANONYMOUS), "D"(-1)
                         : "memory");
    return result;
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

static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (int)(size_t)"MEMPROBE FAIL: ", 15);
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

/* MemFree from /proc/meminfo, in KiB, or 0. */
static unsigned mem_free_kib(void)
{
    static char buf[1024];
    int fd = syscall3(SYS_OPEN, (int)(size_t)"/proc/meminfo", O_RDONLY, 0);
    int n, i;
    const char *key = "MemFree:";

    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_READ, fd, (int)(size_t)buf, (int)sizeof(buf) - 1);
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
            unsigned v = 0;

            i += k;
            while (buf[i] == ' ') {
                i++;
            }
            while (buf[i] >= '0' && buf[i] <= '9') {
                v = v * 10 + (unsigned)(buf[i] - '0');
                i++;
            }
            return v;
        }
    }
    return 0;
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

    if (syscall3(SYS_WAITPID, pid, (int)(size_t)&status, 0) != pid) {
        return -1;
    }
    return status;
}

/* `memprobe fresh`: 16 MiB mapped, every word zero. */
static void run_fresh(void)
{
    unsigned len = 16u << 20;
    unsigned base = sys_mmap2_anon(len);
    unsigned i, w;

    if (base >= 0xFFFFF000u) {
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

static void arm(unsigned n)
{
    char digits[12];
    int i = 0, fd;
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
    fd = syscall3(SYS_OPEN, (int)(size_t)"/dev/framefail", O_WRONLY, 0);
    if (fd < 0 || syscall3(SYS_WRITE, fd, (int)(size_t)digits, i) != i) {
        fail("rach2.framefail is not on this boot's line");
    }
    syscall1(SYS_CLOSE, fd);
}

/* The parent's own pages after an attempt: as written, then rewritten. */
static void survivors(unsigned base, unsigned pages, unsigned round)
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
    unsigned pages = 256, base = sys_mmap2_anon(pages * PAGE);
    unsigned baseline, n, i, round = 0;

    if (base >= 0xFFFFF000u) {
        fail("the mapping");
    }
    for (i = 0; i < pages; i++) {
        write_page((unsigned *)(base + i * PAGE), i);
    }
    baseline = mem_free_kib();
    for (n = 1; n <= 32; n++) {
        int pid, status;
        unsigned free_now;

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
            status = syscall3(SYS_EXECVE, (int)(size_t)"/memprobe", (int)(size_t)args, 0);
            sys_exit(status == -ENOMEM ? 7 : 5);
        }
        if (pid < 0) {
            fail("the fork before an exec failed");
        }
        if (pid > 0) {
            status = wait_status(pid);
            if (status != 0 && status != (7 << 8)) {
                fail("an exec that met the failure neither ran nor answered ENOMEM");
            }
        }
        survivors(base, pages, round++);
        free_now = mem_free_kib();
        if (free_now + 64 < baseline || free_now > baseline + 64) {
            fail("memory leaked or was freed twice around an exec");
        }
    }
    puts("MEMPROBE OK");
    sys_exit(0);
}

#define SYS_NANOSLEEP    162
#define SYS_RT_SIGACTION 174
#define SIGHUP           1
#define EFAULT           14

static void run_hog(void)
{
    int zero = syscall3(SYS_OPEN, (int)(size_t)"/dev/zero", 0, 0);
    unsigned act[5];
    unsigned taken = 0;
    int pid;

    if (zero < 0) {
        fail("open /dev/zero");
    }
    act[0] = 1; /* SIG_IGN */
    act[1] = 0;
    act[2] = 0;
    act[3] = 0;
    act[4] = 0;
    if (syscall4(SYS_RT_SIGACTION, SIGHUP, (int)(size_t)act, 0, 8) != 0) {
        fail("ignoring SIGHUP");
    }
    for (;;) {
        unsigned base = sys_mmap2_anon(1u << 20);
        unsigned off;
        int refused = 0;

        if (base >= 0xFFFFF000u) {
            break; /* no room for the region: the reserve reached another way */
        }
        for (off = 0; off < (1u << 20); off += PAGE) {
            int r = syscall3(SYS_READ, zero, (int)(base + off), 16);

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
        int ts[2];

        ts[0] = 1;
        ts[1] = 0;
        syscall2(SYS_NANOSLEEP, (int)(size_t)ts, 0);
    }
}

void probe_main(unsigned int *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned int *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    unsigned mib = argc >= 2 ? parse_mib(argv[1]) : 64;
    unsigned len, base, pages, i, before, after;
    int pid, status;

    if (argc >= 2 && argv[1][0] == 'f' && argv[1][1] == 'r') {
        run_fresh();
    }
    if (argc >= 2 && argv[1][0] == 'h') {
        run_hog();
    }
    if (argc >= 2 && argv[1][0] == 'f') {
        run_fail();
    }
    if (mib == 0) {
        fail("a size in MiB");
    }
    len = mib << 20;
    pages = len / PAGE;
    before = mem_free_kib();
    if (before == 0) {
        fail("MemFree");
    }
    base = sys_mmap2_anon(len);
    if (base >= 0xFFFFF000u) {
        fail("the mapping");
    }
    for (i = 0; i < pages; i++) {
        write_page((unsigned *)(base + i * PAGE), i);
    }
    for (i = 0; i < pages; i++) {
        if (!check_page((const unsigned *)(base + i * PAGE), i)) {
            fail("a page did not read back what was written");
        }
    }

    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork with the frames below the window in use");
    }
    if (pid == 0) {
        for (i = 0; i < pages; i += 4) {
            write_page((unsigned *)(base + i * PAGE), i + 0x100000u);
        }
        for (i = 0; i < pages; i++) {
            unsigned want = (i % 4 == 0) ? i + 0x100000u : i;

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
        if (!check_page((const unsigned *)(base + i * PAGE), i)) {
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
        syscall3(SYS_EXECVE, (int)(size_t)"/memprobe", (int)(size_t)args, 0);
        sys_exit(5);
    }
    status = wait_status(pid);
    if (status != 0) {
        fail("a fresh image did not see zeroed pages");
    }

    if (syscall2(SYS_MUNMAP, (int)base, (int)len) != 0) {
        fail("munmap");
    }
    /* The page tables of the mapping stay with the process until it exits,
     * one page per 4 MiB; everything else must be back. */
    after = mem_free_kib();
    if (after + 256 + (pages / 1024 + 1) * 4 < before) {
        fail("the frames did not come back");
    }
    puts("MEMPROBE OK");
    sys_exit(0);
}
