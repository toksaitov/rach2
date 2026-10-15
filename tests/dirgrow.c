/**
 * dirgrow: a file keeps its directory when a write moves it.
 *
 * KRFFS grows a file in place into the free node after it, and when that
 * node is taken moves the file to a new allocation, copying its header and
 * data (`krffs_resize_at`). The copy must keep the file's parent_id, or the
 * moved file would leave its directory for the root and the tree on the disk
 * would be wrong. The probe makes /dg/f, then /dg/g right after it so that f
 * has no room beside it, grows f past its slack so that it moves, and checks
 * that f still resolves under /dg with its first bytes, and not as /f.
 *
 * Prints "DIRGROW OK" and exits 0, or "DIRGROW FAIL: " with the reason and
 * exits 1. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define O_RDONLY 0
#define O_RDWR   2
#define O_CREAT  0100

#define GROW 2048

/** exit(2); does not return. */
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

/** Write the string `s` and a newline to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"\n", 1);
}

/** Print "DIRGROW FAIL: `why`" and exit 1. */
static void fail(const char *why)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"DIRGROW FAIL: ", 14);
    puts(why);
    sys_exit(1);
}

static char big[GROW];

/** The probe's entry: make the two files, grow the first, and look for it. */
PROBE_START void _start(void)
{
    int fd_f, fd_g, fd;
    int i;
    char buf[4];

    if (syscall2(SYS_MKDIR, (long)"/dg", 0755) != 0) {
        fail("mkdir /dg");
    }

    fd_f = syscall3(SYS_OPEN, (long)"/dg/f", O_CREAT | O_RDWR, 0644);
    if (fd_f < 0) {
        fail("create /dg/f");
    }
    if (syscall3(SYS_WRITE, fd_f, (long)"FF", 2) != 2) {
        fail("write /dg/f");
    }

    /* A second file right after f leaves it no adjacent room to grow into. */
    fd_g = syscall3(SYS_OPEN, (long)"/dg/g", O_CREAT | O_RDWR, 0644);
    if (fd_g < 0) {
        fail("create /dg/g");
    }
    if (syscall3(SYS_WRITE, fd_g, (long)"GG", 2) != 2) {
        fail("write /dg/g");
    }

    /* Grow f past its slack: with g adjacent, this relocates f. */
    for (i = 0; i < GROW; i++) {
        big[i] = (char)('a' + (i & 31));
    }
    if (syscall3(SYS_WRITE, fd_f, (long)big, GROW) != GROW) {
        fail("grow /dg/f");
    }

    syscall2(SYS_CLOSE, fd_f, 0);
    syscall2(SYS_CLOSE, fd_g, 0);

    /* The moved f is still under /dg. */
    fd = syscall3(SYS_OPEN, (long)"/dg/f", O_RDONLY, 0);
    if (fd < 0) {
        fail("/dg/f vanished from its directory after a relocating grow");
    }
    if (syscall3(SYS_READ, fd, (long)buf, 2) != 2 || buf[0] != 'F' || buf[1] != 'F') {
        fail("/dg/f lost its head bytes across the relocation");
    }
    syscall2(SYS_CLOSE, fd, 0);

    /* And not at the root as /f. */
    fd = syscall3(SYS_OPEN, (long)"/f", O_RDONLY, 0);
    if (fd >= 0) {
        fail("the relocated file leaked to the root as /f");
    }

    puts("DIRGROW OK");
    sys_exit(0);
}
