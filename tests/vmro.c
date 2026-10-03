/**
 * Probes that a PROT_READ-only page faults on write.
 *
 * Maps an anonymous page with PROT_READ only, reads it (must succeed and read
 * zero), then writes to it, which must fault as a protection violation, since
 * the demand-paging handler honors the VMA's protection in the PTE. The
 * process must die and the kernel return to its shell; the "still alive" line
 * must NEVER appear. Companion to vmfault (an address in no VMA at all).
 *
 * Run as a standalone boot; the harness then checks the kernel survived.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define PAGE 4096u

#define PROT_READ     0x1
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

/** An anonymous mapping of `len` bytes with `prot`; its address, or a
 * negated errno. */
static long mmap_anon(long len, int prot)
{
    return probe_mmap(0, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

PROBE_START void _start(void)
{
    long p = mmap_anon(PAGE, PROT_READ);
    volatile unsigned char *ro = (volatile unsigned char *)p;

    if (sys_failed(p)) {
        puts("vmro: ERROR mmap PROT_READ failed");
        sys_exit(1);
    }

    /* A read is allowed and the lazy page reads zero. */
    if (ro[0] != 0) {
        puts("vmro: ERROR read-only page did not read zero");
        sys_exit(1);
    }

    puts("vmro: about to write a read-only page");

    ro[0] = 0xFF; /* protection violation: the kernel must kill us */

    puts("vmro: ERROR still alive after writing read-only page");
    sys_exit(0);
}
