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
 * Prints "EXT2PROBE OK (mode)" or "EXT2PROBE FAIL: reason". Freestanding;
 * int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT     1
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_LINK     9
#define SYS_MOUNT    21
#define SYS_MKDIR    39
#define SYS_UMOUNT2  52
#define SYS_SYMLINK  83
#define SYS_READLINK 85
#define SYS_STAT64   195
#define SYS_LSTAT64  196

#define O_RDONLY   0
#define O_WRONLY   1
#define O_CREAT    0100
#define O_EXCL     0200
#define O_NOFOLLOW 0400000

#define MS_RDONLY 1

#define EPERM  1
#define ENOENT 2
#define EEXIST 17
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
    /* O_NOFOLLOW asks for the link, which open cannot open. */
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
    /* Hard links: the count, and a directory refused. */
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

    sys2(SYS_MKDIR, (int)"/e2", 0755);
    if (sys5(SYS_MOUNT, (int)"/dev/disk0p1", (int)"/e2", (int)"ext2", MS_RDONLY, 0) != 0) {
        fail("mounting the fixture read-only");
    }
    if (streq(mode, "links")) {
        links_rows();
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
