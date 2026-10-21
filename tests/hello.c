/**
 * Minimal ELF probe: prints "Hello from ELF!" and exits.
 *
 * Exercises the ELF loader and the write and exit syscalls. Writes etude's
 * report records (probe_report.h) for the case ETUDE_CASE names, with one
 * assertion, `write`: the write of the line returned its length.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

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

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

/** Write `s` and a newline to standard output; 1 when each write wrote all
 * its bytes. */
static int puts(const char *s)
{
    int n = (int)strlen(s);

    return (sys_write(STDOUT_FILENO, s, n) == n) & (sys_write(STDOUT_FILENO, "\n", 1) == 1);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The program's entry, which `_start` calls with the initial stack: it reads
 * the environment for the case's identity, prints a fixed line and exits 0.
 */
void probe_main(unsigned long *sp)
{
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.hello"));
    if (puts("Hello from ELF!")) {
        report_pass("write");
    } else {
        report_fail("write", "the write of the line fell short");
    }
    report_end();
    sys_exit(0);
}
