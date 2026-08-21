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
 * Prints "DIRGROW OK". Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT  1
#define SYS_READ  3
#define SYS_WRITE 4
#define SYS_OPEN  5
#define SYS_CLOSE 6
#define SYS_MKDIR 39

#define O_RDONLY 0
#define O_RDWR   2
#define O_CREAT  0100

#define GROW 2048

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
    sys3(SYS_WRITE, STDOUT_FILENO, (int)"DIRGROW FAIL: ", 14);
    puts(why);
    sys_exit(1);
}

static char big[GROW];

void _start(void)
{
    int fd_f, fd_g, fd;
    int i;
    char buf[4];

    if (sys2(SYS_MKDIR, (int)"/dg", 0755) != 0) {
        fail("mkdir /dg");
    }

    fd_f = sys3(SYS_OPEN, (int)"/dg/f", O_CREAT | O_RDWR, 0644);
    if (fd_f < 0) {
        fail("create /dg/f");
    }
    if (sys3(SYS_WRITE, fd_f, (int)"FF", 2) != 2) {
        fail("write /dg/f");
    }

    /* A second file right after f leaves it no adjacent room to grow into. */
    fd_g = sys3(SYS_OPEN, (int)"/dg/g", O_CREAT | O_RDWR, 0644);
    if (fd_g < 0) {
        fail("create /dg/g");
    }
    if (sys3(SYS_WRITE, fd_g, (int)"GG", 2) != 2) {
        fail("write /dg/g");
    }

    /* Grow f past its slack: with g adjacent, this relocates f. */
    for (i = 0; i < GROW; i++) {
        big[i] = (char)('a' + (i & 31));
    }
    if (sys3(SYS_WRITE, fd_f, (int)big, GROW) != GROW) {
        fail("grow /dg/f");
    }

    sys2(SYS_CLOSE, fd_f, 0);
    sys2(SYS_CLOSE, fd_g, 0);

    /* The relocated f must still live under /dg (not re-parented to the
     * root). */
    fd = sys3(SYS_OPEN, (int)"/dg/f", O_RDONLY, 0);
    if (fd < 0) {
        fail("/dg/f vanished from its directory after a relocating grow");
    }
    if (sys3(SYS_READ, fd, (int)buf, 2) != 2 || buf[0] != 'F' || buf[1] != 'F') {
        fail("/dg/f lost its head bytes across the relocation");
    }
    sys2(SYS_CLOSE, fd, 0);

    /* It must NOT have appeared at the root as /f. */
    fd = sys3(SYS_OPEN, (int)"/f", O_RDONLY, 0);
    if (fd >= 0) {
        fail("the relocated file leaked to the root as /f");
    }

    puts("DIRGROW OK");
    sys_exit(0);
}
