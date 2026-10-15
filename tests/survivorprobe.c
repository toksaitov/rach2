/**
 * An init that exits with a runnable child returns to the debug shell.
 *
 * The probe forks a child that runs in user mode until the kernel ends it,
 * and exits while the child runs. The kernel ends the child and returns to
 * the shell. A kernel that scheduled the child at the exit would run it and
 * never return, since the exited init, a zombie, is not picked again; one
 * that returned and left the child frozen would run it under the next
 * program.
 *
 * Prints "SURVIVORPROBE OK" before exiting. The verdict is the line the
 * kernel prints as it returns to the shell, `sys_exit: init process N exited
 * with status 0`, which runtests requires after the probe's own output.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

/** The length of the string `s`. */
static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/** Write the string `s` to standard output. */
static void print(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)slen(s));
}

/** Fork a child that spins in user mode, and exit 0 while it runs. */
PROBE_START void _start(void)
{
    int pid = syscall1(SYS_FORK, 0);

    if (pid < 0) {
        print("SURVIVORPROBE FAIL: fork\n");
        syscall1(SYS_EXIT, 1);
    }
    if (pid == 0) {
        for (;;) {
            __asm__ __volatile__("" ::: "memory");
        }
    }
    print("SURVIVORPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
