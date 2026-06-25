/**
 * Exercises hierarchical KRFFS directories from user space.
 *
 * Makes a directory, creates and writes a file inside it, and reads it back;
 * rejects a duplicate mkdir and a create in a missing directory; refuses rmdir
 * on a non-empty directory; then unlinks the file, rmdirs the now-empty
 * directory, and confirms the file is gone. Goes on to test getdents on a
 * subdirectory, the *at API (openat, fstat64, fstatat64, _llseek, fcntl64),
 * O_APPEND and O_CREAT mode, and chdir/getcwd with relative-name resolution.
 * Prints "DIRPROBE OK" when every check passes, else "DIRPROBE FAIL".
 *
 * Freestanding; direct int 0x80.
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
#define SYS_CHDIR      12
#define SYS_GETCWD     183
#define SYS_LLSEEK     140
#define SYS_FSTAT64    197
#define SYS_FCNTL64    221
#define SYS_MKDIR      39
#define SYS_RMDIR      40
#define SYS_GETDENTS64 220
#define SYS_OPENAT     295
#define SYS_FSTATAT64  300

#define F_DUPFD    0
#define F_GETFD    1
#define F_SETFD    2
#define FD_CLOEXEC 1

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_APPEND 02000

#define ENOTDIR 20

/* Special dir fd: openat/fstatat resolve relative to the current dir ("/"). */
#define AT_FDCWD (-100)

/* File-type bits in st_mode. */
#define S_IFMT  0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000

/*
 * fstat64/fstatat64 fill a Linux i386 stat64 image (the kernel's struct
 * linux_stat64, 96 bytes). The probe only checks two fields, read straight from
 * the buffer at their ABI byte offsets: st_mode is a 32-bit field at +16, and
 * st_size's low 32 bits sit at +44.
 */
#define STAT64_SIZE       96
#define STAT64_MODE(b)    (*(unsigned int *)((b) + 16))
#define STAT64_SIZE_LO(b) (*(unsigned int *)((b) + 44))

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

static int syscall5(int n, int a, int b, int c, int d, int e)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory");
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

    /* 9. The *at API on a regular file: openat, fstat64, _llseek, fcntl64. */
    fd = syscall2(SYS_CREAT, (int)(size_t)"/g", 0644);
    check(fd >= 0, "creat /g");
    if (fd >= 0) {
        check(sys_write(fd, "hello", 5) == 5, "write /g");
        syscall1(SYS_CLOSE, fd);
    }
    /* openat with AT_FDCWD resolves from the root, like open. */
    fd = syscall3(SYS_OPENAT, AT_FDCWD, (int)(size_t)"/g", O_RDONLY);
    check(fd >= 0, "openat /g");
    if (fd >= 0) {
        unsigned char st[STAT64_SIZE] = {0}; /* fstat64 overwrites; init quiets cppcheck */
        long long result = 0;
        char c = 0;

        /* fstat64 the descriptor: a 5-byte regular file. */
        check(syscall2(SYS_FSTAT64, fd, (int)(size_t)st) == 0, "fstat64 /g");
        check((STAT64_MODE(st) & S_IFMT) == S_IFREG, "fstat64 /g is regular");
        check(STAT64_SIZE_LO(st) == 5, "fstat64 /g size is 5");

        /* _llseek SEEK_SET to offset 1; the result offset must come back as 1. */
        check(syscall5(SYS_LLSEEK, fd, 0, 1, (int)(size_t)&result, 0) == 0, "_llseek /g");
        check(result == 1, "_llseek result offset is 1");
        check(sys_read(fd, &c, 1) == 1 && c == 'e', "read byte after seek is 'e'");

        /* FD_CLOEXEC round-trip through fcntl64. */
        check(syscall3(SYS_FCNTL64, fd, F_SETFD, FD_CLOEXEC) == 0, "fcntl F_SETFD CLOEXEC");
        check(syscall3(SYS_FCNTL64, fd, F_GETFD, 0) == FD_CLOEXEC, "fcntl F_GETFD CLOEXEC");

        syscall1(SYS_CLOSE, fd);
    }

    /* 10. fstatat64 by path: the file is regular, a directory reports S_IFDIR. */
    {
        unsigned char st[STAT64_SIZE] = {0}; /* fstatat64 overwrites; init quiets cppcheck */

        check(syscall5(SYS_FSTATAT64, AT_FDCWD, (int)(size_t)"/g", (int)(size_t)st, 0, 0) == 0,
              "fstatat64 /g");
        check((STAT64_MODE(st) & S_IFMT) == S_IFREG && STAT64_SIZE_LO(st) == 5,
              "fstatat64 /g regular, size 5");

        check(syscall2(SYS_MKDIR, (int)(size_t)"/h", 0755) == 0, "mkdir /h");
        check(syscall5(SYS_FSTATAT64, AT_FDCWD, (int)(size_t)"/h", (int)(size_t)st, 0, 0) == 0,
              "fstatat64 /h");
        check((STAT64_MODE(st) & S_IFMT) == S_IFDIR, "fstatat64 /h is directory");
        check(syscall1(SYS_RMDIR, (int)(size_t)"/h") == 0, "rmdir /h");
    }

    /* 11. O_APPEND forces writes to EOF; F_DUPFD honors its minimum fd; O_CREAT
     *     stamps the requested mode. */
    {
        unsigned char st[STAT64_SIZE] = {0}; /* fstat64 overwrites; init quiets cppcheck */
        long long result = 0;
        char rb[8];
        int dupfd, r;

        /* /g holds "hello". Open O_APPEND, seek to 0, write '!': the append
         * contract must ignore the seek and place it at EOF -> "hello!". */
        fd = syscall3(SYS_OPENAT, AT_FDCWD, (int)(size_t)"/g", O_WRONLY | O_APPEND);
        check(fd >= 0, "openat /g O_APPEND");
        if (fd >= 0) {
            syscall5(SYS_LLSEEK, fd, 0, 0, (int)(size_t)&result, 0); /* SEEK_SET 0 */
            check(sys_write(fd, "!", 1) == 1, "append write /g");
            syscall1(SYS_CLOSE, fd);
        }
        fd = syscall3(SYS_OPENAT, AT_FDCWD, (int)(size_t)"/g", O_RDONLY);
        if (fd >= 0) {
            r = sys_read(fd, rb, sizeof rb);
            check(r == 6 && rb[5] == '!', "O_APPEND wrote at EOF (hello!)");

            /* F_DUPFD must return the lowest free fd >= 10, not just any fd. */
            dupfd = syscall3(SYS_FCNTL64, fd, F_DUPFD, 10);
            check(dupfd >= 10, "F_DUPFD honors minimum fd");
            if (dupfd >= 0) {
                syscall1(SYS_CLOSE, dupfd);
            }
            syscall1(SYS_CLOSE, fd);
        }

        /* O_CREAT must apply the requested mode (openat's mode is the 4th arg,
         * passed in ESI, so use the 5-arg form). */
        fd = syscall5(SYS_OPENAT, AT_FDCWD, (int)(size_t)"/m", O_WRONLY | O_CREAT, 0644, 0);
        check(fd >= 0, "openat /m O_CREAT");
        if (fd >= 0) {
            check(syscall2(SYS_FSTAT64, fd, (int)(size_t)st) == 0, "fstat64 /m");
            check((STAT64_MODE(st) & 0777) == 0644, "O_CREAT applied mode 0644");
            syscall1(SYS_CLOSE, fd);
        }
        check(syscall1(SYS_UNLINK, (int)(size_t)"/m") == 0, "unlink /m");
    }

    check(syscall1(SYS_UNLINK, (int)(size_t)"/g") == 0, "unlink /g");

    /* 12. chdir into a subdir, getcwd, relative-name resolution, and a
     *     dirfd-relative openat. */
    {
        char cwd[64];
        char rb[8];
        int n, dfd;

        check(syscall2(SYS_MKDIR, (int)(size_t)"/work", 0755) == 0, "mkdir /work");
        check(syscall1(SYS_CHDIR, (int)(size_t)"/work") == 0, "chdir /work");

        n = syscall2(SYS_GETCWD, (int)(size_t)cwd, sizeof cwd);
        check(n > 0 && cwd[0] == '/' && cwd[1] == 'w' && cwd[2] == 'o' && cwd[3] == 'r' &&
                  cwd[4] == 'k' && cwd[5] == '\0',
              "getcwd is /work");

        /* Create + write a file by a RELATIVE name; it must land in /work. */
        fd = syscall2(SYS_CREAT, (int)(size_t)"rel.txt", 0644);
        check(fd >= 0, "creat relative rel.txt");
        if (fd >= 0) {
            check(sys_write(fd, "deep", 4) == 4, "write rel.txt");
            syscall1(SYS_CLOSE, fd);
        }
        /* The same file is visible at the absolute path /work/rel.txt. */
        fd = syscall3(SYS_OPEN, (int)(size_t)"/work/rel.txt", O_RDONLY, 0);
        check(fd >= 0, "open /work/rel.txt (absolute)");
        if (fd >= 0) {
            int r = sys_read(fd, rb, sizeof rb);
            check(r == 4 && rb[0] == 'd', "relative write landed in /work");
            syscall1(SYS_CLOSE, fd);
        }

        /* dirfd-relative openat: open /work as a dir fd, then resolve "rel.txt"
         * against it. */
        dfd = syscall3(SYS_OPENAT, AT_FDCWD, (int)(size_t)"/work", O_RDONLY);
        check(dfd >= 0, "open /work as a dir fd");
        if (dfd >= 0) {
            fd = syscall3(SYS_OPENAT, dfd, (int)(size_t)"rel.txt", O_RDONLY);
            check(fd >= 0, "openat(dirfd, rel.txt)");
            if (fd >= 0) {
                int r = sys_read(fd, rb, sizeof rb);
                check(r == 4 && rb[0] == 'd', "dirfd-relative read");
                syscall1(SYS_CLOSE, fd);
            }
            syscall1(SYS_CLOSE, dfd);
        }

        /* A path that descends through a regular file is ENOTDIR, not ENOENT. */
        check(syscall1(SYS_CHDIR, (int)(size_t)"rel.txt/x") == -ENOTDIR,
              "chdir through a file is ENOTDIR");

        /* "/work/.." resolves back to the root, which is a directory. */
        {
            unsigned char rst[STAT64_SIZE] = {0}; /* fstatat64 overwrites */
            check(syscall5(SYS_FSTATAT64, AT_FDCWD, (int)(size_t)"/work/..", (int)(size_t)rst, 0,
                           0) == 0,
                  "fstatat64 /work/..");
            check((STAT64_MODE(rst) & S_IFMT) == S_IFDIR, "/work/.. is the root directory");
        }

        /* chdir back to the root via ".."; getcwd is "/". */
        check(syscall1(SYS_CHDIR, (int)(size_t)"..") == 0, "chdir ..");
        n = syscall2(SYS_GETCWD, (int)(size_t)cwd, sizeof cwd);
        check(n > 0 && cwd[0] == '/' && cwd[1] == '\0', "getcwd is / after chdir ..");

        check(syscall1(SYS_UNLINK, (int)(size_t)"/work/rel.txt") == 0, "unlink /work/rel.txt");
        check(syscall1(SYS_RMDIR, (int)(size_t)"/work") == 0, "rmdir /work");
    }

    if (fails == 0) {
        puts("DIRPROBE OK");
    } else {
        puts("DIRPROBE FAIL");
    }
    sys_exit(fails);
}
