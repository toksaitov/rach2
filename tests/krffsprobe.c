/**
 * krffsprobe: a mounted KRFFS volume, from a program.
 *
 * `krffsprobe read` mounts /dev/disk0p1, the volume
 * tests/system/suites/krffsmount.sh builds, on /k read-only and asks it what
 * a mounted volume owes: every byte of a file whose contents follow a rule,
 * the names in the root and in a subdirectory, a stat of each kind, a read
 * past the end, a name that is not there, and a write refused because the
 * mount is read-only. Then it unmounts.
 *
 * `krffsprobe refuse` mounts the same device expecting the kernel to refuse
 * it: the suite gives it a volume broken on purpose, and the row is the
 * errno plus the kernel's own line on the console.
 *
 * Prints "KRFFSPROBE OK (mode)" or "KRFFSPROBE FAIL: reason". Freestanding;
 * int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT       1
#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_MOUNT      21
#define SYS_MKDIR      39
#define SYS_UMOUNT2    52
#define SYS_STAT64     195
#define SYS_GETDENTS64 220

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100

#define MS_RDONLY 1

#define ENOENT 2
#define EINVAL 22
#define EROFS  30

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
    sys3(SYS_WRITE, 1, (int)"KRFFSPROBE FAIL: ", 17);
    puts(why);
    sys_exit(1);
}

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
    sys3(SYS_WRITE, 1, (int)"KRFFSPROBE FAIL: ", 17);
    sys3(SYS_WRITE, 1, (int)why, (int)strlen(why));
    sys3(SYS_WRITE, 1, (int)" (got ", 6);
    sys3(SYS_WRITE, 1, (int)(digits + i), (int)strlen(digits + i));
    puts(")");
    sys_exit(1);
}

static int streq(const char *a, const char *b)
{
    size_t i;

    for (i = 0; a[i] && a[i] == b[i]; i++) {
    }
    return a[i] == b[i];
}

/* What the suite wrote into /pattern.bin: the byte at `i` is a rule, so a
   volume that reads back wrong says where. */
static unsigned char pattern_byte(unsigned i)
{
    return (unsigned char)((i * 31u + 7u) & 0xFFu);
}

#define PATTERN_BYTES 65536u

/* stat64's fields, at the offsets the kernel writes them. */
#define ST_MODE_OFF 16
#define ST_SIZE_OFF 44

static unsigned stat_mode(const char *path, unsigned char *st)
{
    if (sys2(SYS_STAT64, (int)path, (int)st) != 0) {
        fail("stat");
    }
    return *(unsigned *)(st + ST_MODE_OFF);
}

/**
 * `krffs` is among the types `/proc/filesystems` names.
 *
 * A type mount(2) accepts and does not list is one nobody can find without
 * reading the kernel.
 */
static void listed_rows(void)
{
    char buf[512];
    int fd = sys3(SYS_OPEN, (int)"/proc/filesystems", O_RDONLY, 0);
    int n;
    int i;

    if (fd < 0) {
        fail_with("opening /proc/filesystems", fd);
    }
    n = sys3(SYS_READ, fd, (int)buf, (int)sizeof(buf) - 1);
    sys1(SYS_CLOSE, fd);
    if (n <= 0) {
        fail_with("reading /proc/filesystems", n);
    }
    buf[n] = '\0';
    for (i = 0; i + 5 <= n; i++) {
        if (buf[i] == 'k' && buf[i + 1] == 'r' && buf[i + 2] == 'f' && buf[i + 3] == 'f' &&
            buf[i + 4] == 's') {
            return;
        }
    }
    fail("/proc/filesystems does not name krffs");
}

static void read_rows(void)
{
    unsigned char st[144];
    char buf[4096];
    unsigned at = 0;
    unsigned mode;
    int fd;
    int n;

    fd = sys3(SYS_OPEN, (int)"/k/pattern.bin", O_RDONLY, 0);
    if (fd < 0) {
        fail_with("opening /k/pattern.bin", fd);
    }
    while (at < PATTERN_BYTES) {
        int i;

        n = sys3(SYS_READ, fd, (int)buf, (int)sizeof(buf));
        if (n <= 0) {
            fail_with("reading /k/pattern.bin", n);
        }
        for (i = 0; i < n; i++) {
            if ((unsigned char)buf[i] != pattern_byte(at + (unsigned)i)) {
                fail_with("a byte of /k/pattern.bin", (int)(at + (unsigned)i));
            }
        }
        at += (unsigned)n;
    }
    if (at != PATTERN_BYTES) {
        fail_with("the length of /k/pattern.bin", (int)at);
    }
    n = sys3(SYS_READ, fd, (int)buf, (int)sizeof(buf));
    if (n != 0) {
        fail_with("a read past the end", n);
    }
    sys1(SYS_CLOSE, fd);

    fd = sys3(SYS_OPEN, (int)"/k/sub/inner.txt", O_RDONLY, 0);
    if (fd < 0) {
        fail_with("opening a file in a subdirectory", fd);
    }
    n = sys3(SYS_READ, fd, (int)buf, (int)sizeof(buf));
    if (n != 6 || buf[0] != 'i' || buf[5] != '\n') {
        fail_with("the bytes of /k/sub/inner.txt", n);
    }
    sys1(SYS_CLOSE, fd);

    if (sys3(SYS_OPEN, (int)"/k/nothing", O_RDONLY, 0) != -ENOENT) {
        fail("a name that is not there");
    }

    mode = stat_mode("/k", st);
    if ((mode & 0170000u) != 0040000u) {
        fail_with("the root of the mount is not a directory", (int)mode);
    }
    mode = stat_mode("/k/sub", st);
    if ((mode & 0170000u) != 0040000u) {
        fail_with("/k/sub is not a directory", (int)mode);
    }
    mode = stat_mode("/k/pattern.bin", st);
    if ((mode & 0170000u) != 0100000u) {
        fail_with("/k/pattern.bin is not a regular file", (int)mode);
    }
    if (*(unsigned *)(st + ST_SIZE_OFF) != PATTERN_BYTES) {
        fail_with("the size stat reports", (int)*(unsigned *)(st + ST_SIZE_OFF));
    }

    /* The names, through getdents. */
    {
        int saw_pattern = 0;
        int saw_sub = 0;
        int off = 0;

        fd = sys3(SYS_OPEN, (int)"/k", O_RDONLY, 0);
        if (fd < 0) {
            fail_with("opening the mount as a directory", fd);
        }
        n = sys3(SYS_GETDENTS64, fd, (int)buf, (int)sizeof(buf));
        if (n <= 0) {
            fail_with("getdents on the mount", n);
        }
        while (off + 19 < n) {
            unsigned short reclen = *(unsigned short *)(buf + off + 16);
            const char *name = buf + off + 19;

            if (streq(name, "pattern.bin")) {
                saw_pattern = 1;
            }
            if (streq(name, "sub")) {
                saw_sub = 1;
            }
            if (reclen == 0) {
                break;
            }
            off += reclen;
        }
        sys1(SYS_CLOSE, fd);
        if (!saw_pattern || !saw_sub) {
            fail("the listing of the mount");
        }
    }

    (void)0;
}

/**
 * `/proc/mounts` calls the volume read-only.
 *
 * A type with no write among its operations refuses a write whatever the
 * flags say, so this line is the only place a caller sees what it got. The
 * row that matters is the mount that did not ask for read-only: the kernel
 * gives it anyway, and says so here.
 */
static void mounts_say_ro(void)
{
    {
        char mounts[1024];
        int fd = sys3(SYS_OPEN, (int)"/proc/mounts", O_RDONLY, 0);
        int n;
        int i;
        int saw = 0;

        if (fd < 0) {
            fail_with("opening /proc/mounts", fd);
        }
        n = sys3(SYS_READ, fd, (int)mounts, (int)sizeof(mounts) - 1);
        sys1(SYS_CLOSE, fd);
        if (n <= 0) {
            fail_with("reading /proc/mounts", n);
        }
        mounts[n] = '\0';
        for (i = 0; i + 11 <= n; i++) {
            if (mounts[i] == 'k' && mounts[i + 1] == 'r' && mounts[i + 2] == 'f' &&
                mounts[i + 3] == 'f' && mounts[i + 4] == 's' && mounts[i + 5] == ' ' &&
                mounts[i + 6] == 'r' && mounts[i + 7] == 'o') {
                saw = 1;
            }
        }
        if (!saw) {
            fail("/proc/mounts does not call the volume read-only");
        }
    }
}

/* A read-only mount refuses every way of changing it. */
static void refusal_rows(void)
{
    int rc = sys3(SYS_OPEN, (int)"/k/pattern.bin", O_WRONLY, 0);

    if (rc != -EROFS) {
        fail_with("opening a file for writing on a read-only mount", rc);
    }
    rc = sys3(SYS_OPEN, (int)"/k/made", O_WRONLY | O_CREAT, 0644);
    if (rc != -EROFS) {
        fail_with("creating a file on a read-only mount", rc);
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
    const char *mode = argc > 1 ? argv[1] : "";
    int rc;

    sys2(SYS_MKDIR, (int)"/k", 0755);

    if (streq(mode, "refuse")) {
        rc = sys5(SYS_MOUNT, (int)"/dev/disk0p1", (int)"/k", (int)"krffs", MS_RDONLY, 0);
        if (rc == 0) {
            fail("the kernel mounted a volume it should have refused");
        }
        if (rc != -EINVAL) {
            fail_with("the errno of a refused mount", rc);
        }
        puts("KRFFSPROBE OK (refuse)");
        sys_exit(0);
    }

    if (!streq(mode, "read")) {
        fail("unknown mode");
    }

    listed_rows();
    rc = sys5(SYS_MOUNT, (int)"/dev/disk0p1", (int)"/k", (int)"krffs", MS_RDONLY, 0);
    if (rc != 0) {
        fail_with("mounting the volume", rc);
    }
    read_rows();
    mounts_say_ro();
    refusal_rows();
    rc = sys2(SYS_UMOUNT2, (int)"/k", 0);
    if (rc != 0) {
        fail_with("unmounting the volume", rc);
    }

    /* A mount that did not ask for read-only gets it, and is told. */
    rc = sys5(SYS_MOUNT, (int)"/dev/disk0p1", (int)"/k", (int)"krffs", 0, 0);
    if (rc != 0) {
        fail_with("mounting the volume without MS_RDONLY", rc);
    }
    mounts_say_ro();
    refusal_rows();
    rc = sys2(SYS_UMOUNT2, (int)"/k", 0);
    if (rc != 0) {
        fail_with("unmounting the second time", rc);
    }
    if (sys3(SYS_OPEN, (int)"/k/pattern.bin", O_RDONLY, 0) >= 0) {
        fail("a file of the volume is still there after the unmount");
    }
    puts("KRFFSPROBE OK (read)");
    sys_exit(0);
}
