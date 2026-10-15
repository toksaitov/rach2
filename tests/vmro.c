/**
 * Probes that a write to a page mapped PROT_READ alone faults.
 *
 * Maps an anonymous page with PROT_READ alone, reads it, which succeeds and
 * reads zero, then writes to it, which faults as a protection violation,
 * since the page fault handler maps the page with the region's protection.
 * The process dies with SIGSEGV and the kernel returns to its shell; the
 * "still alive" line appears only when the write did not fault. vmfault is
 * the companion case, an address in no region at all.
 *
 * `tests/run fault` runs it with vmfault and winprobe in one boot.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define PAGE 4096u

#define PROT_READ     0x1
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

/** write(2): the bytes written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** exit(2) with `status`; does not return. */
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
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/** Map a page PROT_READ, read it, then write it; the write ends the
 * process, and exit 1 reports a failed map or a page that is not zero. */
PROBE_START void _start(void)
{
    long p = mmap_anon(PAGE, PROT_READ);
    volatile unsigned char *ro = (volatile unsigned char *)p;

    if (sys_failed(p)) {
        puts("vmro: ERROR mmap PROT_READ failed");
        sys_exit(1);
    }

    if (ro[0] != 0) {
        puts("vmro: ERROR read-only page did not read zero");
        sys_exit(1);
    }

    puts("vmro: about to write a read-only page");

    ro[0] = 0xFF;

    puts("vmro: ERROR still alive after writing read-only page");
    sys_exit(0);
}
