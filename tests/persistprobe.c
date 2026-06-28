/**
 * fsync durability probe (two-boot).
 *
 * On a KRFFS mounted from a real disk (rach2.fsdisk), if /persist.txt is absent
 * this is the first boot: it writes a marker, fsyncs and syncs, and reports
 * "PERSISTPROBE OK (wrote)". If /persist.txt is present this is a later boot
 * against the same disk: it reads the marker back and reports "PERSISTPROBE OK
 * (read back)" only when the bytes match. Run twice against one scratch image,
 * the second boot proves fsync actually pushed the write through the page cache
 * to the disk, since a RAM-only or dropped write would leave the file absent.
 *
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT  1
#define SYS_READ  3
#define SYS_WRITE 4
#define SYS_OPEN  5
#define SYS_CLOSE 6
#define SYS_SYNC  36
#define SYS_FSYNC 118

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

static const char MARKER[] = "RACH2 PERSIST MARKER v1\n";
#define MARKER_LEN (sizeof(MARKER) - 1)

static int syscall1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
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
    syscall3(SYS_WRITE, 1, (int)(size_t)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (int)(size_t)"\n", 1);
}

void _start(void)
{
    const char *path = "/persist.txt";
    char buf[64];
    int fd;
    size_t i;

    fd = syscall3(SYS_OPEN, (int)(size_t)path, O_RDONLY, 0);
    if (fd >= 0) {
        int n = syscall3(SYS_READ, fd, (int)(size_t)buf, (int)sizeof(buf));
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

    fd = syscall3(SYS_OPEN, (int)(size_t)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        puts("PERSISTPROBE FAIL: open for write");
        sys_exit(1);
    }
    if (syscall3(SYS_WRITE, fd, (int)(size_t)MARKER, (int)MARKER_LEN) != (int)MARKER_LEN) {
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
