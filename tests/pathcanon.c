/**
 * Probes that paths are canonicalized before the synthetic-mount classifier.
 *
 * The classifier decides /dev vs /proc vs KRFFS lexically, while the KRFFS
 * resolver collapses '..'. If the two disagree, a spelling like /x/../proc
 * classifies as KRFFS and then resolves to /proc, so mkdir could create a real
 * KRFFS /proc that shadows the synthetic one. The kernel must canonicalize the
 * absolute path once, before classification, so such a path is recognized as the
 * synthetic mount and refused. The probe makes a real directory, then checks a
 * '..' path through it onto /proc and /dev is refused, and that a '..' path that
 * canonicalizes onto a real device or directory still works.
 *
 * Prints "PATHCANON OK". Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT   1
#define SYS_WRITE  4
#define SYS_OPEN   5
#define SYS_CLOSE  6
#define SYS_EXECVE 11
#define SYS_MKDIR  39

#define O_RDONLY 0
#define O_RDWR   2

#define EACCES 13

static int sys2(int n, int a, int b)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
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
    sys3(SYS_WRITE, STDOUT_FILENO, (int)s, (int)strlen(s));
    sys3(SYS_WRITE, STDOUT_FILENO, (int)"\n", 1);
}

static void fail(const char *why)
{
    sys3(SYS_WRITE, STDOUT_FILENO, (int)"PATHCANON FAIL: ", 16);
    puts(why);
    sys_exit(1);
}

void _start(void)
{
    int fd;

    if (sys2(SYS_MKDIR, (int)"/cx", 0755) != 0) {
        fail("mkdir /cx");
    }

    /* A '..' path that canonicalizes onto a synthetic mount must be refused, not
     * create a shadowing KRFFS node. */
    if (sys2(SYS_MKDIR, (int)"/cx/../proc", 0755) == 0) {
        fail("mkdir /cx/../proc created a KRFFS shadow of synthetic /proc");
    }
    if (sys2(SYS_MKDIR, (int)"/cx/../dev", 0755) == 0) {
        fail("mkdir /cx/../dev created a KRFFS shadow of synthetic /dev");
    }

    /* A '..' path that canonicalizes onto a real device still reaches it. */
    fd = sys3(SYS_OPEN, (int)"/cx/../dev/null", O_RDWR, 0);
    if (fd < 0) {
        fail("/cx/../dev/null did not reach the device");
    }
    if (sys3(SYS_WRITE, fd, (int)"x", 1) != 1) {
        fail("write to /dev/null via a '..' path");
    }
    sys2(SYS_CLOSE, fd, 0);

    /* And one that canonicalizes back onto a real directory opens it. */
    fd = sys3(SYS_OPEN, (int)"/cx/../cx", O_RDONLY, 0);
    if (fd < 0) {
        fail("/cx/../cx did not resolve to the real directory");
    }
    sys2(SYS_CLOSE, fd, 0);

    /* execve classifies the canonical path like open: a synthetic path is
     * refused with -EACCES before it can reach KRFFS, so a file a hostile image
     * placed below the synthetic prefix could never be executed. Reaching KRFFS
     * instead would fail with -ENOENT, so the errno distinguishes the guard. */
    {
        char *av[1];
        av[0] = 0;
        if (sys3(SYS_EXECVE, (int)"/cx/../dev/null", (int)av, (int)av) != -EACCES) {
            fail("execve of a synthetic path was not refused with EACCES");
        }
    }

    puts("PATHCANON OK");
    sys_exit(0);
}
