/**
 * loopprobe: the loopback, the probe's sockets reaching each other through
 * 127.0.0.1 with no ARP and no wire.
 *
 * The IP layer turns a datagram to 127/8 around through the loopback ring, from
 * 127.0.0.1. A datagram socket connected to 127.0.0.1 reports 127.0.0.1 as its
 * own address. A UDP datagram sent to 127.0.0.1 at the socket's own bound port
 * comes back with the payload intact and the source 127.0.0.1, an empty
 * datagram comes back as one, and a recvfrom given an address and no length
 * pointer fails with EFAULT and leaves the datagram for the next receive. A
 * receive with MSG_PEEK leaves a datagram and a stream's bytes for the next
 * one, and recvmsg reads a stream into its buffers in order, peeking or taking.
 * A datagram recvmsg fails only on a buffer the datagram reaches, and then
 * takes it. A stream receive into an address below user space fails with every
 * byte kept, one whose later segment cannot be written returns the segments
 * before it and keeps that one, past 32 segments as before them, and one with
 * MSG_WAITALL waits until its buffers are full. recv and recvfrom into a buffer
 * whose second page cannot be written return the bytes queued, and into a page
 * they cannot write at all fail with EAGAIN on an empty nonblocking stream and
 * return 0 at the end of input. A send on a nonblocking socket still connecting
 * fails with EAGAIN. A TCP connection to the probe's own listener completes its
 * handshake, moves data both ways and reports the peer 127.0.0.1, and one the
 * server closes at once after accept still connects; a nonblocking accept right
 * after the connect finds the connection, and a poll with a zero timeout right
 * after a write finds the bytes. A transfer larger than the ring arrives whole
 * with no datagram dropped, and the legs of the round trips between two
 * processes are delivered at the exits of the calls that sent them, none left
 * on the ring for the timer while the machine idles. A listener bound to
 * 127.0.0.1 is not reached at the machine's interface address, a bind to an
 * address the machine lacks is refused, and a connect to a closed port is
 * refused by the reset that answers its SYN. The rows of TCP ports check a
 * client's bound source port, the one listener a port may have, ephemeral ports
 * and a shutdown, and a socket that dials its own port opens a connection to
 * itself by a simultaneous open.
 *
 * A row that waits for a datagram or a connection blocks in the receive or
 * in poll(2) until it comes, and judges what came; the case's watchdog ends
 * one that never does.
 *
 * The probe uses UDP ports 7100 to 7102 and TCP ports 7200 to 7207, 7300,
 * 7301 and 7310, and 7300 is the closed port until its rows listen on it. A
 * native run maps them with `ports=OLD:NEW,...` (probe_args.h). With
 * `machine=native` the probe leaves out the checks of counts that every
 * loopback datagram of the machine moves, no datagram dropped in the bulk
 * transfer, and the round trips delivered at their calls' exits with none
 * left for the timer by an idle machine.
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
#define MSG_WAITALL  0x100
#define MSG_NOSIGNAL 0x4000
#define EAGAIN       11
#define EFAULT       14

#define PROT_NONE     0x0
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

/* An address below the bottom of user space, USER_ADDR_MIN, on both kernels:
 * 0x4000000 on i686 and 0x10000 on x86-64, which runs this i686 binary in its
 * ia32 profile too. An address between the two would be refused on i686 and
 * only unmapped on x86-64. */
#define LOW_ADDR 0x8000UL

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

/* The ports, 7100 to 7102, 7200 to 7207, 7300, 7301 and 7310, or the values
 * `ports=` gives in their place: `udp_port`, `nolen_port` for the recvfrom
 * given no length pointer, `peek_port` for the peek rows' datagram and
 * connection, `tcp_port[N]` for 7200 + N, the bound-port rows' two of
 * `tcp_ports`, and `sim_port`, the simultaneous-open case's. `closed_port`
 * is the bound-port rows' listener's; `tcp_refused` runs before those rows,
 * while nothing listens on it. */
static uint16_t udp_port, nolen_port, peek_port, tcp_port[8];
static uint16_t bound_listen_port, bound_client_port, closed_port, sim_port;

/* The round trips of `tcp_loop_pingpong`, a byte each way. */
#define PINGPONG_ROUNDS 200

#define EADDRINUSE  98
#define EPIPE       32
#define SIGPIPE     13
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
    {"a udp recvmsg failed on a buffer the datagram did not reach", "udp-recvmsg-unreached"},
    {"a udp recvmsg on an empty socket with a bad buffer was not EAGAIN", "udp-recvmsg-empty-bad"},
    {"a udp recvmsg into a bad buffer it reached was not EFAULT with the datagram taken",
     "udp-recvmsg-reached"},
    {"tcp sockets for the peek", "tcp-peek-copies"},
    {"tcp bind for the peek", "tcp-peek-copies"},
    {"tcp listen for the peek", "tcp-peek-copies"},
    {"tcp connect for the peek", "tcp-peek-copies"},
    {"tcp accept for the peek", "tcp-peek-copies"},
    {"tcp write for the peek", "tcp-peek-copies"},
    {"a tcp peek of one byte did not copy it", "tcp-peek-copies"},
    {"a tcp read after a peek did not start at the byte peeked", "tcp-read-after-peek"},
    {"tcp write for recvmsg", "tcp-recvmsg-peek"},
    {"a tcp recvmsg peek did not fill both buffers in order", "tcp-recvmsg-peek"},
    {"a tcp recvmsg gave an address, control data or flags", "tcp-recvmsg-header"},
    {"a tcp recvmsg did not take the bytes the two buffers hold", "tcp-recvmsg-take"},
    {"a nonblocking tcp recvmsg did not take the rest", "tcp-recvmsg-rest"},
    {"a nonblocking tcp recvmsg of an empty stream was not EAGAIN", "tcp-recvmsg-empty"},
    {"tcp recvmsg changed a copied vector", "tcp-recvmsg-overlap"},
    {"a tcp receive below user space was not EFAULT with every byte kept", "tcp-recv-low"},
    {"no page for the tcp receive buffer rows", "tcp-recv-buffer-kept"},
    {"a tcp receive did not keep the buffer whose destination failed", "tcp-recv-buffer-kept"},
    {"a tcp receive past 32 buffers did not keep the buffer whose destination failed",
     "tcp-recv-buffer-kept-many"},
    {"no pages for the tcp recv span rows", "tcp-recv-span"},
    {"a tcp recv into a buffer whose second page cannot be written did not return the bytes queued",
     "tcp-recv-span"},
    {"a nonblocking tcp recv of an empty stream into a page it cannot write was not EAGAIN",
     "tcp-recv-span-empty"},
    {"a tcp receive with MSG_WAITALL did not wait for its buffers to fill", "tcp-recv-waitall"},
    {"a nonblocking tcp receive with MSG_WAITALL did not return what had come",
     "tcp-recv-waitall-nonblock"},
    {"tcp recvmsg with empty buffers was not EAGAIN", "tcp-recvmsg-zero-empty"},
    {"tcp recvmsg with empty buffers did not wait for input", "tcp-recvmsg-zero-wait"},
    {"tcp recvmsg with empty buffers took queued input", "tcp-recvmsg-zero-ready"},
    {"tcp recvmsg with empty buffers missed the end of input", "tcp-recvmsg-zero-eof"},
    {"a tcp recv into a page it cannot write at the end of input was not 0", "tcp-recv-span-eof"},
    {"tcp listener socket", "tcp-connect"},
    {"tcp bind", "tcp-connect"},
    {"tcp listen", "tcp-connect"},
    {"tcp client socket", "tcp-connect"},
    {"tcp listener non-blocking", "tcp-connect"},
    {"tcp connect", "tcp-connect"},
    {"tcp accept", "tcp-accept-now"},
    {"a nonblocking accept right after the connect found no connection", "tcp-accept-now"},
    {"tcp client write", "tcp-poll-now"},
    {"a zero-timeout poll right after the write found nothing to read", "tcp-poll-now"},
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
    {"the exits of the round trips' calls delivered fewer datagrams than rounds", "pingpong-idle"},
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
    {"connecting socket", "send-connecting-eagain"},
    {"connecting socket non-blocking", "send-connecting-eagain"},
    {"a nonblocking connect to 192.0.2.1 did not return EINPROGRESS", "send-connecting-eagain"},
    {"a send on a nonblocking socket still connecting was not EAGAIN", "send-connecting-eagain"},
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
    {"fork for the send that raises SIGPIPE", "shutdown-epipe"},
    {"a send after shutdown without MSG_NOSIGNAL did not end the sender with SIGPIPE",
     "shutdown-epipe"},
    {"an empty sendmsg after shutdown is not EPIPE", "shutdown-epipe"},
    {"fork for the empty sendmsg that raises SIGPIPE", "shutdown-epipe"},
    {"an empty sendmsg after shutdown without MSG_NOSIGNAL did not end the sender with SIGPIPE",
     "shutdown-epipe"},
    {"a blocking read after shutdown is not the end of input", "shutdown-blocking-eof"},
    {"nonblocking after shutdown", "shutdown-nonblocking-eof"},
    {"a nonblocking read after shutdown is not the end of input", "shutdown-nonblocking-eof"},
    {"fresh stream socket", "never-connected-enotconn"},
    {"nonblocking on the fresh socket", "never-connected-enotconn"},
    {"a read of a stream that never connected is not ENOTCONN", "never-connected-enotconn"},
    {"simultaneous socket", "simultaneous-open"},
    {"simultaneous bind", "simultaneous-open"},
    {"simultaneous nonblock", "simultaneous-open"},
    {"simultaneous connect", "simultaneous-open"},
    {"a socket that dialed its own port is not connected", "simultaneous-open"},
    {"a socket that dialed its own port did not receive what it sent", "simultaneous-open"}};

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
#define POLLHUP 0x010

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

/** poll(2) with a zero timeout: how many descriptors of `fds` are ready now,
 * without waiting, or a negated errno. */
static int sys_poll_now(struct pollfd_probe *fds, unsigned int n)
{
    return syscall3(SYS_POLL, (long)fds, (long)n, 0);
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

/** A page of no access, mapped once, which a receive cannot write: its
 * address, or NULL when it cannot be mapped. */
static void *bad_page(void)
{
    static long page;

    if (page == 0) {
        page = probe_mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (sys_failed(page))
            page = 0;
    }
    return (void *)page;
}

/* The struct iovec and msghdr of the probe's ABI. */
struct iovec_probe {
    void *iov_base;
    size_t iov_len;
};

struct msghdr_probe {
    void *msg_name;
    unsigned int msg_namelen;
    struct iovec_probe *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

/** Receive on `s` into `count` vectors at `iov`, with no address or control
 * buffer: the count read or a negated errno. */
static int recvmsg_vec(int s, struct iovec_probe *iov, size_t count, int flags)
{
    struct msghdr_probe msg = {0};
    unsigned long args[3];

    msg.msg_iov = iov;
    msg.msg_iovlen = count;
    args[0] = (unsigned long)s;
    args[1] = (unsigned long)&msg;
    args[2] = (unsigned long)flags;
    return socketcall(SC_RECVMSG, args);
}

/** Send the `len` bytes of `msg` from the UDP socket `s` to the address `sa`:
 * the bytes sent or a negated errno. */
static int udp_send(int s, const uint8_t *sa, const char *msg, int len)
{
    unsigned long args[6];

    args[0] = (uint32_t)s;
    args[1] = (unsigned long)msg;
    args[2] = (uint32_t)len;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    return socketcall(SC_SENDTO, args);
}

/** recvmsg on the datagram socket `s` writes its buffers only as it copies
 * the datagram into them, as Linux's udp_recvmsg does, with `sa` the
 * socket's own address: a datagram of four bytes fills the first of two
 * buffers whose second cannot be written and returns four; a buffer that
 * cannot be written on an empty socket gives EAGAIN with MSG_DONTWAIT; and a
 * datagram of eight bytes that reaches it gives EFAULT and is taken. */
static void udp_recvmsg_bad(int s, const uint8_t *sa)
{
    struct iovec_probe iov[2];
    char first[4];

    iov[0].iov_base = first;
    iov[0].iov_len = sizeof(first);
    iov[1].iov_base = bad_page();
    iov[1].iov_len = 4096;
    if (!iov[1].iov_base || udp_send(s, sa, "dgrm", 4) != 4 || recvmsg_vec(s, iov, 2, 0) != 4 ||
        !mem_eq((const uint8_t *)first, (const uint8_t *)"dgrm", 4))
        fail("a udp recvmsg failed on a buffer the datagram did not reach");
    if (recvmsg_vec(s, iov + 1, 1, MSG_DONTWAIT) != -EAGAIN)
        fail("a udp recvmsg on an empty socket with a bad buffer was not EAGAIN");
    iov[1].iov_len = 4;
    if (udp_send(s, sa, "datagram", 8) != 8 || recvmsg_vec(s, iov, 2, 0) != -EFAULT ||
        recvmsg_vec(s, iov, 1, MSG_DONTWAIT) != -EAGAIN)
        fail("a udp recvmsg into a bad buffer it reached was not EFAULT with the datagram taken");
}

/** A datagram peeked at with MSG_PEEK stays queued, whole and with its
 * sender, for the next receive, which takes it. recvmsg then reads the same
 * socket with buffers it cannot write (`udp_recvmsg_bad`). */
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
    udp_recvmsg_bad(s, sa);
    sys_close(s);
}

/* The two buffers `recvmsg2` reads into, and the header it gives back. */
static char rm_a[16], rm_b[16];
static struct msghdr_probe rm_msg;

/** recvmsg on the stream `s` with `flags` into the first `alen` bytes of
 * `rm_a` and the first `blen` of `rm_b`, the header given back in `rm_msg`:
 * the bytes received, or a negated errno. The header offers an address and
 * a control buffer, which a stream leaves unwritten. */
static int recvmsg2(int s, int alen, int blen, int flags)
{
    static uint8_t name[16], control[32];
    static struct iovec_probe iov[2];
    unsigned long args[3];

    iov[0].iov_base = rm_a;
    iov[0].iov_len = (size_t)alen;
    iov[1].iov_base = rm_b;
    iov[1].iov_len = (size_t)blen;
    rm_msg.msg_name = name;
    rm_msg.msg_namelen = sizeof(name);
    rm_msg.msg_iov = iov;
    rm_msg.msg_iovlen = 2;
    rm_msg.msg_control = control;
    rm_msg.msg_controllen = sizeof(control);
    rm_msg.msg_flags = -1;
    args[0] = (uint32_t)s;
    args[1] = (unsigned long)&rm_msg;
    args[2] = (uint32_t)flags;
    return socketcall(SC_RECVMSG, args);
}

/** Whether `rm_a` begins with the `alen` bytes of `want` and `rm_b` with
 * the `blen` after them. */
static int recvmsg2_got(const char *want, int alen, int blen)
{
    return mem_eq((const uint8_t *)rm_a, (const uint8_t *)want, alen) &&
           mem_eq((const uint8_t *)rm_b, (const uint8_t *)want + alen, blen);
}

/** A receive that overwrites its second vector must still use its original
 * destination, for blocking, nonblocking and peeking reads on `cs` from
 * `as`. The replacement is writable too, so the unfixed kernel fails the
 * contents check without faulting. The nonblocking read comes right after
 * the write, and finds its bytes, since the write's exit delivered the
 * loopback ring, as Linux's loopback delivers within the sender's call. */
static void tcp_recvmsg_overlap(int cs, int as)
{
    struct iovec_probe iov[2], replacement;
    unsigned char wire[sizeof(replacement) + 1], got[sizeof(wire)];
    char original, other;
    int mode, flags, j;

    for (mode = 0; mode < 3; mode++) {
        flags = mode == 0 ? 0 : mode == 1 ? MSG_DONTWAIT : MSG_PEEK;
        original = other = 0;
        replacement.iov_base = &other;
        replacement.iov_len = 1;
        for (j = 0; j < (int)sizeof(replacement); j++)
            wire[j] = ((unsigned char *)&replacement)[j];
        wire[j] = 'x';
        iov[0].iov_base = &iov[1];
        iov[0].iov_len = sizeof(iov[1]);
        iov[1].iov_base = &original;
        iov[1].iov_len = 1;
        if (sys_write(as, wire, sizeof(wire)) != (int)sizeof(wire) ||
            recvmsg_vec(cs, iov, 2, flags) != (int)sizeof(wire) || original != 'x' || other != 0)
            fail("tcp recvmsg changed a copied vector");
        if (flags == MSG_PEEK && (sys_read(cs, got, sizeof(got)) != (int)sizeof(got) ||
                                  !mem_eq(got, wire, sizeof(wire))))
            fail("tcp recvmsg changed a copied vector");
    }
}

/** recv(2) on `s` into `buf` with `flags`, through its own subcall: the
 * bytes received or a negated errno. */
static int recv_call(int s, char *buf, int len, int flags)
{
    unsigned long args[4];

    args[0] = (uint32_t)s;
    args[1] = (unsigned long)buf;
    args[2] = (uint32_t)len;
    args[3] = (uint32_t)flags;
    return socketcall(SC_RECV, args);
}

/** A stream receive whose destination lies below user space fails with
 * EFAULT and leaves every byte queued, through readv, recvmsg, recvmsg with
 * MSG_PEEK, recv and recvfrom, as Linux's checked copy fails; a read then
 * takes them. recv and recvfrom find the address at the copy, as the vector
 * calls do, since their first check is only that the buffer ends in user
 * space. */
static void tcp_recv_low(int cs, int as)
{
    const char *msg = "below-user-space";
    struct iovec_probe iov[1];
    char got[16];

    iov[0].iov_base = (void *)LOW_ADDR;
    iov[0].iov_len = sizeof(got);
    if (sys_write(as, msg, sizeof(got)) != (int)sizeof(got) ||
        syscall3(SYS_READV, cs, (long)iov, 1) != -EFAULT || recvmsg_vec(cs, iov, 1, 0) != -EFAULT ||
        recvmsg_vec(cs, iov, 1, MSG_PEEK) != -EFAULT ||
        recv_call(cs, (char *)LOW_ADDR, sizeof(got), 0) != -EFAULT ||
        recv_wait(cs, (char *)LOW_ADDR, sizeof(got), 0) != -EFAULT ||
        sys_read(cs, got, sizeof(got)) != (int)sizeof(got) ||
        !mem_eq((const uint8_t *)got, (const uint8_t *)msg, (int)sizeof(got)))
        fail("a tcp receive below user space was not EFAULT with every byte kept");
}

/** A stream receive takes each receive buffer whole or not at all: with two
 * segments queued, which a peek of eight bytes finds, and the second's
 * destination a page that cannot be written, readv and recvmsg return the
 * first and leave the second queued for a read, as Linux's tcp_recvmsg
 * returns what it copied before a failed copy. The two segments are two
 * buffers because Rach2 keeps each segment's bounds; Linux may have
 * joined the second to the first (tcp_try_coalesce) and failed the whole
 * read with EFAULT, so the row checks Rach2's own bounds. */
static void tcp_recv_kept(int cs, int as)
{
    struct iovec_probe iov[2];
    char first[4], got[8];
    int mode, n;

    iov[0].iov_base = first;
    iov[0].iov_len = sizeof(first);
    iov[1].iov_base = bad_page();
    iov[1].iov_len = 4;
    if (!iov[1].iov_base)
        fail("no page for the tcp receive buffer rows");
    for (mode = 0; mode < 2; mode++) {
        if (sys_write(as, "seg1", 4) != 4 || sys_write(as, "seg2", 4) != 4 ||
            recv_wait(cs, got, sizeof(got), MSG_PEEK | MSG_DONTWAIT) != (int)sizeof(got))
            fail("a tcp receive did not keep the buffer whose destination failed");
        n = mode == 0 ? syscall3(SYS_READV, cs, (long)iov, 2) : recvmsg_vec(cs, iov, 2, 0);
        if (n != 4 || !mem_eq((const uint8_t *)first, (const uint8_t *)"seg1", 4) ||
            sys_read(cs, got, sizeof(got)) != 4 ||
            !mem_eq((const uint8_t *)got, (const uint8_t *)"seg2", 4))
            fail("a tcp receive did not keep the buffer whose destination failed");
    }
}

/** Past 32 receive buffers each is still kept whole: with 33 four-byte
 * segments queued and the 33rd's destination a page that cannot be written,
 * readv returns the 32 before it and leaves the 33rd for a read, where a
 * table of 32 ends would have joined the 32nd to it and returned 31. */
static void tcp_recv_kept_many(int cs, int as)
{
    struct iovec_probe iov[2];
    char seg[4], got[33 * 4];
    int i;

    iov[0].iov_base = got;
    iov[0].iov_len = 32 * 4;
    iov[1].iov_base = bad_page();
    iov[1].iov_len = 4;
    for (i = 0; i < 33; i++) {
        seg[0] = 's';
        seg[1] = (char)('0' + i / 10);
        seg[2] = (char)('0' + i % 10);
        seg[3] = '.';
        if (sys_write(as, seg, 4) != 4)
            fail("a tcp receive past 32 buffers did not keep the buffer whose destination failed");
    }
    if (recv_wait(cs, got, sizeof(got), MSG_PEEK | MSG_DONTWAIT) != (int)sizeof(got) ||
        syscall3(SYS_READV, cs, (long)iov, 2) != 32 * 4 ||
        !mem_eq((const uint8_t *)got + 31 * 4, (const uint8_t *)"s31.", 4) ||
        sys_read(cs, got, sizeof(got)) != 4 ||
        !mem_eq((const uint8_t *)got, (const uint8_t *)"s32.", 4))
        fail("a tcp receive past 32 buffers did not keep the buffer whose destination failed");
}

/** Two pages, the first writable and the second of no access, mapped once:
 * the first's address, or NULL when they cannot be had. */
static char *span_pages(void)
{
    static long pages;
    long m;

    if (pages == 0) {
        m = probe_mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (sys_failed(m))
            return 0;
        if (syscall3(SYS_MPROTECT, m + 4096, 4096, PROT_NONE) != 0)
            return 0;
        pages = m;
    }
    return (char *)pages;
}

/** recv and recvfrom on a stream check their buffer only to end in user
 * space and copy a receive buffer at a time, as Linux's import_ubuf and
 * tcp_recvmsg do: into 8192 bytes whose second page cannot be written, with
 * four bytes queued, each returns the four; on an empty stream with
 * MSG_DONTWAIT, into a page it cannot write at all, each fails with EAGAIN,
 * the wait coming before any copy. */
static void tcp_recv_span(int cs, int as)
{
    char *buf = span_pages();
    char *none = bad_page();
    int mode, n;

    if (!buf || !none) {
        fail("no pages for the tcp recv span rows");
        return;
    }
    for (mode = 0; mode < 2; mode++) {
        buf[0] = 0;
        if (sys_write(as, "span", 4) != 4)
            fail("a tcp recv into a buffer whose second page cannot be written did not return the "
                 "bytes queued");
        n = mode == 0 ? recv_call(cs, buf, 8192, 0) : recv_wait(cs, buf, 8192, 0);
        if (n != 4 || !mem_eq((const uint8_t *)buf, (const uint8_t *)"span", 4))
            fail("a tcp recv into a buffer whose second page cannot be written did not return the "
                 "bytes queued");
    }
    if (recv_call(cs, none, 4096, MSG_DONTWAIT) != -EAGAIN ||
        recv_wait(cs, none, 4096, MSG_DONTWAIT) != -EAGAIN)
        fail(
            "a nonblocking tcp recv of an empty stream into a page it cannot write was not EAGAIN");
}

/** At the end of input, recv and recvfrom into a page they cannot write
 * return 0, as Linux's tcp_recvmsg ends before it copies. `cs` has been shut
 * down for reading. */
static void tcp_recv_span_eof(int cs)
{
    char *none = bad_page();

    if (recv_call(cs, none, 4096, 0) != 0 || recv_wait(cs, none, 4096, 0) != 0)
        fail("a tcp recv into a page it cannot write at the end of input was not 0");
}

/** Receive on the stream `s` into `buf` with MSG_WAITALL, by recv when
 * `mode` is 0, recvfrom when 1, and recvmsg into two buffers when 2: the
 * bytes received or a negated errno. */
static int recv_waitall(int s, char *buf, int len, int mode)
{
    struct iovec_probe iov[2];

    if (mode == 0)
        return recv_call(s, buf, len, MSG_WAITALL);
    if (mode == 1)
        return recv_wait(s, buf, len, MSG_WAITALL);
    iov[0].iov_base = buf;
    iov[0].iov_len = 3;
    iov[1].iov_base = buf + 3;
    iov[1].iov_len = (size_t)len - 3;
    return recvmsg_vec(s, iov, 2, MSG_WAITALL);
}

/** A stream receive with MSG_WAITALL waits until its buffers are full: recv,
 * recvfrom and recvmsg, each in a child seen asleep after the first half
 * came, return all eight bytes once the parent sends the second half; with
 * MSG_DONTWAIT the call returns the half that came, as Linux's tcp_recvmsg
 * returns what it took where it would wait. */
static void tcp_recv_waitall(int cs, int as)
{
    char buf[8];
    int mode, pid, status;
    char state;

    for (mode = 0; mode < 3; mode++) {
        if (sys_write(as, "wait", 4) != 4)
            fail("a tcp receive with MSG_WAITALL did not wait for its buffers to fill");
        pid = sys_fork();
        if (pid < 0)
            fail("a tcp receive with MSG_WAITALL did not wait for its buffers to fill");
        if (pid == 0)
            sys_exit(recv_waitall(cs, buf, sizeof(buf), mode) == (int)sizeof(buf) &&
                             mem_eq((const uint8_t *)buf, (const uint8_t *)"waitall!", 8)
                         ? 0
                         : 1);
        while ((state = state_of(pid)) != 'S') {
            if (state == 0 || state == 'Z') {
                sys_waitpid(pid, &status);
                fail("a tcp receive with MSG_WAITALL did not wait for its buffers to fill");
            }
            sys_nanosleep_ms(10);
        }
        if (sys_write(as, "all!", 4) != 4) {
            syscall2(SYS_KILL, pid, 9);
            sys_waitpid(pid, &status);
            fail("a tcp receive with MSG_WAITALL did not wait for its buffers to fill");
        }
        if (sys_waitpid(pid, &status) != pid || status != 0)
            fail("a tcp receive with MSG_WAITALL did not wait for its buffers to fill");
    }
    if (sys_write(as, "half", 4) != 4 ||
        recv_wait(cs, buf, sizeof(buf), MSG_WAITALL | MSG_DONTWAIT) != 4 ||
        !mem_eq((const uint8_t *)buf, (const uint8_t *)"half", 4))
        fail("a nonblocking tcp receive with MSG_WAITALL did not return what had come");
}

/** Both empty vector forms must sleep before the peer sends, then return
 * zero and leave its byte queued. The parent reaps each receiver. */
static void tcp_recvmsg_zero_wait(int cs, int as)
{
    struct iovec_probe iov[2] = {{0, 0}, {0, 0}};
    int count, pid, status;
    char state, byte;

    for (count = 0; count <= 2; count += 2) {
        pid = sys_fork();
        if (pid < 0)
            fail("tcp recvmsg with empty buffers did not wait for input");
        if (pid == 0)
            sys_exit(recvmsg_vec(cs, iov, (size_t)count, 0) == 0 ? 0 : 1);
        while ((state = state_of(pid)) != 'S') {
            if (state == 0 || state == 'Z') {
                sys_waitpid(pid, &status);
                fail("tcp recvmsg with empty buffers did not wait for input");
            }
            sys_nanosleep_ms(10);
        }
        if (sys_write(as, "z", 1) != 1) {
            syscall2(SYS_KILL, pid, 9);
            sys_waitpid(pid, &status);
            fail("tcp recvmsg with empty buffers did not wait for input");
        }
        if (sys_waitpid(pid, &status) != pid || status != 0 || sys_read(cs, &byte, 1) != 1 ||
            byte != 'z')
            fail("tcp recvmsg with empty buffers did not wait for input");
    }
}

/** Empty TCP buffers, both a zero count and zero lengths, wait on an empty
 * stream and leave a byte ready after they wake. The parent observes the
 * child's sleeping state before sending, then reaps it and takes the byte.
 * The last row closes input on `cs`. */
static void tcp_recvmsg_zero(int cs, int as)
{
    struct iovec_probe iov[2] = {{0, 0}, {0, 0}};
    unsigned long args[2];
    int count;
    char byte;

    for (count = 0; count <= 2; count += 2) {
        if (recvmsg_vec(cs, iov, (size_t)count, MSG_DONTWAIT) != -EAGAIN ||
            syscall3(SYS_FCNTL, cs, F_SETFL, O_NONBLOCK) != 0 ||
            recvmsg_vec(cs, iov, (size_t)count, 0) != -EAGAIN ||
            syscall3(SYS_FCNTL, cs, F_SETFL, 0) != 0)
            fail("tcp recvmsg with empty buffers was not EAGAIN");
    }
    tcp_recvmsg_zero_wait(cs, as);
    if (sys_write(as, "r", 1) != 1)
        fail("tcp recvmsg with empty buffers took queued input");
    for (count = 0; count <= 2; count += 2) {
        if (recvmsg_vec(cs, iov, (size_t)count, 0) != 0 ||
            recvmsg_vec(cs, iov, (size_t)count, MSG_DONTWAIT | MSG_PEEK) != 0)
            fail("tcp recvmsg with empty buffers took queued input");
    }
    if (sys_read(cs, &byte, 1) != 1 || byte != 'r')
        fail("tcp recvmsg with empty buffers took queued input");
    args[0] = (unsigned long)cs;
    args[1] = 2; /* SHUT_RDWR */
    if (socketcall(SC_SHUTDOWN, args) != 0)
        fail("tcp recvmsg with empty buffers missed the end of input");
    for (count = 0; count <= 2; count += 2) {
        if (recvmsg_vec(cs, iov, (size_t)count, 0) != 0 ||
            recvmsg_vec(cs, iov, (size_t)count, MSG_DONTWAIT) != 0)
            fail("tcp recvmsg with empty buffers missed the end of input");
    }
}

/** recvmsg on a stream, as QEMU reads its monitor's socket: a peek fills two
 * buffers in order and takes nothing, the header comes back with no
 * address, no control data and no flags, a receive takes what its buffers
 * hold and leaves the rest, a nonblocking one takes that rest, and one of an
 * empty stream fails with EAGAIN. Receives retain copied vectors when a
 * buffer overlaps the list. Empty buffers wait for data or EOF, take no
 * bytes, and return EAGAIN on an empty nonblocking stream. The empty-buffer
 * rows shut down cs before this function returns. */
static void tcp_recvmsg(int cs, int as)
{
    const char *msg = "recvmsg-tcp";
    int n;

    if (sys_write(as, msg, strlen(msg)) != (int)strlen(msg))
        fail("tcp write for recvmsg");
    n = recvmsg2(cs, 4, sizeof(rm_b), MSG_PEEK);
    if (n != (int)strlen(msg) || !recvmsg2_got(msg, 4, n - 4))
        fail("a tcp recvmsg peek did not fill both buffers in order");
    if (rm_msg.msg_namelen != 0 || rm_msg.msg_controllen != 0 || rm_msg.msg_flags != 0)
        fail("a tcp recvmsg gave an address, control data or flags");
    n = recvmsg2(cs, 4, 4, 0);
    if (n != 8 || !recvmsg2_got(msg, 4, 4))
        fail("a tcp recvmsg did not take the bytes the two buffers hold");
    n = recvmsg2(cs, 2, sizeof(rm_b), MSG_DONTWAIT);
    if (n != 3 || !recvmsg2_got(msg + 8, 2, 1))
        fail("a nonblocking tcp recvmsg did not take the rest");
    if (recvmsg2(cs, 2, sizeof(rm_b), MSG_DONTWAIT) != -EAGAIN)
        fail("a nonblocking tcp recvmsg of an empty stream was not EAGAIN");
    tcp_recvmsg_overlap(cs, as);
    tcp_recv_low(cs, as);
    tcp_recv_kept(cs, as);
    tcp_recv_kept_many(cs, as);
    tcp_recv_span(cs, as);
    tcp_recv_waitall(cs, as);
    tcp_recvmsg_zero(cs, as);
    tcp_recv_span_eof(cs);
}

/** A stream's bytes peeked at with MSG_PEEK stay in its ring: one byte
 * peeked, as Chromium checks a connection it keeps idle, and the read after
 * it gets the whole message from its first byte. recvmsg then reads the
 * same connection (`tcp_recvmsg`). */
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
    tcp_recvmsg(cs, as);
    sys_close(cs);
    sys_close(as);
    sys_close(ls);
}

/**
 * A TCP connection to the probe's own listener on 127.0.0.1 completes its
 * handshake, carries a message each way, and reports its peer as
 * 127.0.0.1:tcp_port[0].
 *
 * Each step finds what the call before it sent with no wait, as on Linux
 * 6.12.115, whose loopback delivers within the sender's call: the connect's
 * last acknowledgement (net/ipv4/tcp_input.c:6639) puts the connection in
 * the listener's queue, where a nonblocking accept finds it instead of
 * EAGAIN (net/ipv4/inet_connection_sock.c:666-672), and the client's write
 * puts its bytes in the server's receive queue, which a poll with a zero
 * timeout reports readable (net/ipv4/tcp.c:576-577).
 */
static void tcp_loop(void)
{
    struct pollfd_probe pfd;
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
    if (sys_fcntl(ls, F_SETFL, O_NONBLOCK) != 0)
        fail("tcp listener non-blocking");

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
    if (as == -EAGAIN)
        fail("a nonblocking accept right after the connect found no connection");
    if (as < 0)
        fail("tcp accept");

    if (sys_write(cs, ping, strlen(ping)) != (int)strlen(ping)) {
        fail("tcp client write");
    }
    pfd.fd = as;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (sys_poll_now(&pfd, 1) != 1 || !(pfd.revents & POLLIN)) {
        fail("a zero-timeout poll right after the write found nothing to read");
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
 * ring's 64 slots, and the call's exit delivers the ring. The ring's
 * high-water mark is kept since boot, so it can show that the ring was
 * pressed but not that this transfer pressed it: the row requires
 * LOOP_PEAK_FLOOR of it and prints it.
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
 * child over a loopback connection, judged by the kernel's counts of the
 * loopback datagrams each path delivered (`loopback_delivered_*` in
 * /proc/stat).
 *
 * Each leg queues one datagram, which the exit of the write that sent it
 * delivers, with the acknowledgement it draws, before the write returns, as
 * Linux 6.12.115's local_bh_enable runs the softirq that loopback_xmit queued
 * it for (drivers/net/loopback.c:69-92, kernel/softirq.c:402-407); a tick
 * that comes between the send and the exit may take it first. The sender
 * then blocks in its read with the ring empty, so the idle wait's drain
 * (`idle_wait_irq_off`) finds nothing and delivers no leg. The exits must
 * have delivered at least one datagram a round, which a broken exit drain
 * leaves to the idle wait and the timer. `loopback_left_idle`, the datagrams
 * a tick found on the ring with every processor halted, must not rise: such
 * a leg waited for the timer with nothing running, a turn of one window a
 * tick. The other counts and the times of these rounds and of the same
 * rounds over pipes are printed for a person and not judged.
 */
static void tcp_loop_pingpong(void)
{
    unsigned long args[6];
    uint8_t sa[16];
    long t0[2], t1[2];
    char one = 'p';
    int ls, cs, as, pid, status = 0, round, ms, base;
    uint32_t call, idle, tick, left;

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
    call = proc_stat_u32("loopback_delivered_call ", 24);
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
    call = proc_stat_u32("loopback_delivered_call ", 24) - call;
    idle = proc_stat_u32("loopback_delivered_idle ", 24) - idle;
    tick = proc_stat_u32("loopback_delivered_tick ", 24) - tick;
    left = proc_stat_u32("loopback_left_idle ", 19) - left;
    print("LOOPPROBE ping-pong ");
    print_u32((uint32_t)ms);
    print(" ms over the loopback, ");
    print_u32((uint32_t)base);
    print(" over a pipe, for 200 round trips; delivered at a call's exit ");
    print_u32(call);
    print(", by the idle wait ");
    print_u32(idle);
    print(", by the timer ");
    print_u32(tick);
    print(", left for the timer by an idle machine ");
    print_u32(left);
    print("\n");
    if (!probe_native() && call < PINGPONG_ROUNDS) {
        fail("the exits of the round trips' calls delivered fewer datagrams than rounds");
    }
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
 * A send on a nonblocking socket whose connection is still in its handshake
 * fails with EAGAIN, as Linux's sk_stream_wait_connect returns with no time
 * to wait (net/core/stream.c:56-82). The loopback completes a handshake
 * within connect, so the socket dials the documentation address 192.0.2.1,
 * which `learn_addresses` showed is routed out of the interface and which
 * nothing answers before the send that follows the connect at once. The
 * socket is closed in SYN_SENT.
 */
static void tcp_send_connecting(void)
{
    unsigned long args[6];
    uint8_t sa[16];
    int cs;

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    cs = socketcall(SC_SOCKET, args);
    if (cs < 0)
        fail("connecting socket");
    if (sys_fcntl(cs, F_SETFL, O_NONBLOCK) < 0)
        fail("connecting socket non-blocking");

    make_sockaddr(sa, 9, 192, 0, 2, 1);
    args[0] = (uint32_t)cs;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != -EINPROGRESS)
        fail("a nonblocking connect to 192.0.2.1 did not return EINPROGRESS");

    args[0] = (uint32_t)cs;
    args[1] = (unsigned long)"x";
    args[2] = 1;
    args[3] = MSG_NOSIGNAL;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_SENDTO, args) != -EAGAIN)
        fail("a send on a nonblocking socket still connecting was not EAGAIN");

    sys_close(cs);
}

/**
 * A reader already asleep on a stream another process shuts down wakes with
 * the end of input. The sleeper is in the socket's wait queue, and
 * SHUT_RDWR shuts the receiving half, which ends a read of an empty stream
 * at once, and wakes the queue: nothing the peer sends need come for the
 * sleeper to see it. `cs` is a connected socket with nothing to read. The
 * shutdown waits until /proc shows the child asleep, since a shutdown before
 * its read takes the path `shutdown_rows` covers, and a child that ends
 * first fails.
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

/** sendmsg on the stream `s` of `count` buffers of no bytes, or of none
 * when `count` is 0, with `flags`: the count sent or a negated errno. */
static int sendmsg_empty(int s, size_t count, int flags)
{
    static struct iovec_probe iov[2];
    struct msghdr_probe msg = {0};
    unsigned long args[3];

    iov[0].iov_base = rm_a;
    iov[1].iov_base = rm_b;
    msg.msg_iov = count ? iov : (struct iovec_probe *)0;
    msg.msg_iovlen = count;
    args[0] = (unsigned long)s;
    args[1] = (unsigned long)&msg;
    args[2] = (unsigned long)flags;
    return socketcall(SC_SENDMSG, args);
}

/**
 * A stream this end shut down for writing: a send with MSG_NOSIGNAL is
 * EPIPE, and one without it ends the sender with SIGPIPE, as on Linux, and
 * so is a sendmsg with no byte to send, of no buffer or of empty ones; a
 * read, blocking or not, is the end of input, since the peer has closed
 * its end. A stream that never connected reads ENOTCONN. `cs` is a
 * connected client whose peer has shut down; `sa` is scratch space of a
 * byte.
 */
static void shutdown_rows(int cs, uint8_t *sa)
{
    /* A zeroed sigaction, handler, flags, restorer and an 8-byte mask:
     * SIG_DFL, whatever the probe inherited. */
    static const unsigned long dfl[5] = {0, 0, 0, 0, 0};
    unsigned long args[6];
    int fresh, pid, status = 0;
    size_t count;

    args[0] = (uint32_t)cs;
    args[1] = 1; /* SHUT_WR */
    if (socketcall(SC_SHUTDOWN, args) != 0)
        fail("shutdown of the client");
    args[0] = (uint32_t)cs;
    args[1] = (unsigned long)"x";
    args[2] = 1;
    args[3] = MSG_NOSIGNAL;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_SENDTO, args) != -EPIPE)
        fail("a send after shutdown is not EPIPE");
    pid = sys_fork();
    if (pid < 0)
        fail("fork for the send that raises SIGPIPE");
    if (pid == 0) {
        syscall4(SYS_RT_SIGACTION, SIGPIPE, (long)dfl, 0, 8);
        args[3] = 0;
        socketcall(SC_SENDTO, args);
        sys_exit(0);
    }
    if (sys_waitpid(pid, &status) != pid || (status & 0x7F) != SIGPIPE)
        fail("a send after shutdown without MSG_NOSIGNAL did not end the sender with SIGPIPE");
    if (sendmsg_empty(cs, 0, MSG_NOSIGNAL) != -EPIPE ||
        sendmsg_empty(cs, 2, MSG_NOSIGNAL) != -EPIPE)
        fail("an empty sendmsg after shutdown is not EPIPE");
    for (count = 0; count <= 2; count += 2) {
        pid = sys_fork();
        if (pid < 0)
            fail("fork for the empty sendmsg that raises SIGPIPE");
        if (pid == 0) {
            syscall4(SYS_RT_SIGACTION, SIGPIPE, (long)dfl, 0, 8);
            sendmsg_empty(cs, count, 0);
            sys_exit(0);
        }
        if (sys_waitpid(pid, &status) != pid || (status & 0x7F) != SIGPIPE)
            fail("an empty sendmsg after shutdown without MSG_NOSIGNAL did not end the sender "
                 "with SIGPIPE");
    }
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

/**
 * Simultaneous open (RFC 793, figure 8): a SYN reaches a socket in SYN_SENT,
 * which answers with a SYN-ACK at its `iss` and waits in SYN_RCVD for the
 * peer's. A socket bound to 127.0.0.1 at `sim_port` dials that same address
 * and port, so its own SYN is the one that reaches it in SYN_SENT, and the
 * SYN-ACK it answers with is the peer's; the connection it opens to itself
 * carries a message back to it, as Linux's tcp_rcv_synsent_state_process
 * takes "connect to self" in its arm for crossed SYNs (tcp_input.c:6661-6704
 * in 6.12.115) and tcp_validate_incoming passes the SYN-ACK in SYN_RECV
 * (6101-6106). Two sockets of one thread cannot cross two SYNs over the
 * loopback, since each send is delivered before its call returns, as on
 * Linux; a socket's SYN to itself is crossed every time.
 *
 * The connect is nonblocking and is decided in poll, as a crossing between
 * two machines would be: POLLOUT with neither POLLERR nor POLLHUP is the
 * open connection, and a reset would have refused it.
 */
static void tcp_simultaneous(void)
{
    unsigned long args[6];
    uint8_t sa[16];
    int a, rc, n;
    const char *msg = "simultaneous";
    char rbuf[32];

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    a = socketcall(SC_SOCKET, args);
    if (a < 0)
        fail("simultaneous socket");

    make_sockaddr(sa, sim_port, 127, 0, 0, 1);
    args[0] = (uint32_t)a;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("simultaneous bind");

    if (sys_fcntl(a, F_SETFL, O_NONBLOCK) < 0)
        fail("simultaneous nonblock");

    args[0] = (uint32_t)a;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    rc = socketcall(SC_CONNECT, args);
    if (rc != -EINPROGRESS && rc != 0)
        fail("simultaneous connect");

    if ((wait_for(a, POLLOUT) & (POLLOUT | POLLERR | POLLHUP)) != POLLOUT)
        fail("a socket that dialed its own port is not connected");

    n = 0;
    args[0] = (uint32_t)a;
    args[1] = (unsigned long)msg;
    args[2] = (uint32_t)strlen(msg);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_SENDTO, args) == (int)strlen(msg)) {
        (void)wait_for(a, POLLIN);
        args[0] = (uint32_t)a;
        args[1] = (unsigned long)rbuf;
        args[2] = sizeof(rbuf);
        args[3] = MSG_DONTWAIT;
        args[4] = 0;
        args[5] = 0;
        n = socketcall(SC_RECVFROM, args);
    }
    if (n != (int)strlen(msg) || !mem_eq((const uint8_t *)rbuf, (const uint8_t *)msg, n))
        fail("a socket that dialed its own port did not receive what it sent");
    sys_close(a);
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
    sim_port = (uint16_t)probe_port(7310);
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
    rows_enter("connecting socket");
    tcp_send_connecting();
    rows_enter("bound-port listen socket");
    tcp_ports();
    rows_enter("simultaneous socket");
    tcp_simultaneous();
    rows_report(STEPS, 0);
    print("LOOPPROBE OK\n");
    sys_exit(0);
}
