/**
 * Fills the file system, then proves the space comes back.
 *
 * A program writing a large file meets a full file system, and what it is told
 * decides whether it can do anything about it: `ENOSPC` says the disk is full
 * and `EIO` says the write itself went wrong. The kernel returned `EIO` for
 * both until this probe was written.
 *
 * So: write until the write is refused, require `ENOSPC`, require the file to
 * hold what was written before the refusal, unlink it, and write again. The
 * last step is what a file system that lost the space instead of freeing it
 * would fail.
 *
 * The probe writes to the mounted root, so it needs an image with room to
 * fill; `tests/run diskfull` gives it a small one. It removes its own file
 * whatever happens.
 *
 * Prints "DISKFULL OK" and exits 0. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000

#define ENOSPC 28

#define CHUNK 4096
/* Larger than any image this runs on, so the loop always reaches the
   refusal rather than running out of tries. */
#define TRIES 8192

static char block[CHUNK];

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

static void print(const char *s)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
}

static void puts(const char *s)
{
    print(s);
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"\n", 1);
}

static void print_num(int v)
{
    char buf[12];
    int i = 0;

    if (v < 0) {
        print("-");
        v = -v;
    }
    if (v == 0) {
        buf[i++] = '0';
    }
    while (v > 0) {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (i > 0) {
        syscall3(SYS_WRITE, STDOUT_FILENO, (long)&buf[--i], 1);
    }
}

static void fail(const char *why, int fd)
{
    if (fd >= 0) {
        syscall3(SYS_CLOSE, fd, 0, 0);
    }
    syscall3(SYS_UNLINK, (long)"/diskfull.tmp", 0, 0);
    print("DISKFULL FAIL: ");
    puts(why);
    sys_exit(1);
}

PROBE_START void _start(void)
{
    int fd, n, i, refused, written;

    for (i = 0; i < CHUNK; i++) {
        block[i] = (char)('a' + (i & 15));
    }

    fd = syscall3(SYS_OPEN, (long)"/diskfull.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail("the file could not be created", -1);
    }

    refused = 0;
    written = 0;
    for (i = 0; i < TRIES; i++) {
        n = syscall3(SYS_WRITE, fd, (long)block, CHUNK);
        if (n < 0) {
            refused = n;
            break;
        }
        written += n;
    }

    if (refused == 0) {
        fail("the file system never refused a write", fd);
    }
    if (refused != -ENOSPC) {
        print("DISKFULL FAIL: the refusal was ");
        print_num(refused);
        puts(", not -ENOSPC");
        syscall3(SYS_CLOSE, fd, 0, 0);
        syscall3(SYS_UNLINK, (long)"/diskfull.tmp", 0, 0);
        sys_exit(1);
    }
    if (written == 0) {
        fail("nothing was written before the refusal", fd);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);

    /* The space comes back. A file system that refused the same write again
       after the file was removed would have lost it, not freed it. */
    if (syscall3(SYS_UNLINK, (long)"/diskfull.tmp", 0, 0) != 0) {
        fail("the file could not be removed", -1);
    }
    fd = syscall3(SYS_OPEN, (long)"/diskfull.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail("the file could not be created again", -1);
    }
    n = syscall3(SYS_WRITE, fd, (long)block, CHUNK);
    if (n != CHUNK) {
        fail("the space a removed file held did not come back", fd);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    syscall3(SYS_UNLINK, (long)"/diskfull.tmp", 0, 0);

    print("DISKFULL OK (");
    print_num(written / 1024);
    puts(" KiB written, refused with ENOSPC, the space came back)");
    sys_exit(0);
}
