/**
 * loopprobe: the loopback, the probe's sockets reaching each other through
 * 127.0.0.1 with no ARP and no wire.
 *
 * The IP layer turns a datagram to 127/8 around through the loopback ring,
 * from 127.0.0.1. A datagram socket connected to 127.0.0.1 reports 127.0.0.1
 * as its own address. A UDP datagram sent to 127.0.0.1 at the socket's own
 * bound port comes back with the payload intact and the source 127.0.0.1, an
 * empty datagram comes back as one, and a recvfrom given an address and no
 * length pointer fails with EFAULT and leaves the datagram for the next
 * receive. A receive with MSG_PEEK leaves a datagram and a stream's bytes
 * for the next one. A TCP connection to the probe's own listener completes
 * its handshake, moves data both ways and reports the peer 127.0.0.1, and
 * one the server closes at once after accept still connects. A transfer
 * larger than the ring arrives whole with no datagram dropped, and no leg of
 * the round trips between two processes is left on the ring for the timer
 * while the machine idles. A listener bound to 127.0.0.1 is not reached at
 * the machine's interface address, a bind to an address the machine lacks
 * is refused, and a connect to a closed port is refused by the reset that
 * answers its SYN. The rows of TCP ports check a client's bound source port,
 * the one listener a port may have, ephemeral ports and a shutdown, and two
 * sockets that dial each other at once open one connection.
 *
 * A row that waits for a datagram or a connection blocks in the receive or
 * in poll(2) until it comes, and judges what came; the case's watchdog ends
 * one that never does.
 *
 * The probe uses UDP ports 7100 to 7102 and TCP ports 7200 to 7207 and 7300
 * to 7325, and 7300 is the closed port until its rows listen on it. A native
 * run maps them with `ports=OLD:NEW,...` (probe_args.h). With
 * `machine=native` the probe leaves out the two checks of counts that every
 * loopback datagram of the machine moves, no datagram dropped in the bulk
 * transfer and no round trip left for the timer by an idle machine.
 *
 * Prints "LOOPPROBE OK" and exits 0 on success.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, an assertion for each check, as `steps` lists them: the setup a
 * check needs, such as its sockets, is part of its assertion, and so is
 * every round of a loop that checks one claim. A failure fails the
 * assertion of its check and leaves those after it not run; one in a helper
 * shared by several checks, such as a read of /proc/stat, fails the first
 * assertion of the case it came in. Given `machine=native`, `bulk-no-drop`
 * and `pingpong-idle` are not run. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define STDOUT_FILENO 1

#define MSG_DONTWAIT 0x40
#define MSG_PEEK     0x2
#define EAGAIN       11
#define EFAULT       14

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

/* The ports, 7100 to 7102, 7200 to 7207 and 7300 to 7325, or the values
 * `ports=` gives in their place: `udp_port`, `nolen_port` for the recvfrom
 * given no length pointer, `peek_port` for the peek rows' datagram and
 * connection, `tcp_port[N]` for 7200 + N, the bound-port rows' two of
 * `tcp_ports`, and the simultaneous-open case's SIM_PAIRS pairs, taken in
 * turn, a pair a round. `closed_port` is the bound-port rows' listener's;
 * `tcp_refused` runs before those rows, while nothing listens on it. */
#define SIM_PAIRS 8
static uint16_t udp_port, nolen_port, peek_port, tcp_port[8];
static uint16_t bound_listen_port, bound_client_port, closed_port;
static uint16_t sim_port[2 * SIM_PAIRS];

/* The round trips of `tcp_loop_pingpong`, a byte each way. */
#define PINGPONG_ROUNDS 200

#define EADDRINUSE  98
#define EPIPE       32
#define ENOTCONN    107
#define EINPROGRESS 115

#define F_SETFL       4
#define O_NONBLOCK    04000
#define ECONNREFUSED  111
#define EADDRNOTAVAIL 99

/** write(2). */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** read(2). */
static int sys_read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)count);
}

#define O_RDONLY 0

/** open(2). */
static int sys_open(const char *path, int flags, int mode)
{
    return syscall3(SYS_OPEN, (long)path, (long)flags, (long)mode);
}

/** close(2). */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** The socket operation `call` on `args` (`sock_call`), as an int. */
static int socketcall(int call, unsigned long *args)
{
    return (int)sock_call(call, args);
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

/** Write the string `s` to standard output. */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

/** Write `v` in decimal to standard output. */
static void print_u32(uint32_t v)
{
    char d[12];
    int n = 0;

    if (v == 0) {
        print("0");
        return;
    }
    while (v > 0 && n < 12) {
        d[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        char one[2];

        one[0] = d[--n];
        one[1] = 0;
        print(one);
    }
}

/* The probe's checks in the order it makes them: the detail each failure
 * prints, and its assertion. Consecutive steps of one assertion are one
 * row. */
static const struct step {
    const char *label;
    const char *id;
} steps[] = {
    {"a socket to learn the local address", "loopback-address"},
    {"connecting to learn the local address", "loopback-address"},
    {"getsockname to learn the local address", "loopback-address"},
    {"a socket connected to 127.0.0.1 does not report 127.0.0.1", "loopback-address"},
    {"a socket to learn the local address", "interface-address"},
    {"connecting to learn the local address", "interface-address"},
    {"getsockname to learn the local address", "interface-address"},
    {"the local address read back as a loopback or a zero address", "interface-address"},
    {"udp socket", "udp-payload"},
    {"udp bind", "udp-payload"},
    {"udp sendto", "udp-payload"},
    {"udp payload", "udp-payload"},
    {"udp source not 127.0.0.1", "udp-source-address"},
    {"udp source port", "udp-source-port"},
    {"udp empty sendto", "udp-empty-payload"},
    {"udp empty payload", "udp-empty-payload"},
    {"udp empty source not 127.0.0.1", "udp-empty-source-address"},
    {"udp empty source port", "udp-empty-source-port"},
    {"udp sendto after empty", "udp-after-empty"},
    {"udp payload after empty", "udp-after-empty"},
    {"udp drained socket not empty", "udp-drained"},
    {"udp socket for the recvfrom with no length", "nolen-efault"},
    {"udp bind for the recvfrom with no length", "nolen-efault"},
    {"udp sendto for the recvfrom with no length", "nolen-efault"},
    {"a recvfrom with an address and no length was not EFAULT", "nolen-efault"},
    {"udp sendto after the recvfrom with no length", "nolen-datagram-kept"},
    {"a recvfrom refused with EFAULT took the datagram", "nolen-datagram-kept"},
    {"udp socket for the peek", "udp-peek-copies"},
    {"udp bind for the peek", "udp-peek-copies"},
    {"udp sendto for the peek", "udp-peek-copies"},
    {"a udp peek did not copy the datagram", "udp-peek-copies"},
    {"a second udp peek did not find the datagram and its sender", "udp-peek-again"},
    {"a udp receive after a peek did not take the datagram", "udp-receive-after-peek"},
    {"a udp receive after a peek left the datagram queued", "udp-peek-dequeued"},
    {"tcp sockets for the peek", "tcp-peek-copies"},
    {"tcp bind for the peek", "tcp-peek-copies"},
    {"tcp listen for the peek", "tcp-peek-copies"},
    {"tcp connect for the peek", "tcp-peek-copies"},
    {"tcp accept for the peek", "tcp-peek-copies"},
    {"tcp write for the peek", "tcp-peek-copies"},
    {"a tcp peek of one byte did not copy it", "tcp-peek-copies"},
    {"a tcp read after a peek did not start at the byte peeked", "tcp-read-after-peek"},
    {"tcp listener socket", "tcp-connect"},
    {"tcp bind", "tcp-connect"},
    {"tcp listen", "tcp-connect"},
    {"tcp client socket", "tcp-connect"},
    {"tcp connect", "tcp-connect"},
    {"tcp accept", "tcp-connect"},
    {"tcp client write", "tcp-client-to-server"},
    {"tcp server read", "tcp-client-to-server"},
    {"tcp server write", "tcp-server-to-client"},
    {"tcp client read", "tcp-server-to-client"},
    {"tcp getpeername", "tcp-peer-address"},
    {"tcp peer not 127.0.0.1", "tcp-peer-address"},
    {"tcp peer port", "tcp-peer-port"},
    {"immediate-close listener", "accept-close-connects"},
    {"immediate-close bind", "accept-close-connects"},
    {"immediate-close listen", "accept-close-connects"},
    {"immediate-close fork", "accept-close-connects"},
    {"immediate-close client", "accept-close-connects"},
    {"immediate-close connect", "accept-close-connects"},
    {"immediate-close eof", "accept-close-eof"},
    {"immediate-close child", "accept-close-child"},
    {"bulk listener socket", "bulk-connect"},
    {"bulk bind", "bulk-connect"},
    {"bulk listen", "bulk-connect"},
    {"bulk client socket", "bulk-connect"},
    {"bulk connect", "bulk-connect"},
    {"bulk accept", "bulk-connect"},
    {"bulk client non-blocking", "bulk-connect"},
    {"bulk server non-blocking", "bulk-connect"},
    {"a poll of the bulk transfer returned without a ready descriptor", "bulk-arrives"},
    {"the bulk connection reported an error", "bulk-arrives"},
    {"a bulk write that poll said had room wrote nothing", "bulk-arrives"},
    {"the bulk connection ended before the transfer arrived", "bulk-arrives"},
    {"bulk bytes came back wrong", "bulk-arrives"},
    {"a transfer larger than the loopback ring did not arrive whole", "bulk-arrives"},
    {"the loopback ring refused a datagram during the transfer", "bulk-no-drop"},
    {"the loopback ring was never pressed in this boot", "bulk-ring-pressed"},
    {"pipes for the round-trip calibration", "pipe-calibration"},
    {"fork for the round-trip calibration", "pipe-calibration"},
    {"writing to the child in the round-trip calibration", "pipe-calibration"},
    {"reading from the child in the round-trip calibration", "pipe-calibration"},
    {"the calibration child did not finish", "pipe-calibration"},
    {"ping-pong listener socket", "pingpong-connect"},
    {"ping-pong bind", "pingpong-connect"},
    {"ping-pong listen", "pingpong-connect"},
    {"ping-pong fork", "pingpong-connect"},
    {"ping-pong accept", "pingpong-connect"},
    {"ping-pong write", "pingpong-rounds"},
    {"ping-pong read", "pingpong-rounds"},
    {"the ping-pong child did not finish", "pingpong-child"},
    {"a local round trip waited for the timer with nothing running", "pingpong-idle"},
    {"bound listener socket", "bind-missing-address"},
    {"a bind to an address the machine lacks was not refused", "bind-missing-address"},
    {"bind to 127.0.0.1", "bound-listen"},
    {"listen on 127.0.0.1", "bound-listen"},
    {"bound client socket", "bound-not-at-interface"},
    {"a listener bound to 127.0.0.1 took a connection to the interface address",
     "bound-not-at-interface"},
    {"bound client socket", "bound-at-loopback"},
    {"connect to a listener bound to 127.0.0.1", "bound-at-loopback"},
    {"accept on 127.0.0.1", "bound-at-loopback"},
    {"refused socket", "closed-port-refused"},
    {"closed port not refused", "closed-port-refused"},
    {"bound-port listen socket", "second-bind-before-listen"},
    {"bound-port listen bind", "second-bind-before-listen"},
    {"second listener socket", "second-bind-before-listen"},
    {"a second bind before any listen was refused", "second-bind-before-listen"},
    {"bound-port listen", "second-listener-refused"},
    {"second listener on one port not refused", "second-listener-refused"},
    {"late bind socket", "live-port-bind-refused"},
    {"binding a live listener's port was not reported", "live-port-bind-refused"},
    {"unbound listener socket", "unbound-listener-ephemeral"},
    {"unbound listener refused", "unbound-listener-ephemeral"},
    {"unbound listener given no ephemeral port", "unbound-listener-ephemeral"},
    {"port 0 socket", "port-zero-ephemeral"},
    {"bind to port 0", "port-zero-ephemeral"},
    {"bind to port 0 given no ephemeral port", "port-zero-ephemeral"},
    {"restart socket", "restart-on-port"},
    {"a port a closed session still has could not be bound", "restart-on-port"},
    {"a server could not restart on its own last session's port", "restart-on-port"},
    {"bound-port client socket", "client-bound-port"},
    {"bound-port client bind", "client-bound-port"},
    {"bound-port connect", "client-bound-port"},
    {"bound-port accept", "client-bound-port"},
    {"the peer port is not the one the client bound", "client-bound-port"},
    {"duplicate pairing socket", "duplicate-pairing-refused"},
    {"duplicate pairing bind", "duplicate-pairing-refused"},
    {"a repeated local-remote pairing was not refused", "duplicate-pairing-refused"},
    {"fork for the shutdown wake", "shutdown-wakes-reader"},
    {"the reader was never seen asleep before the shutdown", "shutdown-wakes-reader"},
    {"shutdown under a sleeping reader", "shutdown-wakes-reader"},
    {"the reader did not end after the shutdown", "shutdown-wakes-reader"},
    {"a reader asleep through a shutdown did not read the end of input", "shutdown-wakes-reader"},
    {"shutdown of the client", "shutdown-epipe"},
    {"a send after shutdown is not EPIPE", "shutdown-epipe"},
    {"a blocking read after shutdown is not the end of input", "shutdown-blocking-eof"},
    {"nonblocking after shutdown", "shutdown-nonblocking-eof"},
    {"a nonblocking read after shutdown is not the end of input", "shutdown-nonblocking-eof"},
    {"fresh stream socket", "never-connected-enotconn"},
    {"nonblocking on the fresh socket", "never-connected-enotconn"},
    {"a read of a stream that never connected is not ENOTCONN", "never-connected-enotconn"},
    {"simultaneous sockets", "simultaneous-open"},
    {"simultaneous bind a", "simultaneous-open"},
    {"simultaneous bind b", "simultaneous-open"},
    {"simultaneous nonblock", "simultaneous-open"},
    {"simultaneous connect a", "simultaneous-open"},
    {"simultaneous connect b", "simultaneous-open"},
    {"crossing SYNs in flight at once opened no connection that carries data",
     "simultaneous-open"}};

#define STEPS ((int)(sizeof(steps) / sizeof(steps[0])))

/* The first step of the case running, the first step not yet reported, and
 * the process that writes the report. */
static int step_floor, step_next, report_pid;

/** Whether the strings `a` and `b` are equal. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** The first step of the row of step `i`. */
static int row_start(int i)
{
    while (i > 0 && same(steps[i - 1].id, steps[i].id)) {
        i--;
    }
    return i;
}

/** The step labelled `label` from `step_floor` on, or `step_floor` when no
 * step there has it. */
static int step_of(const char *label)
{
    int i;

    for (i = step_floor; i < STEPS; i++) {
        if (same(steps[i].label, label)) {
            return i;
        }
    }
    return step_floor;
}

/** Begin the steps of the row whose step `label` names: the case, or the
 * part of one, that runs next. */
static void rows_enter(const char *label)
{
    step_floor = row_start(step_of(label));
}

/**
 * Report the rows not yet reported: held before the row of step `failed`,
 * that row failed with `why`, and the rest not run, or every row held when
 * `failed` is STEPS; then the report's end. Given `machine=native`, the rows
 * of the machine's counts are not run.
 */
static void rows_report(int failed, const char *why)
{
    int row = failed < STEPS ? row_start(failed) : STEPS;
    int i;

    for (i = step_next; i < STEPS; i++) {
        if (i > 0 && same(steps[i - 1].id, steps[i].id)) {
            continue;
        }
        if (probe_native() &&
            (same(steps[i].id, "bulk-no-drop") || same(steps[i].id, "pingpong-idle"))) {
            report_notrun(steps[i].id, "machine=native leaves out the machine's loopback counts");
        } else if (i < row) {
            report_pass(steps[i].id);
        } else if (i == row) {
            report_fail(steps[i].id, why);
        } else {
            report_notrun(steps[i].id, "an earlier check failed");
        }
    }
    step_next = STEPS;
    report_end();
}

/** Print "LOOPPROBE FAIL (`what`)", report the row of the step `what` names
 * when this is the process that writes the report, and exit 1. */
static void fail(const char *what)
{
    print("LOOPPROBE FAIL (");
    print(what);
    print(")\n");
    if (syscall0(SYS_GETPID) == report_pid) {
        rows_report(step_of(what), what);
    }
    sys_exit(1);
}

/** Whether the `n` bytes at `a` and `b` are the same. */
static int mem_eq(const uint8_t *a, const uint8_t *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

/** Fill a 16-byte sockaddr_in: family, big-endian port, big-endian IPv4. */
static void make_sockaddr(uint8_t *sa, uint16_t port, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    int i;
    for (i = 0; i < 16; i++)
        sa[i] = 0;
    sa[0] = AF_INET;
    sa[1] = 0;
    sa[2] = (uint8_t)(port >> 8);
    sa[3] = (uint8_t)port;
    sa[4] = a;
    sa[5] = b;
    sa[6] = c;
    sa[7] = d;
}

/* The address every loopback peer reads back as, and the machine's own
 * interface address, which `learn_addresses` reads from the kernel rather
 * than assuming the emulator's. */
static const uint8_t loop_ip[4] = {127, 0, 0, 1};
static uint8_t own_ip[4];

/** The 16-byte sockaddr_in `getsockname` reports for a datagram socket
 * connected to a.b.c.d, into `me`, which is cleared first. Fails the probe
 * when a call fails. */
static void source_for(uint8_t *me, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    unsigned long args[6];
    uint8_t sa[16];
    int len = 16;
    int fd;
    int i;

    for (i = 0; i < 16; i++) {
        me[i] = 0;
    }

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0)
        fail("a socket to learn the local address");
    make_sockaddr(sa, 9, a, b, c, d);
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0)
        fail("connecting to learn the local address");
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)me;
    args[2] = (unsigned long)&len;
    if (socketcall(SC_GETSOCKNAME, args) != 0)
        fail("getsockname to learn the local address");
    sys_close(fd);
}

/** A datagram socket connected to 127.0.0.1 leaves from 127.0.0.1, and one
 * connected to a documentation address, 192.0.2.1, from the machine's own,
 * which goes into `own_ip`; a loopback or zero address there fails. */
static void learn_addresses(void)
{
    uint8_t me[16];

    source_for(me, 127, 0, 0, 1);
    if (!mem_eq(me + 4, loop_ip, 4))
        fail("a socket connected to 127.0.0.1 does not report 127.0.0.1");
    rows_enter("the local address read back as a loopback or a zero address");
    source_for(me, 192, 0, 2, 1);
    own_ip[0] = me[4];
    own_ip[1] = me[5];
    own_ip[2] = me[6];
    own_ip[3] = me[7];
    if (own_ip[0] == 127 || (own_ip[0] | own_ip[1] | own_ip[2] | own_ip[3]) == 0) {
        fail("the local address read back as a loopback or a zero address");
    }
}

/** The local port `getsockname` reports for `fd`, or -1 when it fails. The
 * address is cleared first, so that a call that writes nothing reads as 0. */
static int local_port(int fd)
{
    unsigned long args[6];
    uint8_t me[16];
    int len = 16;
    int i;

    for (i = 0; i < 16; i++) {
        me[i] = 0;
    }
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)me;
    args[2] = (unsigned long)&len;
    if (socketcall(SC_GETSOCKNAME, args) != 0)
        return -1;
    return me[2] * 256 + me[3];
}

/** pipe(2) into `fds`, which the kernel writes and the call's "memory"
 * clobber covers. */
static int sys_pipe(int *fds)
{
    return syscall1(SYS_PIPE, (long)fds);
}

/** fork(2). */
static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

/** waitpid(2) for `pid`, with no options. */
static int sys_waitpid(int pid, int *status)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)0);
}

/** The milliseconds from `a` to `b`, both as `gettimeofday` fills them. The
 * microseconds are subtracted as signed values, so a second that borrows
 * does not wrap through zero and report four billion. */
static int elapsed_ms(const long *a, const long *b)
{
    int sec = (int)(b[0] - a[0]);
    int usec = (int)b[1] - (int)a[1];

    return sec * 1000 + usec / 1000;
}

/** gettimeofday(2) into `tv`, seconds and microseconds, with no zone. */
static int sys_gettimeofday(long *tv)
{
    return syscall2(SYS_GETTIMEOFDAY, (long)tv, (long)0);
}

/** fcntl(2). */
static int sys_fcntl(int fd, int cmd, int arg)
{
    return syscall3(SYS_FCNTL, (long)fd, (long)cmd, (long)arg);
}

#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLERR 0x008

struct pollfd_probe {
    int fd;
    short events;
    short revents;
};

/** poll(2) with no timeout: blocks until a descriptor of `fds` is ready, and
 * returns how many are, or a negated errno. */
static int sys_poll_wait(struct pollfd_probe *fds, unsigned int n)
{
    return syscall3(SYS_POLL, (long)fds, (long)n, (long)-1);
}

/** Block until `fd` reports one of `events`, or an error or hangup, which
 * poll reports whether asked for or not; the events it reported. Fails the
 * probe when poll fails. */
static short wait_for(int fd, short events)
{
    struct pollfd_probe pfd;

    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    if (sys_poll_wait(&pfd, 1) != 1)
        fail("a poll with no timeout returned without a ready descriptor");
    return pfd.revents;
}

struct timespec_probe {
    long tv_sec;
    long tv_nsec;
};

/** Sleep for `ms` milliseconds (nanosleep(2)). */
static void sys_nanosleep_ms(long ms)
{
    struct timespec_probe req;
    int r;

    req.tv_sec = ms / 1000L;
    req.tv_nsec = (ms % 1000L) * 1000000L;
    /* interval: the reader's state is read again every 10 ms */
    r = syscall2(SYS_NANOSLEEP, (long)(&req), (long)0);
    (void)r;
}

/** The state letter of process `pid`, the field after the name in
 * /proc/<pid>/stat, or 0 when it cannot be read. */
static char state_of(int pid)
{
    char path[32] = "/proc/";
    char digits[12], line[256];
    int i = 0, j = 6, fd, n;

    do {
        digits[i++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0);
    while (i > 0) {
        path[j++] = digits[--i];
    }
    for (i = 0; "/stat"[i] != '\0'; i++) {
        path[j++] = "/stat"[i];
    }
    path[j] = '\0';
    fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    n = sys_read(fd, line, sizeof(line));
    sys_close(fd);
    for (i = n - 1; i >= 0 && line[i] != ')'; i--) {
    }
    return i >= 0 && i + 2 < n && line[i + 1] == ' ' ? line[i + 2] : 0;
}

/**
 * A UDP datagram sent to 127.0.0.1 at the socket's own bound port comes
 * back with its payload and the source 127.0.0.1:udp_port. A datagram of no
 * bytes comes back as one, with its source, and the datagram after it
 * arrives whole; the drained socket then answers EAGAIN. A receive that took
 * the empty datagram for an empty queue would block, and the runner reports
 * a probe that never ends; the EAGAIN fails a receive that reports a
 * datagram whether or not one is queued, which passes the rows before it.
 */
static void udp_loop(void)
{
    unsigned long args[6];
    uint8_t sa[16], dst[16], src[16];
    uint32_t salen = 16;
    char rbuf[32];
    const char *msg = "loopback-udp";
    int s, n;

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    s = socketcall(SC_SOCKET, args);
    if (s < 0)
        fail("udp socket");

    make_sockaddr(sa, udp_port, 0, 0, 0, 0);
    args[0] = (uint32_t)s;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("udp bind");

    make_sockaddr(dst, udp_port, 127, 0, 0, 1);
    args[0] = (uint32_t)s;
    args[1] = (unsigned long)msg;
    args[2] = (uint32_t)strlen(msg);
    args[3] = 0;
    args[4] = (unsigned long)dst;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != (int)strlen(msg))
        fail("udp sendto");

    args[0] = (uint32_t)s;
    args[1] = (unsigned long)rbuf;
    args[2] = sizeof(rbuf);
    args[3] = 0;
    args[4] = (unsigned long)src;
    args[5] = (unsigned long)&salen;
    n = socketcall(SC_RECVFROM, args);
    if (n != (int)strlen(msg) || !mem_eq((const uint8_t *)rbuf, (const uint8_t *)msg, n)) {
        fail("udp payload");
    }
    if (!mem_eq(src + 4, loop_ip, 4))
        fail("udp source not 127.0.0.1");
    if (src[2] != (udp_port >> 8) || src[3] != (udp_port & 0xFF)) {
        fail("udp source port");
    }

    /* A datagram of no bytes. */
    args[0] = (uint32_t)s;
    args[1] = (unsigned long)msg;
    args[2] = 0;
    args[3] = 0;
    args[4] = (unsigned long)dst;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 0)
        fail("udp empty sendto");

    args[0] = (uint32_t)s;
    args[1] = (unsigned long)rbuf;
    args[2] = sizeof(rbuf);
    args[3] = 0;
    args[4] = (unsigned long)src;
    args[5] = (unsigned long)&salen;
    salen = 16;
    n = socketcall(SC_RECVFROM, args);
    if (n != 0)
        fail("udp empty payload");
    if (!mem_eq(src + 4, loop_ip, 4))
        fail("udp empty source not 127.0.0.1");
    if (src[2] != (udp_port >> 8) || src[3] != (udp_port & 0xFF)) {
        fail("udp empty source port");
    }

    /* The receive took the empty datagram: the next arrives whole. */
    args[0] = (uint32_t)s;
    args[1] = (unsigned long)msg;
    args[2] = (uint32_t)strlen(msg);
    args[3] = 0;
    args[4] = (unsigned long)dst;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != (int)strlen(msg))
        fail("udp sendto after empty");

    args[0] = (uint32_t)s;
    args[1] = (unsigned long)rbuf;
    args[2] = sizeof(rbuf);
    args[3] = 0;
    args[4] = (unsigned long)src;
    args[5] = (unsigned long)&salen;
    salen = 16;
    n = socketcall(SC_RECVFROM, args);
    if (n != (int)strlen(msg) || !mem_eq((const uint8_t *)rbuf, (const uint8_t *)msg, n)) {
        fail("udp payload after empty");
    }

    /* The socket is empty. */
    args[0] = (uint32_t)s;
    args[1] = (unsigned long)rbuf;
    args[2] = sizeof(rbuf);
    args[3] = MSG_DONTWAIT;
    args[4] = (unsigned long)src;
    args[5] = (unsigned long)&salen;
    salen = 16;
    n = socketcall(SC_RECVFROM, args);
    if (n != -EAGAIN)
        fail("udp drained socket not empty");

    sys_close(s);
}

/** A recvfrom given an address and no length pointer fails with EFAULT
 * before it takes the datagram, which the next recvfrom takes whole. A
 * second datagram follows the refused call, and the ring and the socket's
 * queue keep their order, so the blocking receive after it takes the first
 * if it is still queued and the second if the refused call took it. */
static void udp_no_length(void)
{
    unsigned long args[6];
    uint8_t sa[16], src[16];
    uint32_t salen = 16;
    char rbuf[32];
    const char *msg = "no-length";
    const char *after = "after";
    int s, n;

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    s = socketcall(SC_SOCKET, args);
    if (s < 0)
        fail("udp socket for the recvfrom with no length");
    make_sockaddr(sa, nolen_port, 0, 0, 0, 0);
    args[0] = (uint32_t)s;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("udp bind for the recvfrom with no length");
    make_sockaddr(sa, nolen_port, 127, 0, 0, 1);
    args[0] = (uint32_t)s;
    args[1] = (unsigned long)msg;
    args[2] = (uint32_t)strlen(msg);
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != (int)strlen(msg))
        fail("udp sendto for the recvfrom with no length");

    args[0] = (uint32_t)s;
    args[1] = (unsigned long)rbuf;
    args[2] = sizeof(rbuf);
    args[3] = 0;
    args[4] = (unsigned long)src;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != -EFAULT)
        fail("a recvfrom with an address and no length was not EFAULT");

    args[0] = (uint32_t)s;
    args[1] = (unsigned long)after;
    args[2] = (uint32_t)strlen(after);
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != (int)strlen(after))
        fail("udp sendto after the recvfrom with no length");

    args[0] = (uint32_t)s;
    args[1] = (unsigned long)rbuf;
    args[2] = sizeof(rbuf);
    args[3] = 0;
    args[4] = (unsigned long)src;
    args[5] = (unsigned long)&salen;
    n = socketcall(SC_RECVFROM, args);
    if (n != (int)strlen(msg) || !mem_eq((const uint8_t *)rbuf, (const uint8_t *)msg, n))
        fail("a recvfrom refused with EFAULT took the datagram");
    sys_close(s);
}

/** recvfrom on `s` into `buf` with `flags`, no address asked for, blocking
 * until a datagram or a stream's bytes arrive: the bytes received, or a
 * negated errno. */
static int recv_wait(int s, char *buf, int len, int flags)
{
    unsigned long args[6];

    args[0] = (uint32_t)s;
    args[1] = (unsigned long)buf;
    args[2] = (uint32_t)len;
    args[3] = (uint32_t)flags;
    args[4] = 0;
    args[5] = 0;
    return socketcall(SC_RECVFROM, args);
}

/** A datagram peeked at with MSG_PEEK stays queued, whole and with its
 * sender, for the next receive, which takes it. */
static void udp_peek(void)
{
    unsigned long args[6];
    uint8_t sa[16], src[16];
    uint32_t salen = 16;
    char rbuf[32];
    const char *msg = "peek-udp";
    int s, n;

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    s = socketcall(SC_SOCKET, args);
    if (s < 0)
        fail("udp socket for the peek");
    make_sockaddr(sa, peek_port, 0, 0, 0, 0);
    args[0] = (uint32_t)s;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("udp bind for the peek");
    make_sockaddr(sa, peek_port, 127, 0, 0, 1);
    args[0] = (uint32_t)s;
    args[1] = (unsigned long)msg;
    args[2] = (uint32_t)strlen(msg);
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != (int)strlen(msg))
        fail("udp sendto for the peek");
    if (recv_wait(s, rbuf, sizeof(rbuf), MSG_PEEK) != (int)strlen(msg))
        fail("a udp peek did not copy the datagram");

    args[0] = (uint32_t)s;
    args[1] = (unsigned long)rbuf;
    args[2] = sizeof(rbuf);
    args[3] = MSG_PEEK | MSG_DONTWAIT;
    args[4] = (unsigned long)src;
    args[5] = (unsigned long)&salen;
    n = socketcall(SC_RECVFROM, args);
    if (n != (int)strlen(msg) || !mem_eq((const uint8_t *)rbuf, (const uint8_t *)msg, n) ||
        src[2] != (peek_port >> 8) || src[3] != (peek_port & 0xFF))
        fail("a second udp peek did not find the datagram and its sender");

    args[3] = MSG_DONTWAIT;
    n = socketcall(SC_RECVFROM, args);
    if (n != (int)strlen(msg) || !mem_eq((const uint8_t *)rbuf, (const uint8_t *)msg, n))
        fail("a udp receive after a peek did not take the datagram");
    if (socketcall(SC_RECVFROM, args) != -EAGAIN)
        fail("a udp receive after a peek left the datagram queued");
    sys_close(s);
}

/** A stream's bytes peeked at with MSG_PEEK stay in its ring: one byte
 * peeked, as Chromium checks a connection it keeps idle, and the read after
 * it gets the whole message from its first byte. */
static void tcp_peek(void)
{
    unsigned long args[6];
    uint8_t sa[16];
    char rbuf[32];
    const char *msg = "peek-tcp";
    int ls, cs, as, n;

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    ls = socketcall(SC_SOCKET, args);
    cs = socketcall(SC_SOCKET, args);
    if (ls < 0 || cs < 0)
        fail("tcp sockets for the peek");
    make_sockaddr(sa, peek_port, 0, 0, 0, 0);
    args[0] = (uint32_t)ls;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("tcp bind for the peek");
    args[1] = 1;
    if (socketcall(SC_LISTEN, args) != 0)
        fail("tcp listen for the peek");
    make_sockaddr(sa, peek_port, 127, 0, 0, 1);
    args[0] = (uint32_t)cs;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0)
        fail("tcp connect for the peek");
    args[0] = (uint32_t)ls;
    args[1] = 0;
    args[2] = 0;
    as = socketcall(SC_ACCEPT, args);
    if (as < 0)
        fail("tcp accept for the peek");
    if (sys_write(as, msg, strlen(msg)) != (int)strlen(msg))
        fail("tcp write for the peek");

    n = recv_wait(cs, rbuf, 1, MSG_PEEK);
    if (n != 1 || rbuf[0] != msg[0])
        fail("a tcp peek of one byte did not copy it");
    n = sys_read(cs, rbuf, sizeof(rbuf));
    if (n != (int)strlen(msg) || !mem_eq((const uint8_t *)rbuf, (const uint8_t *)msg, n))
        fail("a tcp read after a peek did not start at the byte peeked");
    sys_close(cs);
    sys_close(as);
    sys_close(ls);
}

/** A TCP connection to the probe's own listener on 127.0.0.1 completes its
 * handshake, carries a message each way, and reports its peer as
 * 127.0.0.1:tcp_port[0]. */
static void tcp_loop(void)
{
    unsigned long args[6];
    uint8_t sa[16], peer[16];
    uint32_t salen = 16;
    char rbuf[32];
    const char *ping = "ping-over-loop";
    const char *pong = "pong";
    int ls, cs, as, n;

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    ls = socketcall(SC_SOCKET, args);
    if (ls < 0)
        fail("tcp listener socket");

    make_sockaddr(sa, tcp_port[0], 0, 0, 0, 0);
    args[0] = (uint32_t)ls;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("tcp bind");

    args[0] = (uint32_t)ls;
    args[1] = 2;
    if (socketcall(SC_LISTEN, args) != 0)
        fail("tcp listen");

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    cs = socketcall(SC_SOCKET, args);
    if (cs < 0)
        fail("tcp client socket");

    make_sockaddr(sa, tcp_port[0], 127, 0, 0, 1);
    args[0] = (uint32_t)cs;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0)
        fail("tcp connect");

    args[0] = (uint32_t)ls;
    args[1] = 0;
    args[2] = 0;
    as = socketcall(SC_ACCEPT, args);
    if (as < 0)
        fail("tcp accept");

    if (sys_write(cs, ping, strlen(ping)) != (int)strlen(ping)) {
        fail("tcp client write");
    }
    n = sys_read(as, rbuf, sizeof(rbuf));
    if (n != (int)strlen(ping) || !mem_eq((const uint8_t *)rbuf, (const uint8_t *)ping, n)) {
        fail("tcp server read");
    }

    if (sys_write(as, pong, strlen(pong)) != (int)strlen(pong)) {
        fail("tcp server write");
    }
    n = sys_read(cs, rbuf, sizeof(rbuf));
    if (n != (int)strlen(pong) || !mem_eq((const uint8_t *)rbuf, (const uint8_t *)pong, n)) {
        fail("tcp client read");
    }

    args[0] = (uint32_t)cs;
    args[1] = (unsigned long)peer;
    args[2] = (unsigned long)&salen;
    if (socketcall(SC_GETPEERNAME, args) != 0)
        fail("tcp getpeername");
    if (!mem_eq(peer + 4, loop_ip, 4))
        fail("tcp peer not 127.0.0.1");
    if (peer[2] != (tcp_port[0] >> 8) || peer[3] != (tcp_port[0] & 0xFF)) {
        fail("tcp peer port");
    }

    sys_close(cs);
    sys_close(as);
    sys_close(ls);
}

/* The bytes `tcp_loop_bulk` sends, more than the loopback ring holds. */
#define BULK_BYTES      (256u * 1024u)
/* The least the ring's high-water mark must reach, so that the transfer is
 * known to have pressed the ring. */
#define LOOP_PEAK_FLOOR 8u

/** The counter `key` of /proc/stat, `keylen` bytes with its trailing space,
 * read as an unsigned decimal; fails the probe when the counter is missing.
 * The file is read to its end, since it has a line for each processor before
 * the network's counters: 64 of them pass 2 KiB. */
static uint32_t proc_stat_u32(const char *key, int keylen)
{
    int fd = sys_open("/proc/stat", O_RDONLY, 0);
    static char buf[32768];
    uint32_t v = 0;
    int n = 0, got, i;

    if (fd < 0) {
        fail("opening /proc/stat");
    }
    while ((got = sys_read(fd, buf + n, (int)sizeof(buf) - 1 - n)) > 0) {
        n += got;
    }
    sys_close(fd);
    if (got < 0 || n <= 0) {
        fail("reading /proc/stat");
    }
    if (n == (int)sizeof(buf) - 1) {
        fail("/proc/stat is longer than the probe's buffer");
    }
    buf[n] = 0;
    for (i = 0; i + keylen < n; i++) {
        if ((i == 0 || buf[i - 1] == '\n') &&
            mem_eq((const uint8_t *)(buf + i), (const uint8_t *)key, keylen)) {
            int k = i + keylen;

            while (buf[k] >= '0' && buf[k] <= '9') {
                v = v * 10u + (uint32_t)(buf[k] - '0');
                k++;
            }
            return v;
        }
    }
    fail("/proc/stat has no such counter");
    return 0;
}

/** The datagrams the loopback ring has refused since boot. */
static uint32_t loop_dropped_now(void)
{
    return proc_stat_u32("loopback_dropped ", 17);
}

/** The most datagrams the loopback ring has held at once since boot. */
static uint32_t loop_peak_now(void)
{
    return proc_stat_u32("loopback_peak ", 14);
}

/** The byte the stream carries at offset `n`. Every eight-bit window of the
 * offset reaches the result, so two blocks of the buffer differ wherever
 * their offsets do, and a replayed buffer or a copy from the wrong place in
 * the ring does not compare equal. */
static char stream_byte(uint32_t n)
{
    return (char)((n * 7u + (n >> 8) * 31u + (n >> 16) * 131u + 1u) & 0xFFu);
}

/**
 * Send BULK_BYTES of the stream from `cs` and receive them at `as`, the two
 * ends of one connection, both non-blocking, within this process. It waits
 * in poll for either: the client writes when its ring has room and the
 * server reads what has arrived, so the two take turns, and every byte must
 * arrive in order. The wait in poll is also what delivers the loopback ring
 * when no system call's exit has, since a blocked process's idle wait
 * drains it. Fails the probe on a wrong byte, an error or an early end.
 */
static void bulk_transfer(int cs, int as)
{
    static char out[65536], in[16384];
    struct pollfd_probe pfd[2];
    uint32_t sent = 0, got = 0;
    int n, i;

    for (i = 0; i < (int)sizeof(out); i++) {
        out[i] = stream_byte((uint32_t)i);
    }
    while (got < BULK_BYTES) {
        pfd[0].fd = as;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = cs;
        pfd[1].events = sent < BULK_BYTES ? POLLOUT : 0;
        pfd[1].revents = 0;
        if (sys_poll_wait(pfd, 2) <= 0)
            fail("a poll of the bulk transfer returned without a ready descriptor");
        if ((pfd[0].revents | pfd[1].revents) & POLLERR)
            fail("the bulk connection reported an error");
        if (sent < BULK_BYTES && (pfd[1].revents & POLLOUT)) {
            /* A short write is ordinary; the next starts where it ended. */
            uint32_t at = sent % sizeof(out);
            uint32_t want = (uint32_t)sizeof(out) - at;

            if (want > BULK_BYTES - sent)
                want = BULK_BYTES - sent;
            n = sys_write(cs, out + at, (int)want);
            if (n <= 0)
                fail("a bulk write that poll said had room wrote nothing");
            sent += (uint32_t)n;
        }
        if (pfd[0].revents & POLLIN) {
            n = sys_read(as, in, (int)sizeof(in));
            if (n <= 0)
                fail("the bulk connection ended before the transfer arrived");
            for (i = 0; i < n; i++) {
                if (in[i] != stream_byte((got + (uint32_t)i) % (uint32_t)sizeof(out))) {
                    fail("bulk bytes came back wrong");
                }
            }
            got += (uint32_t)n;
        }
    }
    if (got != BULK_BYTES) {
        fail("a transfer larger than the loopback ring did not arrive whole");
    }
}

/**
 * More bytes than the loopback ring holds, BULK_BYTES, through the ring
 * within one process, as `bulk_transfer` moves them, and the kernel's count
 * of datagrams the ring refused must not rise.
 *
 * A write offers up to 64 KiB, so that a call puts the most it can in the
 * ring: a window's worth, 65535 bytes, is 45 segments of 1460 bytes, of the
 * ring's 64 slots, and a system call's exit delivers the ring once it holds
 * three or more. The ring's high-water mark is kept since boot, so it can
 * show that the ring was pressed but not that this transfer pressed it: the
 * row requires LOOP_PEAK_FLOOR of it and prints it.
 */
static void tcp_loop_bulk(void)
{
    unsigned long args[6];
    uint8_t sa[16];
    uint32_t dropped_before, dropped_after;
    int ls, cs, as;

    dropped_before = loop_dropped_now();

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    ls = socketcall(SC_SOCKET, args);
    if (ls < 0)
        fail("bulk listener socket");
    make_sockaddr(sa, tcp_port[3], 0, 0, 0, 0);
    args[0] = (uint32_t)ls;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("bulk bind");
    args[0] = (uint32_t)ls;
    args[1] = 2;
    if (socketcall(SC_LISTEN, args) != 0)
        fail("bulk listen");

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    cs = socketcall(SC_SOCKET, args);
    if (cs < 0)
        fail("bulk client socket");
    make_sockaddr(sa, tcp_port[3], 127, 0, 0, 1);
    args[0] = (uint32_t)cs;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0)
        fail("bulk connect");

    args[0] = (uint32_t)ls;
    args[1] = 0;
    args[2] = 0;
    as = socketcall(SC_ACCEPT, args);
    if (as < 0)
        fail("bulk accept");

    if (sys_fcntl(cs, F_SETFL, O_NONBLOCK) != 0)
        fail("bulk client non-blocking");
    if (sys_fcntl(as, F_SETFL, O_NONBLOCK) != 0)
        fail("bulk server non-blocking");

    bulk_transfer(cs, as);
    dropped_after = loop_dropped_now();
    if (!probe_native() && dropped_after != dropped_before) {
        fail("the loopback ring refused a datagram during the transfer");
    }
    if (loop_peak_now() < LOOP_PEAK_FLOOR) {
        fail("the loopback ring was never pressed in this boot");
    }
    print("LOOPPROBE ring peak ");
    print_u32(loop_peak_now());
    print(" of 64 since boot\n");
    sys_close(cs);
    sys_close(as);
    sys_close(ls);
}

/** The milliseconds PINGPONG_ROUNDS round trips of a byte each way take
 * between this process and a child over two pipes, the same rounds as the
 * loopback's without a ring. */
static int pipe_pingpong_ms(void)
{
    int up[2], down[2];
    long t0[2], t1[2];
    char one = 'p';
    int pid, status = 0, round;

    if (sys_pipe(up) != 0 || sys_pipe(down) != 0)
        fail("pipes for the round-trip calibration");
    pid = sys_fork();
    if (pid < 0)
        fail("fork for the round-trip calibration");
    if (pid == 0) {
        sys_close(up[0]);
        sys_close(down[1]);
        for (round = 0; round < PINGPONG_ROUNDS; round++) {
            if (sys_read(down[0], &one, 1) != 1)
                sys_exit(1);
            if (sys_write(up[1], &one, 1) != 1)
                sys_exit(1);
        }
        sys_exit(0);
    }
    sys_close(up[1]);
    sys_close(down[0]);
    sys_gettimeofday(t0);
    for (round = 0; round < PINGPONG_ROUNDS; round++) {
        if (sys_write(down[1], &one, 1) != 1)
            fail("writing to the child in the round-trip calibration");
        if (sys_read(up[0], &one, 1) != 1)
            fail("reading from the child in the round-trip calibration");
    }
    sys_gettimeofday(t1);
    sys_waitpid(pid, &status);
    if (status != 0)
        fail("the calibration child did not finish");
    sys_close(up[0]);
    sys_close(down[1]);
    return elapsed_ms(t0, t1);
}

/**
 * PINGPONG_ROUNDS round trips of a byte each way between this process and a
 * child over a loopback connection, judged leg by leg by the kernel's count
 * of the datagrams a tick found on the ring while every processor was idle
 * (`loopback_left_idle` in /proc/stat).
 *
 * Each leg queues one datagram, below the three at which a system call's exit
 * delivers the ring, and the sender then blocks in its read with nothing else
 * runnable, so the processor goes to its idle wait, which delivers the ring
 * before it halts. A tick may still deliver a leg first, when it comes
 * between the write and the idle wait, and a slower processor makes that
 * likelier; the count does not see those, since the processor was running.
 * It sees a leg the idle wait left on the ring and halted over, which then
 * waited for the timer: a connection that does turns over one window a tick.
 * No leg may, so the count must not rise. The legs each path delivered and
 * the times of these rounds and of the same rounds over pipes are printed for
 * a person and not judged.
 */
static void tcp_loop_pingpong(void)
{
    unsigned long args[6];
    uint8_t sa[16];
    long t0[2], t1[2];
    char one = 'p';
    int ls, cs, as, pid, status = 0, round, ms, base;
    uint32_t idle, tick, left;

    base = pipe_pingpong_ms();

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    ls = socketcall(SC_SOCKET, args);
    if (ls < 0)
        fail("ping-pong listener socket");
    make_sockaddr(sa, tcp_port[5], 0, 0, 0, 0);
    args[0] = (uint32_t)ls;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("ping-pong bind");
    args[0] = (uint32_t)ls;
    args[1] = 2;
    if (socketcall(SC_LISTEN, args) != 0)
        fail("ping-pong listen");

    pid = sys_fork();
    if (pid < 0)
        fail("ping-pong fork");
    if (pid == 0) {
        args[0] = AF_INET;
        args[1] = SOCK_STREAM;
        args[2] = 0;
        cs = socketcall(SC_SOCKET, args);
        if (cs < 0)
            sys_exit(1);
        make_sockaddr(sa, tcp_port[5], 127, 0, 0, 1);
        args[0] = (uint32_t)cs;
        args[1] = (unsigned long)sa;
        args[2] = 16;
        if (socketcall(SC_CONNECT, args) != 0)
            sys_exit(1);
        for (round = 0; round < PINGPONG_ROUNDS; round++) {
            if (sys_read(cs, &one, 1) != 1)
                sys_exit(1);
            if (sys_write(cs, &one, 1) != 1)
                sys_exit(1);
        }
        sys_close(cs);
        sys_exit(0);
    }

    args[0] = (uint32_t)ls;
    args[1] = 0;
    args[2] = 0;
    as = socketcall(SC_ACCEPT, args);
    if (as < 0)
        fail("ping-pong accept");
    idle = proc_stat_u32("loopback_delivered_idle ", 24);
    tick = proc_stat_u32("loopback_delivered_tick ", 24);
    left = proc_stat_u32("loopback_left_idle ", 19);
    sys_gettimeofday(t0);
    for (round = 0; round < PINGPONG_ROUNDS; round++) {
        if (sys_write(as, &one, 1) != 1)
            fail("ping-pong write");
        if (sys_read(as, &one, 1) != 1)
            fail("ping-pong read");
    }
    sys_gettimeofday(t1);
    sys_waitpid(pid, &status);
    if (status != 0)
        fail("the ping-pong child did not finish");
    sys_close(as);
    sys_close(ls);
    ms = elapsed_ms(t0, t1);
    idle = proc_stat_u32("loopback_delivered_idle ", 24) - idle;
    tick = proc_stat_u32("loopback_delivered_tick ", 24) - tick;
    left = proc_stat_u32("loopback_left_idle ", 19) - left;
    print("LOOPPROBE ping-pong ");
    print_u32((uint32_t)ms);
    print(" ms over the loopback, ");
    print_u32((uint32_t)base);
    print(" over a pipe, for 200 round trips; delivered by the idle path ");
    print_u32(idle);
    print(", by the timer ");
    print_u32(tick);
    print(", left for the timer by an idle machine ");
    print_u32(left);
    print("\n");
    if (!probe_native() && left != 0) {
        fail("a local round trip waited for the timer with nothing running");
    }
}

/** A listener bound to 127.0.0.1 is reached at that address alone: a
 * connect to the machine's interface address on its port is refused, as one
 * from the wire would be, and a connect to 127.0.0.1 is accepted. A bind to
 * an address the machine does not have is EADDRNOTAVAIL. */
static void tcp_bound_loopback(void)
{
    unsigned long args[6];
    uint8_t sa[16];
    int ls, cs, as;

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    ls = socketcall(SC_SOCKET, args);
    if (ls < 0)
        fail("bound listener socket");
    make_sockaddr(sa, tcp_port[7], 192, 0, 2, 1);
    args[0] = (uint32_t)ls;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != -EADDRNOTAVAIL)
        fail("a bind to an address the machine lacks was not refused");
    make_sockaddr(sa, tcp_port[7], 127, 0, 0, 1);
    if (socketcall(SC_BIND, args) != 0)
        fail("bind to 127.0.0.1");
    args[0] = (uint32_t)ls;
    args[1] = 2;
    if (socketcall(SC_LISTEN, args) != 0)
        fail("listen on 127.0.0.1");

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    cs = socketcall(SC_SOCKET, args);
    if (cs < 0)
        fail("bound client socket");
    make_sockaddr(sa, tcp_port[7], own_ip[0], own_ip[1], own_ip[2], own_ip[3]);
    args[0] = (uint32_t)cs;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != -ECONNREFUSED)
        fail("a listener bound to 127.0.0.1 took a connection to the interface address");
    sys_close(cs);
    rows_enter("connect to a listener bound to 127.0.0.1");

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    cs = socketcall(SC_SOCKET, args);
    if (cs < 0)
        fail("bound client socket");
    make_sockaddr(sa, tcp_port[7], 127, 0, 0, 1);
    args[0] = (uint32_t)cs;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0)
        fail("connect to a listener bound to 127.0.0.1");
    args[0] = (uint32_t)ls;
    args[1] = 0;
    args[2] = 0;
    as = socketcall(SC_ACCEPT, args);
    if (as < 0)
        fail("accept on 127.0.0.1");
    sys_close(as);
    sys_close(cs);
    sys_close(ls);
}

/** A connect to a closed loopback port is refused at once by the reset
 * that answers its SYN (RFC 793), and not left to wait for the
 * retransmissions to give up. */
static void tcp_refused(void)
{
    unsigned long args[6];
    uint8_t sa[16];
    int cs, rc;

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    cs = socketcall(SC_SOCKET, args);
    if (cs < 0)
        fail("refused socket");

    make_sockaddr(sa, closed_port, 127, 0, 0, 1);
    args[0] = (uint32_t)cs;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    rc = socketcall(SC_CONNECT, args);
    if (rc != -ECONNREFUSED)
        fail("closed port not refused");

    sys_close(cs);
}

/**
 * A reader already asleep on a stream another process shuts down wakes with
 * the end of input. The sleeper is in the socket's wait queue, and the
 * shutdown detaches the socket from its block both ways, so nothing the peer
 * sends afterwards can reach that queue: the shutdown wakes it or nothing
 * does. `cs` is a connected socket with nothing to read. The shutdown waits
 * until /proc shows the child asleep, since a shutdown before its read takes
 * the path `shutdown_rows` covers, and a child that ends first fails.
 */
static void shutdown_wakes_a_sharer(int cs)
{
    unsigned long args[6];
    uint8_t byte;
    int pid, status = 0;
    char state;

    pid = sys_fork();
    if (pid < 0)
        fail("fork for the shutdown wake");
    if (pid == 0) {
        /* Blocks: the ring is empty and the peer sends nothing. */
        sys_exit(sys_read(cs, &byte, 1) == 0 ? 0 : 1);
    }
    while ((state = state_of(pid)) != 'S') {
        if (state == 0 || state == 'Z')
            fail("the reader was never seen asleep before the shutdown");
        sys_nanosleep_ms(10);
    }
    args[0] = (uint32_t)cs;
    args[1] = 2; /* SHUT_RDWR */
    if (socketcall(SC_SHUTDOWN, args) != 0)
        fail("shutdown under a sleeping reader");
    if (sys_waitpid(pid, &status) != pid)
        fail("the reader did not end after the shutdown");
    if (((status >> 8) & 0xFF) != 0)
        fail("a reader asleep through a shutdown did not read the end of input");
}

/**
 * A stream this end shut down for writing: a send is EPIPE, and a read,
 * blocking or not, is the end of input, since the peer has closed its end.
 * A stream that never connected reads ENOTCONN. `cs` is a connected client
 * whose peer has shut down; `sa` is scratch space of a byte.
 */
static void shutdown_rows(int cs, uint8_t *sa)
{
    unsigned long args[6];
    int fresh;

    args[0] = (uint32_t)cs;
    args[1] = 1; /* SHUT_WR */
    if (socketcall(SC_SHUTDOWN, args) != 0)
        fail("shutdown of the client");
    args[0] = (uint32_t)cs;
    args[1] = (unsigned long)"x";
    args[2] = 1;
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_SENDTO, args) != -EPIPE)
        fail("a send after shutdown is not EPIPE");
    if (sys_read(cs, sa, 1) != 0)
        fail("a blocking read after shutdown is not the end of input");
    if (sys_fcntl(cs, F_SETFL, O_NONBLOCK) != 0)
        fail("nonblocking after shutdown");
    if (sys_read(cs, sa, 1) != 0)
        fail("a nonblocking read after shutdown is not the end of input");

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    fresh = socketcall(SC_SOCKET, args);
    if (fresh < 0)
        fail("fresh stream socket");
    if (sys_fcntl(fresh, F_SETFL, O_NONBLOCK) != 0)
        fail("nonblocking on the fresh socket");
    if (sys_read(fresh, sa, 1) != -ENOTCONN)
        fail("a read of a stream that never connected is not ENOTCONN");
    sys_close(fresh);
}

/**
 * The ports of TCP sockets: the one listener a port may have, ephemeral
 * ports, and the source port a client binds.
 *
 * Two sockets may bind one port before either listens, and then only one may
 * listen, since listen makes the check where the listener is published;
 * once a listener is live, a bind to its port is EADDRINUSE too. A listener
 * with no port and a bind to port 0 are each given an ephemeral port, 49152
 * or above, as Linux gives them. A server binds and listens again on
 * tcp_port[0], whose connection `tcp_loop` closed, since bind and listen refuse
 * a port only for a live listener; bind does not read SO_REUSEADDR, so a
 * server that sets it gains nothing.
 *
 * A client that binds a port before it connects is seen by the server from
 * that port. A second connection from the same port to the same peer is
 * EADDRINUSE, since the connection table keys on the local port and the
 * remote address and port. The shutdown rows then run on this connection.
 */
static void tcp_ports(void)
{
    unsigned long args[6];
    uint8_t sa[16], peer[16];
    uint32_t salen = 16;
    int ls, cs, as, second;

    /* A listener to dial, and a second socket bound to its port first. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    ls = socketcall(SC_SOCKET, args);
    if (ls < 0)
        fail("bound-port listen socket");

    make_sockaddr(sa, bound_listen_port, 0, 0, 0, 0);
    args[0] = (uint32_t)ls;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("bound-port listen bind");

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    second = socketcall(SC_SOCKET, args);
    if (second < 0)
        fail("second listener socket");
    args[0] = (uint32_t)second;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("a second bind before any listen was refused");

    args[0] = (uint32_t)ls;
    args[1] = 4;
    if (socketcall(SC_LISTEN, args) != 0)
        fail("bound-port listen");

    /* Both are bound; only one may listen. */
    args[0] = (uint32_t)second;
    args[1] = 4;
    if (socketcall(SC_LISTEN, args) != -EADDRINUSE)
        fail("second listener on one port not refused");
    sys_close(second);

    /* A bind to a live listener's port. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    second = socketcall(SC_SOCKET, args);
    if (second < 0)
        fail("late bind socket");
    args[0] = (uint32_t)second;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != -EADDRINUSE)
        fail("binding a live listener's port was not reported");
    sys_close(second);

    /* A listener with no port, then a bind to port 0. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    second = socketcall(SC_SOCKET, args);
    if (second < 0)
        fail("unbound listener socket");
    args[0] = (uint32_t)second;
    args[1] = 4;
    if (socketcall(SC_LISTEN, args) != 0)
        fail("unbound listener refused");
    if (local_port(second) < 49152)
        fail("unbound listener given no ephemeral port");
    sys_close(second);
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    second = socketcall(SC_SOCKET, args);
    if (second < 0)
        fail("port 0 socket");
    make_sockaddr(sa, 0, 0, 0, 0, 0);
    args[0] = (uint32_t)second;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("bind to port 0");
    if (local_port(second) < 49152)
        fail("bind to port 0 given no ephemeral port");
    sys_close(second);

    /* A server restarted on the port of `tcp_loop`'s closed connection. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    second = socketcall(SC_SOCKET, args);
    if (second < 0)
        fail("restart socket");
    make_sockaddr(sa, tcp_port[0], 0, 0, 0, 0);
    args[0] = (uint32_t)second;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("a port a closed session still has could not be bound");
    args[0] = (uint32_t)second;
    args[1] = 4;
    if (socketcall(SC_LISTEN, args) != 0)
        fail("a server could not restart on its own last session's port");
    sys_close(second);

    /* A client that binds its source port before it connects. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    cs = socketcall(SC_SOCKET, args);
    if (cs < 0)
        fail("bound-port client socket");

    make_sockaddr(sa, bound_client_port, 0, 0, 0, 0);
    args[0] = (uint32_t)cs;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("bound-port client bind");

    make_sockaddr(sa, bound_listen_port, 127, 0, 0, 1);
    args[0] = (uint32_t)cs;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0)
        fail("bound-port connect");

    args[0] = (uint32_t)ls;
    args[1] = (unsigned long)peer;
    args[2] = (unsigned long)&salen;
    salen = 16;
    as = socketcall(SC_ACCEPT, args);
    if (as < 0)
        fail("bound-port accept");

    if (peer[2] != (bound_client_port >> 8) || peer[3] != (bound_client_port & 0xFF)) {
        fail("the peer port is not the one the client bound");
    }

    /* A second connection from that port to the same peer. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    second = socketcall(SC_SOCKET, args);
    if (second < 0)
        fail("duplicate pairing socket");
    make_sockaddr(sa, bound_client_port, 0, 0, 0, 0);
    args[0] = (uint32_t)second;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("duplicate pairing bind");
    make_sockaddr(sa, bound_listen_port, 127, 0, 0, 1);
    args[0] = (uint32_t)second;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != -EADDRINUSE)
        fail("a repeated local-remote pairing was not refused");
    sys_close(second);

    /* On the accepted end, so the rows below still have a connected client. */
    shutdown_wakes_a_sharer(as);
    shutdown_rows(cs, sa);

    sys_close(as);
    sys_close(cs);
    sys_close(ls);
}

/** The datagrams the loopback ring has delivered since boot, by the tick, the
 * idle wait and system calls' exits together. */
static uint32_t loop_delivered_now(void)
{
    return proc_stat_u32("loopback_delivered_tick ", 24) +
           proc_stat_u32("loopback_delivered_idle ", 24) +
           proc_stat_u32("loopback_delivered_call ", 24);
}

/**
 * Simultaneous open (RFC 793): both ends send a SYN and neither has seen an
 * ACK. Two sockets, each bound to the port the other dials, connect without
 * blocking, so that both SYNs are in flight at once from one process and
 * each arrives at a socket that has sent its own. Each end answers with a
 * SYN-ACK at its `iss`, the sequence the peer expects, and the two establish
 * a connection that carries a message from `a` to `b`.
 *
 * A round waits in poll for `a` to connect or be refused, and is decided by
 * the message, which `b` waits for in poll as well. A round may lose: a tick
 * of the 100 Hz timer between the two connects delivers the first SYN
 * before `b` has a block, and the reset refuses it. The kernel's count of
 * the datagrams the ring delivered, read before the first connect and after
 * the second, says whether that can have happened: a round that loses with
 * the count unchanged had both SYNs on the ring at once, and fails the
 * probe; one that loses after a delivery is tried again on the next pair of
 * ports, SIM_PAIRS pairs in turn.
 */
static void tcp_simultaneous(void)
{
    unsigned long args[6];
    uint8_t sa[16];
    int a, b, rc, round, crossed = 0;
    uint16_t port_a, port_b;
    uint32_t before, delivered;
    const char *msg = "simultaneous";
    char rbuf[32];
    int n;

    for (round = 0; !crossed; round++) {
        port_a = sim_port[(round % SIM_PAIRS) * 2];
        port_b = sim_port[(round % SIM_PAIRS) * 2 + 1];
        args[0] = AF_INET;
        args[1] = SOCK_STREAM;
        args[2] = 0;
        a = socketcall(SC_SOCKET, args);
        args[0] = AF_INET;
        args[1] = SOCK_STREAM;
        args[2] = 0;
        b = socketcall(SC_SOCKET, args);
        if (a < 0 || b < 0)
            fail("simultaneous sockets");

        make_sockaddr(sa, port_a, 0, 0, 0, 0);
        args[0] = (uint32_t)a;
        args[1] = (unsigned long)sa;
        args[2] = 16;
        if (socketcall(SC_BIND, args) != 0)
            fail("simultaneous bind a");
        make_sockaddr(sa, port_b, 0, 0, 0, 0);
        args[0] = (uint32_t)b;
        args[1] = (unsigned long)sa;
        args[2] = 16;
        if (socketcall(SC_BIND, args) != 0)
            fail("simultaneous bind b");

        if (sys_fcntl(a, F_SETFL, O_NONBLOCK) < 0 || sys_fcntl(b, F_SETFL, O_NONBLOCK) < 0)
            fail("simultaneous nonblock");

        before = loop_delivered_now();
        make_sockaddr(sa, port_b, 127, 0, 0, 1);
        args[0] = (uint32_t)a;
        args[1] = (unsigned long)sa;
        args[2] = 16;
        rc = socketcall(SC_CONNECT, args);
        if (rc != -EINPROGRESS && rc != 0)
            fail("simultaneous connect a");

        make_sockaddr(sa, port_a, 127, 0, 0, 1);
        args[0] = (uint32_t)b;
        args[1] = (unsigned long)sa;
        args[2] = 16;
        rc = socketcall(SC_CONNECT, args);
        if (rc != -EINPROGRESS && rc != 0)
            fail("simultaneous connect b");
        delivered = loop_delivered_now() - before;

        (void)wait_for(a, POLLOUT);

        /* The round crossed when a message reaches `b`. */
        n = 0;
        args[0] = (uint32_t)a;
        args[1] = (unsigned long)msg;
        args[2] = (uint32_t)strlen(msg);
        args[3] = 0;
        args[4] = 0;
        args[5] = 0;
        if (socketcall(SC_SENDTO, args) == (int)strlen(msg)) {
            (void)wait_for(b, POLLIN);
            args[0] = (uint32_t)b;
            args[1] = (unsigned long)rbuf;
            args[2] = sizeof(rbuf);
            args[3] = MSG_DONTWAIT;
            args[4] = 0;
            args[5] = 0;
            n = socketcall(SC_RECVFROM, args);
        }
        if (n == (int)strlen(msg) && mem_eq((const uint8_t *)rbuf, (const uint8_t *)msg, n)) {
            crossed = 1;
        } else if (delivered == 0) {
            fail("crossing SYNs in flight at once opened no connection that carries data");
        }
        sys_close(a);
        sys_close(b);
    }
}

/** A server that closes a connection at once after accept: the client's
 * connect still succeeds, and its read is the end of input. */
static void tcp_accept_close(void)
{
    unsigned long args[6];
    uint8_t sa[16];
    char byte;
    int listener, client, accepted, pid, status = 0;

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    listener = socketcall(SC_SOCKET, args);
    if (listener < 0)
        fail("immediate-close listener");
    make_sockaddr(sa, tcp_port[6], 0, 0, 0, 0);
    args[0] = (uint32_t)listener;
    args[1] = (unsigned long)sa;
    args[2] = sizeof(sa);
    if (socketcall(SC_BIND, args) != 0)
        fail("immediate-close bind");
    args[1] = 1;
    if (socketcall(SC_LISTEN, args) != 0)
        fail("immediate-close listen");
    pid = sys_fork();
    if (pid < 0)
        fail("immediate-close fork");
    if (pid == 0) {
        args[0] = (uint32_t)listener;
        args[1] = 0;
        args[2] = 0;
        accepted = socketcall(SC_ACCEPT, args);
        if (accepted < 0)
            sys_exit(1);
        sys_close(accepted);
        sys_close(listener);
        sys_exit(0);
    }
    sys_close(listener);
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    client = socketcall(SC_SOCKET, args);
    if (client < 0)
        fail("immediate-close client");
    make_sockaddr(sa, tcp_port[6], 127, 0, 0, 1);
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)sa;
    args[2] = sizeof(sa);
    if (socketcall(SC_CONNECT, args) != 0)
        fail("immediate-close connect");
    if (sys_read(client, &byte, 1) != 0)
        fail("immediate-close eof");
    sys_close(client);
    if (sys_waitpid(pid, &status) != pid || status != 0)
        fail("immediate-close child");
}

/** Take the ports from the arguments, each the value `ports=` gives in place
 * of its own number or that number itself. */
static void take_ports(void)
{
    int i;

    udp_port = (uint16_t)probe_port(7100);
    nolen_port = (uint16_t)probe_port(7101);
    peek_port = (uint16_t)probe_port(7102);
    for (i = 0; i < 8; i++) {
        tcp_port[i] = (uint16_t)probe_port(7200 + i);
    }
    bound_listen_port = (uint16_t)probe_port(7300);
    bound_client_port = (uint16_t)probe_port(7301);
    closed_port = bound_listen_port;
    for (i = 0; i < 2 * SIM_PAIRS; i++) {
        sim_port[i] = (uint16_t)probe_port(7310 + i);
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: each case in turn, then
 * LOOPPROBE OK. */
void probe_main(unsigned long *sp)
{
    probe_args((int)sp[0], (char **)(sp + 1));
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.loopprobe"));
    report_pid = syscall0(SYS_GETPID);
    take_ports();
    learn_addresses();
    rows_enter("udp socket");
    udp_loop();
    rows_enter("udp socket for the recvfrom with no length");
    udp_no_length();
    rows_enter("udp socket for the peek");
    udp_peek();
    rows_enter("tcp sockets for the peek");
    tcp_peek();
    rows_enter("tcp listener socket");
    tcp_loop();
    rows_enter("immediate-close listener");
    tcp_accept_close();
    rows_enter("bulk listener socket");
    tcp_loop_bulk();
    rows_enter("pipes for the round-trip calibration");
    tcp_loop_pingpong();
    rows_enter("bound listener socket");
    tcp_bound_loopback();
    rows_enter("refused socket");
    tcp_refused();
    rows_enter("bound-port listen socket");
    tcp_ports();
    rows_enter("simultaneous sockets");
    tcp_simultaneous();
    rows_report(STEPS, 0);
    print("LOOPPROBE OK\n");
    sys_exit(0);
}
