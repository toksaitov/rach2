/**
 * Exercises the packet socket: the tap a DHCP client sees replies through.
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
 * Prints "PACKETPROBE OK" and exits 0 on success. Freestanding; direct int
 * 0x80.
 */

#include "probe.h"

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define STDOUT_FILENO 1

#define SC_GETSOCKNAME 6

#define SIOCGIFHWADDR 0x8927

#define SC_SOCKET     1
#define SC_BIND       2
#define SC_SENDTO     11
#define SC_SETSOCKOPT 14
#define SC_RECVFROM   12
#define SC_RECVMSG    17
#define SC_PING       0x1000

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

/* The discover this probe sends and the offer it waits for. The transaction id
   is what ties the two together; SLIRP echoes it back. */
#define DHCP_XID_0 0x52
#define DHCP_XID_1 0x41
#define DHCP_XID_2 0x43
#define DHCP_XID_3 0x32
#define DHCP_LEN   272

static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

static int socketcall(int call, uint32_t *args)
{
    return syscall2(SYS_SOCKETCALL, (long)call, (long)args);
}

#define POLLIN 0x001

struct timeval_probe {
    long tv_sec;
    long tv_usec;
};

struct timespec_probe {
    long tv_sec;
    long tv_nsec;
};

static void nap(long ms)
{
    struct timespec_probe ts;
    int r;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    r = syscall2(SYS_NANOSLEEP, (long)(&ts), (long)0);
}

static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

static int sys_setuid(int uid)
{
    return syscall1(SYS_SETUID, (long)uid);
}

static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
}

/* The one's-complement sum an IP header has; SLIRP checks it, so a wrong
   one is an offer that never comes, not a visible failure. */
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

static int sys_poll(struct pollfd_probe *fds, unsigned int n, int timeout)
{
    return syscall3(SYS_POLL, (long)fds, (long)n, (long)timeout);
}

static int sys_select(int n, void *rd, void *wr, void *ex, void *tv)
{
    return syscall5(SYS_SELECT, (long)n, (long)rd, (long)wr, (long)ex, (long)tv);
}

static void say(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    sys_write(STDOUT_FILENO, s, n);
}

/* A failure that names a number tells whoever reads the log which refusal it
   was, which matters here because every one of them is a negative errno. */
/** ioctl(2). */
static int sys_ioctl(int fd, unsigned int req, void *arg)
{
    return syscall3(SYS_IOCTL, (long)fd, (long)req, (long)arg);
}

/** open(2), read-only. */
static int sys_open_ro(const char *path)
{
    return syscall3(SYS_OPEN, (long)path, (long)0, (long)0);
}

/** read(2). */
static int sys_read(int fd, void *buf, int n)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)n);
}

/** Read a whole small file into `out`; the bytes read, or a negative errno. */
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

/** 1 if `needle` occurs in `hay[0..len)`, else 0. */
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

static void fail_n(const char *what, int n)
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
    sys_exit(1);
}

static void fail(const char *what)
{
    say("PACKETPROBE FAIL: ");
    say(what);
    say("\n");
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

/* An fd_set is 1024 bits by definition, and select works on the whole object
   however few descriptors the caller names. */
#define FDSET_WORDS_PROBE 32
static unsigned int readfds[FDSET_WORDS_PROBE];

static uint8_t frame[1600];
static uint8_t discover[DHCP_LEN];
static uint8_t sender[24];
static uint8_t control[64];

PROBE_START void _start(void)
{
    uint32_t args[6];
    struct sockaddr_ll_probe sll;
    struct sockaddr_ll_probe from_ll;
    struct iovec_probe iov;
    struct msghdr_probe msg;
    int fd, r, tries, seen_offer, polled, pid, status;
    struct pollfd_probe pfd;
    struct timeval_probe tv;
    /* Written by the kernel through a pointer, which is why it is volatile: a
       reader of this file alone sees only the assignment before the call. */
    volatile unsigned int senderlen;
    uint16_t csum;
    unsigned int i;

    /* What it refuses. A stream packet socket is not a thing, a protocol this
       stack has no frames for is not offered, and the option a client asks for
       to be told about checksum offload must be refused, not promised. */
    args[0] = AF_PACKET;
    args[1] = SOCK_STREAM;
    args[2] = ETH_P_IP_BE;
    if (socketcall(SC_SOCKET, args) != -ESOCKTNOSUPPORT) {
        fail("a stream packet socket was not refused");
    }
    args[1] = SOCK_DGRAM;
    args[2] = ETH_P_IPV6_BE;
    if (socketcall(SC_SOCKET, args) != -EINVAL) {
        fail("a packet socket for a protocol this stack has no frames for "
             "was not refused");
    }
    /* And what it accepts at creation: no protocol at all, which is what
       `arping` opens and then names in its bind. */
    args[2] = 0;
    r = socketcall(SC_SOCKET, args);
    if (r < 0) {
        fail_n("a packet socket with no protocol did not open", r);
    }
    sys_close(r);

    args[2] = ETH_P_IP_BE;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0) {
        fail("a packet socket did not open");
    }

    r = 1;
    args[0] = (uint32_t)fd;
    args[1] = SOL_PACKET;
    args[2] = PACKET_AUXDATA;
    args[3] = (unsigned long)&r;
    args[4] = sizeof(int);
    if (socketcall(SC_SETSOCKOPT, args) != -ENOPROTOOPT) {
        fail("PACKET_AUXDATA was not refused");
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
        fail("a bind to an interface that does not exist was not refused");
    }
    sll.sll_ifindex = 1;
    if (socketcall(SC_BIND, args) != 0) {
        fail("a bind to the machine's interface failed");
    }

    /* And what it sends: a DHCP discover, which is an IP packet the caller
       built entirely, broadcast from a machine that has no address yet. That
       is the case this socket exists for, and it is also the only way to make
       something answer, which is what proves the frame left well formed. */
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
    /* The UDP checksum is left zero, which means "not computed" and is legal
       over IPv4; SLIRP accepts it. */

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
        fail("the discover did not send");
    }
    sll.sll_halen = 4;
    if (socketcall(SC_SENDTO, args) != -EINVAL) {
        fail("a destination that is not a MAC address was not refused");
    }

    /* And what it receives: the offer, broadcast to 255.255.255.255, which is
       an address this machine does not have. `ip_rx` drops that packet, so
       reading it here is the whole reason the socket exists. */
    seen_offer = 0;
    polled = 0;
    for (tries = 0; tries < 200 && !seen_offer; tries++) {
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

        /* Waited for the way the real client waits: it polls the raw socket
           and only then reads it, so a queue that never reports itself
           readable is a client that times out with an offer in hand. */
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (sys_poll(&pfd, 1, 50) <= 0 || !(pfd.revents & POLLIN)) {
            continue;
        }
        polled = 1;

        /* And select must agree with poll about the same descriptor. The set
           is the whole 128-byte fd_set the type is, not one word of it: this
           kernel reads and writes all of it whatever `nfds` says, and a
           smaller object here is 124 bytes of this probe's stack overwritten.
           It is static, so it starts zeroed and only the one word is touched. */
        readfds[0] = 1u << fd;
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (sys_select(fd + 1, readfds, 0, 0, &tv) != 1) {
            fail("select did not see what poll saw");
        }

        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)&msg;
        args[2] = 0x40; /* MSG_DONTWAIT */
        r = socketcall(SC_RECVMSG, args);
        if (r < 0) {
            fail_n("a socket poll called readable had nothing to read", r);
        }
        if (r < 20) {
            fail("a received packet was shorter than an IP header");
        }
        if ((frame[0] >> 4) != 4) {
            fail("a received packet was not IPv4");
        }
        if (msg.msg_controllen != 0) {
            fail("control data was reported where this stack attaches none");
        }
        {
            const uint8_t *ll = (const uint8_t *)&from_ll;

            if (msg.msg_namelen != 20) {
                fail("recvmsg did not report the link address the frame came from");
            }
            if (ll[0] != AF_PACKET || ll[1] != 0) {
                fail("the reported address was not a sockaddr_ll");
            }
            if (ll[4] != 1 || ll[8] != 1 || ll[11] != 6) {
                fail("the reported address did not name this Ethernet interface");
            }
            /* The offer is broadcast, and a reader decides on this field. */
            if (ll[10] != 1) {
                fail("a broadcast offer was not reported as a broadcast");
            }
            if (ll[12] == 0 && ll[13] == 0 && ll[14] == 0 && ll[15] == 0 && ll[16] == 0 &&
                ll[17] == 0) {
                fail("the reported address had no sender in it");
            }
        }
        if (msg.msg_flags != 0) {
            fail("a message flag was reported where none is set");
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
            fail("the reply on port 68 was not a BOOTP reply");
        }
        /* And it is addressed to nobody in particular, which is the point:
           this is a packet ip_rx would have thrown away. */
        if (frame[16] != 0xff || frame[17] != 0xff || frame[18] != 0xff || frame[19] != 0xff) {
            fail("the offer was not the broadcast ip_rx drops");
        }
        seen_offer = 1;
    }
    if (!seen_offer) {
        fail("the offer never arrived on the packet socket");
    }
    if (!polled) {
        fail("the offer was read without poll ever reporting it readable");
    }

    /* A failed receive must leave the caller's address arguments exactly as
       they were. The socket is empty again now that the offer has been read,
       so this receive is the failing one. */
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
        fail("an empty socket did not report that it was empty");
    }
    if (senderlen != 16 || sender[0] != 0xa5) {
        fail("a receive that failed wrote to the caller's address arguments");
    }

    /* And once more with a datagram: send a second discover and take the offer
       through recvfrom, with a buffer too small for the address, which is
       where the truncation rule is proved. */
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)discover;
    args[2] = DHCP_LEN;
    args[3] = 0;
    args[4] = (unsigned long)&sll;
    args[5] = sizeof(sll);
    sll.sll_halen = 6;
    if (socketcall(SC_SENDTO, args) != DHCP_LEN) {
        fail("the second discover did not send");
    }
    seen_offer = 0;
    for (tries = 0; tries < 200 && !seen_offer; tries++) {
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (sys_poll(&pfd, 1, 50) <= 0 || !(pfd.revents & POLLIN)) {
            continue;
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
            fail_n("a socket poll called readable had nothing for recvfrom", r);
        }
        /* A sockaddr_ll is twenty bytes and this caller declared sixteen, so
           the length reported is the true one and only what fits is written:
           the truncation rule, which a caller comparing lengths depends on. */
        if (senderlen != 20) {
            fail("a packet socket did not report the true address length");
        }
        if (sender[0] != AF_PACKET || sender[4] != 1 || sender[11] != 6) {
            fail("the address written was not this interface's sockaddr_ll");
        }
        if (sender[16] != 0xa5 || sender[17] != 0xa5) {
            fail("a packet socket wrote past the capacity the caller declared");
        }
        seen_offer = 1;
    }
    if (!seen_offer) {
        fail("the second offer never arrived");
    }

    /* A bind that fails must leave the socket receiving what it received
       before: the protocol is the filter, and a refusal that changed it would
       leave a client reading frames it never asked for. */
    {
        struct sockaddr_ll_probe back;

        sll.sll_family = AF_PACKET;
        sll.sll_protocol = ETH_P_ARP_BE;
        sll.sll_ifindex = 99; /* no such interface */
        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)&sll;
        args[2] = sizeof(sll);
        if (socketcall(SC_BIND, args) != -ENODEV) {
            fail("a bind to an interface that does not exist was not refused");
        }
        for (i = 0; i < sizeof(back); i++) {
            ((uint8_t *)&back)[i] = 0;
        }
        senderlen = sizeof(back);
        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)&back;
        args[2] = (unsigned long)&senderlen;
        if (socketcall(SC_GETSOCKNAME, args) != 0) {
            fail("getsockname after the refused bind failed");
        }
        if (back.sll_protocol != ETH_P_IP_BE) {
            fail("a refused bind changed the protocol the socket receives");
        }
    }

    /* And a bind that succeeds with another protocol drops what arrived under
       the first, which would otherwise be handed to a reader labelled as the
       new one. A third discover is sent to put an offer in the queue. */
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
            fail("the third discover did not send");
        }
        pfd.revents = 0;
        for (tries = 0; tries < 200; tries++) {
            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (sys_poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
                break;
            }
        }
        if (!(pfd.revents & POLLIN)) {
            fail("the third offer never arrived");
        }
        sll.sll_protocol = ETH_P_ARP_BE;
        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)&sll;
        args[2] = sizeof(sll);
        if (socketcall(SC_BIND, args) != 0) {
            fail("a rebind to ARP was refused");
        }
        senderlen = sizeof(sender);
        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)frame;
        args[2] = sizeof(frame);
        args[3] = 0x40; /* MSG_DONTWAIT */
        args[4] = (unsigned long)sender;
        args[5] = (unsigned long)&senderlen;
        if (socketcall(SC_RECVFROM, args) != -EAGAIN) {
            fail("a rebound socket still had what arrived under its old protocol");
        }
    }

    sys_close(fd);

    /* The other protocol, and the filter between them. One socket is bound to
       ARP and one to IPv4; a ping to the gateway makes the machine resolve an
       address it does not know, so both kinds of frame cross the wire within
       the same second. Each socket must see its own and not the other's, and
       the kernel's ARP must still have run: a delivery that took ARP away from
       `arp_rx` would pass a socket-only test and leave the machine unable to
       resolve anything. */
    {
        int arp_fd, ip_fd, arp_frames = 0, arp_replies = 0, ip_frames = 0, n;
        unsigned char ifr[40];
        unsigned char mac[6];

        args[0] = AF_PACKET;
        args[1] = SOCK_DGRAM;
        args[2] = 0;
        arp_fd = socketcall(SC_SOCKET, args);
        if (arp_fd < 0) {
            fail_n("the ARP socket did not open", arp_fd);
        }
        for (i = 0; i < sizeof(sll); i++) {
            ((uint8_t *)&sll)[i] = 0;
        }
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = ETH_P_ARP_BE;
        sll.sll_ifindex = 1;
        args[0] = (uint32_t)arp_fd;
        args[1] = (unsigned long)&sll;
        args[2] = sizeof(sll);
        if (socketcall(SC_BIND, args) != 0) {
            fail("a bind naming ARP was refused");
        }

        /* getsockname reports the machine's own link address, which `arping`
           reads back and refuses to run without. It must be the address the
           interface really has, so it is compared with the one the interface
           ioctl reports. */
        for (i = 0; i < sizeof(ifr); i++) {
            ifr[i] = 0;
        }
        ifr[0] = 'e';
        ifr[1] = 't';
        ifr[2] = 'h';
        ifr[3] = '0';
        if (sys_ioctl(arp_fd, SIOCGIFHWADDR, ifr) != 0) {
            fail("the interface would not report its hardware address");
        }
        for (i = 0; i < 6; i++) {
            mac[i] = ifr[18 + i]; /* ifr_hwaddr.sa_data, past name and family */
        }
        for (i = 0; i < sizeof(sll); i++) {
            ((uint8_t *)&sll)[i] = 0xa5;
        }
        sll = zero_sll; /* so each field below is the kernel's answer */
        senderlen = sizeof(sll);
        args[0] = (uint32_t)arp_fd;
        args[1] = (unsigned long)&sll;
        args[2] = (unsigned long)&senderlen;
        if (socketcall(SC_GETSOCKNAME, args) != 0) {
            fail("getsockname on a packet socket failed");
        }
        if (senderlen != 20) {
            fail("getsockname did not report a whole sockaddr_ll");
        }
        if (sll.sll_family != AF_PACKET || sll.sll_protocol != ETH_P_ARP_BE) {
            fail("getsockname did not report the family and protocol bound");
        }
        if (sll.sll_ifindex != 1 || sll.sll_hatype != 1 || sll.sll_halen != 6) {
            fail("getsockname did not report the interface it is bound to");
        }
        for (i = 0; i < 6; i++) {
            if (sll.sll_addr[i] != mac[i]) {
                fail("getsockname reported an address the interface does not have");
            }
        }

        args[0] = AF_PACKET;
        args[1] = SOCK_DGRAM;
        args[2] = ETH_P_IP_BE;
        ip_fd = socketcall(SC_SOCKET, args);
        if (ip_fd < 0) {
            fail_n("the IPv4 socket did not open", ip_fd);
        }
        for (i = 0; i < sizeof(sll); i++) {
            ((uint8_t *)&sll)[i] = 0;
        }
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = ETH_P_IP_BE;
        sll.sll_ifindex = 1;
        args[0] = (uint32_t)ip_fd;
        args[1] = (unsigned long)&sll;
        args[2] = sizeof(sll);
        if (socketcall(SC_BIND, args) != 0) {
            fail("a bind naming IPv4 was refused");
        }

        /* And what it sends: an ARP request of the probe's own making, under
           the protocol the socket is bound to. Sent as IPv4, which is what
           this layer used to do whatever the socket asked for, nothing would
           answer it. 10.0.2.2 is the gateway of the network QEMU puts a guest
           on, and it answers both this and the echo below. */
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
        frame[14] = 10; /* sender protocol address: this machine */
        frame[15] = 0;
        frame[16] = 2;
        frame[17] = 15;
        /* The name server's address, not the gateway's: the machine resolves
           the gateway by itself for the echo below, and a reply about it would
           arrive whether or not this request ever left. Nothing here asks
           about 10.0.2.3, so an answer about it can only be this request's. */
        frame[24] = 10;
        frame[25] = 0;
        frame[26] = 2;
        frame[27] = 3;
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
            fail("the ARP request did not send");
        }

        /* 10.0.2.2 answers the echo too, which is the IPv4 traffic the other
           socket must see and this one must not. */
        args[0] = 0x0A000202u;
        socketcall(SC_PING, args);

        for (tries = 0; tries < 100; tries++) {
            pfd.fd = arp_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (sys_poll(&pfd, 1, 20) > 0 && (pfd.revents & POLLIN)) {
                senderlen = 0;
                args[0] = (uint32_t)arp_fd;
                args[1] = (unsigned long)frame;
                args[2] = sizeof(frame);
                args[3] = 0x40; /* MSG_DONTWAIT */
                args[4] = 0;
                args[5] = 0;
                n = socketcall(SC_RECVFROM, args);
                if (n < 28) {
                    fail("a frame on the ARP socket was shorter than an ARP packet");
                }
                /* An ARP packet over Ethernet for IPv4: hardware 1,
                   protocol 0x0800, six-byte and four-byte addresses. */
                if (frame[0] != 0 || frame[1] != 1 || frame[2] != 0x08 || frame[3] != 0 ||
                    frame[4] != 6 || frame[5] != 4) {
                    fail("the ARP socket was given a frame that is not ARP");
                }
                arp_frames++;
                /* The answer to the request above: an ARP reply whose sender
                   is the address that was asked about, which nothing else
                   here asks for. */
                if (frame[6] == 0 && frame[7] == 2 && frame[14] == 10 && frame[15] == 0 &&
                    frame[16] == 2 && frame[17] == 3) {
                    arp_replies++;
                }
            }
            pfd.fd = ip_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (sys_poll(&pfd, 1, 20) > 0 && (pfd.revents & POLLIN)) {
                args[0] = (uint32_t)ip_fd;
                args[1] = (unsigned long)frame;
                args[2] = sizeof(frame);
                args[3] = 0x40;
                args[4] = 0;
                args[5] = 0;
                n = socketcall(SC_RECVFROM, args);
                if (n < 20) {
                    fail("a frame on the IPv4 socket was shorter than an IP header");
                }
                if ((frame[0] >> 4) != 4) {
                    fail("the IPv4 socket was given a frame that is not IPv4");
                }
                ip_frames++;
            }
            if (arp_replies > 0 && ip_frames > 0) {
                break;
            }
        }
        if (arp_frames == 0) {
            fail("the socket bound to ARP saw no ARP frame while one was resolved");
        }
        if (arp_replies == 0) {
            fail("nothing answered the ARP request the socket sent, so it did "
                 "not leave under the protocol the socket is bound to");
        }
        if (ip_frames == 0) {
            fail("the socket bound to IPv4 saw no IPv4 frame while one arrived");
        }

        /* And the kernel answered the same frames: the address it resolved is
           in its own cache, which is what a program reads out of /proc. */
        n = read_file("/proc/net/arp", (char *)frame, (int)sizeof(frame));
        if (n <= 0) {
            fail("/proc/net/arp could not be read");
        }
        if (!contains((const char *)frame, n, "10.0.2.2 ")) {
            fail("the address the machine resolved is not in its ARP cache, so "
                 "the packet socket took the frame away from the kernel");
        }

        sys_close(arp_fd);
        sys_close(ip_fd);
    }

    /* Closing must take the socket out of the delivery table, or the table
       fills with the dead and a later client cannot bind. There are two slots,
       so binding four in a row proves the release. */
    for (tries = 0; tries < 4; tries++) {
        int again;

        args[0] = AF_PACKET;
        args[1] = SOCK_DGRAM;
        args[2] = ETH_P_IP_BE;
        again = socketcall(SC_SOCKET, args);
        if (again < 0) {
            fail("a packet socket did not open after an earlier one closed");
        }
        sll.sll_ifindex = 1;
        args[0] = (uint32_t)again;
        args[1] = (unsigned long)&sll;
        args[2] = sizeof(sll);
        if (socketcall(SC_BIND, args) != 0) {
            fail("a bind failed, so closing did not free the delivery table");
        }
        sys_close(again);
    }

    /* And it is root's socket. A child that drops to an ordinary user must be
       refused, because the socket reads every frame the machine receives. */
    pid = sys_fork();
    if (pid == 0) {
        if (sys_setuid(1000) != 0) {
            sys_exit(2);
        }
        args[0] = AF_PACKET;
        args[1] = SOCK_DGRAM;
        args[2] = ETH_P_IP_BE;
        sys_exit(socketcall(SC_SOCKET, args) == -EPERM ? 0 : 3);
    }
    if (pid < 0) {
        fail("the fork for the unprivileged case failed");
    }
    status = -1;
    if (sys_waitpid(pid, &status, 0) != pid) {
        fail("waiting for the unprivileged case failed");
    }
    /* A child killed by a signal has a zero exit byte, so the signal is checked
       first: a crash on the privilege boundary must not read as a refusal. */
    if ((status & 0x7f) != 0) {
        fail("the unprivileged case died on a signal");
    }
    if (((status >> 8) & 0xff) != 0) {
        fail("an ordinary user was not refused a packet socket");
    }

    say("PACKETPROBE OK\n");
    sys_exit(0);
}
