/**
 * Exercises the synthetic /dev character devices and the /proc self views.
 *
 * From user space, proves that /dev/zero reads back all-zero bytes; /dev/null
 * reads as immediate EOF and sinks writes; /dev/full fails a write with
 * -ENOSPC; and /dev/urandom yields a varying stream. It then reads
 * /proc/self/stat for our pid and comm and resolves /proc/self/fd/0 to the
 * console. Prints "DEVPROBE OK" when every check passes.
 *
 * Freestanding; direct int 0x80.
 *
 * Needs: the console as stdin. One row reads /proc/self/fd/0 and requires it
 * to name /dev/console, which is true of a console session and not of one
 * carried over SSH.
 */

typedef unsigned int size_t;

#define SYS_EXIT     1
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_GETPID   20
#define SYS_READLINK 85
#define SYS_IOCTL    54

#define O_RDONLY 0
#define O_WRONLY 1

#define ENOSPC 28

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

void _start(void)
{
    char buf[64];
    char buf2[64];
    int fd, n, i, allzero, differ;

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

    if (fails == 0) {
        puts("DEVPROBE OK");
    } else {
        puts("DEVPROBE FAIL");
    }
    sys_exit(fails);
}
