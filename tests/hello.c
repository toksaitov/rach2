/**
 * Minimal ELF probe: prints "Hello from ELF!" and exits.
 *
 * Exercises the ELF loader and the write and exit syscalls. Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"

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

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/**
 * The program's entry, where the kernel starts it with the stack pointer at
 * argc and the argv pointers above it; this probe reads neither, prints a
 * fixed line and exits 0.
 */
PROBE_START void _start(void)
{
    puts("Hello from ELF!");
    sys_exit(0);
}
