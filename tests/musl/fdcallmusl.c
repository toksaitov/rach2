/**
 * The descriptor forms of the calls that take a path, which are what GNU
 * libc's fstat and futimens are: fstatat64 with an empty path and
 * AT_EMPTY_PATH, and utimensat with a NULL path.
 *
 * musl reaches few of them: its fstat is fstat64, and its fstatat turns an
 * empty path into fstat64 as well. So the rows call the kernel by number,
 * with the stat64 layout the kernel writes, and require the empty-path form
 * to report what fstat64 reports for the same descriptor, byte for byte, on
 * each kind of descriptor there is: a file, a directory, a pipe, a socket, a
 * device and the terminal. With AT_FDCWD the empty path is the working
 * directory, which must report what a path to it reports.
 *
 * utimensat with a NULL path sets the times of the file the descriptor is
 * open on, and so does an empty path with AT_EMPTY_PATH; musl's futimens is
 * the same call. The rows set explicit times and read them back, and ask for
 * each refusal Linux makes of a NULL path: a flag, AT_FDCWD, a descriptor
 * that is not open, and a nanosecond count out of range, besides the pipe
 * that has no times to set. Two UTIME_OMIT return 0 before anything else is
 * looked at.
 *
 * /dev/fd/N and /proc/self/fd/N open what descriptor N is open on: a pipe's
 * read end opened by its link reads what the pipe's writer wrote, as bash's
 * process substitution reads it, and a file opened by its link starts a
 * description of its own at offset 0; /dev/stdin is descriptor 0's file.
 *
 * Prints "FDCALLMUSL OK" on success and "FDCALLMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* The file and the directory the time rows make and remove. */
#define FILE_PATH "/fdcall.tmp"
#define DIR_PATH  "/fdcalldir"

/* An instant with a nanosecond part, so a store that drops it shows. */
#define WHEN_SEC  1786712707L
#define WHEN_NSEC 123456789L

/* The i386 stat64 as the kernel writes it (man/man2/stat.2). */
struct kstat64 {
    unsigned long long st_dev;
    unsigned char pad0[4];
    unsigned long old_st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned long st_uid;
    unsigned long st_gid;
    unsigned long long st_rdev;
    unsigned char pad3[4];
    long long st_size;
    unsigned long st_blksize;
    unsigned long long st_blocks;
    unsigned long st_atime_sec;
    unsigned long st_atime_nsec;
    unsigned long st_mtime_sec;
    unsigned long st_mtime_nsec;
    unsigned long st_ctime_sec;
    unsigned long st_ctime_nsec;
    unsigned long long st_ino;
};

/* The timespec utimensat (320) takes on i386, with 32-bit seconds. */
struct ktimespec {
    long tv_sec;
    long tv_nsec;
};

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("FDCALLMUSL FAIL (%s)\n", what);

    return 1;
}

/** Whether a raw call returned -1 with `err`. */
static int refused(long rc, int err)
{
    return rc == -1 && errno == err;
}

/**
 * Whether fstatat64 of `fd` with an empty path and AT_EMPTY_PATH reports what
 * fstat64 reports, and the file type is `type`. Each buffer starts with its
 * own fill, so a field the call leaves alone shows as a difference.
 */
static int same_as_fstat(int fd, unsigned int type, const char *path)
{
    struct kstat64 by_fd, by_empty;

    memset(&by_fd, 0xAA, sizeof(by_fd));
    memset(&by_empty, 0x55, sizeof(by_empty));
    if (syscall(SYS_fstat64, fd, &by_fd) != 0) {
        return 0;
    }
    if (syscall(SYS_fstatat64, fd, path, &by_empty, AT_EMPTY_PATH) != 0) {
        return 0;
    }
    return memcmp(&by_fd, &by_empty, sizeof(by_fd)) == 0 && (by_fd.st_mode & 0170000) == type;
}

/** The empty path on each kind of descriptor. */
static int stat_rows(void)
{
    struct kstat64 by_path, by_empty;
    int pipefd[2];
    int fd;
    int ok;

    fd = open("/hello.txt", O_RDONLY);
    ok = fd >= 0 && same_as_fstat(fd, 0100000, "");
    close(fd);
    if (!ok) {
        return fail("a regular file");
    }
    fd = open("/", O_RDONLY | O_DIRECTORY);
    ok = fd >= 0 && same_as_fstat(fd, 0040000, "");
    close(fd);
    if (!ok) {
        return fail("a directory");
    }
    if (pipe(pipefd) != 0) {
        return fail("pipe");
    }
    ok = same_as_fstat(pipefd[0], 0010000, "") && same_as_fstat(pipefd[1], 0010000, "");
    close(pipefd[0]);
    close(pipefd[1]);
    if (!ok) {
        return fail("a pipe");
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    ok = fd >= 0 && same_as_fstat(fd, 0140000, "");
    close(fd);
    if (!ok) {
        return fail("a socket");
    }
    fd = open("/dev/null", O_RDWR);
    ok = fd >= 0 && same_as_fstat(fd, 0020000, "");
    close(fd);
    if (!ok) {
        return fail("a device");
    }
    if (!same_as_fstat(1, 0020000, "")) {
        return fail("the terminal");
    }
    fd = open("/hello.txt", O_RDONLY);
    ok = fd >= 0 && same_as_fstat(fd, 0100000, NULL);
    close(fd);
    if (!ok) {
        return fail("a null path with AT_EMPTY_PATH is not the descriptor");
    }

    /* AT_FDCWD: the working directory, as a path to it reports it. */
    if (chdir("/dev") != 0) {
        return fail("chdir to /dev");
    }
    memset(&by_path, 0xAA, sizeof(by_path));
    memset(&by_empty, 0x55, sizeof(by_empty));
    ok = syscall(SYS_stat64, "/dev", &by_path) == 0 &&
         syscall(SYS_fstatat64, AT_FDCWD, "", &by_empty, AT_EMPTY_PATH) == 0 &&
         memcmp(&by_path, &by_empty, sizeof(by_path)) == 0;
    chdir("/");
    if (!ok) {
        return fail("an empty path with AT_FDCWD is not the working directory");
    }

    /* A path that is not empty is looked up as ever, the flag or not. */
    memset(&by_path, 0xAA, sizeof(by_path));
    memset(&by_empty, 0x55, sizeof(by_empty));
    if (syscall(SYS_stat64, "/hello.txt", &by_path) != 0 ||
        syscall(SYS_fstatat64, AT_FDCWD, "/hello.txt", &by_empty, AT_EMPTY_PATH) != 0 ||
        memcmp(&by_path, &by_empty, sizeof(by_path)) != 0) {
        return fail("a path with AT_EMPTY_PATH is not looked up");
    }

    if (!refused(syscall(SYS_fstatat64, 0, "", &by_empty, 0), ENOENT)) {
        return fail("an empty path without AT_EMPTY_PATH is not ENOENT");
    }
    if (!refused(syscall(SYS_fstatat64, 999, "", &by_empty, AT_EMPTY_PATH), EBADF)) {
        return fail("AT_EMPTY_PATH on a descriptor that is not open is not EBADF");
    }
    if (!refused(syscall(SYS_fstatat64, AT_FDCWD, "/", &by_empty, 0x8000), EINVAL)) {
        return fail("an unknown flag with the root's path is not EINVAL");
    }
    return 0;
}

/** Whether the file at `path`, or open on `fd` when `path` is NULL, has
 * the access and modification times `sec` and `nsec`. */
static int times_are(int fd, const char *path, long sec, long nsec)
{
    struct kstat64 st;
    long rc = path ? syscall(SYS_stat64, path, &st) : syscall(SYS_fstat64, fd, &st);

    return rc == 0 && (long)st.st_atime_sec == sec && (long)st.st_atime_nsec == nsec &&
           (long)st.st_mtime_sec == sec && (long)st.st_mtime_nsec == nsec;
}

/** Both times of `kt` set to `sec` and `nsec`. */
static void set_both(struct ktimespec *kt, long sec, long nsec)
{
    kt[0].tv_sec = sec;
    kt[0].tv_nsec = nsec;
    kt[1].tv_sec = sec;
    kt[1].tv_nsec = nsec;
}

/** utimensat with a NULL path, and with an empty one and AT_EMPTY_PATH. */
static int utimens_rows(int fd)
{
    struct ktimespec kt[2];
    struct timespec ts[2];
    struct kstat64 st;
    int pipefd[2];
    long before;
    long after;
    long rc;

    set_both(kt, WHEN_SEC, WHEN_NSEC);
    if (syscall(SYS_utimensat, fd, NULL, kt, 0) != 0 || !times_are(fd, NULL, WHEN_SEC, WHEN_NSEC)) {
        return fail("a null path does not set the times of the descriptor's file");
    }
    ts[0].tv_sec = WHEN_SEC + 60;
    ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    if (futimens(fd, ts) != 0 || !times_are(fd, NULL, WHEN_SEC + 60, 0)) {
        return fail("musl's futimens does not set the times");
    }
    set_both(kt, WHEN_SEC + 120, 5);
    if (syscall(SYS_utimensat, fd, "", kt, AT_EMPTY_PATH) != 0 ||
        !times_are(fd, NULL, WHEN_SEC + 120, 5)) {
        return fail("an empty path with AT_EMPTY_PATH does not set the descriptor's times");
    }
    before = (long)time(NULL);
    if (syscall(SYS_utimensat, fd, NULL, NULL, 0) != 0 || syscall(SYS_fstat64, fd, &st) != 0) {
        return fail("a null path with null times");
    }
    after = (long)time(NULL);
    if ((long)st.st_mtime_sec < before || (long)st.st_mtime_sec > after) {
        return fail("a null path with null times does not set the time to now");
    }

    if (!refused(syscall(SYS_utimensat, fd, NULL, kt, AT_SYMLINK_NOFOLLOW), EINVAL)) {
        return fail("a null path with AT_SYMLINK_NOFOLLOW is not EINVAL");
    }
    if (!refused(syscall(SYS_utimensat, AT_FDCWD, NULL, kt, 0), EFAULT)) {
        return fail("a null path with AT_FDCWD is not EFAULT");
    }
    if (!refused(syscall(SYS_utimensat, 999, NULL, kt, 0), EBADF)) {
        return fail("a null path on a descriptor that is not open is not EBADF");
    }
    if (!refused(syscall(SYS_utimensat, AT_FDCWD, FILE_PATH, kt, 0x8000), EINVAL)) {
        return fail("an unknown flag is not EINVAL");
    }
    kt[1].tv_nsec = 1000000000L;
    if (!refused(syscall(SYS_utimensat, fd, NULL, kt, 0), EINVAL)) {
        return fail("a nanosecond count out of range is not EINVAL");
    }
    kt[0].tv_nsec = UTIME_OMIT;
    kt[1].tv_nsec = UTIME_OMIT;
    if (syscall(SYS_utimensat, 999, NULL, kt, 0x8000) != 0) {
        return fail("two UTIME_OMIT look at the descriptor and the flags");
    }
    if (pipe(pipefd) != 0) {
        return fail("pipe");
    }
    rc = syscall(SYS_utimensat, pipefd[0], NULL, NULL, 0);
    close(pipefd[0]);
    close(pipefd[1]);
    if (!refused(rc, EROFS)) {
        return fail("a pipe, which has no times, is not EROFS");
    }

    /* AT_FDCWD with AT_EMPTY_PATH: the working directory. */
    if (mkdir(DIR_PATH, 0755) != 0 || chdir(DIR_PATH) != 0) {
        return fail("the working directory for the time rows");
    }
    set_both(kt, WHEN_SEC, WHEN_NSEC);
    rc = syscall(SYS_utimensat, AT_FDCWD, "", kt, AT_EMPTY_PATH);
    chdir("/");
    if (rc != 0 || !times_are(-1, DIR_PATH, WHEN_SEC, WHEN_NSEC)) {
        return fail("an empty path with AT_FDCWD does not set the working directory's times");
    }
    return 0;
}

/* Opens of descriptors by their links. */
static int fd_link_rows(void)
{
    char path[32], buf[8];
    struct stat a, b;
    int p[2], r, f, g;

    if (pipe(p) != 0 || write(p[1], "sub", 3) != 3) {
        return fail("a pipe for /dev/fd");
    }
    snprintf(path, sizeof(path), "/dev/fd/%d", p[0]);
    r = open(path, O_RDONLY);
    if (r < 0 || read(r, buf, 3) != 3 || memcmp(buf, "sub", 3) != 0) {
        return fail("a pipe opened by /dev/fd/N did not read the pipe");
    }
    close(r);
    close(p[0]);
    close(p[1]);
    f = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (f < 0 || write(f, "abc", 3) != 3) {
        return fail("a file for /proc/self/fd");
    }
    snprintf(path, sizeof(path), "/proc/self/fd/%d", f);
    g = open(path, O_RDONLY);
    if (g < 0 || read(g, buf, 3) != 3 || memcmp(buf, "abc", 3) != 0 || lseek(f, 0, SEEK_CUR) != 3) {
        return fail("a file opened by /proc/self/fd/N was not a description of its own");
    }
    close(g);
    close(f);
    if (stat("/dev/stdin", &a) != 0 || fstat(0, &b) != 0 || a.st_ino != b.st_ino ||
        a.st_rdev != b.st_rdev || stat("/dev/fd", &a) != 0 || !S_ISDIR(a.st_mode)) {
        return fail("/dev/stdin is not descriptor 0's file, or /dev/fd no directory");
    }
    return 0;
}

int main(void)
{
    int rc = stat_rows();
    int fd;

    if (rc == 0) {
        rc = fd_link_rows();
    }
    unlink(FILE_PATH);
    rmdir(DIR_PATH);
    if (rc == 0) {
        fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
        rc = fd < 0 ? fail("could not create the file") : utimens_rows(fd);
        close(fd);
    }
    unlink(FILE_PATH);
    rmdir(DIR_PATH);
    if (rc == 0) {
        printf("FDCALLMUSL OK\n");
    }
    return rc;
}
