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

typedef unsigned int size_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define STDOUT_FILENO 1

#define SYS_EXIT       1
#define SYS_FORK       2
#define SYS_WRITE      4
#define SYS_CLOSE      6
#define SYS_WAITPID    7
#define SYS_SOCKETCALL 102
#define SYS_NANOSLEEP  162
#define SYS_SETUID32   213
#define SYS_POLL       168
#define SYS_SELECT     142

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
#define ETH_P_IP_BE  0x0008
#define ETH_P_ARP_BE 0x0608

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
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
    return r;
}

static int sys_close(int fd)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_CLOSE), "b"(fd));
    return r;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static int socketcall(int call, uint32_t *args)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_SOCKETCALL), "b"(call), "c"(args)
                         : "memory");
    return r;
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
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_NANOSLEEP), "b"(&ts), "c"(0) : "memory");
}

static int sys_fork(void)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_FORK) : "memory");
    return r;
}

static int sys_setuid(int uid)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_SETUID32), "b"(uid));
    return r;
}

static int sys_waitpid(int pid, int *status, int options)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WAITPID), "b"(pid), "c"(status), "d"(options)
                         : "memory");
    return r;
}

/* The one's-complement sum an IP header carries; SLIRP checks it, so a wrong
   one is an offer that never comes rather than a visible failure. */
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
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_POLL), "b"(fds), "c"(n), "d"(timeout)
                         : "memory");
    return r;
}

static int sys_select(int n, void *rd, void *wr, void *ex, void *tv)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_SELECT), "b"(n), "c"(rd), "d"(wr), "S"(ex), "D"(tv)
                         : "memory");
    return r;
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
static uint8_t sender[16];
static uint8_t control[64];

void _start(void)
{
    uint32_t args[6];
    struct sockaddr_ll_probe sll;
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

    /* What it refuses. A stream packet socket is not a thing, a protocol other
       than IPv4 is not offered, and the option a client asks for to be told
       about checksum offload must be refused rather than promised. */
    args[0] = AF_PACKET;
    args[1] = SOCK_STREAM;
    args[2] = ETH_P_IP_BE;
    if (socketcall(SC_SOCKET, args) != -ESOCKTNOSUPPORT) {
        fail("a stream packet socket was not refused");
    }
    args[1] = SOCK_DGRAM;
    args[2] = ETH_P_ARP_BE;
    if (socketcall(SC_SOCKET, args) != -EINVAL) {
        fail("a packet socket for a protocol other than IPv4 was not refused");
    }

    args[2] = ETH_P_IP_BE;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0) {
        fail("a packet socket did not open");
    }

    r = 1;
    args[0] = (uint32_t)fd;
    args[1] = SOL_PACKET;
    args[2] = PACKET_AUXDATA;
    args[3] = (uint32_t)&r;
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
    args[1] = (uint32_t)&sll;
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
    args[1] = (uint32_t)frame;
    args[2] = DHCP_LEN;
    args[3] = 0;
    args[4] = (uint32_t)&sll;
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
        msg.msg_name = 0;
        msg.msg_namelen = 0;
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
        args[1] = (uint32_t)&msg;
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
        if (msg.msg_namelen != 0) {
            fail("a sender was reported for a link the stack does not record");
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
       they were, and a packet socket's successful one must report no address
       at all rather than a sender it never had. Both are checked on the same
       socket, which is empty again now that the offer has been read. */
    for (i = 0; i < sizeof(sender); i++) {
        sender[i] = 0xa5;
    }
    senderlen = 16;
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)frame;
    args[2] = 100;
    args[3] = 0x40; /* MSG_DONTWAIT */
    args[4] = (uint32_t)sender;
    args[5] = (uint32_t)&senderlen;
    if (socketcall(SC_RECVFROM, args) != -EAGAIN) {
        fail("an empty socket did not report that it was empty");
    }
    if (senderlen != 16 || sender[0] != 0xa5) {
        fail("a receive that failed wrote to the caller's address arguments");
    }

    /* And once more with a datagram: send a second discover and take the offer
       through recvfrom, where the reported length must be zero. */
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)discover;
    args[2] = DHCP_LEN;
    args[3] = 0;
    args[4] = (uint32_t)&sll;
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
        args[1] = (uint32_t)frame;
        args[2] = sizeof(frame);
        args[3] = 0x40;
        args[4] = (uint32_t)sender;
        args[5] = (uint32_t)&senderlen;
        r = socketcall(SC_RECVFROM, args);
        if (r < 0) {
            fail_n("a socket poll called readable had nothing for recvfrom", r);
        }
        if (senderlen != 0) {
            fail("a packet socket reported a sender it does not keep");
        }
        if (sender[0] != 0xa5) {
            fail("a packet socket wrote an address it does not have");
        }
        seen_offer = 1;
    }
    if (!seen_offer) {
        fail("the second offer never arrived");
    }

    sys_close(fd);

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
        args[1] = (uint32_t)&sll;
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
