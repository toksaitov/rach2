/*
 * dirprobe.c - hierarchical KRFFS probe for Rach2 OS (Phase 6)
 *
 * Exercises real directories from user space: mkdir a directory, create/write a
 * file INSIDE it, read it back, reject duplicate mkdir and create-in-missing-dir,
 * refuse rmdir on a non-empty directory, then unlink the file and rmdir the now
 * empty directory and confirm the file is gone. Prints "DIRPROBE OK".
 *
 * Freestanding; direct int 0x80 syscalls. Build:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 \
 *                -o dirprobe dirprobe.c
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT       1
#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_CREAT      8
#define SYS_UNLINK     10
#define SYS_MKDIR      39
#define SYS_RMDIR      40
#define SYS_GETDENTS64 220

#define O_RDONLY 0

static int syscall1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int syscall2(int n, int a, int b)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory");
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

static int sys_write(int fd, const void *buf, size_t n)
{
    return syscall3(SYS_WRITE, fd, (int)(size_t)buf, (int)n);
}

static int sys_read(int fd, void *buf, size_t n)
{
    return syscall3(SYS_READ, fd, (int)(size_t)buf, (int)n);
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

static int fails = 0;

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        sys_write(STDOUT_FILENO, "  FAIL: ", 8);
        puts(msg);
    }
}

void _start(void)
{
    int fd;
    char buf[8];

    /* 1. Create a directory; a second mkdir of the same name must fail. */
    check(syscall2(SYS_MKDIR, (int)(size_t)"/d", 0755) == 0, "mkdir /d");
    check(syscall2(SYS_MKDIR, (int)(size_t)"/d", 0755) < 0, "mkdir /d again fails");

    /* 2. Create a file INSIDE the directory and write to it. */
    fd = syscall2(SYS_CREAT, (int)(size_t)"/d/f", 0644);
    check(fd >= 0, "creat /d/f");
    if (fd >= 0) {
        check(sys_write(fd, "hi", 2) == 2, "write /d/f");
        syscall1(SYS_CLOSE, fd);
    }

    /* 3. Creating inside a missing directory must fail. */
    check(syscall2(SYS_CREAT, (int)(size_t)"/nope/f", 0644) < 0, "creat in missing dir fails");

    /* 4. Reopen the nested file and read the data back. */
    fd = syscall3(SYS_OPEN, (int)(size_t)"/d/f", O_RDONLY, 0);
    check(fd >= 0, "open /d/f");
    if (fd >= 0) {
        int r = sys_read(fd, buf, sizeof buf);
        check(r == 2 && buf[0] == 'h' && buf[1] == 'i', "read /d/f back");
        syscall1(SYS_CLOSE, fd);
    }

    /* 5. rmdir must refuse a non-empty directory. */
    check(syscall1(SYS_RMDIR, (int)(size_t)"/d") < 0, "rmdir non-empty /d fails");

    /* 6. Remove the file, then the now-empty directory. */
    check(syscall1(SYS_UNLINK, (int)(size_t)"/d/f") == 0, "unlink /d/f");
    check(syscall1(SYS_RMDIR, (int)(size_t)"/d") == 0, "rmdir empty /d");

    /* 7. The file is gone. */
    check(syscall3(SYS_OPEN, (int)(size_t)"/d/f", O_RDONLY, 0) < 0, "open removed /d/f fails");

    /* 8. getdents on a subdirectory lists EXACTLY its own children. */
    check(syscall2(SYS_MKDIR, (int)(size_t)"/e", 0755) == 0, "mkdir /e");
    fd = syscall2(SYS_CREAT, (int)(size_t)"/e/a", 0644);
    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
    }
    fd = syscall2(SYS_CREAT, (int)(size_t)"/e/b", 0644);
    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
    }
    fd = syscall3(SYS_OPEN, (int)(size_t)"/e", O_RDONLY, 0);
    check(fd >= 0, "open /e as directory");
    if (fd >= 0) {
        char dbuf[256];
        int n = syscall3(SYS_GETDENTS64, fd, (int)(size_t)dbuf, (int)sizeof dbuf);
        int off = 0, count = 0, saw_a = 0, saw_b = 0;

        while (off + 19 < n) {
            unsigned short reclen = *(unsigned short *)(dbuf + off + 16);
            const char *nm = dbuf + off + 19;

            if (nm[0] == 'a' && nm[1] == '\0') {
                saw_a = 1;
            }
            if (nm[0] == 'b' && nm[1] == '\0') {
                saw_b = 1;
            }
            count++;
            if (reclen == 0) {
                break;
            }
            off += reclen;
        }
        check(count == 2 && saw_a && saw_b, "getdents /e lists exactly a and b");
        syscall1(SYS_CLOSE, fd);
    }
    syscall1(SYS_UNLINK, (int)(size_t)"/e/a");
    syscall1(SYS_UNLINK, (int)(size_t)"/e/b");
    check(syscall1(SYS_RMDIR, (int)(size_t)"/e") == 0, "rmdir emptied /e");

    if (fails == 0) {
        puts("DIRPROBE OK");
    } else {
        puts("DIRPROBE FAIL");
    }
    sys_exit(fails);
}
