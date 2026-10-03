/**
 * Probes that a file keeps its directory when a write relocates it.
 *
 * KRFFS grows a file in place when the following block is free, but relocates
 * it to a fresh allocation (copying the header and data) when the next block is
 * taken. The copy must carry the file's parent_id, or the relocated file
 * silently leaves its directory and re-parents under the root, corrupting the
 * tree on disk. The probe makes /dg/f, then /dg/g right after it so f has no
 * adjacent room, grows f past its slack to force the relocation, and checks f
 * still resolves under /dg with its bytes intact.
 *
 * Prints "DIRGROW OK". Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define O_RDONLY 0
#define O_RDWR   2
#define O_CREAT  0100

#define GROW 2048

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
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"DIRGROW FAIL: ", 14);
    puts(why);
    sys_exit(1);
}

static char big[GROW];

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

    /* The relocated f must still live under /dg (not re-parented to the
     * root). */
    fd = syscall3(SYS_OPEN, (long)"/dg/f", O_RDONLY, 0);
    if (fd < 0) {
        fail("/dg/f vanished from its directory after a relocating grow");
    }
    if (syscall3(SYS_READ, fd, (long)buf, 2) != 2 || buf[0] != 'F' || buf[1] != 'F') {
        fail("/dg/f lost its head bytes across the relocation");
    }
    syscall2(SYS_CLOSE, fd, 0);

    /* It must NOT have appeared at the root as /f. */
    fd = syscall3(SYS_OPEN, (long)"/f", O_RDONLY, 0);
    if (fd >= 0) {
        fail("the relocated file leaked to the root as /f");
    }

    puts("DIRGROW OK");
    sys_exit(0);
}
