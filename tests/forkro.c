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
 * Prints "FORKRO OK". Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

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

/** Map a page PROT_READ, read it, fork, and require the child's write to
 * the page to kill it with SIGSEGV; exit 1 on any failure. */
PROBE_START void _start(void)
{
    long p = mmap_anon(PAGE, PROT_READ);
    volatile unsigned char *ro = (volatile unsigned char *)p;
    int pid, status;

    if (sys_failed(p)) {
        puts("forkro: ERROR mmap PROT_READ failed");
        sys_exit(1);
    }

    /* Fault the read-only page in (a read is allowed) before forking. */
    if (ro[0] != 0) {
        puts("forkro: ERROR read-only page did not read zero");
        sys_exit(1);
    }

    puts("forkro: forking; child will write the read-only page");

    pid = sys_fork();
    if (pid < 0) {
        puts("forkro: ERROR fork failed");
        sys_exit(1);
    }

    if (pid == 0) {
        /* The child's copy of the page is read-only too: SIGSEGV. */
        ro[0] = 0xFF;
        /* Reached only if fork wrongly gave the child a writable copy. */
        puts("forkro: ERROR child wrote a read-only page and survived");
        sys_exit(0);
    }

    /* Parent: the child must have been killed by the protection fault. */
    if (sys_waitpid(pid, &status, 0) != pid) {
        puts("forkro: ERROR waitpid failed");
        sys_exit(1);
    }

    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV) {
        puts("FORKRO OK");
        sys_exit(0);
    }
    puts("forkro: FAIL child was not killed by SIGSEGV on the read-only write");
    sys_exit(1);
}
