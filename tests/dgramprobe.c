/**
 * dgramprobe: what a DHCP client needs of an ordinary datagram socket, a peer
 * it can be connected to and permission to broadcast.
 *
 * A client renews its lease by talking to the server it already has, which is
 * `connect` and `write` on a UDP socket, and asks for a lease in the first
 * place by broadcasting, which is `SO_BROADCAST`. What a connected socket
 * hears matters as much as what it sends: a client connected to its server
 * is not answered by whoever else is on the wire.
 *
 * The rows, in order: a socket with no peer refuses a write (EDESTADDRREQ);
 * connected, it writes to its peer and hears its peer alone, neither a third
 * socket on this machine nor the datagram the suite sends from outside at
 * "DGRAMPROBE OUTSIDE"; a connect on a descriptor that is not a socket is
 * ENOTSOCK; AF_UNSPEC disconnects a datagram socket and is EAFNOSUPPORT for a
 * stream; a packet socket cannot be connected; two sockets share a port only
 * when both set SO_REUSEADDR, even with different file-system user ids,
 * and the reply goes to the one connected to the sender; a port below
 * 1024 is root's; a connect beside eight bound sockets answers 0 or
 * EAGAIN; a broadcast is EACCES until SO_BROADCAST is set; the options
 * read back; and SO_BINDTODEVICE takes the machine's interface, by name or
 * as the whole `struct ifreq` BusyBox sends, and no other.
 *
 * The peer is the machine itself at its own address, so both ends of the
 * exchanges are in this process. Prints "DGRAMPROBE OK" and exits 0 on
 * success; a failure prints "DGRAMPROBE FAIL: " with the reason and exits 1.
 * Freestanding; direct system calls (probe.h).
 *
 * Beside its own lines it writes etude's report records (probe_report.h),
 * one assertion for each row, for the case ETUDE_CASE names or
 * machine.dgram.dgramprobe, whose scenario runs it at the debug shell. A
 * failed check ends the probe: its assertion fails and those after it are
 * not run.
 *
 * The machine is QEMU's: its address is 10.0.2.15, its subnet's broadcast
 * 10.0.2.255 and its interface eth0; the probe binds UDP ports 7500 to
 * 7503, 7511 and 7600 to 7607, and the low-port row drops to uid 1000 and
 * binds port 68. A native run gives the machine's own as arguments
 * (probe_args.h): `ifname=NAME` names the interface, whose address and
 * broadcast the probe then reads with SIOCGIFADDR and SIOCGIFBRDADDR unless
 * `addr=A.B.C.D` and `bcast=A.B.C.D` give them, and `ports=OLD:NEW,...` and
 * `ids=OLD:NEW,...` map the ports and the uid. Given `machine=native`, it
 * first receives the outsider's datagram, "outside", which the controller's
 * peer sends each second, on the socket before that socket is connected,
 * so that the run shows the datagram arrived; it then leaves out the check
 * that the connected socket refused the outsider, since the count of
 * refused datagrams it waits on is the machine's, which other traffic
 * moves, and still checks that the socket hears its peer, past any of the
 * outsider's datagrams that arrived before the connect.
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define STDOUT_FILENO 1

#define AF_INET     2
#define AF_UNSPEC   0
#define AF_PACKET   17
#define SOCK_DGRAM  2
#define SOCK_STREAM 1
#define ETH_P_IP_BE 0x0008

#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_BROADCAST    6
#define SO_BINDTODEVICE 25

#define MSG_DONTWAIT 0x40

#define EACCES       13
#define EAGAIN       11
#define EDESTADDRREQ 89
#define ENODEV       19
#define ENOTSOCK     88
#define EADDRINUSE   98
#define EAFNOSUPPORT 97
#define EOPNOTSUPP   95

#define BCAST_IP 0xFFFFFFFFUL

#define SIOCGIFADDR    0x8915
#define SIOCGIFBRDADDR 0x8919
/* Where an ifreq's sockaddr_in has its address, in network order. */
#define IFREQ_SA_ADDR  20

/* The machine's address and its subnet's broadcast, in host order, the name
 * of its interface, and the ports and the uid the rows use: QEMU's, or the
 * values the arguments give. */
static uint32_t our_ip, subnet_bc;
static const char *ifname;
static uint16_t peer_port, other_port, outside_port, renew_port, spare_port, low_port;
static uint16_t filler_port[8];
static int user_id;

/** write(2). */
static int sys_write(int fd, const void *data, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)data, (long)count);
}

/** read(2). */
static int sys_read(int fd, void *data, size_t count)
{
    return syscall3(SYS_READ, (long)fd, (long)data, (long)count);
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

struct timespec_probe {
    long tv_sec;
    long tv_nsec;
};

/** Sleep for `ms` milliseconds (nanosleep(2)). */
static void nap(long ms)
{
    struct timespec_probe ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    /* interval: the UDP counters are read again every 10 ms */
    (void)syscall2(SYS_NANOSLEEP, (long)(&ts), (long)0);
}

/** fork(2). */
static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

/** setuid(2). */
static int sys_setuid(int uid)
{
    return syscall1(SYS_SETUID, (long)uid);
}

/** waitpid(2). */
static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
}

/** The IPv4 address A.B.C.D at `s` in host order, or `fallback` when `s` is
 * NULL or is not one. */
static uint32_t parse_ip(const char *s, uint32_t fallback)
{
    uint32_t ip = 0;
    int part;

    if (s == 0) {
        return fallback;
    }
    for (part = 0; part < 4; part++) {
        uint32_t v = 0;
        int digits = 0;

        while (*s >= '0' && *s <= '9' && digits < 3) {
            v = v * 10 + (uint32_t)(*s++ - '0');
            digits++;
        }
        if (digits == 0 || v > 255 || *s != (part < 3 ? '.' : '\0')) {
            return fallback;
        }
        ip = (ip << 8) | v;
        s++;
    }
    return ip;
}

/** Take the machine's interface, the ports and the uid from the arguments,
 * with QEMU's as the defaults. */
static void take_arguments(unsigned long *sp)
{
    int i;

    probe_args((int)sp[0], (char **)(sp + 1));
    ifname = probe_arg("ifname", "eth0");
    for (i = 0; ifname[i] != '\0'; i++) {
    }
    if (i == 0 || i > 15) {
        ifname = "eth0";
    }
    peer_port = (uint16_t)probe_port(7500);
    other_port = (uint16_t)probe_port(7501);
    outside_port = (uint16_t)probe_port(7502);
    renew_port = (uint16_t)probe_port(7503);
    spare_port = (uint16_t)probe_port(7511);
    low_port = (uint16_t)probe_port(68);
    for (i = 0; i < 8; i++) {
        filler_port[i] = (uint16_t)probe_port(7600 + i);
    }
    user_id = (int)probe_id(1000);
}

/** Write the string `s` to standard output. */
static void say(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    sys_write(STDOUT_FILENO, s, n);
}

/** Print "DGRAMPROBE FAIL: `what`", report it, and exit 1. */
static void fail(const char *what)
{
    say("DGRAMPROBE FAIL: ");
    say(what);
    say("\n");
    report_stop(what);
    sys_exit(1);
}

/* The report's assertions, one for each row, in the order the probe checks
 * them. */
static const char *const rows[] = {
    "no-peer-refused",   "connected-write",     "source-port",       "peer-only",
    "outsider-refused",  "peer-after-outsider", "not-socket",        "unspec-disconnect",
    "unspec-stream",     "packet-connect",      "packet-binding",    "reuse-both",
    "reuse-reply",       "reuse-close",         "low-port",          "connect-beside-bound",
    "broadcast-refused", "broadcast-sent",      "options-read-back", "bind-device-name",
    "bind-device-ifreq"};

static uint8_t sa[16];
static uint8_t buf[64];
/* A `struct ifreq`: a 16-byte name and a union whose largest member,
 * struct ifmap, begins with two unsigned longs; 32 bytes on i386 and 40 on
 * x86-64. */
#if defined __x86_64__
#define IFREQ_SIZE 40
#else
#define IFREQ_SIZE 32
#endif
static uint8_t ifreq[IFREQ_SIZE];

/** The address the interface `ifname` has for `request`, SIOCGIFADDR or
 * SIOCGIFBRDADDR, in host order; fails the probe with `what` when the
 * interface does not answer. */
static uint32_t interface_ip(int request, const char *what)
{
    unsigned long args[3];
    uint32_t ip = 0;
    int fd, i;

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0) {
        fail("the socket that reads the interface's addresses did not open");
    }
    for (i = 0; i < IFREQ_SIZE; i++) {
        ifreq[i] = 0;
    }
    for (i = 0; ifname[i] != '\0'; i++) {
        ifreq[i] = (uint8_t)ifname[i];
    }
    if (syscall3(SYS_IOCTL, (long)fd, (long)request, (long)ifreq) != 0) {
        fail(what);
    }
    for (i = IFREQ_SA_ADDR; i < IFREQ_SA_ADDR + 4; i++) {
        ip = (ip << 8) | ifreq[i];
    }
    sys_close(fd);
    return ip;
}

/** Take the machine's address and its subnet's broadcast: those `addr=` and
 * `bcast=` give, those of the interface `ifname=` names when it is given,
 * or QEMU's, 10.0.2.15 and 10.0.2.255. */
static void take_addresses(void)
{
    const char *addr = probe_arg("addr", 0), *bcast = probe_arg("bcast", 0);
    int named = probe_arg("ifname", 0) != 0;

    if (addr == 0 && named) {
        our_ip = interface_ip(SIOCGIFADDR, "the interface's address would not read");
    } else {
        our_ip = parse_ip(addr, 0x0A00020FUL);
    }
    if (bcast == 0 && named) {
        subnet_bc = interface_ip(SIOCGIFBRDADDR, "the interface's broadcast would not read");
    } else {
        subnet_bc = parse_ip(bcast, 0x0A0002FFUL);
    }
}

/** Fill the sockaddr_in `sa` with `ip` and `port`, both in host order. */
static void set_sockaddr(uint32_t ip, uint16_t port)
{
    int i;

    for (i = 0; i < 16; i++) {
        sa[i] = 0;
    }
    sa[0] = AF_INET;
    sa[2] = (uint8_t)(port >> 8);
    sa[3] = (uint8_t)port;
    sa[4] = (uint8_t)(ip >> 24);
    sa[5] = (uint8_t)(ip >> 16);
    sa[6] = (uint8_t)(ip >> 8);
    sa[7] = (uint8_t)ip;
}

/** A datagram socket bound to `port`: its descriptor, or -1 when the socket
 * or the bind fails. */
static int open_bound_maybe(uint16_t port)
{
    unsigned long args[6];
    int fd;

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0) {
        return -1;
    }
    set_sockaddr(0, port);
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0) {
        sys_close(fd);
        return -1;
    }
    return fd;
}

/** The same, failing the probe when the socket or the bind fails. */
static int open_bound(uint16_t port)
{
    unsigned long args[6];
    int fd;

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0) {
        fail("a datagram socket did not open");
    }
    set_sockaddr(0, port);
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0) {
        fail("a datagram socket did not bind");
    }
    return fd;
}

/**
 * The machine's count of UDP datagrams that found no socket to take them,
 * NoPorts of the Udp line of /proc/net/snmp; fails the probe when the file
 * or the line cannot be read. A datagram to a port whose only socket is
 * connected to another sender is one of them.
 */
static uint32_t udp_no_ports(void)
{
    static char text[2048];
    int fd, n = 0, r, i;
    uint32_t v = 0;

    fd = syscall3(SYS_OPEN, (long)"/proc/net/snmp", 0, 0);
    if (fd < 0) {
        fail("/proc/net/snmp did not open");
    }
    while (n < (int)sizeof(text) - 1 && (r = sys_read(fd, text + n, sizeof(text) - 1 - n)) > 0) {
        n += r;
    }
    sys_close(fd);
    text[n] = '\0';
    for (i = 0; i + 6 < n; i++) {
        if (text[i] == '\n' && text[i + 1] == 'U' && text[i + 2] == 'd' && text[i + 3] == 'p' &&
            text[i + 4] == ':' && text[i + 5] == ' ' && text[i + 6] >= '0' && text[i + 6] <= '9') {
            break;
        }
    }
    if (i + 6 >= n) {
        fail("/proc/net/snmp has no Udp counts");
    }
    for (i += 6; text[i] >= '0' && text[i] <= '9'; i++) {
    }
    for (i++; text[i] >= '0' && text[i] <= '9'; i++) {
        v = v * 10 + (uint32_t)(text[i] - '0');
    }
    return v;
}

/** Fail with `what` unless the socket `fd` has nothing to read. */
static void expect_nothing(int fd, const char *what)
{
    unsigned long args[6];

    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = MSG_DONTWAIT;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != -EAGAIN) {
        fail(what);
    }
}

/**
 * Wait until a datagram has found no socket since /proc/net/snmp counted
 * `base` of them, which is the datagram sent to `fd`, a socket connected to
 * another sender, when `fd` does not take it; `fd` must have nothing to read
 * meanwhile and then, or the probe fails with `what`.
 */
static void expect_refused(uint32_t base, int fd, const char *what)
{
    while (udp_no_ports() == base) {
        expect_nothing(fd, what);
        nap(10);
    }
    expect_nothing(fd, what);
}

/** Receive one datagram on socket `fd` into buf, waiting for it; give its
 * length, or a negative error. */
static int receive(int fd)
{
    unsigned long args[6];

    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    return socketcall(SC_RECVFROM, args);
}

/** Tell whether a receive of `r` bytes into buf took the outsider's
 * datagram, "outside". */
static int is_outside(int r)
{
    static const char text[] = "outside";
    int i;

    if (r != 7) {
        return 0;
    }
    for (i = 0; i < 7; i++) {
        if (buf[i] != (uint8_t)text[i]) {
            return 0;
        }
    }
    return 1;
}

/** Open the socket of the outside case on its port; in a native run, also
 * receive there the outsider's datagram, which the controller's peer sends
 * each second, or fail. */
static int open_outside(void)
{
    int fd = open_bound(outside_port);

    if (probe_native() && !is_outside(receive(fd))) {
        fail("the outsider's datagram did not arrive");
    }
    return fd;
}

/** Receive one datagram on socket `fd`, past, in a native run, the
 * outsider's datagrams that arrived before `fd` was connected; give its
 * length, or a negative error. */
static int receive_past_outsider(int fd)
{
    int r;

    do {
        r = receive(fd);
    } while (probe_native() && is_outside(r));
    return r;
}

/** Bind two users' sockets to the renewal port, refusing the second
 * until both set SO_REUSEADDR; return it and keep the first in `*out`. */
static int reuse_socket(int *out)
{
    unsigned long args[6];
    int listener, sender, on;

    listener = open_bound(renew_port);
    if (syscall1(SYS_SETFSUID, user_id) != 0 || syscall1(SYS_SETFSUID, -1) != user_id) {
        fail("the renewal socket's file-system user id did not change");
    }
    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    sender = socketcall(SC_SOCKET, args);
    if (sender < 0) {
        fail("the renewal socket did not open");
    }
    set_sockaddr(0, renew_port);
    args[0] = (uint32_t)sender;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != -EADDRINUSE) {
        fail("a second socket took a bound port without asking to share it");
    }
    on = 1;
    args[0] = (uint32_t)sender;
    args[1] = SOL_SOCKET;
    args[2] = SO_REUSEADDR;
    args[3] = (unsigned long)&on;
    args[4] = sizeof(on);
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("SO_REUSEADDR was refused on the renewal socket");
    }
    set_sockaddr(0, renew_port);
    args[0] = (uint32_t)sender;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != -EADDRINUSE) {
        fail("one socket asking to share was enough");
    }
    args[0] = (uint32_t)listener;
    args[1] = SOL_SOCKET;
    args[2] = SO_REUSEADDR;
    args[3] = (unsigned long)&on;
    args[4] = sizeof(on);
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("SO_REUSEADDR was refused on the listener");
    }
    set_sockaddr(0, renew_port);
    args[0] = (uint32_t)sender;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0) {
        fail("two users' sockets that both asked to share a port could not");
    }
    if (syscall1(SYS_SETFSUID, 0) != user_id || syscall1(SYS_SETFSUID, -1) != 0) {
        fail("the file-system user id did not return to root");
    }
    *out = listener;
    return sender;
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe, entered with `sp` at argc: the rows of the list above, in its
 * order.
 *
 * A datagram to the machine's own address goes on the loopback ring, which
 * the kernel delivers after the send returns, so the first read of each
 * exchange waits for it, and a read that expects nothing waits first for
 * /proc/net/snmp to count the datagram as refused (`expect_refused`). The
 * socket that ignores the outsider must still hear its peer afterwards, or
 * its silence would show only that nothing was sent. The eight packet
 * sockets, opened and closed, show that none of them took a UDP binding.
 * The connect beside eight bound sockets has to find an ephemeral port, and
 * answers at once, 0 or EAGAIN.
 */
void probe_main(unsigned long *sp)
{
    unsigned long args[6];
    int peer, other, client, outside, stream, extra, listener, sender, r, i;
    int fillers[8];
    int pid, status;
    /* The kernel writes it through the pointer getsockopt is given. */
    volatile int on;
    uint16_t client_port;
    uint32_t refused;
    /* The kernel writes it through the pointer the call is given. */
    volatile uint32_t optlen;
    uint32_t name_len = 0;

    take_arguments(sp);
    report_begin(report_case((char **)(sp + 2 + sp[0]), "machine.dgram.dgramprobe"));
    for (i = 0; i < (int)(sizeof(rows) / sizeof(rows[0])); i++) {
        report_plan(rows[i]);
    }
    take_addresses();
    while (ifname[name_len] != '\0') {
        name_len++;
    }
    peer = open_bound(peer_port);
    other = open_bound(other_port);

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    client = socketcall(SC_SOCKET, args);
    if (client < 0) {
        fail("the client socket did not open");
    }

    /* A socket with nowhere to send says so instead of sending nowhere. */
    if (sys_write(client, "x", 1) != -EDESTADDRREQ) {
        fail("a write with no peer was not refused");
    }
    report_held();

    /* Connected, it writes to its peer without being told again. */
    set_sockaddr(our_ip, peer_port);
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("connecting a datagram socket failed");
    }
    if (sys_write(client, "hello", 5) != 5) {
        fail("a write to the connected peer failed");
    }
    args[0] = (uint32_t)peer;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    r = socketcall(SC_RECVFROM, args);
    if (r != 5 || buf[0] != 'h' || buf[4] != 'o') {
        fail("what the peer received is not what was written");
    }
    report_held();

    /* And it hears from that peer and from nobody else. */
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)sa;
    optlen = 16;
    args[2] = (unsigned long)&optlen;
    if (socketcall(SC_GETSOCKNAME, args) != 0) {
        fail("the client's own address did not read");
    }
    client_port = (uint16_t)(((uint16_t)sa[2] << 8) | sa[3]);
    if (client_port == 0) {
        fail("a connected datagram socket has no source port for a reply");
    }
    report_held();

    set_sockaddr(our_ip, client_port);
    args[0] = (uint32_t)peer;
    args[1] = (unsigned long)"peer";
    args[2] = 4;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 4) {
        fail("the peer's reply did not send");
    }
    refused = udp_no_ports();
    set_sockaddr(our_ip, client_port);
    args[0] = (uint32_t)other;
    args[1] = (unsigned long)"other";
    args[2] = 5;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 5) {
        fail("the third socket's datagram did not send");
    }

    /* The peer's reply arrives, in either order, and the other's does not. */
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    r = socketcall(SC_RECVFROM, args);
    if (r != 4 || buf[0] != 'p') {
        fail("a connected socket did not receive from its peer");
    }
    expect_refused(refused, client,
                   "a connected socket received from a sender it is not connected to");
    report_held();

    /* A sender at another address: the suite's datagram, through a forward. */
    outside = open_outside();
    set_sockaddr(our_ip, peer_port);
    args[0] = (uint32_t)outside;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("connecting the socket for the outside case failed");
    }
    refused = udp_no_ports();
    say("DGRAMPROBE OUTSIDE\n");
    if (!probe_native()) {
        expect_refused(refused, outside,
                       "a connected socket received from an address that is not its peer's");
    }
    report_held();
    /* The same socket still hears its peer. */
    set_sockaddr(our_ip, outside_port);
    args[0] = (uint32_t)peer;
    args[1] = (unsigned long)"peer";
    args[2] = 4;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 4) {
        fail("the peer's datagram to the outside-case socket did not send");
    }
    if (receive_past_outsider(outside) != 4) {
        fail("the socket that ignored the outsider also ignored its peer");
    }
    sys_close(outside);
    report_held();

    /* The descriptor is checked before the address. */
    args[0] = 1; /* the console */
    args[1] = 0;
    args[2] = 2;
    if (socketcall(SC_CONNECT, args) != -ENOTSOCK) {
        fail("a connect on a descriptor that is not a socket reported the address instead");
    }
    report_held();

    /* A two-byte AF_UNSPEC address disconnects a datagram socket. */
    set_sockaddr(0, 0);
    sa[0] = AF_UNSPEC;
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)sa;
    args[2] = 2;
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("disconnecting a datagram socket failed");
    }
    if (sys_write(client, "x", 1) != -EDESTADDRREQ) {
        fail("a write after disconnecting was not refused");
    }
    report_held();

    /* A stream socket has no disconnect. */
    set_sockaddr(0, 0);
    sa[0] = AF_UNSPEC;
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    stream = socketcall(SC_SOCKET, args);
    if (stream < 0) {
        fail("the stream socket did not open");
    }
    args[0] = (uint32_t)stream;
    args[1] = (unsigned long)sa;
    args[2] = 2;
    if (socketcall(SC_CONNECT, args) != -EAFNOSUPPORT) {
        fail("a stream socket accepted a two-byte disconnect");
    }
    sys_close(stream);
    report_held();

    /* A packet socket has no peer, and takes no UDP binding. */
    for (i = 0; i < 8; i++) {
        int pkt;

        args[0] = AF_PACKET;
        args[1] = SOCK_DGRAM;
        args[2] = ETH_P_IP_BE;
        pkt = socketcall(SC_SOCKET, args);
        if (pkt < 0) {
            fail("a packet socket did not open");
        }
        set_sockaddr(our_ip, peer_port);
        args[0] = (uint32_t)pkt;
        args[1] = (unsigned long)sa;
        args[2] = 16;
        if (socketcall(SC_CONNECT, args) != -EOPNOTSUPP) {
            fail("a packet socket was connected like a datagram one");
        }
        sys_close(pkt);
    }
    report_held();
    if (open_bound(spare_port) < 0) {
        fail("the UDP binding table was exhausted by packet sockets");
    }
    report_held();

    sender = reuse_socket(&listener);
    report_held();

    /* The sender connects to the peer, so the peer's answer is its own. */
    set_sockaddr(our_ip, peer_port);
    args[0] = (uint32_t)sender;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("the renewal socket could not connect to the server");
    }
    if (sys_write(sender, "renew", 5) != 5) {
        fail("the renewal did not send");
    }
    args[0] = (uint32_t)peer;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != 5) {
        fail("the server did not receive the renewal");
    }
    set_sockaddr(our_ip, renew_port);
    args[0] = (uint32_t)peer;
    args[1] = (unsigned long)"ack";
    args[2] = 3;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 3) {
        fail("the server's answer did not send");
    }
    args[0] = (uint32_t)sender;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != 3 || buf[0] != 'a') {
        fail("the answer went to the listener instead of the socket that asked for it");
    }
    args[0] = (uint32_t)listener;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = MSG_DONTWAIT;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != -EAGAIN) {
        fail("the listener took a datagram the connected socket had asked for");
    }
    report_held();

    /* Closing the sender leaves the listener bound. */
    sys_close(sender);
    set_sockaddr(our_ip, renew_port);
    args[0] = (uint32_t)peer;
    args[1] = (unsigned long)"after";
    args[2] = 5;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 5) {
        fail("the datagram after the close did not send");
    }
    args[0] = (uint32_t)listener;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != 5) {
        fail("closing one of two sockets on a port unbound the other");
    }
    sys_close(listener);
    report_held();

    /* A port below 1024 is root's: uid 1000 binding port 68 is EACCES. */
    pid = sys_fork();
    if (pid == 0) {
        int low;

        args[0] = AF_INET;
        args[1] = SOCK_DGRAM;
        args[2] = 0;
        low = socketcall(SC_SOCKET, args);
        if (low < 0 || sys_setuid(user_id) != 0) {
            sys_exit(2);
        }
        set_sockaddr(0, low_port);
        args[0] = (uint32_t)low;
        args[1] = (unsigned long)sa;
        args[2] = 16;
        sys_exit(socketcall(SC_BIND, args) == -EACCES ? 0 : 3);
    }
    if (pid < 0) {
        fail("the fork for the low-port case failed");
    }
    status = -1;
    if (sys_waitpid(pid, &status, 0) != pid) {
        fail("waiting for the low-port case failed");
    }
    if ((status & 0x7f) != 0) {
        fail("the low-port case died on a signal");
    }
    if (((status >> 8) & 0xff) != 0) {
        fail("an ordinary user bound a port below 1024");
    }
    report_held();

    /* A connect beside eight bound sockets answers 0 or EAGAIN. */
    for (i = 0; i < 8; i++) {
        fillers[i] = open_bound_maybe(filler_port[i]);
    }
    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    extra = socketcall(SC_SOCKET, args);
    if (extra >= 0) {
        set_sockaddr(our_ip, peer_port);
        args[0] = (uint32_t)extra;
        args[1] = (unsigned long)sa;
        args[2] = 16;
        r = socketcall(SC_CONNECT, args);
        if (r != 0 && r != -EAGAIN) {
            fail("a connect with a full binding table answered with something else");
        }
        sys_close(extra);
    }
    for (i = 0; i < 8; i++) {
        if (fillers[i] >= 0) {
            sys_close(fillers[i]);
        }
    }
    report_held();

    /* Broadcasting is asked for, not assumed. */
    set_sockaddr(BCAST_IP, peer_port);
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)"b";
    args[2] = 1;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != -EACCES) {
        fail("a broadcast was sent by a socket that never asked to broadcast");
    }
    set_sockaddr(subnet_bc, peer_port);
    args[4] = (unsigned long)sa;
    if (socketcall(SC_SENDTO, args) != -EACCES) {
        fail("the subnet's broadcast address was not treated as a broadcast");
    }
    report_held();

    on = 1;
    args[0] = (uint32_t)client;
    args[1] = SOL_SOCKET;
    args[2] = SO_BROADCAST;
    args[3] = (unsigned long)&on;
    args[4] = sizeof(on);
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("SO_BROADCAST was refused");
    }
    set_sockaddr(BCAST_IP, peer_port);
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)"b";
    args[2] = 1;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 1) {
        fail("a broadcast was refused after the socket asked to broadcast");
    }
    report_held();

    /* The options read back. */
    on = 0;
    args[0] = (uint32_t)client;
    args[1] = SOL_SOCKET;
    args[2] = SO_BROADCAST;
    args[3] = (unsigned long)&on;
    optlen = sizeof(on);
    args[4] = (unsigned long)&optlen;
    if (socketcall(SC_GETSOCKOPT, args) != 0 || on != 1) {
        fail("SO_BROADCAST did not read back as set");
    }
    on = 1;
    args[1] = SOL_SOCKET;
    args[2] = SO_REUSEADDR;
    args[3] = (unsigned long)&on;
    args[4] = sizeof(on);
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("SO_REUSEADDR was refused");
    }
    on = 0;
    args[3] = (unsigned long)&on;
    optlen = sizeof(on);
    args[4] = (unsigned long)&optlen;
    if (socketcall(SC_GETSOCKOPT, args) != 0 || on != 1) {
        fail("SO_REUSEADDR did not read back as set");
    }
    report_held();

    /* SO_BINDTODEVICE takes the machine's interface and no other. */
    args[0] = (uint32_t)client;
    args[1] = SOL_SOCKET;
    args[2] = SO_BINDTODEVICE;
    args[3] = (unsigned long)ifname;
    args[4] = name_len + 1;
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("binding to this machine's interface was refused");
    }
    args[3] = (unsigned long)"eth9";
    args[4] = 5;
    if (socketcall(SC_SETSOCKOPT, args) != -ENODEV) {
        fail("binding to an interface this machine does not have was allowed");
    }
    args[3] = (unsigned long)"";
    args[4] = 1;
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("an empty name did not unbind");
    }
    report_held();
    /* BusyBox sends a whole `struct ifreq`, with the name at the front. */
    for (i = 0; i < IFREQ_SIZE; i++) {
        ifreq[i] = 0;
    }
    for (i = 0; i < (int)name_len && i < 15; i++) {
        ifreq[i] = (uint8_t)ifname[i];
    }
    args[3] = (unsigned long)ifreq;
    args[4] = IFREQ_SIZE;
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("the whole struct ifreq a DHCP client sends was refused");
    }
    /* It reads back as the name, five bytes with the NUL for eth0. */
    for (i = 0; i < IFREQ_SIZE; i++) {
        ifreq[i] = 0x5c;
    }
    optlen = IFREQ_SIZE;
    args[3] = (unsigned long)ifreq;
    args[4] = (unsigned long)&optlen;
    if (socketcall(SC_GETSOCKOPT, args) != 0) {
        fail("the bound interface did not read back");
    }
    if (optlen != name_len + 1 || ifreq[0] != (uint8_t)ifname[0] ||
        ifreq[name_len - 1] != (uint8_t)ifname[name_len - 1] || ifreq[name_len] != 0) {
        fail("the bound interface did not read back as its name");
    }
    report_held();

    sys_close(client);
    sys_close(other);
    sys_close(peer);
    report_end();
    say("DGRAMPROBE OK\n");
    sys_exit(0);
}
