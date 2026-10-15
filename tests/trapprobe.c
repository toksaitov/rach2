/**
 * Probes that a CPU exception in user mode kills only the faulting process.
 *
 * Two children fault on purpose: one executes an invalid opcode (#UD), one
 * divides by zero (#DE). Every CPU exception but the page fault, the double
 * fault, the NMI and the machine check goes to the kernel's default
 * exception handler (rach2/sys/arch/x86/isr.c), which sends the faulting
 * process the signal its cause maps to (SIGILL for #UD, SIGFPE for #DE) and
 * leaves the parent and the kernel running. A handler that returned into the
 * faulting instruction would fault again without end. The probe reaps each
 * child and checks that the wait status has the expected signal.
 *
 * Prints "TRAPPROBE OK" and exits 0. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define SIGILL 4
#define SIGFPE 8

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

/** Execute ud2, the invalid opcode (#UD, vector 6). */
static void raise_invalid_opcode(void)
{
    __asm__ __volatile__("ud2");
}

/** Divide 1 by 0 unsigned (#DE, vector 0), in assembly, so that the
 * compiler can neither fold nor drop the division. */
static void raise_divide_error(void)
{
    __asm__ __volatile__("xorl %%ecx, %%ecx\n\t"
                         "xorl %%edx, %%edx\n\t"
                         "movl $1, %%eax\n\t"
                         "divl %%ecx"
                         :
                         :
                         : "eax", "ecx", "edx", "cc");
}

/**
 * Fork a child that runs `fault`, reap it, and return whether it was killed
 * by `expect_signo`; 0 also when fork or waitpid fails. The Linux wait
 * status has the terminating signal in its low seven bits.
 */
static int faulted_with(void (*fault)(void), int expect_signo)
{
    int pid, status;

    pid = sys_fork();
    if (pid == 0) {
        fault();
        sys_exit(0); /* reached only when `fault` did not fault */
    }
    if (pid < 0) {
        return 0;
    }

    status = -1;
    if (sys_waitpid(pid, &status, 0) < 0) {
        return 0;
    }
    return (status & 0x7f) == expect_signo;
}

/** Check #UD and then #DE; exit 1 or 2 for the first whose child did not
 * die of the expected signal. */
PROBE_START void _start(void)
{
    if (!faulted_with(raise_invalid_opcode, SIGILL)) {
        puts("trapprobe: #UD was not contained as SIGILL");
        sys_exit(1);
    }
    if (!faulted_with(raise_divide_error, SIGFPE)) {
        puts("trapprobe: #DE was not contained as SIGFPE");
        sys_exit(2);
    }

    puts("TRAPPROBE OK");
    sys_exit(0);
}
