/**
 * vhangprobe: vhangup(2) on a pseudo-terminal, as Linux 6.12 answers it.
 *
 * A child makes the slave of a new pair the controlling terminal of a new
 * session, clears ECHO on it, blocks SIGHUP and SIGCONT, starts a writer
 * that writes more to the slave than the master's ring holds, which nobody
 * reads, waits until the ring is full and the writer asleep in its wait for
 * room, and calls vhangup:
 *
 * * setup: the pair opens and the child's session takes the slave, whose
 *   echo it turns off, and the writer fills the ring and sleeps.
 * * vhangup: the call returns 0 and leaves SIGHUP and SIGCONT pending.
 * * old-read: a read of the descriptor opened before is end of input.
 * * old-write: a write of it is EIO.
 * * old-ioctl: TCGETS on it is EIO, and TIOCSPGRP ENOTTY.
 * * old-poll: poll gives it POLLIN, POLLOUT, POLLERR, POLLHUP, POLLRDNORM
 *   and POLLWRNORM.
 * * no-ctty: the session has no controlling terminal: /dev/tty is ENXIO.
 * * reopen-defaults: a new open of the slave works and has the default
 *   termios again, ECHO among them.
 * * eperm: a process of uid 1000, without CAP_SYS_TTY_CONFIG, gets EPERM.
 * * blocked-write: the writer, which was waiting for room in the ring, ends
 *   with the bytes it wrote, as Linux's n_tty_write.
 * * output-flush: unread output is discarded, and the master in packet
 *   mode has no status pending: its read is EAGAIN at once, as Linux's
 *   __tty_hangup clears pktstatus after its flush.
 * * cleanup: the child exits 0 and the master closes.
 *
 * The child reports through its exit status the first row that failed; the
 * parent turns it into the report. In a native run, `ids=1000:NEW`
 * (probe_args.h) gives the id that stands for 1000. Prints "VHANGPROBE OK"
 * or "VHANGPROBE FAIL: why". etude's report (probe_report.h), for the case
 * ETUDE_CASE names, has an assertion for each row above in order; the first
 * failure fails its assertion and leaves the rest not run. Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

#define O_RDONLY   0
#define O_RDWR     2
#define O_NOCTTY   0400
#define O_NONBLOCK 04000
#define EAGAIN     11
#define SIGHUP     1
#define SIGCONT    18
#define EINTR      4
#define EIO        5
#define ENXIO      6
#define EPERM      1
#define ENOTTY     25

#define TIOCPKT    0x5420
#define FIONREAD   0x541B
#define TIOCSCTTY  0x540E
#define TIOCSPGRP  0x5410
#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431
#define TCGETS     0x5401
#define TCSETS     0x5402

#define ECHO      0x0008
#define TIO_LFLAG 12
#define TIO_SIZE  36

/* poll's bits: POLLIN, POLLOUT, POLLERR, POLLHUP, POLLRDNORM, POLLWRNORM. */
#define POLL_ASKED 0x145
#define POLL_HUNG  0x15D

#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)

/* The child's writer, killed and reaped on any failed row. */
static long writer_pid;

struct pollfd {
    int fd;
    short events;
    short revents;
};

/* The child's exit status for the first row that failed: one more than
 * the row's index in `plan`. */
#define ROW_SETUP     1
#define ROW_VHANGUP   2
#define ROW_OLD_READ  3
#define ROW_OLD_WRITE 4
#define ROW_OLD_IOCTL 5
#define ROW_OLD_POLL  6
#define ROW_NO_CTTY   7
#define ROW_REOPEN    8
#define ROW_EPERM     9
#define ROW_WRITER    10
#define ROW_FLUSH     11

/* The bytes the writer writes, more than the master's ring holds. */
#define WRITER_BYTES 65536

/* The master's ring is 4096 bytes; a writer under OPOST, which keeps room
 * for ONLCR's CR LF, waits once fewer than 2 are free. */
#define RING_FULL 4095

/* The rows, in the order of the report and of the child's exit codes. */
static const char *const plan[] = {"setup",     "vhangup",       "old-read",     "old-write",
                                   "old-ioctl", "old-poll",      "no-ctty",      "reopen-defaults",
                                   "eperm",     "blocked-write", "output-flush", "cleanup"};

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Zero the `n` bytes at `p`, a buffer the kernel fills. */
static void clear(void *p, size_t n)
{
    char *d = p;

    while (n--) {
        *d++ = 0;
    }
}

/** Copy `n` bytes from `src` to `dst`. */
static void copy(void *dst, const void *src, size_t n)
{
    char *d = dst;
    const char *s = src;

    while (n--) {
        *d++ = *s++;
    }
}

/** Write `head`, `s` and a newline to standard output, in one write. */
static void say(const char *head, const char *s)
{
    char line[160];
    size_t n = strlen(head), m = strlen(s);

    if (n + m > sizeof(line) - 1) {
        m = sizeof(line) - 1 - n;
    }
    copy(line, head, n);
    copy(line + n, s, m);
    line[n + m] = '\n';
    syscall3(SYS_WRITE, 1, (long)line, (long)(n + m + 1));
}

/** Write "/dev/pts/" and the decimal `n` into `out`. */
static void pts_path(char *out, int n)
{
    static const char head[] = "/dev/pts/";
    char digits[12];
    int k = 0;

    copy(out, head, sizeof(head) - 1);
    out += sizeof(head) - 1;
    do {
        digits[k++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (k) {
        *out++ = digits[--k];
    }
    *out = '\0';
}

/** The lflag word of the termios of `fd`, or -1 when TCGETS fails. */
static long lflag_of(int fd)
{
    unsigned char tio[TIO_SIZE];
    unsigned int lflag;

    clear(tio, sizeof(tio));
    if (syscall3(SYS_IOCTL, fd, TCGETS, (long)tio) != 0) {
        return -1;
    }
    copy(&lflag, tio + TIO_LFLAG, sizeof(lflag));
    return (long)lflag;
}

/** Exit with `code`; does not return. */
static void leave(int code)
{
    if (writer_pid > 0) {
        int status;

        syscall2(SYS_KILL, writer_pid, 9);
        while (syscall3(SYS_WAITPID, writer_pid, (long)&status, 0) == -EINTR) {
        }
    }
    syscall1(SYS_EXIT, code);
    __builtin_unreachable();
}

/** Whether a child of uid `uid` gets EPERM from vhangup. */
static int eperm_row(long uid)
{
    long pid = syscall0(SYS_FORK);
    int status = -1;

    if (pid == 0) {
        writer_pid = 0;
        if (syscall1(SYS_SETUID, uid) != 0) {
            leave(2);
        }
        leave(syscall0(SYS_VHANGUP) == -EPERM ? 0 : 1);
    }
    if (pid < 0 || syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        return 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/** Start a process that writes WRITER_BYTES to the slave `slave` and exits
 * 0 when the write ends with fewer bytes, as one the hangup ended in its
 * wait does: its pid, or -1. */
static long start_writer(long slave)
{
    static char bytes[WRITER_BYTES];
    long pid = syscall0(SYS_FORK), rc;

    if (pid != 0) {
        return pid;
    }
    for (rc = 0; rc < WRITER_BYTES; rc++) {
        bytes[rc] = 'w';
    }
    rc = syscall3(SYS_WRITE, slave, (long)bytes, WRITER_BYTES);
    leave(rc > 0 && rc < WRITER_BYTES ? 0 : 1);
    return -1;
}

/** Open the child's controlling slave, turn echo off, block hangup signals and
 * queue output for the hangup, with `master` in packet mode. Its descriptor
 * or -1 when setup fails; `ptn` is the slave's index. */
static long child_setup(int ptn, long master)
{
    unsigned long long mask = (1ull << (SIGHUP - 1)) | (1ull << (SIGCONT - 1));
    unsigned char tio[TIO_SIZE];
    unsigned int lflag;
    char path[24];
    long slave;
    int packet = 1;

    clear(tio, sizeof(tio));
    pts_path(path, ptn);
    if (syscall0(SYS_SETSID) < 0) {
        return -1;
    }
    slave = syscall3(SYS_OPEN, (long)path, O_RDWR | O_NOCTTY, 0);
    if (slave < 0 || syscall3(SYS_IOCTL, slave, TIOCSCTTY, 0) != 0 ||
        syscall3(SYS_IOCTL, slave, TCGETS, (long)tio) != 0) {
        return -1;
    }
    copy(&lflag, tio + TIO_LFLAG, sizeof(lflag));
    lflag &= ~(unsigned int)ECHO;
    copy(tio + TIO_LFLAG, &lflag, sizeof(lflag));
    if (syscall3(SYS_IOCTL, slave, TCSETS, (long)tio) != 0 || (lflag_of((int)slave) & ECHO)) {
        return -1;
    }
    if (syscall4(SYS_RT_SIGPROCMASK, 0, (long)&mask, 0, sizeof(mask)) != 0) {
        return -1;
    }
    if (syscall3(SYS_IOCTL, master, TIOCPKT, (long)&packet) != 0 ||
        syscall3(SYS_WRITE, slave, (long)"unread", 6) != 6) {
        return -1;
    }
    return slave;
}

/** The state letter of process `pid`, the field after the parenthesised
 * name in its `/proc/<pid>/stat`: `S` for a sleep a signal ends. 0 when the
 * file cannot be read or has no state. */
static char state_of(long pid)
{
    static const char head[] = "/proc/", tail[] = "/stat";
    char path[32], text[256], digits[12];
    long fd, len;
    int n = 0, i = 0, j;

    do {
        digits[n++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0 && n < 11);
    for (j = 0; head[j]; j++) {
        path[i++] = head[j];
    }
    while (n > 0) {
        path[i++] = digits[--n];
    }
    for (j = 0; tail[j]; j++) {
        path[i++] = tail[j];
    }
    path[i] = '\0';
    fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    len = syscall3(SYS_READ, fd, (long)text, (long)sizeof(text));
    syscall1(SYS_CLOSE, fd);
    for (i = 0; i + 2 < len; i++) {
        if (text[i] == ')' && text[i + 1] == ' ') {
            return text[i + 2];
        }
    }
    return 0;
}

/**
 * Wait until the writer `pid` sleeps in its wait for room: the master
 * `master` counts RING_FULL bytes or more with FIONREAD, and the writer is
 * `S`, which once the ring is full only that wait gives. Returns 1, or 0
 * when FIONREAD fails or the writer has ended. The probe yields between
 * looks, and their number decides nothing; the case's watchdog ends a run
 * whose writer never sleeps.
 */
static int writer_asleep(long master, long pid)
{
    int queued = 0;
    char state;

    for (;;) {
        if (syscall3(SYS_IOCTL, master, FIONREAD, (long)&queued) != 0) {
            return 0;
        }
        state = state_of(pid);
        if (state == 0 || state == 'Z') {
            return 0;
        }
        if (queued >= RING_FULL && state == 'S') {
            return 1;
        }
        syscall0(SYS_SCHED_YIELD);
    }
}

/** Hang up the slave and require both signals pending on its session leader. */
static int hangup_signals(void)
{
    unsigned long long pending = 0;
    unsigned long long want = (1ull << (SIGHUP - 1)) | (1ull << (SIGCONT - 1));

    return syscall0(SYS_VHANGUP) == 0 &&
           syscall2(SYS_RT_SIGPENDING, (long)&pending, sizeof(pending)) == 0 &&
           (pending & want) == want;
}

/** Reap `pid`, the slave writer, and clear its cleanup hold once reaped.
 * Return whether it exited successfully. */
static int writer_done(long pid)
{
    int status = -1;

    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        return 0;
    }
    writer_pid = 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/**
 * The child's rows on the slave /dev/pts/`ptn`, with `uid` for the eperm
 * row and its nonblocking `master` for the output check. Returns the ROW_
 * of the first that failed, or 0 when all held.
 */
static int child_rows(int ptn, long uid, long master)
{
    const unsigned char tio[TIO_SIZE] = {0};
    struct pollfd pfd;
    char path[24], byte;
    long slave = child_setup(ptn, master), fd, writer;
    int queued = -1;

    if (slave < 0) {
        return ROW_SETUP;
    }
    pts_path(path, ptn);
    writer = start_writer(slave);
    if (writer < 0) {
        return ROW_SETUP;
    }
    writer_pid = writer;
    if (!writer_asleep(master, writer)) {
        return ROW_SETUP;
    }
    if (!hangup_signals()) {
        return ROW_VHANGUP;
    }
    if (syscall3(SYS_READ, slave, (long)&byte, 1) != 0) {
        return ROW_OLD_READ;
    }
    if (syscall3(SYS_WRITE, slave, (long)"x", 1) != -EIO) {
        return ROW_OLD_WRITE;
    }
    if (syscall3(SYS_IOCTL, slave, TCGETS, (long)tio) != -EIO ||
        syscall3(SYS_IOCTL, slave, TIOCSPGRP, (long)&ptn) != -ENOTTY) {
        return ROW_OLD_IOCTL;
    }
    pfd.fd = (int)slave;
    pfd.events = POLL_ASKED;
    pfd.revents = 0;
    if (syscall3(SYS_POLL, (long)&pfd, 1, 0) != 1 || (pfd.revents & 0xFFFF) != POLL_HUNG) {
        return ROW_OLD_POLL;
    }
    if (syscall3(SYS_OPEN, (long)"/dev/tty", O_RDWR, 0) != -ENXIO) {
        return ROW_NO_CTTY;
    }
    fd = syscall3(SYS_OPEN, (long)path, O_RDWR | O_NOCTTY, 0);
    if (fd < 0 || lflag_of((int)fd) < 0 || !(lflag_of((int)fd) & ECHO)) {
        return ROW_REOPEN;
    }
    if (!eperm_row(uid)) {
        return ROW_EPERM;
    }
    if (!writer_done(writer)) {
        return ROW_WRITER;
    }
    if (syscall3(SYS_IOCTL, master, FIONREAD, (long)&queued) != 0 || queued != 0 ||
        syscall3(SYS_READ, master, (long)&byte, 1) != -EAGAIN) {
        return ROW_FLUSH;
    }
    return 0;
}

/** Report the rows before the index `failed` as held and that one as
 * failed with `why`, print the failure and exit 1. */
static void fail_at(int failed, const char *why)
{
    int i;

    for (i = 0; i < failed; i++) {
        report_held();
    }
    say("VHANGPROBE FAIL: ", why);
    report_stop(why);
    leave(1);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * Open the pair, run the child's rows and report them. `_start` calls it
 * with the initial stack.
 */
void probe_main(unsigned long *sp)
{
    static const char *const why[] = {
        "setup: the child's session did not take the slave and turn its echo off, or the "
        "writer did not fill the ring and sleep",
        "vhangup: the call failed or SIGHUP and SIGCONT were not pending",
        "old-read: a read of the old descriptor was not end of input",
        "old-write: a write of the old descriptor was not EIO",
        "old-ioctl: TCGETS was not EIO, or TIOCSPGRP not ENOTTY",
        "old-poll: poll did not give the hung-up bits",
        "no-ctty: /dev/tty was not ENXIO",
        "reopen-defaults: a new open did not have the default termios",
        "eperm: uid 1000 did not get EPERM",
        "blocked-write: the waiting writer did not end with the bytes it wrote",
        "output-flush: unread output survived, or the master had packet status pending"};
    int ptn = -1, unlock = 0, status = -1;
    long master, pid;
    int i;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.vhangprobe"));
    for (i = 0; i < (int)(sizeof(plan) / sizeof(plan[0])); i++) {
        report_plan(plan[i]);
    }
    probe_args((int)sp[0], (char **)(sp + 1));
    master = syscall3(SYS_OPEN, (long)"/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
    if (master < 0 || syscall3(SYS_IOCTL, master, TIOCGPTN, (long)&ptn) != 0 || ptn < 0 ||
        syscall3(SYS_IOCTL, master, TIOCSPTLCK, (long)&unlock) != 0) {
        fail_at(0, "setup: /dev/ptmx did not open and unlock a pair");
    }
    pid = syscall0(SYS_FORK);
    if (pid == 0) {
        leave(child_rows(ptn, probe_id(1000), master));
    }
    if (pid < 0 || syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail_at(0, "setup: the child could not be reaped");
    }
    if (!WIFEXITED(status)) {
        char signal_why[] = "vhangup: child ended by signal 00";
        int sig = status & 0x7f;

        signal_why[sizeof(signal_why) - 3] = (char)('0' + sig / 10);
        signal_why[sizeof(signal_why) - 2] = (char)('0' + sig % 10);
        fail_at(ROW_VHANGUP - 1, signal_why);
    }
    i = WEXITSTATUS(status);
    if (i > 0 && i <= (int)(sizeof(why) / sizeof(why[0]))) {
        syscall1(SYS_CLOSE, master);
        fail_at(i - 1, why[i - 1]);
    }
    if (i != 0) {
        fail_at(0, "setup: the child exited with a status no row gives");
    }
    for (i = 0; i < (int)(sizeof(why) / sizeof(why[0])); i++) {
        report_held();
    }
    if (syscall1(SYS_CLOSE, master) != 0) {
        fail_at(0, "cleanup: the master did not close");
    }
    report_held();
    report_end();
    say("VHANGPROBE OK", "");
    leave(0);
}
