/**
 * Probe that fork copies a read-only page with its protection intact.
 *
 * The parent maps an anonymous PROT_READ page, faults it in (it reads zero),
 * then forks. The child writes to that page: fork gives the child's copy the
 * parent's protection, read-only, so the write is a protection violation
 * and the kernel kills the child. The parent reaps it and checks that the
 * wait status is a death by SIGSEGV (WIFSIGNALED and WTERMSIG == SIGSEGV).
 * A child whose copy fork made writable would survive and exit 0, which this
 * probe reports as a failure.
 *
 * Prints "FORKRO OK". Writes etude's report records (probe_report.h) for
 * the case ETUDE_CASE names, with the assertions `zero-read`, the mapping
 * and its read of zero, and `child-segv`, the child's death by SIGSEGV.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define STDOUT_FILENO 1

#define PAGE 4096u

#define PROT_READ     0x1
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

#define SIGSEGV 11 /* the signal a fault kill is reported as */

/* Linux wait-status decoding for a signal death (low 7 bits = the signal). */
#define WIFSIGNALED(s) (((s) & 0x7F) != 0 && ((s) & 0x7F) != 0x7F)
#define WTERMSIG(s)    ((s) & 0x7F)

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

/** fork(2): the child's pid, 0 in the child, or a negated errno. */
static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

/** waitpid(2): the pid reaped, or a negated errno. */
static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
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

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Map a page PROT_READ, read it, fork, and require the child's write to
 * the page to kill it with SIGSEGV; exit 1 on any failure. `_start` calls
 * it with the initial stack. */
void probe_main(unsigned long *sp)
{
    long p;
    volatile unsigned char *ro;
    int pid, status;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.forkro"));
    report_plan("zero-read");
    report_plan("child-segv");
    p = mmap_anon(PAGE, PROT_READ);
    ro = (volatile unsigned char *)p;
    if (sys_failed(p)) {
        puts("forkro: ERROR mmap PROT_READ failed");
        report_stop("mmap PROT_READ failed");
        sys_exit(1);
    }

    if (ro[0] != 0) {
        puts("forkro: ERROR read-only page did not read zero");
        report_stop("read-only page did not read zero");
        sys_exit(1);
    }
    report_held();

    puts("forkro: forking; child will write the read-only page");

    pid = sys_fork();
    if (pid < 0) {
        puts("forkro: ERROR fork failed");
        report_stop("fork failed");
        sys_exit(1);
    }

    if (pid == 0) {
        ro[0] = 0xFF;
        puts("forkro: ERROR child wrote a read-only page and survived");
        sys_exit(0);
    }

    if (sys_waitpid(pid, &status, 0) != pid) {
        puts("forkro: ERROR waitpid failed");
        report_stop("waitpid failed");
        sys_exit(1);
    }

    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV) {
        report_held();
        report_end();
        puts("FORKRO OK");
        sys_exit(0);
    }
    report_stop("child was not killed by SIGSEGV on the read-only write");
    puts("forkro: FAIL child was not killed by SIGSEGV on the read-only write");
    sys_exit(1);
}
