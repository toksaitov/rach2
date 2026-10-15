/**
 * filetest: open hello.txt in the working directory, read it, print its
 * contents and close it, with the system calls made directly (probe.h).
 */

#include "probe.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define O_RDONLY 0

/** The write call: the count written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** The read call: the count read, 0 at the end, or a negated errno. */
static int sys_read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)count);
}

/** The open call: a descriptor, or a negated errno. */
static int sys_open(const char *path, int flags)
{
    return syscall2(SYS_OPEN, (long)path, (long)flags);
}

/** The close call: 0, or a negated errno. */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/** End the process with `status`; it does not return. */
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

/** Write `s` to standard output. */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

/** The probe: it exits 0 when the open, the read and the close succeed, and
 * 1 at the first that fails. The read takes at most 255 bytes. */
PROBE_START void _start(void)
{
    char buf[256];
    int fd;
    int n;

    print("Opening hello.txt... ");

    fd = sys_open("hello.txt", O_RDONLY);
    if (fd < 0) {
        puts("FAILED");
        sys_exit(1);
    }
    puts("OK");

    print("Reading file... ");
    n = sys_read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        puts("FAILED");
        sys_close(fd);
        sys_exit(1);
    }
    buf[n] = '\0';
    puts("OK");

    print("Content: ");
    puts(buf);

    print("Closing file... ");
    if (sys_close(fd) < 0) {
        puts("FAILED");
        sys_exit(1);
    }
    puts("OK");

    puts("All file syscalls passed!");
    sys_exit(0);
}
