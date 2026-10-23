/**
 * Minimal ELF probe: prints "Hello from ELF!" and exits 0.
 *
 * Other probes, suites and host tests run it as a plain program or open its
 * file: shelltest execs it, the exitline and etude-guest cases run it, and
 * fdfull and limitprobe open it. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

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

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The program's entry, which `_start` calls with the initial stack. */
void probe_main(unsigned long *sp)
{
    static const char line[] = "Hello from ELF!\n";

    (void)sp;
    sys_write(STDOUT_FILENO, line, sizeof(line) - 1);
    sys_exit(0);
}
