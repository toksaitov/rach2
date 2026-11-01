/**
 * ip6optmusl: check the IPv6 socket options, their control messages and raw
 * ICMPv6's filter, through musl.
 *
 * The rows, each judged by what the kernel reports:
 *
 * A new AF_INET6 UDP socket reads IPV6_UNICAST_HOPS 64, IPV6_MULTICAST_HOPS
 * 1, IPV6_MULTICAST_LOOP 1, IPV6_MULTICAST_IF 0, IPV6_TCLASS 0 and
 * IPV6_RECVPKTINFO 0. A hop limit or class of 256 or -2 and a loop of 2 are
 * EINVAL, a multicast interface that does not exist ENODEV, an IPv6 option
 * on an AF_INET socket ENOPROTOOPT, and a join of a unicast address EINVAL.
 * IPV6_UNICAST_HOPS 7 and IPV6_TCLASS 0x28 read back, and -1 gives back 64
 * and 0.
 *
 * Over ::1, a receiver with IPV6_RECVPKTINFO, IPV6_RECVHOPLIMIT and
 * IPV6_RECVTCLASS takes a datagram from a sender with hop limit 7 and class
 * 0x28 with the control messages IPV6_PKTINFO (::1 and lo's index),
 * IPV6_HOPLIMIT 7 and IPV6_TCLASS 0x28, in that order. A sendmsg with
 * IPV6_HOPLIMIT 9 and IPV6_TCLASS 0x10 sends that datagram alone with them,
 * and the next send has the socket's again. A control buffer with room for
 * one int is cut with MSG_CTRUNC, and a socket that asked for nothing gets
 * no control data.
 *
 * A raw ICMPv6 socket's filter reads back all passing at first, and as
 * set after; of an echo request to ::1 sent with IPV6_UNICAST_HOPS 5 and
 * its reply, a socket that passes echo requests alone sees the request with
 * hop limit 5 and one that passes replies alone the reply with the kernel's
 * 64, and neither sees more. A UDP socket has no filter: ENOPROTOOPT.
 *
 * On the card, eth0, a UDP socket joins ff02::114 and a second join is
 * EADDRINUSE; a datagram sent to the group with IPV6_MULTICAST_IF eth0 and
 * IPV6_MULTICAST_HOPS 3 comes back to it with IPV6_PKTINFO ff02::114 on
 * eth0 and hop limit 3, while one sent with IPV6_MULTICAST_LOOP 0 does not:
 * the next datagram is a later one sent with the loop on. After the leave,
 * a second leave is EADDRNOTAVAIL and a datagram to the group is not
 * received: a datagram to ::1 sent after it comes first.
 *
 * A UDP datagram to fe80::1 with no scope id is EINVAL, but with an
 * IPV6_PKTINFO naming eth0 it is sent, and goes out eth0: the neighbour
 * solicitation for fe80::1 that it starts raises eth0's transmitted packets
 * in /proc/net/dev and Icmp6OutNeighborSolicits in /proc/net/snmp6.
 *
 * Last, in a network namespace of its own, whose `lo` carries no IPv6, a
 * UDP socket's join of ff02::114 naming eth0's index, or index 0, and an
 * IPV6_MULTICAST_IF of eth0's index are ENODEV.
 *
 * Datagrams over ::1 and the copies of a group's datagrams go round the
 * loopback ring in the order they were sent, so an absence is judged by the
 * datagram that arrives in its place, never by a wait. The card's
 * link-local address is tentative for a second after boot, and the first
 * send to the group waits for it, polling without a time limit of its own.
 *
 * It prints "IP6OPTMUSL OK" and exits 0 on success, or prints "IP6OPTMUSL
 * FAIL (detail)" and exits 1 at the first failure. etude's report
 * (probe_report.h), for the case ETUDE_CASE names, has an assertion for each
 * row in order: `defaults`, `option-errors`, `options-kept`, `ancillary`,
 * `sendmsg-control`, `ctrunc`, `no-control`, `filter`, `join`,
 * `multicast-loop`, `no-loop`, `leave`, `pktinfo-scope` and `namespace-join`. The first failure
 * fails its assertion and leaves the rest not run.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../probe_report.h"

/* The ports of the probe's UDP sockets over ::1 and on the card. */
#define PORT_LOOP  47011
#define PORT_GROUP 47012

/* An echo request's identifier, apart from the kernel's ping. */
#define ECHO_ID 0x6e01

/* The control buffers, of longs, which have the alignment of a struct
 * cmsghdr: one a receive fills and one a send's messages are built in. */
static unsigned long control[32];
static unsigned long sendctl[(CMSG_SPACE(sizeof(int)) * 2 + sizeof(long) - 1) / sizeof(long)];
static unsigned char data[64];

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("IP6OPTMUSL FAIL (%s)\n", what);
    report_stop(what);
    return 1;
}

/** The int of option `name` at SOL_IPV6 on `fd`, or -1000 when the read
 * fails. */
static int get6(int fd, int name)
{
    int v = 0;
    socklen_t len = sizeof(v);

    return getsockopt(fd, IPPROTO_IPV6, name, &v, &len) == 0 && len == sizeof(v) ? v : -1000;
}

/** Set option `name` at SOL_IPV6 on `fd` to `v`: 0, or the errno. */
static int set6(int fd, int name, int v)
{
    return setsockopt(fd, IPPROTO_IPV6, name, &v, sizeof(v)) == 0 ? 0 : errno;
}

/** Fill `sa` with `addr` and `port`, in the scope `scope`. */
static void address(struct sockaddr_in6 *sa, const struct in6_addr *addr, int port, int scope)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin6_family = AF_INET6;
    sa->sin6_port = htons((unsigned short)port);
    sa->sin6_addr = *addr;
    sa->sin6_scope_id = (unsigned)scope;
}

/** The `col`th count, from 1, of interface `name`'s line of /proc/net/dev,
 * or -1 when it has none. */
static long dev_count(const char *name, int col)
{
    char line[256];
    size_t n = strlen(name);
    FILE *f = fopen("/proc/net/dev", "r");
    long v = -1;

    while (f && fgets(line, sizeof(line), f)) {
        char *p = line + strspn(line, " ");
        int i;

        if (strncmp(p, name, n) != 0 || p[n] != ':') {
            continue;
        }
        p += n + 1;
        for (i = 0; i < col; i++) {
            char *end;

            v = strtol(p, &end, 10);
            if (end == p) {
                v = -1;
                break;
            }
            p = end;
        }
        break;
    }
    if (f) {
        fclose(f);
    }
    return v;
}

/** The counter `name` of /proc/net/snmp6, or -1 when it has none. */
static long snmp6(const char *name)
{
    char key[64];
    long v;
    FILE *f = fopen("/proc/net/snmp6", "r");

    while (f && fscanf(f, "%63s %ld", key, &v) == 2) {
        if (strcmp(key, name) == 0) {
            fclose(f);
            return v;
        }
    }
    if (f) {
        fclose(f);
    }
    return -1;
}

/** A UDP socket bound to `addr` and `port`, or -1. */
static int udp_bound(const struct in6_addr *addr, int port)
{
    struct sockaddr_in6 sa;
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);

    address(&sa, addr, port, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        return -1;
    }
    return fd;
}

/* What one recvmsg found: the bytes, the flags, the control length, and
 * each control message's value, -1 for one that did not come. `order` is
 * the types in the order they came, up to four. */
struct got {
    int n;
    int flags;
    size_t clen;
    struct in6_addr dst;
    int ifindex;
    int hlim;
    int tclass;
    int order[4];
    int count;
};

/** Receive one datagram on `fd` into `data`, with `room` bytes of control
 * buffer, and read its control messages into `g`; `nowait` asks
 * MSG_DONTWAIT. Returns the bytes or -1. */
static int take(int fd, size_t room, int nowait, struct got *g)
{
    struct msghdr m;
    struct iovec iov;
    struct cmsghdr *c;

    memset(&m, 0, sizeof(m));
    memset(g, 0, sizeof(*g));
    memset(control, 0, sizeof(control));
    g->ifindex = g->hlim = g->tclass = -1;
    iov.iov_base = data;
    iov.iov_len = sizeof(data);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = room ? control : NULL;
    m.msg_controllen = room;
    g->n = (int)recvmsg(fd, &m, nowait ? MSG_DONTWAIT : 0);
    if (g->n < 0) {
        return -1;
    }
    g->flags = m.msg_flags;
    g->clen = m.msg_controllen;
    for (c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level != IPPROTO_IPV6) {
            continue;
        }
        if (g->count < 4) {
            g->order[g->count++] = c->cmsg_type;
        }
        if (c->cmsg_type == IPV6_PKTINFO) {
            struct in6_pktinfo pi;

            memcpy(&pi, CMSG_DATA(c), sizeof(pi));
            g->dst = pi.ipi6_addr;
            g->ifindex = (int)pi.ipi6_ifindex;
        } else if (c->cmsg_type == IPV6_HOPLIMIT) {
            memcpy(&g->hlim, CMSG_DATA(c), sizeof(int));
        } else if (c->cmsg_type == IPV6_TCLASS) {
            memcpy(&g->tclass, CMSG_DATA(c), sizeof(int));
        }
    }
    return g->n;
}

/** Send the byte `tag` from `fd` to `to`, with the control messages
 * IPV6_HOPLIMIT `hlim` and IPV6_TCLASS `tclass` when they are not -1.
 * Returns 0 or the errno. */
static int send_tag(int fd, const struct sockaddr_in6 *to, unsigned char tag, int hlim, int tclass)
{
    struct msghdr m;
    struct iovec iov;
    struct cmsghdr *c;
    size_t used = 0;

    memset(&m, 0, sizeof(m));
    memset(sendctl, 0, sizeof(sendctl));
    iov.iov_base = &tag;
    iov.iov_len = 1;
    m.msg_name = (void *)to;
    m.msg_namelen = sizeof(*to);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    if (hlim != -1 || tclass != -1) {
        m.msg_control = sendctl;
        m.msg_controllen = sizeof(sendctl);
        c = CMSG_FIRSTHDR(&m);
        if (hlim != -1) {
            c->cmsg_level = IPPROTO_IPV6;
            c->cmsg_type = IPV6_HOPLIMIT;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(c), &hlim, sizeof(int));
            used += CMSG_SPACE(sizeof(int));
            c = CMSG_NXTHDR(&m, c);
        }
        if (tclass != -1) {
            c->cmsg_level = IPPROTO_IPV6;
            c->cmsg_type = IPV6_TCLASS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(c), &tclass, sizeof(int));
            used += CMSG_SPACE(sizeof(int));
        }
        m.msg_controllen = used;
    }
    return sendmsg(fd, &m, 0) == 1 ? 0 : errno;
}

/** Send the byte `tag` from `fd` to `to` with the control message
 * IPV6_PKTINFO naming no source and the interface `ifindex`. Returns 0 or
 * the errno. */
static int send_via(int fd, const struct sockaddr_in6 *to, unsigned char tag, int ifindex)
{
    unsigned long buf[(CMSG_SPACE(sizeof(struct in6_pktinfo)) + sizeof(long) - 1) / sizeof(long)];
    struct in6_pktinfo pi;
    struct msghdr m;
    struct iovec iov;
    struct cmsghdr *c;

    memset(&m, 0, sizeof(m));
    memset(buf, 0, sizeof(buf));
    memset(&pi, 0, sizeof(pi));
    pi.ipi6_ifindex = (unsigned)ifindex;
    iov.iov_base = &tag;
    iov.iov_len = 1;
    m.msg_name = (void *)to;
    m.msg_namelen = sizeof(*to);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = buf;
    m.msg_controllen = CMSG_SPACE(sizeof(pi));
    c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = IPPROTO_IPV6;
    c->cmsg_type = IPV6_PKTINFO;
    c->cmsg_len = CMSG_LEN(sizeof(pi));
    memcpy(CMSG_DATA(c), &pi, sizeof(pi));
    return sendmsg(fd, &m, 0) == 1 ? 0 : errno;
}

/** The defaults, the refusals and the values kept, on new sockets. */
static int options(void)
{
    struct ipv6_mreq mreq;
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    int fd4 = socket(AF_INET, SOCK_DGRAM, 0);
    int v = 7;

    if (fd < 0 || fd4 < 0) {
        return fail("socket");
    }
    if (get6(fd, IPV6_UNICAST_HOPS) != 64 || get6(fd, IPV6_MULTICAST_HOPS) != 1 ||
        get6(fd, IPV6_MULTICAST_LOOP) != 1 || get6(fd, IPV6_MULTICAST_IF) != 0 ||
        get6(fd, IPV6_TCLASS) != 0 || get6(fd, IPV6_RECVPKTINFO) != 0) {
        return fail("defaults");
    }
    report_held();

    memset(&mreq, 0, sizeof(mreq));
    mreq.ipv6mr_multiaddr = in6addr_loopback;
    if (set6(fd, IPV6_UNICAST_HOPS, 256) != EINVAL || set6(fd, IPV6_MULTICAST_HOPS, -2) != EINVAL ||
        set6(fd, IPV6_TCLASS, 256) != EINVAL || set6(fd, IPV6_MULTICAST_LOOP, 2) != EINVAL ||
        set6(fd, IPV6_MULTICAST_IF, 999) != ENODEV ||
        setsockopt(fd4, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &v, sizeof(v)) == 0 ||
        errno != ENOPROTOOPT ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) == 0 ||
        errno != EINVAL) {
        return fail("option-errors");
    }
    report_held();

    if (set6(fd, IPV6_UNICAST_HOPS, 7) != 0 || set6(fd, IPV6_TCLASS, 0x28) != 0 ||
        get6(fd, IPV6_UNICAST_HOPS) != 7 || get6(fd, IPV6_TCLASS) != 0x28 ||
        set6(fd, IPV6_UNICAST_HOPS, -1) != 0 || set6(fd, IPV6_TCLASS, -1) != 0 ||
        get6(fd, IPV6_UNICAST_HOPS) != 64 || get6(fd, IPV6_TCLASS) != 0) {
        return fail("options-kept");
    }
    report_held();
    close(fd);
    close(fd4);
    return 0;
}

/** The control messages of datagrams over ::1. */
static int ancillary(void)
{
    struct sockaddr_in6 to;
    struct got g;
    int rx = udp_bound(&in6addr_loopback, PORT_LOOP);
    int tx = socket(AF_INET6, SOCK_DGRAM, 0);
    int plain = socket(AF_INET6, SOCK_DGRAM, 0);
    int lo = (int)if_nametoindex("lo");

    if (rx < 0 || tx < 0 || plain < 0 || lo <= 0) {
        return fail("ancillary: setup");
    }
    address(&to, &in6addr_loopback, PORT_LOOP, 0);
    if (set6(rx, IPV6_RECVPKTINFO, 1) != 0 || set6(rx, IPV6_RECVHOPLIMIT, 1) != 0 ||
        set6(rx, IPV6_RECVTCLASS, 1) != 0 || set6(tx, IPV6_UNICAST_HOPS, 7) != 0 ||
        set6(tx, IPV6_TCLASS, 0x28) != 0 || send_tag(tx, &to, 1, -1, -1) != 0 ||
        take(rx, sizeof(control), 0, &g) != 1 || data[0] != 1) {
        return fail("ancillary: datagram");
    }
    if (g.count != 3 || g.order[0] != IPV6_PKTINFO || g.order[1] != IPV6_HOPLIMIT ||
        g.order[2] != IPV6_TCLASS || !IN6_IS_ADDR_LOOPBACK(&g.dst) || g.ifindex != lo ||
        g.hlim != 7 || g.tclass != 0x28 || (g.flags & MSG_CTRUNC)) {
        return fail("ancillary");
    }
    report_held();

    if (send_tag(tx, &to, 2, 9, 0x10) != 0 || send_tag(tx, &to, 3, -1, -1) != 0 ||
        take(rx, sizeof(control), 0, &g) != 1 || data[0] != 2 || g.hlim != 9 || g.tclass != 0x10) {
        return fail("sendmsg-control: with control");
    }
    if (take(rx, sizeof(control), 0, &g) != 1 || data[0] != 3 || g.hlim != 7 || g.tclass != 0x28) {
        return fail("sendmsg-control: the socket's again");
    }
    report_held();

    if (send_tag(tx, &to, 4, -1, -1) != 0 || take(rx, CMSG_SPACE(sizeof(int)), 0, &g) != 1 ||
        data[0] != 4 || !(g.flags & MSG_CTRUNC) || g.clen > CMSG_SPACE(sizeof(int))) {
        return fail("ctrunc");
    }
    report_held();

    close(rx);
    rx = udp_bound(&in6addr_loopback, PORT_LOOP);
    if (rx < 0 || send_tag(plain, &to, 5, -1, -1) != 0 || take(rx, sizeof(control), 0, &g) != 1 ||
        data[0] != 5 || g.clen != 0 || g.count != 0) {
        return fail("no-control");
    }
    report_held();
    close(rx);
    close(tx);
    close(plain);
    return 0;
}

/** Two raw ICMPv6 sockets with complementary filters, and an echo request
 * to ::1 from the first. */
static int filter(void)
{
    struct icmp6_filter f, back;
    struct sockaddr_in6 to;
    struct got g;
    socklen_t len = sizeof(back);
    unsigned char echo[8];
    int a = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    int b = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    int u = socket(AF_INET6, SOCK_DGRAM, 0);
    int i;

    if (a < 0 || b < 0 || u < 0) {
        return fail("filter: socket");
    }
    memset(&back, 0xff, sizeof(back));
    if (getsockopt(a, IPPROTO_ICMPV6, ICMP6_FILTER, &back, &len) != 0 || len != sizeof(back)) {
        return fail("filter: read");
    }
    for (i = 0; i < 256; i++) {
        if (ICMP6_FILTER_WILLBLOCK(i, &back)) {
            return fail("filter: a new filter blocks a type");
        }
    }
    ICMP6_FILTER_SETBLOCKALL(&f);
    ICMP6_FILTER_SETPASS(ICMP6_ECHO_REQUEST, &f);
    if (setsockopt(a, IPPROTO_ICMPV6, ICMP6_FILTER, &f, sizeof(f)) != 0) {
        return fail("filter: set");
    }
    len = sizeof(back);
    if (getsockopt(a, IPPROTO_ICMPV6, ICMP6_FILTER, &back, &len) != 0 ||
        memcmp(&back, &f, sizeof(f)) != 0) {
        return fail("filter: read back");
    }
    ICMP6_FILTER_SETBLOCKALL(&f);
    ICMP6_FILTER_SETPASS(ICMP6_ECHO_REPLY, &f);
    if (setsockopt(b, IPPROTO_ICMPV6, ICMP6_FILTER, &f, sizeof(f)) != 0 ||
        setsockopt(u, IPPROTO_ICMPV6, ICMP6_FILTER, &f, sizeof(f)) == 0 || errno != ENOPROTOOPT) {
        return fail("filter: set, or a UDP socket's");
    }
    if (set6(a, IPV6_UNICAST_HOPS, 5) != 0 || set6(a, IPV6_RECVHOPLIMIT, 1) != 0 ||
        set6(b, IPV6_RECVHOPLIMIT, 1) != 0) {
        return fail("filter: options");
    }
    memset(echo, 0, sizeof(echo));
    echo[0] = ICMP6_ECHO_REQUEST;
    echo[4] = ECHO_ID >> 8;
    echo[5] = ECHO_ID & 0xff;
    echo[7] = 1;
    address(&to, &in6addr_loopback, 0, 0);
    if (sendto(a, echo, sizeof(echo), 0, (struct sockaddr *)&to, sizeof(to)) != sizeof(echo)) {
        return fail("filter: send");
    }
    if (take(b, sizeof(control), 0, &g) != 8 || data[0] != ICMP6_ECHO_REPLY || g.hlim != 64) {
        return fail("filter: the reply on the second socket");
    }
    if (take(a, sizeof(control), 1, &g) != 8 || data[0] != ICMP6_ECHO_REQUEST || g.hlim != 5) {
        return fail("filter: the request on the first socket");
    }
    if (take(a, sizeof(control), 1, &g) >= 0 || errno != EAGAIN ||
        take(b, sizeof(control), 1, &g) >= 0 || errno != EAGAIN) {
        return fail("filter: a blocked message came");
    }
    report_held();
    close(a);
    close(b);
    close(u);
    return 0;
}

/** Send `tag` from `fd` to `to` until the card's link-local address is no
 * longer tentative: EADDRNOTAVAIL until then. Returns 0 or the errno. */
static int send_ready(int fd, const struct sockaddr_in6 *to, unsigned char tag)
{
    int r;

    while ((r = send_tag(fd, to, tag, -1, -1)) == EADDRNOTAVAIL) {
        usleep(100000);
    }
    return r;
}

/** A group on the card, joined, sent to with and without the loop, and
 * left. */
static int group(void)
{
    struct ipv6_mreq mreq;
    struct sockaddr_in6 to, loop;
    struct got g;
    int eth = (int)if_nametoindex("eth0");
    int rx = udp_bound(&in6addr_any, PORT_GROUP);
    int tx = socket(AF_INET6, SOCK_DGRAM, 0);
    char why[80];
    int r;

    if (eth <= 0 || rx < 0 || tx < 0) {
        return fail("join: setup");
    }
    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET6, "ff02::114", &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = (unsigned)eth;
    if (setsockopt(rx, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) != 0) {
        return fail("join");
    }
    if (setsockopt(rx, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) == 0 ||
        errno != EADDRINUSE) {
        return fail("join: again");
    }
    report_held();

    address(&to, &mreq.ipv6mr_multiaddr, PORT_GROUP, 0);
    address(&loop, &in6addr_loopback, PORT_GROUP, 0);
    if (set6(rx, IPV6_RECVPKTINFO, 1) != 0 || set6(rx, IPV6_RECVHOPLIMIT, 1) != 0 ||
        set6(tx, IPV6_MULTICAST_IF, eth) != 0 || get6(tx, IPV6_MULTICAST_IF) != eth ||
        set6(tx, IPV6_MULTICAST_HOPS, 3) != 0) {
        return fail("multicast-loop: options");
    }
    if ((r = send_ready(tx, &to, 1)) != 0) {
        snprintf(why, sizeof(why), "multicast-loop: send: %s", strerror(r));
        return fail(why);
    }
    if (take(rx, sizeof(control), 0, &g) != 1 || data[0] != 1 ||
        !IN6_ARE_ADDR_EQUAL(&g.dst, &mreq.ipv6mr_multiaddr) || g.ifindex != eth || g.hlim != 3) {
        return fail("multicast-loop");
    }
    report_held();

    if (set6(tx, IPV6_MULTICAST_LOOP, 0) != 0 || send_tag(tx, &to, 2, -1, -1) != 0 ||
        set6(tx, IPV6_MULTICAST_LOOP, 1) != 0 || send_tag(tx, &to, 3, -1, -1) != 0 ||
        take(rx, sizeof(control), 0, &g) != 1 || data[0] != 3) {
        return fail("no-loop");
    }
    report_held();

    if (setsockopt(rx, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq, sizeof(mreq)) != 0 ||
        setsockopt(rx, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq, sizeof(mreq)) == 0 ||
        errno != EADDRNOTAVAIL) {
        return fail("leave");
    }
    if (send_tag(tx, &to, 4, -1, -1) != 0 || send_tag(tx, &loop, 5, -1, -1) != 0 ||
        take(rx, sizeof(control), 0, &g) != 1 || data[0] != 5) {
        return fail("leave: a datagram to the group came");
    }
    report_held();
    close(rx);
    close(tx);
    return 0;
}

/** A datagram to fe80::1 with no scope id, sent by eth0 as IPV6_PKTINFO
 * says. Runs after `group`, once eth0's link-local address is no longer
 * tentative, so that the send has a source. */
static int pktinfo_scope(void)
{
    struct sockaddr_in6 to;
    struct in6_addr peer;
    int eth = (int)if_nametoindex("eth0");
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    long tx, ns;
    char why[80];
    int r;

    inet_pton(AF_INET6, "fe80::1", &peer);
    address(&to, &peer, PORT_GROUP, 0);
    if (eth <= 0 || fd < 0) {
        return fail("pktinfo-scope: setup");
    }
    if (send_tag(fd, &to, 6, -1, -1) != EINVAL) {
        return fail("pktinfo-scope: sent with no interface");
    }
    tx = dev_count("eth0", 10);
    ns = snmp6("Icmp6OutNeighborSolicits");
    if (tx < 0 || ns < 0) {
        return fail("pktinfo-scope: counters");
    }
    if ((r = send_via(fd, &to, 7, eth)) != 0) {
        snprintf(why, sizeof(why), "pktinfo-scope: send: %s", strerror(r));
        return fail(why);
    }
    if (dev_count("eth0", 10) <= tx || snmp6("Icmp6OutNeighborSolicits") <= ns) {
        return fail("pktinfo-scope: nothing went out eth0");
    }
    report_held();
    close(fd);
    return 0;
}

/** Joins and a multicast interface naming eth0 from a network namespace
 * of the probe's own, which it enters for good, so this runs last. */
static int namespace_join(void)
{
    struct ipv6_mreq mreq;
    int eth = (int)if_nametoindex("eth0");
    int fd;

    if (eth <= 0 || unshare(CLONE_NEWNET) != 0) {
        return fail("namespace-join: unshare");
    }
    fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) {
        return fail("namespace-join: socket");
    }
    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET6, "ff02::114", &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = (unsigned)eth;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) == 0 ||
        errno != ENODEV) {
        return fail("namespace-join: eth0's index");
    }
    mreq.ipv6mr_interface = 0;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) == 0 ||
        errno != ENODEV) {
        return fail("namespace-join: index 0");
    }
    if (set6(fd, IPV6_MULTICAST_IF, eth) != ENODEV) {
        return fail("namespace-join: IPV6_MULTICAST_IF");
    }
    report_held();
    close(fd);
    return 0;
}

/** Begin the report and plan its assertions, in the order they are
 * checked. */
static void plan(void)
{
    static const char *const ids[] = {
        "defaults", "option-errors", "options-kept",  "ancillary",     "sendmsg-control",
        "ctrunc",   "no-control",    "filter",        "join",          "multicast-loop",
        "no-loop",  "leave",         "pktinfo-scope", "namespace-join"};
    unsigned i;

    report_begin(report_case(environ, "guest.default.ip6optmusl"));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        report_plan(ids[i]);
    }
}

int main(void)
{
    plan();
    if (options() != 0 || ancillary() != 0 || filter() != 0 || group() != 0 ||
        pktinfo_scope() != 0 || namespace_join() != 0) {
        return 1;
    }
    report_end();
    printf("IP6OPTMUSL OK\n");
    return 0;
}
