/**
 * Probe that fork copies a read-only page with its protection intact.
 *
 * The parent maps an anonymous PROT_READ page, faults it in (reads zero), then
 * forks. The child writes to that read-only page: because fork must copy the
 * page into the child with the SAME (read-only) protection, the write is a
 * protection violation and the kernel kills the child. The parent reaps it
 * and checks the wait status is a SIGSEGV death (WIFSIGNALED and WTERMSIG
 * == SIGSEGV). If fork wrongly made the child's copy writable, the child
 * would survive and exit 0, which this probe reports as a failure.
 *
 * Freestanding; direct system calls (probe.h).
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

static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
}

static unsigned sys_mmap2_anon(unsigned len, int prot)
{
    unsigned result;
    __asm__ __volatile__("push %%ebp\n\t"
                         "xor %%ebp, %%ebp\n\t"
                         "int $0x80\n\t"
                         "pop %%ebp\n\t"
                         : "=a"(result)
                         : "0"(SYS_MMAP2), "b"(0), "c"(len), "d"(prot),
                           "S"(MAP_PRIVATE | MAP_ANONYMOUS), "D"(-1)
                         : "memory");
    return result;
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
    unsigned p = sys_mmap2_anon(PAGE, PROT_READ);
    volatile unsigned char *ro = (volatile unsigned char *)p;
    int pid, status;

    if (p >= 0xFFFFF001u) {
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
        /* Child: writing the inherited read-only page must fault and kill it.
         */
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
