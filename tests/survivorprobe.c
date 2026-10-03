/**
 * An init that exits with a runnable child returns to the debug shell.
 *
 * The probe forks a child that runs in user mode until the kernel ends it,
 * and exits while the child runs. The kernel ends the child and returns
 * to the shell: an exit that scheduled the child instead ran it and never
 * returned, since the exited init, a zombie, was never picked again, and a
 * return that left the child frozen would run it under the next program.
 *
 * Prints "SURVIVORPROBE OK" before exiting. The verdict is the kernel's
 * return record, which the runner requires after the probe's own exit.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void print(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)slen(s));
}

PROBE_START void _start(void)
{
    int pid = syscall1(SYS_FORK, 0);

    if (pid < 0) {
        print("SURVIVORPROBE FAIL: fork\n");
        syscall1(SYS_EXIT, 1);
    }
    if (pid == 0) {
        for (;;) {
            __asm__ __volatile__("" ::: "memory"); /* a spin, meant to last */
        }
    }
    print("SURVIVORPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
