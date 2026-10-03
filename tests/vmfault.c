/**
 * Probes that a write through a wild pointer is killed by the fault handler.
 *
 * The write targets an unmapped gap of the address space (no VMA covers it),
 * which the demand-paging fault handler must treat as an illegal access: it
 * kills the process and returns to the shell. The probe prints a line before
 * the fault; the "still alive" line must NEVER appear.
 *
 * Run as a standalone boot (it kills itself); the harness then checks that the
 * kernel survived and the shell came back. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

/* An address in the hole between the program image/heap and the mmap/stack
 * regions: no VMA covers it, so a touch must segfault, not fault in. */
#define WILD_ADDR 0x40000000u

static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
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
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

PROBE_START void _start(void)
{
    volatile unsigned *wild = (volatile unsigned *)WILD_ADDR;

    puts("vmfault: about to dereference a wild pointer");

    *wild = 0x1234; /* illegal: no VMA here -> the kernel must kill us */

    /* If demand paging wrongly backed the page, we would reach this line. */
    puts("vmfault: ERROR still alive after wild write");
    sys_exit(0);
}
