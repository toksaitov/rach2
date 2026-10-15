/**
 * winprobe: a user read of the page-cache window ends the process with
 * SIGSEGV and leaves the kernel running.
 *
 * `PCWIN_BASE` (0xD0000000 on i686, 0xFFFFC20000000000 on x86-64, the
 * `KVA_PCWIN` of each architecture's cpu.h) is the base of the kernel's
 * demand-paged disk window. Its pages are supervisor-only or not present, so
 * the read faults, and the page fault handler (rach2/sys/mm/paging.c) serves
 * a window fault only in supervisor mode, for the file systems; a user-mode
 * one gets SIGSEGV. A handler that served the user fault would map a
 * supervisor-only page, on which the read faults again without end, or would
 * panic when the disk cannot fill the page.
 *
 * Prints "winprobe: about to read the page-cache window" and dies on signal
 * 11. `tests/run fault` expects both, and then runs a program that prints,
 * which a kernel that went down with the fault cannot; the line the probe
 * prints when the read returns, "winprobe: ERROR still alive after window
 * read", is that suite's `--never` text. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#if defined __x86_64__
#define PCWIN_BASE 0xFFFFC20000000000ul
#else
#define PCWIN_BASE 0xD0000000u
#endif

/** write(2). */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** exit(2); does not return. */
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

/** Write the string `s` and a newline to standard output. */
static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/**
 * The probe's entry: one read at `PCWIN_BASE`, which ends the process. When
 * the read returns, prints the error line and exits 0.
 */
PROBE_START void _start(void)
{
    volatile unsigned *win = (volatile unsigned *)PCWIN_BASE;
    unsigned sink;

    puts("winprobe: about to read the page-cache window");

    sink = *win; /* a fault from ring 3: SIGSEGV */
    (void)sink;

    puts("winprobe: ERROR still alive after window read");
    sys_exit(0);
}
