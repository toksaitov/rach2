/**
 * Probes that a write through a wild pointer kills the process and leaves
 * the kernel running.
 *
 * The write targets an unmapped gap of the address space, which no region
 * covers and the page fault handler treats as an illegal access: it kills
 * the process with SIGSEGV and returns to the shell. The probe prints a
 * line before the fault; the "still alive" line after it appears only when
 * the write did not fault.
 *
 * `tests/run fault` runs it with vmro and winprobe in one boot, requires
 * "killed by signal 11" after each, and then runs hello, which a kernel the
 * fault took down cannot run. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

/* An address in the hole between the program image and heap and the mmap
 * and stack regions: no region covers it, so a touch faults and is not
 * paged in. */
#define WILD_ADDR 0x40000000u

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

/** Print a line, then store to WILD_ADDR; the store ends the process. */
PROBE_START void _start(void)
{
    volatile unsigned *wild = (volatile unsigned *)WILD_ADDR;

    puts("vmfault: about to dereference a wild pointer");

    *wild = 0x1234; /* no region covers it: SIGSEGV */

    puts("vmfault: ERROR still alive after wild write");
    sys_exit(0);
}
