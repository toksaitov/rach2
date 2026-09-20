/**
 * ext2probe: the ext2 root's contract from a program.
 *
 * `ext2probe links` mounts /dev/disk0p1, the fixture volume
 * tests/system/mkext2fixture.sh writes, on /e2 and runs the symbolic-link
 * rows the walker must answer: a relative and an absolute target, a link
 * to a directory with `..` through it, a chain of forty followed and one
 * of forty-one refused, `O_NOFOLLOW`, `O_CREAT|O_EXCL` on a dangling link,
 * `lstat` against `stat`, `readlink`, and a dangling link; and the
 * hard-link rows a read-only volume can answer: the count in `stat` and
 * `EPERM` for a directory.
 *
 * `ext2probe cost` mounts the same fixture for writing and counts commands
 * and cache flushes for repeated rewrites and appends through `/proc/disk`.
 * Both counts include fsync, and both files are reopened and checked.
 *
 * Prints "EXT2PROBE OK (mode)" or "EXT2PROBE FAIL: reason". Freestanding;
 * int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT      1
#define SYS_READ      3
#define SYS_WRITE     4
#define SYS_OPEN      5
#define SYS_CLOSE     6
#define SYS_LINK      9
#define SYS_LSEEK     19
#define SYS_MOUNT     21
#define SYS_MKDIR     39
#define SYS_UMOUNT2   52
#define SYS_SYMLINK   83
#define SYS_READLINK  85
#define SYS_STAT64    195
#define SYS_LSTAT64   196
#define SYS_STAT      106
#define SYS_LSTAT     107
#define SYS_FSYNC     118
#define SYS_FSTATAT64 300
#define SYS_UTIMENSAT 320

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_SYMLINK_FOLLOW   0x400

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0100
#define O_EXCL     0200
#define O_NOFOLLOW 0400000

#define MS_RDONLY 1

#define EPERM  1
#define ENOENT 2
#define EEXIST 17
#define EXDEV  18
#define EINVAL 22
#define EROFS  30
#define ELOOP  40

static int sys1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int sys2(int n, int a, int b)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static int sys4(int n, int a, int b, int c, int d)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
    return r;
}

static int sys5(int n, int a, int b, int c, int d, int e)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory");
    return r;
}

static void sys_exit(int status) __attribute__((noreturn));
static void sys_exit(int status)
{
    sys1(SYS_EXIT, status);
    __builtin_unreachable();
}

static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static void puts(const char *s)
{
    sys3(SYS_WRITE, 1, (int)s, (int)strlen(s));
    sys3(SYS_WRITE, 1, (int)"\n", 1);
}

static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why)
{
    sys3(SYS_WRITE, 1, (int)"EXT2PROBE FAIL: ", 16);
    puts(why);
    sys_exit(1);
}

/* A failure with the number the row got, for a wrong count or errno. */
static void fail_with(const char *why, int n) __attribute__((noreturn));
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
    sys3(SYS_WRITE, 1, (int)"EXT2PROBE FAIL: ", 16);
    sys3(SYS_WRITE, 1, (int)why, (int)strlen(why));
    sys3(SYS_WRITE, 1, (int)" (got ", 6);
    sys3(SYS_WRITE, 1, (int)(digits + i), (int)strlen(digits + i));
    puts(")");
    sys_exit(1);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The stat64 buffer, in the i386 layout: st_mode at offset 16, st_nlink at
   20. The kernel writes it through the pointer the syscall is given. */
static unsigned char st[104];

/* The mode word of a stat or lstat. */
static int stat_mode(int call, const char *path, int *mode)
{
    int rc = sys2(call, (int)path, (int)st);

    if (rc == 0) {
        *mode = (int)(st[16] | (st[17] << 8) | (st[18] << 16) | (st[19] << 24));
    }
    return rc;
}

/* The link count. */
static int stat_nlink(const char *path)
{
    if (sys2(SYS_STAT64, (int)path, (int)st) != 0) {
        return -1;
    }
    return (int)(st[20] | (st[21] << 8) | (st[22] << 16) | (st[23] << 24));
}

static int read_all(const char *path, char *buf, int cap)
{
    int fd = sys3(SYS_OPEN, (int)path, O_RDONLY, 0);
    int n;

    if (fd < 0) {
        return fd;
    }
    n = sys3(SYS_READ, fd, (int)buf, cap);
    sys1(SYS_CLOSE, fd);
    return n;
}

static void expect_open(const char *path, int flags, int want, const char *what)
{
    int fd = sys3(SYS_OPEN, (int)path, flags, 0644);

    if (want == 0) {
        if (fd < 0) {
            fail(what);
        }
        sys1(SYS_CLOSE, fd);
    } else if (fd != -want) {
        fail(what);
    }
}

/* The mode word of the stat64 buffer. */
static int st_mode(void)
{
    return (int)(st[16] | (st[17] << 8) | (st[18] << 16) | (st[19] << 24));
}

static char big[512];

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
    /* A link to a directory, and `..` through it goes to the target's
       parent, since `..` is physical. */
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
    /* Under O_NOFOLLOW a link at the end is the result, and open cannot
       open a link. */
    expect_open("/e2/fast", O_RDONLY | O_NOFOLLOW, ELOOP, "O_NOFOLLOW on a link answers ELOOP");
    expect_open("/e2/hello.txt", O_RDONLY | O_NOFOLLOW, 0, "O_NOFOLLOW on a file opens it");
    /* O_CREAT|O_EXCL on a dangling link: the link is there. */
    expect_open("/e2/dangling", O_WRONLY | O_CREAT | O_EXCL, EEXIST,
                "O_CREAT|O_EXCL on a dangling link answers EEXIST");
    /* lstat sees the link, stat its target. */
    if (stat_mode(SYS_LSTAT64, "/e2/fast", &mode) != 0 || (mode & 0170000) != 0120000) {
        fail("lstat reports the link");
    }
    if (stat_mode(SYS_STAT64, "/e2/fast", &mode) != 0 || (mode & 0170000) != 0100000) {
        fail("stat reports the target");
    }
    /* readlink gives the bytes without a terminator. */
    n = sys3(SYS_READLINK, (int)"/e2/fast", (int)buf, sizeof(buf));
    if (n != 9 || buf[0] != 'h' || buf[8] != 't') {
        fail("readlink gives the target's bytes");
    }
    n = sys3(SYS_READLINK, (int)"/e2/hello.txt", (int)buf, sizeof(buf));
    if (n != -22) {
        fail("readlink on a file answers EINVAL");
    }
    /* A dangling link: ENOENT through it, the link itself by lstat. */
    if (stat_mode(SYS_STAT64, "/e2/dangling", &mode) != -ENOENT) {
        fail("a dangling link answers ENOENT to stat");
    }
    if (stat_mode(SYS_LSTAT64, "/e2/dangling", &mode) != 0 || (mode & 0170000) != 0120000) {
        fail("and is there for lstat");
    }
    if (read_all("/e2/dangling", buf, sizeof(buf)) != -ENOENT) {
        fail("and to open");
    }
    /* A link in the middle of a path is always followed, even under
       O_NOFOLLOW: `fast` becomes the file, and `..` on a file is refused. */
    n = sys3(SYS_OPEN, (int)"/e2/fast/../hello.txt", O_RDONLY | O_NOFOLLOW, 0);
    if (n != -20) {
        fail_with("a link in the middle of a path is followed to its file", n);
    }
    /* The hard-link rows a read-only volume can answer. */
    if (stat_nlink("/e2/hello.txt") != 2 || stat_nlink("/e2/hello.link") != 2) {
        fail("a hard link's count is two on both names");
    }
    if (sys2(SYS_LINK, (int)"/e2/sub", (int)"/e2/sub2") != -EPERM) {
        fail("a directory cannot be hard-linked");
    }
    if (sys2(SYS_LINK, (int)"/e2/hello.txt", (int)"/e2/another") != -EROFS) {
        fail("a hard link on a read-only mount answers EROFS");
    }
    if (sys2(SYS_SYMLINK, (int)"hello.txt", (int)"/e2/newlink") != -EROFS) {
        fail("a symbolic link on a read-only mount answers EROFS");
    }
    if (sys2(SYS_SYMLINK, (int)"x", (int)"/newlink") != -EPERM) {
        fail("a symbolic link on the KRFFS root answers EPERM");
    }
    if (sys2(SYS_LINK, (int)"/e2/hello.txt", (int)"/another") != -EXDEV) {
        fail("a hard link across mounts answers EXDEV");
    }
    /* O_CREAT through a dangling link is the target's creation, which the
       read-only mount refuses; it must answer and not loop. */
    expect_open("/e2/dangling", O_WRONLY | O_CREAT, EROFS,
                "O_CREAT through a dangling link creates the target, refused read-only");
    expect_open("/e2/fast", O_WRONLY | O_CREAT | O_EXCL, EEXIST,
                "O_CREAT|O_EXCL on a link with a target answers EEXIST");
    /* readlink: a short buffer, an empty one, no terminator, a long target. */
    buf[1] = 'Z';
    n = sys3(SYS_READLINK, (int)"/e2/fast", (int)buf, 1);
    if (n != 1 || buf[0] != 'h' || ((volatile char *)buf)[1] != 'Z') {
        fail_with("readlink into a one-byte buffer gives one byte and no terminator", n);
    }
    n = sys3(SYS_READLINK, (int)"/e2/fast", (int)buf, 0);
    if (n != -EINVAL) {
        fail_with("readlink into an empty buffer answers EINVAL", n);
    }
    n = sys3(SYS_READLINK, (int)"/e2/long300", (int)big, sizeof(big));
    if (n != 300 || big[0] != 'x' || big[299] != 'x') {
        fail_with("readlink gives a 300-byte target", n);
    }
    /* fstatat64 with and without AT_SYMLINK_NOFOLLOW, and an invalid flag. */
    if (sys4(SYS_FSTATAT64, AT_FDCWD, (int)"/e2/fast", (int)st, AT_SYMLINK_NOFOLLOW) != 0 ||
        (st_mode() & 0170000) != 0120000) {
        fail("fstatat64 with AT_SYMLINK_NOFOLLOW reports the link");
    }
    if (sys4(SYS_FSTATAT64, AT_FDCWD, (int)"/e2/fast", (int)st, 0) != 0 ||
        (st_mode() & 0170000) != 0100000) {
        fail("fstatat64 without it reports the target");
    }
    if (sys4(SYS_FSTATAT64, AT_FDCWD, (int)"/e2/fast", (int)st, AT_SYMLINK_FOLLOW) != -EINVAL) {
        fail("fstatat64 with AT_SYMLINK_FOLLOW answers EINVAL");
    }
    if (sys4(SYS_UTIMENSAT, AT_FDCWD, (int)"/e2/fast", 0, AT_SYMLINK_FOLLOW) != -EINVAL) {
        fail("utimensat with AT_SYMLINK_FOLLOW answers EINVAL");
    }
    /* The old stat and lstat, whose mode is a 16-bit word at offset 8. */
    if (sys2(SYS_LSTAT, (int)"/e2/fast", (int)st) != 0 ||
        ((st[8] | (st[9] << 8)) & 0170000) != 0120000) {
        fail("the old lstat reports the link");
    }
    if (sys2(SYS_STAT, (int)"/e2/fast", (int)st) != 0 ||
        ((st[8] | (st[9] << 8)) & 0170000) != 0100000) {
        fail("the old stat reports the target");
    }
    /* The links of /proc answer the same two ways. */
    expect_open("/proc/self", O_RDONLY | O_NOFOLLOW, ELOOP,
                "/proc/self under O_NOFOLLOW answers ELOOP");
    expect_open("/proc/self/fd/1", O_WRONLY | O_CREAT | O_EXCL, EEXIST,
                "/proc/self/fd/1 under O_CREAT|O_EXCL answers EEXIST");
}

struct disk_counts {
    unsigned commands;
    unsigned flushes;
};

static unsigned char cost_block[4096];

/** Read one unsigned counter from a complete procfs line. */
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

/** Opening `/proc/disk` takes a snapshot without issuing a disk command. */
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
    sys3(SYS_WRITE, 1, (int)(digits + i), (int)sizeof(digits) - i);
}

/** Record counts before checking them, including a failing mutation's cost. */
static void cost_report(const char *name, struct disk_counts before, struct disk_counts collected,
                        struct disk_counts committed)
{
    sys3(SYS_WRITE, 1, (int)"ext2-cost: result: ", 19);
    sys3(SYS_WRITE, 1, (int)name, (int)strlen(name));
    sys3(SYS_WRITE, 1, (int)" collected commands=", 20);
    cost_number(collected.commands - before.commands);
    sys3(SYS_WRITE, 1, (int)" flushes=", 9);
    cost_number(collected.flushes - before.flushes);
    sys3(SYS_WRITE, 1, (int)" committed commands=", 20);
    cost_number(committed.commands - before.commands);
    sys3(SYS_WRITE, 1, (int)" flushes=", 9);
    cost_number(committed.flushes - before.flushes);
    puts("");
}

/** Write one whole block with a pattern that identifies its last writer. */
static void cost_write(int fd, unsigned char pattern)
{
    unsigned i;

    for (i = 0; i < sizeof(cost_block); i++) {
        cost_block[i] = pattern;
    }
    if (sys3(SYS_WRITE, fd, (int)cost_block, sizeof(cost_block)) != (int)sizeof(cost_block)) {
        fail("writing a cost block");
    }
}

/** Create and sync one block before either count begins. */
static int cost_create(const char *path, unsigned char pattern)
{
    int fd = sys3(SYS_OPEN, (int)path, O_RDWR | O_CREAT | O_EXCL, 0644);

    if (fd < 0) {
        fail("creating a cost file");
    }
    cost_write(fd, pattern);
    if (sys1(SYS_FSYNC, fd) != 0) {
        fail("syncing a cost fixture");
    }
    return fd;
}

/** Reopen and check every byte and the exact file length after the count. */
static void cost_verify(const char *path, unsigned first, unsigned blocks)
{
    int fd = sys3(SYS_OPEN, (int)path, O_RDONLY, 0);
    unsigned block, i;

    if (fd < 0) {
        fail("reopening a cost file");
    }
    for (block = 0; block < blocks; block++) {
        if (sys3(SYS_READ, fd, (int)cost_block, sizeof(cost_block)) != (int)sizeof(cost_block)) {
            fail("reading a cost block");
        }
        for (i = 0; i < sizeof(cost_block); i++) {
            if (cost_block[i] != first + block) {
                fail("a cost block has the wrong bytes");
            }
        }
    }
    if (sys3(SYS_READ, fd, (int)cost_block, 1) != 0 || sys1(SYS_CLOSE, fd) != 0) {
        fail("a cost file has the wrong length or failed close");
    }
}

/**
 * Count sixteen rewrites and two appends, including their final fsync.
 * The rewrite permits four commands and two flushes, the cost of one data
 * write and one inode write. Two appends together permit at most the ten
 * commands and five flushes of one append in the former per-call path.
 */
static void cost_rows(int append_only)
{
    struct disk_counts before, collected, committed;
    int fd = cost_create("/e2/cost-rewrite", 0);
    unsigned i;

    before = disk_counts();
    for (i = 1; i <= 16; i++) {
        if (sys3(SYS_LSEEK, fd, 0, 0) != 0) {
            fail("seeking to rewrite a cost block");
        }
        cost_write(fd, (unsigned char)i);
    }
    collected = disk_counts();
    if (sys1(SYS_FSYNC, fd) != 0) {
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
    if (sys1(SYS_CLOSE, fd) != 0) {
        fail("closing the rewrite file");
    }
    cost_verify("/e2/cost-rewrite", 16, 1);

    fd = cost_create("/e2/cost-append", 64);
    before = disk_counts();
    cost_write(fd, 65);
    cost_write(fd, 66);
    collected = disk_counts();
    if (sys1(SYS_FSYNC, fd) != 0) {
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
    if (sys1(SYS_CLOSE, fd) != 0) {
        fail("closing the append file");
    }
    cost_verify("/e2/cost-append", 64, 3);
}

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned int *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    const char *mode = argc > 1 ? argv[1] : "links";
    int writable = streq(mode, "cost") || streq(mode, "cost-append");

    sys2(SYS_MKDIR, (int)"/e2", 0755);
    if (sys5(SYS_MOUNT, (int)"/dev/disk0p1", (int)"/e2", (int)"ext2", writable ? 0 : MS_RDONLY,
             0) != 0) {
        fail("mounting the fixture");
    }
    if (streq(mode, "links")) {
        links_rows();
    } else if (writable) {
        cost_rows(streq(mode, "cost-append"));
    } else {
        fail("unknown mode");
    }
    if (sys2(SYS_UMOUNT2, (int)"/e2", 0) != 0) {
        fail("unmount");
    }
    sys3(SYS_WRITE, 1, (int)"EXT2PROBE OK (", 14);
    sys3(SYS_WRITE, 1, (int)mode, (int)strlen(mode));
    puts(")");
    sys_exit(0);
}
