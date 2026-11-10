/**
 * ttyflowprobe: a pseudo-terminal's software flow control and the signal
 * character's flush, as Linux 6.12 answers them.
 *
 * A child of the probe, which leads no session, opens a pair, clears ECHO on
 * the slave, puts the master in packet mode, and checks in order:
 *
 * * setup: the pair opens, the slave's echo goes off and packet mode on.
 * * defaults: a new slave has IXON without IXANY or IXOFF, VSTART ^Q and
 *   VSTOP ^S.
 * * stop: ^S from the master stops the slave's output: the master reads
 *   TIOCPKT_STOP, a nonblocking slave write is EAGAIN and poll gives no
 *   POLLOUT.
 * * blocked-write: a blocking writer of the slave sleeps while output is
 *   stopped, and the master reads nothing.
 * * start: ^Q restarts output: the master reads TIOCPKT_START, the writer
 *   ends with the bytes it wrote, and the master reads them.
 * * not-read: ^S and ^Q in a line reach no reader.
 * * ixany: with IXANY any character restarts output and is read as well.
 * * tcooff: TCXONC TCOOFF stops output with TIOCPKT_STOP, which ^Q does not
 *   end, and TCOON restarts it with TIOCPKT_START; an action past TCION is
 *   EINVAL.
 * * tcioff: TCXONC TCIOFF and TCION send ^S and ^Q to the master.
 * * dostop: turning IXON off gives the master TIOCPKT_NOSTOP, and on again
 *   TIOCPKT_DOSTOP.
 * * signal-flush: ^C under ISIG discards the partial line and the unread
 *   output, all of it or what Linux's master read buffer had not taken, and
 *   the master reads TIOCPKT_FLUSHREAD and TIOCPKT_FLUSHWRITE.
 * * noflsh: under NOFLSH ^C keeps both, and the master reads no status.
 * * cleanup: the descriptors close.
 *
 * Prints "TTYFLOWPROBE OK" or "TTYFLOWPROBE FAIL: why". etude's report
 * (probe_report.h), for the case ETUDE_CASE names, has an assertion for each
 * row above in order; the first failure fails its assertion and leaves the
 * rest not run. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define O_RDONLY   0
#define O_RDWR     2
#define O_NOCTTY   0400
#define O_NONBLOCK 04000
#define EINTR      4
#define EAGAIN     11
#define EINVAL     22

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCXONC     0x540A
#define TIOCPKT    0x5420
#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431

#define TCOOFF 0
#define TCOON  1
#define TCIOFF 2
#define TCION  3

#define TIOCPKT_FLUSHREAD  1
#define TIOCPKT_FLUSHWRITE 2
#define TIOCPKT_STOP       4
#define TIOCPKT_START      8
#define TIOCPKT_NOSTOP     16
#define TIOCPKT_DOSTOP     32

/* struct termios as TCGETS fills it: the flag words, then c_line and c_cc. */
#define TIO_IFLAG 0
#define TIO_LFLAG 12
#define TIO_CC    17
#define TIO_SIZE  36
#define VINTR     0
#define VSTART    8
#define VSTOP     9

#define IXON   0002000
#define IXANY  0004000
#define IXOFF  0010000
#define ECHO   0000010
#define NOFLSH 0000200

#define CTRL_C 003
#define CTRL_Q 021
#define CTRL_S 023

#define POLLIN  0x001
#define POLLOUT 0x004

#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)

struct pollfd {
    int fd;
    short events;
    short revents;
};

/* The writer of the blocked-write row, killed and reaped on any failure. */
static long writer_pid;

/* The pair: the nonblocking master in packet mode, the slave opened
 * nonblocking, and a second, blocking description of it for the writer. */
static long master = -1, slave = -1, bslave = -1;

/* The rows, in the order of the report. */
static const char *const plan[] = {"setup",        "defaults", "stop",   "blocked-write", "start",
                                   "not-read",     "ixany",    "tcooff", "tcioff",        "dostop",
                                   "signal-flush", "noflsh",   "cleanup"};

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
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
    char line[200];
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

/** Kill and reap the writer, if one runs, and exit with `code`. */
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

/** Report the rows before the index `failed` as held and that one as
 * failed with `why`, print the failure and exit 1. */
static void fail_at(int failed, const char *why)
{
    int i;

    for (i = 0; i < failed; i++) {
        report_held();
    }
    say("TTYFLOWPROBE FAIL: ", why);
    report_stop(why);
    leave(1);
}

/** Read the slave's termios into `tio`; 0, or -1 when TCGETS fails. */
static int get_tio(unsigned char *tio)
{
    return syscall3(SYS_IOCTL, slave, TCGETS, (long)tio) == 0 ? 0 : -1;
}

/** The flag word at `off` in `tio`. */
static unsigned int flag_at(const unsigned char *tio, int off)
{
    unsigned int v;

    copy(&v, tio + off, sizeof(v));
    return v;
}

/** Set the slave's flag word at `off` to its value with `set` added and
 * `clear` taken away; 0, or -1 when TCGETS or TCSETS fails. */
static int change_flags(int off, unsigned int set, unsigned int clear)
{
    unsigned char tio[TIO_SIZE];
    unsigned int v;

    if (get_tio(tio) != 0) {
        return -1;
    }
    v = (flag_at(tio, off) | set) & ~clear;
    copy(tio + off, &v, sizeof(v));
    return syscall3(SYS_IOCTL, slave, TCSETS, (long)tio) == 0 ? 0 : -1;
}

/** Write the `n` bytes of `s` to the master; whether all were taken. */
static int type(const char *s, long n)
{
    return syscall3(SYS_WRITE, master, (long)s, n) == n;
}

/** The status byte the master's next read gives alone, or -1 when the read
 * gives data or fails. */
static int status_of(void)
{
    unsigned char buf[8];

    if (syscall3(SYS_READ, master, (long)buf, sizeof(buf)) != 1 || buf[0] == 0) {
        return -1;
    }
    return buf[0];
}

/** Wait with no time limit until the master is readable; whether poll said so.
 * Linux moves a slave's output to the master from a work queue, so it may
 * reach the master after the write that made it has returned. */
static int master_ready(void)
{
    struct pollfd pfd;

    pfd.fd = (int)master;
    pfd.events = POLLIN;
    pfd.revents = 0;
    return syscall3(SYS_POLL, (long)&pfd, 1, -1) == 1 && (pfd.revents & POLLIN);
}

/** Whether the master's next read is a data packet of the `n` bytes `s`. */
static int data_is(const char *s, long n)
{
    unsigned char buf[64];
    long i;

    if (!master_ready() || syscall3(SYS_READ, master, (long)buf, sizeof(buf)) != n + 1 ||
        buf[0] != 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (buf[i + 1] != (unsigned char)s[i]) {
            return 0;
        }
    }
    return 1;
}

/** Whether the master has nothing to read: its read is EAGAIN. */
static int master_empty(void)
{
    unsigned char b;

    return syscall3(SYS_READ, master, (long)&b, 1) == -EAGAIN;
}

/**
 * Whether what the master holds after the slave's output flush is an answer
 * Linux gives: nothing, a read that is EAGAIN, or a data packet of a prefix
 * of "out". Linux's pty_flush_buffer frees only the master's flip buffers,
 * not its n_tty read buffer, so bytes already moved there survive.
 */
static int flushed_out(void)
{
    static const char out[] = "out";
    unsigned char buf[8];
    long n, i;

    n = syscall3(SYS_READ, master, (long)buf, sizeof(buf));
    if (n == -EAGAIN) {
        return 1;
    }
    if (n < 1 || n > 4 || buf[0] != 0) {
        return 0;
    }
    for (i = 1; i < n; i++) {
        if (buf[i] != (unsigned char)out[i - 1]) {
            return 0;
        }
    }
    return master_empty();
}

/** Whether the slave's next read gives the `n` bytes `s`. */
static int slave_reads(const char *s, long n)
{
    char buf[64];
    long i;

    if (syscall3(SYS_READ, slave, (long)buf, sizeof(buf)) != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (buf[i] != s[i]) {
            return 0;
        }
    }
    return 1;
}

/** Whether the slave's output is stopped: a nonblocking write is EAGAIN
 * and poll gives no POLLOUT. */
static int slave_stopped(void)
{
    struct pollfd pfd;

    pfd.fd = (int)slave;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    return syscall3(SYS_WRITE, slave, (long)"x", 1) == -EAGAIN &&
           syscall3(SYS_POLL, (long)&pfd, 1, 0) == 0;
}

/** Whether the slave's output flows: a write of one byte goes to the master,
 * which reads it as data. */
static int slave_flows(void)
{
    return syscall3(SYS_WRITE, slave, (long)"y", 1) == 1 && data_is("y", 1);
}

/** The state letter of process `pid` from its `/proc/<pid>/stat`, or 0. */
static char state_of(long pid)
{
    static const char head[] = "/proc/", tail[] = "/stat";
    static char path[32], text[256];
    char digits[12];
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
 * Start a writer of "late" on the blocking slave and wait until it sleeps,
 * `S`, which while output is stopped only its wait gives. 1, or 0 when the
 * fork fails or the writer ends. The probe yields between looks, and their
 * number decides nothing; the case's watchdog ends a run whose writer never
 * sleeps.
 */
static int start_writer(void)
{
    long pid = syscall0(SYS_FORK);
    char state;

    if (pid == 0) {
        writer_pid = 0;
        leave(syscall3(SYS_WRITE, bslave, (long)"late", 4) == 4 ? 0 : 1);
    }
    if (pid < 0) {
        return 0;
    }
    writer_pid = pid;
    for (;;) {
        state = state_of(pid);
        if (state == 0 || state == 'Z') {
            return 0;
        }
        if (state == 'S') {
            return 1;
        }
        syscall0(SYS_SCHED_YIELD);
    }
}

/** Reap the writer; whether it exited 0, having written all its bytes. */
static int writer_done(void)
{
    int status = -1;

    if (syscall3(SYS_WAITPID, writer_pid, (long)&status, 0) != writer_pid) {
        return 0;
    }
    writer_pid = 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/** Open and unlock a pair, open its slave twice, clear ECHO and set packet
 * mode; whether all of it worked. */
static int setup(void)
{
    char path[24];
    int ptn = -1, unlock = 0, packet = 1;

    master = syscall3(SYS_OPEN, (long)"/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
    if (master < 0 || syscall3(SYS_IOCTL, master, TIOCGPTN, (long)&ptn) != 0 || ptn < 0 ||
        syscall3(SYS_IOCTL, master, TIOCSPTLCK, (long)&unlock) != 0) {
        return 0;
    }
    pts_path(path, ptn);
    slave = syscall3(SYS_OPEN, (long)path, O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
    bslave = syscall3(SYS_OPEN, (long)path, O_RDWR | O_NOCTTY, 0);
    return slave >= 0 && bslave >= 0 && change_flags(TIO_LFLAG, 0, ECHO) == 0 &&
           syscall3(SYS_IOCTL, master, TIOCPKT, (long)&packet) == 0 && master_empty();
}

/** The defaults row: IXON alone of the three flags, and ^Q and ^S. */
static int defaults(void)
{
    unsigned char tio[TIO_SIZE];
    unsigned int iflag;

    if (get_tio(tio) != 0) {
        return 0;
    }
    iflag = flag_at(tio, TIO_IFLAG);
    return (iflag & (IXON | IXANY | IXOFF)) == IXON && tio[TIO_CC + VSTART] == CTRL_Q &&
           tio[TIO_CC + VSTOP] == CTRL_S;
}

/** The stop row: ^S stops output, with TIOCPKT_STOP. */
static int stop(void)
{
    return type("\023", 1) && status_of() == TIOCPKT_STOP && slave_stopped();
}

/** The blocked-write row: the writer sleeps and the master reads nothing. */
static int blocked_write(void)
{
    return start_writer() && master_empty();
}

/** The start row: ^Q, TIOCPKT_START, the writer's end and its bytes. */
static int start(void)
{
    return type("\021", 1) && writer_done() && status_of() == TIOCPKT_START && data_is("late", 4);
}

/** The not-read row: "a^S^Qb\n" reads as "ab\n". */
static int not_read(void)
{
    return type("a\023\021b\n", 5) && status_of() == TIOCPKT_START && slave_reads("ab\n", 3);
}

/** The ixany row: under IXANY, 'z' after ^S restarts output and is read. */
static int ixany(void)
{
    return change_flags(TIO_IFLAG, IXANY, 0) == 0 && type("\023", 1) &&
           status_of() == TIOCPKT_STOP && slave_stopped() && type("z", 1) &&
           status_of() == TIOCPKT_START && slave_flows() && type("\n", 1) &&
           slave_reads("z\n", 2) && change_flags(TIO_IFLAG, 0, IXANY) == 0 && master_empty();
}

/** The tcooff row: TCOOFF's stop outlasts ^Q; TCOON ends it. */
static int tcooff(void)
{
    return syscall3(SYS_IOCTL, slave, TCXONC, TCOOFF) == 0 && status_of() == TIOCPKT_STOP &&
           slave_stopped() && type("\021", 1) && slave_stopped() && master_empty() &&
           syscall3(SYS_IOCTL, slave, TCXONC, TCOON) == 0 && status_of() == TIOCPKT_START &&
           slave_flows() && syscall3(SYS_IOCTL, slave, TCXONC, TCION + 1) == -EINVAL;
}

/** The tcioff row: TCIOFF and TCION reach the master as ^S and ^Q. */
static int tcioff(void)
{
    return syscall3(SYS_IOCTL, slave, TCXONC, TCIOFF) == 0 && data_is("\023", 1) &&
           syscall3(SYS_IOCTL, slave, TCXONC, TCION) == 0 && data_is("\021", 1) && master_empty();
}

/** The dostop row: IXON off gives TIOCPKT_NOSTOP and on TIOCPKT_DOSTOP. */
static int dostop(void)
{
    return change_flags(TIO_IFLAG, 0, IXON) == 0 && status_of() == TIOCPKT_NOSTOP &&
           change_flags(TIO_IFLAG, IXON, 0) == 0 && status_of() == TIOCPKT_DOSTOP && master_empty();
}

/** The signal-flush row: ^C discards "out" and "in", with both statuses. */
static int signal_flush(void)
{
    return syscall3(SYS_WRITE, slave, (long)"out", 3) == 3 && type("in", 2) && type("\003", 1) &&
           status_of() == (TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE) && flushed_out() &&
           type("\n", 1) && slave_reads("\n", 1);
}

/** The noflsh row: under NOFLSH ^C keeps "out" and "in", with no status. */
static int noflsh(void)
{
    return change_flags(TIO_LFLAG, NOFLSH, 0) == 0 &&
           syscall3(SYS_WRITE, slave, (long)"out", 3) == 3 && type("in", 2) && type("\003", 1) &&
           data_is("out", 3) && master_empty() && type("\n", 1) && slave_reads("in\n", 3);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * Run the rows in a child and report them. The child leads no session, so
 * reading the slave does not make it a controlling terminal, as Rach2 lets
 * a session leader's read claim an unowned slave and etude-guest starts the
 * probe as one. `_start` calls it with the initial stack.
 */
void probe_main(unsigned long *sp)
{
    static const char *const why[] = {
        "setup: the pair did not open, or the slave's echo did not go off or packet mode on",
        "defaults: the slave lacked IXON, ^Q or ^S, or had IXANY or IXOFF",
        "stop: ^S did not stop the output with TIOCPKT_STOP, EAGAIN and no POLLOUT",
        "blocked-write: the writer did not sleep, or its bytes reached the master",
        "start: ^Q did not restart the output with TIOCPKT_START and the writer's bytes",
        "not-read: ^S or ^Q reached the reader",
        "ixany: under IXANY a character did not restart output and reach the reader",
        "tcooff: TCOOFF and TCOON did not stop and restart output, ^Q ended TCOOFF's stop, or "
        "an unknown action was not EINVAL",
        "tcioff: TCIOFF and TCION did not send ^S and ^Q to the master",
        "dostop: IXON off and on did not give TIOCPKT_NOSTOP and TIOCPKT_DOSTOP",
        "signal-flush: ^C did not discard output and input with FLUSHREAD and FLUSHWRITE",
        "noflsh: under NOFLSH ^C flushed a queue or gave a status"};
    static int (*const rows[])(void) = {setup,  defaults, stop,         blocked_write,
                                        start,  not_read, ixany,        tcooff,
                                        tcioff, dostop,   signal_flush, noflsh};
    long pid;
    int i, status = -1;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.ttyflowprobe"));
    for (i = 0; i < (int)(sizeof(plan) / sizeof(plan[0])); i++) {
        report_plan(plan[i]);
    }
    pid = syscall0(SYS_FORK);
    if (pid < 0) {
        fail_at(0, "setup: the probe could not fork");
    }
    if (pid > 0) {
        while (syscall3(SYS_WAITPID, pid, (long)&status, 0) == -EINTR) {
        }
        if (!WIFEXITED(status)) {
            say("TTYFLOWPROBE FAIL: ", "the rows' process ended by a signal");
            report_stop("the rows' process ended by a signal");
        }
        leave(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
    }
    for (i = 0; i < (int)(sizeof(rows) / sizeof(rows[0])); i++) {
        if (!rows[i]()) {
            fail_at(i, why[i]);
        }
        report_held();
    }
    if (syscall1(SYS_CLOSE, bslave) != 0 || syscall1(SYS_CLOSE, slave) != 0 ||
        syscall1(SYS_CLOSE, master) != 0) {
        fail_at(i, "cleanup: a descriptor did not close");
    }
    report_held();
    report_end();
    say("TTYFLOWPROBE OK", "");
    leave(0);
}
