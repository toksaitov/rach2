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
 * `ext2probe xattr` runs all twelve attribute calls on a writable fixture,
 * checks remount and final links, and checks KRFFS and tmpfs defaults.
 *
 * `ext2probe direct` mounts the fixture for writing and runs the rows of
 * O_DIRECT, O_TMPFILE and renameat2 on ext2: an O_DIRECT write and read of
 * whole sectors from a sector-aligned buffer, which leave no block of the
 * file in the cache by cachestat(2), the EINVAL of a buffer, a length or an
 * offset off the sector and the 0 of such a read past the end, F_SETFL
 * clearing and setting O_DIRECT, a directory opened with it; a file with no
 * name from O_TMPFILE, which takes an inode, linkat with AT_EMPTY_PATH
 * names, and whose inode a last close frees when it has none; and the
 * EINVAL of RENAME_EXCHANGE, which Linux's ext2 refuses too.
 *
 * `ext2probe cost` mounts the same fixture for writing with `MS_SYNCHRONOUS`
 * and counts, through `/proc/disk`, the commands and cache flushes of
 * repeated rewrites and of appends. A mount without `sync` writes back, so
 * its writes cost no commands before fsync whether or not ext2 batches them;
 * the batching saves commands on the ordered write-through of a `sync`
 * mount. Both counts include fsync, and both files are reopened and checked.
 *
 * `ext2probe names` mounts the fixture for writing and judges the name
 * cache by its counts in /proc/stat: warm walks of the deep paths of the
 * report's fs_thread_scaling.c, fstatat in a directory descriptor and
 * processes walking at once read no directory; each mutator that makes or
 * takes away a name is seen though the cache had the name's answer; removed
 * files and directories give back their inodes; a cached name is EACCES to a
 * user who may not search its directory; and an unmount forgets the
 * volume's names. The counts are the machine's, so no native run has it.
 *
 * `ext2probe lookup` fills the buffer cache to its limit from the fixture
 * and from copies of it on loop devices, then makes a thousand files in a
 * directory of the fixture and looks each up once, a lookup the name cache
 * passes to ext2 with its blocks in the buffer cache. It judges by the
 * counts of /proc/disk and /proc/stat that the cache is full and its index
 * has a bucket for each buffer, that a get at the cap and a get of a lookup
 * read few buffers, and that a lookup reads few of ext2's nodes in memory.
 * The counts are the machine's, so no native run has it.
 *
 * A native run gives the fixture as a loop device (probe_args.h): the first
 * device that `loops=DEVICE,...` names is mounted in place of /dev/disk0p1,
 * and `cost` counts that device's own writes and syncs, from /proc/loop, in
 * place of the commands and flushes of every disk, from /proc/disk, which
 * the other processes of a running machine move.
 *
 * Prints "EXT2PROBE OK (mode)" or "EXT2PROBE FAIL: reason". Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_SYMLINK_FOLLOW   0x400

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0100
#define O_EXCL     0200
#define O_NOFOLLOW 0400000

#define S_IFIFO 0010000

#define MS_RDONLY      1
#define MS_SYNCHRONOUS 16

#define EPERM  1
#define ENOENT 2
#define EACCES 13
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

/**
 * The rows of `ext2probe links`, on the fixture mounted read-only on /e2;
 * fails the probe at the first row that fails. `..` is physical, so through
 * a link to a directory it is the target's parent, and O_CREAT through a
 * dangling link creates the target, which the read-only mount refuses with
 * an answer and not a loop.
 */
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

/* The disk's counts of commands and cache flushes, from /proc/disk, or the
 * loop device's writes and syncs, from /proc/loop. */
struct disk_counts {
    unsigned commands;
    unsigned flushes;
};

/* The fixture's node, and the name of its loop device, such as "loop2", or
 * "" when it is not on one. */
static char node[64] = "/dev/disk0p1";
static char loop_name[16];

static unsigned char cost_block[4096];

/** The unsigned counter on the line `key` of the text `text` of /proc/disk
 * or /proc/stat, "key value"; a counter that is absent, empty, too large or
 * followed by other text fails the probe. */
static unsigned text_counter(const char *text, const char *key)
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
                fail("a counter has no value");
            }
            while (*p >= '0' && *p <= '9') {
                unsigned digit = (unsigned)(*p++ - '0');

                if (value > (0xFFFFFFFFu - digit) / 10) {
                    fail("a counter overflows");
                }
                value = value * 10 + digit;
            }
            if (*p != '\n' && *p != '\0') {
                fail("a counter has trailing text");
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
    fail("a counter is absent");
}

/** The unsigned number after the word `key` and a space on the line that
 * begins at `line`, past its first byte; an absent or empty number fails the
 * probe. */
static unsigned loop_counter(const char *line, const char *key)
{
    size_t len = strlen(key);

    for (line++; *line && *line != '\n'; line++) {
        size_t i = 0;

        while (i < len && line[i] == key[i]) {
            i++;
        }
        if (i == len && line[-1] == ' ' && line[i] == ' ' && line[i + 1] >= '0' &&
            line[i + 1] <= '9') {
            unsigned value = 0;

            for (line += i + 1; *line >= '0' && *line <= '9'; line++) {
                value = value * 10 + (unsigned)(*line - '0');
            }
            return value;
        }
    }
    fail("a loop counter is absent");
}

/** The loop device's writes and syncs now, from its line of /proc/loop,
 * "loopN BYTES rw writes W syncs S PATH": W + S commands, since a disk's
 * count of commands has its flushes in it, and S flushes. */
static struct disk_counts loop_counts(void)
{
    static char text[1024];
    struct disk_counts counts;
    size_t len = strlen(loop_name);
    int n = read_all("/proc/loop", text, sizeof(text) - 1);
    int i;

    if (n <= 0 || n == (int)sizeof(text) - 1) {
        fail("reading the loop counters");
    }
    text[n] = '\0';
    for (i = 0; i < n; i++) {
        size_t k = 0;

        while (k < len && text[i + k] == loop_name[k]) {
            k++;
        }
        if ((i == 0 || text[i - 1] == '\n') && k == len && text[i + len] == ' ') {
            counts.flushes = loop_counter(text + i, "syncs");
            counts.commands = loop_counter(text + i, "writes") + counts.flushes;
            return counts;
        }
    }
    fail("the loop device has no line in /proc/loop");
}

/** The disk's counts now, or the loop device's. Reading `/proc/disk` takes
 * a snapshot without issuing a disk command. */
static struct disk_counts disk_counts(void)
{
    static char text[8192];
    struct disk_counts counts;
    int n;

    if (loop_name[0]) {
        return loop_counts();
    }
    n = read_all("/proc/disk", text, sizeof(text) - 1);
    if (n <= 0 || n == (int)sizeof(text) - 1) {
        fail("reading the disk counters");
    }
    text[n] = '\0';
    counts.commands = text_counter(text, "disk-commands");
    counts.flushes = text_counter(text, "disk-flushes");
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

/** Create `path`, which must not exist yet, write one block of `pattern`
 * and fsync it, before a count begins: the open descriptor. */
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
 * fsync. The rewrites cost exactly four commands and two flushes, the cost of
 * one data write and one inode write, and none before the fsync. The two
 * appends together cost at least one command and one flush and at most ten
 * commands and five flushes, what one append costs when each write call is
 * written through on its own, and no flush before the fsync.
 */
static void cost_rows(void)
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
    if (committed.commands - before.commands != 4 || committed.flushes - before.flushes != 2) {
        fail("rewrite command count");
    }
    if (collected.commands != before.commands || collected.flushes != before.flushes) {
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

/* The rows of `ext2probe names`. */

#define NAMES_ROUNDS   200 /* warm walks of each path */
#define NAMES_CHILDREN 4   /* processes that walk together */

/* The name cache's counts of /proc/stat: lookups it answered, and those it
 * passed to ext2, each a read of a directory. */
struct name_counts {
    unsigned hits;
    unsigned misses;
};

/** The name cache's counts now. */
static struct name_counts name_counts(void)
{
    static char text[8192];
    struct name_counts counts;
    int n = read_all("/proc/stat", text, sizeof(text) - 1);

    if (n <= 0 || n == (int)sizeof(text) - 1) {
        fail("reading /proc/stat");
    }
    text[n] = '\0';
    counts.hits = text_counter(text, "namecache_hits");
    counts.misses = text_counter(text, "namecache_misses");
    return counts;
}

/** stat of `path` into the stat buffer: 0, or the negated errno. */
static int stat_rc(const char *path)
{
    return syscall2(SYS_STAT, (long)path, (long)st);
}

/** The free inodes of the fixture, which statfs reports. */
static unsigned free_inodes(void)
{
    static unsigned char fs[KSTATFS_SIZE];

    if (probe_statfs("/e2", fs) != 0) {
        fail("statfs of the fixture");
    }
    return (unsigned)(fs[KSTATFS_FFREE] | (fs[KSTATFS_FFREE + 1] << 8) |
                      (fs[KSTATFS_FFREE + 2] << 16) | ((unsigned)fs[KSTATFS_FFREE + 3] << 24));
}

/** Fail `what` unless stat of `path` answers `want`, 0 or an errno, twice,
 * the second time without reading a directory: the answer is the cache's. */
static void expect_cached(const char *path, int want, const char *what)
{
    struct name_counts before, after;
    int rc = stat_rc(path);

    if (rc != -want) {
        fail_with(what, rc);
    }
    before = name_counts();
    rc = stat_rc(path);
    after = name_counts();
    if (rc != -want) {
        fail_with(what, rc);
    }
    if (after.misses != before.misses || after.hits == before.hits) {
        fail_with(what, (int)(after.misses - before.misses));
    }
}

/** Fail `what` unless stat of `path` answers `want`, 0 or an errno. */
static void expect_stat(const char *path, int want, const char *what)
{
    int rc = stat_rc(path);

    if (rc != -want) {
        fail_with(what, rc);
    }
}

/** Fail `what` unless the call's result `rc` is 0. */
static void expect_ok(int rc, const char *what)
{
    if (rc != 0) {
        fail_with(what, rc);
    }
}

/** Make the empty file `path`, or fail `what`. */
static void make_file(const char *path, const char *what)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_EXCL, 0644);

    if (fd < 0) {
        fail_with(what, fd);
    }
    syscall1(SYS_CLOSE, fd);
}

/** Wait for the child `pid` and fail `what` unless it exited with 0. */
static void expect_child(int pid, const char *what)
{
    int status = -1;

    if (pid < 0) {
        fail_with("fork", pid);
    }
    if (syscall4(SYS_WAIT4, pid, (long)&status, 0, 0) != pid || status != 0) {
        fail_with(what, status);
    }
}

/* The report's paths, from fs_thread_scaling.c, and the names on ext2 of
 * each: /e2 is a mount over a KRFFS directory, whose lookup is not ext2's. */
static const char *const deep_paths[] = {"/e2/nc/deep/f", "/e2/nc/deep/1/2/3/f",
                                         "/e2/nc/deep/1/2/3/4/5/6/7/f"};
static const unsigned deep_names[] = {3, 6, 10};
static const char *const deep_dirs[] = {"/e2/nc",
                                        "/e2/nc/deep",
                                        "/e2/nc/deep/1",
                                        "/e2/nc/deep/1/2",
                                        "/e2/nc/deep/1/2/3",
                                        "/e2/nc/deep/1/2/3/4",
                                        "/e2/nc/deep/1/2/3/4/5",
                                        "/e2/nc/deep/1/2/3/4/5/6",
                                        "/e2/nc/deep/1/2/3/4/5/6/7"};

#define DEEP_PATHS 3
#define DEEP_DIRS  9

/**
 * The report's walks, warm: stat of each deep path, fstatat of a name in a
 * directory descriptor, and NAMES_CHILDREN processes walking one path at
 * once, NAMES_ROUNDS times each, read no directory by the kernel's count,
 * and the cache answers each name of each walk.
 */
static void warm_rows(void)
{
    struct name_counts before, after;
    unsigned want = 0;
    int i, r, fd, pid[NAMES_CHILDREN];

    for (i = 0; i < DEEP_DIRS; i++) {
        expect_ok(syscall2(SYS_MKDIR, (long)deep_dirs[i], 0755), "making the deep directories");
        expect_ok(syscall2(SYS_CHMOD, (long)deep_dirs[i], 0755), "opening the deep directories");
    }
    for (i = 0; i < DEEP_PATHS; i++) {
        make_file(deep_paths[i], "making the deep files");
        expect_stat(deep_paths[i], 0, "a cold walk of a deep path");
    }
    fd = syscall3(SYS_OPEN, (long)"/e2/nc/deep", O_RDONLY, 0);
    if (fd < 0) {
        fail_with("opening the deep directory", fd);
    }
    before = name_counts();
    for (r = 0; r < NAMES_ROUNDS; r++) {
        for (i = 0; i < DEEP_PATHS; i++) {
            expect_stat(deep_paths[i], 0, "a warm walk of a deep path");
            want += deep_names[i];
        }
        if (syscall4(SYS_FSTATAT, fd, (long)"f", (long)st, 0) != 0) {
            fail("fstatat of a name in the deep directory");
        }
        want++;
    }
    for (i = 0; i < NAMES_CHILDREN; i++) {
        pid[i] = syscall0(SYS_FORK);
        if (pid[i] == 0) {
            for (r = 0; r < NAMES_ROUNDS; r++) {
                if (stat_rc(deep_paths[1]) != 0) {
                    sys_exit(1);
                }
            }
            sys_exit(0);
        }
        want += NAMES_ROUNDS * deep_names[1];
    }
    for (i = 0; i < NAMES_CHILDREN; i++) {
        expect_child(pid[i], "a process walking with others");
    }
    after = name_counts();
    syscall1(SYS_CLOSE, fd);
    syscall3(SYS_WRITE, 1, (long)"ext2-names: result: warm names=", 31);
    cost_number(after.hits - before.hits);
    syscall3(SYS_WRITE, 1, (long)" directory reads=", 17);
    cost_number(after.misses - before.misses);
    puts("");
    if (after.misses != before.misses) {
        fail_with("warm walks read no directory", (int)(after.misses - before.misses));
    }
    if (after.hits - before.hits < want) {
        fail_with("the cache answers each name of a warm walk", (int)(after.hits - before.hits));
    }
}

/**
 * Each mutator that makes a name, then each that takes one away, with the
 * name's answer in the cache before it: creat, mkdir, symlink, mknod, link
 * and rename onto a name the cache had as missing find it; unlink, rmdir
 * and rename away from a name the cache had find it gone. A removed file and
 * a removed directory with a negative entry in it give back their inodes,
 * and a file a rename replaces gives back its own.
 */
static void mutator_rows(void)
{
    unsigned free0, ino;

    expect_cached("/e2/nc/m-creat", ENOENT, "a missing name is cached before creat");
    make_file("/e2/nc/m-creat", "creat");
    expect_stat("/e2/nc/m-creat", 0, "creat makes a name the cache had as missing");

    expect_cached("/e2/nc/m-dir", ENOENT, "a missing name is cached before mkdir");
    expect_ok(syscall2(SYS_MKDIR, (long)"/e2/nc/m-dir", 0755), "mkdir");
    expect_stat("/e2/nc/m-dir", 0, "mkdir makes a name the cache had as missing");

    expect_cached("/e2/nc/m-sym", ENOENT, "a missing name is cached before symlink");
    expect_ok(syscall2(SYS_SYMLINK, (long)"m-creat", (long)"/e2/nc/m-sym"), "symlink");
    expect_stat("/e2/nc/m-sym", 0, "symlink makes a name the cache had as missing");

    expect_cached("/e2/nc/m-fifo", ENOENT, "a missing name is cached before mknod");
    expect_ok(syscall3(SYS_MKNOD, (long)"/e2/nc/m-fifo", S_IFIFO | 0644, 0), "mknod of a FIFO");
    expect_stat("/e2/nc/m-fifo", 0, "mknod makes a name the cache had as missing");

    expect_cached("/e2/nc/m-link", ENOENT, "a missing name is cached before link");
    expect_ok(syscall2(SYS_LINK, (long)"/e2/nc/m-creat", (long)"/e2/nc/m-link"), "link");
    expect_stat("/e2/nc/m-link", 0, "link makes a name the cache had as missing");

    expect_cached("/e2/nc/m-to", ENOENT, "a missing name is cached before a rename onto it");
    expect_cached("/e2/nc/m-link", 0, "a name is cached before a rename away from it");
    expect_ok(syscall2(SYS_RENAME, (long)"/e2/nc/m-link", (long)"/e2/nc/m-to"), "rename");
    expect_stat("/e2/nc/m-to", 0, "rename makes a name the cache had as missing");
    expect_stat("/e2/nc/m-link", ENOENT, "rename takes away a name the cache had");

    expect_cached("/e2/nc/m-to", 0, "a name is cached before unlink");
    expect_ok(syscall1(SYS_UNLINK, (long)"/e2/nc/m-to"), "unlink of a second link");
    expect_stat("/e2/nc/m-to", ENOENT, "unlink takes away a name the cache had");

    free0 = free_inodes();
    make_file("/e2/nc/m-file", "creat of a file to unlink");
    expect_cached("/e2/nc/m-file", 0, "a file is cached before unlink");
    expect_ok(syscall1(SYS_UNLINK, (long)"/e2/nc/m-file"), "unlink of a last link");
    expect_stat("/e2/nc/m-file", ENOENT, "unlink takes away a file the cache had");
    if (free_inodes() != free0) {
        fail_with("an unlinked file the cache had gives back its inode", (int)free_inodes());
    }

    free0 = free_inodes();
    expect_ok(syscall2(SYS_MKDIR, (long)"/e2/nc/m-gone", 0755), "mkdir of a directory to remove");
    expect_cached("/e2/nc/m-gone/none", ENOENT, "a missing name in a directory is cached");
    expect_cached("/e2/nc/m-gone", 0, "a directory is cached before rmdir");
    expect_ok(syscall1(SYS_RMDIR, (long)"/e2/nc/m-gone"), "rmdir");
    expect_stat("/e2/nc/m-gone", ENOENT, "rmdir takes away a name the cache had");
    if (free_inodes() != free0) {
        fail_with("a removed directory with a cached entry gives back its inode",
                  (int)free_inodes());
    }

    free0 = free_inodes();
    make_file("/e2/nc/m-a", "creat of a file to rename");
    make_file("/e2/nc/m-b", "creat of a file to replace");
    expect_stat("/e2/nc/m-a", 0, "stat of the file to rename");
    ino = (unsigned)st_word(KSTAT_INO);
    expect_cached("/e2/nc/m-a", 0, "a file is cached before a rename away from it");
    expect_cached("/e2/nc/m-b", 0, "a file is cached before a rename replaces it");
    expect_ok(syscall2(SYS_RENAME, (long)"/e2/nc/m-a", (long)"/e2/nc/m-b"), "rename over a file");
    expect_stat("/e2/nc/m-b", 0, "the name a rename replaced");
    if ((unsigned)st_word(KSTAT_INO) != ino) {
        fail("the name a rename replaced names the file renamed");
    }
    expect_stat("/e2/nc/m-a", ENOENT, "a rename over a file takes its old name away");
    if (free_inodes() != free0 - 1) {
        fail_with("the file a rename replaced gives back its inode", (int)free_inodes());
    }

    expect_ok(syscall2(SYS_MKDIR, (long)"/e2/nc/m-tree", 0755), "mkdir of a directory to move");
    make_file("/e2/nc/m-tree/f", "creat in a directory to move");
    expect_cached("/e2/nc/m-tree/f", 0, "a name in a directory is cached before it moves");
    expect_ok(syscall2(SYS_RENAME, (long)"/e2/nc/m-tree", (long)"/e2/nc/m-moved"),
              "rename of a directory");
    expect_stat("/e2/nc/m-tree/f", ENOENT, "a moved directory's old path is gone");
    expect_stat("/e2/nc/m-moved/f", 0, "a moved directory's names go with it");
}

/**
 * A name in a directory only root may search, and a missing name there,
 * both in the cache from root's walks, are EACCES to a process of uid 1000,
 * which may still walk the deep path.
 */
static void denied_rows(void)
{
    int pid;

    expect_ok(syscall2(SYS_MKDIR, (long)"/e2/nc/priv", 0700), "mkdir of a private directory");
    expect_ok(syscall2(SYS_CHMOD, (long)"/e2/nc/priv", 0700), "closing the private directory");
    make_file("/e2/nc/priv/f", "creat in the private directory");
    expect_cached("/e2/nc/priv/f", 0, "root's walk into the private directory is cached");
    expect_cached("/e2/nc/priv/none", ENOENT, "root's missing name there is cached");
    pid = syscall0(SYS_FORK);
    if (pid == 0) {
        if (syscall2(SYS_SETGROUPS, 0, 0) != 0 || syscall1(SYS_SETGID, 1000) != 0 ||
            syscall1(SYS_SETUID, 1000) != 0) {
            sys_exit(1);
        }
        if (stat_rc(deep_paths[0]) != 0) {
            sys_exit(2);
        }
        if (stat_rc("/e2/nc/priv/f") != -EACCES) {
            sys_exit(3);
        }
        if (stat_rc("/e2/nc/priv/none") != -EACCES) {
            sys_exit(4);
        }
        sys_exit(0);
    }
    expect_child(pid, "a cached name in a directory uid 1000 may not search is EACCES to it");
}

/**
 * The volume unmounted with its entries in the cache and mounted again: the
 * first walk of a deep path reads each of its three directories again.
 */
static void remount_rows(void)
{
    struct name_counts before, after;

    expect_cached(deep_paths[0], 0, "a deep path is cached before the unmount");
    expect_ok(syscall2(SYS_UMOUNT2, (long)"/e2", 0), "unmount with names in the cache");
    expect_ok(syscall5(SYS_MOUNT, (long)node, (long)"/e2", (long)"ext2", 0, 0), "mounting again");
    before = name_counts();
    expect_stat(deep_paths[0], 0, "a deep path after the volume is mounted again");
    after = name_counts();
    if (after.misses - before.misses < deep_names[0]) {
        fail_with("a volume mounted again reads its directories again",
                  (int)(after.misses - before.misses));
    }
}

/** Remove what the rows made, deepest first. */
static void names_clean(void)
{
    static const char *const files[] = {"/e2/nc/priv/f", "/e2/nc/m-moved/f", "/e2/nc/m-b",
                                        "/e2/nc/m-fifo", "/e2/nc/m-sym",     "/e2/nc/m-creat"};
    static const char *const dirs[] = {"/e2/nc/priv", "/e2/nc/m-moved", "/e2/nc/m-dir"};
    int i;

    for (i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++) {
        expect_ok(syscall1(SYS_UNLINK, (long)files[i]), "removing a file the rows made");
    }
    for (i = 0; i < DEEP_PATHS; i++) {
        expect_ok(syscall1(SYS_UNLINK, (long)deep_paths[i]), "removing a deep file");
    }
    for (i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); i++) {
        expect_ok(syscall1(SYS_RMDIR, (long)dirs[i]), "removing a directory the rows made");
    }
    for (i = DEEP_DIRS - 1; i >= 0; i--) {
        expect_ok(syscall1(SYS_RMDIR, (long)deep_dirs[i]), "removing a deep directory");
    }
}

/** The rows of `ext2probe names`, on the fixture mounted for writing on /e2;
 * fails the probe at the first row that fails. */
static void names_rows(void)
{
    warm_rows();
    mutator_rows();
    denied_rows();
    remount_rows();
    names_clean();
}

/*
 * The rows of `ext2probe lookup`.
 *
 * The buffer cache is filled to its limit first, from the fixture and from
 * LOOKUP_COPIES copies of it on loop devices, each a device of its own whose
 * blocks the cache keys apart, since the fixture alone has fewer blocks than
 * the cache of a machine of some memory holds. Then LOOKUP_FILES files are
 * made in one directory and each is looked up once: a lookup the name cache
 * passes to ext2, with its directory and inode blocks in the buffer cache.
 * The kernel's counts judge what each search read: the buffers of the
 * cache's walks for each get, and ext2's nodes in memory for each lookup,
 * which the name cache holds one of for every name it keeps.
 */

#define LOOKUP_COPIES        3    /* loop devices over copies of the fixture */
#define LOOKUP_FILES         1000 /* files made and looked up */
#define LOOKUP_SPARE         192  /* blocks the fill leaves free for the files */
/* The buckets of the buffer cache's first index, which a full cache of this
 * case's machine must outgrow. */
#define LOOKUP_FIRST_BUCKETS 4096u
/* The most buffers the cache's walks may read for each get on average, in
 * tenths: of a read at the cap, where each miss takes a victim, and of a
 * lookup. A read at the cap reads some 2.5 with an index of a bucket for
 * each buffer and 4 with the first index's 4096 buckets. */
#define LOOKUP_FILL_TENTHS   35
#define LOOKUP_WARM_TENTHS   30
/* The most nodes ext2's searches may read for each lookup on average. */
#define LOOKUP_NODE_VISITS   2

/* The counts of /proc/disk and /proc/stat the rows judge. */
struct cache_counts {
    unsigned hits, fills, visits;
    unsigned buffers, limit, buckets;
    unsigned nodes, misses;
};

/** The buffer cache's and ext2's counts now. */
static struct cache_counts cache_counts(void)
{
    static char text[8192];
    struct cache_counts c;
    int n = read_all("/proc/disk", text, sizeof(text) - 1);

    if (n <= 0 || n == (int)sizeof(text) - 1) {
        fail("reading /proc/disk");
    }
    text[n] = '\0';
    c.hits = text_counter(text, "cache-hits");
    c.fills = text_counter(text, "cache-fills");
    c.visits = text_counter(text, "cache-visits");
    c.buffers = text_counter(text, "cache-buffers");
    c.limit = text_counter(text, "cache-limit");
    c.buckets = text_counter(text, "cache-buckets");
    n = read_all("/proc/stat", text, sizeof(text) - 1);
    if (n <= 0 || n == (int)sizeof(text) - 1) {
        fail("reading /proc/stat");
    }
    text[n] = '\0';
    c.nodes = text_counter(text, "ext2_node_visits");
    c.misses = text_counter(text, "namecache_misses");
    return c;
}

/** Print "ext2-lookup: result: " and `what`, then `value`, for a person. */
static void lookup_report(const char *what, unsigned value)
{
    syscall3(SYS_WRITE, 1, (long)"ext2-lookup: result: ", 21);
    syscall3(SYS_WRITE, 1, (long)what, (int)strlen(what));
    cost_number(value);
    puts("");
}

/** The free blocks of the fixture, which statfs reports. */
static unsigned free_blocks(void)
{
    static unsigned char fs[KSTATFS_SIZE];
    int at = KSTATFS_FILES - 16; /* f_bfree, two 64-bit words before f_files */

    if (probe_statfs("/e2", fs) != 0) {
        fail("statfs of the fixture");
    }
    return (unsigned)(fs[at] | (fs[at + 1] << 8) | (fs[at + 2] << 16) |
                      ((unsigned)fs[at + 3] << 24));
}

/** Write /e2/lk-fill until the fixture has LOOKUP_SPARE blocks free, and
 * sync it. */
static void lookup_fill_file(void)
{
    int fd = syscall3(SYS_OPEN, (long)"/e2/lk-fill", O_WRONLY | O_CREAT | O_EXCL, 0644);
    unsigned i;

    if (fd < 0) {
        fail_with("creating the fill file", fd);
    }
    for (i = 0; i < sizeof(cost_block); i++) {
        cost_block[i] = (unsigned char)i;
    }
    while (free_blocks() > LOOKUP_SPARE) {
        if (syscall3(SYS_WRITE, fd, (long)cost_block, sizeof(cost_block)) !=
            (int)sizeof(cost_block)) {
            fail("writing the fill file");
        }
    }
    if (syscall1(SYS_FSYNC, fd) != 0 || syscall1(SYS_CLOSE, fd) != 0) {
        fail("syncing the fill file");
    }
}

/** Read the file `path` to its end. */
static void read_through(const char *path)
{
    static unsigned char chunk[65536];
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int n;

    if (fd < 0) {
        fail_with("opening a fill file", fd);
    }
    while ((n = syscall3(SYS_READ, fd, (long)chunk, sizeof(chunk))) > 0) {
    }
    if (n < 0) {
        fail_with("reading a fill file", n);
    }
    syscall1(SYS_CLOSE, fd);
}

/** Copy the fixture's partition to the file `to` through its device node,
 * whose reads pass the buffer cache by. */
static void copy_fixture(const char *to)
{
    static unsigned char chunk[65536];
    int in = syscall3(SYS_OPEN, (long)node, O_RDONLY, 0);
    int out = syscall3(SYS_OPEN, (long)to, O_WRONLY | O_CREAT | O_EXCL, 0644);
    int n;

    if (in < 0 || out < 0) {
        fail("opening the fixture and its copy");
    }
    while ((n = syscall3(SYS_READ, in, (long)chunk, sizeof(chunk))) > 0) {
        if (syscall3(SYS_WRITE, out, (long)chunk, n) != n) {
            fail("writing a copy of the fixture");
        }
    }
    if (n < 0) {
        fail_with("reading the fixture's device", n);
    }
    syscall1(SYS_CLOSE, in);
    syscall1(SYS_CLOSE, out);
}

/** One command to /dev/loop-control, or fail `what`. */
static void loop_control(const char *line, const char *what)
{
    int fd = syscall3(SYS_OPEN, (long)"/dev/loop-control", O_WRONLY, 0);
    int rc;

    if (fd < 0) {
        fail_with("opening /dev/loop-control", fd);
    }
    rc = syscall3(SYS_WRITE, fd, (long)line, (int)strlen(line));
    syscall1(SYS_CLOSE, fd);
    if (rc < 0) {
        fail_with(what, rc);
    }
}

/* The copies' files, loop devices, mount points and commands, by index. */
static char copy_path[] = "/lk/v0";
static char loop_path[] = "/dev/loop0";
static char loop_dir[] = "/l0";
static char loop_file[] = "/l0/lk-fill";
static char attach_line[] = "attach ro 0 /lk/v0";
static char detach_line[] = "detach 0";

/** Point the copy's names at index `i`. */
static void copy_names(int i)
{
    char digit = (char)('0' + i);

    copy_path[5] = digit;
    loop_path[9] = digit;
    loop_dir[2] = digit;
    loop_file[2] = digit;
    attach_line[10] = digit;
    attach_line[17] = digit;
    detach_line[7] = digit;
}

/* The loop device over a fill file, the one the copies leave free. */
#define FILL_LOOP "3"

/**
 * Attach the loop device FILL_LOOP over the fill file `path`, on an ext2
 * volume, for `fill_read`.
 */
static void fill_attach(const char *path)
{
    static char line[48] = "attach ro " FILL_LOOP " ";
    int at = 12, i;

    for (i = 0; path[i] && at + i < (int)sizeof(line) - 1; i++) {
        line[at + i] = path[i];
    }
    line[at + i] = '\0';
    loop_control(line, "attaching a loop device over a fill file");
}

/**
 * Read the fill file attached to FILL_LOOP through the device, and detach
 * it unless `keep`. A read of the file itself fills pages of the file page
 * cache straight from the disk, as Linux's readahead does, and leaves the
 * buffer cache alone; the loop device reads its file through ext2's own
 * read, which gets each block from the buffer cache, so this read fills it
 * with the volume's blocks. A detach walks the whole cache for the device's
 * buffers, which a judged read leaves out by keeping it.
 */
static void fill_read(int keep)
{
    read_through("/dev/loop" FILL_LOOP);
    if (!keep) {
        loop_control("detach " FILL_LOOP, "detaching the fill file's loop device");
    }
}

/** Read the fill file `path` through FILL_LOOP, as `fill_read` says. */
static void fill_through(const char *path)
{
    fill_attach(path);
    fill_read(0);
}

/**
 * Fill the buffer cache to its limit: the fill file on the fixture, the
 * fixture unmounted and copied LOOKUP_COPIES times to a tmpfs, each copy on a
 * loop device mounted read-only, and each fill file read through a loop
 * device over it (`fill_through`), more blocks than the cache holds. The
 * fixture's fill file, whose blocks went first, is read again last; that
 * read is at the cap, where each miss takes a victim, and the walks of its
 * gets are judged. The cache must then be full, larger than its first
 * index, and its index must have a bucket for each buffer it may hold.
 */
static void lookup_fill(void)
{
    struct cache_counts before, after;
    int i;

    lookup_fill_file();
    expect_ok(syscall2(SYS_UMOUNT2, (long)"/e2", 0), "unmounting the fixture to copy it");
    syscall2(SYS_MKDIR, (long)"/lk", 0755);
    expect_ok(syscall5(SYS_MOUNT, (long)"tmpfs", (long)"/lk", (long)"tmpfs", 0, 0),
              "mounting a tmpfs for the copies");
    for (i = 0; i < LOOKUP_COPIES; i++) {
        copy_names(i);
        copy_fixture(copy_path);
        loop_control(attach_line, "attaching a copy of the fixture");
        syscall2(SYS_MKDIR, (long)loop_dir, 0755);
        expect_ok(syscall5(SYS_MOUNT, (long)loop_path, (long)loop_dir, (long)"ext2", MS_RDONLY, 0),
                  "mounting a copy of the fixture");
    }
    expect_ok(syscall5(SYS_MOUNT, (long)node, (long)"/e2", (long)"ext2", 0, 0),
              "mounting the fixture again");
    fill_through("/e2/lk-fill");
    for (i = 0; i < LOOKUP_COPIES; i++) {
        copy_names(i);
        fill_through(loop_file);
    }
    fill_attach("/e2/lk-fill");
    before = cache_counts();
    fill_read(1);
    after = cache_counts();
    loop_control("detach " FILL_LOOP, "detaching the fill file's loop device");
    lookup_report("buffers=", after.buffers);
    lookup_report("limit=", after.limit);
    lookup_report("buckets=", after.buckets);
    lookup_report("cap fills=", after.fills - before.fills);
    lookup_report("cap gets=", (after.hits - before.hits) + (after.fills - before.fills));
    lookup_report("cap visits=", after.visits - before.visits);
    if (before.buffers != before.limit) {
        fail_with("the cache is full before the last read", (int)before.buffers);
    }
    if (after.buffers != after.limit) {
        fail_with("the cache is full", (int)after.buffers);
    }
    if (after.limit <= LOOKUP_FIRST_BUCKETS) {
        fail_with("the cache's limit is past its first index", (int)after.limit);
    }
    if (after.buckets < after.limit) {
        fail_with("the cache's index has a bucket for each buffer", (int)after.buckets);
    }
    if (after.fills - before.fills < after.limit / 8) {
        fail_with("the read at the cap fills buffers", (int)(after.fills - before.fills));
    }
    if ((after.visits - before.visits) * 10 >
        LOOKUP_FILL_TENTHS * ((after.hits - before.hits) + (after.fills - before.fills))) {
        fail_with("a get at the cap reads few buffers", (int)(after.visits - before.visits));
    }
}

/**
 * Make LOOKUP_FILES files in /e2/lk, then look each up once with stat: each
 * a lookup the name cache passes to ext2, since a name made is not cached,
 * whose blocks are in the buffer cache, so that no block is read. The
 * buffers each get reads and the nodes each lookup reads are judged.
 */
static void lookup_rows(void)
{
    static char path[] = "/e2/lk/f0000";
    struct cache_counts before, after;
    unsigned gets, lookups;
    int i;

    expect_ok(syscall2(SYS_MKDIR, (long)"/e2/lk", 0755), "making the lookup directory");
    for (i = 0; i < LOOKUP_FILES; i++) {
        path[8] = (char)('0' + i / 1000);
        path[9] = (char)('0' + i / 100 % 10);
        path[10] = (char)('0' + i / 10 % 10);
        path[11] = (char)('0' + i % 10);
        make_file(path, "making a file to look up");
    }
    before = cache_counts();
    for (i = 0; i < LOOKUP_FILES; i++) {
        path[8] = (char)('0' + i / 1000);
        path[9] = (char)('0' + i / 100 % 10);
        path[10] = (char)('0' + i / 10 % 10);
        path[11] = (char)('0' + i % 10);
        expect_stat(path, 0, "a lookup of a file made");
    }
    after = cache_counts();
    gets = (after.hits - before.hits) + (after.fills - before.fills);
    lookups = after.misses - before.misses;
    lookup_report("lookups=", lookups);
    lookup_report("gets=", gets);
    lookup_report("fills=", after.fills - before.fills);
    lookup_report("buffer visits=", after.visits - before.visits);
    lookup_report("node visits=", after.nodes - before.nodes);
    lookup_report("buffers=", after.buffers);
    if (lookups < LOOKUP_FILES) {
        fail_with("each lookup reaches ext2", (int)lookups);
    }
    if (gets < LOOKUP_FILES) {
        fail_with("each lookup gets its blocks from the cache", (int)gets);
    }
    if (after.fills != before.fills) {
        fail_with("a lookup with its blocks cached reads none", (int)(after.fills - before.fills));
    }
    if (after.buffers != after.limit) {
        fail_with("the cache is full through the lookups", (int)after.buffers);
    }
    if ((after.visits - before.visits) * 10 > LOOKUP_WARM_TENTHS * gets) {
        fail_with("a lookup's get reads few buffers", (int)(after.visits - before.visits));
    }
    if (after.nodes - before.nodes > LOOKUP_NODE_VISITS * lookups) {
        fail_with("a lookup reads few of ext2's nodes", (int)(after.nodes - before.nodes));
    }
}

/** Undo what the fill made: the copies' mounts, devices and files, and the
 * tmpfs. The fixture's files go with the disk's copy. */
static void lookup_clean(void)
{
    int i;

    for (i = 0; i < LOOKUP_COPIES; i++) {
        copy_names(i);
        expect_ok(syscall2(SYS_UMOUNT2, (long)loop_dir, 0), "unmounting a copy");
        loop_control(detach_line, "detaching a copy");
        expect_ok(syscall1(SYS_UNLINK, (long)copy_path), "removing a copy");
    }
    expect_ok(syscall2(SYS_UMOUNT2, (long)"/lk", 0), "unmounting the copies' tmpfs");
}

/** Take the fixture's node from the first device of `loops=`, when it is
 * given, and its last component as `loop_name`. */
static void take_loop(void)
{
    const char *loops = probe_arg("loops", 0);
    const char *last;
    int n = 0;

    if (loops == 0 || loops[0] == '\0') {
        return;
    }
    while (n < (int)sizeof(node) - 1 && loops[n] && loops[n] != ',') {
        node[n] = loops[n];
        n++;
    }
    node[n] = '\0';
    for (last = node, n = 0; node[n]; n++) {
        if (node[n] == '/') {
            last = node + n + 1;
        }
    }
    for (n = 0; n < (int)sizeof(loop_name) - 1 && last[n]; n++) {
        loop_name[n] = last[n];
    }
    loop_name[n] = '\0';
}

#ifdef __x86_64__
#define XA_SET 188
#else
#define XA_SET 226
#endif

/** Exercise all twelve attribute calls on ext2 and unsupported file systems.
 * The fixture is private to this case. Each descriptor and scratch name is
 * removed before success; a failed case keeps its disk as evidence. */
static void xattr_rows(void)
{
    const char *path = "/e2/xa-file";
    const char *name = "user.DOSATTRIB";
    char value[64] = {0};
    long fd, target, rc;
    int form;

    fd = syscall3(SYS_OPEN, (long)path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        fail_with("creating the attribute file", (int)fd);
    }
    for (form = 0; form < 3; form++) {
        target = form == 2 ? fd : (long)path;
        expect_ok(syscall5(XA_SET + form, target, (long)name, (long)"ABCD", 4, 1),
                  "setting a user attribute");
        if (syscall4(XA_SET + 3 + form, target, (long)name, 0, 0) != 4 ||
            syscall4(XA_SET + 3 + form, target, (long)name, (long)value, 3) != -34 ||
            syscall4(XA_SET + 3 + form, target, (long)name, (long)value, sizeof(value)) != 4 ||
            value[0] != 'A' || value[3] != 'D')
            fail("attribute query, short buffer or read");
        rc = syscall3(XA_SET + 6 + form, target, (long)value, sizeof(value));
        if (rc != (long)strlen(name) + 1 || !streq(value, name))
            fail("listing user attributes");
        if (syscall5(XA_SET + form, target, (long)name, (long)"X", 1, 1) != -EEXIST ||
            syscall5(XA_SET + form, target, (long)name, (long)"X", 1, 4) != -EINVAL)
            fail("attribute flags");
        expect_ok(syscall2(XA_SET + 9 + form, target, (long)name), "removing an attribute");
        if (syscall4(XA_SET + 3 + form, target, (long)name, (long)value, sizeof(value)) != -61)
            fail("a removed attribute is ENODATA");
    }
    expect_ok(syscall5(XA_SET, (long)path, (long)name, (long)"saved", 5, 0), "saving an attribute");
    syscall1(SYS_CLOSE, fd);
    expect_ok(syscall2(SYS_UMOUNT2, (long)"/e2", 0), "unmounting attribute data");
    expect_ok(syscall5(SYS_MOUNT, (long)node, (long)"/e2", (long)"ext2", 0, 0),
              "remounting attribute data");
    if (syscall4(XA_SET + 3, (long)path, (long)name, (long)value, sizeof(value)) != 5 ||
        value[0] != 's' || value[4] != 'd')
        fail("an attribute survives a remount");
    expect_ok(syscall2(SYS_SYMLINK, (long)"xa-file", (long)"/e2/xa-link"),
              "making an attribute link");
    if (syscall4(XA_SET + 3, (long)"/e2/xa-link", (long)name, (long)value, sizeof(value)) != 5 ||
        syscall4(XA_SET + 4, (long)"/e2/xa-link", (long)name, (long)value, sizeof(value)) != -61 ||
        syscall5(XA_SET + 1, (long)"/e2/xa-link", (long)name, (long)"X", 1, 0) != -EPERM)
        fail("following and not following the final link");
    expect_ok(syscall1(SYS_UNLINK, (long)"/e2/xa-link"), "removing the attribute link");
    expect_ok(syscall1(SYS_UNLINK, (long)path), "removing the attribute file");
    for (form = 0; form < 2; form++) {
        path = form ? "/dev/shm/xa-unsupported" : "/xa-unsupported";
        fd = syscall3(SYS_OPEN, (long)path, O_CREAT | O_RDWR, 0600);
        if (fd < 0) {
            fail_with("creating an unsupported attribute file", (int)fd);
        }
        if (syscall4(XA_SET + 3, (long)path, (long)name, (long)value, sizeof(value)) != -95 ||
            syscall3(XA_SET + 8, fd, (long)value, sizeof(value)) != 0)
            fail("KRFFS and tmpfs keep their unsupported answers");
        syscall1(SYS_CLOSE, fd);
        expect_ok(syscall1(SYS_UNLINK, (long)path), "removing an unsupported attribute file");
    }
}

#define O_DIRECT        040000
#define O_DIRECTORY     0200000
#define O_TMPFILE       (020000000 | O_DIRECTORY)
#define AT_EMPTY_PATH   0x1000
#define F_GETFL         3
#define F_SETFL         4
#define RENAME_EXCHANGE 2

#if defined __x86_64__
#define SYS_PWRITE64_  18
#define SYS_RENAMEAT2_ 316
#else
#define SYS_PWRITE64_  181
#define SYS_RENAMEAT2_ 353
#endif
#define SYS_CACHESTAT_ 451

/* The buffer of the O_DIRECT rows, on a page, so that it and its first
 * sectors are aligned and the byte after its start is not. */
static unsigned char direct_buf[2048] __attribute__((aligned(4096)));

/** pread64 or pwrite64, as `n` says, of `count` bytes at `buf` and the
 * file's offset `off`, which i386 passes in two words: the call's result. */
static long pio(long n, long fd, void *buf, long count, long off)
{
#if defined __x86_64__
    return syscall4(n, fd, (long)buf, count, off);
#else
    return syscall5(n, fd, (long)buf, count, off, 0);
#endif
}

/** The pages of the first `len` bytes of `fd`'s file that cachestat(2)
 * counts in memory, or the call's error. */
static long cached_pages(long fd, long len)
{
    static unsigned long long range[2], cs[5];
    long rc;

    range[0] = 0;
    range[1] = (unsigned long long)len;
    cs[0] = 0;
    rc = syscall4(SYS_CACHESTAT_, fd, (long)range, (long)cs, 0);
    return rc != 0 ? rc : (long)cs[0];
}

/** Fail `what` unless `got` is `want`. */
static void expect_is(long got, long want, const char *what)
{
    if (got != want) {
        fail_with(what, (int)got);
    }
}

/** The O_DIRECT rows on the writable fixture, which leave nothing behind. */
static void direct_io_rows(void)
{
    long fd, flags, i;

    for (i = 0; i < 1024; i++) {
        direct_buf[i] = (unsigned char)(i * 7 + 1);
    }
    fd = syscall3(SYS_OPEN, (long)"/e2/direct", O_CREAT | O_RDWR | O_DIRECT, 0644);
    if (fd < 0) {
        fail_with("opening a file with O_DIRECT", (int)fd);
    }
    flags = syscall2(SYS_FCNTL, fd, F_GETFL);
    if (flags < 0 || !(flags & O_DIRECT)) {
        fail_with("F_GETFL reports O_DIRECT", (int)flags);
    }
    expect_is(pio(SYS_PWRITE64_, fd, direct_buf, 1024, 0), 1024, "an aligned O_DIRECT write");
    expect_is(pio(SYS_PWRITE64_, fd, direct_buf + 1, 512, 0), -EINVAL,
              "an O_DIRECT write from a buffer off the sector is EINVAL");
    expect_is(pio(SYS_PWRITE64_, fd, direct_buf, 100, 0), -EINVAL,
              "an O_DIRECT write of a length off the sector is EINVAL");
    expect_is(pio(SYS_PWRITE64_, fd, direct_buf, 512, 1), -EINVAL,
              "an O_DIRECT write at an offset off the sector is EINVAL");
    expect_is(cached_pages(fd, 1024), 0, "an O_DIRECT write leaves no block in the cache");
    for (i = 1024; i < 2048; i++) {
        direct_buf[i] = 0;
    }
    expect_is(pio(SYS_PREAD64, fd, direct_buf + 1024, 1024, 0), 1024, "an aligned O_DIRECT read");
    for (i = 0; i < 1024; i++) {
        if (direct_buf[i] != direct_buf[1024 + i]) {
            fail_with("an O_DIRECT read gives back the bytes written, wrong at", (int)i);
        }
    }
    expect_is(cached_pages(fd, 1024), 0, "an O_DIRECT read leaves no block in the cache");
    expect_is(pio(SYS_PREAD64, fd, direct_buf + 1024, 100, 4096), 0,
              "an O_DIRECT read off the sector past the end is 0");
    expect_is(pio(SYS_PREAD64, fd, direct_buf + 1024, 100, 0), -EINVAL,
              "an O_DIRECT read off the sector before the end is EINVAL");
    expect_is(syscall3(SYS_FCNTL, fd, F_SETFL, flags & ~O_DIRECT), 0, "F_SETFL clears O_DIRECT");
    expect_is(pio(SYS_PWRITE64_, fd, direct_buf + 1, 100, 3), 100,
              "a write off the sector once O_DIRECT is cleared");
    expect_is(syscall3(SYS_FCNTL, fd, F_SETFL, flags), 0, "F_SETFL sets O_DIRECT");
    if (!(syscall2(SYS_FCNTL, fd, F_GETFL) & O_DIRECT)) {
        fail("F_GETFL reports O_DIRECT set again");
    }
    syscall1(SYS_CLOSE, fd);
    expect_ok(syscall1(SYS_UNLINK, (long)"/e2/direct"), "removing the O_DIRECT file");
    fd = syscall3(SYS_OPEN, (long)"/e2", O_RDONLY | O_DIRECTORY | O_DIRECT, 0);
    if (fd < 0) {
        fail_with("a directory of ext2 opens with O_DIRECT", (int)fd);
    }
    syscall1(SYS_CLOSE, fd);
}

/** The O_TMPFILE and RENAME_EXCHANGE rows on the writable fixture, which
 * leave nothing behind and give back every inode they take. */
static void direct_name_rows(void)
{
    unsigned before = free_inodes();
    long fd, rc;

    fd = syscall3(SYS_OPEN, (long)"/e2", O_TMPFILE | O_RDWR, 0600);
    if (fd < 0) {
        fail_with("opening a file with O_TMPFILE", (int)fd);
    }
    expect_is(syscall3(SYS_WRITE, fd, (long)"tmp\n", 4), 4, "writing the O_TMPFILE file");
    expect_is(syscall2(SYS_FSTAT, fd, (long)st), 0, "fstat of the O_TMPFILE file");
    expect_is(st_word(KSTAT_NLINK), 0, "the O_TMPFILE file has no link");
    expect_is(free_inodes(), before - 1, "the O_TMPFILE file takes an inode");
    expect_is(syscall5(SYS_LINKAT, fd, (long)"", AT_FDCWD, (long)"/e2/tmpnamed", AT_EMPTY_PATH), 0,
              "linkat AT_EMPTY_PATH names the O_TMPFILE file");
    expect_is(stat_nlink("/e2/tmpnamed"), 1, "the named file has one link");
    syscall1(SYS_CLOSE, fd);
    expect_ok(syscall1(SYS_UNLINK, (long)"/e2/tmpnamed"), "removing the named file");
    fd = syscall3(SYS_OPEN, (long)"/e2", O_TMPFILE | O_RDWR, 0600);
    if (fd < 0) {
        fail_with("opening a second file with O_TMPFILE", (int)fd);
    }
    syscall1(SYS_CLOSE, fd);
    expect_is(free_inodes(), before, "the last close frees an O_TMPFILE file's inode");
    fd = syscall3(SYS_OPEN, (long)"/e2/xa", O_CREAT | O_RDWR, 0644);
    syscall1(SYS_CLOSE, fd);
    fd = syscall3(SYS_OPEN, (long)"/e2/xb", O_CREAT | O_RDWR, 0644);
    syscall1(SYS_CLOSE, fd);
    rc = syscall5(SYS_RENAMEAT2_, AT_FDCWD, (long)"/e2/xa", AT_FDCWD, (long)"/e2/xb",
                  RENAME_EXCHANGE);
    expect_is(rc, -EINVAL, "RENAME_EXCHANGE on ext2 is EINVAL");
    expect_ok(syscall1(SYS_UNLINK, (long)"/e2/xa"), "removing the first exchange file");
    expect_ok(syscall1(SYS_UNLINK, (long)"/e2/xb"), "removing the second exchange file");
}

PROBE_SP_ENTRY;

/** The probe's entry, given the initial stack `sp`: the mode from the first
 * positional argument, `links` by default, run on the fixture mounted on
 * /e2, read-only for `links`, with MS_SYNCHRONOUS for `cost`. */
void probe_main(unsigned long *sp)
{
    const char *mode;
    int writable;

    probe_args((int)sp[0], (char **)(sp + 1));
    mode = probe_word(1) != 0 ? probe_word(1) : "links";
    writable = streq(mode, "xattr") || streq(mode, "cost") || streq(mode, "names") ||
               streq(mode, "lookup") || streq(mode, "direct");
    take_loop();
    syscall2(SYS_MKDIR, (long)"/e2", 0755);
    if (syscall5(SYS_MOUNT, (long)node, (long)"/e2", (long)"ext2",
                 streq(mode, "cost") ? MS_SYNCHRONOUS
                 : writable          ? 0
                                     : MS_RDONLY,
                 0) != 0) {
        fail("mounting the fixture");
    }
    if (streq(mode, "xattr")) {
        xattr_rows();
    } else if (streq(mode, "links")) {
        links_rows();
    } else if (streq(mode, "cost")) {
        cost_rows();
    } else if (streq(mode, "direct")) {
        direct_io_rows();
        direct_name_rows();
    } else if (streq(mode, "names")) {
        names_rows();
    } else if (streq(mode, "lookup")) {
        lookup_fill();
        lookup_rows();
        lookup_clean();
    } else {
        fail("unknown mode");
    }
    if (syscall2(SYS_UMOUNT2, (long)"/e2", 0) != 0) {
        fail("unmount");
    }
    probe_line("EXT2PROBE OK (", mode, ")");
    sys_exit(0);
}
