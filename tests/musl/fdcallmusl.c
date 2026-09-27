/**
 * The descriptor forms of the calls that take a path, which are what GNU
 * libc's fstat and futimens are: fstatat64 with an empty path and
 * AT_EMPTY_PATH.
 *
 * musl reaches none of them: its fstat is fstat64, and its fstatat turns an
 * empty path into fstat64 as well. So the rows call the kernel by number,
 * with the stat64 layout the kernel writes, and require the empty-path form
 * to report what fstat64 reports for the same descriptor, byte for byte, on
 * each kind of descriptor there is: a file, a directory, a pipe, a socket, a
 * device and the terminal. With AT_FDCWD the empty path is the working
 * directory, which must report what a path to it reports.
 *
 * Prints "FDCALLMUSL OK" on success and "FDCALLMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

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

int main(void)
{
    int rc = stat_rows();

    if (rc == 0) {
        printf("FDCALLMUSL OK\n");
    }
    return rc;
}
