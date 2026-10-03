/**
 * Exercises the file syscalls by opening `hello.txt`, reading it, and
 * echoing the contents to stdout, closing the descriptor at the end.
 *
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Open flags */
#define O_RDONLY 0

static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

static int sys_read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)count);
}

static int sys_open(const char *path, int flags)
{
    return syscall2(SYS_OPEN, (long)path, (long)flags);
}

static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
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

static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

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
