/**
 * dirprobe: directories, tested from user space on the root it is run on.
 *
 * One row depends on the file system: KRFFS does not store `.` and `..` and
 * does not synthesize them in a directory read, and ext2 has them on the disk
 * and returns them, as Linux does. The probe reads the root's type by
 * `statfs` of `/`, prints it, and refuses to judge a root that is neither.
 * `statfs` gives the type of the root the probe sees, a private root inside
 * a chroot among them, where `/proc/mounts` would name the machine's.
 *
 * Makes a directory, creates and writes a file inside it, and reads it back;
 * rejects a duplicate mkdir and a create in a missing directory; refuses rmdir
 * on a non-empty directory; then unlinks the file, rmdirs the emptied
 * directory, and confirms the file is gone. Goes on to test getdents on a
 * subdirectory, on KRFFS the EINVAL of a directory whose identifier another
 * has, the *at API (openat, fstat64, fstatat64, _llseek, fcntl64),
 * O_APPEND, F_DUPFD and O_CREAT mode, and chdir/getcwd with relative-name
 * resolution. Prints "DIRPROBE OK" and exits 0 when every check passes, and
 * "DIRPROBE FAIL" with the count of failed checks as its status otherwise.
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, an assertion for each check `checks` lists, with the branch `root`
 * ext2, krffs or other; a check that a failed one guards is not run.
 */

#include "probe.h"
#include "probe_report.h"

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

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

/** The write call: the count written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t n)
{
    return syscall3(SYS_WRITE, fd, (long)buf, (int)n);
}

/** The read call: the count read, 0 at the end, or a negated errno. */
static int sys_read(int fd, void *buf, size_t n)
{
    return syscall3(SYS_READ, fd, (long)buf, (int)n);
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

static int fails = 0;

#define DT_DIR 4

#define ROOT_KRFFS 1
#define ROOT_EXT2  2

/* The `statfs` magic of a KRFFS root image, of a KRFFS volume mounted by
 * mount(2), and of ext2. */
#define MAGIC_KRFFS_ROOT  0x4B524646u
#define MAGIC_KRFFS_MOUNT 0x4B52u
#define MAGIC_EXT2        0xEF53u

/**
 * The root's file system type, read by `statfs` of `/`: ROOT_KRFFS,
 * ROOT_EXT2, or 0 when the call fails or the type is another.
 *
 * The two give different answers for a directory's entries, so a probe that
 * did not read the type would judge only the image it was written for.
 * The caller fails a root of type 0.
 */
static int root_type(void)
{
    static unsigned buf[KSTATFS_SIZE / sizeof(unsigned)];

    if (probe_statfs("/", buf) != 0) {
        return 0;
    }
    if (buf[0] == MAGIC_KRFFS_ROOT || buf[0] == MAGIC_KRFFS_MOUNT) {
        return ROOT_KRFFS;
    }
    return buf[0] == MAGIC_EXT2 ? ROOT_EXT2 : 0;
}

/* The checks, one assertion each, in the order the probe makes them, and the
 * text it prints for each that fails. `GETDENTS_EXT2` is checked on ext2,
 * and `GETDENTS_KRFFS` and the checks from `MKDIR_IDS` to `RMDIR_IDS` on
 * KRFFS. */
enum {
    ROOT_TYPE,
    MKDIR_D,
    MKDIR_D_AGAIN,
    CREAT_D_F,
    WRITE_D_F,
    CREAT_MISSING,
    OPEN_D_F,
    READ_D_F,
    RMDIR_NONEMPTY,
    UNLINK_D_F,
    RMDIR_EMPTY,
    OPEN_REMOVED,
    MKDIR_E,
    OPEN_E,
    GETDENTS_EXT2,
    GETDENTS_KRFFS,
    RMDIR_E,
    MKDIR_IDS,
    MKDIR_IDS_EINVAL,
    RMDIR_IDS,
    CREAT_G,
    WRITE_G,
    OPENAT_G,
    FSTAT_G,
    FSTAT_G_REGULAR,
    FSTAT_G_SIZE,
    LLSEEK_G,
    LLSEEK_RESULT,
    READ_AFTER_SEEK,
    SETFD_CLOEXEC,
    GETFD_CLOEXEC,
    FSTATAT_G,
    FSTATAT_G_REGULAR,
    MKDIR_H,
    FSTATAT_H,
    FSTATAT_H_DIR,
    RMDIR_H,
    OPENAT_APPEND,
    APPEND_WRITE,
    APPEND_AT_END,
    DUPFD_MINIMUM,
    OPENAT_CREAT,
    FSTAT_M,
    CREAT_MODE,
    UNLINK_M,
    UNLINK_G,
    MKDIR_WORK,
    CHDIR_WORK,
    GETCWD_WORK,
    CREAT_RELATIVE,
    WRITE_RELATIVE,
    OPEN_ABSOLUTE,
    RELATIVE_LANDED,
    OPEN_WORK_FD,
    OPENAT_DIRFD,
    DIRFD_READ,
    CHDIR_ENOTDIR,
    FSTATAT_DOTDOT,
    DOTDOT_ROOT,
    CHDIR_DOTDOT,
    GETCWD_ROOT,
    UNLINK_REL,
    RMDIR_WORK,
    CHECKS
};

static const struct {
    const char *id;
    const char *text;
} checks[CHECKS] = {
    {"root-type", "the root's type is krffs or ext2"},
    {"mkdir-d", "mkdir /d"},
    {"mkdir-d-again", "mkdir /d again fails"},
    {"creat-d-f", "creat /d/f"},
    {"write-d-f", "write /d/f"},
    {"creat-missing", "creat in missing dir fails"},
    {"open-d-f", "open /d/f"},
    {"read-d-f", "read /d/f back"},
    {"rmdir-nonempty", "rmdir non-empty /d fails"},
    {"unlink-d-f", "unlink /d/f"},
    {"rmdir-empty", "rmdir empty /d"},
    {"open-removed", "open removed /d/f fails"},
    {"mkdir-e", "mkdir /e"},
    {"open-e", "open /e as directory"},
    {"getdents-ext2", "getdents /e on ext2 lists a, b and one each of . and .."},
    {"getdents-krffs", "getdents /e on the boot root lists exactly a and b"},
    {"rmdir-e", "rmdir emptied /e"},
    {"mkdir-ids", "mkdir /ids and /ids/bC"},
    {"mkdir-ids-einval", "mkdir /ids/cb beside /ids/bC is EINVAL on krffs"},
    {"rmdir-ids", "rmdir /ids/bC and /ids"},
    {"creat-g", "creat /g"},
    {"write-g", "write /g"},
    {"openat-g", "openat /g"},
    {"fstat-g", "fstat64 /g"},
    {"fstat-g-regular", "fstat64 /g is regular"},
    {"fstat-g-size", "fstat64 /g size is 5"},
    {"llseek-g", LLSEEK_NAME " /g"},
    {"llseek-result", LLSEEK_NAME " result offset is 1"},
    {"read-after-seek", "read byte after seek is 'e'"},
    {"setfd-cloexec", "fcntl F_SETFD CLOEXEC"},
    {"getfd-cloexec", "fcntl F_GETFD CLOEXEC"},
    {"fstatat-g", "fstatat64 /g"},
    {"fstatat-g-regular", "fstatat64 /g regular, size 5"},
    {"mkdir-h", "mkdir /h"},
    {"fstatat-h", "fstatat64 /h"},
    {"fstatat-h-dir", "fstatat64 /h is directory"},
    {"rmdir-h", "rmdir /h"},
    {"openat-append", "openat /g O_APPEND"},
    {"append-write", "append write /g"},
    {"append-at-end", "O_APPEND wrote at EOF (hello!)"},
    {"dupfd-minimum", "F_DUPFD honors minimum fd"},
    {"openat-creat", "openat /m O_CREAT"},
    {"fstat-m", "fstat64 /m"},
    {"creat-mode", "O_CREAT applied mode 0644"},
    {"unlink-m", "unlink /m"},
    {"unlink-g", "unlink /g"},
    {"mkdir-work", "mkdir /work"},
    {"chdir-work", "chdir /work"},
    {"getcwd-work", "getcwd is /work"},
    {"creat-relative", "creat relative rel.txt"},
    {"write-relative", "write rel.txt"},
    {"open-absolute", "open /work/rel.txt (absolute)"},
    {"relative-landed", "relative write landed in /work"},
    {"open-work-fd", "open /work as a dir fd"},
    {"openat-dirfd", "openat(dirfd, rel.txt)"},
    {"dirfd-read", "dirfd-relative read"},
    {"chdir-enotdir", "chdir through a file is ENOTDIR"},
    {"fstatat-dotdot", "fstatat64 /work/.."},
    {"dotdot-root", "/work/.. is the root directory"},
    {"chdir-dotdot", "chdir .."},
    {"getcwd-root", "getcwd is / after chdir .."},
    {"unlink-rel", "unlink /work/rel.txt"},
    {"rmdir-work", "rmdir /work"},
};

/* Each check's outcome: 0 not made, 1 held, 2 failed. */
static unsigned char outcome[CHECKS];

/** Record the outcome of the check `id`, and count a failure and print its
 * text when `cond` is false. */
static void check(int id, int cond)
{
    outcome[id] = cond ? 1 : 2;
    if (!cond) {
        fails++;
        sys_write(STDOUT_FILENO, "  FAIL: ", 8);
        puts(checks[id].text);
    }
}

/**
 * Rows 1 to 7: make /d, a duplicate mkdir that fails, a file in it written
 * and read back, a create in a missing directory that fails, an rmdir of the
 * non-empty /d that fails, and the removal of the file and of /d, after
 * which the file is gone.
 */
static void file_rows(void)
{
    char buf[8];
    int fd;

    check(MKDIR_D, syscall2(SYS_MKDIR, (long)"/d", 0755) == 0);
    check(MKDIR_D_AGAIN, syscall2(SYS_MKDIR, (long)"/d", 0755) < 0);
    fd = syscall2(SYS_CREAT, (long)"/d/f", 0644);
    check(CREAT_D_F, fd >= 0);
    if (fd >= 0) {
        check(WRITE_D_F, sys_write(fd, "hi", 2) == 2);
        syscall1(SYS_CLOSE, fd);
    }
    check(CREAT_MISSING, syscall2(SYS_CREAT, (long)"/nope/f", 0644) < 0);
    fd = syscall3(SYS_OPEN, (long)"/d/f", O_RDONLY, 0);
    check(OPEN_D_F, fd >= 0);
    if (fd >= 0) {
        int r = sys_read(fd, buf, sizeof buf);
        check(READ_D_F, r == 2 && buf[0] == 'h' && buf[1] == 'i');
        syscall1(SYS_CLOSE, fd);
    }
    check(RMDIR_NONEMPTY, syscall1(SYS_RMDIR, (long)"/d") < 0);
    check(UNLINK_D_F, syscall1(SYS_UNLINK, (long)"/d/f") == 0);
    check(RMDIR_EMPTY, syscall1(SYS_RMDIR, (long)"/d") == 0);
    check(OPEN_REMOVED, syscall3(SYS_OPEN, (long)"/d/f", O_RDONLY, 0) < 0);
}

/**
 * Row 8's listing: read the directory `fd`, which has the files a and b, with
 * getdents64, and check that it lists them and, on ext2 as `root` gives, one
 * each of `.` and `..`, and nothing else.
 */
static void list_e(int fd, int root)
{
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
        check(GETDENTS_EXT2, count == 4 && saw_a == 1 && saw_b == 1 && saw_dot == 1 &&
                                 saw_dotdot == 1 && other == 0);
    } else {
        check(GETDENTS_KRFFS, count == 2 && saw_a == 1 && saw_b == 1 && other == 0);
    }
}

/**
 * Row 8: getdents64 of a subdirectory lists its own children and no others.
 * Row 8b, on KRFFS: a directory's identifier is its parent's mixed with the
 * hash of its name, which bC and cb share, so the second of the two in one
 * directory is a name the format cannot hold, EINVAL, and not a full or a
 * failing volume.
 */
static void listing_rows(int root)
{
    int fd;

    check(MKDIR_E, syscall2(SYS_MKDIR, (long)"/e", 0755) == 0);
    fd = syscall2(SYS_CREAT, (long)"/e/a", 0644);
    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
    }
    fd = syscall2(SYS_CREAT, (long)"/e/b", 0644);
    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
    }
    fd = syscall3(SYS_OPEN, (long)"/e", O_RDONLY, 0);
    check(OPEN_E, fd >= 0);
    if (fd >= 0) {
        list_e(fd, root);
        syscall1(SYS_CLOSE, fd);
    }
    syscall1(SYS_UNLINK, (long)"/e/a");
    syscall1(SYS_UNLINK, (long)"/e/b");
    check(RMDIR_E, syscall1(SYS_RMDIR, (long)"/e") == 0);

    if (root == ROOT_KRFFS) {
        check(MKDIR_IDS, syscall2(SYS_MKDIR, (long)"/ids", 0755) == 0 &&
                             syscall2(SYS_MKDIR, (long)"/ids/bC", 0755) == 0);
        check(MKDIR_IDS_EINVAL, syscall2(SYS_MKDIR, (long)"/ids/cb", 0755) == -EINVAL);
        check(RMDIR_IDS,
              syscall1(SYS_RMDIR, (long)"/ids/bC") == 0 && syscall1(SYS_RMDIR, (long)"/ids") == 0);
    }
}

/**
 * Row 9: the *at API on a regular file, /g, which has "hello": openat with
 * AT_FDCWD, fstat64, _llseek and the FD_CLOEXEC round trip of fcntl64.
 */
static void at_rows(void)
{
    int fd;

    fd = syscall2(SYS_CREAT, (long)"/g", 0644);
    check(CREAT_G, fd >= 0);
    if (fd >= 0) {
        check(WRITE_G, sys_write(fd, "hello", 5) == 5);
        syscall1(SYS_CLOSE, fd);
    }
    fd = syscall3(SYS_OPENAT, AT_FDCWD, (long)"/g", O_RDONLY);
    check(OPENAT_G, fd >= 0);
    if (fd >= 0) {
        /* fstat64 overwrites st; the initializer just quiets cppcheck. */
        unsigned char st[STAT64_SIZE] = {0};
        long long result = 0;
        char c = 0;

        check(FSTAT_G, syscall2(SYS_FSTAT, fd, (long)st) == 0);
        check(FSTAT_G_REGULAR, (STAT64_MODE(st) & S_IFMT) == S_IFREG);
        check(FSTAT_G_SIZE, STAT64_SIZE_LO(st) == 5);
        check(LLSEEK_G, probe_llseek(fd, 1, 0, &result) == 0);
        check(LLSEEK_RESULT, result == 1);
        check(READ_AFTER_SEEK, sys_read(fd, &c, 1) == 1 && c == 'e');
        check(SETFD_CLOEXEC, syscall3(SYS_FCNTL, fd, F_SETFD, FD_CLOEXEC) == 0);
        check(GETFD_CLOEXEC, syscall3(SYS_FCNTL, fd, F_GETFD, 0) == FD_CLOEXEC);
        syscall1(SYS_CLOSE, fd);
    }
}

/** Row 10: fstatat64 by path gives /g as a regular file of 5 bytes and a new
 * directory, /h, as a directory. */
static void fstatat_rows(void)
{
    /* fstatat64 overwrites st; the initializer quiets cppcheck. */
    unsigned char st[STAT64_SIZE] = {0};

    check(FSTATAT_G, syscall5(SYS_FSTATAT, AT_FDCWD, (long)"/g", (long)st, 0, 0) == 0);
    check(FSTATAT_G_REGULAR, (STAT64_MODE(st) & S_IFMT) == S_IFREG && STAT64_SIZE_LO(st) == 5);
    check(MKDIR_H, syscall2(SYS_MKDIR, (long)"/h", 0755) == 0);
    check(FSTATAT_H, syscall5(SYS_FSTATAT, AT_FDCWD, (long)"/h", (long)st, 0, 0) == 0);
    check(FSTATAT_H_DIR, (STAT64_MODE(st) & S_IFMT) == S_IFDIR);
    check(RMDIR_H, syscall1(SYS_RMDIR, (long)"/h") == 0);
}

/**
 * Row 11: O_APPEND, the minimum of F_DUPFD, and the mode O_CREAT stamps. A
 * write of '!' through an O_APPEND descriptor after a seek to 0 goes to the
 * end of /g, giving "hello!". /g is removed afterwards.
 */
static void append_rows(void)
{
    /* fstat64 overwrites st; the initializer just quiets cppcheck. */
    unsigned char st[STAT64_SIZE] = {0};
    long long result = 0;
    char rb[8];
    int fd, dupfd, r;

    fd = syscall3(SYS_OPENAT, AT_FDCWD, (long)"/g", O_WRONLY | O_APPEND);
    check(OPENAT_APPEND, fd >= 0);
    if (fd >= 0) {
        probe_llseek(fd, 0, 0, &result); /* SEEK_SET 0 */
        check(APPEND_WRITE, sys_write(fd, "!", 1) == 1);
        syscall1(SYS_CLOSE, fd);
    }
    fd = syscall3(SYS_OPENAT, AT_FDCWD, (long)"/g", O_RDONLY);
    if (fd >= 0) {
        r = sys_read(fd, rb, sizeof rb);
        check(APPEND_AT_END, r == 6 && rb[5] == '!');
        dupfd = syscall3(SYS_FCNTL, fd, F_DUPFD, 10);
        check(DUPFD_MINIMUM, dupfd >= 10);
        if (dupfd >= 0) {
            syscall1(SYS_CLOSE, dupfd);
        }
        syscall1(SYS_CLOSE, fd);
    }
    /* openat's mode is its fourth argument, which syscall3 omits. */
    fd = syscall5(SYS_OPENAT, AT_FDCWD, (long)"/m", O_WRONLY | O_CREAT, 0644, 0);
    check(OPENAT_CREAT, fd >= 0);
    if (fd >= 0) {
        check(FSTAT_M, syscall2(SYS_FSTAT, fd, (long)st) == 0);
        check(CREAT_MODE, (STAT64_MODE(st) & 0777) == 0644);
        syscall1(SYS_CLOSE, fd);
    }
    check(UNLINK_M, syscall1(SYS_UNLINK, (long)"/m") == 0);
    check(UNLINK_G, syscall1(SYS_UNLINK, (long)"/g") == 0);
}

/**
 * Row 12, the first half: chdir to /work and getcwd; a file made there by a
 * relative name is /work/rel.txt, and openat of the relative name through a
 * descriptor of /work reaches it too.
 */
static void relative_rows(void)
{
    char cwd[64];
    char rb[8];
    int fd, n, dfd;

    check(MKDIR_WORK, syscall2(SYS_MKDIR, (long)"/work", 0755) == 0);
    check(CHDIR_WORK, syscall1(SYS_CHDIR, (long)"/work") == 0);
    n = syscall2(SYS_GETCWD, (long)cwd, sizeof cwd);
    check(GETCWD_WORK, n > 0 && cwd[0] == '/' && cwd[1] == 'w' && cwd[2] == 'o' && cwd[3] == 'r' &&
                           cwd[4] == 'k' && cwd[5] == '\0');
    fd = syscall2(SYS_CREAT, (long)"rel.txt", 0644);
    check(CREAT_RELATIVE, fd >= 0);
    if (fd >= 0) {
        check(WRITE_RELATIVE, sys_write(fd, "deep", 4) == 4);
        syscall1(SYS_CLOSE, fd);
    }
    fd = syscall3(SYS_OPEN, (long)"/work/rel.txt", O_RDONLY, 0);
    check(OPEN_ABSOLUTE, fd >= 0);
    if (fd >= 0) {
        int r = sys_read(fd, rb, sizeof rb);
        check(RELATIVE_LANDED, r == 4 && rb[0] == 'd');
        syscall1(SYS_CLOSE, fd);
    }
    dfd = syscall3(SYS_OPENAT, AT_FDCWD, (long)"/work", O_RDONLY);
    check(OPEN_WORK_FD, dfd >= 0);
    if (dfd >= 0) {
        fd = syscall3(SYS_OPENAT, dfd, (long)"rel.txt", O_RDONLY);
        check(OPENAT_DIRFD, fd >= 0);
        if (fd >= 0) {
            int r = sys_read(fd, rb, sizeof rb);
            check(DIRFD_READ, r == 4 && rb[0] == 'd');
            syscall1(SYS_CLOSE, fd);
        }
        syscall1(SYS_CLOSE, dfd);
    }
}

/**
 * Row 12, the second half, from /work: a path through a regular file is
 * ENOTDIR and not ENOENT; /work/.. is the root, a directory; chdir .. makes
 * getcwd give /; and /work is removed.
 */
static void parent_rows(void)
{
    /* fstatat64 overwrites rst; the initializer quiets cppcheck. */
    unsigned char rst[STAT64_SIZE] = {0};
    char cwd[64];
    int n;

    check(CHDIR_ENOTDIR, syscall1(SYS_CHDIR, (long)"rel.txt/x") == -ENOTDIR);
    check(FSTATAT_DOTDOT, syscall5(SYS_FSTATAT, AT_FDCWD, (long)"/work/..", (long)rst, 0, 0) == 0);
    check(DOTDOT_ROOT, (STAT64_MODE(rst) & S_IFMT) == S_IFDIR);
    check(CHDIR_DOTDOT, syscall1(SYS_CHDIR, (long)"..") == 0);
    n = syscall2(SYS_GETCWD, (long)cwd, sizeof cwd);
    check(GETCWD_ROOT, n > 0 && cwd[0] == '/' && cwd[1] == '\0');
    check(UNLINK_REL, syscall1(SYS_UNLINK, (long)"/work/rel.txt") == 0);
    check(RMDIR_WORK, syscall1(SYS_RMDIR, (long)"/work") == 0);
}

/** Whether the check `id` is one of the branch of the root `root` does not
 * take. */
static int other_branch(int id, int root)
{
    if (root == ROOT_EXT2) {
        return id == GETDENTS_KRFFS || (id >= MKDIR_IDS && id <= RMDIR_IDS);
    }
    return id == GETDENTS_EXT2;
}

/**
 * Report the branch of the group `root`, ext2, krffs or other, and each
 * check of it: held, failed with its text, or not run, since a check it
 * depends on failed or, on another root, since the KRFFS rows need KRFFS;
 * and end the report.
 */
static void report_checks(int root)
{
    int i;

    report_branch("root", root == ROOT_EXT2 ? "ext2" : root == ROOT_KRFFS ? "krffs" : "other");
    for (i = 0; i < CHECKS; i++) {
        if (other_branch(i, root)) {
            continue;
        }
        if (outcome[i] == 1) {
            report_pass(checks[i].id);
        } else if (outcome[i] == 2) {
            report_fail(checks[i].id, checks[i].text);
        } else if (i >= MKDIR_IDS && i <= RMDIR_IDS && root != ROOT_KRFFS) {
            report_notrun(checks[i].id, "the root is not krffs");
        } else {
            report_notrun(checks[i].id, "a check it depends on failed");
        }
    }
    report_end();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe: the numbered rows in order, each check counted by `check`, and
 * then the report. `_start` calls it with the initial stack, whose
 * environment gives the case's identity.
 */
void probe_main(unsigned long *sp)
{
    int root;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.dirprobe"));
    root = root_type();
    check(ROOT_TYPE, root == ROOT_KRFFS || root == ROOT_EXT2);
    if (root == ROOT_EXT2) {
        puts("dirprobe: the root is ext2");
    } else if (root == ROOT_KRFFS) {
        puts("dirprobe: the root is krffs");
    }
    file_rows();
    listing_rows(root);
    at_rows();
    fstatat_rows();
    append_rows();
    relative_rows();
    parent_rows();
    report_checks(root);
    if (fails == 0) {
        puts("DIRPROBE OK");
    } else {
        puts("DIRPROBE FAIL");
    }
    sys_exit(fails);
}
