/*
 * crt0.c - C runtime startup for Rach2 user programs
 *
 * Provides _start entry point that extracts argc/argv from the stack
 * (as set up by the kernel's execve), calls main(), and exits.
 */

#include "ish_syscalls.h"

/* Forward declaration of main */
int main(int argc, char **argv, char **envp);

/*
 * _start - ELF entry point
 *
 * Stack layout on entry (set up by kernel's execve):
 *   [ESP+0]          argc
 *   [ESP+4]          argv[0]
 *   [ESP+8]          argv[1]
 *   ...
 *   [ESP+4+argc*4]   NULL (argv terminator)
 *   [ESP+4+argc*4+4] NULL (envp terminator)
 *
 * We extract argc, compute argv pointer, and call main.
 */
void _start(void)
{
    int argc;
    char **argv;
    char **envp;
    int ret;

    /* Get argc from stack */
    __asm__ __volatile__ (
        "movl (%%esp), %0"
        : "=r"(argc)
    );

    /* argv is at ESP+4 */
    __asm__ __volatile__ (
        "leal 4(%%esp), %0"
        : "=r"(argv)
    );

    /* envp is after argv's NULL terminator */
    envp = argv + argc + 1;

    /* Call main */
    ret = main(argc, argv, envp);

    /* Exit with main's return value */
    ish_exit(ret);
}
