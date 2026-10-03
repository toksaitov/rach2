/**
 * Probes that a user string running off a mapped page into a hole is rejected
 * with -EFAULT, not a kernel fault.
 *
 * The kernel copies a syscall pathname out of user memory with
 * strncpy_from_user, which validates and backs each page as the copy crosses a
 * boundary. An unterminated string that runs from a mapped page into an
 * unmapped one must make the syscall return -EFAULT and leave the process
 * running; it must never fault the kernel. The probe maps two pages, unmaps the
 * second to leave a hole, fills the end of the first with non-NUL bytes up to
 * the boundary, and calls open() with a path pointer near the end so the copy
 * walks off the mapped page into the hole.
 *
 * Prints "USERCOPYFAULT OK" and exits 0. Freestanding; direct system calls
 * (probe.h).
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

static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

static long mmap_anon(long len, int prot)
{
    return probe_mmap(0, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static int sys_munmap(unsigned long addr, unsigned long len)
{
    return syscall2(SYS_MUNMAP, (long)addr, (long)len);
}

static int sys_open(const char *path, int flags)
{
    return syscall3(SYS_OPEN, (long)path, (long)flags, (long)0);
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
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

static void fail(const char *why)
{
    sys_write(STDOUT_FILENO, "USERCOPYFAULT FAIL: ", 20);
    puts(why);
    sys_exit(1);
}

PROBE_START void _start(void)
{
    long p;
    char *path;
    int i, r;

    p = mmap_anon(2 * PAGE, PROT_READ | PROT_WRITE);
    if (sys_failed(p)) {
        fail("mmap two pages");
    }

    /* Back both pages, then drop the second so it becomes an unmapped hole. */
    ((volatile char *)p)[0] = 'a';
    ((volatile char *)p)[PAGE] = 'a';
    if (sys_munmap(p + PAGE, PAGE) != 0) {
        fail("munmap the trailing page");
    }

    /* Eight non-NUL bytes ending exactly at the page boundary: the ninth byte
     * the copy would read sits in the now-unmapped page. */
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
