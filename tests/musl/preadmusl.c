/**
 * preadmusl: check positioned reads and writes, and lseek of /proc files.
 *
 * musl's and GNU libc's pread and pwrite are pread64 and pwrite64, with the
 * offset split over ESI and EDI on i686, or whole in R10 on x86-64.
 *
 * The rows read and write a file at offsets and require the descriptor's own
 * offset to stay where lseek left it, a write past the end to leave a hole of
 * zeros, and O_APPEND to write at the end whatever offset is asked for, as
 * Linux does. A /proc snapshot and /dev/zero are read at offsets without moving
 * theirs, and a position past 4 GiB in a snapshot is its end. A /proc snapshot
 * and /proc/self/mem seek with SEEK_SET and SEEK_CUR, by _llseek and by the old
 * call 19 (both lseek, call 8, on x86-64), and not with SEEK_END;
 * /proc/self/mem seeks to an address of the stack, above 4 GiB on x86-64, and
 * on i686 a write of it 4 GiB past an address is EIO; a writable /proc/sys file
 * reads nothing at 4 GiB. Then each error, in Linux's order: a negative offset
 * before the descriptor, a pipe, a socket and the terminal that cannot seek, a
 * descriptor open for the other direction, an end past the signed range, and
 * a directory. lseek of a socket and of a pseudo-terminal master is ESPIPE, by
 * _llseek and by call 19 (lseek, call 8, on x86-64), as on Linux.
 *
 * preadv and pwritev move a vector at an offset and leave the descriptor's
 * own: each buffer at the place the ones before it reached, a buffer of no
 * bytes skipped, and a short read ending the call. The call's fifth
 * argument is the offset's high half on i686, and on x86-64, which has the
 * whole offset in the fourth, it is not read. A vector with a buffer the
 * probe cannot reach is refused before a byte is written, and a negative
 * offset, a count past 1024, a pipe, a descriptor open for the other
 * direction and one not open are refused, the last with no buffers too.
 * pwritev2 with RWF_APPEND writes at the end, moving the descriptor's
 * offset at offset -1 and not at another, and leaves O_APPEND clear;
 * preadv2 reads the bytes back. preadv2 and pwritev2 refuse a descriptor
 * that is not open with EBADF whatever the flags, and only then a flag they
 * do not take with EOPNOTSUPP. sync_file_range flushes a range of the file,
 * and refuses a descriptor that is not open with EBADF before its flags
 * and /dev/null with ESPIPE.
 *
 * Prints `PREADMUSL OK` and exits 0 on success, and prints
 * `PREADMUSL FAIL (detail)` and exits 1 otherwise.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, an assertion for each check, as `assertion_id` lists them; a failed
 * check fails its assertion and leaves those after it not run. The branch of
 * the group `arch` is the architecture the program was built for: `x86-64`
 * with `preadv-high-half`, the fifth argument not read, and `i686` with
 * `mem-write-eio`, the write past 32 bits of /proc/self/mem, and
 * `preadv-high-word`, the fifth argument read as the offset's high half.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include "../probe_report.h"

extern char **environ;

/* The file the rows make and remove. */
#define FILE_PATH "/pread.tmp"

/*
 * A positioned transfer at the 64-bit offset of the words `lo` and `hi`:
 * i386's pread64 and pwrite64 take them in ESI and EDI, and x86-64's take
 * the whole offset in R10.
 */
#if defined __x86_64__
#define WIDE(lo, hi)                    ((long)((unsigned long)(hi) << 32 | (unsigned long)(lo)))
#define PREAD_WIDE(fd, buf, n, lo, hi)  syscall(SYS_pread64, fd, buf, n, WIDE(lo, hi))
#define PWRITE_WIDE(fd, buf, n, lo, hi) syscall(SYS_pwrite64, fd, buf, n, WIDE(lo, hi))
#define WIDE_ROW                        "the offset in R10"
#else
#define PREAD_WIDE(fd, buf, n, lo, hi)  syscall(SYS_pread64, fd, buf, n, lo, hi)
#define PWRITE_WIDE(fd, buf, n, lo, hi) syscall(SYS_pwrite64, fd, buf, n, lo, hi)
#define WIDE_ROW                        "the offset in ESI and EDI"
#endif

/* The assertions, a check each, in the order the program makes them, with
 * those of the architecture's alternative of the group `arch` in place. */
static const char *const assertion_id[] = {
    "make-file",        "pread-middle",    "pread-across-end",  "pread-past-end",
    "pwrite-middle",    "pwrite-past-end", "file-after-writes", "read-from-offset",
    "pread-wide",       "append",          "proc-snapshot",     "dev-zero",
    "dev-null",         "proc-seek",       "mem-stack",
#if !defined __x86_64__
    "mem-write-eio",
#endif
    "proc-sys-4g",      "negative-offset", "sign-bit",          "make-pipe",
    "pipe-espipe",      "socket-espipe",   "stdout-espipe",     "other-direction",
    "directory",        "lseek-espipe",    "vector-file",       "pwritev-vector",
    "preadv-vector",    "preadv-short",
#if defined __x86_64__
    "preadv-high-half",
#else
    "preadv-high-word",
#endif
    "pwritev-efault",   "vector-refusals", "pwritev2-append",   "preadv2-readback",
    "rwv2-refusals",    "rwv2-import",     "rwv2-zero",         "range-sync"};

/** Begin the report, give the branch of `arch`, and plan the assertions. */
static void report_start(void)
{
    unsigned int i;

    report_begin(report_case(environ, "guest.default.preadmusl"));
#if defined __x86_64__
    report_branch("arch", "x86-64");
#else
    report_branch("arch", "i686");
#endif
    for (i = 0; i < sizeof(assertion_id) / sizeof(assertion_id[0]); i++) {
        report_plan(assertion_id[i]);
    }
}

/** Report one failed check, end the report at the assertion that `what`
 * fails, and return 1, the probe's failure status. */
static int fail(const char *what)
{
    report_stop(what);
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

/**
 * Check reads and writes of a file at offsets, and the descriptor's offset,
 * which they leave alone. Returns 0, or 1 once the failure is reported.
 */
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
    report_held();
    memset(buf, 0, sizeof(buf));
    if (pread(fd, buf, 4, 5) != 4 || memcmp(buf, "5678", 4) != 0 || !offset_is(fd, 3)) {
        return fail("pread in the middle");
    }
    report_held();
    if (pread(fd, buf, 4, 8) != 2 || memcmp(buf, "89", 2) != 0) {
        return fail("pread across the end");
    }
    report_held();
    if (pread(fd, buf, 4, 100) != 0 || pread(fd, NULL, 0, 0) != 0) {
        return fail("pread past the end or of nothing");
    }
    report_held();
    if (pwrite(fd, "AB", 2, 1) != 2 || !offset_is(fd, 3)) {
        return fail("pwrite in the middle");
    }
    report_held();
    if (pwrite(fd, "Z", 1, 12) != 1 || fstat(fd, &st) != 0 || st.st_size != 13) {
        return fail("pwrite past the end");
    }
    report_held();
    memset(buf, 'x', sizeof(buf));
    if (pread(fd, buf, sizeof(buf), 0) != 13 || memcmp(buf, "0AB3456789\0\0Z", 13) != 0) {
        return fail("the file after the writes");
    }
    report_held();
    if (read(fd, buf, 2) != 2 || memcmp(buf, "34", 2) != 0 || !offset_is(fd, 5)) {
        return fail("read goes on from the offset lseek left");
    }
    report_held();
    if (PREAD_WIDE(fd, buf, 3, 7, 0) != 3 || memcmp(buf, "789", 3) != 0) {
        return fail(WIDE_ROW);
    }
    report_held();
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
    report_held();
    return 0;
}

/**
 * Check a /proc snapshot, /dev/zero and /dev/null read at offsets. Returns 0,
 * or 1 once the failure is reported.
 */
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
    report_held();
    fd = open("/dev/zero", O_RDWR);
    memset(part, 'x', sizeof(part));
    ok = fd >= 0 && pread(fd, part, 8, 0x10000000000LL) == 8 &&
         memcmp(part, "\0\0\0\0\0\0\0\0", 8) == 0;
    ok = ok && pwrite(fd, "abc", 3, 5) == 3;
    close(fd);
    if (!ok) {
        return fail("/dev/zero");
    }
    report_held();
    fd = open("/dev/null", O_RDONLY);
    ok = fd >= 0 && pread(fd, part, 8, 100) == 0;
    close(fd);
    if (!ok) {
        return fail("/dev/null");
    }
    report_held();
    return 0;
}

/* The bytes the /proc/self/mem row reads back from the probe's own memory. */
static const char mem_mark[] = "procseek";

/**
 * lseek of a /proc snapshot and of /proc/self/mem, as Linux seeks them:
 * SEEK_SET and SEEK_CUR move the offset, by _llseek and by the old call 19
 * (both lseek on x86-64), and the read after each finds the bytes at the offset
 * returned, so an offset returned and not kept fails; SEEK_END is EINVAL, and
 * an address of /proc/self/mem reads what the probe has there. Returns 0, or
 * 1 once the failure is reported.
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
    report_held();
    return 0;
}

/**
 * Offsets past 32 bits of the /proc files a write can open: /proc/self/mem
 * seeks to an address of the stack, above 4 GiB on x86-64, and reads what
 * the probe has there; on i686, where no address passes 32 bits, a write at
 * 4 GiB past a variable's address is EIO and leaves the variable; and
 * /proc/sys/kernel/hostname, open for writing too, reads nothing at 4 GiB.
 * Returns 0, or 1 once the failure is reported.
 */
static int wide_proc_rows(void)
{
    char stack_mark[8], part[8];
    off_t at = (off_t)(uintptr_t)stack_mark;
    int fd, ok, wide = 1;

    memcpy(stack_mark, "stackmem", 8);
    fd = open("/proc/self/mem", O_RDWR);
    ok = fd >= 0 && lseek(fd, at, SEEK_SET) == at && read(fd, part, 8) == 8 &&
         memcmp(part, stack_mark, 8) == 0;
#if !defined __x86_64__
    wide = ok && refused(pwrite(fd, "x", 1, ((off_t)1 << 32) + at), EIO) &&
           memcmp(stack_mark, "stackmem", 8) == 0;
#endif
    close(fd);
    if (!ok) {
        return fail("/proc/self/mem at an address of the stack, or past 32 bits");
    }
    report_held();
    if (!wide) {
        return fail("/proc/self/mem at an address of the stack, or past 32 bits");
    }
#if !defined __x86_64__
    report_held();
#endif
    fd = open("/proc/sys/kernel/hostname", O_RDWR);
    ok = fd >= 0 && pread(fd, part, sizeof(part), (off_t)1 << 32) == 0;
    close(fd);
    if (!ok) {
        return fail("a writable /proc/sys file read at 4 GiB is not at its end");
    }
    report_held();
    return 0;
}

/**
 * Check each error, in the order Linux checks for them. Returns 0, or 1 once
 * the failure is reported.
 */
static int refusal_rows(void)
{
    char buf[8];
    int pipefd[2];
    int fd, dirfd;
    int ok;

    if (!refused(pread(999, buf, 1, -1), EINVAL) || !refused(pread(999, buf, 1, 0), EBADF)) {
        return fail("a negative offset before the descriptor");
    }
    report_held();
    if (!refused(PWRITE_WIDE(999, buf, 1, 0, 0x80000000L), EINVAL)) {
        return fail("a high word with its sign bit set");
    }
    report_held();
    if (pipe(pipefd) != 0) {
        return fail("make a pipe");
    }
    report_held();
    ok = refused(pread(pipefd[0], buf, 1, 0), ESPIPE) &&
         refused(pwrite(pipefd[1], "p", 1, 0), ESPIPE) &&
         refused(pread(pipefd[1], buf, 1, 0), ESPIPE);
    close(pipefd[0]);
    close(pipefd[1]);
    if (!ok) {
        return fail("a pipe");
    }
    report_held();
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    ok = fd >= 0 && refused(pread(fd, buf, 1, 0), ESPIPE);
    close(fd);
    if (!ok) {
        return fail("a socket");
    }
    report_held();
    if (!refused(pwrite(1, "t", 0, 0), ESPIPE)) {
        return fail("the terminal, with nothing to write");
    }
    report_held();

    fd = open(FILE_PATH, O_RDONLY);
    ok = fd >= 0 && refused(pwrite(fd, "r", 1, 0), EBADF);
    ok = ok && refused(PREAD_WIDE(fd, buf, 2, 0xFFFFFFFFL, 0x7FFFFFFFL), EINVAL);
    close(fd);
    fd = open(FILE_PATH, O_WRONLY);
    ok = ok && fd >= 0 && refused(pread(fd, buf, 1, 0), EBADF);
    close(fd);
    if (!ok) {
        return fail("the other direction, or an end past the signed range");
    }
    report_held();
    dirfd = open("/", O_RDONLY | O_DIRECTORY);
    ok = dirfd >= 0 && refused(pread(dirfd, buf, 1, 0), EISDIR) &&
         refused(pread(dirfd, buf, 0, 0), EISDIR) &&
         refused(PREAD_WIDE(dirfd, buf, 2, 0xFFFFFFFFL, 0x7FFFFFFFL), EINVAL);
    close(dirfd);
    dirfd = open("/dev", O_RDONLY | O_DIRECTORY);
    ok = ok && dirfd >= 0 && refused(pread(dirfd, buf, 1, 0), EISDIR);
    close(dirfd);
    if (!ok) {
        return fail("a directory");
    }
    report_held();
    return 0;
}

/**
 * Check that lseek of a socket and of a pseudo-terminal master is ESPIPE, by
 * musl's lseek and by call SYS_lseek. Returns 0, or 1 once the failure is
 * reported.
 */
static int seek_refusal_rows(void)
{
    int fd;
    int ok;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    ok = fd >= 0 && refused(lseek(fd, 0, SEEK_CUR), ESPIPE) &&
         refused(syscall(SYS_lseek, fd, 0L, SEEK_CUR), ESPIPE);
    close(fd);
    fd = posix_openpt(O_RDWR | O_NOCTTY);
    ok = ok && fd >= 0 && refused(lseek(fd, 0, SEEK_SET), ESPIPE) &&
         refused(syscall(SYS_lseek, fd, 0L, SEEK_SET), ESPIPE);
    close(fd);
    if (!ok) {
        return fail("lseek of a socket or a pseudo-terminal master");
    }
    report_held();
    return 0;
}

/* The x86-64 kernel half, or the i686 kernel's base, which this program
 * leaves unmapped when it runs under the ia32 profile. */
#if defined __x86_64__
#define KERNEL_ADDRESS ((void *)0xffff800000000000UL)
#else
#define KERNEL_ADDRESS ((void *)0xc0000000UL)
#endif

/* RWF_APPEND, which musl's headers may not name. */
#ifndef RWF_APPEND
#define RWF_APPEND 0x10
#endif

/* preadv2's and pwritev2's position as the two words the call takes, low
 * first, and the position -1, the descriptor's own offset. */
#define POS_LO(p)                      ((long)(p))
#define POS_HI(p)                      ((long)((unsigned long long)(p) >> 32))
#define RWV2(nr, fd, iov, n, p, flags) syscall(nr, fd, iov, n, POS_LO(p), POS_HI(p), flags)

/** Check vector import before an unknown flag, and zero bytes before the
 * flag, for both directions and both position forms. Return 0 or report 1. */
static int vector_flag_order(int fd)
{
    const long calls[2] = {SYS_preadv2, SYS_pwritev2};
    struct iovec invalid = {(void *)0x1, (size_t)-1};
    struct iovec unfaulted = {(void *)0x1, 1};
    struct iovec empty = {0, 0};
    int i, pos;

    for (i = 0; i < 2; i++) {
        for (pos = -1; pos <= 0; pos++) {
            if (!refused(RWV2(calls[i], fd, (void *)0x1, 1, pos, 0x40), EFAULT) ||
                !refused(RWV2(calls[i], fd, &empty, 1025, pos, 0x40), EINVAL) ||
                !refused(RWV2(calls[i], fd, &invalid, 1, pos, 0x40), EINVAL) ||
                !refused(RWV2(calls[i], fd, &unfaulted, 1, pos, 0x40), EOPNOTSUPP)) {
                return fail("vector import precedes flags, but data faults follow flags");
            }
        }
    }
    report_held();
    for (i = 0; i < 2; i++) {
        for (pos = -1; pos <= 0; pos++) {
            if (RWV2(calls[i], fd, (void *)0x1, 0, pos, 0x40) != 0 ||
                RWV2(calls[i], fd, &empty, 1, pos, 0x40) != 0) {
                return fail("zero bytes return before an unknown vector flag");
            }
        }
    }
    report_held();
    return 0;
}

/**
 * Check pwritev2's RWF_APPEND, preadv2's read back, the two calls' refusals
 * and sync_file_range on a new copy of the file. Returns 0, or 1 once the
 * failure is reported.
 */
static int vector_flag_rows(void)
{
    char back[8];
    struct iovec out[2], in[1];
    int fd, null, ok;

    fd = open(FILE_PATH, O_RDWR | O_TRUNC);
    out[0].iov_base = "C";
    out[0].iov_len = 1;
    out[1].iov_base = "D";
    out[1].iov_len = 1;
    if (fd < 0 || write(fd, "ab", 2) != 2 || lseek(fd, 0, SEEK_SET) != 0 ||
        RWV2(SYS_pwritev2, fd, out, 2, -1LL, RWF_APPEND) != 2 || !offset_is(fd, 4)) {
        return fail("pwritev2 with RWF_APPEND at -1 writes at the end and moves the offset");
    }
    out[0].iov_base = "E";
    if (lseek(fd, 1, SEEK_SET) != 1 || RWV2(SYS_pwritev2, fd, out, 1, 0LL, RWF_APPEND) != 1 ||
        !offset_is(fd, 1) || (fcntl(fd, F_GETFL) & O_APPEND) != 0) {
        return fail("pwritev2 with RWF_APPEND at 0 writes at the end and leaves the offset");
    }
    report_held();
    memset(back, 'x', sizeof(back));
    in[0].iov_base = back;
    in[0].iov_len = sizeof(back);
    if (RWV2(SYS_preadv2, fd, in, 1, 0LL, 0) != 5 || memcmp(back, "abCDExxx", 8) != 0 ||
        !offset_is(fd, 1)) {
        return fail("preadv2 reads the appended bytes back");
    }
    report_held();
    ok = refused(RWV2(SYS_preadv2, 999, in, 1, 0LL, 0x40), EBADF) &&
         refused(RWV2(SYS_pwritev2, 999, out, 1, -1LL, 0x40), EBADF) &&
         refused(RWV2(SYS_preadv2, fd, in, 1, 0LL, 0x40), EOPNOTSUPP) &&
         refused(RWV2(SYS_pwritev2, fd, out, 1, -1LL, 0x40), EOPNOTSUPP);
    if (!ok) {
        return fail("preadv2 and pwritev2 refuse a bad descriptor before a bad flag");
    }
    report_held();
    if (vector_flag_order(fd) != 0) {
        return 1;
    }
    null = open("/dev/null", O_RDWR);
    ok = sync_file_range(fd, 0, 5, SYNC_FILE_RANGE_WRITE) == 0 &&
         refused(sync_file_range(999, -1, 0, 0x100), EBADF) &&
         refused(sync_file_range(fd, 0, 0, 0x100), EINVAL) && null >= 0 &&
         refused(sync_file_range(null, 0, 0, 0), ESPIPE);
    close(null);
    close(fd);
    if (!ok) {
        return fail("sync_file_range flushes a file and refuses in Linux's order");
    }
    report_held();
    return 0;
}

/**
 * Check the refusals of preadv and pwritev on the file vector_rows makes.
 * Returns 0, or 1 once the failure is reported.
 */
static int vector_refusal_rows(void)
{
    char two[2];
    struct iovec out[2], in[1];
    int fd, pipefd[2], ok;

    fd = open(FILE_PATH, O_RDWR);
    out[0].iov_base = "XY";
    out[0].iov_len = 2;
    out[1].iov_base = KERNEL_ADDRESS;
    out[1].iov_len = 4;
    if (fd < 0 || !refused(pwritev(fd, out, 2, 0), EFAULT) || pread(fd, two, 2, 0) != 2 ||
        memcmp(two, "01", 2) != 0) {
        return fail("pwritev refuses a vector with a bad buffer before it writes");
    }
    report_held();
    in[0].iov_base = two;
    in[0].iov_len = sizeof(two);
    ok = refused(preadv(fd, in, 1, -1), EINVAL) && refused(preadv(fd, in, -1, 0), EINVAL) &&
         refused(preadv(fd, in, 1025, 0), EINVAL) && refused(preadv(999, in, 1, 0), EBADF) &&
         refused(preadv(999, in, 0, 0), EBADF);
    close(fd);
    fd = open(FILE_PATH, O_RDONLY);
    ok = ok && fd >= 0 && refused(pwritev(fd, out, 1, 0), EBADF);
    close(fd);
    ok = ok && pipe(pipefd) == 0 && refused(preadv(pipefd[0], in, 1, 0), ESPIPE) &&
         refused(pwritev(pipefd[1], out, 1, 0), ESPIPE);
    close(pipefd[0]);
    close(pipefd[1]);
    if (!ok) {
        return fail("the refusals of preadv and pwritev");
    }
    report_held();
    return 0;
}

/**
 * Check preadv and pwritev on the file the earlier rows leave, which this
 * makes again. Returns 0, or 1 once the failure is reported.
 */
static int vector_rows(void)
{
    char head[2], tail[4], three[3], whole[32];
    struct iovec out[3], in[3];
    int fd;

    fd = open(FILE_PATH, O_RDWR | O_TRUNC);
    if (fd < 0 || write(fd, "0123456789", 10) != 10 || lseek(fd, 3, SEEK_SET) != 3) {
        return fail("make the file for the vector rows");
    }
    report_held();
    out[0].iov_base = "ab";
    out[0].iov_len = 2;
    out[1].iov_base = NULL;
    out[1].iov_len = 0;
    out[2].iov_base = "cdefg";
    out[2].iov_len = 5;
    memset(whole, 'x', sizeof(whole));
    if (pwritev(fd, out, 3, 12) != 7 || !offset_is(fd, 3) ||
        pread(fd, whole, sizeof(whole), 0) != 19 ||
        memcmp(whole, "0123456789\0\0abcdefg", 19) != 0) {
        return fail("pwritev writes its buffers in turn at the offset");
    }
    report_held();
    in[0].iov_base = head;
    in[0].iov_len = sizeof(head);
    in[1].iov_base = NULL;
    in[1].iov_len = 0;
    in[2].iov_base = tail;
    in[2].iov_len = sizeof(tail);
    if (preadv(fd, in, 3, 13) != 6 || memcmp(head, "bc", 2) != 0 || memcmp(tail, "defg", 4) != 0 ||
        !offset_is(fd, 3)) {
        return fail("preadv reads its buffers in turn from the offset");
    }
    report_held();
    memset(head, 'x', sizeof(head));
    memset(tail, 'x', sizeof(tail));
    in[0].iov_base = tail;
    in[0].iov_len = sizeof(tail);
    in[1].iov_base = head;
    in[1].iov_len = sizeof(head);
    if (preadv(fd, in, 2, 17) != 2 || memcmp(tail, "fgxx", 4) != 0 || memcmp(head, "xx", 2) != 0 ||
        preadv(fd, in, 2, 19) != 0 || preadv(fd, in, 0, 0) != 0) {
        return fail("a short preadv ends at the end of the file");
    }
    report_held();
    in[0].iov_base = three;
    in[0].iov_len = sizeof(three);
#if defined __x86_64__
    if (syscall(SYS_preadv, fd, in, 1, 4L, 0xdeadL) != 3 || memcmp(three, "456", 3) != 0) {
        return fail("preadv does not read the offset's high half");
    }
#else
    if (syscall(SYS_preadv, fd, in, 1, 4L, 1L) != 0 ||
        syscall(SYS_preadv, fd, in, 1, 4L, 0L) != 3 || memcmp(three, "456", 3) != 0) {
        return fail("preadv reads its fifth argument as the offset's high half");
    }
#endif
    report_held();
    close(fd);
    return vector_refusal_rows() != 0 ? 1 : vector_flag_rows();
}

int main(void)
{
    report_start();
    if (file_rows() != 0 || device_rows() != 0 || proc_seek_rows() != 0 || wide_proc_rows() != 0 ||
        refusal_rows() != 0 || seek_refusal_rows() != 0 || vector_rows() != 0) {
        unlink(FILE_PATH);
        return 1;
    }
    unlink(FILE_PATH);
    report_end();
    printf("PREADMUSL OK\n");
    return 0;
}
