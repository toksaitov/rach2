/**
 * Probes that a path is made canonical before the kernel finds the mount it
 * is on.
 *
 * The kernel collapses the '..' of a path once, as it copies the path in,
 * and finds the mount from the canonical path. A kernel that found the
 * mount from the path as written and collapsed '..' afterwards would take
 * /x/../proc for a path of the KRFFS root and resolve it to /proc, so mkdir
 * could make a KRFFS /proc that hides the synthetic one. The probe makes a
 * directory, then checks that a '..' path through it onto /proc and /dev is
 * refused, and that a '..' path that comes out on a device or a directory
 * still reaches it.
 *
 * Prints "PATHCANON OK". Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define O_RDONLY 0
#define O_RDWR   2

#define EACCES 13

/** exit(2) with `status`; does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
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
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"\n", 1);
}

/** Print "PATHCANON FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"PATHCANON FAIL: ", 16);
    puts(why);
    sys_exit(1);
}

/**
 * Run the cases in order and fail at the first that does not hold: mkdir
 * of /cx/../proc and of /cx/../dev fails; /cx/../dev/null opens the device
 * and takes a write; /cx/../cx opens the directory; and execve of
 * /cx/../dev/null fails with -EACCES, the answer for a path on a synthetic
 * mount, where a path that reached the KRFFS root would fail with -ENOENT.
 * The refusal is what keeps a file an image placed under a synthetic prefix
 * on the KRFFS root from being run.
 */
PROBE_START void _start(void)
{
    int fd;

    if (syscall2(SYS_MKDIR, (long)"/cx", 0755) != 0) {
        fail("mkdir /cx");
    }

    if (syscall2(SYS_MKDIR, (long)"/cx/../proc", 0755) == 0) {
        fail("mkdir /cx/../proc created a KRFFS shadow of synthetic /proc");
    }
    if (syscall2(SYS_MKDIR, (long)"/cx/../dev", 0755) == 0) {
        fail("mkdir /cx/../dev created a KRFFS shadow of synthetic /dev");
    }

    fd = syscall3(SYS_OPEN, (long)"/cx/../dev/null", O_RDWR, 0);
    if (fd < 0) {
        fail("/cx/../dev/null did not reach the device");
    }
    if (syscall3(SYS_WRITE, fd, (long)"x", 1) != 1) {
        fail("write to /dev/null via a '..' path");
    }
    syscall2(SYS_CLOSE, fd, 0);

    fd = syscall3(SYS_OPEN, (long)"/cx/../cx", O_RDONLY, 0);
    if (fd < 0) {
        fail("/cx/../cx did not resolve to the real directory");
    }
    syscall2(SYS_CLOSE, fd, 0);

    {
        char *av[1];
        av[0] = 0;
        if (syscall3(SYS_EXECVE, (long)"/cx/../dev/null", (long)av, (long)av) != -EACCES) {
            fail("execve of a synthetic path was not refused with EACCES");
        }
    }

    puts("PATHCANON OK");
    sys_exit(0);
}
