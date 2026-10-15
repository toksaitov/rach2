/**
 * opathmusl: check O_PATH descriptors, which refer to a file and open nothing.
 *
 * The rows open a file of the boot root, a directory, /dev/null, /dev/ptmx,
 * /proc/meminfo, /dev and /proc/self with O_PATH, and require what Linux
 * gives: fstat and fstatfs report the file, fchdir enters a directory, the
 * *at calls resolve against one, utimensat with an empty path sets a file's
 * times, the duplications and F_GETFD, F_SETFD and F_GETFL work, and
 * F_GETFL reports O_PATH, on a duplicate made by F_DUPFD or dup too, where
 * F_SETFL, read and write stay EBADF. Read, write, a zero-byte read, pread,
 * lseek, getdents64, fsync, ftruncate, fchmod, fchown, futimens, ioctl and
 * the other fcntl commands are EBADF, and poll reports POLLNVAL. The other
 * open flags are dropped, so nothing is created or truncated; a file with no
 * permission bits opens for an unprivileged caller; /dev/ptmx allocates no
 * terminal; and O_NOFOLLOW opens the /proc/self link itself.
 *
 * Prints `OPATHMUSL OK` and exits 0 on success, and prints
 * `OPATHMUSL FAIL (detail)` and exits 1 otherwise.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* fchown of 32-bit ids: fchown32 on i386, whose fchown takes 16-bit ones,
 * and fchown on x86-64, which has only that. */
#ifndef SYS_fchown32
#define SYS_fchown32 SYS_fchown
#endif

/* The file and the directory the rows make and remove. */
#define FILE_PATH "/opath.tmp"
#define DIR_PATH  "/opathdir"

/* The file systems fstatfs names (man/man2/statfs.2). */
#define DEVFS_MAGIC 0x1373
#define PROC_MAGIC  0x9fa0

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("OPATHMUSL FAIL (%s)\n", what);

    return 1;
}

/** Whether a call returned -1 with `err`. */
static int refused(long rc, int err)
{
    return rc == -1 && errno == err;
}

/** Whether fstat of `fd` and stat of `path` report the same file. */
static int same_file(int fd, const char *path)
{
    struct stat a, b;

    return fstat(fd, &a) == 0 && stat(path, &b) == 0 && a.st_dev == b.st_dev &&
           a.st_ino == b.st_ino && a.st_mode == b.st_mode && a.st_rdev == b.st_rdev;
}

/** Whether fstatfs of `fd` names the file system `magic`. */
static int on_fs(int fd, long magic)
{
    struct statfs sf;

    return fstatfs(fd, &sf) == 0 && (long)sf.f_type == magic;
}

/** The number of names in /dev/pts, or -1. */
static int pts_count(void)
{
    DIR *d = opendir("/dev/pts");
    struct dirent *de;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        n++;
    }
    closedir(d);
    return n;
}

/**
 * Whether every call that must fail with EBADF on a path descriptor does so
 * on `fd`, and poll reports POLLNVAL. fchmod and fchown are called by number,
 * since musl's wrappers retry an EBADF through /proc/self/fd, which is how
 * musl serves an O_PATH descriptor on a kernel that fails them.
 */
static int refuses_io(int fd)
{
    char buf[8];
    struct timespec ts[2];
    struct pollfd pfd;
    fd_set set;
    struct timeval tv;
    int avail;

    ts[0].tv_sec = ts[1].tv_sec = 0;
    ts[0].tv_nsec = ts[1].tv_nsec = UTIME_NOW;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    return refused(read(fd, buf, sizeof(buf)), EBADF) && refused(read(fd, buf, 0), EBADF) &&
           refused(write(fd, "x", 1), EBADF) && refused(pread(fd, buf, 1, 0), EBADF) &&
           refused(lseek(fd, 0, SEEK_SET), EBADF) && refused(fsync(fd), EBADF) &&
           refused(ftruncate(fd, 0), EBADF) && refused(syscall(SYS_fchmod, fd, 0644), EBADF) &&
           refused(syscall(SYS_fchown32, fd, 0, 0), EBADF) && refused(futimens(fd, ts), EBADF) &&
           refused(ioctl(fd, FIONREAD, &avail), EBADF) &&
           refused(fcntl(fd, F_SETFL, O_NONBLOCK), EBADF) &&
           refused(syscall(SYS_getdents64, fd, buf, sizeof(buf)), EBADF) && poll(&pfd, 1, 0) == 1 &&
           pfd.revents == POLLNVAL && refused(select(fd + 1, &set, NULL, NULL, &tv), EBADF);
}

/** Whether `fd` is a path descriptor still: F_GETFL reports O_PATH, and
 * F_SETFL, read and write are EBADF. */
static int keeps_path(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    char c;

    return flags != -1 && (flags & O_PATH) == O_PATH &&
           refused(fcntl(fd, F_SETFL, O_NONBLOCK), EBADF) && refused(read(fd, &c, 1), EBADF) &&
           refused(write(fd, "x", 1), EBADF);
}

/**
 * Check an O_PATH descriptor on a file of the boot root, its duplicates, and
 * the flags O_PATH drops and keeps. Leaves `FILE_PATH` for the later rows.
 * Returns 0, or 1 once the failure is reported.
 */
static int file_rows(void)
{
    struct timespec ts[2];
    struct stat st;
    int fd, dup_fd, flags;

    unlink(FILE_PATH);
    fd = open(FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "abc", 3) != 3) {
        return fail("make the file");
    }
    close(fd);
    fd = open(FILE_PATH, O_PATH | O_RDWR | O_TRUNC | O_APPEND);
    if (fd < 0 || stat(FILE_PATH, &st) != 0 || st.st_size != 3) {
        return fail("O_TRUNC is dropped");
    }
    flags = fcntl(fd, F_GETFL);
    if (flags == -1 || (flags & O_PATH) != O_PATH || (flags & 3) != O_RDONLY ||
        (flags & O_APPEND) != 0) {
        return fail("F_GETFL reports O_PATH and nothing else");
    }
    if (!same_file(fd, FILE_PATH) || !refuses_io(fd)) {
        return fail("a file of the boot root");
    }
    if (fcntl(fd, F_GETFD) != 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(fd, F_GETFD) != FD_CLOEXEC) {
        return fail("F_GETFD and F_SETFD");
    }
    dup_fd = fcntl(fd, F_DUPFD, 10);
    if (dup_fd < 10 || fcntl(dup_fd, F_GETFD) != 0 || !same_file(dup_fd, FILE_PATH) ||
        !keeps_path(dup_fd)) {
        return fail("F_DUPFD keeps O_PATH");
    }
    close(dup_fd);
    dup_fd = dup(fd);
    if (dup_fd < 0 || !keeps_path(dup_fd)) {
        return fail("dup keeps O_PATH");
    }
    close(dup_fd);
    ts[0].tv_sec = ts[1].tv_sec = 1786712707;
    ts[0].tv_nsec = ts[1].tv_nsec = 0;
    if (utimensat(fd, "", ts, AT_EMPTY_PATH) != 0 || stat(FILE_PATH, &st) != 0 ||
        st.st_mtime != 1786712707) {
        return fail("utimensat with AT_EMPTY_PATH");
    }
    if (close(fd) != 0 || !refused(close(fd), EBADF)) {
        return fail("close");
    }
    unlink("/opath.none");
    if (!refused(open("/opath.none", O_PATH | O_CREAT | O_WRONLY, 0644), ENOENT) ||
        access("/opath.none", F_OK) == 0) {
        return fail("O_CREAT is dropped");
    }
    fd = open(FILE_PATH, O_PATH | O_CLOEXEC);
    if (fd < 0 || fcntl(fd, F_GETFD) != FD_CLOEXEC) {
        return fail("O_CLOEXEC is kept");
    }
    close(fd);
    if (!refused(open(FILE_PATH, O_PATH | O_DIRECTORY), ENOTDIR)) {
        return fail("O_DIRECTORY is checked");
    }
    return 0;
}

/**
 * Check that a forked child that has become uid 1000 opens a file with no
 * permission bits with O_PATH and not with O_RDONLY. Returns 0, or 1 once the
 * failure is reported.
 */
static int permission_rows(void)
{
    pid_t pid;
    int status;

    if (chmod(FILE_PATH, 0) != 0) {
        return fail("chmod 0");
    }
    pid = fork();
    if (pid == 0) {
        int fd;

        if (setgid(1000) != 0 || setuid(1000) != 0) {
            _exit(2);
        }
        if (!refused(open(FILE_PATH, O_RDONLY), EACCES)) {
            _exit(3);
        }
        fd = open(FILE_PATH, O_PATH);
        _exit(fd >= 0 && same_file(fd, FILE_PATH) ? 0 : 4);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail("no permission on the file itself");
    }
    return 0;
}

/**
 * Check an O_PATH descriptor on a directory: the calls that fail on it,
 * fchdir, and the *at calls against it. Returns 0, or 1 once the failure is
 * reported.
 */
static int dir_rows(void)
{
    char cwd[64];
    struct stat st;
    int dfd, fd;

    rmdir(DIR_PATH);
    if (mkdir(DIR_PATH, 0755) != 0) {
        return fail("make the directory");
    }
    fd = open(DIR_PATH "/inner", O_WRONLY | O_CREAT, 0644);
    close(fd);
    dfd = open(DIR_PATH, O_PATH | O_DIRECTORY);
    if (dfd < 0 || !same_file(dfd, DIR_PATH) || !refuses_io(dfd)) {
        return fail("a directory of the boot root");
    }
    fd = openat(dfd, "inner", O_RDONLY);
    if (fd < 0 || fstatat(dfd, "inner", &st, 0) != 0 || !S_ISREG(st.st_mode)) {
        return fail("the *at calls against it");
    }
    close(fd);
    if (fchdir(dfd) != 0 || !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, DIR_PATH) != 0 ||
        chdir("/") != 0) {
        return fail("fchdir");
    }
    close(dfd);
    unlink(DIR_PATH "/inner");
    rmdir(DIR_PATH);
    return 0;
}

/**
 * Check O_PATH descriptors on /dev and /proc: devices, a /proc file, a
 * directory and two links, followed and not. Returns 0, or 1 once the failure
 * is reported.
 */
static int synth_rows(void)
{
    char buf[64], cwd[64], want[64];
    struct stat st;
    int fd, before;
    ssize_t n;

    fd = open("/dev/null", O_PATH);
    snprintf(want, sizeof(want), "/proc/self/fd/%d", fd);
    n = readlink(want, buf, sizeof(buf) - 1);
    if (fd < 0 || !same_file(fd, "/dev/null") || !on_fs(fd, DEVFS_MAGIC) || !refuses_io(fd) ||
        n != 9 || memcmp(buf, "/dev/null", 9) != 0) {
        return fail("/dev/null");
    }
    close(fd);
    before = pts_count();
    fd = open("/dev/ptmx", O_PATH | O_RDWR);
    if (fd < 0 || before < 0 || pts_count() != before || !refuses_io(fd)) {
        return fail("/dev/ptmx allocates no terminal");
    }
    close(fd);
    fd = open("/proc/meminfo", O_PATH);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || !on_fs(fd, PROC_MAGIC) ||
        !refuses_io(fd)) {
        return fail("/proc/meminfo");
    }
    close(fd);
    fd = open("/dev", O_PATH | O_DIRECTORY);
    if (fd < 0 || !same_file(fd, "/dev") || !refuses_io(fd) || fstatat(fd, "null", &st, 0) != 0 ||
        !S_ISCHR(st.st_mode) || fchdir(fd) != 0 || !getcwd(cwd, sizeof(cwd)) ||
        strcmp(cwd, "/dev") != 0 || chdir("/") != 0) {
        return fail("/dev");
    }
    close(fd);
    fd = open("/proc/self", O_PATH | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISLNK(st.st_mode)) {
        return fail("O_NOFOLLOW names /proc/self");
    }
    close(fd);
    fd = open("/proc/self/fd/1", O_PATH | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISLNK(st.st_mode)) {
        return fail("O_NOFOLLOW names a descriptor link");
    }
    close(fd);
    snprintf(want, sizeof(want), "/proc/%d", (int)getpid());
    fd = open("/proc/self", O_PATH);
    if (fd < 0 || fchdir(fd) != 0 || !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, want) != 0 ||
        chdir("/") != 0) {
        return fail("/proc/self followed");
    }
    close(fd);
    return 0;
}

int main(void)
{
    if (file_rows() != 0 || permission_rows() != 0 || dir_rows() != 0 || synth_rows() != 0) {
        unlink(FILE_PATH);
        return 1;
    }
    unlink(FILE_PATH);
    printf("OPATHMUSL OK\n");
    return 0;
}
