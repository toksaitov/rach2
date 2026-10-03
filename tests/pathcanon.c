/**
 * Probes that paths are canonicalized before the synthetic-mount classifier.
 *
 * The classifier decides /dev vs /proc vs KRFFS lexically, while the KRFFS
 * resolver collapses '..'. If the two disagree, a spelling like /x/../proc
 * classifies as KRFFS and then resolves to /proc, so mkdir could create a real
 * KRFFS /proc that shadows the synthetic one. The kernel must canonicalize the
 * absolute path once, before classification, so such a path is recognized as
 * the synthetic mount and refused. The probe makes a real directory, then
 * checks a '..' path through it onto /proc and /dev is refused, and that a '..'
 * path that canonicalizes onto a real device or directory still works.
 *
 * Prints "PATHCANON OK". Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define O_RDONLY 0
#define O_RDWR   2

#define EACCES 13

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
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
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"\n", 1);
}

static void fail(const char *why)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"PATHCANON FAIL: ", 16);
    puts(why);
    sys_exit(1);
}

PROBE_START void _start(void)
{
    int fd;

    if (syscall2(SYS_MKDIR, (long)"/cx", 0755) != 0) {
        fail("mkdir /cx");
    }

    /* A '..' path that canonicalizes onto a synthetic mount must be refused,
     * not create a shadowing KRFFS node. */
    if (syscall2(SYS_MKDIR, (long)"/cx/../proc", 0755) == 0) {
        fail("mkdir /cx/../proc created a KRFFS shadow of synthetic /proc");
    }
    if (syscall2(SYS_MKDIR, (long)"/cx/../dev", 0755) == 0) {
        fail("mkdir /cx/../dev created a KRFFS shadow of synthetic /dev");
    }

    /* A '..' path that canonicalizes onto a real device still reaches it. */
    fd = syscall3(SYS_OPEN, (long)"/cx/../dev/null", O_RDWR, 0);
    if (fd < 0) {
        fail("/cx/../dev/null did not reach the device");
    }
    if (syscall3(SYS_WRITE, fd, (long)"x", 1) != 1) {
        fail("write to /dev/null via a '..' path");
    }
    syscall2(SYS_CLOSE, fd, 0);

    /* And one that canonicalizes back onto a real directory opens it. */
    fd = syscall3(SYS_OPEN, (long)"/cx/../cx", O_RDONLY, 0);
    if (fd < 0) {
        fail("/cx/../cx did not resolve to the real directory");
    }
    syscall2(SYS_CLOSE, fd, 0);

    /* execve classifies the canonical path like open: a synthetic path is
     * refused with -EACCES before it can reach KRFFS, so a file a hostile image
     * placed below the synthetic prefix could never be executed. Reaching KRFFS
     * instead would fail with -ENOENT, so the errno distinguishes the guard. */
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
