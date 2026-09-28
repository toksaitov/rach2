/**
 * Positioned reads and writes: pread64 and pwrite64, which musl's and GNU
 * libc's pread and pwrite are, with the offset split over ESI and EDI.
 *
 * The rows read and write a file at offsets and require the descriptor's own
 * offset to stay where lseek left it, a write past the end to leave a hole of
 * zeros, and O_APPEND to write at the end whatever offset is asked for, as
 * Linux does. A /proc snapshot and /dev/zero are read at offsets without
 * moving theirs, and a position past 4 GiB in a snapshot is its end. A
 * /proc snapshot and /proc/self/mem seek with SEEK_SET and SEEK_CUR, by
 * _llseek and by the old call 19, and not with SEEK_END. Then
 * each refusal, in Linux's order: a negative offset before the descriptor, a
 * pipe, a socket and the terminal that cannot seek, a descriptor open for the
 * other direction, an end past the signed range, and a directory.
 *
 * Prints "PREADMUSL OK" on success and "PREADMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The file the rows make and remove. */
#define FILE_PATH "/pread.tmp"

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("PREADMUSL FAIL (%s)\n", what);

    return 1;
}

/** Whether a call returned -1 with `err`. */
static int refused(long rc, int err)
{
    return rc == -1 && errno == err;
}

/** Whether descriptor `fd`'s offset is `want`. */
static int offset_is(int fd, off_t want)
{
    return lseek(fd, 0, SEEK_CUR) == want;
}

/** Reads and writes of a file at offsets, and the offset they leave alone. */
static int file_rows(void)
{
    char buf[32];
    struct stat st;
    int fd;
    int ok;

    unlink(FILE_PATH);
    fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, "0123456789", 10) != 10 || lseek(fd, 3, SEEK_SET) != 3) {
        return fail("make the file");
    }
    memset(buf, 0, sizeof(buf));
    if (pread(fd, buf, 4, 5) != 4 || memcmp(buf, "5678", 4) != 0 || !offset_is(fd, 3)) {
        return fail("pread in the middle");
    }
    if (pread(fd, buf, 4, 8) != 2 || memcmp(buf, "89", 2) != 0) {
        return fail("pread across the end");
    }
    if (pread(fd, buf, 4, 100) != 0 || pread(fd, NULL, 0, 0) != 0) {
        return fail("pread past the end or of nothing");
    }
    if (pwrite(fd, "AB", 2, 1) != 2 || !offset_is(fd, 3)) {
        return fail("pwrite in the middle");
    }
    if (pwrite(fd, "Z", 1, 12) != 1 || fstat(fd, &st) != 0 || st.st_size != 13) {
        return fail("pwrite past the end");
    }
    memset(buf, 'x', sizeof(buf));
    if (pread(fd, buf, sizeof(buf), 0) != 13 || memcmp(buf, "0AB3456789\0\0Z", 13) != 0) {
        return fail("the file after the writes");
    }
    if (read(fd, buf, 2) != 2 || memcmp(buf, "34", 2) != 0 || !offset_is(fd, 5)) {
        return fail("read goes on from the offset lseek left");
    }
    if (syscall(SYS_pread64, fd, buf, 3, 7, 0) != 3 || memcmp(buf, "789", 3) != 0) {
        return fail("the offset in ESI and EDI");
    }
    close(fd);

    fd = open(FILE_PATH, O_WRONLY | O_APPEND);
    ok = fd >= 0 && pwrite(fd, "E", 1, 0) == 1 && offset_is(fd, 0);
    close(fd);
    fd = open(FILE_PATH, O_RDONLY);
    memset(buf, 0, sizeof(buf));
    ok = ok && fd >= 0 && pread(fd, buf, sizeof(buf), 0) == 14 && buf[0] == '0' && buf[13] == 'E';
    close(fd);
    if (!ok) {
        return fail("O_APPEND writes at the end");
    }
    return 0;
}

/** A /proc snapshot and /dev/zero read at offsets. */
static int device_rows(void)
{
    char whole[64], part[16], next[8], at[8];
    int fd;
    int ok;

    fd = open("/proc/meminfo", O_RDONLY);
    ok = fd >= 0 && read(fd, whole, 32) == 32;
    ok = ok && pread(fd, part, 8, 4) == 8 && memcmp(part, whole + 4, 8) == 0;
    ok = ok && pread(fd, at, 8, 32) == 8 && read(fd, next, 8) == 8 && memcmp(at, next, 8) == 0;
    ok = ok && pread(fd, part, 8, 0x100000000LL) == 0;
    close(fd);
    if (!ok) {
        return fail("a /proc snapshot");
    }
    fd = open("/dev/zero", O_RDWR);
    memset(part, 'x', sizeof(part));
    ok = fd >= 0 && pread(fd, part, 8, 0x10000000000LL) == 8 &&
         memcmp(part, "\0\0\0\0\0\0\0\0", 8) == 0;
    ok = ok && pwrite(fd, "abc", 3, 5) == 3;
    close(fd);
    if (!ok) {
        return fail("/dev/zero");
    }
    fd = open("/dev/null", O_RDONLY);
    ok = fd >= 0 && pread(fd, part, 8, 100) == 0;
    close(fd);
    if (!ok) {
        return fail("/dev/null");
    }
    return 0;
}

/* The bytes the /proc/self/mem row reads back from the probe's own memory. */
static const char mem_mark[] = "procseek";

/**
 * lseek of a /proc snapshot and of /proc/self/mem, as Linux seeks them:
 * SEEK_SET and SEEK_CUR move the offset, by _llseek and by the old call 19,
 * and the read after each finds the bytes at the offset returned, so an
 * offset returned and not kept fails; SEEK_END is EINVAL, and an address of
 * /proc/self/mem reads what the probe has there.
 */
static int proc_seek_rows(void)
{
    char whole[16], part[8];
    off_t at = (off_t)(uintptr_t)mem_mark;
    int fd;
    int ok;

    fd = open("/proc/meminfo", O_RDONLY);
    ok = fd >= 0 && read(fd, whole, 16) == 16 && lseek(fd, 4, SEEK_SET) == 4;
    ok = ok && read(fd, part, 8) == 8 && memcmp(part, whole + 4, 8) == 0;
    ok = ok && lseek(fd, -6, SEEK_CUR) == 6 && read(fd, part, 8) == 8 &&
         memcmp(part, whole + 6, 8) == 0 && refused(lseek(fd, 0, SEEK_END), EINVAL);
    ok = ok && syscall(SYS_lseek, fd, 2, SEEK_SET) == 2 && read(fd, part, 8) == 8 &&
         memcmp(part, whole + 2, 8) == 0;
    close(fd);
    fd = open("/proc/self/mem", O_RDONLY);
    ok = ok && fd >= 0 && lseek(fd, at, SEEK_SET) == at && read(fd, part, 8) == 8 &&
         memcmp(part, mem_mark, 8) == 0;
    close(fd);
    if (!ok) {
        return fail("lseek of a /proc snapshot or of /proc/self/mem");
    }
    return 0;
}

/** Each refusal, in the order Linux makes them. */
static int refusal_rows(void)
{
    char buf[8];
    int pipefd[2];
    int fd, dirfd;
    int ok;

    if (!refused(pread(999, buf, 1, -1), EINVAL) || !refused(pread(999, buf, 1, 0), EBADF)) {
        return fail("a negative offset before the descriptor");
    }
    if (!refused(syscall(SYS_pwrite64, 999, buf, 1, 0, 0x80000000L), EINVAL)) {
        return fail("a high word with its sign bit set");
    }
    if (pipe(pipefd) != 0) {
        return fail("make a pipe");
    }
    ok = refused(pread(pipefd[0], buf, 1, 0), ESPIPE) &&
         refused(pwrite(pipefd[1], "p", 1, 0), ESPIPE) &&
         refused(pread(pipefd[1], buf, 1, 0), ESPIPE);
    close(pipefd[0]);
    close(pipefd[1]);
    if (!ok) {
        return fail("a pipe");
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    ok = fd >= 0 && refused(pread(fd, buf, 1, 0), ESPIPE);
    close(fd);
    if (!ok) {
        return fail("a socket");
    }
    if (!refused(pwrite(1, "t", 0, 0), ESPIPE)) {
        return fail("the terminal, with nothing to write");
    }

    fd = open(FILE_PATH, O_RDONLY);
    ok = fd >= 0 && refused(pwrite(fd, "r", 1, 0), EBADF);
    ok = ok && refused(syscall(SYS_pread64, fd, buf, 2, 0xFFFFFFFFL, 0x7FFFFFFFL), EINVAL);
    close(fd);
    fd = open(FILE_PATH, O_WRONLY);
    ok = ok && fd >= 0 && refused(pread(fd, buf, 1, 0), EBADF);
    close(fd);
    if (!ok) {
        return fail("the other direction, or an end past the signed range");
    }
    dirfd = open("/", O_RDONLY | O_DIRECTORY);
    ok = dirfd >= 0 && refused(pread(dirfd, buf, 1, 0), EISDIR) &&
         refused(pread(dirfd, buf, 0, 0), EISDIR) &&
         refused(syscall(SYS_pread64, dirfd, buf, 2, 0xFFFFFFFFL, 0x7FFFFFFFL), EINVAL);
    close(dirfd);
    dirfd = open("/dev", O_RDONLY | O_DIRECTORY);
    ok = ok && dirfd >= 0 && refused(pread(dirfd, buf, 1, 0), EISDIR);
    close(dirfd);
    if (!ok) {
        return fail("a directory");
    }
    return 0;
}

int main(void)
{
    if (file_rows() != 0 || device_rows() != 0 || proc_seek_rows() != 0 || refusal_rows() != 0) {
        unlink(FILE_PATH);
        return 1;
    }
    unlink(FILE_PATH);
    printf("PREADMUSL OK\n");
    return 0;
}
