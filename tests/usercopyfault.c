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
 * Prints "USERCOPYFAULT OK" and exits 0. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT   1
#define SYS_WRITE  4
#define SYS_OPEN   5
#define SYS_MUNMAP 91
#define SYS_MMAP2  192

#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

#define O_RDONLY 0
#define EFAULT   14
#define PAGE     4096u

static int sys_write(int fd, const void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
    return r;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static unsigned sys_mmap2_anon(unsigned len, int prot)
{
    unsigned r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_MMAP2), "b"(0), "c"(len), "d"(prot),
                           "S"(MAP_PRIVATE | MAP_ANONYMOUS), "D"(-1)
                         : "memory");
    return r;
}

static int sys_munmap(unsigned addr, unsigned len)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_MUNMAP), "b"(addr), "c"(len)
                         : "memory");
    return r;
}

static int sys_open(const char *path, int flags)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_OPEN), "b"(path), "c"(flags), "d"(0)
                         : "memory");
    return r;
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

void _start(void)
{
    unsigned p;
    char *path;
    int i, r;

    p = sys_mmap2_anon(2 * PAGE, PROT_READ | PROT_WRITE);
    if (p >= 0xFFFFF001u) {
        fail("mmap two pages");
    }

    /* Back both pages, then drop the second so it becomes an unmapped hole. */
    ((volatile char *) p)[0] = 'a';
    ((volatile char *) p)[PAGE] = 'a';
    if (sys_munmap(p + PAGE, PAGE) != 0) {
        fail("munmap the trailing page");
    }

    /* Eight non-NUL bytes ending exactly at the page boundary: the ninth byte
     * the copy would read sits in the now-unmapped page. */
    path = (char *) (p + PAGE - 8);
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
