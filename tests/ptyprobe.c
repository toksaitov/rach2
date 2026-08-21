/**
 * Exercises the pseudo-terminal pair (/dev/ptmx + /dev/pts/N) from user space,
 * the way a network login drives it.
 *
 * * open("/dev/ptmx") returns a master; TIOCGPTN reports the slave index N
 *   (exact request 0x80045430); TIOCSPTLCK(0) unlocks the pair.
 * * The master sets the window size (TIOCSWINSZ), which is what a login server
 *   does with the size the client asked for, before the shell is started.
 * * open("/dev/pts/N") returns the slave; a forked child setsid()s and
 *   TIOCSCTTYs it as its controlling terminal.
 * * On the slave: TIOCGWINSZ reports the size the master set, and TCGETS and
 *   TCSETS read and write the termios. These reach the shared terminal handler
 *   rather than the slave's own ops, and `stty` over a network login is
 *   nothing but these three.
 * * master write -> the slave reads the cooked line, and the echo comes back
 *   on the master; slave write -> the master reads the ONLCR output.
 *
 * The child verifies the ioctls and the cooked read and reports via exit
 * status; the parent verifies the echo and output it reads back on the master.
 * The child writes a readiness line before the parent writes anything, so the
 * echo the parent checks is never settled while the child has ECHO off, and
 * every wait here is bounded (a non-blocking master, a reaped-within-a-limit
 * child) so a stalled half fails the test rather than hanging it.
 * Prints "PTYPROBE OK" or "PTYPROBE FAIL". Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT      1
#define SYS_FORK      2
#define SYS_READ      3
#define SYS_WRITE     4
#define SYS_OPEN      5
#define SYS_CLOSE     6
#define SYS_WAITPID   7
#define SYS_IOCTL     54
#define SYS_SETSID    66
#define SYS_NANOSLEEP 162

#define O_RDWR 2

#define TIOCSCTTY  0x540E
#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431
#define FIONBIO    0x5421
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

#define ECHO 0x0008 /* c_lflag bit, the one a password prompt clears */

/* The kernel termios (sys/tty/n_tty.h): four flag words, a line discipline
   byte, and nineteen control characters. */
#define TIO_LFLAG 12
#define TIO_SIZE  36

#define WS_ROWS 40 /* the size the master sets, as a login server does */
#define WS_COLS 100

#define WNOHANG 1

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

/** Sleep for `ms` milliseconds. */
static void nap_ms(int ms)
{
    int ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    syscall3(SYS_NANOSLEEP, (int)(size_t)ts, 0, 0);
}

/**
 * Wait up to `ms` milliseconds for `pid` to exit, without blocking.
 *
 * Returns 1 with `*status` filled if it was reaped, else 0. A blocking wait
 * here would turn a child stuck before its own exit into a test that hangs
 * instead of one that fails.
 */
static int reap_within(int pid, int *status, int ms)
{
    int waited = 0;

    while (waited < ms) {
        if (sys_waitpid(pid, status, WNOHANG) == pid) {
            return 1;
        }
        nap_ms(20);
        waited += 20;
    }
    return 0;
}

/** Substring search: 1 if `needle` occurs in `hay[0..len)`, else 0. */
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
    int master, slave, ptn, unlock, pid, status, nb;
    char path[16];
    char mbuf[128];
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

    {
        short ws[4];
        ws[0] = WS_ROWS;
        ws[1] = WS_COLS;
        ws[2] = 0;
        ws[3] = 0;
        if (sys_ioctl(master, TIOCSWINSZ, ws) < 0) {
            puts("PTYPROBE FAIL (master TIOCSWINSZ)\n");
            sys_exit(1);
        }
    }

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
        {
            unsigned char tio[TIO_SIZE], back[TIO_SIZE];
            unsigned int lflag;
            short ws[4];
            int b;

            ws[0] = 0;
            ws[1] = 0;
            if (sys_ioctl(slave, TIOCGWINSZ, ws) < 0) {
                sys_exit(14); /* the slave has no TIOCGWINSZ */
            }
            if (ws[0] != WS_ROWS || ws[1] != WS_COLS) {
                sys_exit(15); /* the size the master set did not reach it */
            }
            for (b = 0; b < TIO_SIZE; b++) {
                tio[b] = 0;
            }
            if (sys_ioctl(slave, TCGETS, tio) < 0) {
                sys_exit(16); /* the slave has no TCGETS */
            }
            lflag = *(unsigned int *)(tio + TIO_LFLAG);
            if (!(lflag & ECHO)) {
                sys_exit(17); /* a fresh slave echoes */
            }
            *(unsigned int *)(tio + TIO_LFLAG) = lflag & ~(unsigned int)ECHO;
            if (sys_ioctl(slave, TCSETS, tio) < 0) {
                sys_exit(18); /* the slave has no TCSETS */
            }
            if (sys_ioctl(slave, TCGETS, back) < 0 ||
                (*(unsigned int *)(back + TIO_LFLAG) & ECHO)) {
                sys_exit(19); /* the write did not take */
            }
            *(unsigned int *)(tio + TIO_LFLAG) = lflag;
            if (sys_ioctl(slave, TCSETS, tio) < 0) {
                sys_exit(20); /* the echo must return for the round trip */
            }
        }
        /* The line the parent waits for before it writes. */
        if (sys_write(slave, "ready\n", 6) != 6) {
            sys_exit(21); /* the readiness line was not written whole */
        }
        sn = sys_read(slave, sbuf, (int)sizeof(sbuf));
        if (sn < 5 || !contains(sbuf, sn, "hello")) {
            sys_exit(13); /* did not read the cooked line */
        }
        if (sys_write(slave, "world\n", 6) != 6) {
            sys_exit(22);
        }
        sys_close(slave);
        sys_exit(0);
    }

    /* Parent: drive the master. Writing the line cooks it into the slave (and
     * echoes it back to us); the child then writes "world" on the slave, which
     * we read back here. The master is put in non-blocking mode first: every
     * wait below is then a bounded number of naps, and a child that stalls
     * before writing fails this test instead of hanging it. */
    nb = 1;
    if (sys_ioctl(master, FIONBIO, &nb) < 0) {
        puts("PTYPROBE FAIL (the master would not go non-blocking)\n");
        sys_exit(1);
    }
    n = 0;
    for (i = 0; i < 100 && n < (int)sizeof(mbuf) - 1; i++) {
        int r = sys_read(master, mbuf + n, (int)sizeof(mbuf) - 1 - n);
        if (r > 0) {
            n += r;
            if (contains(mbuf, n, "ready")) {
                break;
            }
            continue;
        }
        /* Nothing yet: no slave open, nothing written, or a child that has
         * died. Wait a little and look again, within the bound. */
        nap_ms(20);
    }
    if (!contains(mbuf, n, "ready")) {
        /* A child that failed one of its own checks has exited with the status
         * that says which; one still running is reported as itself. */
        status = -1;
        if (!reap_within(pid, &status, 2000)) {
            puts("PTYPROBE FAIL (the child never reported its terminal ready)\n");
            sys_exit(1);
        }
        puts("PTYPROBE FAIL (the child refused its terminal)\n");
        sys_exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
    }
    if (sys_write(master, "hello\n", 6) != 6) {
        puts("PTYPROBE FAIL (the master would not take the line)\n");
        sys_exit(1);
    }
    if (!reap_within(pid, &status, 5000)) {
        puts("PTYPROBE FAIL (the child never finished)\n");
        sys_exit(1);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        puts("PTYPROBE FAIL (child)\n");
        sys_exit(1);
    }

    /* Drain everything the master accumulated: the echo of "hello" plus the
     * slave's "world" output. */
    for (i = 0; i < 100 && n < (int)sizeof(mbuf) - 1; i++) {
        int r = sys_read(master, mbuf + n, (int)sizeof(mbuf) - 1 - n);
        if (r > 0) {
            n += r;
            if (contains(mbuf, n, "hello") && contains(mbuf, n, "world")) {
                break;
            }
            continue;
        }
        nap_ms(20);
    }
    if (!contains(mbuf, n, "hello") || !contains(mbuf, n, "world")) {
        puts("PTYPROBE FAIL (master round-trip)\n");
        sys_exit(1);
    }

    sys_close(master);
    puts("PTYPROBE OK\n");
    sys_exit(0);
}
