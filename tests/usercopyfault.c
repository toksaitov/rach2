/**
 * usercopyfault: a user path that runs off a mapped page into a hole is
 * refused with -EFAULT, and the kernel does not fault.
 *
 * The kernel copies a system call's path out of user memory with
 * strncpy_from_user, which checks and backs each page as the copy crosses a
 * boundary. An unterminated string that runs from a mapped page into an
 * unmapped one makes the call return -EFAULT and leaves the process
 * running. The probe maps two pages, unmaps the second to leave a hole,
 * fills the end of the first with non-NUL bytes up to the boundary, and
 * calls open with a path that starts near the end, so the copy runs off the
 * mapped page into the hole.
 *
 * Prints "USERCOPYFAULT OK" and exits 0 on success.
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

#define O_RDONLY 0
#define EFAULT   14
#define PAGE     4096u

/** The write call: the count written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** Map `len` bytes of private anonymous memory: the address, or a negated
 * errno that `sys_failed` recognizes. */
static long mmap_anon(long len, int prot)
{
    return probe_mmap(0, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/** The munmap call: 0, or a negated errno. */
static int sys_munmap(unsigned long addr, unsigned long len)
{
    return syscall2(SYS_MUNMAP, (long)addr, (long)len);
}

/** The open call with mode 0: a descriptor, or a negated errno. */
static int sys_open(const char *path, int flags)
{
    return syscall3(SYS_OPEN, (long)path, (long)flags, (long)0);
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
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/** Print "USERCOPYFAULT FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    sys_write(STDOUT_FILENO, "USERCOPYFAULT FAIL: ", 20);
    puts(why);
    sys_exit(1);
}

/** The probe, as the file's opening comment describes; it exits 1 through
 * `fail` at the first step that does not hold. */
PROBE_START void _start(void)
{
    long p;
    char *path;
    int i, r;

    p = mmap_anon(2 * PAGE, PROT_READ | PROT_WRITE);
    if (sys_failed(p)) {
        fail("mmap two pages");
    }

    /* Back both pages, then unmap the second to leave a hole. */
    ((volatile char *)p)[0] = 'a';
    ((volatile char *)p)[PAGE] = 'a';
    if (sys_munmap(p + PAGE, PAGE) != 0) {
        fail("munmap the trailing page");
    }

    /* The ninth byte the copy reads is the first of the unmapped page. */
    path = (char *)(p + PAGE - 8);
    for (i = 0; i < 8; i++) {
        path[i] = 'A';
    }

    r = sys_open(path, O_RDONLY);
    if (r >= 0) {
        fail("open of an unterminated run-off path unexpectedly succeeded");
    }
    if (r != -EFAULT) {
        fail("open did not reject the run-off path with -EFAULT");
    }

    puts("USERCOPYFAULT OK");
    sys_exit(0);
}
