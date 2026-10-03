/**
 * Minimal ELF probe: prints "Hello from ELF!" and exits.
 *
 * Exercises the ELF loader and the write and exit syscalls. Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */

static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/**
 * ELF entry point: the address the loader jumps to (no libc, no main).
 *
 * The stack on entry has argc at `[ESP+0]`, then the argv pointers; this
 * probe ignores them and prints a fixed message.
 */
PROBE_START void _start(void)
{
    puts("Hello from ELF!");
    sys_exit(0);
}
