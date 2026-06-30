/**
 * Probes that an operation through a descriptor whose KRFFS node was freed is
 * rejected, not a use-after-free.
 *
 * KRFFS nodes are not refcounted, so unlinking a file frees its node while a
 * descriptor still points at it. The kernel must revalidate the cached node
 * before each op and fail a stale descriptor with -EBADF rather than read or
 * write the freed free-list metadata. The probe creates a file, opens it,
 * unlinks it, and checks that read, lseek, and fchmod through the now-stale fd
 * each return -EBADF and the kernel survives.
 *
 * Prints "FDSTALE OK". Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT   1
#define SYS_READ   3
#define SYS_WRITE  4
#define SYS_OPEN   5
#define SYS_CLOSE  6
#define SYS_UNLINK 10
#define SYS_LSEEK  19
#define SYS_FCHMOD 94
#define SYS_LLSEEK 140

#define O_RDWR   2
#define O_CREAT  0100
#define SEEK_END 2
#define EBADF    9

#define PATH "/fdstale.tmp"

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

static int sys1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
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
    sys3(SYS_WRITE, STDOUT_FILENO, (int)"FDSTALE FAIL: ", 14);
    puts(why);
    sys_exit(1);
}

void _start(void)
{
    char buf[8];
    int fd;

    fd = sys3(SYS_OPEN, (int)PATH, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        fail("could not create the test file");
    }
    if (sys3(SYS_WRITE, fd, (int)"data", 4) != 4) {
        fail("write to the fresh file");
    }

    /* Free the node out from under the still-open descriptor. */
    if (sys1(SYS_UNLINK, (int)PATH) != 0) {
        fail("unlink");
    }

    /* Every op through the stale fd must report -EBADF, not touch freed memory. */
    if (sys3(SYS_READ, fd, (int)buf, sizeof(buf)) != -EBADF) {
        fail("read through a stale fd did not return -EBADF");
    }
    if (sys3(SYS_LSEEK, fd, 0, SEEK_END) != -EBADF) {
        fail("lseek SEEK_END through a stale fd did not return -EBADF");
    }
    {
        /* _llseek (the path musl's lseek takes): EBX fd, ECX/EDX off hi/lo,
         * ESI result, EDI whence. */
        long long res;
        int r;
        __asm__ __volatile__("int $0x80"
                             : "=a"(r)
                             : "0"(SYS_LLSEEK), "b"(fd), "c"(0), "d"(0), "S"(&res), "D"(SEEK_END)
                             : "memory");
        if (r != -EBADF) {
            fail("_llseek SEEK_END through a stale fd did not return -EBADF");
        }
    }
    if (sys2(SYS_FCHMOD, fd, 0600) != -EBADF) {
        fail("fchmod through a stale fd did not return -EBADF");
    }

    sys1(SYS_CLOSE, fd);
    puts("FDSTALE OK");
    sys_exit(0);
}
