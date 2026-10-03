/**
 * Exercises the pseudo-terminal pair (/dev/ptmx + /dev/pts/N) from user space,
 * the way a network login drives it.
 *
 * * open("/dev/ptmx") returns a master; TIOCGPTN reports the slave index N
 *   (exact request 0x80045430); TIOCSPTLCK(0) unlocks the pair.
 * * The master sets the window size (TIOCSWINSZ), which is what a login server
 *   does with the size the client asked for, before the shell is started.
 * * open("/dev/pts/N") returns the slave; a forked child setsid()s and
 *   TIOCSCTTYs it as its controlling terminal. The child's /proc/PID/stat
 *   then gives its session as its process group and session, the slave's
 *   device 136:N as its terminal, and its group as the terminal's foreground
 *   group, the fields `ps` and `w` read.
 * * On the slave: TIOCGWINSZ reports the size the master set, and TCGETS and
 *   TCSETS read and write the termios. These reach the shared terminal handler
 *   rather than the slave's own ops, and `stty` over a network login is
 *   nothing but these three. TCGETS2 and TCSETSW2, the termios2 requests GNU
 *   libc's tcgetattr, tcsetattr and isatty make, read the same settings with
 *   the speeds the baud code names, and install them.
 * * master write -> the slave reads the cooked line, and the echo comes back
 *   on the master; slave write -> the master reads the ONLCR output.
 *
 * The child verifies the ioctls and the cooked read and reports via exit
 * status; the parent verifies the echo and output it reads back on the master.
 * The child writes a readiness line before the parent writes anything, so the
 * echo the parent checks is never settled while the child has ECHO off, and
 * every wait here is bounded (a non-blocking master, a reaped-within-a-limit
 * child) so a stalled half fails the test rather than hanging it.
 * Prints "PTYPROBE OK" or "PTYPROBE FAIL". Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"

#define O_RDONLY 0
#define O_RDWR   2
#define SIGHUP   1
#define SIGKILL  9
#define EAGAIN   11

#define TIOCSCTTY  0x540E
#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431
#define FIONBIO    0x5421
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCGETS2    0x802C542A
#define TCSETSW2   0x402C542C
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

#define ECHO   0x0008  /* c_lflag bit, the one a password prompt clears */
#define CBAUD  0010017 /* c_cflag's baud code */
#define B38400 0000017

/* The kernel termios (sys/tty/n_tty.h): four flag words, a line discipline
   byte, and nineteen control characters; the termios2 adds the input and
   output speeds. */
#define TIO_CFLAG   8
#define TIO_LFLAG   12
#define TIO_SIZE    36
#define TIO2_ISPEED 36
#define TIO2_OSPEED 40
#define TIO2_SIZE   44

#define WS_ROWS 40 /* the size the master sets, as a login server does */
#define WS_COLS 100

#define WNOHANG 1

#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)

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
    return syscall3(SYS_OPEN, (long)p, flags, 0);
}
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, fd);
}
static int sys_read(int fd, void *b, int n)
{
    return syscall3(SYS_READ, fd, (long)b, n);
}
static int sys_write(int fd, const void *b, int n)
{
    return syscall3(SYS_WRITE, fd, (long)b, n);
}
static int sys_ioctl(int fd, int req, void *arg)
{
    return syscall3(SYS_IOCTL, fd, req, (long)arg);
}
static int sys_waitpid(int pid, int *st, int o)
{
    return syscall3(SYS_WAITPID, pid, (long)st, o);
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
    long ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    syscall3(SYS_NANOSLEEP, (long)ts, 0, 0);
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

/** Append a decimal number and suffix to a path prefix. */
static void numbered_path(char *path, const char *prefix, int number, const char *suffix)
{
    char digits[12];
    int n = 0;

    while (*prefix) {
        *path++ = *prefix++;
    }
    do {
        digits[n++] = (char)('0' + number % 10);
        number /= 10;
    } while (number);
    while (n) {
        *path++ = digits[--n];
    }
    while (*suffix) {
        *path++ = *suffix++;
    }
    *path = 0;
}

/**
 * Whether /proc/<sid>/stat, read for the leader `sid` of a session whose
 * controlling terminal is /dev/pts/<ptn>, gives `sid` as the process group,
 * the session and the terminal's foreground group (fields 5, 6 and 8) and the
 * slave's device number as the terminal (field 7).
 */
static int stat_has_tty(int sid, int ptn)
{
    char path[40], state[256], want[64];
    int fd, n;

    numbered_path(path, "/proc/", sid, "/stat");
    fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    n = sys_read(fd, state, sizeof(state));
    sys_close(fd);
    numbered_path(want, " ", sid, "");
    numbered_path(want + strlen(want), " ", sid, "");
    numbered_path(want + strlen(want), " ", (136 << 8) | ptn, "");
    numbered_path(want + strlen(want), " ", sid, " ");
    return n > 0 && contains(state, n, want);
}

/**
 * The termios2 rows on the slave, whose settings as TCGETS reads them are
 * `tio`: TCGETS2 must read the same 36 bytes, and TCSETSW2 must install a
 * change to the echo and to the baud code, which TCGETS2 then reads back
 * with B38400's speed in both speed fields. Returns 0 with the echo off, or
 * the status that says which row failed.
 */
static int termios2_rows(int slave, const unsigned char *tio)
{
    unsigned char tio2[TIO2_SIZE], back[TIO2_SIZE];
    unsigned int *cflag = (unsigned int *)(tio2 + TIO_CFLAG);
    unsigned int *lflag = (unsigned int *)(tio2 + TIO_LFLAG);
    int b;

    for (b = 0; b < TIO2_SIZE; b++) {
        tio2[b] = 0xFF;
    }
    if (sys_ioctl(slave, TCGETS2, tio2) < 0) {
        return 23; /* the slave has no TCGETS2 */
    }
    for (b = 0; b < TIO_SIZE && tio2[b] == tio[b]; b++) {
    }
    if (b < TIO_SIZE) {
        return 24; /* TCGETS2 read other settings than TCGETS */
    }
    *cflag = (*cflag & ~(unsigned int)CBAUD) | B38400;
    *lflag &= ~(unsigned int)ECHO;
    if (sys_ioctl(slave, TCSETSW2, tio2) < 0) {
        return 25; /* the slave has no TCSETSW2 */
    }
    if (sys_ioctl(slave, TCGETS2, back) < 0 || (*(unsigned int *)(back + TIO_LFLAG) & ECHO) ||
        *(unsigned int *)(back + TIO2_ISPEED) != 38400 ||
        *(unsigned int *)(back + TIO2_OSPEED) != 38400) {
        return 26; /* the change did not take, or the speeds are not B38400's */
    }
    return 0;
}

/**
 * The child's rows on its slave: the window size the master set, a TCGETS
 * and TCSETS round trip of the echo, and the termios2 rows. Returns 0 with
 * the settings as they were, or the status that says which row failed.
 */
static int slave_rows(int slave)
{
    unsigned char tio[TIO_SIZE], back[TIO_SIZE];
    unsigned int lflag;
    short ws[4];
    int b;

    ws[0] = 0;
    ws[1] = 0;
    if (sys_ioctl(slave, TIOCGWINSZ, ws) < 0) {
        return 14; /* the slave has no TIOCGWINSZ */
    }
    if (ws[0] != WS_ROWS || ws[1] != WS_COLS) {
        return 15; /* the size the master set did not reach it */
    }
    for (b = 0; b < TIO_SIZE; b++) {
        tio[b] = 0;
    }
    if (sys_ioctl(slave, TCGETS, tio) < 0) {
        return 16; /* the slave has no TCGETS */
    }
    lflag = *(unsigned int *)(tio + TIO_LFLAG);
    if (!(lflag & ECHO)) {
        return 17; /* a fresh slave echoes */
    }
    *(unsigned int *)(tio + TIO_LFLAG) = lflag & ~(unsigned int)ECHO;
    if (sys_ioctl(slave, TCSETS, tio) < 0) {
        return 18; /* the slave has no TCSETS */
    }
    if (sys_ioctl(slave, TCGETS, back) < 0 || (*(unsigned int *)(back + TIO_LFLAG) & ECHO)) {
        return 19; /* the write did not take */
    }
    *(unsigned int *)(tio + TIO_LFLAG) = lflag;
    if (sys_ioctl(slave, TCSETS, tio) < 0) {
        return 20; /* the echo must return for the round trip */
    }
    b = termios2_rows(slave, tio);
    if (b != 0) {
        return b;
    }
    if (sys_ioctl(slave, TCSETS, tio) < 0) {
        return 27; /* the echo must return again */
    }
    return 0;
}

/** Open a fresh canonical pair with both endpoints blocking. */
static int open_pair(int *master, int *slave)
{
    int index = -1, unlock = 0;
    char path[32];

    *slave = -1;
    *master = sys_open("/dev/ptmx", O_RDWR);
    if (*master < 0 || sys_ioctl(*master, TIOCGPTN, &index) < 0 || index < 0 ||
        sys_ioctl(*master, TIOCSPTLCK, &unlock) < 0) {
        return 0;
    }
    numbered_path(path, "/dev/pts/", index, "");
    *slave = sys_open(path, O_RDWR);
    return *slave >= 0;
}

/**
 * Require the child to reach its read wait before closing the master.
 * The child closes its master and reads; it has no other blocking operation.
 */
static int reader_blocked(int pid)
{
    char path[40], state[256];
    int tries, fd, n;

    numbered_path(path, "/proc/", pid, "/stat");
    for (tries = 0; tries < 100; tries++) {
        fd = sys_open(path, O_RDONLY);
        if (fd < 0) {
            return 0;
        }
        n = sys_read(fd, state, sizeof(state));
        sys_close(fd);
        if (contains(state, n, ") S ")) {
            return 1;
        }
        if (contains(state, n, ") Z ")) {
            return 0;
        }
        nap_ms(20);
    }
    return 0;
}

/** Reap every child, including a reader left blocked by a faulty kernel. */
static void stop_readers(int *pids)
{
    int i, status;

    for (i = 0; i < 2; i++) {
        if (pids[i] > 0) {
            syscall3(SYS_KILL, pids[i], SIGKILL, 0);
            sys_waitpid(pids[i], &status, 0);
        }
    }
}

/**
 * Close the master with two canonical readers blocked and require EOF in both.
 * SIGHUP is ignored or blocked by the whole foreground group, including us.
 * Each child reads again to check that EOF persists after the marker is gone.
 */
static int hangup_readers(int block_hup)
{
    /* The kernel's sigaction: the handler, SIG_IGN, the flags, the restorer
       and the 64-bit mask. */
    struct {
        unsigned long handler, flags, restorer;
        unsigned long long mask;
    } action = {1, 0, 0, 0};
    static const unsigned int mask[2] = {1, 0};
    int master = -1, slave = -1, pids[2] = {0, 0};
    int i, status, ok = 0;
    char byte;

    if (block_hup) {
        action.handler = 0;
        if (syscall4(SYS_RT_SIGPROCMASK, 0, (long)mask, 0, 8) < 0) {
            goto done;
        }
    }
    if (syscall4(SYS_RT_SIGACTION, SIGHUP, (long)&action, 0, 8) < 0 ||
        !open_pair(&master, &slave) || sys_ioctl(slave, TIOCSCTTY, 0) < 0) {
        goto done;
    }
    for (i = 0; i < 2; i++) {
        pids[i] = sys_fork();
        if (pids[i] < 0) {
            goto done;
        }
        if (pids[i] == 0) {
            sys_close(master);
            if (sys_read(slave, &byte, 1) != 0 || sys_read(slave, &byte, 1) != 0) {
                sys_exit(30);
            }
            sys_exit(0);
        }
    }
    if (!reader_blocked(pids[0]) || !reader_blocked(pids[1])) {
        puts("PTYPROBE FAIL (both readers must block before hangup)\n");
        goto done;
    }
    puts(block_hup ? "ptyprobe: hangup: two readers blocked, sighup blocked\n"
                   : "ptyprobe: hangup: two readers blocked, sighup ignored\n");
    sys_close(master);
    master = -1;
    for (i = 0; i < 2; i++) {
        if (!reap_within(pids[i], &status, 2000)) {
            puts("PTYPROBE FAIL (hangup releases both blocked readers)\n");
            goto done;
        }
        pids[i] = 0;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            goto done;
        }
    }
    ok = 1;
done:
    stop_readers(pids);
    if (master >= 0) {
        sys_close(master);
    }
    if (slave >= 0) {
        sys_close(slave);
    }
    return ok;
}

/** Drain a queued line in short reads, then require the persistent EOF. */
static int hangup_queued(void)
{
    int master = -1, slave = -1, nb = 1, ok = 0;
    char buf[8];

    if (!open_pair(&master, &slave)) {
        goto done;
    }
    if (sys_ioctl(slave, FIONBIO, &nb) < 0 || sys_read(slave, buf, 1) != -EAGAIN ||
        sys_write(master, "kept\n", 5) != 5) {
        goto done;
    }
    nb = 0;
    if (sys_ioctl(slave, FIONBIO, &nb) < 0) {
        goto done;
    }
    sys_close(master);
    master = -1;
    if (sys_read(slave, buf, 2) != 2 || buf[0] != 'k' || buf[1] != 'e' ||
        sys_read(slave, buf, sizeof(buf)) != 3 || !contains(buf, 3, "pt\n") ||
        sys_read(slave, buf, 1) != 0 || sys_read(slave, buf, 1) != 0) {
        goto done;
    }
    nb = 1;
    ok = sys_ioctl(slave, FIONBIO, &nb) == 0 && sys_read(slave, buf, 1) == 0;
done:
    if (master >= 0) {
        sys_close(master);
    }
    if (slave >= 0) {
        sys_close(slave);
    }
    return ok;
}

PROBE_START void _start(void)
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
        int sn, sid;

        sid = sys_setsid();
        slave = sys_open(path, O_RDWR);
        if (slave < 0) {
            sys_exit(11); /* open slave failed */
        }
        if (sys_ioctl(slave, TIOCSCTTY, 0) < 0) {
            sys_exit(12); /* TIOCSCTTY failed */
        }
        if (!stat_has_tty(sid, ptn)) {
            sys_exit(28); /* /proc/<pid>/stat does not give the terminal */
        }
        sn = slave_rows(slave);
        if (sn != 0) {
            sys_exit(sn);
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
        status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        numbered_path(mbuf, "PTYPROBE FAIL (the child refused its terminal: status ", status,
                      ")\n");
        puts(mbuf);
        sys_exit(status);
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
    if (!hangup_readers(0) || !hangup_queued() || !hangup_readers(1)) {
        puts("PTYPROBE FAIL (persistent hangup EOF)\n");
        sys_exit(1);
    }
    puts("PTYPROBE OK\n");
    sys_exit(0);
}
