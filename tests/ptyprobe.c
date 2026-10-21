/**
 * ptyprobe: the pseudo-terminal pair, /dev/ptmx and /dev/pts/N, as a network
 * login drives it.
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
 *   the speeds of the baud code, and install them.
 * * master write -> the slave reads the cooked line, and the echo comes back
 *   on the master; slave write -> the master reads the ONLCR output.
 * * Closing the master gives two canonical readers blocked on the slave the
 *   end of input, and again on a second read, with SIGHUP ignored and then
 *   with it blocked; a line queued before the close is read in short reads,
 *   and the end of input after it stays.
 *
 * The child checks the ioctls and the cooked read and reports through its
 * exit status; the parent checks the echo and the output it reads back on
 * the master. The child writes a readiness line before the parent writes
 * anything, so that the echo the parent checks is never settled while the
 * child has ECHO off. Every wait ends on what it waits for or on evidence
 * that it cannot come: the readiness line or the child's exit, a reader's
 * state S in /proc/PID/stat or its exit, a released reader's exit or its
 * sleep again after the hangup. No wait counts its looks or its time; the
 * runner's limit on the probe's step ends a run that stalls. Prints
 * "PTYPROBE START", then "PTYPROBE OK" or "PTYPROBE FAIL (why)".
 * Freestanding; direct system calls (probe.h).
 *
 * etude's report (probe_report.h), for the case ETUDE_CASE names, comes from
 * the parent alone and has an assertion for each row, a setup step joined to
 * the row after it: `ptmx-open`, `tiocgptn`, `unlock`, `master-winsz`,
 * `master-nonblock`, `ctty`, `proc-stat-tty`, `slave-winsz`, `tcgets`,
 * `tcsets`, `tcgets2`, `tcsetsw2`, `cooked-read`, `slave-output`, `echo`,
 * `hangup-ignored`, `hangup-queued` and `hangup-blocked`. The child's rows,
 * `ctty` to `tcsetsw2` and the cooked read, are reported from its exit status
 * and its readiness line. The first failure fails its assertion and leaves the
 * rest not run; a child that ends before it is ready with a status no row gives
 * fails `ctty`.
 */

#include "probe.h"
#include "probe_report.h"

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

/* The kernel termios (rach2/sys/tty/n_tty.h): four flag words, a line
 * discipline byte and nineteen control characters; the termios2 of TCGETS2
 * (rach2/sys/syscall.c) adds the input and output speeds. */
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

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}
/** fork(2). */
static int sys_fork(void)
{
    return syscall1(SYS_FORK, 0);
}
/** setsid(2). */
static int sys_setsid(void)
{
    return syscall1(SYS_SETSID, 0);
}
/** open(2) with no mode. */
static int sys_open(const char *p, int flags)
{
    return syscall3(SYS_OPEN, (long)p, flags, 0);
}
/** close(2). */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, fd);
}
/** read(2). */
static int sys_read(int fd, void *b, int n)
{
    return syscall3(SYS_READ, fd, (long)b, n);
}
/** write(2). */
static int sys_write(int fd, const void *b, int n)
{
    return syscall3(SYS_WRITE, fd, (long)b, n);
}
/** ioctl(2). */
static int sys_ioctl(int fd, int req, void *arg)
{
    return syscall3(SYS_IOCTL, fd, req, (long)arg);
}
/** waitpid(2). */
static int sys_waitpid(int pid, int *st, int o)
{
    return syscall3(SYS_WAITPID, pid, (long)st, o);
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
/** Write the string `s`, which ends in a newline, to standard output. */
static void puts(const char *s)
{
    sys_write(1, s, (int)strlen(s));
}

/** Give the processor to another process that can run, if any. */
static void yield(void)
{
    syscall0(SYS_SCHED_YIELD);
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

/**
 * Write `prefix`, the non-negative `number` in decimal and `suffix` into
 * `path`, ended with a NUL; `path` must have room for all of it.
 */
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

/**
 * Open a new pair and unlock it, both ends blocking and the slave canonical,
 * as a new pair starts. Returns 1 with the ends in `*master` and `*slave`, or
 * 0 when a step fails; an end that was not opened is then negative, and the
 * caller closes each end that was.
 */
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
 * The state letter of the unreaped child `pid`, the field after the
 * parenthesised name in /proc/PID/stat: 'S' for a sleep a signal ends, 'Z'
 * for a zombie. 0 when the file cannot be opened or has no state.
 */
static char child_state(int pid)
{
    char path[40], state[256];
    int fd, n, i;

    numbered_path(path, "/proc/", pid, "/stat");
    fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    n = sys_read(fd, state, sizeof(state));
    sys_close(fd);
    for (i = n - 3; i >= 0; i--) {
        if (state[i] == ')' && state[i + 1] == ' ') {
            return state[i + 2];
        }
    }
    return 0;
}

/**
 * Wait until the reader `pid` is in state S. A reader closes its master and
 * reads, and has nothing else to sleep on, so state S means it is blocked in
 * its read. Returns 1 then, and 0 once it is a zombie or its stat file
 * cannot be opened, since it can then no longer block. The probe yields
 * between looks, and their number decides nothing.
 */
static int reader_blocked(int pid)
{
    char state;

    for (;;) {
        state = child_state(pid);
        if (state == 'S') {
            return 1;
        }
        if (state == 0 || state == 'Z') {
            return 0;
        }
        yield();
    }
}

/**
 * Wait until the reader `pid`, blocked before the master closed, is reaped
 * with `*status` filled: 1. Returns 0 when it is seen in state S, or its
 * stat file cannot be opened, before it is reaped.
 *
 * Closing the master wakes the slave's readers before close returns
 * (`tty_hangup`), and a released reader reads the end of input twice and
 * exits without sleeping again, so a reader seen asleep after the close was
 * not released and never will be. Until it is reaped or seen asleep, the
 * probe yields and looks again.
 */
static int reader_released(int pid, int *status)
{
    char state;

    for (;;) {
        if (sys_waitpid(pid, status, WNOHANG) == pid) {
            return 1;
        }
        state = child_state(pid);
        if (state == 0 || state == 'S') {
            return 0;
        }
        yield();
    }
}

/**
 * Kill and reap each of the two readers in `pids` not yet reaped, whose pid
 * is still positive, such as one a faulty kernel left blocked in its read.
 */
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
 * Close the master of a new pair with two canonical readers blocked on the
 * slave, and require the end of input in both, on a first read and again on
 * a second. Returns 1 when both readers saw it, else 0.
 *
 * The slave becomes the probe's controlling terminal, and closing the master
 * sends SIGHUP to its foreground group, the probe and both readers. The probe
 * ignores SIGHUP, or with `block_hup` sets its default action and blocks
 * it, before it forks, so that the readers inherit the setting and the
 * signal ends none of them. The setting stays in the probe on return.
 */
static int hangup_readers(int block_hup)
{
    /* The kernel's sigaction: handler (SIG_IGN), flags, restorer, mask. */
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
        if (!reader_released(pids[i], &status)) {
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

/**
 * On a new pair, require a non-blocking read of the empty slave to give
 * -EAGAIN; then queue the line "kept", close the master, and require the
 * slave to give the line in two short reads and then the end of input on
 * each read, blocking and non-blocking. Returns 1 when all of it held.
 */
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

/** Print "PTYPROBE FAIL (`why`)", fail the report's next assertion with
 * `why` (`report_stop`), and exit 1. */
static void fail(const char *why)
{
    char line[96];
    int n = 0;
    const char *part[3];
    int i, k;

    part[0] = "PTYPROBE FAIL (";
    part[1] = why;
    part[2] = ")\n";
    for (i = 0; i < 3; i++) {
        for (k = 0; part[i][k] != '\0' && n < (int)sizeof(line) - 1; k++) {
            line[n++] = part[i][k];
        }
    }
    line[n] = '\0';
    puts(line);
    report_stop(why);
    sys_exit(1);
}

/* The index in the planned path of the child's first row, `ctty`, and of the
 * cooked read. */
#define ROW_CTTY   5
#define ROW_COOKED 12

/** The index in the planned path of the row whose failure the child's exit
 * status `status` says, or ROW_CTTY for a status no row gives. */
static int child_row(int status)
{
    static const signed char rows[] = {11, 5,  12, 5,  28, 6,  14, 7,  15, 7,  16, 8,
                                       17, 8,  18, 9,  19, 9,  20, 9,  23, 10, 24, 10,
                                       25, 11, 26, 11, 27, 11, 21, 12, 13, 12, 22, 13};
    int i;

    for (i = 0; i < (int)sizeof(rows); i += 2) {
        if (rows[i] == status) {
            return rows[i + 1];
        }
    }
    return ROW_CTTY;
}

/** Report the planned assertions before the one at `row` held. */
static void held_to(int row)
{
    while (report_path_at < row) {
        report_held();
    }
}

/** Report the planned assertions before the one at `row` held, and that one
 * failed with `why`, the rest not run. */
static void stop_at(int row, const char *why)
{
    held_to(row);
    report_stop(why);
}

/**
 * Read the non-blocking `master` into `mbuf`, of `cap` bytes, until the
 * child `pid` has written its readiness line, and return the bytes read.
 *
 * Between empty reads the probe looks for the child's exit and yields. A
 * child that exits before it is ready has refused its terminal, and the
 * probe prints its status, reports the row the status says and exits with
 * it; one that fills `mbuf` without the line is killed and the probe exits
 * 1. Neither returns.
 */
static int await_ready(int master, int pid, char *mbuf, int cap)
{
    int n = 0, status = -1;
    char why[64];

    while (n < cap - 1) {
        int r = sys_read(master, mbuf + n, cap - 1 - n);

        if (r > 0) {
            n += r;
            if (contains(mbuf, n, "ready")) {
                return n;
            }
            continue;
        }
        if (sys_waitpid(pid, &status, WNOHANG) == pid) {
            status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            numbered_path(mbuf, "PTYPROBE FAIL (the child refused its terminal: status ", status,
                          ")\n");
            puts(mbuf);
            numbered_path(why, "the child refused its terminal: status ", status, "");
            stop_at(child_row(status), why);
            sys_exit(status);
        }
        yield();
    }
    syscall3(SYS_KILL, pid, SIGKILL, 0);
    sys_waitpid(pid, &status, 0);
    fail("the child never reported its terminal ready");
    return 0;
}

/**
 * Open the master, read its slave's index into `*ptn`, unlock the pair and
 * set the window size, as a login server does before the shell is started.
 * Returns the master; a failure exits.
 */
static int open_master(int *ptn)
{
    int master, unlock = 0;
    short ws[4];

    master = sys_open("/dev/ptmx", O_RDWR);
    if (master < 0) {
        fail("open ptmx");
    }
    report_held();
    *ptn = -1;
    if (sys_ioctl(master, TIOCGPTN, ptn) < 0 || *ptn < 0) {
        fail("TIOCGPTN");
    }
    report_held();
    if (sys_ioctl(master, TIOCSPTLCK, &unlock) < 0) {
        fail("TIOCSPTLCK");
    }
    report_held();
    ws[0] = WS_ROWS;
    ws[1] = WS_COLS;
    ws[2] = 0;
    ws[3] = 0;
    if (sys_ioctl(master, TIOCSWINSZ, ws) < 0) {
        fail("master TIOCSWINSZ");
    }
    report_held();
    return master;
}

/**
 * The child on the slave /dev/pts/`ptn`, at `path`: a new session leader, it
 * makes the slave its controlling terminal, runs its rows, writes "ready",
 * reads the cooked line the parent sends and writes "world". It exits 0, or
 * with the status of the row that failed. It does not return.
 */
static void child(const char *path, int ptn)
{
    char sbuf[32];
    int sn, sid, slave;

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

/** Report the hangup row whose result is `ok` held, or fail it. */
static void hangup_row(int ok)
{
    if (!ok) {
        fail("persistent hangup EOF");
    }
    report_held();
}

/** Begin the report of the case the environment of the initial stack `sp`
 * gives, and plan its assertions in the order they are reported. */
static void plan(unsigned long *sp)
{
    static const char *const ids[] = {
        "ptmx-open",      "tiocgptn",      "unlock",        "master-winsz", "master-nonblock",
        "ctty",           "proc-stat-tty", "slave-winsz",   "tcgets",       "tcsets",
        "tcgets2",        "tcsetsw2",      "cooked-read",   "slave-output", "echo",
        "hangup-ignored", "hangup-queued", "hangup-blocked"};
    unsigned i;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.ptyprobe"));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        report_plan(ids[i]);
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe: the pair, the window size, the child on the slave, the round
 * trip, and the hangup rows.
 *
 * The parent puts the master in non-blocking mode and reads it until
 * "ready" arrives or the child has exited, yielding between tries, so that
 * a child that exits before it is ready fails the test instead of hanging
 * it; when a row of the child fails, it exits with that row's status, which
 * the parent prints. The parent then writes "hello", which the slave reads
 * cooked and the master gets back as the echo, reaps the child, and reads
 * the echo and "world" back on the master: the child wrote both before it
 * exited, so the master has them, and an empty read ends the reading.
 */
void probe_main(unsigned long *sp)
{
    int master, ptn, pid, status, nb;
    char path[32];
    char mbuf[128];
    int n;

    puts("PTYPROBE START\n");
    plan(sp);
    master = open_master(&ptn);
    numbered_path(path, "/dev/pts/", ptn, "");

    pid = sys_fork();
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        child(path, ptn);
    }

    nb = 1;
    if (sys_ioctl(master, FIONBIO, &nb) < 0) {
        fail("the master would not go non-blocking");
    }
    report_held();
    n = await_ready(master, pid, mbuf, (int)sizeof(mbuf));
    held_to(ROW_COOKED);
    if (sys_write(master, "hello\n", 6) != 6) {
        fail("the master would not take the line");
    }
    if (sys_waitpid(pid, &status, 0) != pid) {
        fail("the child never finished");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        puts("PTYPROBE FAIL (child)\n");
        stop_at(WIFEXITED(status) && WEXITSTATUS(status) == 22 ? ROW_COOKED + 1 : ROW_COOKED,
                "child");
        sys_exit(1);
    }
    report_held();

    while (n < (int)sizeof(mbuf) - 1) {
        int r = sys_read(master, mbuf + n, (int)sizeof(mbuf) - 1 - n);
        if (r <= 0) {
            break;
        }
        n += r;
        if (contains(mbuf, n, "hello") && contains(mbuf, n, "world")) {
            break;
        }
    }
    if (!contains(mbuf, n, "world")) {
        fail("master round-trip");
    }
    report_held();
    if (!contains(mbuf, n, "hello")) {
        fail("master round-trip");
    }
    report_held();

    sys_close(master);
    hangup_row(hangup_readers(0));
    hangup_row(hangup_queued());
    hangup_row(hangup_readers(1));
    report_end();
    puts("PTYPROBE OK\n");
    sys_exit(0);
}
