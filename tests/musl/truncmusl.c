/**
 * truncmusl: check truncate by path through both of its system calls.
 *
 * On i686 musl's and GNU libc's truncate is truncate64, call 193, with the
 * length in ECX and EDX, and the older call 92 takes a signed 32-bit length
 * in ECX. On x86-64 both are truncate, call 76, with the whole length in RSI.
 *
 * The rows shorten and lengthen a file through both, and require an
 * extension to read as zeros and a descriptor open on the file to see each
 * new length, including one the file has to move to reach. An unprivileged
 * caller's truncation clears the set-id bits, one the file has to move for
 * included, a privileged caller's keeps them, and a caller without write
 * permission fails with EACCES. A file that has to move is made afresh for
 * the row, empty, with a second empty file made right after it, so that
 * nothing behind its node is free, the method tests/fdstale.c describes. Then
 * each error, in Linux's order: a negative length before the path is looked
 * at, a missing file, a directory, a device, a /proc file, a file used as a
 * directory, a path too long, and a length past what the file system can
 * hold.
 *
 * The program runs as root. It prints `TRUNCMUSL OK` and exits 0 on success,
 * and prints `TRUNCMUSL FAIL (detail)` and exits 1 otherwise.
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

/* The file the rows make and remove, and the file made right after it. */
#define FILE_PATH "/trunc.tmp"
#define NEXT_PATH "/trunc.next"

/* A length past the room a new, empty file's node has. */
#define FAR 65536

/*
 * The truncate of a 64-bit length given as its low and high words: i386's
 * truncate64, which takes them in two registers, and x86-64's truncate,
 * which takes the length in one.
 */
#if defined __x86_64__
#define TRUNCATE_WIDE(path, lo, hi) \
    syscall(SYS_truncate, path, (long)((unsigned long)(hi) << 32 | (unsigned long)(lo)))
#define WIDE_ROW "call 76 with the length in one word"
#else
#define TRUNCATE_WIDE(path, lo, hi) syscall(SYS_truncate64, path, lo, hi)
#define WIDE_ROW                    "call 193 with the length in two words"
#endif

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
 * Make the file afresh and empty, and a second empty file right after it,
 * so that the file's first growth past the room its new node has must move
 * it. The allocator is first-fit: the file takes the first free node, and
 * the second file takes the rest of that node, right behind the file, or,
 * when the node was too small to split, what follows it was taken already,
 * since no two free nodes are adjacent (tests/fdstale.c). Returns 0, or -1.
 */
static int fresh_file(void)
{
    int fd, next;

    unlink(FILE_PATH);
    unlink(NEXT_PATH);
    fd = open(FILE_PATH, O_WRONLY | O_CREAT | O_EXCL, 0644);
    next = open(NEXT_PATH, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd >= 0) {
        close(fd);
    }
    if (next >= 0) {
        close(next);
    }
    return fd >= 0 && next >= 0 ? 0 : -1;
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

/**
 * Check lengths set through both calls, as a descriptor open on the file sees
 * them, and a length the file must move to reach. Returns 0, or 1 once the
 * failure is reported.
 */
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
    if (TRUNCATE_WIDE(FILE_PATH, 7L, 0L) != 0 || !size_is(fd, 7)) {
        return fail(WIDE_ROW);
    }
    close(fd);
    if (fresh_file() != 0 || (fd = open(FILE_PATH, O_RDWR)) < 0) {
        return fail("make the file afresh, with a file right after it");
    }
    memset(buf, 'x', sizeof(buf));
    if (truncate(FILE_PATH, FAR) != 0 || !size_is(fd, FAR) || pread(fd, buf, 4, FAR - 4) != 4 ||
        memcmp(buf, "\0\0\0\0", 4) != 0) {
        return fail("a length the file moves to reach");
    }
    if (write(fd, "Z", 1) != 1 || pread(fd, buf, 1, 0) != 1 || buf[0] != 'Z') {
        return fail("write through the descriptor after the move");
    }
    if (truncate(FILE_PATH, 0) != 0 || !size_is(fd, 0)) {
        return fail("truncate to nothing");
    }
    close(fd);
    return 0;
}

/**
 * Check the set-id bits and write permission, as root and as uid 1000.
 * Returns 0, or 1 once the failure is reported.
 */
static int identity_rows(void)
{
    if (chmod(FILE_PATH, 06777) != 0 || truncate(FILE_PATH, 3) != 0 || !mode_is(06777)) {
        return fail("root keeps the set-id bits");
    }
    if (!as_user(5, 0, 0) || !mode_is(0777)) {
        return fail("an unprivileged truncate drops the set-id bits");
    }
    if (fresh_file() != 0 || chmod(FILE_PATH, 06777) != 0 || !as_user(2 * FAR, 0, 0) ||
        !mode_is(0777)) {
        return fail("the set-id bits of a file that moved");
    }
    if (chmod(FILE_PATH, 0644) != 0 || !as_user(1, -1, EACCES)) {
        return fail("no write permission");
    }
    return 0;
}

/**
 * Check each error, in the order Linux checks for them. Returns 0, or 1 once
 * the failure is reported.
 */
static int refusal_rows(void)
{
    char *longpath;

    if (!refused(truncate(FILE_PATH, -1), EINVAL) ||
        !refused(syscall(SYS_truncate, "/no.such.trunc", -5L), EINVAL) ||
        !refused(TRUNCATE_WIDE(NULL, 0L, 0x80000000L), EINVAL)) {
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
    if (!refused(TRUNCATE_WIDE(FILE_PATH, 0L, 1L), EFBIG)) {
        return fail("4 GiB on KRFFS");
    }
    return 0;
}

int main(void)
{
    if (length_rows() != 0 || identity_rows() != 0 || refusal_rows() != 0) {
        unlink(FILE_PATH);
        unlink(NEXT_PATH);
        return 1;
    }
    unlink(FILE_PATH);
    unlink(NEXT_PATH);
    printf("TRUNCMUSL OK\n");
    return 0;
}
