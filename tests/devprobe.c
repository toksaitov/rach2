/**
 * Exercises the synthetic /dev character devices and the /proc self views.
 *
 * From user space, proves that /dev/zero reads back all-zero bytes; /dev/null
 * reads as immediate EOF and sinks writes; /dev/full fails a write with
 * -ENOSPC; and /dev/urandom yields a varying stream. A disk's `BLKRRPART`
 * asked of /dev/null answers ENOTTY, and fstat gives /dev/null the inode the
 * /dev listing does. It then reads /proc/self/stat for our pid and comm and
 * resolves /proc/self/fd/0 to the console. Prints "DEVPROBE OK" when every
 * check passes.
 *
 * Freestanding; direct int 0x80.
 *
 * Needs: the console as stdin. One row reads /proc/self/fd/0 and requires it
 * to name /dev/console, which is true of a console session and not of one
 * carried over SSH.
 */

typedef unsigned int size_t;

#define SYS_EXIT       1
#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_GETPID     20
#define SYS_READLINK   85
#define SYS_IOCTL      54
#define SYS_FORK       2
#define SYS_WAITPID    7
#define SYS_PIPE       42
#define SYS_FSTAT64    197
#define SYS_GETDENTS64 220

#define O_RDONLY    0
#define O_WRONLY    1
#define O_DIRECTORY 0200000

/* i386 struct stat64: st_ino is the 64-bit field at offset 88. A
   linux_dirent64 record has its 64-bit d_ino first, its length at 16 and its
   name at 19. */
#define ST_INO64 88
#define D_RECLEN 16
#define D_NAME   19

#define ENOSPC        28
#define EBUSY         16
#define FB_MODE_13H   0x4601
#define FB_MODE_TEXT  0x4602
#define FB_SETPALETTE 0x4603

static int syscall1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
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
    syscall3(SYS_WRITE, 1, (int)(size_t)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (int)(size_t)"\n", 1);
}

static int fails = 0;

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, 1, (int)(size_t)"  FAIL: ", 8);
        puts(msg);
    }
}

static int do_open(const char *path, int flags)
{
    return syscall3(SYS_OPEN, (int)(size_t)path, flags, 0);
}

static int do_read(int fd, char *buf, int n)
{
    return syscall3(SYS_READ, fd, (int)(size_t)buf, n);
}

static int do_readlink(const char *path, char *buf, int n)
{
    return syscall3(SYS_READLINK, (int)(size_t)path, (int)(size_t)buf, n);
}

/** Parse the leading decimal of s. */
static int atoin(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

/** Does haystack contain needle? */
static int contains(const char *h, const char *n)
{
    int i, j;
    for (i = 0; h[i]; i++) {
        for (j = 0; n[j] && h[i + j] == n[j]; j++) {
        }
        if (!n[j]) {
            return 1;
        }
    }
    return 0;
}

static int streq_n(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * The low word of the inode the /dev listing gives `name`, or 0 when the
 * listing has no such leaf or gives it a number wider than 32 bits.
 */
static unsigned int listed_ino(const char *name)
{
    static char dbuf[2048];
    unsigned int ino = 0;
    int fd, n, pos, reclen;
    size_t len = strlen(name) + 1;

    fd = do_open("/dev", O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return 0;
    }
    while ((n = syscall3(SYS_GETDENTS64, fd, (int)(size_t)dbuf, (int)sizeof(dbuf))) > 0) {
        for (pos = 0; pos < n; pos += reclen) {
            reclen = *(unsigned short *)(dbuf + pos + D_RECLEN);
            if (reclen <= 0) {
                break;
            }
            if (streq_n(dbuf + pos + D_NAME, name, (int)len) &&
                *(unsigned int *)(dbuf + pos + 4) == 0u) {
                ino = *(unsigned int *)(dbuf + pos);
            }
        }
    }
    syscall1(SYS_CLOSE, fd);
    return ino;
}

/** Check mode reentry and a descriptor retained across two display owners. */
static void framebuffer(void)
{
    int ready[2], next[2];
    int fd, child, other, status = 0;
    char one = 1;
    static char palette[768];

    check(syscall1(SYS_PIPE, (int)(size_t)ready) == 0, "framebuffer ready pipe");
    check(syscall1(SYS_PIPE, (int)(size_t)next) == 0, "framebuffer command pipe");
    fd = do_open("/fb", O_WRONLY);
    check(fd >= 0, "open framebuffer");
    check(syscall3(SYS_IOCTL, fd, FB_MODE_TEXT, 0) == 0, "framebuffer text mode");
    check(syscall3(SYS_IOCTL, fd, FB_MODE_13H, 0) == 0, "framebuffer graphics reentry");
    check(syscall3(SYS_WRITE, fd, (int)(size_t)&one, 1) == 1,
          "graphics reentry restores writable display");
    child = syscall1(SYS_FORK, 0);
    check(child >= 0, "framebuffer fork");
    if (child < 0) {
        syscall1(SYS_CLOSE, fd);
        return;
    }
    if (child == 0) {
        syscall1(SYS_CLOSE, ready[0]);
        syscall1(SYS_CLOSE, next[1]);
        check(do_open("/fb", O_WRONLY) == -EBUSY, "second opener cannot take owned display");
        check(syscall3(SYS_IOCTL, fd, FB_MODE_TEXT, 0) == -EBUSY,
              "inherited descriptor cannot release parent's display");
        syscall3(SYS_WRITE, ready[1], (int)(size_t)&one, 1);
        check(do_read(next[0], &one, 1) == 1, "wait for display release");
        other = do_open("/fb", O_WRONLY);
        check(other >= 0, "second opener acquires released display");
        syscall3(SYS_WRITE, ready[1], (int)(size_t)&one, 1);
        check(do_read(next[0], &one, 1) == 1, "wait for stale requests");
        check(syscall3(SYS_WRITE, other, (int)(size_t)&one, 1) == 1,
              "stale requests preserve owner's graphics mode");
        syscall1(SYS_CLOSE, other);
        syscall1(SYS_CLOSE, fd);
        sys_exit(fails != 0);
    }
    syscall1(SYS_CLOSE, ready[1]);
    syscall1(SYS_CLOSE, next[0]);
    check(do_read(ready[0], &one, 1) == 1, "wait for inherited descriptor check");
    check(syscall3(SYS_WRITE, fd, (int)(size_t)&one, 1) == 1,
          "second opener preserves first owner's graphics mode");
    check(syscall3(SYS_IOCTL, fd, FB_MODE_TEXT, 0) == 0, "first owner releases display");
    syscall3(SYS_WRITE, next[1], (int)(size_t)&one, 1);
    check(do_read(ready[0], &one, 1) == 1, "wait for second owner");
    check(syscall3(SYS_IOCTL, fd, FB_MODE_TEXT, 0) == -EBUSY,
          "stale descriptor cannot release second owner's display");
    check(syscall3(SYS_IOCTL, fd, FB_MODE_13H, 0) == -EBUSY,
          "stale descriptor cannot acquire second owner's display");
    check(syscall3(SYS_IOCTL, fd, FB_SETPALETTE, (int)(size_t)palette) == -EBUSY,
          "stale descriptor cannot set second owner's palette");
    check(do_open("/fb", O_WRONLY) == -EBUSY, "second owner retains display");
    syscall3(SYS_WRITE, next[1], (int)(size_t)&one, 1);
    check(syscall3(SYS_WAITPID, child, (int)(size_t)&status, 0) == child && status == 0,
          "framebuffer child checks pass");
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_CLOSE, ready[0]);
    syscall1(SYS_CLOSE, next[1]);
}

void _start(void)
{
    static char st[96];
    char buf[64];
    char buf2[64];
    int fd, n, i, allzero, differ;
    unsigned int ino;

    /* (a) /dev/zero: a read returns the requested count, all zero. */
    fd = do_open("/dev/zero", O_RDONLY);
    check(fd >= 0, "open /dev/zero");
    for (i = 0; i < 64; i++) {
        buf[i] = (char)0xAB; /* poison so we see real zeros, not stale */
    }
    n = do_read(fd, buf, 64);
    check(n == 64, "/dev/zero read returns the full count");
    allzero = 1;
    for (i = 0; i < 64; i++) {
        if (buf[i] != 0) {
            allzero = 0;
        }
    }
    check(allzero, "/dev/zero yields zero bytes");
    syscall1(SYS_CLOSE, fd);

    /* (b) /dev/null: read is immediate EOF (0); write is accepted (sink). */
    fd = do_open("/dev/null", O_RDONLY);
    check(fd >= 0, "open /dev/null");
    check(do_read(fd, buf, 64) == 0, "/dev/null read is EOF (0)");
    syscall1(SYS_CLOSE, fd);

    /* A disk's own ioctl asked of a device that is not a disk: the request
       reaches every device, and a record that is not a disk node read as one
       reads past the table it is in. `BLKRRPART` is 0x125F and ENOTTY 25. */
    fd = do_open("/dev/null", O_RDONLY);
    check(fd >= 0, "open /dev/null for the ioctl row");
    check(syscall3(SYS_IOCTL, fd, 0x125F, 0) == -25, "a disk's BLKRRPART on /dev/null is ENOTTY");
    syscall1(SYS_CLOSE, fd);

    /* The row above cannot see a table entry taken for a disk node: the
       misread node's partition number is the major and minor of the entry
       two further on, never zero, so the request is refused either way. The
       inode can. The listing numbers a leaf by its place in the table, and
       fstat by what the descriptor's record is. */
    fd = do_open("/dev/null", O_RDONLY);
    check(fd >= 0, "open /dev/null for its inode");
    check(syscall3(SYS_FSTAT64, fd, (int)(size_t)st, 0) == 0, "fstat /dev/null");
    ino = listed_ino("null");
    check(ino != 0 && *(unsigned int *)(st + ST_INO64) == ino &&
              *(unsigned int *)(st + ST_INO64 + 4) == 0u,
          "fstat and the /dev listing give /dev/null one inode");
    syscall1(SYS_CLOSE, fd);

    fd = do_open("/dev/null", O_WRONLY);
    check(fd >= 0, "open /dev/null O_WRONLY");
    check(syscall3(SYS_WRITE, fd, (int)(size_t)"discard me", 10) == 10,
          "/dev/null write is accepted and discarded");
    syscall1(SYS_CLOSE, fd);

    /* (c) /dev/full: a write fails with -ENOSPC. */
    fd = do_open("/dev/full", O_WRONLY);
    check(fd >= 0, "open /dev/full");
    check(syscall3(SYS_WRITE, fd, (int)(size_t)"x", 1) == -ENOSPC, "/dev/full write -> -ENOSPC");
    syscall1(SYS_CLOSE, fd);

    /* (d) /dev/urandom: two reads differ (the stream is not constant). */
    fd = do_open("/dev/urandom", O_RDONLY);
    check(fd >= 0, "open /dev/urandom");
    check(do_read(fd, buf, 64) == 64 && do_read(fd, buf2, 64) == 64,
          "/dev/urandom reads return data");
    differ = 0;
    for (i = 0; i < 64; i++) {
        if (buf[i] != buf2[i]) {
            differ = 1;
        }
    }
    check(differ, "/dev/urandom varies between reads");
    syscall1(SYS_CLOSE, fd);

    /* (e) /proc/self/stat: field 1 is our pid, comm is our name. */
    {
        int mypid = syscall1(SYS_GETPID, 0);
        fd = do_open("/proc/self/stat", O_RDONLY);
        check(fd >= 0, "open /proc/self/stat");
        n = do_read(fd, buf, 63);
        check(n > 0, "/proc/self/stat is readable");
        if (n > 0) {
            buf[n] = '\0';
        }
        check(atoin(buf) == mypid, "/proc/self/stat field 1 is our pid");
        check(contains(buf, "(devprobe)"), "/proc/self/stat reports our comm");
        syscall1(SYS_CLOSE, fd);
    }

    /* (f) /proc/self/fd/0 is a symlink to the console (what ttyname reads). */
    {
        n = do_readlink("/proc/self/fd/0", buf, 64);
        check(n == 12 && streq_n(buf, "/dev/console", 12), "/proc/self/fd/0 -> /dev/console");
    }

    framebuffer();
    if (fails == 0) {
        puts("DEVPROBE OK");
    } else {
        puts("DEVPROBE FAIL");
    }
    sys_exit(fails);
}
