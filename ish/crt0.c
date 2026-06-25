/*
 * C runtime startup for Rach2 user programs: the `_start` entry, with no
 * libc to set up the stack first.
 */

#include "ish_syscalls.h"

int main(int argc, char **argv, char **envp);

/**
 * Freestanding ELF entry: read `argc`, `argv`, and `envp` off the initial
 * stack, run `main`, and exit with its return value.
 *
 * The kernel's `execve` hands a new process a stack whose top words are
 * `argc`, then the `argc` pointers of `argv`, a NULL terminator, the `envp`
 * pointers, and a final NULL. There is no libc startup, so this entry reads
 * that layout directly. The compiler's prologue (`push %ebp; mov %esp,%ebp`)
 * has already moved ESP, so the words are addressed EBP-relative: `argc` at
 * `EBP+4`, just above the saved EBP, and `argv` at `EBP+8`. `envp` follows
 * `argv` past its NULL terminator, at `argv + argc + 1`.
 */
void _start(void)
{
    int argc;
    char **argv;
    char **envp;
    int ret;

    __asm__ __volatile__ (
        "movl 4(%%ebp), %0"
        : "=r"(argc)
    );

    __asm__ __volatile__ (
        "leal 8(%%ebp), %0"
        : "=r"(argv)
    );

    envp = argv + argc + 1;

    ret = main(argc, argv, envp);

    ish_exit(ret);
}
