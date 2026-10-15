/**
 * ext2probe: the ext2 root's contract from a program.
 *
 * `ext2probe links` mounts /dev/disk0p1, the fixture volume
 * tests/system/mkext2fixture.sh writes, read-only on /e2 and runs the
 * symbolic-link rows the walker must answer: a relative and an absolute
 * target, a link to a directory with `..` through it, a chain of forty
 * followed and one of forty-one refused, `O_NOFOLLOW`, `O_CREAT|O_EXCL` on a
 * dangling link, `lstat` against `stat`, `readlink`, a dangling link, and
 * the flags of fstatat and utimensat; the hard-link rows a read-only volume
 * can answer, the count in `stat` and `EPERM` for a directory, with EROFS,
 * EXDEV and the KRFFS root's EPERM for the links it cannot make; and the
 * links of /proc.
 *
 * `ext2probe cost` mounts the same fixture for writing with `MS_SYNCHRONOUS`
 * and counts, through `/proc/disk`, the commands and cache flushes of
 * repeated rewrites and of appends. A mount without `sync` writes back, so
 * its writes cost no commands before fsync whether or not ext2 batches them;
 * the batching saves commands on the ordered write-through of a `sync`
 * mount. Both counts include fsync, and both files are reopened and checked.
 * `ext2probe cost-append` judges the appends alone, for
 * tests/system/ext2_batch_mutate.sh.
 *
 * Prints "EXT2PROBE OK (mode)" or "EXT2PROBE FAIL: reason". Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_SYMLINK_FOLLOW   0x400

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0100
#define O_EXCL     0200
#define O_NOFOLLOW 0400000

#define MS_RDONLY      1
#define MS_SYNCHRONOUS 16

#define EPERM  1
#define ENOENT 2
#define EEXIST 17
#define EXDEV  18
#define EINVAL 22
#define EROFS  30
#define ELOOP  40

static void sys_exit(int status) __attribute__((noreturn));
/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
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

/** Write the string `s` and a newline to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

static void fail(const char *why) __attribute__((noreturn));
/** Print "EXT2PROBE FAIL: `why`" and exit 1. */
static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (long)"EXT2PROBE FAIL: ", 16);
    puts(why);
    sys_exit(1);
}

static void fail_with(const char *why, int n) __attribute__((noreturn));
/** A failure with the number `n` the row got, for a wrong count or errno. */
static void fail_with(const char *why, int n)
{
    char digits[12];
    int i = 11;
    unsigned v = n < 0 ? (unsigned)-n : (unsigned)n;

    digits[i] = '\0';
    do {
        digits[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    if (n < 0) {
        digits[--i] = '-';
    }
    syscall3(SYS_WRITE, 1, (long)"EXT2PROBE FAIL: ", 16);
    syscall3(SYS_WRITE, 1, (long)why, (int)strlen(why));
    syscall3(SYS_WRITE, 1, (long)" (got ", 6);
    syscall3(SYS_WRITE, 1, (long)(digits + i), (int)strlen(digits + i));
    puts(")");
    sys_exit(1);
}

/** Whether the strings `a` and `b` are equal. */
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The stat buffer, in the architecture's layout (probe.h's KSTAT_ offsets).
 * The kernel writes it through the pointer the call is given. */
static unsigned char st[KSTAT_SIZE];

/** The 32-bit word at `at` in the stat buffer, least significant byte first. */
static int st_word(int at)
{
    return (int)(st[at] | (st[at + 1] << 8) | (st[at + 2] << 16) | (st[at + 3] << 24));
}

/** stat or lstat, as `call` says, of `path`, with the mode word in `*mode`
 * on success: the call's result. */
static int stat_mode(int call, const char *path, int *mode)
{
    int rc = syscall2(call, (long)path, (long)st);

    if (rc == 0) {
        *mode = st_word(KSTAT_MODE);
    }
    return rc;
}

/** The link count of `path`, or -1 when stat fails. */
static int stat_nlink(const char *path)
{
    if (syscall2(SYS_STAT, (long)path, (long)st) != 0) {
        return -1;
    }
    return st_word(KSTAT_NLINK);
}

/** Open `path` and read up to `cap` bytes of it into `buf` with one read:
 * the bytes read, or open's or read's negated errno. */
static int read_all(const char *path, char *buf, int cap)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int n;

    if (fd < 0) {
        return fd;
    }
    n = syscall3(SYS_READ, fd, (long)buf, cap);
    syscall1(SYS_CLOSE, fd);
    return n;
}

/** Fail `what` unless open of `path` with `flags` succeeds when `want` is 0,
 * or fails with the errno `want` otherwise. */
static void expect_open(const char *path, int flags, int want, const char *what)
{
    int fd = syscall3(SYS_OPEN, (long)path, flags, 0644);

    if (want == 0) {
        if (fd < 0) {
            fail(what);
        }
        syscall1(SYS_CLOSE, fd);
    } else if (fd != -want) {
        fail(what);
    }
}

/** The mode word of the stat buffer. */
static int st_mode(void)
{
    return st_word(KSTAT_MODE);
}

static char big[512];

/** The rows of `ext2probe links`, on the fixture mounted read-only on /e2;
 * fails the probe at the first row that fails. */
static void links_rows(void)
{
    char buf[64];
    int mode, n;

    /* A relative target and an absolute one, read through the link. */
    n = read_all("/e2/fast", buf, sizeof(buf));
    if (n != 6 || buf[0] != 'h') {
        fail("a relative link reads its target");
    }
    n = read_all("/e2/abs", buf, sizeof(buf));
    if (n != 6 || buf[0] != 'h') {
        fail("an absolute link reads its target");
    }
    /* A link to a directory; `..` through it is the target's parent. */
    n = read_all("/e2/dirlink/deep.txt", buf, sizeof(buf));
    if (n != 5 || buf[0] != 'd') {
        fail("a link to a directory is walked into");
    }
    n = read_all("/e2/dirlink/../sub/deep.txt", buf, sizeof(buf));
    if (n != 5 || buf[0] != 'd') {
        fail("dot-dot through a directory link goes to the target's parent");
    }
    /* Forty links along one path, and forty-one. */
    n = read_all("/e2/c1", buf, sizeof(buf));
    if (n != 6) {
        fail_with("a chain of forty links is followed", n);
    }
    n = read_all("/e2/d1", buf, sizeof(buf));
    if (n != -ELOOP) {
        fail_with("a chain of forty-one links answers ELOOP", n);
    }
    /* O_NOFOLLOW: a link at the end is ELOOP, since open cannot open one. */
    expect_open("/e2/fast", O_RDONLY | O_NOFOLLOW, ELOOP, "O_NOFOLLOW on a link answers ELOOP");
    expect_open("/e2/hello.txt", O_RDONLY | O_NOFOLLOW, 0, "O_NOFOLLOW on a file opens it");
    /* O_CREAT|O_EXCL on a dangling link: the link is there. */
    expect_open("/e2/dangling", O_WRONLY | O_CREAT | O_EXCL, EEXIST,
                "O_CREAT|O_EXCL on a dangling link answers EEXIST");
    /* lstat sees the link, stat its target. */
    if (stat_mode(SYS_LSTAT, "/e2/fast", &mode) != 0 || (mode & 0170000) != 0120000) {
        fail("lstat reports the link");
    }
    if (stat_mode(SYS_STAT, "/e2/fast", &mode) != 0 || (mode & 0170000) != 0100000) {
        fail("stat reports the target");
    }
    /* readlink gives the bytes without a terminator. */
    n = syscall3(SYS_READLINK, (long)"/e2/fast", (long)buf, sizeof(buf));
    if (n != 9 || buf[0] != 'h' || buf[8] != 't') {
        fail("readlink gives the target's bytes");
    }
    n = syscall3(SYS_READLINK, (long)"/e2/hello.txt", (long)buf, sizeof(buf));
    if (n != -22) {
        fail("readlink on a file answers EINVAL");
    }
    /* A dangling link: ENOENT through it, the link itself by lstat. */
    if (stat_mode(SYS_STAT, "/e2/dangling", &mode) != -ENOENT) {
        fail("a dangling link answers ENOENT to stat");
    }
    if (stat_mode(SYS_LSTAT, "/e2/dangling", &mode) != 0 || (mode & 0170000) != 0120000) {
        fail("and is there for lstat");
    }
    if (read_all("/e2/dangling", buf, sizeof(buf)) != -ENOENT) {
        fail("and to open");
    }
    /* A link inside a path is followed under O_NOFOLLOW too: ENOTDIR. */
    n = syscall3(SYS_OPEN, (long)"/e2/fast/../hello.txt", O_RDONLY | O_NOFOLLOW, 0);
    if (n != -20) {
        fail_with("a link in the middle of a path is followed to its file", n);
    }
    /* The hard-link rows a read-only volume can answer. */
    if (stat_nlink("/e2/hello.txt") != 2 || stat_nlink("/e2/hello.link") != 2) {
        fail("a hard link's count is two on both names");
    }
    if (syscall2(SYS_LINK, (long)"/e2/sub", (long)"/e2/sub2") != -EPERM) {
        fail("a directory cannot be hard-linked");
    }
    if (syscall2(SYS_LINK, (long)"/e2/hello.txt", (long)"/e2/another") != -EROFS) {
        fail("a hard link on a read-only mount answers EROFS");
    }
    if (syscall2(SYS_SYMLINK, (long)"hello.txt", (long)"/e2/newlink") != -EROFS) {
        fail("a symbolic link on a read-only mount answers EROFS");
    }
    if (syscall2(SYS_SYMLINK, (long)"x", (long)"/newlink") != -EPERM) {
        fail("a symbolic link on the KRFFS root answers EPERM");
    }
    if (syscall2(SYS_LINK, (long)"/e2/hello.txt", (long)"/another") != -EXDEV) {
        fail("a hard link across mounts answers EXDEV");
    }
    /* O_CREAT through a dangling link creates the target: EROFS here. */
    expect_open("/e2/dangling", O_WRONLY | O_CREAT, EROFS,
                "O_CREAT through a dangling link creates the target, refused read-only");
    expect_open("/e2/fast", O_WRONLY | O_CREAT | O_EXCL, EEXIST,
                "O_CREAT|O_EXCL on a link with a target answers EEXIST");
    /* readlink: a short buffer, an empty one, no terminator, a long target. */
    buf[1] = 'Z';
    n = syscall3(SYS_READLINK, (long)"/e2/fast", (long)buf, 1);
    if (n != 1 || buf[0] != 'h' || ((volatile char *)buf)[1] != 'Z') {
        fail_with("readlink into a one-byte buffer gives one byte and no terminator", n);
    }
    n = syscall3(SYS_READLINK, (long)"/e2/fast", (long)buf, 0);
    if (n != -EINVAL) {
        fail_with("readlink into an empty buffer answers EINVAL", n);
    }
    n = syscall3(SYS_READLINK, (long)"/e2/long300", (long)big, sizeof(big));
    if (n != 300 || big[0] != 'x' || big[299] != 'x') {
        fail_with("readlink gives a 300-byte target", n);
    }
    /* fstatat with and without AT_SYMLINK_NOFOLLOW, and an invalid flag. */
    if (syscall4(SYS_FSTATAT, AT_FDCWD, (long)"/e2/fast", (long)st, AT_SYMLINK_NOFOLLOW) != 0 ||
        (st_mode() & 0170000) != 0120000) {
        fail("fstatat64 with AT_SYMLINK_NOFOLLOW reports the link");
    }
    if (syscall4(SYS_FSTATAT, AT_FDCWD, (long)"/e2/fast", (long)st, 0) != 0 ||
        (st_mode() & 0170000) != 0100000) {
        fail("fstatat64 without it reports the target");
    }
    if (syscall4(SYS_FSTATAT, AT_FDCWD, (long)"/e2/fast", (long)st, AT_SYMLINK_FOLLOW) != -EINVAL) {
        fail("fstatat64 with AT_SYMLINK_FOLLOW answers EINVAL");
    }
    if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/e2/fast", 0, AT_SYMLINK_FOLLOW) != -EINVAL) {
        fail("utimensat with AT_SYMLINK_FOLLOW answers EINVAL");
    }
#if !defined __x86_64__
    /* i386's old stat and lstat: the mode is a 16-bit word at offset 8. */
    if (syscall2(SYS_OLDLSTAT, (long)"/e2/fast", (long)st) != 0 ||
        ((st[8] | (st[9] << 8)) & 0170000) != 0120000) {
        fail("the old lstat reports the link");
    }
    if (syscall2(SYS_OLDSTAT, (long)"/e2/fast", (long)st) != 0 ||
        ((st[8] | (st[9] << 8)) & 0170000) != 0100000) {
        fail("the old stat reports the target");
    }
#endif
    /* The links of /proc answer the same two ways. */
    expect_open("/proc/self", O_RDONLY | O_NOFOLLOW, ELOOP,
                "/proc/self under O_NOFOLLOW answers ELOOP");
    expect_open("/proc/self/fd/1", O_WRONLY | O_CREAT | O_EXCL, EEXIST,
                "/proc/self/fd/1 under O_CREAT|O_EXCL answers EEXIST");
}

/* The disk's counts of commands and cache flushes, from /proc/disk. */
struct disk_counts {
    unsigned commands;
    unsigned flushes;
};

static unsigned char cost_block[4096];

/** The unsigned counter on the line `key` of the /proc/disk text `text`,
 * "key value"; a counter that is absent, empty, too large or followed by
 * other text fails the probe. */
static unsigned disk_counter(const char *text, const char *key)
{
    const char *p = text;
    size_t len = strlen(key);

    while (*p) {
        size_t i = 0;

        while (i < len && p[i] && p[i] == key[i]) {
            i++;
        }
        if (i == len && p[i] == ' ') {
            unsigned value = 0;

            p += i + 1;
            if (*p < '0' || *p > '9') {
                fail("a disk counter has no value");
            }
            while (*p >= '0' && *p <= '9') {
                unsigned digit = (unsigned)(*p++ - '0');

                if (value > (0xFFFFFFFFu - digit) / 10) {
                    fail("a disk counter overflows");
                }
                value = value * 10 + digit;
            }
            if (*p != '\n' && *p != '\0') {
                fail("a disk counter has trailing text");
            }
            return value;
        }
        while (*p && *p != '\n') {
            p++;
        }
        if (*p) {
            p++;
        }
    }
    fail("a disk counter is absent");
}

/** The disk's counts now. Reading `/proc/disk` takes a snapshot without
 * issuing a disk command. */
static struct disk_counts disk_counts(void)
{
    static char text[8192];
    struct disk_counts counts;
    int n = read_all("/proc/disk", text, sizeof(text) - 1);

    if (n <= 0 || n == (int)sizeof(text) - 1) {
        fail("reading the disk counters");
    }
    text[n] = '\0';
    counts.commands = disk_counter(text, "disk-commands");
    counts.flushes = disk_counter(text, "disk-flushes");
    return counts;
}

/** Print one counter without a hosted library or a 64-bit division. */
static void cost_number(unsigned value)
{
    char digits[10];
    int i = sizeof(digits);

    do {
        digits[--i] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    syscall3(SYS_WRITE, 1, (long)(digits + i), (int)sizeof(digits) - i);
}

/** Print the counts of the row `name` since `before`, at the end of its
 * writes and after its fsync, before they are checked, so that the log has
 * the cost of a run that fails too. */
static void cost_report(const char *name, struct disk_counts before, struct disk_counts collected,
                        struct disk_counts committed)
{
    syscall3(SYS_WRITE, 1, (long)"ext2-cost: result: ", 19);
    syscall3(SYS_WRITE, 1, (long)name, (int)strlen(name));
    syscall3(SYS_WRITE, 1, (long)" collected commands=", 20);
    cost_number(collected.commands - before.commands);
    syscall3(SYS_WRITE, 1, (long)" flushes=", 9);
    cost_number(collected.flushes - before.flushes);
    syscall3(SYS_WRITE, 1, (long)" committed commands=", 20);
    cost_number(committed.commands - before.commands);
    syscall3(SYS_WRITE, 1, (long)" flushes=", 9);
    cost_number(committed.flushes - before.flushes);
    puts("");
}

/** Write one whole block of the byte `pattern`, which identifies its last
 * writer, to `fd`; a short write fails the probe. */
static void cost_write(int fd, unsigned char pattern)
{
    unsigned i;

    for (i = 0; i < sizeof(cost_block); i++) {
        cost_block[i] = pattern;
    }
    if (syscall3(SYS_WRITE, fd, (long)cost_block, sizeof(cost_block)) != (int)sizeof(cost_block)) {
        fail("writing a cost block");
    }
}

/** Create `path`, which may not exist, write one block of `pattern` and
 * fsync it, before a count begins: the open descriptor. */
static int cost_create(const char *path, unsigned char pattern)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_RDWR | O_CREAT | O_EXCL, 0644);

    if (fd < 0) {
        fail("creating a cost file");
    }
    cost_write(fd, pattern);
    if (syscall1(SYS_FSYNC, fd) != 0) {
        fail("syncing a cost fixture");
    }
    return fd;
}

/** Reopen `path` after the count and check that it is `blocks` blocks long
 * and that block i holds the byte `first` + i throughout. */
static void cost_verify(const char *path, unsigned first, unsigned blocks)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    unsigned block, i;

    if (fd < 0) {
        fail("reopening a cost file");
    }
    for (block = 0; block < blocks; block++) {
        if (syscall3(SYS_READ, fd, (long)cost_block, sizeof(cost_block)) !=
            (int)sizeof(cost_block)) {
            fail("reading a cost block");
        }
        for (i = 0; i < sizeof(cost_block); i++) {
            if (cost_block[i] != first + block) {
                fail("a cost block has the wrong bytes");
            }
        }
    }
    if (syscall3(SYS_READ, fd, (long)cost_block, 1) != 0 || syscall1(SYS_CLOSE, fd) != 0) {
        fail("a cost file has the wrong length or failed close");
    }
}

/**
 * Count sixteen rewrites of one block and two appends, each with its final
 * fsync. The rewrites may cost four commands and two flushes, the cost of one
 * data write and one inode write, and none before the fsync. The two appends
 * together may cost at most ten commands and five flushes, what one append
 * costs when each write call is written through on its own, and no flush
 * before the fsync. With `append_only` the rewrites' counts are printed and
 * not judged.
 */
static void cost_rows(int append_only)
{
    struct disk_counts before, collected, committed;
    int fd = cost_create("/e2/cost-rewrite", 0);
    unsigned i;

    before = disk_counts();
    for (i = 1; i <= 16; i++) {
        if (syscall3(SYS_LSEEK, fd, 0, 0) != 0) {
            fail("seeking to rewrite a cost block");
        }
        cost_write(fd, (unsigned char)i);
    }
    collected = disk_counts();
    if (syscall1(SYS_FSYNC, fd) != 0) {
        fail("syncing the rewrites");
    }
    committed = disk_counts();
    cost_report("rewrite", before, collected, committed);
    if (!append_only &&
        (committed.commands - before.commands != 4 || committed.flushes - before.flushes != 2)) {
        fail("rewrite command count");
    }
    if (!append_only &&
        (collected.commands != before.commands || collected.flushes != before.flushes)) {
        fail("rewrite collection issued commands");
    }
    if (syscall1(SYS_CLOSE, fd) != 0) {
        fail("closing the rewrite file");
    }
    cost_verify("/e2/cost-rewrite", 16, 1);

    fd = cost_create("/e2/cost-append", 64);
    before = disk_counts();
    cost_write(fd, 65);
    cost_write(fd, 66);
    collected = disk_counts();
    if (syscall1(SYS_FSYNC, fd) != 0) {
        fail("syncing the appends");
    }
    committed = disk_counts();
    cost_report("append", before, collected, committed);
    if (committed.commands - before.commands > 10 || committed.flushes - before.flushes > 5 ||
        committed.commands == before.commands || committed.flushes == before.flushes) {
        fail("append command count");
    }
    if (collected.flushes != before.flushes) {
        fail("append collection issued flushes");
    }
    if (syscall1(SYS_CLOSE, fd) != 0) {
        fail("closing the append file");
    }
    cost_verify("/e2/cost-append", 64, 3);
}

PROBE_SP_ENTRY;

/** The probe's entry, given the initial stack `sp`: the mode from the first
 * argument, `links` by default, run on the fixture mounted on /e2. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    const char *mode = argc > 1 ? argv[1] : "links";
    int writable = streq(mode, "cost") || streq(mode, "cost-append");

    syscall2(SYS_MKDIR, (long)"/e2", 0755);
    if (syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/e2", (long)"ext2",
                 writable ? MS_SYNCHRONOUS : MS_RDONLY, 0) != 0) {
        fail("mounting the fixture");
    }
    if (streq(mode, "links")) {
        links_rows();
    } else if (writable) {
        cost_rows(streq(mode, "cost-append"));
    } else {
        fail("unknown mode");
    }
    if (syscall2(SYS_UMOUNT2, (long)"/e2", 0) != 0) {
        fail("unmount");
    }
    syscall3(SYS_WRITE, 1, (long)"EXT2PROBE OK (", 14);
    syscall3(SYS_WRITE, 1, (long)mode, (int)strlen(mode));
    puts(")");
    sys_exit(0);
}
