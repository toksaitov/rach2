/**
 * Exercises hierarchical directories from user space, on the root it is run on.
 *
 * One row depends on the file system: KRFFS does not store `.` and `..` and
 * does not synthesize them in a directory read, and ext2 has them on the disk
 * and returns them, as Linux does. The probe reads the root's type from
 * `/proc/mounts`, prints it, and refuses to judge a root that is neither.
 *
 * Makes a directory, creates and writes a file inside it, and reads it back;
 * rejects a duplicate mkdir and a create in a missing directory; refuses rmdir
 * on a non-empty directory; then unlinks the file, rmdirs the now-empty
 * directory, and confirms the file is gone. Goes on to test getdents on a
 * subdirectory, on KRFFS the EINVAL of a directory whose identifier another
 * has, the *at API (openat, fstat64, fstatat64, _llseek, fcntl64),
 * O_APPEND and O_CREAT mode, and chdir/getcwd with relative-name resolution.
 * Prints "DIRPROBE OK" when every check passes, else "DIRPROBE FAIL".
 *
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define F_DUPFD    0
#define F_GETFD    1
#define F_SETFD    2
#define FD_CLOEXEC 1

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_APPEND 02000

#define ENOTDIR 20
#define EINVAL  22

/* Special dir fd: openat/fstatat resolve relative to the current dir ("/"). */
#define AT_FDCWD (-100)

/* File-type bits in st_mode. */
#define S_IFMT  0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000

/*
 * fstat and fstatat fill the kernel's stat image, i386's stat64 or x86-64's
 * stat. The probe only checks two fields, read straight from the buffer at
 * their ABI byte offsets (probe.h): st_mode, a 32-bit field, and st_size's
 * low 32 bits.
 */
#define STAT64_SIZE       KSTAT_SIZE
#define STAT64_MODE(b)    (*(unsigned int *)((b) + KSTAT_MODE))
#define STAT64_SIZE_LO(b) (*(unsigned int *)((b) + KSTAT_SIZE_AT))

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

static int sys_write(int fd, const void *buf, size_t n)
{
    return syscall3(SYS_WRITE, fd, (long)buf, (int)n);
}

static int sys_read(int fd, void *buf, size_t n)
{
    return syscall3(SYS_READ, fd, (long)buf, (int)n);
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

/**
 * The root's file system type, read from `/proc/mounts`: 1 for KRFFS, 2 for
 * ext2.
 *
 * The two owe different answers for a directory's entries, so a probe that
 * could not read the type would judge whichever image it was written for.
 * Anything this cannot read, or a type it does not know, ends the probe.
 */
#define DT_DIR 4

#define ROOT_KRFFS 1
#define ROOT_EXT2  2

static int root_type(void)
{
    static char mounts[2048];
    int fd = syscall3(SYS_OPEN, (long)"/proc/mounts", O_RDONLY, 0);
    int n, i;

    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_READ, fd, (long)mounts, (int)sizeof(mounts) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return 0;
    }
    mounts[n] = 0;
    /* Each line is "device mountpoint type options 0 0"; the root's is the one
       whose mount point is a bare slash, and the field after it is the type. */
    for (i = 0; i < n; i++) {
        if (mounts[i] == ' ' && mounts[i + 1] == '/' && mounts[i + 2] == ' ') {
            const char *t = &mounts[i + 3];

            if (t[0] == 'k' && t[1] == 'r' && t[2] == 'f' && t[3] == 'f' && t[4] == 's' &&
                t[5] == ' ') {
                return ROOT_KRFFS;
            }
            if (t[0] == 'e' && t[1] == 'x' && t[2] == 't' && t[3] == '2' && t[4] == ' ') {
                return ROOT_EXT2;
            }
            return 0;
        }
    }
    return 0;
}

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        sys_write(STDOUT_FILENO, "  FAIL: ", 8);
        puts(msg);
    }
}

PROBE_START void _start(void)
{
    int fd;
    int root = root_type();
    char buf[8];

    /* The root's type decides one row below, and a root this probe does not
       know is not one it can judge. */
    check(root == ROOT_KRFFS || root == ROOT_EXT2, "the root's type is krffs or ext2");
    if (root == ROOT_EXT2) {
        puts("dirprobe: the root is ext2");
    } else if (root == ROOT_KRFFS) {
        puts("dirprobe: the root is krffs");
    }

    /* 1. Create a directory; a second mkdir of the same name must fail. */
    check(syscall2(SYS_MKDIR, (long)"/d", 0755) == 0, "mkdir /d");
    check(syscall2(SYS_MKDIR, (long)"/d", 0755) < 0, "mkdir /d again fails");

    /* 2. Create a file INSIDE the directory and write to it. */
    fd = syscall2(SYS_CREAT, (long)"/d/f", 0644);
    check(fd >= 0, "creat /d/f");
    if (fd >= 0) {
        check(sys_write(fd, "hi", 2) == 2, "write /d/f");
        syscall1(SYS_CLOSE, fd);
    }

    /* 3. Creating inside a missing directory must fail. */
    check(syscall2(SYS_CREAT, (long)"/nope/f", 0644) < 0, "creat in missing dir fails");

    /* 4. Reopen the nested file and read the data back. */
    fd = syscall3(SYS_OPEN, (long)"/d/f", O_RDONLY, 0);
    check(fd >= 0, "open /d/f");
    if (fd >= 0) {
        int r = sys_read(fd, buf, sizeof buf);
        check(r == 2 && buf[0] == 'h' && buf[1] == 'i', "read /d/f back");
        syscall1(SYS_CLOSE, fd);
    }

    /* 5. rmdir must refuse a non-empty directory. */
    check(syscall1(SYS_RMDIR, (long)"/d") < 0, "rmdir non-empty /d fails");

    /* 6. Remove the file, then the now-empty directory. */
    check(syscall1(SYS_UNLINK, (long)"/d/f") == 0, "unlink /d/f");
    check(syscall1(SYS_RMDIR, (long)"/d") == 0, "rmdir empty /d");

    /* 7. The file is gone. */
    check(syscall3(SYS_OPEN, (long)"/d/f", O_RDONLY, 0) < 0, "open removed /d/f fails");

    /* 8. getdents on a subdirectory lists EXACTLY its own children. */
    check(syscall2(SYS_MKDIR, (long)"/e", 0755) == 0, "mkdir /e");
    fd = syscall2(SYS_CREAT, (long)"/e/a", 0644);
    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
    }
    fd = syscall2(SYS_CREAT, (long)"/e/b", 0644);
    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
    }
    fd = syscall3(SYS_OPEN, (long)"/e", O_RDONLY, 0);
    check(fd >= 0, "open /e as directory");
    if (fd >= 0) {
        char dbuf[256];
        int n = syscall3(SYS_GETDENTS64, fd, (long)dbuf, (int)sizeof dbuf);
        int off = 0, count = 0, saw_a = 0, saw_b = 0;
        int saw_dot = 0, saw_dotdot = 0, other = 0;

        while (off + 19 < n) {
            unsigned short reclen = *(unsigned short *)(dbuf + off + 16);
            unsigned char type = (unsigned char)dbuf[off + 18];
            const char *nm = dbuf + off + 19;

            if (reclen < 20 || off + reclen > n) {
                break; /* a record that runs past what was read */
            }
            if (nm[0] == 'a' && nm[1] == '\0') {
                saw_a++;
            } else if (nm[0] == 'b' && nm[1] == '\0') {
                saw_b++;
            } else if (nm[0] == '.' && nm[1] == '\0' && type == DT_DIR) {
                saw_dot++;
            } else if (nm[0] == '.' && nm[1] == '.' && nm[2] == '\0' && type == DT_DIR) {
                saw_dotdot++;
            } else {
                other++;
            }
            count++;
            off += reclen;
        }
        if (root == ROOT_EXT2) {
            check(count == 4 && saw_a == 1 && saw_b == 1 && saw_dot == 1 && saw_dotdot == 1 &&
                      other == 0,
                  "getdents /e on ext2 lists a, b and one each of . and ..");
        } else {
            check(count == 2 && saw_a == 1 && saw_b == 1 && other == 0,
                  "getdents /e on the boot root lists exactly a and b");
        }
        syscall1(SYS_CLOSE, fd);
    }
    syscall1(SYS_UNLINK, (long)"/e/a");
    syscall1(SYS_UNLINK, (long)"/e/b");
    check(syscall1(SYS_RMDIR, (long)"/e") == 0, "rmdir emptied /e");

    /* 8b. A KRFFS directory's identifier is its parent's mixed with the hash
       of its name, which bC and cb share: the second of the two in one
       directory is a name the format cannot hold, EINVAL, and not a full
       or a failing volume. */
    if (root == ROOT_KRFFS) {
        check(syscall2(SYS_MKDIR, (long)"/ids", 0755) == 0 &&
                  syscall2(SYS_MKDIR, (long)"/ids/bC", 0755) == 0,
              "mkdir /ids and /ids/bC");
        check(syscall2(SYS_MKDIR, (long)"/ids/cb", 0755) == -EINVAL,
              "mkdir /ids/cb beside /ids/bC is EINVAL on krffs");
        check(syscall1(SYS_RMDIR, (long)"/ids/bC") == 0 && syscall1(SYS_RMDIR, (long)"/ids") == 0,
              "rmdir /ids/bC and /ids");
    }

    /* 9. The *at API on a regular file: openat, fstat64, _llseek, fcntl64. */
    fd = syscall2(SYS_CREAT, (long)"/g", 0644);
    check(fd >= 0, "creat /g");
    if (fd >= 0) {
        check(sys_write(fd, "hello", 5) == 5, "write /g");
        syscall1(SYS_CLOSE, fd);
    }
    /* openat with AT_FDCWD resolves from the root, like open. */
    fd = syscall3(SYS_OPENAT, AT_FDCWD, (long)"/g", O_RDONLY);
    check(fd >= 0, "openat /g");
    if (fd >= 0) {
        /* fstat64 overwrites st; the initializer just quiets cppcheck. */
        unsigned char st[STAT64_SIZE] = {0};
        long long result = 0;
        char c = 0;

        /* fstat64 the descriptor: a 5-byte regular file. */
        check(syscall2(SYS_FSTAT, fd, (long)st) == 0, "fstat64 /g");
        check((STAT64_MODE(st) & S_IFMT) == S_IFREG, "fstat64 /g is regular");
        check(STAT64_SIZE_LO(st) == 5, "fstat64 /g size is 5");

        /* _llseek SEEK_SET to offset 1; the result offset must come back
         * as 1. */
        check(probe_llseek(fd, 1, 0, &result) == 0, LLSEEK_NAME " /g");
        check(result == 1, LLSEEK_NAME " result offset is 1");
        check(sys_read(fd, &c, 1) == 1 && c == 'e', "read byte after seek is 'e'");

        /* FD_CLOEXEC round-trip through fcntl64. */
        check(syscall3(SYS_FCNTL, fd, F_SETFD, FD_CLOEXEC) == 0, "fcntl F_SETFD CLOEXEC");
        check(syscall3(SYS_FCNTL, fd, F_GETFD, 0) == FD_CLOEXEC, "fcntl F_GETFD CLOEXEC");

        syscall1(SYS_CLOSE, fd);
    }

    /* 10. fstatat64 by path: the file is regular, a directory reports
     *     S_IFDIR. */
    {
        unsigned char st[STAT64_SIZE] = {0}; /* fstatat64 overwrites; init quiets cppcheck */

        check(syscall5(SYS_FSTATAT, AT_FDCWD, (long)"/g", (long)st, 0, 0) == 0, "fstatat64 /g");
        check((STAT64_MODE(st) & S_IFMT) == S_IFREG && STAT64_SIZE_LO(st) == 5,
              "fstatat64 /g regular, size 5");

        check(syscall2(SYS_MKDIR, (long)"/h", 0755) == 0, "mkdir /h");
        check(syscall5(SYS_FSTATAT, AT_FDCWD, (long)"/h", (long)st, 0, 0) == 0, "fstatat64 /h");
        check((STAT64_MODE(st) & S_IFMT) == S_IFDIR, "fstatat64 /h is directory");
        check(syscall1(SYS_RMDIR, (long)"/h") == 0, "rmdir /h");
    }

    /* 11. O_APPEND forces writes to EOF; F_DUPFD honors its minimum fd; O_CREAT
     *     stamps the requested mode. */
    {
        /* fstat64 overwrites st; the initializer just quiets cppcheck. */
        unsigned char st[STAT64_SIZE] = {0};
        long long result = 0;
        char rb[8];
        int dupfd, r;

        /* /g holds "hello". Open O_APPEND, seek to 0, write '!': the append
         * contract must ignore the seek and place it at EOF -> "hello!". */
        fd = syscall3(SYS_OPENAT, AT_FDCWD, (long)"/g", O_WRONLY | O_APPEND);
        check(fd >= 0, "openat /g O_APPEND");
        if (fd >= 0) {
            probe_llseek(fd, 0, 0, &result); /* SEEK_SET 0 */
            check(sys_write(fd, "!", 1) == 1, "append write /g");
            syscall1(SYS_CLOSE, fd);
        }
        fd = syscall3(SYS_OPENAT, AT_FDCWD, (long)"/g", O_RDONLY);
        if (fd >= 0) {
            r = sys_read(fd, rb, sizeof rb);
            check(r == 6 && rb[5] == '!', "O_APPEND wrote at EOF (hello!)");

            /* F_DUPFD must return the lowest free fd >= 10, not just any fd. */
            dupfd = syscall3(SYS_FCNTL, fd, F_DUPFD, 10);
            check(dupfd >= 10, "F_DUPFD honors minimum fd");
            if (dupfd >= 0) {
                syscall1(SYS_CLOSE, dupfd);
            }
            syscall1(SYS_CLOSE, fd);
        }

        /* O_CREAT must apply the requested mode (openat's mode is the 4th arg,
         * passed in ESI, so use the 5-arg form). */
        fd = syscall5(SYS_OPENAT, AT_FDCWD, (long)"/m", O_WRONLY | O_CREAT, 0644, 0);
        check(fd >= 0, "openat /m O_CREAT");
        if (fd >= 0) {
            check(syscall2(SYS_FSTAT, fd, (long)st) == 0, "fstat64 /m");
            check((STAT64_MODE(st) & 0777) == 0644, "O_CREAT applied mode 0644");
            syscall1(SYS_CLOSE, fd);
        }
        check(syscall1(SYS_UNLINK, (long)"/m") == 0, "unlink /m");
    }

    check(syscall1(SYS_UNLINK, (long)"/g") == 0, "unlink /g");

    /* 12. chdir into a subdir, getcwd, relative-name resolution, and a
     *     dirfd-relative openat. */
    {
        char cwd[64];
        char rb[8];
        int n, dfd;

        check(syscall2(SYS_MKDIR, (long)"/work", 0755) == 0, "mkdir /work");
        check(syscall1(SYS_CHDIR, (long)"/work") == 0, "chdir /work");

        n = syscall2(SYS_GETCWD, (long)cwd, sizeof cwd);
        check(n > 0 && cwd[0] == '/' && cwd[1] == 'w' && cwd[2] == 'o' && cwd[3] == 'r' &&
                  cwd[4] == 'k' && cwd[5] == '\0',
              "getcwd is /work");

        /* Create + write a file by a RELATIVE name; it must land in /work. */
        fd = syscall2(SYS_CREAT, (long)"rel.txt", 0644);
        check(fd >= 0, "creat relative rel.txt");
        if (fd >= 0) {
            check(sys_write(fd, "deep", 4) == 4, "write rel.txt");
            syscall1(SYS_CLOSE, fd);
        }
        /* The same file is visible at the absolute path /work/rel.txt. */
        fd = syscall3(SYS_OPEN, (long)"/work/rel.txt", O_RDONLY, 0);
        check(fd >= 0, "open /work/rel.txt (absolute)");
        if (fd >= 0) {
            int r = sys_read(fd, rb, sizeof rb);
            check(r == 4 && rb[0] == 'd', "relative write landed in /work");
            syscall1(SYS_CLOSE, fd);
        }

        /* dirfd-relative openat: open /work as a dir fd, then resolve "rel.txt"
         * against it. */
        dfd = syscall3(SYS_OPENAT, AT_FDCWD, (long)"/work", O_RDONLY);
        check(dfd >= 0, "open /work as a dir fd");
        if (dfd >= 0) {
            fd = syscall3(SYS_OPENAT, dfd, (long)"rel.txt", O_RDONLY);
            check(fd >= 0, "openat(dirfd, rel.txt)");
            if (fd >= 0) {
                int r = sys_read(fd, rb, sizeof rb);
                check(r == 4 && rb[0] == 'd', "dirfd-relative read");
                syscall1(SYS_CLOSE, fd);
            }
            syscall1(SYS_CLOSE, dfd);
        }

        /* A path that descends through a regular file is ENOTDIR, not
         * ENOENT. */
        check(syscall1(SYS_CHDIR, (long)"rel.txt/x") == -ENOTDIR,
              "chdir through a file is ENOTDIR");

        /* "/work/.." resolves back to the root, which is a directory. */
        {
            unsigned char rst[STAT64_SIZE] = {0}; /* fstatat64 overwrites */
            check(syscall5(SYS_FSTATAT, AT_FDCWD, (long)"/work/..", (long)rst, 0, 0) == 0,
                  "fstatat64 /work/..");
            check((STAT64_MODE(rst) & S_IFMT) == S_IFDIR, "/work/.. is the root directory");
        }

        /* chdir back to the root via ".."; getcwd is "/". */
        check(syscall1(SYS_CHDIR, (long)"..") == 0, "chdir ..");
        n = syscall2(SYS_GETCWD, (long)cwd, sizeof cwd);
        check(n > 0 && cwd[0] == '/' && cwd[1] == '\0', "getcwd is / after chdir ..");

        check(syscall1(SYS_UNLINK, (long)"/work/rel.txt") == 0, "unlink /work/rel.txt");
        check(syscall1(SYS_RMDIR, (long)"/work") == 0, "rmdir /work");
    }

    if (fails == 0) {
        puts("DIRPROBE OK");
    } else {
        puts("DIRPROBE FAIL");
    }
    sys_exit(fails);
}
