/**
 * truncate by path: truncate64, which musl's and GNU libc's truncate are,
 * with the length in ECX and EDX, and the older call 92 with a signed 32-bit
 * length in ECX.
 *
 * The rows shorten and lengthen a file through both, and require an
 * extension to read as zeros and a descriptor open on the file to see each
 * new length, including one the file has to move to reach. An unprivileged
 * caller's truncation drops the set-id bits, one the file has to move for
 * included, a privileged caller's keeps them, and a caller without write
 * permission is refused. Then each refusal, in Linux's order: a negative
 * length before the path is looked at, a missing file, a directory, a
 * device, a /proc file, a file used as a directory, a path too long, and a
 * length past what the file system can hold.
 *
 * Prints "TRUNCMUSL OK" on success and "TRUNCMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* The file the rows make and remove. */
#define FILE_PATH "/trunc.tmp"

/* A length the file cannot reach without moving. */
#define FAR 65536

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("TRUNCMUSL FAIL (%s)\n", what);

    return 1;
}

/** Whether a call returned -1 with `err`. */
static int refused(long rc, int err)
{
    return rc == -1 && errno == err;
}

/** Whether the file open on `fd` is `size` bytes long. */
static int size_is(int fd, off_t size)
{
    struct stat st;

    return fstat(fd, &st) == 0 && st.st_size == size;
}

/** Whether the file's permission bits, set-id bits included, are `mode`. */
static int mode_is(mode_t mode)
{
    struct stat st;

    return stat(FILE_PATH, &st) == 0 && (st.st_mode & 07777) == mode;
}

/**
 * Truncate the file to `length` as uid and gid 1000 in a child, and return
 * whether the call returned `want`, or failed with `err` when `want` is -1.
 */
static int as_user(off_t length, int want, int err)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        if (setgid(1000) != 0 || setuid(1000) != 0) {
            _exit(2);
        }
        if (want == 0) {
            _exit(truncate(FILE_PATH, length) == 0 ? 0 : 1);
        }
        _exit(refused(truncate(FILE_PATH, length), err) ? 0 : 1);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/** Lengths through both calls, as a descriptor open on the file sees them. */
static int length_rows(void)
{
    char buf[16];
    int fd;

    unlink(FILE_PATH);
    fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, "0123456789", 10) != 10) {
        return fail("make the file");
    }
    if (truncate(FILE_PATH, 4) != 0 || !size_is(fd, 4) || pread(fd, buf, 16, 0) != 4 ||
        memcmp(buf, "0123", 4) != 0) {
        return fail("shorten");
    }
    memset(buf, 'x', sizeof(buf));
    if (truncate(FILE_PATH, 12) != 0 || !size_is(fd, 12) || pread(fd, buf, 16, 0) != 12 ||
        memcmp(buf, "0123\0\0\0\0\0\0\0\0", 12) != 0) {
        return fail("lengthen with zeros");
    }
    if (syscall(SYS_truncate, FILE_PATH, 2L) != 0 || !size_is(fd, 2)) {
        return fail("call 92");
    }
    if (syscall(SYS_truncate64, FILE_PATH, 7L, 0L) != 0 || !size_is(fd, 7)) {
        return fail("call 193 with the length in two words");
    }
    memset(buf, 'x', sizeof(buf));
    if (truncate(FILE_PATH, FAR) != 0 || !size_is(fd, FAR) || pread(fd, buf, 4, FAR - 4) != 4 ||
        memcmp(buf, "\0\0\0\0", 4) != 0 || pread(fd, buf, 3, 0) != 3 ||
        memcmp(buf, "01\0", 3) != 0) {
        return fail("a length the file moves to reach");
    }
    if (write(fd, "Z", 1) != 1 || pread(fd, buf, 1, 10) != 1 || buf[0] != 'Z') {
        return fail("write through the descriptor after the move");
    }
    if (truncate(FILE_PATH, 0) != 0 || !size_is(fd, 0)) {
        return fail("truncate to nothing");
    }
    close(fd);
    return 0;
}

/** The set-id bits and write permission, as root and as uid 1000. */
static int identity_rows(void)
{
    if (chmod(FILE_PATH, 06777) != 0 || truncate(FILE_PATH, 3) != 0 || !mode_is(06777)) {
        return fail("root keeps the set-id bits");
    }
    if (!as_user(5, 0, 0) || !mode_is(0777)) {
        return fail("an unprivileged truncate drops the set-id bits");
    }
    if (chmod(FILE_PATH, 06777) != 0 || !as_user(2 * FAR, 0, 0) || !mode_is(0777)) {
        return fail("the set-id bits of a file that moved");
    }
    if (chmod(FILE_PATH, 0644) != 0 || !as_user(1, -1, EACCES)) {
        return fail("no write permission");
    }
    return 0;
}

/** Each refusal, in the order Linux makes them. */
static int refusal_rows(void)
{
    char *longpath;

    if (!refused(truncate(FILE_PATH, -1), EINVAL) ||
        !refused(syscall(SYS_truncate, "/no.such.trunc", -5L), EINVAL) ||
        !refused(syscall(SYS_truncate64, NULL, 0L, 0x80000000L), EINVAL)) {
        return fail("a negative length before the path");
    }
    if (!refused(truncate("/no.such.trunc", 0), ENOENT) || !refused(truncate("", 0), ENOENT)) {
        return fail("a missing file");
    }
    if (!refused(truncate("/", 0), EISDIR) || !refused(truncate("/dev", 0), EISDIR)) {
        return fail("a directory");
    }
    if (!refused(truncate("/dev/null", 0), EINVAL)) {
        return fail("a device");
    }
    if (!refused(truncate("/proc/meminfo", 0), EACCES)) {
        return fail("a /proc file");
    }
    if (!refused(truncate(FILE_PATH "/x", 0), ENOTDIR)) {
        return fail("a file used as a directory");
    }
    longpath = malloc(4097);
    if (!longpath) {
        return fail("allocate the long path");
    }
    memset(longpath, 'a', 4096);
    longpath[0] = '/';
    longpath[4096] = '\0';
    if (!refused(truncate(longpath, 0), ENAMETOOLONG)) {
        free(longpath);
        return fail("a path of 4096 bytes");
    }
    free(longpath);
    if (!refused(syscall(SYS_truncate64, FILE_PATH, 0L, 1L), EFBIG)) {
        return fail("4 GiB on KRFFS");
    }
    return 0;
}

int main(void)
{
    if (length_rows() != 0 || identity_rows() != 0 || refusal_rows() != 0) {
        unlink(FILE_PATH);
        return 1;
    }
    unlink(FILE_PATH);
    printf("TRUNCMUSL OK\n");
    return 0;
}
