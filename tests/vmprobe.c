/**
 * Exercises the VMA and demand-paging machinery end to end from user space:
 * brk() (query, grow, lazy-zero, read/write, shrink), anonymous mmap() (map,
 * lazy-zero, read/write, munmap), brk refusing to grow over an mmap region, and
 * stack growth by deep recursion that faults pages well past the initial one.
 *
 * Prints "VMPROBE OK" and exits 0 when every check passes, otherwise the failed
 * count. The wild-pointer/segfault case is a separate program (vmfault) since
 * it deliberately kills the process.
 *
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define PAGE 4096u

/* mmap prot/flags (Linux's, on i386 and x86-64). */
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

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

static int sys_munmap(unsigned long addr, unsigned long len)
{
    return syscall2(SYS_MUNMAP, (long)addr, (long)len);
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
    sys_write(STDOUT_FILENO, s, strlen(s));
}

static void puts(const char *s)
{
    print(s);
    sys_write(STDOUT_FILENO, "\n", 1);
}

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

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        print("  FAIL: ");
        puts(msg);
    }
}

/**
 * Recurse 'n' deep with a 1 KB buffer per frame, touching every byte so each
 * new stack page must fault in. Crucially the buffer is re-checked AFTER the
 * deeper call returns, so a deeper stack fault that corrupted this frame would
 * be caught on the way back up. Returns 1 + 2 + ... + n if every frame stayed
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

    deeper = recurse(n - 1); /* faults in more stack pages below us */

    /* After unwinding, our frame's buffer must be exactly as we left it. */
    for (i = 0; i < 1024; i++) {
        if (buf[i] != (char)(n + i)) {
            return -100000;
        }
    }
    return n + deeper;
}

PROBE_START void _start(void)
{
    puts("vmprobe: start");

    /* 1. brk: query, grow four pages, lazy-zero, read/write, shrink. */
    {
        unsigned long base = sys_brk(0);
        unsigned long want = base + 4 * PAGE;
        unsigned long got = sys_brk(want);
        volatile unsigned char *heap = (volatile unsigned char *)base;
        unsigned i;

        check(got == want, "brk grow returns new break");
        check(heap[0] == 0 && heap[PAGE] == 0 && heap[3 * PAGE + 17] == 0,
              "fresh heap pages read zero");
        for (i = 0; i < 4 * PAGE; i += 137) {
            heap[i] = (unsigned char)(i & 0xFF);
        }
        for (i = 0; i < 4 * PAGE; i += 137) {
            check(heap[i] == (unsigned char)(i & 0xFF), "heap read-back matches");
        }
        check(sys_brk(base) == base, "brk shrink returns base");
    }

    /* 2. mmap anonymous: map eight pages, lazy-zero, read/write, munmap. */
    {
        unsigned len = 8 * PAGE;
        unsigned long p = mmap_anon(len, PROT_READ | PROT_WRITE);
        volatile unsigned char *m = (volatile unsigned char *)p;
        unsigned i;

        check(!sys_failed((long)p), "mmap returns an address, not -errno");
        if (!sys_failed((long)p)) {
            check(m[0] == 0 && m[5 * PAGE] == 0 && m[len - 1] == 0, "fresh mmap pages read zero");
            for (i = 0; i < len; i += 251) {
                m[i] = (unsigned char)((i * 7) & 0xFF);
            }
            for (i = 0; i < len; i += 251) {
                check(m[i] == (unsigned char)((i * 7) & 0xFF), "mmap read-back matches");
            }
            check(sys_munmap(p, len) == 0, "munmap succeeds");
        }
    }

    /* 3. brk must refuse to grow over an existing mmap region: map a page, then
     * try to push the break up past it. brk returns the (unchanged) old break
     * on refusal, never the requested value. */
    {
        unsigned long p = mmap_anon(PAGE, PROT_READ | PROT_WRITE);

        if (!sys_failed((long)p)) {
            unsigned long old = sys_brk(0);
            unsigned long got = sys_brk(p + PAGE); /* would overlap [p, p+PAGE) */

            check(got == old, "brk refuses to grow over an mmap region");
            check(got != p + PAGE, "brk did not honor the colliding request");
            sys_munmap(p, PAGE);
        }
    }

    /* 4. Stack growth via deep recursion (~64 KB, far past the initial
     * page). */
    check(recurse(64) == 64 * 65 / 2, "stack grew under deep recursion");

    if (fails == 0) {
        puts("VMPROBE OK");
    } else {
        print("VMPROBE FAIL count=");
        print_num(fails);
        puts("");
    }
    sys_exit(fails);
}
