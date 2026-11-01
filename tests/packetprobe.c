/**
 * packetprobe: the packet socket, the tap a DHCP client sees replies through.
 *
 * `ip_rx` drops every datagram not addressed to this machine, and a DHCP reply
 * is always one of those, so a client reads its replies from an
 * `AF_PACKET/SOCK_DGRAM` socket bound to the interface instead. This probe
 * opens one, checks what it refuses, and then does the exchange for real: it
 * builds a DHCP discover, sends it through the socket to the broadcast MAC, and
 * waits for the offer SLIRP broadcasts back. That proves both halves at once.
 * The frame must have been well formed or nothing would answer it, and the
 * answer is addressed to 255.255.255.255, which `ip_rx` drops, so receiving it
 * is exactly the case the socket exists for.
 *
 * A native run (probe_args.h) gives the addresses of its own network in
 * place of QEMU's: `gateway=A.B.C.D`, which answers a ping, in place of
 * 10.0.2.2, and `peer=A.B.C.D:PORT`, whose address answers the probe's ARP
 * request, in place of the name server 10.0.2.3; `ifname=NAME` and
 * `addr=A.B.C.D/PREFIX`, the interface and its address; and
 * `ids=OLD:NEW,...` for the ordinary user 1000. With `machine=native` the
 * probe takes the interface of the default route when no `ifname` is given
 * (probe_netif.h), reads its index, and its address when no `addr` is
 * given, from the machine in place of 1 and 10.0.2.15, and leaves out the
 * discovers and their offers, which need a DHCP server on a network of their
 * own, and the receives that need a socket nothing else reaches.
 *
 * Prints "PACKETPROBE OK" and exits 0 on success, or "PACKETPROBE FAIL: " and
 * the reason and exits 1. Beside its own lines it writes etude's report
 * records (probe_report.h) for the case ETUDE_CASE names or
 * guest.packet.packetprobe: one assertion for each check, in the order of
 * `path_rows`, and the branch of the group `network`: `own` on QEMU's
 * network, or `shared` with `machine=native`, whose path leaves out the
 * rows named above.
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_netif.h"
#include "probe_report.h"

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define STDOUT_FILENO 1

#define SIOCGIFADDR   0x8915
#define SIOCGIFHWADDR 0x8927
#define SIOCGIFINDEX  0x8933

#define AF_INET     2
#define AF_PACKET   17
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

/* The protocol argument is in network byte order, as Linux takes it. */
#define ETH_P_IP_BE   0x0008
#define ETH_P_ARP_BE  0x0608
#define ETH_P_IPV6_BE 0xDD86

#define SOL_PACKET     263
#define PACKET_AUXDATA 8

#define EPERM           1
#define EINVAL          22
#define ENODEV          19
#define ENOPROTOOPT     92
#define ESOCKTNOSUPPORT 94
#define EAGAIN          11

#define IPPROTO_UDP 17

/* The discover this probe sends and the offer it waits for. The transaction
 * id ties the two together; SLIRP echoes it back. */
#define DHCP_XID_0 0x52
#define DHCP_XID_1 0x41
#define DHCP_XID_2 0x43
#define DHCP_XID_3 0x32
#define DHCP_LEN   272

/** The write call: the count written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** The close call: 0, or a negated errno. */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** The socket operation `call`, an SC_ number, through `sock_call`. */
static int socketcall(int call, unsigned long *args)
{
    return (int)sock_call(call, args);
}

#define POLLIN 0x001

struct timeval_probe {
    long tv_sec;
    long tv_usec;
};

/** The fork call: the child's pid, 0 in the child, or a negated errno. */
static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

/** The setuid call: 0, or a negated errno. */
static int sys_setuid(int uid)
{
    return syscall1(SYS_SETUID, (long)uid);
}

/** Wait for the child `pid`: the pid reaped, or a negated errno. */
static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
}

/** The one's-complement sum an IP header has; SLIRP checks it, so a wrong
 * one is an offer that never comes, not a visible failure. */
static uint16_t ip_checksum(const uint8_t *p, unsigned int len)
{
    unsigned long sum = 0;
    unsigned int i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((unsigned long)p[i] << 8) | p[i + 1];
    }
    if (i < len) {
        sum += (unsigned long)p[i] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xffff);
}

struct pollfd_probe {
    int fd;
    short events;
    short revents;
};

/** The poll call with no timeout: blocks until a descriptor of `fds` is
 * ready, and returns the count of ready descriptors or a negated errno. */
static int sys_poll_wait(struct pollfd_probe *fds, unsigned int n)
{
    return syscall3(SYS_POLL, (long)fds, (long)n, (long)-1);
}

/** The select call: the count of ready descriptors, 0 at the timeout, or a
 * negated errno. */
static int sys_select(int n, void *rd, void *wr, void *ex, void *tv)
{
    return syscall5(SYS_SELECT, (long)n, (long)rd, (long)wr, (long)ex, (long)tv);
}

/** Write `s` to standard output. */
static void say(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    sys_write(STDOUT_FILENO, s, n);
}

/** The ioctl call: its result, or a negated errno. */
static int sys_ioctl(int fd, unsigned int req, void *arg)
{
    return syscall3(SYS_IOCTL, (long)fd, (long)req, (long)arg);
}

/** Open `path` read-only: the descriptor, or a negated errno. */
static int sys_open_ro(const char *path)
{
    return syscall3(SYS_OPEN, (long)path, (long)0, (long)0);
}

/** The read call: the count read, 0 at the end, or a negated errno. */
static int sys_read(int fd, void *buf, int n)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)n);
}

/**
 * Read up to `cap` - 1 bytes of the file `path` into `out` and end them with
 * a NUL: the count read, or the negated errno of the open.
 */
static int read_file(const char *path, char *out, int cap)
{
    int fd, total = 0, n;

    fd = sys_open_ro(path);
    if (fd < 0) {
        return fd;
    }
    while (total < cap - 1) {
        n = sys_read(fd, out + total, cap - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    sys_close(fd);
    out[total] = '\0';
    return total;
}

/** 1 if `needle` occurs in the first `len` bytes of `hay`, else 0. */
static int contains(const char *hay, int len, const char *needle)
{
    int nl = 0, i, j;

    while (needle[nl]) {
        nl++;
    }
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

/* The assertions of the probe, in the order it checks them. Those of the
 * alternative `own` of the group `network` are checked only on QEMU's
 * network; the alternative `shared`, a native run's, has none. */
static const struct {
    const char *id;
    int own;
} path_rows[] = {
    {"stream-refused", 0},
    {"ipv6-open", 0},
    {"open", 0},
    {"auxdata-refused", 0},
    {"bind-missing", 0},
    {"bind", 0},
    {"discover-sent", 1},
    {"not-mac-refused", 0},
    {"offer-polled", 1},
    {"select-agrees", 1},
    {"offer-ipv4", 1},
    {"no-control", 1},
    {"offer-address", 1},
    {"no-flags", 1},
    {"bootp-broadcast", 1},
    {"empty-eagain", 1},
    {"no-address-written", 1},
    {"second-offer", 1},
    {"true-length", 1},
    {"truncated-address", 1},
    {"capacity", 1},
    {"refused-bind-keeps", 0},
    {"third-offer", 1},
    {"rebind-drops", 1},
    {"arp-bind", 0},
    {"hwaddr", 0},
    {"getsockname-whole", 0},
    {"getsockname-bound", 0},
    {"getsockname-mac", 0},
    {"ip-bind", 0},
    {"arp-request-sent", 0},
    {"gateway-ping", 0},
    {"arp-only", 0},
    {"ipv4-only", 0},
    {"both-answered", 0},
    {"arp-cache", 0},
    {"ipv6-frame", 1},
    {"reopen", 0},
    {"table-read", 0},
    {"bind-listed", 0},
    {"close-frees", 0},
    {"unprivileged-refused", 0},
};

/** Plan the path of `path_rows`, with the rows of `own` when `own` is
 * nonzero, and report the branch of the group `network`. */
static void plan(int own)
{
    unsigned int k;

    for (k = 0; k < sizeof(path_rows) / sizeof(path_rows[0]); k++) {
        if (own || !path_rows[k].own) {
            report_plan(path_rows[k].id);
        }
    }
    report_branch("network", own ? "own" : "shared");
}

/** 1 if the strings `a` and `b` are equal, else 0. */
static int same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * Report as held each assertion of the path before `id`, or each one left
 * when `id` is NULL; an `id` the path has passed changes nothing.
 *
 * A check inside a loop is made once for each frame, so the probe reports
 * the loop's rows after the loop, or at a failure, when `fail` reaches the
 * failed row: the checks before it held for every frame so far.
 */
static void reach(const char *id)
{
    int k = report_path_at;

    while (k < report_path_len && (id == 0 || !same(report_path[k], id))) {
        k++;
    }
    if (id != 0 && k == report_path_len) {
        return;
    }
    while (report_path_at < k) {
        report_held();
    }
}

/**
 * Print "PACKETPROBE FAIL: ", `what` and the number `n` in parentheses,
 * report that the assertion `id` failed, and exit 1. The number tells the
 * reader of the log which errno the refusal was.
 */
static void fail_n(const char *id, const char *what, int n)
{
    char buf[12];
    int i = (int)sizeof(buf);
    unsigned int v = (unsigned int)(n < 0 ? -n : n);

    buf[--i] = '\0';
    do {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v && i > 1);
    if (n < 0) {
        buf[--i] = '-';
    }
    say("PACKETPROBE FAIL: ");
    say(what);
    say(" (");
    say(&buf[i]);
    say(")\n");
    reach(id);
    report_stop(what);
    sys_exit(1);
}

/** Print "PACKETPROBE FAIL: " and `what`, report that the assertion `id`
 * failed with it, and exit 1. */
static void fail(const char *id, const char *what)
{
    say("PACKETPROBE FAIL: ");
    say(what);
    say("\n");
    reach(id);
    report_stop(what);
    sys_exit(1);
}

/* struct sockaddr_ll: family, protocol, ifindex, hatype, pkttype, halen,
 * addr. */
struct sockaddr_ll_probe {
    uint16_t sll_family;
    uint16_t sll_protocol;
    int sll_ifindex;
    uint16_t sll_hatype;
    uint8_t sll_pkttype;
    uint8_t sll_halen;
    uint8_t sll_addr[8];
};

/* An all-zero address, for clearing one before a call fills it. */
static const struct sockaddr_ll_probe zero_sll;

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

/* An fd_set is 1024 bits, and select reads and writes the whole object
 * however few descriptors the caller names. */
#define FDSET_WORDS_PROBE 32
static unsigned int readfds[FDSET_WORDS_PROBE];

static uint8_t frame[1600];
static uint8_t discover[DHCP_LEN];
static uint8_t sender[24];
static uint8_t control[64];

/* The interface and the addresses the rows use: QEMU's, or those a native
 * run gives or reads (`interface`). Addresses are in host byte order. */
static const char *if_name;
static int if_index;
static uint32_t self_ip, gateway_ip, asked_ip;

/** The IPv4 address written in dots at the start of `s`, in host byte order,
 * or `fallback` when `s` is NULL or does not begin with one. */
static uint32_t arg_ip(const char *s, uint32_t fallback)
{
    uint32_t ip = 0;
    long part;
    int i;

    for (i = 0; s != 0 && i < 4; i++) {
        part = probe_number(&s);
        if (part < 0 || part > 255 || (i < 3 && *s++ != '.')) {
            return fallback;
        }
        ip = (ip << 8) | (uint32_t)part;
    }
    return s != 0 ? ip : fallback;
}

/** Fill the 40-byte ifreq `ifr` with zeros and the interface's name. */
static void ifreq_for(unsigned char *ifr)
{
    unsigned int i;

    for (i = 0; i < 40; i++) {
        ifr[i] = 0;
    }
    for (i = 0; i < 15 && if_name[i] != '\0'; i++) {
        ifr[i] = (unsigned char)if_name[i];
    }
}

/**
 * Set the interface and the addresses from the arguments. Given
 * `machine=native`, read the interface's index from the machine through
 * SIOCGIFINDEX on the socket `fd`, and its address through SIOCGIFADDR
 * unless `addr=A.B.C.D/PREFIX` gives it.
 */
static void interface(int fd)
{
    unsigned char ifr[40];

    if_name = probe_ifname();
    if_index = 1;
    self_ip = 0x0A00020Fu; /* 10.0.2.15 */
    gateway_ip = arg_ip(probe_arg("gateway", 0), 0x0A000202u);
    asked_ip = arg_ip(probe_arg("peer", 0), 0x0A000203u);
    if (!probe_native()) {
        return;
    }
    ifreq_for(ifr);
    if (sys_ioctl(fd, SIOCGIFINDEX, ifr) != 0) {
        fail("open", "the interface would not report its index");
    }
    if_index = (int)(ifr[16] | (ifr[17] << 8) | (ifr[18] << 16) | ((unsigned)ifr[19] << 24));
    ifreq_for(ifr);
    if (sys_ioctl(fd, SIOCGIFADDR, ifr) != 0) {
        fail("open", "the interface would not report its address");
    }
    self_ip = arg_ip(probe_arg("addr", 0), ((uint32_t)ifr[20] << 24) | ((uint32_t)ifr[21] << 16) |
                                               ((uint32_t)ifr[22] << 8) | ifr[23]);
}

/** Write `ip`, in host byte order, in dots and then a space, to `out`, which
 * has room for 17 bytes, and end it with a NUL. */
static void dotted(char *out, uint32_t ip)
{
    int i, k = 0, shift;

    for (shift = 24; shift >= 0; shift -= 8) {
        unsigned int b = (ip >> shift) & 0xFFu;
        char d[3];
        int n = 0;

        do {
            d[n++] = (char)('0' + b % 10u);
            b /= 10u;
        } while (b != 0);
        for (i = n - 1; i >= 0; i--) {
            out[k++] = d[i];
        }
        out[k++] = shift > 0 ? '.' : ' ';
    }
    out[k] = '\0';
}

/**
 * Write the inode of the socket on descriptor `fd` into `out`, which has
 * room for `cap` bytes, as the decimal digits and a NUL that its
 * /proc/self/fd link, `socket:[N]`, gives: the count of digits, or 0 when the
 * link cannot be read or names no socket.
 */
static int socket_inode(int fd, char *out, int cap)
{
    static const char prefix[] = "/proc/self/fd/";
    char path[sizeof(prefix) + 12], link[48];
    int k, n, i, d;

    for (k = 0; prefix[k] != '\0'; k++) {
        path[k] = prefix[k];
    }
    for (d = 1; fd / d >= 10; d *= 10) {
    }
    for (; d > 0; d /= 10) {
        path[k++] = (char)('0' + fd / d % 10);
    }
    path[k] = '\0';
    n = (int)syscall3(SYS_READLINK, (long)path, (long)link, (long)sizeof(link));
    if (n < 10 || n >= (int)sizeof(link) || link[n - 1] != ']') {
        return 0;
    }
    link[n] = '\0';
    if (!contains(link, 8, "socket:[")) {
        return 0;
    }
    for (i = 8, k = 0; i < n - 1 && k < cap - 1; i++) {
        out[k++] = link[i];
    }
    out[k] = '\0';
    return k;
}

/**
 * The rows of /proc/net/packet whose last column, the inode, is `inode`, or
 * -1 when the file cannot be read or lacks Linux's heading.
 */
static int packet_rows(const char *inode)
{
    char *text = (char *)frame;
    int n = read_file("/proc/net/packet", text, (int)sizeof(frame));
    int i, end, start, k, rows = 0;

    if (n <= 0 || !contains(text, n, "Inode\n")) {
        return -1;
    }
    for (i = 0; i < n && text[i] != '\n'; i++) {
    }
    while (++i < n) {
        for (end = i; end < n && text[end] != '\n'; end++) {
        }
        for (k = end; k > i && text[k - 1] == ' '; k--) {
        }
        for (start = k; start > i && text[start - 1] != ' '; start--) {
        }
        text[k] = '\0';
        rows += same(text + start, inode);
        i = end;
    }
    return rows;
}

/**
 * Open, bind and close four packet sockets in a row, and fail unless each
 * bind puts the socket in the kernel's table of packet bindings and each
 * close takes it out.
 *
 * A binding is a record on the list the receive path walks, and
 * /proc/net/packet has a row for each, with the inode its socket's
 * /proc/self/fd link gives. A close that kept the binding would leave the
 * row, and frames would go on being queued for a socket nobody can read. The
 * check reads only the probe's own rows, so the other sockets of the machine
 * do not move it.
 */
static void check_close_releases(void)
{
    unsigned long args[3];
    struct sockaddr_ll_probe sll;
    char inode[12];
    int fd, r, k;

    for (k = 0; k < 4; k++) {
        args[0] = AF_PACKET;
        args[1] = SOCK_DGRAM;
        args[2] = ETH_P_IP_BE;
        fd = socketcall(SC_SOCKET, args);
        if (fd < 0) {
            fail_n("reopen", "a packet socket did not open after an earlier one closed", fd);
        }
        sll = zero_sll;
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = ETH_P_IP_BE;
        sll.sll_ifindex = if_index;
        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)&sll;
        args[2] = sizeof(sll);
        r = socketcall(SC_BIND, args);
        if (r != 0) {
            fail_n("reopen", "a bind after an earlier socket closed was refused", r);
        }
        if (socket_inode(fd, inode, (int)sizeof(inode)) == 0) {
            fail("table-read", "/proc/self/fd gave no inode for a packet socket");
        }
        r = packet_rows(inode);
        if (r < 0) {
            fail("table-read", "/proc/net/packet could not be read");
        }
        if (r != 1) {
            fail_n("bind-listed", "a bound packet socket did not have one row in /proc/net/packet",
                   r);
        }
        sys_close(fd);
        r = packet_rows(inode);
        if (r < 0) {
            fail("table-read", "/proc/net/packet could not be read");
        }
        if (r != 0) {
            fail_n("close-frees",
                   "a closed packet socket was still in /proc/net/packet, so the "
                   "close kept its binding",
                   r);
        }
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The rows of a native run between the bind and the two protocols: the
 * refusal of a destination that is not a MAC address, which sends nothing,
 * and the refused bind that leaves the protocol as it was, on the socket
 * `fd`. The discovers and what is received of them are left out.
 */
static void native_rows(int fd)
{
    unsigned long args[6];
    struct sockaddr_ll_probe sll, back;
    unsigned int i;
    /* volatile: the kernel writes it through a pointer. */
    volatile unsigned int backlen;

    sll = zero_sll;
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = ETH_P_IP_BE;
    sll.sll_ifindex = if_index;
    sll.sll_hatype = 1;
    sll.sll_halen = 4;
    for (i = 0; i < 6; i++) {
        sll.sll_addr[i] = 0xff;
    }
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)frame;
    args[2] = DHCP_LEN;
    args[3] = 0;
    args[4] = (unsigned long)&sll;
    args[5] = sizeof(sll);
    if (socketcall(SC_SENDTO, args) != -EINVAL) {
        fail("not-mac-refused", "a destination that is not a MAC address was not refused");
    }

    sll.sll_protocol = ETH_P_ARP_BE;
    sll.sll_ifindex = 99; /* no such interface */
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)&sll;
    args[2] = sizeof(sll);
    if (socketcall(SC_BIND, args) != -ENODEV) {
        fail("refused-bind-keeps", "a bind to an interface that does not exist was not refused");
    }
    back = zero_sll;
    backlen = sizeof(back);
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)&back;
    args[2] = (unsigned long)&backlen;
    if (socketcall(SC_GETSOCKNAME, args) != 0) {
        fail("refused-bind-keeps", "getsockname after the refused bind failed");
    }
    if (back.sll_protocol != ETH_P_IP_BE) {
        fail("refused-bind-keeps", "a refused bind changed the protocol the socket receives");
    }
}

/**
 * An IPv6 packet socket bound to the interface sees a frame of EtherType
 * 0x86DD. It sends, from the interface's link-local address, a neighbour
 * solicitation for fe80::2, the address of QEMU's user network, to that
 * address's solicited-node group, ff02::1:ff00:2, at 33:33:ff:00:00:02,
 * with a hop limit of 255 and its station address as an option (RFC 4861,
 * 4.3), and waits for the advertisement that answers it. The kernel has no
 * neighbour discovery to send one of its own, so the frame read can only be
 * the answer to this one.
 */
static void ipv6_frame(void)
{
    static const uint8_t target[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
    unsigned long args[6];
    struct sockaddr_ll_probe sll;
    struct pollfd_probe pfd;
    unsigned char ifr[40];
    uint8_t mac[6];
    unsigned long sum;
    unsigned int i;
    int fd, n;
    /* volatile: the kernel writes it through a pointer. */
    volatile unsigned int fromlen;

    args[0] = AF_PACKET;
    args[1] = SOCK_DGRAM;
    args[2] = ETH_P_IPV6_BE;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0) {
        fail_n("ipv6-frame", "the IPv6 socket did not open", fd);
    }
    ifreq_for(ifr);
    if (sys_ioctl(fd, SIOCGIFHWADDR, ifr) != 0) {
        fail("ipv6-frame", "the interface would not report its hardware address");
    }
    for (i = 0; i < 6; i++) {
        mac[i] = ifr[18 + i];
    }
    sll = zero_sll;
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = ETH_P_IPV6_BE;
    sll.sll_ifindex = if_index;
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)&sll;
    args[2] = sizeof(sll);
    if (socketcall(SC_BIND, args) != 0) {
        fail("ipv6-frame", "a bind naming IPv6 was refused");
    }

    /* The IPv6 header: version 6, a payload of 32 bytes, ICMPv6, hop
     * limit 255, from fe80:: and the modified EUI-64 of the station
     * address, to the solicited-node group. */
    for (i = 0; i < 72; i++) {
        frame[i] = 0;
    }
    frame[0] = 0x60;
    frame[5] = 32;
    frame[6] = 58;
    frame[7] = 255;
    frame[8] = 0xfe;
    frame[9] = 0x80;
    frame[16] = (uint8_t)(mac[0] ^ 0x02);
    frame[17] = mac[1];
    frame[18] = mac[2];
    frame[19] = 0xff;
    frame[20] = 0xfe;
    frame[21] = mac[3];
    frame[22] = mac[4];
    frame[23] = mac[5];
    frame[24] = 0xff;
    frame[25] = 2;
    frame[35] = 1;
    frame[36] = 0xff;
    frame[39] = 2;
    /* The solicitation: type 135, the target, and the option of type 1 and
     * a length of one 8-byte unit holding the station address. */
    frame[40] = 135;
    for (i = 0; i < 16; i++) {
        frame[48 + i] = target[i];
    }
    frame[64] = 1;
    frame[65] = 1;
    for (i = 0; i < 6; i++) {
        frame[66 + i] = mac[i];
    }
    /* The checksum over the pseudo-header, the two addresses, the length
     * and the next header, and the message. */
    sum = 32 + 58;
    for (i = 8; i < 40; i += 2) {
        sum += ((unsigned long)frame[i] << 8) | frame[i + 1];
    }
    for (i = 40; i < 72; i += 2) {
        sum += ((unsigned long)frame[i] << 8) | frame[i + 1];
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    sum = ~sum & 0xffff;
    frame[42] = (uint8_t)(sum >> 8);
    frame[43] = (uint8_t)sum;

    sll.sll_hatype = 1;
    sll.sll_halen = 6;
    sll.sll_addr[0] = 0x33;
    sll.sll_addr[1] = 0x33;
    sll.sll_addr[2] = 0xff;
    sll.sll_addr[3] = 0;
    sll.sll_addr[4] = 0;
    sll.sll_addr[5] = 2;
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)frame;
    args[2] = 72;
    args[3] = 0;
    args[4] = (unsigned long)&sll;
    args[5] = sizeof(sll);
    n = socketcall(SC_SENDTO, args);
    if (n != 72) {
        fail_n("ipv6-frame", "the neighbour solicitation did not send", n);
    }

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    n = sys_poll_wait(&pfd, 1);
    if (n <= 0 || !(pfd.revents & POLLIN)) {
        fail_n("ipv6-frame", "a poll for an IPv6 frame ended with nothing to read", n);
    }
    sll = zero_sll;
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)frame;
    args[2] = sizeof(frame);
    args[3] = 0x40; /* MSG_DONTWAIT */
    args[4] = (unsigned long)&sll;
    fromlen = sizeof(sll);
    args[5] = (unsigned long)&fromlen;
    if (socketcall(SC_RECVFROM, args) < 40) {
        fail("ipv6-frame", "a frame on the IPv6 socket was shorter than an IPv6 header");
    }
    if ((frame[0] >> 4) != 6 || sll.sll_protocol != ETH_P_IPV6_BE) {
        fail("ipv6-frame", "the IPv6 socket was given a frame that is not IPv6");
    }
    sys_close(fd);
}

/**
 * The probe, in the order below; it fails at the first wrong answer.
 *
 * What it refuses: a stream packet socket, and PACKET_AUXDATA, the option a
 * client sets to be told about checksum offload, since the socket attaches
 * no control data. What it accepts at creation: a socket for IPv6, and no
 * protocol at all, which is what `arping` opens and then names in its bind.
 * A bind to an interface that does not exist is -ENODEV.
 *
 * What it sends: a DHCP discover, an IP packet the caller built whole,
 * broadcast from a machine that has no address yet. That is the case this
 * socket exists for, and it is also the only way to make something answer,
 * which shows that the frame left well formed. A destination of four bytes
 * is not a MAC address and is -EINVAL.
 *
 * What it receives: the offer, broadcast to 255.255.255.255, an address
 * this machine does not have, which `ip_rx` drops. It is waited for as a
 * DHCP client waits, by poll and then a read, so a queue that never reports
 * itself readable is a client that times out with an offer in hand. select
 * must agree with poll about the same descriptor, given the whole 128-byte
 * fd_set the type is, not one word of it: the kernel reads and writes all
 * of it whatever `nfds` says, and a smaller object here would have 124
 * bytes of the stack overwritten; it is static, so it starts zeroed and
 * only the one word is set.
 *
 * Once the offer is read the socket is empty, and a receive that fails must
 * leave the caller's address arguments as they were. A second offer is
 * taken through recvfrom with sixteen bytes for the twenty-byte sockaddr_ll:
 * the length reported is the true one and only what fits is written, the
 * truncation rule a caller comparing lengths depends on.
 *
 * A bind that fails leaves the socket receiving what it received before: the
 * protocol is the filter, and a refusal that changed it would leave a client
 * reading frames it never asked for. A bind that succeeds with another
 * protocol drops what arrived under the first, which would otherwise be
 * handed to a reader labelled as the new one; a third discover puts an
 * offer in the queue for it.
 *
 * The two protocols, and the filter between them: one socket is bound to
 * ARP and one to IPv4. getsockname on the ARP socket reports the machine's
 * own link address, which `arping` reads back and refuses to run without,
 * and it must be the address SIOCGIFHWADDR reports. The ARP socket sends an
 * ARP request of its own, which is answered only if it leaves as ARP, and a
 * ping to the gateway 10.0.2.2, the gateway of the network QEMU puts a guest
 * on, makes the machine resolve an address it does not know, so both kinds
 * of frame cross the wire within the same second. The request asks about
 * the name server, 10.0.2.3, not the gateway: the machine resolves the
 * gateway by itself for the echo, and a reply about it would arrive whether
 * or not the request ever left, so a reply about 10.0.2.3 can only be this
 * request's. Each socket must see its own frames and not the other's, and
 * the kernel's ARP must still have run: a delivery that took ARP away from
 * `arp_rx` would pass a socket-only test and leave the machine unable to
 * resolve anything, so the gateway must be in /proc/net/arp.
 *
 * On QEMU's network an IPv6 socket then sees a frame of EtherType 0x86DD,
 * as `ipv6_frame` says.
 *
 * Every frame the probe waits for, an offer, an ARP reply, an echo reply or
 * a neighbour advertisement,
 * it waits for in poll with no timeout, and judges what came; the suite's
 * limit ends a run in which one never comes.
 *
 * Four sockets are then opened, bound and closed in a row, and each close
 * must free its binding, as `check_close_releases` says. Last, a child
 * that drops to an ordinary user is refused a packet socket, since the
 * socket reads every frame the machine receives. A child that a signal
 * killed has an exit byte of zero, so its status is checked for a signal
 * first, and a crash is not read as the refusal.
 */
void probe_main(unsigned long *sp)
{
    unsigned long args[6];
    struct sockaddr_ll_probe sll;
    struct sockaddr_ll_probe from_ll;
    struct iovec_probe iov;
    struct msghdr_probe msg;
    int fd, r, seen_offer, polled, pid, status;
    struct pollfd_probe pfd;
    struct timeval_probe tv;
    /* volatile: the kernel writes it through a pointer. */
    volatile unsigned int senderlen;
    uint16_t csum;
    unsigned int i;

    probe_args((int)sp[0], (char **)(sp + 1));
    report_begin(report_case((char **)(sp + 2 + sp[0]), "guest.packet.packetprobe"));
    plan(!probe_native());

    /* What it refuses: a stream socket, and PACKET_AUXDATA; it takes IPv6. */
    args[0] = AF_PACKET;
    args[1] = SOCK_STREAM;
    args[2] = ETH_P_IP_BE;
    if (socketcall(SC_SOCKET, args) != -ESOCKTNOSUPPORT) {
        fail("stream-refused", "a stream packet socket was not refused");
    }
    args[1] = SOCK_DGRAM;
    args[2] = ETH_P_IPV6_BE;
    r = socketcall(SC_SOCKET, args);
    if (r < 0) {
        fail_n("ipv6-open", "a packet socket for IPv6 did not open", r);
    }
    sys_close(r);
    /* What it accepts at creation: no protocol, as `arping` opens it. */
    args[2] = 0;
    r = socketcall(SC_SOCKET, args);
    if (r < 0) {
        fail_n("open", "a packet socket with no protocol did not open", r);
    }
    sys_close(r);

    args[2] = ETH_P_IP_BE;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0) {
        fail("open", "a packet socket did not open");
    }
    interface(fd);

    r = 1;
    args[0] = (uint32_t)fd;
    args[1] = SOL_PACKET;
    args[2] = PACKET_AUXDATA;
    args[3] = (unsigned long)&r;
    args[4] = sizeof(int);
    if (socketcall(SC_SETSOCKOPT, args) != -ENOPROTOOPT) {
        fail("auxdata-refused", "PACKET_AUXDATA was not refused");
    }

    for (i = 0; i < sizeof(sll); i++) {
        ((uint8_t *)&sll)[i] = 0;
    }
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = ETH_P_IP_BE;
    sll.sll_ifindex = 99;
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)&sll;
    args[2] = sizeof(sll);
    if (socketcall(SC_BIND, args) != -ENODEV) {
        fail("bind-missing", "a bind to an interface that does not exist was not refused");
    }
    sll.sll_ifindex = if_index;
    if (socketcall(SC_BIND, args) != 0) {
        fail("bind", "a bind to the machine's interface failed");
    }
    if (probe_native()) {
        native_rows(fd);
        goto two_protocols;
    }

    /* What it sends: a DHCP discover, built whole and broadcast. */
    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = 0;
    }
    frame[0] = 0x45;                     /* IPv4, 20-byte header */
    frame[2] = (uint8_t)(DHCP_LEN >> 8); /* total length */
    frame[3] = (uint8_t)(DHCP_LEN & 0xff);
    frame[8] = 64; /* TTL */
    frame[9] = IPPROTO_UDP;
    for (i = 16; i < 20; i++) {
        frame[i] = 0xff; /* 0.0.0.0 -> 255.255.255.255 */
    }
    csum = ip_checksum(frame, 20);
    frame[10] = (uint8_t)(csum >> 8);
    frame[11] = (uint8_t)(csum & 0xff);

    frame[21] = 68; /* source port 68 */
    frame[23] = 67; /* destination port 67 */
    frame[24] = (uint8_t)((DHCP_LEN - 20) >> 8);
    frame[25] = (uint8_t)((DHCP_LEN - 20) & 0xff);
    /* A UDP checksum of zero is "not computed", which IPv4 and SLIRP accept. */

    frame[28] = 1; /* BOOTREQUEST */
    frame[29] = 1; /* Ethernet */
    frame[30] = 6; /* six-byte hardware address */
    frame[32] = DHCP_XID_0;
    frame[33] = DHCP_XID_1;
    frame[34] = DHCP_XID_2;
    frame[35] = DHCP_XID_3;
    frame[38] = 0x80; /* ask for a broadcast reply */
    frame[56] = 0x52; /* a client hardware address */
    frame[57] = 0x41;
    frame[58] = 0x43;
    frame[59] = 0x48;
    frame[60] = 0x32;
    frame[61] = 0x01;
    frame[264] = 0x63; /* the magic cookie */
    frame[265] = 0x82;
    frame[266] = 0x53;
    frame[267] = 0x63;
    frame[268] = 53; /* option 53: message type */
    frame[269] = 1;
    frame[270] = 1;   /* DHCPDISCOVER */
    frame[271] = 255; /* end */

    sll.sll_hatype = 1; /* ARPHRD_ETHER */
    sll.sll_pkttype = 0;
    sll.sll_halen = 6;
    for (i = 0; i < 6; i++) {
        sll.sll_addr[i] = 0xff;
    }
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)frame;
    args[2] = DHCP_LEN;
    args[3] = 0;
    args[4] = (unsigned long)&sll;
    args[5] = sizeof(sll);
    for (i = 0; i < DHCP_LEN; i++) {
        discover[i] = frame[i];
    }
    if (socketcall(SC_SENDTO, args) != DHCP_LEN) {
        fail("discover-sent", "the discover did not send");
    }
    sll.sll_halen = 4;
    if (socketcall(SC_SENDTO, args) != -EINVAL) {
        fail("not-mac-refused", "a destination that is not a MAC address was not refused");
    }

    /* What it receives: the offer to 255.255.255.255, which `ip_rx` drops. */
    reach("offer-polled");
    seen_offer = 0;
    polled = 0;
    while (!seen_offer) {
        iov.iov_base = frame;
        iov.iov_len = sizeof(frame);
        for (i = 0; i < sizeof(msg); i++) {
            ((uint8_t *)&msg)[i] = 0;
        }
        msg.msg_name = &from_ll;
        msg.msg_namelen = sizeof(from_ll);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        /* Polled and then read, as the real client waits. */
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        r = sys_poll_wait(&pfd, 1);
        if (r != 1 || !(pfd.revents & POLLIN)) {
            fail_n("offer-polled", "a poll for the offer ended with nothing to read", r);
        }
        polled = 1;

        /* select agrees with poll, given the whole 128-byte fd_set. */
        readfds[0] = 1u << fd;
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (sys_select(fd + 1, readfds, 0, 0, &tv) != 1) {
            fail("select-agrees", "select did not see what poll saw");
        }

        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)&msg;
        args[2] = 0x40; /* MSG_DONTWAIT */
        r = socketcall(SC_RECVMSG, args);
        if (r < 0) {
            fail_n("offer-polled", "a socket poll called readable had nothing to read", r);
        }
        if (r < 20) {
            fail("offer-ipv4", "a received packet was shorter than an IP header");
        }
        if ((frame[0] >> 4) != 4) {
            fail("offer-ipv4", "a received packet was not IPv4");
        }
        if (msg.msg_controllen != 0) {
            fail("no-control", "control data was reported where this stack attaches none");
        }
        {
            const uint8_t *ll = (const uint8_t *)&from_ll;

            if (msg.msg_namelen != 20) {
                fail("offer-address",
                     "recvmsg did not report the link address the frame came from");
            }
            if (ll[0] != AF_PACKET || ll[1] != 0) {
                fail("offer-address", "the reported address was not a sockaddr_ll");
            }
            if (ll[4] != 1 || ll[8] != 1 || ll[11] != 6) {
                fail("offer-address", "the reported address did not name this Ethernet interface");
            }
            /* The offer is broadcast, and a reader decides on this field. */
            if (ll[10] != 1) {
                fail("offer-address", "a broadcast offer was not reported as a broadcast");
            }
            if (ll[12] == 0 && ll[13] == 0 && ll[14] == 0 && ll[15] == 0 && ll[16] == 0 &&
                ll[17] == 0) {
                fail("offer-address", "the reported address had no sender in it");
            }
        }
        if (msg.msg_flags != 0) {
            fail("no-flags", "a message flag was reported where none is set");
        }
        if (frame[9] != IPPROTO_UDP || r < 272) {
            continue;
        }
        if (frame[20] != 0 || frame[21] != 67 || frame[22] != 0 || frame[23] != 68) {
            continue;
        }
        if (frame[32] != DHCP_XID_0 || frame[33] != DHCP_XID_1 || frame[34] != DHCP_XID_2 ||
            frame[35] != DHCP_XID_3) {
            continue;
        }
        if (frame[28] != 2) {
            fail("bootp-broadcast", "the reply on port 68 was not a BOOTP reply");
        }
        /* Addressed to the broadcast, a packet `ip_rx` would have dropped. */
        if (frame[16] != 0xff || frame[17] != 0xff || frame[18] != 0xff || frame[19] != 0xff) {
            fail("bootp-broadcast", "the offer was not the broadcast ip_rx drops");
        }
        seen_offer = 1;
    }
    if (!polled) {
        fail("offer-polled", "the offer was read without poll ever reporting it readable");
    }

    /* On the emptied socket, a failed receive leaves the address arguments. */
    for (i = 0; i < sizeof(sender); i++) {
        sender[i] = 0xa5;
    }
    senderlen = 16;
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)frame;
    args[2] = 100;
    args[3] = 0x40; /* MSG_DONTWAIT */
    args[4] = (unsigned long)sender;
    args[5] = (unsigned long)&senderlen;
    if (socketcall(SC_RECVFROM, args) != -EAGAIN) {
        fail("empty-eagain", "an empty socket did not report that it was empty");
    }
    if (senderlen != 16 || sender[0] != 0xa5) {
        fail("no-address-written", "a receive that failed wrote to the caller's address arguments");
    }

    /* A second offer, by recvfrom with too little room for the address. */
    reach("second-offer");
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)discover;
    args[2] = DHCP_LEN;
    args[3] = 0;
    args[4] = (unsigned long)&sll;
    args[5] = sizeof(sll);
    sll.sll_halen = 6;
    if (socketcall(SC_SENDTO, args) != DHCP_LEN) {
        fail("second-offer", "the second discover did not send");
    }
    seen_offer = 0;
    while (!seen_offer) {
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        r = sys_poll_wait(&pfd, 1);
        if (r != 1 || !(pfd.revents & POLLIN)) {
            fail_n("second-offer", "a poll for the second offer ended with nothing to read", r);
        }
        senderlen = 16;
        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)frame;
        args[2] = sizeof(frame);
        args[3] = 0x40;
        args[4] = (unsigned long)sender;
        args[5] = (unsigned long)&senderlen;
        r = socketcall(SC_RECVFROM, args);
        if (r < 0) {
            fail_n("second-offer", "a socket poll called readable had nothing for recvfrom", r);
        }
        /* The true length, 20, and only the 16 bytes that fit written. */
        if (senderlen != 20) {
            fail("true-length", "a packet socket did not report the true address length");
        }
        if (sender[0] != AF_PACKET || sender[4] != 1 || sender[11] != 6) {
            fail("truncated-address", "the address written was not this interface's sockaddr_ll");
        }
        if (sender[16] != 0xa5 || sender[17] != 0xa5) {
            fail("capacity", "a packet socket wrote past the capacity the caller declared");
        }
        seen_offer = 1;
    }

    /* A bind that fails leaves the socket's protocol as it was. */
    {
        struct sockaddr_ll_probe back;

        sll.sll_family = AF_PACKET;
        sll.sll_protocol = ETH_P_ARP_BE;
        sll.sll_ifindex = 99; /* no such interface */
        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)&sll;
        args[2] = sizeof(sll);
        if (socketcall(SC_BIND, args) != -ENODEV) {
            fail("refused-bind-keeps",
                 "a bind to an interface that does not exist was not refused");
        }
        for (i = 0; i < sizeof(back); i++) {
            ((uint8_t *)&back)[i] = 0;
        }
        senderlen = sizeof(back);
        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)&back;
        args[2] = (unsigned long)&senderlen;
        if (socketcall(SC_GETSOCKNAME, args) != 0) {
            fail("refused-bind-keeps", "getsockname after the refused bind failed");
        }
        if (back.sll_protocol != ETH_P_IP_BE) {
            fail("refused-bind-keeps", "a refused bind changed the protocol the socket receives");
        }
    }

    /* A rebind to another protocol drops what arrived under the first. */
    reach("third-offer");
    {
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = ETH_P_IP_BE;
        sll.sll_ifindex = 1;
        sll.sll_halen = 6;
        for (i = 0; i < 6; i++) {
            sll.sll_addr[i] = 0xff;
        }
        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)discover;
        args[2] = DHCP_LEN;
        args[3] = 0;
        args[4] = (unsigned long)&sll;
        args[5] = sizeof(sll);
        if (socketcall(SC_SENDTO, args) != DHCP_LEN) {
            fail("third-offer", "the third discover did not send");
        }
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        r = sys_poll_wait(&pfd, 1);
        if (r != 1 || !(pfd.revents & POLLIN)) {
            fail_n("third-offer", "a poll for the third offer ended with nothing to read", r);
        }
        sll.sll_protocol = ETH_P_ARP_BE;
        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)&sll;
        args[2] = sizeof(sll);
        if (socketcall(SC_BIND, args) != 0) {
            fail("rebind-drops", "a rebind to ARP was refused");
        }
        senderlen = sizeof(sender);
        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)frame;
        args[2] = sizeof(frame);
        args[3] = 0x40; /* MSG_DONTWAIT */
        args[4] = (unsigned long)sender;
        args[5] = (unsigned long)&senderlen;
        if (socketcall(SC_RECVFROM, args) != -EAGAIN) {
            fail("rebind-drops", "a rebound socket still had what arrived under its old protocol");
        }
    }

two_protocols:
    sys_close(fd);

    /* An ARP and an IPv4 socket each see their own frames only. */
    {
        int arp_fd, ip_fd, arp_replies = 0, ip_frames = 0, n;
        struct pollfd_probe pair[2];
        unsigned char ifr[40];
        unsigned char mac[6];
        char gateway_text[17];

        args[0] = AF_PACKET;
        args[1] = SOCK_DGRAM;
        args[2] = 0;
        arp_fd = socketcall(SC_SOCKET, args);
        if (arp_fd < 0) {
            fail_n("arp-bind", "the ARP socket did not open", arp_fd);
        }
        for (i = 0; i < sizeof(sll); i++) {
            ((uint8_t *)&sll)[i] = 0;
        }
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = ETH_P_ARP_BE;
        sll.sll_ifindex = if_index;
        args[0] = (uint32_t)arp_fd;
        args[1] = (unsigned long)&sll;
        args[2] = sizeof(sll);
        if (socketcall(SC_BIND, args) != 0) {
            fail("arp-bind", "a bind naming ARP was refused");
        }

        /* getsockname gives the address SIOCGIFHWADDR reports. */
        ifreq_for(ifr);
        if (sys_ioctl(arp_fd, SIOCGIFHWADDR, ifr) != 0) {
            fail("hwaddr", "the interface would not report its hardware address");
        }
        for (i = 0; i < 6; i++) {
            mac[i] = ifr[18 + i]; /* ifr_hwaddr.sa_data, past name and family */
        }
        /* No field below may keep 0xa5: each must be the kernel's answer. */
        for (i = 0; i < sizeof(sll); i++) {
            ((uint8_t *)&sll)[i] = 0xa5;
        }
        senderlen = sizeof(sll);
        args[0] = (uint32_t)arp_fd;
        args[1] = (unsigned long)&sll;
        args[2] = (unsigned long)&senderlen;
        if (socketcall(SC_GETSOCKNAME, args) != 0) {
            fail("getsockname-whole", "getsockname on a packet socket failed");
        }
        if (senderlen != 20) {
            fail("getsockname-whole", "getsockname did not report a whole sockaddr_ll");
        }
        if (sll.sll_family != AF_PACKET || sll.sll_protocol != ETH_P_ARP_BE) {
            fail("getsockname-bound", "getsockname did not report the family and protocol bound");
        }
        if (sll.sll_ifindex != if_index || sll.sll_hatype != 1 || sll.sll_halen != 6) {
            fail("getsockname-bound", "getsockname did not report the interface it is bound to");
        }
        for (i = 0; i < 6; i++) {
            if (sll.sll_addr[i] != mac[i]) {
                fail("getsockname-mac",
                     "getsockname reported an address the interface does not have");
            }
        }

        args[0] = AF_PACKET;
        args[1] = SOCK_DGRAM;
        args[2] = ETH_P_IP_BE;
        ip_fd = socketcall(SC_SOCKET, args);
        if (ip_fd < 0) {
            fail_n("ip-bind", "the IPv4 socket did not open", ip_fd);
        }
        for (i = 0; i < sizeof(sll); i++) {
            ((uint8_t *)&sll)[i] = 0;
        }
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = ETH_P_IP_BE;
        sll.sll_ifindex = if_index;
        args[0] = (uint32_t)ip_fd;
        args[1] = (unsigned long)&sll;
        args[2] = sizeof(sll);
        if (socketcall(SC_BIND, args) != 0) {
            fail("ip-bind", "a bind naming IPv4 was refused");
        }

        /* An ARP request of the probe's own, sent under ARP. */
        for (i = 0; i < 28; i++) {
            frame[i] = 0;
        }
        frame[1] = 1;    /* hardware type: Ethernet */
        frame[2] = 0x08; /* protocol type: IPv4 */
        frame[4] = 6;    /* hardware address length */
        frame[5] = 4;    /* protocol address length */
        frame[7] = 1;    /* operation: request */
        for (i = 0; i < 6; i++) {
            frame[8 + i] = mac[i]; /* sender hardware address */
        }
        for (i = 0; i < 4; i++) {
            frame[14 + i] = (uint8_t)(self_ip >> (24 - 8 * i));  /* sender protocol address */
            frame[24 + i] = (uint8_t)(asked_ip >> (24 - 8 * i)); /* the address asked about */
        }
        sll.sll_protocol = ETH_P_ARP_BE;
        sll.sll_hatype = 1;
        sll.sll_pkttype = 0;
        sll.sll_halen = 6;
        for (i = 0; i < 6; i++) {
            sll.sll_addr[i] = 0xff;
        }
        args[0] = (uint32_t)arp_fd;
        args[1] = (unsigned long)frame;
        args[2] = 28;
        args[3] = 0;
        args[4] = (unsigned long)&sll;
        args[5] = sizeof(sll);
        if (socketcall(SC_SENDTO, args) != 28) {
            fail("arp-request-sent", "the ARP request did not send");
        }

        /* The gateway's echo reply is IPv4 traffic for the other socket. */
        args[0] = gateway_ip;
        r = socketcall(SC_PING, args);
        if (r < 0) {
            fail_n("gateway-ping",
                   "the gateway did not answer the ping whose reply the IPv4 socket waits for", r);
        }

        reach("arp-only");
        while (arp_replies == 0 || ip_frames == 0) {
            pair[0].fd = arp_fd;
            pair[0].events = POLLIN;
            pair[0].revents = 0;
            pair[1].fd = ip_fd;
            pair[1].events = POLLIN;
            pair[1].revents = 0;
            n = sys_poll_wait(pair, 2);
            if (n <= 0 || !((pair[0].revents | pair[1].revents) & POLLIN)) {
                fail_n("both-answered",
                       "a poll for the ARP and IPv4 frames ended with nothing to read", n);
            }
            if (pair[0].revents & POLLIN) {
                senderlen = 0;
                args[0] = (uint32_t)arp_fd;
                args[1] = (unsigned long)frame;
                args[2] = sizeof(frame);
                args[3] = 0x40; /* MSG_DONTWAIT */
                args[4] = 0;
                args[5] = 0;
                n = socketcall(SC_RECVFROM, args);
                if (n < 28) {
                    fail("arp-only", "a frame on the ARP socket was shorter than an ARP packet");
                }
                /* Hardware 1, protocol 0x0800, address lengths 6 and 4. */
                if (frame[0] != 0 || frame[1] != 1 || frame[2] != 0x08 || frame[3] != 0 ||
                    frame[4] != 6 || frame[5] != 4) {
                    fail("arp-only", "the ARP socket was given a frame that is not ARP");
                }
                /* A reply from the address asked about answers it. */
                if (frame[6] == 0 && frame[7] == 2 && frame[14] == (uint8_t)(asked_ip >> 24) &&
                    frame[15] == (uint8_t)(asked_ip >> 16) &&
                    frame[16] == (uint8_t)(asked_ip >> 8) && frame[17] == (uint8_t)asked_ip) {
                    arp_replies++;
                }
            }
            if (pair[1].revents & POLLIN) {
                args[0] = (uint32_t)ip_fd;
                args[1] = (unsigned long)frame;
                args[2] = sizeof(frame);
                args[3] = 0x40;
                args[4] = 0;
                args[5] = 0;
                n = socketcall(SC_RECVFROM, args);
                if (n < 20) {
                    fail("ipv4-only", "a frame on the IPv4 socket was shorter than an IP header");
                }
                if ((frame[0] >> 4) != 4) {
                    fail("ipv4-only", "the IPv4 socket was given a frame that is not IPv4");
                }
                ip_frames++;
            }
        }

        /* The kernel's ARP cache has the gateway it resolved. */
        n = read_file("/proc/net/arp", (char *)frame, (int)sizeof(frame));
        if (n <= 0) {
            fail("arp-cache", "/proc/net/arp could not be read");
        }
        dotted(gateway_text, gateway_ip);
        if (!contains((const char *)frame, n, gateway_text)) {
            fail("arp-cache", "the address the machine resolved is not in its ARP cache, so "
                              "the packet socket took the frame away from the kernel");
        }

        sys_close(arp_fd);
        sys_close(ip_fd);
    }

    if (!probe_native()) {
        ipv6_frame();
    }

    check_close_releases();

    /* An ordinary user is refused a packet socket. */
    pid = sys_fork();
    if (pid == 0) {
        if (sys_setuid((int)probe_id(1000)) != 0) {
            sys_exit(2);
        }
        args[0] = AF_PACKET;
        args[1] = SOCK_DGRAM;
        args[2] = ETH_P_IP_BE;
        sys_exit(socketcall(SC_SOCKET, args) == -EPERM ? 0 : 3);
    }
    if (pid < 0) {
        fail("unprivileged-refused", "the fork for the unprivileged case failed");
    }
    status = -1;
    if (sys_waitpid(pid, &status, 0) != pid) {
        fail("unprivileged-refused", "waiting for the unprivileged case failed");
    }
    if ((status & 0x7f) != 0) {
        fail("unprivileged-refused", "the unprivileged case died on a signal");
    }
    if (((status >> 8) & 0xff) != 0) {
        fail("unprivileged-refused", "an ordinary user was not refused a packet socket");
    }

    reach(0);
    report_end();
    say("PACKETPROBE OK\n");
    sys_exit(0);
}
