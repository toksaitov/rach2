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
 * a directory, which read refuses with EISDIR too, while preadv and readv of
 * no bytes return 0 from it and of bytes EISDIR. lseek of a socket and of a
 * pseudo-terminal master is ESPIPE, by
 * _llseek and by call 19 (lseek, call 8, on x86-64), as on Linux.
 *
 * preadv and pwritev move a vector at an offset and leave the descriptor's
 * own: each buffer at the place the ones before it reached, a buffer of no
 * bytes skipped, and a short read ending the call. The call's fifth
 * argument is the offset's high half on i686, and on x86-64, which has the
 * whole offset in the fourth, it is not read. A vector with a buffer
 * outside user memory is refused before a byte is written, and a negative
 * offset, a count past 1024, a pipe, a descriptor open for the other
 * direction and one not open are refused, the last with no buffers too, and
 * the last two before a count past 1024 or a vector outside user memory.
 * writev to /dev/null of a vector past 2 GiB returns 0x7ffff000, Linux's
 * MAX_RW_COUNT, on x86-64, and on i686, where a length of 2 GiB is a
 * negative ssize_t, fails with EINVAL.
 * writev of no bytes returns 0 from /dev/full and an unconnected datagram
 * socket without writing, and refuses a descriptor not open or open only
 * for reading with EBADF, and an epoll set, open for writing with no write,
 * with EINVAL, before its count and its vector; a write to the set is EINVAL
 * too. read and readv refuse the same descriptors with the same errors, with
 * no bytes too and before a count past 1024 or a vector or buffer outside
 * user memory, and a directory's read EFAULT for such a buffer before
 * EISDIR. read, write, pread64 and pwrite64 check the original count before
 * cutting it at MAX_RW_COUNT:
 * an overflowing range is EFAULT, and on the i686 kernel a count negative
 * as ssize_t is EINVAL once its range has passed the address check.
 * Positioned I/O checks the end before that cut too, and checks the range
 * before an overflowing end. At a file's offset one short of LLONG_MAX, a
 * read, write, readv or writev of two bytes is EINVAL and leaves the offset;
 * /proc/self/mem and /dev/mem, which take unsigned offsets, are not refused
 * so. /dev/null lets large writes finish without
 * touching the buffer.
 * pwritev2 with RWF_APPEND writes at the end, moving the descriptor's
 * offset at offset -1 and not at another, and leaves O_APPEND clear;
 * RWF_NOAPPEND overrides O_APPEND for one call at either offset form,
 * leaving the flag set for later writes; preadv2 takes it and reads the
 * bytes back. Both calls refuse APPEND with NOAPPEND after vector import,
 * except for zero bytes, which return before flags. They refuse a descriptor
 * that is not open with EBADF whatever the flags, and only then a flag they
 * do not take with EOPNOTSUPP. On an append-only file (chattr +a), which a
 * root of ext2 run as root lets the probe make, NOAPPEND is EPERM to both
 * calls and F_SETFL may not change O_APPEND, as Linux 6.12's
 * kiocb_set_rw_flags and setfl answer. sync_file_range flushes a range of
 * the file, and refuses a descriptor that is not open with EBADF before its
 * flags and /dev/null with ESPIPE.
 * noappend-seal refuses a vector that would grow a six-byte sealed memfd
 * and allows one within its size. The four append-fault rows use writev,
 * pwritev and pwritev2:
 * later and mid-buffer faults return the completed prefix, an initial
 * fault returns EFAULT, and a demand-paged buffer is brought in.
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
 * The branch of the group `flags` is `append-only`, with the rows of an
 * append-only file, when the probe runs as root on a root that keeps inode
 * flags, and `none`, with no rows, on any other.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
static const char *const assertion_id[] = {"make-file",
                                           "pread-middle",
                                           "pread-across-end",
                                           "pread-past-end",
                                           "pwrite-middle",
                                           "pwrite-past-end",
                                           "file-after-writes",
                                           "read-from-offset",
                                           "pread-wide",
                                           "append",
                                           "proc-snapshot",
                                           "dev-zero",
                                           "dev-null",
                                           "proc-seek",
                                           "mem-stack",
#if !defined __x86_64__
                                           "mem-write-eio",
#endif
                                           "proc-sys-4g",
                                           "negative-offset",
                                           "sign-bit",
                                           "make-pipe",
                                           "pipe-espipe",
                                           "socket-espipe",
                                           "stdout-espipe",
                                           "other-direction",
                                           "directory",
                                           "lseek-espipe",
                                           "vector-file",
                                           "pwritev-vector",
                                           "preadv-vector",
                                           "preadv-short",
#if defined __x86_64__
                                           "preadv-high-half",
#else
                                           "preadv-high-word",
#endif
                                           "pwritev-efault",
                                           "vector-refusals",
                                           "pwritev2-append",
                                           "preadv2-readback",
                                           "rwv2-refusals",
                                           "rwv2-import",
                                           "rwv2-zero",
                                           "range-sync",
                                           "noappend-position",
                                           "noappend-shared",
                                           "noappend-read",
                                           "noappend-flags",
                                           "noappend-seal",
                                           "append-later-fault",
                                           "append-buffer-fault",
                                           "append-first-fault",
                                           "append-demand-buffer",
                                           "offset-end",
                                           "mem-offset-end"};

/* chattr's requests and its append-only flag, which musl's headers do not
 * name: _IOR and _IOW of 'f' with a long, whose argument is an int. */
#define FS_IOC_GETFLAGS _IOR('f', 1, long)
#define FS_IOC_SETFLAGS _IOW('f', 2, long)
#define FS_APPEND_FL    0x20

/* Whether the branch of `flags` is `append-only`, which adds its rows. */
static int append_only;

/** Begin the report, give the branches of `arch` and `flags`, and plan the
 * assertions. */
static void report_start(void)
{
    unsigned int i;
    int root = open("/", O_RDONLY | O_DIRECTORY), flags;

    report_begin(report_case(environ, "guest.default.preadmusl"));
#if defined __x86_64__
    report_branch("arch", "x86-64");
#else
    report_branch("arch", "i686");
#endif
    append_only = geteuid() == 0 && root >= 0 && ioctl(root, FS_IOC_GETFLAGS, &flags) == 0;
    if (root >= 0) {
        close(root);
    }
    report_branch("flags", append_only ? "append-only" : "none");
    for (i = 0; i < sizeof(assertion_id) / sizeof(assertion_id[0]); i++) {
        report_plan(assertion_id[i]);
    }
    if (append_only) {
        report_plan("append-only-noappend");
        report_plan("append-only-setfl");
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
 * Whether a directory, / and /dev, is refused as Linux refuses it: pread
 * and read fail with EISDIR for one byte and for none, and pread with an end
 * past the signed range with EINVAL; preadv and readv return 0 for no bytes
 * and fail with EISDIR for one.
 */
static int directory_refused(void)
{
    char buf[8];
    struct iovec v[1];
    int dirfd, ok;

    dirfd = open("/", O_RDONLY | O_DIRECTORY);
    v[0].iov_base = buf;
    v[0].iov_len = 0;
    ok = dirfd >= 0 && refused(pread(dirfd, buf, 1, 0), EISDIR) &&
         refused(pread(dirfd, buf, 0, 0), EISDIR) &&
         refused(PREAD_WIDE(dirfd, buf, 2, 0xFFFFFFFFL, 0x7FFFFFFFL), EINVAL) &&
         refused(read(dirfd, buf, 1), EISDIR) && refused(read(dirfd, buf, 0), EISDIR) &&
         preadv(dirfd, v, 1, 0) == 0 && preadv(dirfd, v, 0, 0) == 0 && readv(dirfd, v, 1) == 0;
    v[0].iov_len = 1;
    ok = ok && refused(preadv(dirfd, v, 1, 0), EISDIR) && refused(readv(dirfd, v, 1), EISDIR);
    close(dirfd);
    dirfd = open("/dev", O_RDONLY | O_DIRECTORY);
    ok = ok && dirfd >= 0 && refused(pread(dirfd, buf, 1, 0), EISDIR);
    close(dirfd);
    return ok;
}

/**
 * Check each error, in the order Linux checks for them. Returns 0, or 1 once
 * the failure is reported.
 */
static int refusal_rows(void)
{
    char buf[8];
    int pipefd[2];
    int fd;
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
    if (!directory_refused()) {
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
#ifndef RWF_NOAPPEND
#define RWF_NOAPPEND 0x20
#endif
#ifndef RWF_ATOMIC
#define RWF_ATOMIC 0x40
#endif

/* Outside Linux 6.12's RWF_SUPPORTED, whose 0x40 is RWF_ATOMIC. */
#define UNKNOWN_RWF 0x10000000

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
            if (!refused(RWV2(calls[i], fd, (void *)0x1, 1, pos, UNKNOWN_RWF), EFAULT) ||
                !refused(RWV2(calls[i], fd, &empty, 1025, pos, UNKNOWN_RWF), EINVAL) ||
                !refused(RWV2(calls[i], fd, &invalid, 1, pos, UNKNOWN_RWF), EINVAL) ||
                !refused(RWV2(calls[i], fd, &unfaulted, 1, pos, UNKNOWN_RWF), EOPNOTSUPP)) {
                return fail("vector import precedes flags, but data faults follow flags");
            }
        }
    }
    report_held();
    for (i = 0; i < 2; i++) {
        for (pos = -1; pos <= 0; pos++) {
            if (RWV2(calls[i], fd, (void *)0x1, 0, pos, UNKNOWN_RWF) != 0 ||
                RWV2(calls[i], fd, &empty, 1, pos, UNKNOWN_RWF) != 0) {
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
    ok = refused(RWV2(SYS_preadv2, 999, in, 1, 0LL, UNKNOWN_RWF), EBADF) &&
         refused(RWV2(SYS_pwritev2, 999, out, 1, -1LL, UNKNOWN_RWF), EBADF) &&
         refused(RWV2(SYS_preadv2, fd, in, 1, 0LL, UNKNOWN_RWF), EOPNOTSUPP) &&
         refused(RWV2(SYS_pwritev2, fd, out, 1, -1LL, UNKNOWN_RWF), EOPNOTSUPP);
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

/** Check reads with NOAPPEND and both directions' conflicting flags on
 * the append descriptor `fd` after the write rows. Leave it open at offset
 * 3. Return a failure message or NULL, reporting each completed row. */
static const char *noappend_read_flags(int fd)
{
    const long calls[2] = {SYS_preadv2, SYS_pwritev2};
    char back[8];
    struct iovec in = {back, 2}, empty = {0, 0};
    int flags = RWF_APPEND | RWF_NOAPPEND;
    int i, pos;
    const char *error;

    error = "preadv2 takes NOAPPEND at both offset forms";
    if (RWV2(SYS_preadv2, fd, &in, 1, 1LL, RWF_NOAPPEND) != 2 || memcmp(back, "XY", 2) != 0 ||
        !offset_is(fd, 7) || lseek(fd, 1, SEEK_SET) != 1 ||
        RWV2(SYS_preadv2, fd, &in, 1, -1LL, RWF_NOAPPEND) != 2 || memcmp(back, "XY", 2) != 0 ||
        !offset_is(fd, 3)) {
        return error;
    }
    report_held();
    error = "APPEND and NOAPPEND conflict after import and before known unsupported flags";
    for (i = 0; i < 2; i++) {
        for (pos = -1; pos <= 0; pos++) {
            if (!refused(RWV2(calls[i], fd, &in, 1, pos, flags), EINVAL) ||
                !refused(RWV2(calls[i], fd, &in, 1, pos, flags | RWF_ATOMIC), EINVAL) ||
                !refused(RWV2(calls[i], fd, &in, 1, pos, flags | UNKNOWN_RWF), EOPNOTSUPP) ||
                !refused(RWV2(calls[i], fd, (void *)0x1, 1, pos, flags), EFAULT) ||
                RWV2(calls[i], fd, (void *)0x1, 0, pos, flags) != 0 ||
                RWV2(calls[i], fd, &empty, 1, pos, flags) != 0) {
                return error;
            }
        }
    }
    if (!offset_is(fd, 3) || (fcntl(fd, F_GETFL) & O_APPEND) == 0 ||
        pread(fd, back, sizeof(back), 0) != 7 || memcmp(back, "aXYdUV!", 7) != 0) {
        return error;
    }
    report_held();
    return NULL;
}

/** Check NOAPPEND at an explicit and the shared offset, without changing
 * O_APPEND, and its read and conflicting-flag rules. Close the file on
 * every exit; main removes it. Return 0, or report a failure and return 1. */
static int vector_noappend_rows(void)
{
    char back[8];
    struct iovec out[2] = {{"X", 1}, {"Y", 1}};
    int fd = open(FILE_PATH, O_RDWR | O_APPEND | O_TRUNC);
    const char *error = "NOAPPEND writes at the explicit offset and keeps O_APPEND";

    if (fd < 0 || write(fd, "abcdef", 6) != 6 || lseek(fd, 4, SEEK_SET) != 4 ||
        RWV2(SYS_pwritev2, fd, out, 2, 1LL, RWF_NOAPPEND) != 2 || !offset_is(fd, 4) ||
        (fcntl(fd, F_GETFL) & O_APPEND) == 0 || pread(fd, back, sizeof(back), 0) != 6 ||
        memcmp(back, "aXYdef", 6) != 0) {
        goto out;
    }
    report_held();
    error = "NOAPPEND advances the shared offset and later writes still append";
    out[0].iov_base = "U";
    out[1].iov_base = "V";
    if (RWV2(SYS_pwritev2, fd, out, 2, -1LL, RWF_NOAPPEND) != 2 || !offset_is(fd, 6) ||
        (fcntl(fd, F_GETFL) & O_APPEND) == 0 || lseek(fd, 0, SEEK_SET) != 0 ||
        write(fd, "!", 1) != 1 || !offset_is(fd, 7) || pread(fd, back, sizeof(back), 0) != 7 ||
        memcmp(back, "aXYdUV!", 7) != 0) {
        goto out;
    }
    report_held();
    error = noappend_read_flags(fd);
out:
    if (fd >= 0) {
        close(fd);
    }
    return error ? fail(error) : 0;
}

/** Check a NOAPPEND vector against F_SEAL_GROW before it writes any byte,
 * at both offset forms, then an allowed write within the file. Close the
 * memfd on every exit. Return 0, or report a failure and return 1. */
static int noappend_seal_row(void)
{
    struct iovec out[2] = {{"X", 1}, {"Y", 1}};
    char back[8];
    int fd = memfd_create("preadmusl", MFD_ALLOW_SEALING);
    int ok = fd >= 0 && write(fd, "abcdef", 6) == 6 && fcntl(fd, F_SETFL, O_APPEND) == 0 &&
             fcntl(fd, F_ADD_SEALS, F_SEAL_GROW) == 0 && lseek(fd, 5, SEEK_SET) == 5 &&
             refused(RWV2(SYS_pwritev2, fd, out, 2, 5LL, RWF_NOAPPEND), EPERM) &&
             refused(RWV2(SYS_pwritev2, fd, out, 2, -1LL, RWF_NOAPPEND), EPERM) &&
             offset_is(fd, 5) && pread(fd, back, sizeof(back), 0) == 6 &&
             memcmp(back, "abcdef", 6) == 0 &&
             RWV2(SYS_pwritev2, fd, out, 2, 2LL, RWF_NOAPPEND) == 2 && offset_is(fd, 5) &&
             (fcntl(fd, F_GETFL) & O_APPEND) != 0 && pread(fd, back, sizeof(back), 0) == 6 &&
             memcmp(back, "abXYef", 6) == 0;

    if (fd >= 0) {
        close(fd);
    }
    if (!ok) {
        return fail("NOAPPEND refuses sealed growth before writing any element");
    }
    report_held();
    return 0;
}

/** Set or clear the append-only flag of the file `fd` is open on, as
 * `on` says. Return 0, or -1 with errno set. */
static int set_append_only(int fd, int on)
{
    int flags;

    if (ioctl(fd, FS_IOC_GETFLAGS, &flags) != 0) {
        return -1;
    }
    flags = on ? flags | FS_APPEND_FL : flags & ~FS_APPEND_FL;
    return ioctl(fd, FS_IOC_SETFLAGS, &flags);
}

/** Check NOAPPEND and F_SETFL on an append-only file, the descriptor `fd`
 * open on it for reading and writing with O_APPEND and `ro` for reading.
 * Return a failure message or NULL, reporting each completed row. */
static const char *append_only_checks(int fd, int ro)
{
    struct iovec out = {"Z", 1};
    char back[8];
    struct iovec in = {back, sizeof(back)};

    if (!refused(RWV2(SYS_pwritev2, fd, &out, 1, 0LL, RWF_NOAPPEND), EPERM) ||
        !refused(RWV2(SYS_pwritev2, fd, &out, 1, -1LL, RWF_NOAPPEND), EPERM) ||
        !refused(RWV2(SYS_preadv2, fd, &in, 1, 0LL, RWF_NOAPPEND), EPERM) ||
        RWV2(SYS_preadv2, ro, &in, 1, 0LL, RWF_NOAPPEND) != 3 ||
        RWV2(SYS_pwritev2, fd, &out, 1, 0LL, 0) != 1 || pread(fd, back, sizeof(back), 0) != 4 ||
        memcmp(back, "abcZ", 4) != 0) {
        return "NOAPPEND on an append-only file is EPERM to both calls";
    }
    report_held();
    if (!refused(fcntl(fd, F_SETFL, 0), EPERM) || (fcntl(fd, F_GETFL) & O_APPEND) == 0 ||
        fcntl(fd, F_SETFL, O_APPEND | O_NONBLOCK) != 0 ||
        (fcntl(fd, F_GETFL) & (O_APPEND | O_NONBLOCK)) != (O_APPEND | O_NONBLOCK) ||
        !refused(fcntl(ro, F_SETFL, O_APPEND), EPERM) || (fcntl(ro, F_GETFL) & O_APPEND) != 0 ||
        fcntl(ro, F_SETFL, O_NONBLOCK) != 0) {
        return "F_SETFL may not change O_APPEND of an append-only file";
    }
    report_held();
    return NULL;
}

/** Make the file append-only and run `append_only_checks` on it, in the
 * branch `append-only` alone. Clear the flag and close both descriptors on
 * every exit, so that main can remove the file. Return 0, or report a
 * failure and return 1. */
static int append_only_rows(void)
{
    const char *error = "make an append-only file";
    int fd, ro = -1, marked = 0;

    if (!append_only) {
        return 0;
    }
    fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (fd >= 0 && write(fd, "abc", 3) == 3 && set_append_only(fd, 1) == 0) {
        marked = 1;
        ro = open(FILE_PATH, O_RDONLY);
        error = ro >= 0 ? append_only_checks(fd, ro) : error;
    }
    if (marked && set_append_only(fd, 0) != 0 && !error) {
        error = "clear the append-only flag";
    }
    if (ro >= 0) {
        close(ro);
    }
    if (fd >= 0) {
        close(fd);
    }
    return error ? fail(error) : 0;
}

/** Check one kind of append buffer through writev, pwritev and pwritev2.
 * `pages` has one mapped page and an unmapped page after it. Each call must
 * retain its valid prefix, stop at the fault and keep a positioned offset.
 * Return 1 on success, 0 on any mismatch. */
static int append_fault_case(int fd, char *pages, int kind)
{
    struct iovec out[2];
    char got[8];
    const char *want = kind == 3 ? "\0\0UV" : "XY";
    int mode, bytes = kind == 2 ? 0 : kind == 3 ? 4 : 2;

    for (mode = 0; mode < 3; mode++) {
        ssize_t n;

        if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) != 0) {
            return 0;
        }
        out[0].iov_base = "XY";
        out[0].iov_len = 2;
        out[1].iov_base = pages + 4096;
        out[1].iov_len = 2;
        if (kind == 1) {
            pages[4094] = 'X';
            pages[4095] = 'Y';
            out[0].iov_base = pages + 4094;
            out[0].iov_len = 4;
        } else if (kind == 2) {
            out[0].iov_base = pages + 4096;
        } else if (kind == 3) {
            if (madvise(pages, 4096, MADV_DONTNEED) != 0) {
                return 0;
            }
            out[0].iov_base = pages;
        }
        if (kind != 0) {
            out[1].iov_base = "UV";
        }
        n = mode == 0   ? writev(fd, out, 2)
            : mode == 1 ? pwritev(fd, out, 2, 0)
                        : RWV2(SYS_pwritev2, fd, out, 2, 0LL, 0);
        if ((kind == 2 ? !refused(n, EFAULT) : n != bytes) ||
            !offset_is(fd, mode == 0 ? bytes : 0) || pread(fd, got, sizeof(got), 0) != bytes ||
            memcmp(got, want, (size_t)bytes) != 0) {
            return 0;
        }
    }
    return 1;
}

/** Check append's short counts and demand faults, releasing the file and
 * mappings before reporting a failure. The caller removes FILE_PATH. */
static int append_fault_rows(void)
{
    static const char *const why[] = {
        "append retains bytes before a later unmapped buffer",
        "append retains bytes before a fault within a buffer",
        "append returns EFAULT with no bytes when its first buffer is unmapped",
        "append faults in a valid buffer before taking the file lock"};
    int fd = open(FILE_PATH, O_CREAT | O_RDWR | O_APPEND | O_TRUNC, 0600);
    char *pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int kind, ok = 0;
    const char *error = "make append file and unmapped buffer";

    if (fd < 0 || pages == MAP_FAILED || munmap(pages + 4096, 4096) != 0) {
        goto out;
    }
    for (kind = 0; kind < 4; kind++) {
        error = why[kind];
        if (!append_fault_case(fd, pages, kind)) {
            goto out;
        }
        report_held();
    }
    ok = 1;
out:
    if (pages != MAP_FAILED) {
        munmap(pages, 8192);
    }
    if (fd >= 0) {
        close(fd);
    }
    return ok ? 0 : fail(error);
}

/* The buffer writev_cut names past its end; /dev/null reads no byte of it. */
static char cut_buffer[16];

/**
 * Whether writev to /dev/null takes a vector past 2 GiB as Linux's
 * import_iovec does: on x86-64 a length of 2 GiB alone, or one of 2 GiB - 1
 * after 16 bytes, is cut to a total of 0x7ffff000, MAX_RW_COUNT; on i686,
 * where 2 GiB is a negative ssize_t, the call is EINVAL.
 */
static int writev_cut(void)
{
    struct iovec v[2];
    int fd = open("/dev/null", O_WRONLY), ok;

    v[0].iov_base = cut_buffer;
    v[0].iov_len = 0x80000000ul;
#if defined __x86_64__
    ok = writev(fd, v, 1) == 0x7ffff000L;
    v[0].iov_len = sizeof(cut_buffer);
    v[1].iov_base = cut_buffer;
    v[1].iov_len = 0x7ffffffful;
    ok = ok && writev(fd, v, 2) == 0x7ffff000L;
#else
    ok = refused(writev(fd, v, 1), EINVAL);
#endif
    close(fd);
    return fd >= 0 && ok;
}

/**
 * Whether writev of a vector of no bytes checks the descriptor and writes
 * nothing, as Linux's vfs_writev returns 0 before the file's write: 0 from
 * /dev/full, whose write is ENOSPC, and from an unconnected datagram socket,
 * whose send is EDESTADDRREQ, with no elements or one empty element; EBADF
 * for a descriptor not open and one open only for reading, before a count
 * past 1024 or a vector outside user memory; and EINVAL for an epoll set,
 * which Linux opens for reading and writing with no write method, before
 * the same, and for a write of a byte to it.
 */
static int writev_empty(void)
{
    struct iovec v[1];
    int full = open("/dev/full", O_WRONLY), sock = socket(AF_INET, SOCK_DGRAM, 0);
    int rd = open("/dev/null", O_RDONLY), ep = epoll_create1(0), ok;

    v[0].iov_base = cut_buffer;
    v[0].iov_len = 0;
    ok = full >= 0 && sock >= 0 && rd >= 0 && writev(full, v, 0) == 0 && writev(full, v, 1) == 0 &&
         writev(sock, v, 0) == 0 && writev(sock, v, 1) == 0 && refused(writev(999, v, 0), EBADF) &&
         refused(writev(999, v, 1025), EBADF) && refused(writev(999, KERNEL_ADDRESS, 1), EBADF) &&
         refused(writev(rd, v, 0), EBADF) && refused(writev(rd, v, 1), EBADF) &&
         refused(writev(rd, v, 1025), EBADF) && refused(writev(rd, KERNEL_ADDRESS, 1), EBADF) &&
         ep >= 0 && refused(writev(ep, v, 0), EINVAL) && refused(writev(ep, v, 1), EINVAL) &&
         refused(writev(ep, KERNEL_ADDRESS, 1), EINVAL) && refused(writev(ep, v, 1025), EINVAL) &&
         refused(write(ep, "x", 1), EINVAL);
    close(ep);
    close(full);
    close(sock);
    close(rd);
    return ok;
}

/**
 * Whether read and readv check the descriptor before the count, the vector
 * and the buffer, as Linux's vfs_read and vfs_readv test FMODE_READ, then
 * FMODE_CAN_READ, then the address: EBADF for a descriptor not open and one
 * open only for writing, with no bytes too, and before a count past 1024 or
 * a vector outside user memory; EINVAL for an epoll set, which has no read
 * method, for no bytes and one, and before the same; and for a directory
 * EFAULT for a buffer outside user memory before EISDIR, for no bytes too
 * on x86-64. KERNEL_ADDRESS is user memory for an i386 program on the
 * x86-64 kernel, whose read of the directory there is EISDIR, as on Linux,
 * and the i686 kernel's top, where Linux takes a range of no bytes.
 */
static int read_empty(void)
{
    char buf[1];
    struct iovec v[1];
    int wr = open("/dev/null", O_WRONLY), ep = epoll_create1(0), dirfd = open("/", O_RDONLY);
    int ok, fault = EFAULT;

#if !defined __x86_64__
    /* The x86-64 kernel gives an i386 program user memory up to 4 GiB. */
    if (getenv("ETUDE_KERNEL") != NULL && strcmp(getenv("ETUDE_KERNEL"), "x86_64") == 0) {
        fault = EISDIR;
    }
#endif

    v[0].iov_base = buf;
    v[0].iov_len = 0;
    ok = wr >= 0 && ep >= 0 && dirfd >= 0 && refused(read(999, buf, 0), EBADF) &&
         refused(readv(999, v, 0), EBADF) && refused(readv(999, v, 1025), EBADF) &&
         refused(readv(999, KERNEL_ADDRESS, 1), EBADF) && refused(read(wr, buf, 0), EBADF) &&
         refused(readv(wr, v, 0), EBADF) && refused(readv(wr, v, 1025), EBADF) &&
         refused(readv(wr, KERNEL_ADDRESS, 1), EBADF) && refused(read(ep, buf, 0), EINVAL) &&
         refused(read(ep, buf, 1), EINVAL) && refused(readv(ep, NULL, 0), EINVAL) &&
         refused(readv(ep, v, 1025), EINVAL) && refused(readv(ep, KERNEL_ADDRESS, 1), EINVAL) &&
         refused(read(dirfd, KERNEL_ADDRESS, 1), fault);
#if defined __x86_64__
    ok = ok && refused(read(dirfd, KERNEL_ADDRESS, 0), EFAULT);
#endif
    close(wr);
    close(ep);
    close(dirfd);
    return ok;
}

/** Check read's original count before MAX_RW_COUNT cuts it. Linux's
 * native i686 access_ok rejects a range past 3 GiB, then rw_verify_area
 * rejects a count negative as ssize_t. An i386 call on x86-64 instead
 * uses the kernel's 64-bit address range and ssize_t. A directory lets
 * these checks finish without moving any of the requested bytes. */
static int read_raw_count(void)
{
    void *buf = (void *)(uintptr_t)0x10000;
    int dirfd = open("/", O_RDONLY), wr = open("/dev/null", O_WRONLY);
    int ep = epoll_create1(0), huge = EFAULT, sign = EISDIR, span = EISDIR;
    int ok;

#if !defined __x86_64__
    if (getenv("ETUDE_KERNEL") != NULL && strcmp(getenv("ETUDE_KERNEL"), "x86_64") == 0) {
        huge = EISDIR;
    } else {
        sign = EINVAL;
        span = EFAULT;
    }
#endif
    ok = dirfd >= 0 && wr >= 0 && ep >= 0 &&
         refused(syscall(SYS_read, 999, buf, (size_t)-1), EBADF) &&
         refused(syscall(SYS_read, wr, buf, (size_t)-1), EBADF) &&
         refused(syscall(SYS_read, ep, buf, (size_t)-1), EINVAL) &&
         refused(syscall(SYS_read, dirfd, buf, (size_t)-1), huge) &&
         refused(syscall(SYS_read, dirfd, buf, (size_t)0xc0000001u), span) &&
         refused(syscall(SYS_read, dirfd, buf, (size_t)0x80000000u), sign) &&
         refused(syscall(SYS_read, dirfd, buf, (size_t)0x7ffff001u), EISDIR);
    close(ep);
    close(wr);
    close(dirfd);
    return ok;
}

/** A bad descriptor, access mode or seek precedes the raw count check;
 * a negative positioned offset precedes even the descriptor lookup.
 * Return whether every refusal of the open descriptors has Linux's errno. */
static int scalar_count_access(int rd, int wr, int ep, const int *pipefd)
{
    void *buf = (void *)(uintptr_t)0x10000;

    return refused(syscall(SYS_write, 999, buf, (size_t)-1), EBADF) &&
           refused(syscall(SYS_write, rd, buf, (size_t)-1), EBADF) &&
           refused(syscall(SYS_write, ep, buf, (size_t)-1), EINVAL) &&
           refused(PREAD_WIDE(999, buf, (size_t)-1, 0, 0), EBADF) &&
           refused(PWRITE_WIDE(999, buf, (size_t)-1, 0, 0), EBADF) &&
           refused(PREAD_WIDE(wr, buf, (size_t)-1, 0, 0), EBADF) &&
           refused(PWRITE_WIDE(rd, buf, (size_t)-1, 0, 0), EBADF) &&
           refused(PREAD_WIDE(pipefd[0], buf, (size_t)-1, 0, 0), ESPIPE) &&
           refused(PWRITE_WIDE(pipefd[1], buf, (size_t)-1, 0, 0), ESPIPE) &&
           refused(PREAD_WIDE(999, buf, (size_t)-1, 0, 0x80000000u), EINVAL) &&
           refused(PWRITE_WIDE(999, buf, (size_t)-1, 0, 0x80000000u), EINVAL);
}

/** Check the original counts against the four errors the kernel's word
 * size selects, 0 meaning success. /dev/null accepts a write without
 * copying and a read returns EOF; a directory refuses a read after the
 * range and signed-count checks. Return whether each result matches. */
static int scalar_count_ranges(int rd, int wr, int dirfd, const int *errors)
{
    static const size_t counts[] = {(size_t)-1, 0xc0000001u, 0x80000000u, 0x7ffff001u};
    void *buf = (void *)(uintptr_t)0x10000;
    int i, ok = 1;
    long wrote, read_at, wrote_at;

    for (i = 0; i < (int)(sizeof(counts) / sizeof(counts[0])) && ok; i++) {
        wrote = syscall(SYS_write, wr, buf, counts[i]);
        ok = errors[i] ? refused(wrote, errors[i]) : wrote == 0x7ffff000L;
        read_at = PREAD_WIDE(rd, buf, counts[i], 0, 0);
        ok = ok && (errors[i] ? refused(read_at, errors[i]) : read_at == 0);
        wrote_at = PWRITE_WIDE(wr, buf, counts[i], 0, 0);
        ok = ok && (errors[i] ? refused(wrote_at, errors[i]) : wrote_at == 0x7ffff000L);
        ok = ok && refused(PREAD_WIDE(dirfd, buf, counts[i], 0, 0), errors[i] ? errors[i] : EISDIR);
    }
    return ok;
}

/** Whether write, pread64 and pwrite64 of no bytes at `addr` each give
 * EFAULT when `fault`, and 0 otherwise. */
static int zero_count_at(int rd, int wr, void *addr, int fault)
{
    long wrote = syscall(SYS_write, wr, addr, 0);
    int ok = fault ? refused(wrote, EFAULT) : wrote == 0;
    long read_at = PREAD_WIDE(rd, addr, 0, 0, 0);
    long wrote_at;

    ok = ok && (fault ? refused(read_at, EFAULT) : read_at == 0);
    wrote_at = PWRITE_WIDE(wr, addr, 0, 0, 0);
    return ok && (fault ? refused(wrote_at, EFAULT) : wrote_at == 0);
}

/**
 * Check an end at and past LLONG_MAX using the original count, a bad range
 * before an overflowing end, and a zero count at the top of user memory and
 * past it. `faults` says this kernel refuses the huge range, and an i386
 * call on x86-64 passes both range checks. No bytes at KERNEL_ADDRESS are
 * EFAULT on the x86-64 kernel alone: the i686 kernel takes a range of no
 * bytes that ends at its base, as Linux's access_ok does, and refuses one a
 * page above it, which `native32` says. Return whether all match.
 */
static int scalar_count_ends(int rd, int wr, int faults, int native32)
{
    void *buf = (void *)(uintptr_t)0x10000;
    int ok, range = faults ? EFAULT : EINVAL;

    ok = refused(PREAD_WIDE(rd, buf, 0x7ffff001u, 0x80000fffu, 0x7fffffffu), EINVAL) &&
         refused(PWRITE_WIDE(wr, buf, 0x7ffff001u, 0x80000fffu, 0x7fffffffu), EINVAL) &&
         PREAD_WIDE(rd, buf, 0x7ffff000u, 0x80000fffu, 0x7fffffffu) == 0 &&
         PWRITE_WIDE(wr, buf, 0x7ffff000u, 0x80000fffu, 0x7fffffffu) == 0x7ffff000L;
    ok = ok && refused(PREAD_WIDE(rd, buf, (size_t)-1, 0xffffffffu, 0x7fffffffu), range) &&
         refused(PWRITE_WIDE(wr, buf, (size_t)-1, 0xffffffffu, 0x7fffffffu), range);
    ok = ok && zero_count_at(rd, wr, KERNEL_ADDRESS, faults && !native32);
    return ok && zero_count_at(rd, wr, (char *)KERNEL_ADDRESS + 4096, faults);
}

/** Check write, pread64 and pwrite64 before the transfer limit is applied.
 * The native i686 kernel rejects signed counts and ranges as read_raw_count
 * describes; the compat calls use the x86-64 kernel's limits. Close every
 * descriptor before returning whether the rows passed. */
static int scalar_raw_count(void)
{
    int errors[] = {EFAULT, 0, 0, 0};
    int rd = open("/dev/null", O_RDONLY), wr = open("/dev/null", O_WRONLY);
    int dirfd = open("/", O_RDONLY), ep = epoll_create1(0), pipefd[2] = {-1, -1};
    int ok, native32 = 0;

#if !defined __x86_64__
    if (getenv("ETUDE_KERNEL") != NULL && strcmp(getenv("ETUDE_KERNEL"), "x86_64") == 0) {
        errors[0] = 0;
    } else {
        errors[1] = EFAULT;
        errors[2] = EINVAL;
        native32 = 1;
    }
#endif
    ok = rd >= 0 && wr >= 0 && dirfd >= 0 && ep >= 0 && pipe(pipefd) == 0 &&
         scalar_count_access(rd, wr, ep, pipefd) && scalar_count_ranges(rd, wr, dirfd, errors) &&
         scalar_count_ends(rd, wr, errors[0] != 0, native32);
    close(pipefd[0]);
    close(pipefd[1]);
    close(ep);
    close(dirfd);
    close(wr);
    close(rd);
    return ok;
}

/**
 * Check the refusals of preadv and pwritev on the file vector_rows makes,
 * writev's cut of a vector past 2 GiB (`writev_cut`) and its vector of no
 * bytes (`writev_empty`). Returns 0, or 1 once the failure is reported.
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
         refused(preadv(999, in, 0, 0), EBADF) && refused(preadv(999, in, 1025, 0), EBADF);
    close(fd);
    fd = open(FILE_PATH, O_RDONLY);
    ok = ok && fd >= 0 && refused(pwritev(fd, out, 1, 0), EBADF) &&
         refused(pwritev(fd, KERNEL_ADDRESS, 1, 0), EBADF);
    close(fd);
    ok = ok && pipe(pipefd) == 0 && refused(preadv(pipefd[0], in, 1, 0), ESPIPE) &&
         refused(pwritev(pipefd[1], out, 1, 0), ESPIPE);
    close(pipefd[0]);
    close(pipefd[1]);
    if (!ok) {
        return fail("the refusals of preadv and pwritev");
    }
    if (!writev_cut()) {
        return fail("writev of a vector past 2 GiB is not cut at 0x7ffff000 (EINVAL on i686)");
    }
    if (!writev_empty()) {
        return fail("writev of no bytes writes, or does not refuse a bad descriptor first");
    }
    if (!read_empty()) {
        return fail("read or readv does not refuse a bad descriptor before its count and buffer");
    }
    if (!read_raw_count()) {
        return fail("read does not validate its original count before cutting the transfer");
    }
    if (!scalar_raw_count()) {
        return fail("write, pread64 or pwrite64 does not check the raw count before the cut");
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

/** The largest offset, LLONG_MAX, which an end may reach and not pass. */
#define OFFSET_MAX ((off_t)0x7FFFFFFFFFFFFFFFLL)

/** Whether `rc` and errno are anything but -1 with EINVAL. */
static int not_einval(long rc)
{
    return rc != -1 || errno != EINVAL;
}

/**
 * Check the end of a transfer at the description's offset, as Linux's
 * rw_verify_area checks it for read, write, readv and writev: with the
 * file's offset one short of LLONG_MAX each of two bytes is EINVAL and
 * leaves the offset. Then /proc/self/mem, which takes unsigned offsets, and
 * /dev/mem where the probe may open it, are not refused EINVAL at that
 * offset. Returns 0, or 1 once the failure is reported.
 */
static int offset_end_rows(void)
{
    char buf[2];
    struct iovec v[1];
    int fd, ok;

    v[0].iov_base = buf;
    v[0].iov_len = sizeof(buf);
    fd = open(FILE_PATH, O_RDWR);
    ok = fd >= 0 && lseek(fd, OFFSET_MAX - 1, SEEK_SET) == OFFSET_MAX - 1 &&
         refused(read(fd, buf, 2), EINVAL) && refused(write(fd, "ab", 2), EINVAL) &&
         refused(readv(fd, v, 1), EINVAL) && refused(writev(fd, v, 1), EINVAL) &&
         offset_is(fd, OFFSET_MAX - 1);
    close(fd);
    if (!ok) {
        return fail("an end past LLONG_MAX at the description's offset is not EINVAL");
    }
    report_held();
    fd = open("/proc/self/mem", O_RDONLY);
    ok = fd >= 0 && lseek(fd, OFFSET_MAX - 1, SEEK_SET) == OFFSET_MAX - 1 &&
         not_einval(read(fd, buf, 2));
    close(fd);
    fd = open("/dev/mem", O_RDONLY);
    if (fd >= 0) {
        ok = ok && lseek(fd, OFFSET_MAX - 1, SEEK_SET) == OFFSET_MAX - 1 &&
             not_einval(read(fd, buf, 2));
        close(fd);
    }
    if (!ok) {
        return fail("/proc/self/mem or /dev/mem refused an end past LLONG_MAX");
    }
    report_held();
    return 0;
}

int main(void)
{
    report_start();
    if (file_rows() != 0 || device_rows() != 0 || proc_seek_rows() != 0 || wide_proc_rows() != 0 ||
        refusal_rows() != 0 || seek_refusal_rows() != 0 || vector_rows() != 0 ||
        vector_noappend_rows() != 0 || noappend_seal_row() != 0 || append_fault_rows() != 0 ||
        offset_end_rows() != 0 || append_only_rows() != 0) {
        unlink(FILE_PATH);
        return 1;
    }
    unlink(FILE_PATH);
    report_end();
    printf("PREADMUSL OK\n");
    return 0;
}
