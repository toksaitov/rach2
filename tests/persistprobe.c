/**
 * persistprobe: fsync makes a write durable, across two boots of one disk.
 *
 * On a KRFFS mounted from a disk (rach2.fsdisk), a boot that finds no
 * /persist.txt writes a marker to it, fsyncs and syncs, and reports
 * "PERSISTPROBE OK (wrote)"; a boot that finds it reads the marker back and
 * reports "PERSISTPROBE OK (read back)" only when the bytes match. Run twice
 * against one scratch image, the second boot shows that fsync pushed the
 * write through the page cache to the disk, since a write kept only in memory
 * or dropped would leave the file absent.
 *
 * A failure prints "PERSISTPROBE FAIL: " with the step and exits 1.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

static const char MARKER[] = "RACH2 PERSIST MARKER v1\n";
#define MARKER_LEN (sizeof(MARKER) - 1)

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
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
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

/** The probe's entry: read the marker back if the file exists, else write
 * it. */
PROBE_START void _start(void)
{
    const char *path = "/persist.txt";
    char buf[64];
    int fd;
    size_t i;

    fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd >= 0) {
        int n = syscall3(SYS_READ, fd, (long)buf, (int)sizeof(buf));
        syscall1(SYS_CLOSE, fd);
        if (n != (int)MARKER_LEN) {
            puts("PERSISTPROBE FAIL: short read");
            sys_exit(1);
        }
        for (i = 0; i < MARKER_LEN; ++i) {
            if (buf[i] != MARKER[i]) {
                puts("PERSISTPROBE FAIL: content mismatch");
                sys_exit(1);
            }
        }
        puts("PERSISTPROBE OK (read back)");
        sys_exit(0);
    }

    fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        puts("PERSISTPROBE FAIL: open for write");
        sys_exit(1);
    }
    if (syscall3(SYS_WRITE, fd, (long)MARKER, (int)MARKER_LEN) != (int)MARKER_LEN) {
        puts("PERSISTPROBE FAIL: write");
        sys_exit(1);
    }
    if (syscall1(SYS_FSYNC, fd) != 0) {
        puts("PERSISTPROBE FAIL: fsync");
        sys_exit(1);
    }
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_SYNC, 0);
    puts("PERSISTPROBE OK (wrote)");
    sys_exit(0);
}
