/*
 * ptyprobe.c - Phase 19 probe: pseudo-terminals (/dev/ptmx + /dev/pts/N).
 *
 * Proves, from user space via raw int 0x80, the master/slave pair the way a
 * network login uses it:
 *   - open("/dev/ptmx") returns a master; TIOCGPTN reports the slave index N
 *     (exact request 0x80045430); TIOCSPTLCK(0) unlocks the pair;
 *   - open("/dev/pts/N") returns the slave; a forked child setsid()s and
 *     TIOCSCTTYs it as its controlling terminal;
 *   - master write -> the slave reads the COOKED line, and the echo comes back
 *     on the master; slave write -> the master reads the (ONLCR) output.
 *
 * The child verifies the cooked read and reports via exit status; the parent
 * verifies the echo + output it reads back on the master. Prints "PTYPROBE OK" /
 * "PTYPROBE FAIL". Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT    1
#define SYS_FORK    2
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_WAITPID 7
#define SYS_IOCTL   54
#define SYS_SETSID  66

#define O_RDWR 2

#define TIOCSCTTY  0x540E
#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431

#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)

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
static int sys_fork(void)
{
    return syscall1(SYS_FORK, 0);
}
static int sys_setsid(void)
{
    return syscall1(SYS_SETSID, 0);
}
static int sys_open(const char *p, int flags)
{
    return syscall3(SYS_OPEN, (int)(size_t)p, flags, 0);
}
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, fd);
}
static int sys_read(int fd, void *b, int n)
{
    return syscall3(SYS_READ, fd, (int)(size_t)b, n);
}
static int sys_write(int fd, const void *b, int n)
{
    return syscall3(SYS_WRITE, fd, (int)(size_t)b, n);
}
static int sys_ioctl(int fd, int req, void *arg)
{
    return syscall3(SYS_IOCTL, fd, req, (int)(size_t)arg);
}
static int sys_waitpid(int pid, int *st, int o)
{
    return syscall3(SYS_WAITPID, pid, (int)(size_t)st, o);
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
    sys_write(1, s, (int)strlen(s));
}

/* Substring search: 1 if needle occurs in haystack[0..len). */
static int contains(const char *hay, int len, const char *needle)
{
    int nl = (int)strlen(needle);
    int i, j;

    for (i = 0; i + nl <= len; i++) {
        for (j = 0; j < nl && hay[i + j] == needle[j]; j++) {
            ;
        }
        if (j == nl) {
            return 1;
        }
    }
    return 0;
}

void _start(void)
{
    int master, slave, ptn, unlock, pid, status;
    char path[16];
    char mbuf[64];
    int n, i, p;

    puts("PTYPROBE START\n");

    master = sys_open("/dev/ptmx", O_RDWR);
    if (master < 0) {
        puts("PTYPROBE FAIL (open ptmx)\n");
        sys_exit(1);
    }

    ptn = -1;
    if (sys_ioctl(master, TIOCGPTN, &ptn) < 0 || ptn < 0) {
        puts("PTYPROBE FAIL (TIOCGPTN)\n");
        sys_exit(1);
    }

    unlock = 0;
    if (sys_ioctl(master, TIOCSPTLCK, &unlock) < 0) {
        puts("PTYPROBE FAIL (TIOCSPTLCK)\n");
        sys_exit(1);
    }

    /* Build "/dev/pts/<ptn>" (ptn is a small non-negative index). */
    p = 0;
    path[p++] = '/';
    path[p++] = 'd';
    path[p++] = 'e';
    path[p++] = 'v';
    path[p++] = '/';
    path[p++] = 'p';
    path[p++] = 't';
    path[p++] = 's';
    path[p++] = '/';
    if (ptn >= 10) {
        path[p++] = (char)('0' + (ptn / 10));
    }
    path[p++] = (char)('0' + (ptn % 10));
    path[p] = '\0';

    pid = sys_fork();
    if (pid < 0) {
        puts("PTYPROBE FAIL (fork)\n");
        sys_exit(1);
    }
    if (pid == 0) {
        /* Child: a new session leader makes the slave its controlling terminal,
         * then reads the cooked line the parent sends on the master. */
        char sbuf[32];
        int sn;

        sys_setsid();
        slave = sys_open(path, O_RDWR);
        if (slave < 0) {
            sys_exit(11); /* open slave failed */
        }
        if (sys_ioctl(slave, TIOCSCTTY, 0) < 0) {
            sys_exit(12); /* TIOCSCTTY failed */
        }
        sn = sys_read(slave, sbuf, (int)sizeof(sbuf));
        if (sn < 5 || !contains(sbuf, sn, "hello")) {
            sys_exit(13); /* did not read the cooked line */
        }
        sys_write(slave, "world\n", 6);
        sys_close(slave);
        sys_exit(0);
    }

    /* Parent: drive the master. Writing the line cooks it into the slave (and
     * echoes it back to us); the child then writes "world" on the slave, which we
     * read back here. */
    sys_write(master, "hello\n", 6);
    sys_waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        puts("PTYPROBE FAIL (child)\n");
        sys_exit(1);
    }

    /* Drain everything the master accumulated: the echo of "hello" plus the
     * slave's "world" output. */
    n = 0;
    for (i = 0; i < 8 && n < (int)sizeof(mbuf) - 1; i++) {
        int r = sys_read(master, mbuf + n, (int)sizeof(mbuf) - 1 - n);
        if (r <= 0) {
            break;
        }
        n += r;
        if (contains(mbuf, n, "hello") && contains(mbuf, n, "world")) {
            break;
        }
    }
    if (!contains(mbuf, n, "hello") || !contains(mbuf, n, "world")) {
        puts("PTYPROBE FAIL (master round-trip)\n");
        sys_exit(1);
    }

    sys_close(master);
    puts("PTYPROBE OK\n");
    sys_exit(0);
}
