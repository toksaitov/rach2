/*
 * netlink.c: NETLINK_ROUTE and NETLINK_SOCK_DIAG sockets. The contract is in
 * netlink.h; what is here is the reading of requests and the building of the
 * answers from the interface records and the sockets.
 */

#include "netlink.h"
#include "socket.h"
#include "../net.h"
#include "../netif.h"
#include "../netconf.h"
#include "../driver/netdev.h"
#include "../arp/arp.h"
#include "../ip/ip.h"
#include "../tcp/tcp.h"
#include "../../ipc/unix.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../proc/process.h"
#include "../../proc/wait_queue.h"

/* The message header, and the types and flags of Linux's uapi netlink.h and
 * rtnetlink.h that the answers use. */
#define NLMSG_HDRLEN  16
#define NLMSG_NOOP    1
#define NLMSG_ERROR   2
#define NLMSG_DONE    3
#define NLM_F_REQUEST 0x001u
#define NLM_F_MULTI   0x002u
#define NLM_F_ACK     0x004u
#define NLM_F_REPLACE 0x100u
#define NLM_F_EXCL    0x200u
#define NLM_F_DUMP    0x300u
#define RTM_NEWLINK   16
#define RTM_GETLINK   18
#define RTM_SETLINK   19
#define RTM_NEWADDR   20
#define RTM_DELADDR   21
#define RTM_GETADDR   22
#define RTM_NEWROUTE  24
#define RTM_DELROUTE  25
#define RTM_GETROUTE  26
#define RTM_NEWNEIGH  28
#define RTM_DELNEIGH  29
#define RTM_GETNEIGH  30
#define RTM_NEWRULE   32
#define RTM_GETRULE   34
#define RTM_NEWQDISC  36
#define RTM_GETQDISC  38

/* Attributes of a link, an address and a route. */
#define IFLA_ADDRESS   1
#define IFLA_BROADCAST 2
#define IFLA_IFNAME    3
#define IFLA_MTU       4
#define IFLA_STATS     7
#define IFLA_OPERSTATE 16
#define IFLA_STATS64   23
#define IFA_ADDRESS    1
#define IFA_LOCAL      2
#define IFA_LABEL      3
#define IFA_BROADCAST  4
#define RTA_DST        1
#define RTA_OIF        4
#define RTA_GATEWAY    5
#define RTA_PREFSRC    7
#define RTA_PRIORITY   6
#define RTA_TABLE      15
#define NDA_DST        1
#define NDA_LLADDR     2
#define NDA_IFINDEX    8
#define NTF_PROXY      0x08u

#define ARPHRD_ETHER      1
#define IFF_LOWER_UP      0x10000u
#define IF_OPER_UP        6
#define IF_OPER_DOWN      2
#define IFA_F_PERMANENT   0x80u
#define IFA_F_SECONDARY   0x01u
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK     253
#define RT_TABLE_MAIN     254
#define RT_TABLE_DEFAULT  253
#define RT_TABLE_LOCAL    255
#define RT_SCOPE_HOST     254
#define RTN_LOCAL         2
#define RTM_F_CLONED      0x200u
#define FRA_PRIORITY      6
#define FRA_TABLE         15
#define FR_ACT_TO_TBL     1
#define TCA_KIND          1
#define TC_H_ROOT         0xFFFFFFFFu
#define RTPROT_KERNEL     2
#define RTPROT_BOOT       3
#define RTN_UNICAST       1
#define NUD_REACHABLE     0x02u
#define NUD_PERMANENT     0x80u
#define RTPROT_STATIC     4

/* Linux's uapi sock_diag.h, inet_diag.h and unix_diag.h: the request type,
 * the sizes of the requests and the messages, the attributes, and the
 * states, which are TCP's numbers for every kind of socket. */
#define SOCK_DIAG_BY_FAMILY    20
#define AF_INET6               10
#define IPPROTO_RAW            255
#define DIAG_INET_REQ_LEN      56
#define DIAG_INET_MSG_LEN      72
#define DIAG_UNIX_REQ_LEN      24
#define DIAG_UNIX_MSG_LEN      16
#define INET_DIAG_REQ_PROTOCOL 3
#define INET_DIAG_INFO         2
#define INET_DIAG_CONG         4
#define INET_DIAG_PROTOCOL     10
#define TCP_INFO_LEN           104
#define TCPI_OPT_SACK          2
#define TCPI_OPT_WSCALE        4
#define DIAG_TIMER_RETRANS     1
#define DIAG_TIMER_TIMEWAIT    3
#define DIAG_TIMER_PROBE       4
#define DIAG_TICK_US           10000
#define UNIX_DIAG_NAME         0
#define UNIX_DIAG_PEER         2
#define UNIX_DIAG_RQLEN        4
#define UNIX_DIAG_UID          7
#define UDIAG_SHOW_NAME        0x01u
#define UDIAG_SHOW_PEER        0x04u
#define UDIAG_SHOW_RQLEN       0x10u
#define UDIAG_SHOW_UID         0x40u
#define DIAG_ESTABLISHED       1
#define DIAG_CLOSE             7
#define DIAG_LISTEN            10

/* Policy: an answer datagram is at most a page, which every reader's buffer
 * holds: Chromium reads with 4096 bytes and BusyBox with 8192. A message is
 * at most 512 bytes, a link's with its two sets of counters among them. */
#define NL_DGRAM_MAX 4096u
#define NL_MSG_MAX   512u

/* One queued datagram; its bytes follow the record. */
struct nl_dgram {
    struct nl_dgram *next;
    uint32_t len;
};

struct netlink_state {
    struct netlink_state *next; /* every netlink socket's, for the port ids */
    int protocol;
    uint32_t pid;
    uint32_t groups;
    int bound;
    struct nl_dgram *head, *tail;
};

static struct netlink_state *states;

/* An answer being built: the datagram so far and the request's sequence
 * number and port id, which every message of the answer repeats. */
struct nl_build {
    struct socket *sock;
    uint8_t buf[NL_DGRAM_MAX];
    uint32_t len;
    uint32_t seq;
    uint32_t pid;
    int failed;
};

/* The port ids handed to sockets bound with 0 after their process's first:
 * above 2^31, where no process id is. */
static uint32_t next_port = 0x80000000u;

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static struct netlink_state *state_of(struct socket *sock)
{
    return (struct netlink_state *)sock->netlink;
}

int netlink_open(struct socket *sock, int protocol)
{
    struct netlink_state *st;
    uint32_t flags;

    if (protocol != NETLINK_ROUTE && protocol != NETLINK_SOCK_DIAG &&
        protocol != NETLINK_KOBJECT_UEVENT) {
        return -EPROTONOSUPPORT;
    }
    st = kzalloc(sizeof(*st));
    if (!st) {
        return -ENOMEM;
    }
    st->protocol = protocol;
    flags = net_lock();
    st->next = states;
    states = st;
    sock->netlink = st;
    net_unlock(flags);
    return 0;
}

void netlink_close(struct socket *sock)
{
    struct netlink_state *st = state_of(sock);
    struct netlink_state **link = &states;
    uint32_t flags;

    if (!st) {
        return;
    }
    flags = net_lock();
    while (*link && *link != st) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = st->next;
    }
    sock->netlink = 0;
    net_unlock(flags);
    while (st->head) {
        struct nl_dgram *d = st->head;

        st->head = d->next;
        kfree(d);
    }
    kfree(st);
}

/* Whether another netlink socket has port id `pid`; net_lock held. */
static int port_taken(const struct netlink_state *self, uint32_t pid)
{
    const struct netlink_state *o;

    for (o = states; o; o = o->next) {
        if (o != self && o->bound && o->pid == pid) {
            return 1;
        }
    }
    return 0;
}

int netlink_bind(struct socket *sock, uint32_t pid, uint32_t groups)
{
    struct netlink_state *st = state_of(sock);
    uint32_t flags = net_lock();

    if (pid == 0) {
        pid = current_proc ? current_proc->tgid : 0;
        while (pid == 0 || port_taken(st, pid)) {
            pid = next_port++;
        }
    } else if (port_taken(st, pid)) {
        net_unlock(flags);
        return -EADDRINUSE;
    }
    st->pid = pid;
    st->groups = groups;
    st->bound = 1;
    net_unlock(flags);
    return 0;
}

void netlink_name(struct socket *sock, uint8_t *sa)
{
    struct netlink_state *st = state_of(sock);

    if (!st->bound) {
        (void)netlink_bind(sock, 0, 0);
    }
    k_memset(sa, 0, SOCKADDR_NL_LEN);
    put16(sa, AF_NETLINK);
    put32(sa + 4, st->pid);
    put32(sa + 8, st->groups);
}

/* Queue the datagram built so far, if it has anything. */
static void nl_flush(struct nl_build *b)
{
    struct netlink_state *st = state_of(b->sock);
    struct nl_dgram *d;
    uint32_t flags;

    if (b->len == 0 || b->failed) {
        return;
    }
    d = kmalloc(sizeof(*d) + b->len);
    if (!d) {
        b->failed = 1;
        return;
    }
    d->next = 0;
    d->len = b->len;
    k_memcpy(d + 1, b->buf, b->len);
    flags = net_lock();
    if (st->tail) {
        st->tail->next = d;
    } else {
        st->head = d;
    }
    st->tail = d;
    net_unlock(flags);
    b->len = 0;
    wq_wake(&b->sock->waiters, WQ_DATA);
}

/* Start a message of `type` in `msg` with `body` bytes of zeroed body;
 * returns where the body starts. */
static uint8_t *msg_start(const struct nl_build *b, uint8_t *msg, uint16_t type, uint16_t flags,
                          uint32_t body)
{
    k_memset(msg, 0, NL_MSG_MAX);
    put32(msg, NLMSG_HDRLEN + body);
    put16(msg + 4, type);
    put16(msg + 6, flags);
    put32(msg + 8, b->seq);
    put32(msg + 12, b->pid);
    return msg + NLMSG_HDRLEN;
}

/* Append attribute `type` of `len` bytes to `msg`, padded to four. */
static void msg_attr(uint8_t *msg, uint16_t type, const void *data, uint16_t len)
{
    uint32_t at = get32(msg);

    put16(msg + at, (uint16_t)(4 + len));
    put16(msg + at + 2, type);
    k_memcpy(msg + at + 4, data, len);
    put32(msg, at + ((4u + len + 3u) & ~3u));
}

static void msg_attr32(uint8_t *msg, uint16_t type, uint32_t v)
{
    uint8_t le[4];

    put32(le, v);
    msg_attr(msg, type, le, 4);
}

/* An IPv4 address attribute: the address in network order, as it goes on
 * the wire. */
static void msg_attr_ip(uint8_t *msg, uint16_t type, uint32_t ip)
{
    uint8_t be[4];

    be[0] = (uint8_t)(ip >> 24);
    be[1] = (uint8_t)(ip >> 16);
    be[2] = (uint8_t)(ip >> 8);
    be[3] = (uint8_t)ip;
    msg_attr(msg, type, be, 4);
}

/* Add the finished message `msg` to the answer, in a new datagram when the
 * current one has no room. An answer whose datagram could not be queued
 * takes nothing more: the bytes it holds are still there, with no room
 * after them. */
static void nl_append(struct nl_build *b, const uint8_t *msg)
{
    uint32_t len = get32(msg);

    if (b->len + len > NL_DGRAM_MAX) {
        nl_flush(b);
    }
    if (b->failed) {
        return;
    }
    k_memcpy(b->buf + b->len, msg, len);
    b->len += len;
}

static uint32_t prefix_of(uint32_t mask)
{
    uint32_t n = 0;

    while (n < 32 && (mask & (0x80000000u >> n))) {
        n++;
    }
    return n;
}

/* A link, with its counters in Linux's `struct rtnl_link_stats`, 24 words
 * of which this interface keeps seven: the packets, bytes and drops each
 * way, and the receive errors; and the same in `struct rtnl_link_stats64`,
 * 25 eight-byte words, which Linux sends beside it and iproute2 reads
 * first, and without which it prints a counter of the second set that the
 * first does not have, the drops for other hosts, from memory it never
 * set. */
static void link_msg(struct nl_build *b, const struct netdev *nd, uint16_t flags)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *ifi = msg_start(b, msg, RTM_NEWLINK, flags, 16);
    static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t up = netif_is_up() ? IFF_UP | IFF_RUNNING | IFF_LOWER_UP : 0;
    uint8_t oper = up ? IF_OPER_UP : IF_OPER_DOWN;
    uint8_t stats[96];
    uint8_t stats64[200];
    int i;

    put16(ifi + 2, ARPHRD_ETHER);
    put32(ifi + 4, (uint32_t)nd->index);
    put32(ifi + 8, up | IFF_BROADCAST | IFF_MULTICAST);
    msg_attr(msg, IFLA_IFNAME, nd->name, (uint16_t)(k_strlen(nd->name) + 1));
    msg_attr(msg, IFLA_ADDRESS, nd->mac, 6);
    msg_attr(msg, IFLA_BROADCAST, broadcast, 6);
    msg_attr32(msg, IFLA_MTU, 1500);
    msg_attr(msg, IFLA_OPERSTATE, &oper, 1);
    k_memset(stats, 0, sizeof(stats));
    put32(stats + 0, nd->rx_packets);
    put32(stats + 4, nd->tx_packets);
    put32(stats + 8, nd->rx_bytes);
    put32(stats + 12, nd->tx_bytes);
    put32(stats + 16, nd->rx_errors);
    put32(stats + 24, nd->rx_dropped);
    put32(stats + 28, nd->tx_dropped);
    msg_attr(msg, IFLA_STATS, stats, sizeof(stats));
    k_memset(stats64, 0, sizeof(stats64));
    for (i = 0; i < 8; i++) {
        k_memcpy(stats64 + 8 * i, stats + 4 * i, 4); /* each low word; the high ones 0 */
    }
    msg_attr(msg, IFLA_STATS64, stats64, sizeof(stats64));
    nl_append(b, msg);
}

/* A neighbour, the ARP cache's mapping `row`, on the interface of index
 * `index`. The cache keeps no age, so every mapping is reachable, but for
 * one set by hand as permanent. */
static void neigh_msg(struct nl_build *b, const struct arp_row *row, int index)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *ndm = msg_start(b, msg, RTM_NEWNEIGH, NLM_F_MULTI, 12);

    ndm[0] = AF_INET;
    put32(ndm + 4, (uint32_t)index);
    put16(ndm + 8, row->permanent ? NUD_PERMANENT : NUD_REACHABLE);
    ndm[11] = RTN_UNICAST;
    msg_attr_ip(msg, NDA_DST, row->ip);
    msg_attr(msg, NDA_LLADDR, row->mac, 6);
    nl_append(b, msg);
}

/* Whether the interface name `name`, in an array of `cap` bytes, is the
 * `len` bytes at `text`, which may end at the name's NUL, run on past it, or
 * stop at its last letter, since a netlink string needs no terminator. */
static int same_name(const char *name, uint32_t cap, const char *text, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        if (i == cap || name[i] != text[i]) {
            return 0;
        }
        if (name[i] == '\0') {
            return 1;
        }
    }
    return len == cap || name[len] == '\0';
}

/*
 * The interface a request for one link names: by the index of its
 * `ifinfomsg`, or with that 0, by its IFLA_IFNAME attribute, which is how
 * `ip link show eth0` asks. `body` is the request's length past its header.
 */
static struct netdev *link_asked(const uint8_t *req, uint32_t body)
{
    const uint8_t *attr = req + NLMSG_HDRLEN + 16;
    const uint8_t *end = req + NLMSG_HDRLEN + body;
    uint32_t index = get32(req + NLMSG_HDRLEN + 4);
    const char *name = 0;
    uint32_t name_len = 0;
    int i, n = netdev_seen_count();

    while (index == 0 && attr + 4 <= end) {
        uint16_t len = get16(attr);

        if (len < 4 || attr + len > end) {
            break;
        }
        if (get16(attr + 2) == IFLA_IFNAME) {
            name = (const char *)attr + 4;
            name_len = len - 4u;
        }
        attr += (len + 3u) & ~3u;
    }
    for (i = 0; i < n; i++) {
        struct netdev *nd = netdev_at(i);

        if (!nd) {
            continue;
        }
        if (index != 0 ? (uint32_t)nd->index == index
                       : name && same_name(nd->name, sizeof(nd->name), name, name_len)) {
            return nd;
        }
    }
    return 0;
}

/*
 * The neighbours a dump request `req` of `body` bytes asks for, from a
 * snapshot of the ARP cache: none for a family other than IPv4 or any, or
 * for the proxy table, which this kernel has none of, and only those of one
 * interface when its `ndmsg` or an NDA_IFINDEX attribute names one.
 */
static void neigh_dump(struct nl_build *b, const uint8_t *req, uint32_t body)
{
    struct arp_row rows[ARP_CACHE_SIZE];
    const uint8_t *ndm = req + NLMSG_HDRLEN;
    const uint8_t *attr = ndm + 12;
    const uint8_t *end = ndm + body;
    uint32_t want = body >= 8 ? get32(ndm + 4) : 0;
    int k, i, m, n = netdev_seen_count();

    if ((body >= 1 && ndm[0] != 0 && ndm[0] != AF_INET) || (body >= 11 && (ndm[10] & NTF_PROXY))) {
        return;
    }
    while (body >= 12 && attr + 4 <= end) {
        uint16_t len = get16(attr);

        if (len < 4 || attr + len > end) {
            break;
        }
        if (get16(attr + 2) == NDA_IFINDEX && len >= 8) {
            want = get32(attr + 4);
        }
        attr += (len + 3u) & ~3u;
    }
    m = arp_snapshot(rows, ARP_CACHE_SIZE);
    for (k = 0; k < m; k++) {
        for (i = 0; i < n; i++) {
            const struct netdev *nd = netdev_at(i);

            if (nd && rows[k].iface && (want == 0 || (uint32_t)nd->index == want) &&
                same_name(nd->name, sizeof(nd->name), rows[k].iface, sizeof(nd->name))) {
                neigh_msg(b, &rows[k], nd->index);
            }
        }
    }
}

/* Address `ip`/`mask` of `nd`, its first or, with `secondary`, another. */
static void addr_msg(struct nl_build *b, const struct netdev *nd, uint32_t ip, uint32_t mask,
                     int secondary)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *ifa = msg_start(b, msg, RTM_NEWADDR, NLM_F_MULTI, 8);

    ifa[0] = AF_INET;
    ifa[1] = (uint8_t)prefix_of(mask);
    ifa[2] = IFA_F_PERMANENT | (secondary ? IFA_F_SECONDARY : 0u);
    ifa[3] = RT_SCOPE_UNIVERSE;
    put32(ifa + 4, (uint32_t)nd->index);
    msg_attr_ip(msg, IFA_ADDRESS, ip);
    msg_attr_ip(msg, IFA_LOCAL, ip);
    msg_attr_ip(msg, IFA_BROADCAST, ip | ~mask);
    msg_attr(msg, IFA_LABEL, nd->name, (uint16_t)(k_strlen(nd->name) + 1));
    nl_append(b, msg);
}

/* Every address of `nd`, its first then the others. */
static void addr_msgs(struct nl_build *b, const struct netdev *nd)
{
    int i;

    if (nd->ip) {
        addr_msg(b, nd, nd->ip, nd->mask, 0);
    }
    for (i = 0; i < nd->naddr2; i++) {
        addr_msg(b, nd, nd->ip2[i], nd->mask2[i], 1);
    }
}

/* The route `r` of the table: a subnet an address implies, reached on the
 * link, an interface's gateway, or a route a program added. */
static void route_msg(struct nl_build *b, const struct netdev_rt *r)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *rtm = msg_start(b, msg, RTM_NEWROUTE, NLM_F_MULTI, 12);

    rtm[0] = AF_INET;
    rtm[1] = (uint8_t)prefix_of(r->mask);
    rtm[4] = RT_TABLE_MAIN;
    rtm[5] = r->kind == NETDEV_RT_LINK      ? RTPROT_KERNEL
             : r->kind == NETDEV_RT_DEFAULT ? RTPROT_BOOT
                                            : RTPROT_STATIC;
    rtm[6] = r->gw ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
    rtm[7] = RTN_UNICAST;
    msg_attr32(msg, RTA_TABLE, RT_TABLE_MAIN);
    if (r->mask != 0) {
        msg_attr_ip(msg, RTA_DST, r->dst);
    }
    if (r->gw) {
        msg_attr_ip(msg, RTA_GATEWAY, r->gw);
    }
    if (r->kind == NETDEV_RT_LINK) {
        msg_attr_ip(msg, RTA_PREFSRC, r->src);
    }
    if (r->metric != 0) {
        msg_attr32(msg, RTA_PRIORITY, r->metric);
    }
    msg_attr32(msg, RTA_OIF, (uint32_t)r->ifindex);
    nl_append(b, msg);
}

/* The destination of an RTM_GETROUTE request, its RTA_DST, or 0 when it
 * names none. */
static uint32_t route_asked(const uint8_t *req, uint32_t body)
{
    const uint8_t *attr = req + NLMSG_HDRLEN + 12;
    const uint8_t *end = req + NLMSG_HDRLEN + body;

    while (body >= 12 && attr + 4 <= end) {
        uint16_t len = get16(attr);

        if (len < 4 || attr + len > end) {
            break;
        }
        if (get16(attr + 2) == RTA_DST && len >= 8) {
            return ((uint32_t)attr[4] << 24) | ((uint32_t)attr[5] << 16) |
                   ((uint32_t)attr[6] << 8) | attr[7];
        }
        attr += (len + 3u) & ~3u;
    }
    return 0;
}

/* The answer to `ip route get`: the route `dst` takes, as Linux reports a
 * route it looked up (RTM_F_CLONED), through the interface and next hop
 * `netdev_route` chooses, or `dst` itself as a local route on the interface
 * that holds it; -ENETUNREACH when nothing reaches it. */
static int route_get_msg(struct nl_build *b, uint32_t dst)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *rtm;
    const struct netdev *nd;
    uint32_t via = 0;
    int i, local = 0;

    nd = netdev_route(dst, &via);
    for (i = 0; i < netdev_seen_count(); i++) {
        const struct netdev *own = netdev_at(i);

        if (own && netdev_holds(own, dst)) {
            nd = own;
            local = 1;
        }
    }
    if (!nd) {
        return -ENETUNREACH;
    }
    rtm = msg_start(b, msg, RTM_NEWROUTE, 0, 12);
    rtm[0] = AF_INET;
    rtm[1] = 32;
    rtm[4] = local ? RT_TABLE_LOCAL : RT_TABLE_MAIN;
    rtm[6] = local ? RT_SCOPE_HOST : RT_SCOPE_UNIVERSE;
    rtm[7] = local ? RTN_LOCAL : RTN_UNICAST;
    put32(rtm + 8, RTM_F_CLONED);
    msg_attr32(msg, RTA_TABLE, local ? RT_TABLE_LOCAL : RT_TABLE_MAIN);
    msg_attr_ip(msg, RTA_DST, dst);
    msg_attr32(msg, RTA_OIF, (uint32_t)nd->index);
    msg_attr_ip(msg, RTA_PREFSRC, local ? dst : netdev_source(nd, via));
    if (!local && via != dst) {
        msg_attr_ip(msg, RTA_GATEWAY, via);
    }
    nl_append(b, msg);
    return 0;
}

/* The three rules every Linux machine starts with, which are what route
 * lookups here amount to: the local addresses, then the main table, then
 * the empty default table. */
static void rule_dump(struct nl_build *b)
{
    static const uint32_t rules[3][2] = {
        {0, RT_TABLE_LOCAL}, {32766, RT_TABLE_MAIN}, {32767, RT_TABLE_DEFAULT}};
    int i;

    for (i = 0; i < 3; i++) {
        uint8_t msg[NL_MSG_MAX];
        uint8_t *frh = msg_start(b, msg, RTM_NEWRULE, NLM_F_MULTI, 12);

        frh[0] = AF_INET;
        frh[4] = (uint8_t)rules[i][1];
        frh[7] = FR_ACT_TO_TBL;
        msg_attr32(msg, FRA_TABLE, rules[i][1]);
        if (rules[i][0] != 0) {
            msg_attr32(msg, FRA_PRIORITY, rules[i][0]);
        }
        nl_append(b, msg);
    }
}

/* A queueing discipline for each interface: `noqueue`, since a frame here
 * goes to the card when it is sent, with no queue in front of it. */
static void qdisc_dump(struct nl_build *b)
{
    int i;

    for (i = 0; i < netdev_seen_count(); i++) {
        const struct netdev *nd = netdev_at(i);
        uint8_t msg[NL_MSG_MAX];
        uint8_t *tcm;

        if (!nd) {
            continue;
        }
        tcm = msg_start(b, msg, RTM_NEWQDISC, NLM_F_MULTI, 20);
        put32(tcm + 4, (uint32_t)nd->index);
        put32(tcm + 12, TC_H_ROOT);
        put32(tcm + 16, 2);
        msg_attr(msg, TCA_KIND, "noqueue", 8);
        nl_append(b, msg);
    }
}

/*
 * The changes a privileged caller asks for, which `ip` sends, done through
 * the operations the interface ioctls use (netif.c, netconf.c): each answers
 * 0 or a negative errno, which `answer` acknowledges. What an interface here
 * cannot hold is refused with EOPNOTSUPP rather than accepted and dropped:
 * an MTU other than 1500 and a rename.
 */

/* The payload of attribute `type` in the request `req` of `body` bytes past
 * its header, whose fixed part is `fixed` bytes, with its length in `*len`;
 * NULL when it has none. */
static const uint8_t *req_attr(const uint8_t *req, uint32_t body, uint32_t fixed, uint16_t type,
                               uint16_t *len)
{
    const uint8_t *attr = req + NLMSG_HDRLEN + fixed;
    const uint8_t *end = req + NLMSG_HDRLEN + body;

    while (body >= fixed && attr + 4 <= end) {
        uint16_t alen = get16(attr);

        if (alen < 4 || attr + alen > end) {
            break;
        }
        if (get16(attr + 2) == type) {
            *len = (uint16_t)(alen - 4u);
            return attr + 4;
        }
        attr += (alen + 3u) & ~3u;
    }
    return 0;
}

/* An IPv4 attribute's address in the host's order, or 0 for none. */
static uint32_t req_ip(const uint8_t *req, uint32_t body, uint32_t fixed, uint16_t type)
{
    uint16_t len;
    const uint8_t *a = req_attr(req, body, fixed, type, &len);

    return a && len == 4
               ? ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3]
               : 0;
}

/* The interface of index `index`, or NULL. */
static struct netdev *dev_of_index(uint32_t index)
{
    int i, n = netdev_seen_count();

    for (i = 0; i < n; i++) {
        struct netdev *nd = netdev_at(i);

        if (nd && (uint32_t)nd->index == index) {
            return nd;
        }
    }
    return 0;
}

/* RTM_NEWLINK and RTM_SETLINK: up or down by IFF_UP in the change mask, and
 * an MTU or a name only as they are. */
static int32_t link_change(const uint8_t *req, uint32_t body)
{
    struct netdev *nd = body >= 16 ? link_asked(req, body) : 0;
    uint16_t len;
    const uint8_t *a;

    if (!nd) {
        return -ENODEV;
    }
    a = req_attr(req, body, 16, IFLA_MTU, &len);
    if (a && (len != 4 || get32(a) != 1500)) {
        return -EOPNOTSUPP;
    }
    a = req_attr(req, body, 16, IFLA_IFNAME, &len);
    if (a && get32(req + NLMSG_HDRLEN + 4) != 0 &&
        !same_name(nd->name, sizeof(nd->name), (const char *)a, len)) {
        return -EOPNOTSUPP;
    }
    if (get32(req + NLMSG_HDRLEN + 12) & IFF_UP) {
        nd->up = (get32(req + NLMSG_HDRLEN + 8) & IFF_UP) ? 1 : 0;
    }
    return 0;
}

/* RTM_NEWADDR and RTM_DELADDR: an address of an interface and its prefix,
 * its first or another. */
static int32_t addr_change(const uint8_t *req, uint16_t type, uint16_t flags, uint32_t body)
{
    struct netdev *nd = body >= 8 ? dev_of_index(get32(req + NLMSG_HDRLEN + 4)) : 0;
    uint32_t prefix = body >= 8 ? req[NLMSG_HDRLEN + 1] : 0;
    uint32_t ip = req_ip(req, body, 8, IFA_LOCAL);
    uint32_t mask = prefix == 0 || prefix > 32 ? 0 : 0xFFFFFFFFu << (32 - prefix);

    if (ip == 0) {
        ip = req_ip(req, body, 8, IFA_ADDRESS);
    }
    if (!nd) {
        return -ENODEV;
    }
    if (type == RTM_DELADDR) {
        return netconf_del_address(nd, ip);
    }
    if (ip == 0) {
        return -EINVAL;
    }
    if (netdev_holds(nd, ip)) {
        return (flags & NLM_F_EXCL) ? -EEXIST : 0;
    }
    return netconf_add_address(nd, ip, mask);
}

/* The interface a route request is for: the one RTA_OIF names, or, with
 * none named, the one whose subnet has the gateway `gw`, or the first. */
static struct netdev *route_dev(const uint8_t *req, uint32_t body, uint32_t gw)
{
    uint16_t len;
    const uint8_t *oif = req_attr(req, body, 12, RTA_OIF, &len);
    int i;

    if (oif) {
        return len == 4 ? dev_of_index(get32(oif)) : 0;
    }
    for (i = 0; i < netdev_seen_count(); i++) {
        struct netdev *at = netdev_at(i);

        if (at && netdev_on_link(at, gw)) {
            return at;
        }
    }
    return netdev_first();
}

/* The default route of `nd`, which is its gateway: set to `gw`, which a
 * route already there refuses with EEXIST unless NLM_F_REPLACE asks, or
 * cleared, which a route not there refuses with ESRCH. */
static int32_t default_route_change(struct netdev *nd, uint16_t type, uint16_t flags, uint32_t gw)
{
    if (type == RTM_DELROUTE) {
        return nd->gateway == 0 || (gw != 0 && gw != nd->gateway) ? -ESRCH
                                                                  : netconf_set_gateway(nd, 0);
    }
    if (gw == 0) {
        return -EINVAL;
    }
    if (nd->gateway != 0 && !(flags & NLM_F_REPLACE)) {
        return nd->gateway == gw && !(flags & NLM_F_EXCL) ? 0 : -EEXIST;
    }
    return netconf_set_gateway(nd, gw);
}

/* A route as a request names it. */
struct route_req {
    uint32_t dst, mask, gw, metric;
};

/* A route `r` a program adds to the table by `nd`, or removes from it. */
static int32_t added_route_change(struct netdev *nd, uint16_t type, uint16_t flags,
                                  const struct route_req *r)
{
    uint32_t net = net_lock();
    int32_t rc = type == RTM_NEWROUTE ? netdev_route_add(nd, r->dst, r->mask, r->gw, r->metric,
                                                         (flags & NLM_F_REPLACE) != 0)
                                      : netdev_route_del(nd, r->dst, r->mask, r->gw);

    net_unlock(net);
    return rc;
}

/* RTM_NEWROUTE and RTM_DELROUTE: the default route of an interface, which is
 * its gateway, the route a subnet implies, which is there while its address
 * is, and any other route, which goes in the table. */
static int32_t route_change(const uint8_t *req, uint16_t type, uint16_t flags, uint32_t body)
{
    struct route_req r;
    struct netdev *nd;
    uint32_t dst_len = body >= 12 ? req[NLMSG_HDRLEN + 1] : 0;
    uint16_t len;
    const uint8_t *prio = req_attr(req, body, 12, RTA_PRIORITY, &len);

    r.gw = req_ip(req, body, 12, RTA_GATEWAY);
    r.dst = req_ip(req, body, 12, RTA_DST);
    r.metric = prio && len == 4 ? get32(prio) : 0;
    r.mask = dst_len == 0 || dst_len > 32 ? 0 : 0xFFFFFFFFu << (32 - dst_len);
    nd = body >= 12 ? route_dev(req, body, r.gw) : 0;
    if (!nd) {
        return -ENODEV;
    }
    if (dst_len > 32) {
        return -EINVAL;
    }
    if (dst_len == 0 && r.metric == 0 &&
        (type == RTM_NEWROUTE ? nd->gateway == 0 || (flags & NLM_F_REPLACE) || nd->gateway == r.gw
                              : nd->gateway != 0)) {
        return default_route_change(nd, type, flags, r.gw);
    }
    if (type == RTM_NEWROUTE && r.gw == 0 && nd->mask != 0 && dst_len == prefix_of(nd->mask) &&
        r.dst == (nd->ip & nd->mask)) {
        return (flags & NLM_F_EXCL) ? -EEXIST : 0;
    }
    return added_route_change(nd, type, flags, &r);
}

/* RTM_DELNEIGH: forget a mapping, which `ip neigh flush` asks for each;
 * RTM_NEWNEIGH: store one by hand, permanent when its state says so, which
 * `ip neigh add ... nud permanent` asks. */
static int32_t neigh_change(const uint8_t *req, uint16_t type, uint32_t body)
{
    uint32_t ip = req_ip(req, body, 12, NDA_DST);
    uint32_t index = body >= 12 ? get32(req + NLMSG_HDRLEN + 4) : 0;
    struct netdev *nd = index != 0 ? dev_of_index(index) : 0;
    uint16_t len;
    const uint8_t *mac = req_attr(req, body, 12, NDA_LLADDR, &len);

    if (ip == 0 || (index != 0 && !nd)) {
        return -EINVAL;
    }
    if (type == RTM_DELNEIGH) {
        return arp_forget(nd, ip);
    }
    if (!nd || !mac || len != 6) {
        return -EINVAL;
    }
    return arp_set(nd, ip, mac, (get16(req + NLMSG_HDRLEN + 8) & NUD_PERMANENT) != 0);
}

/* Whether `type` asks for a change rather than a reading. */
static int is_change(uint16_t type)
{
    return type == RTM_NEWLINK || type == RTM_SETLINK || type == RTM_NEWADDR ||
           type == RTM_DELADDR || type == RTM_NEWROUTE || type == RTM_DELROUTE ||
           type == RTM_NEWNEIGH || type == RTM_DELNEIGH;
}

/* The answer to the change `req` asks for: EPERM for a caller without
 * privilege, else its handler's. */
static int32_t change(const uint8_t *req, uint16_t type, uint16_t flags, uint32_t body)
{
    if (!current_proc || !proc_privileged(current_proc)) {
        return -EPERM;
    }
    if (type == RTM_NEWLINK || type == RTM_SETLINK) {
        return link_change(req, body);
    }
    if (type == RTM_NEWADDR || type == RTM_DELADDR) {
        return addr_change(req, type, flags, body);
    }
    if (type == RTM_NEWROUTE || type == RTM_DELROUTE) {
        return route_change(req, type, flags, body);
    }
    if (type == RTM_NEWNEIGH || type == RTM_DELNEIGH) {
        return neigh_change(req, type, body);
    }
    return -EOPNOTSUPP;
}

/* The message that ends a dump, or an NLMSG_ERROR of `error` answering the
 * request whose header is `req`, 0 being an acknowledgement. */
static void end_msg(struct nl_build *b, const uint8_t *req, int32_t error)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *body;

    if (!req) {
        body = msg_start(b, msg, NLMSG_DONE, NLM_F_MULTI, 4);
        put32(body, 0);
    } else {
        body = msg_start(b, msg, NLMSG_ERROR, 0, 4 + NLMSG_HDRLEN);
        put32(body, (uint32_t)error);
        k_memcpy(body + 4, req, NLMSG_HDRLEN);
    }
    nl_append(b, msg);
}

/* Make the change `req` asks for and acknowledge it: with its error, or
 * with 0 when NLM_F_ACK asks for an acknowledgement. */
static void change_answer(struct nl_build *b, const uint8_t *req, uint16_t type, uint16_t flags,
                          uint32_t body)
{
    int32_t rc = change(req, type, flags, body);

    if (rc != 0 || (flags & NLM_F_ACK)) {
        end_msg(b, req, rc);
    }
}

/* Answer the request of `type` and `flags`, whose header is `req` and whose
 * body of `body` bytes follows it. */
static void answer(struct nl_build *b, const uint8_t *req, uint16_t type, uint16_t flags,
                   uint32_t body)
{
    int i, n = netdev_seen_count();

    if (!net_ns_seen()) {
        /* A network namespace with no interface: a change or a question
         * about one finds none, and a dump is empty. */
        if (is_change(type) || (flags & NLM_F_DUMP) != NLM_F_DUMP) {
            end_msg(b, req, type == RTM_GETROUTE ? -ENETUNREACH : -ENODEV);
        } else {
            end_msg(b, 0, 0);
        }
        return;
    }
    if (is_change(type)) {
        change_answer(b, req, type, flags, body);
        return;
    }
    if (type == RTM_GETLINK && (flags & NLM_F_DUMP) != NLM_F_DUMP && body >= 16) {
        const struct netdev *nd = link_asked(req, body);

        if (nd) {
            link_msg(b, nd, 0);
        } else {
            end_msg(b, req, -ENODEV);
        }
        return;
    }
    if (type == RTM_GETROUTE && (flags & NLM_F_DUMP) != NLM_F_DUMP) {
        int rc = route_get_msg(b, route_asked(req, body));

        if (rc != 0) {
            end_msg(b, req, rc);
        }
        return;
    }
    if ((flags & NLM_F_DUMP) == NLM_F_DUMP && (type == RTM_GETRULE || type == RTM_GETQDISC)) {
        if (type == RTM_GETRULE) {
            rule_dump(b);
        } else {
            qdisc_dump(b);
        }
        end_msg(b, 0, 0);
        return;
    }
    if ((flags & NLM_F_DUMP) != NLM_F_DUMP || (type != RTM_GETLINK && type != RTM_GETADDR &&
                                               type != RTM_GETROUTE && type != RTM_GETNEIGH)) {
        end_msg(b, req, -EOPNOTSUPP);
        return;
    }
    if (type == RTM_GETNEIGH) {
        neigh_dump(b, req, body);
        end_msg(b, 0, 0);
        return;
    }
    if (type == RTM_GETROUTE) {
        struct netdev_rt r;

        for (i = 0; netdev_route_at(i, &r); i++) {
            route_msg(b, &r);
        }
        end_msg(b, 0, 0);
        return;
    }
    for (i = 0; i < n; i++) {
        const struct netdev *nd = netdev_at(i);

        if (!nd) {
            continue;
        }
        if (type == RTM_GETLINK) {
            link_msg(b, nd, NLM_F_MULTI);
        } else {
            addr_msgs(b, nd);
        }
    }
    end_msg(b, 0, 0);
}

/* An AF_INET socket as `inet_diag_msg_of` writes it, in the host's order:
 * with a timer, its kind and the milliseconds left. */
struct diag_inet {
    uint8_t state;
    uint8_t retrans;
    uint8_t timer;
    uint16_t sport, dport;
    uint32_t src, dst;
    uint32_t expires;
    uint32_t rqueue, wqueue, uid, inode;
};

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    put_be16(p, (uint16_t)(v >> 16));
    put_be16(p + 2, (uint16_t)v);
}

/* Write `d` into `msg` as an inet_diag_msg, the ports and addresses in
 * network order and the cookie its inode; with `protocol` not -1, a raw
 * socket's, the INET_DIAG_PROTOCOL attribute too. Returns `msg`, for the
 * caller to add attributes to and append. */
static uint8_t *inet_diag_msg_of(const struct nl_build *b, uint8_t *msg, const struct diag_inet *d,
                                 int protocol)
{
    uint8_t *body = msg_start(b, msg, SOCK_DIAG_BY_FAMILY, NLM_F_MULTI, DIAG_INET_MSG_LEN);
    uint8_t proto = (uint8_t)protocol;

    body[0] = AF_INET;
    body[1] = d->state;
    body[2] = d->timer;
    body[3] = d->retrans;
    put_be16(body + 4, d->sport);
    put_be16(body + 6, d->dport);
    put_be32(body + 8, d->src);
    put_be32(body + 24, d->dst);
    put32(body + 44, d->inode);
    put32(body + 52, d->expires);
    put32(body + 56, d->rqueue);
    put32(body + 60, d->wqueue);
    put32(body + 64, d->uid);
    put32(body + 68, d->inode);
    if (protocol >= 0) {
        msg_attr(msg, INET_DIAG_PROTOCOL, &proto, 1);
    }
    return msg;
}

/*
 * The first TCP_INFO_LEN bytes of Linux's `struct tcp_info` for `r`, which
 * `ss -i` prints: the state, the loss recovery, the timeouts in a row, the
 * options and window shifts, the retransmission timeout and the round trip
 * in microseconds, the segment size, the segments in flight, the window and
 * threshold in segments, and the segments sent again. The timeout counts in
 * ticks of 10 ms; the round trip and its variance are TCP's own samples
 * timed to the microsecond (`tcp_rtt_us`).
 */
static void tcp_info_of(const struct tcp_row *r, uint8_t *info)
{
    uint32_t mss = r->snd_mss ? r->snd_mss : TCP_MSS;

    k_memset(info, 0, TCP_INFO_LEN);
    info[0] = (uint8_t)tcp_state_to_linux(r->state);
    info[1] = r->recovering == TCP_RECOVER_FAST ? 3 : r->recovering == TCP_RECOVER_RTO ? 4 : 0;
    info[2] = r->rto_backoff;
    info[4] = r->rto_backoff;
    info[5] = (uint8_t)((r->sack_ok ? TCPI_OPT_SACK : 0) |
                        (r->snd_wscale || r->rcv_wscale ? TCPI_OPT_WSCALE : 0));
    info[6] = (uint8_t)((r->snd_wscale & 15u) | (r->rcv_wscale & 15u) << 4);
    put32(info + 8, r->rto * DIAG_TICK_US);
    put32(info + 16, mss);
    put32(info + 24, (r->in_flight + mss - 1) / mss);
    put32(info + 60, mss + 40);
    put32(info + 68, r->srtt_us >> 3);
    put32(info + 72, r->rttvar_us >> 2);
    put32(info + 76, r->ssthresh / mss);
    put32(info + 80, r->cwnd / mss);
    put32(info + 84, TCP_MSS);
    put32(info + 100, r->retransmits);
}

/* The timer of `r` that `ss -o` shows, into `d`: TIME_WAIT's end, a
 * retransmission or a zero-window probe, with the milliseconds left. */
static void tcp_timer_of(const struct tcp_row *r, struct diag_inet *d)
{
    d->timer = 0;
    d->expires = 0;
    if (r->state == TCP_TIME_WAIT && r->timewait_left) {
        d->timer = DIAG_TIMER_TIMEWAIT;
        d->expires = r->timewait_left * (DIAG_TICK_US / 1000);
    } else if (r->rto_timer) {
        d->timer = DIAG_TIMER_RETRANS;
        d->expires = r->rto_timer * (DIAG_TICK_US / 1000);
    } else if (r->zwp_timer) {
        d->timer = DIAG_TIMER_PROBE;
        d->expires = r->zwp_timer * (DIAG_TICK_US / 1000);
    }
}

/* The TCP connections whose states are in the mask `wanted`, with the
 * TCP information and the congestion control's name when the mask `ext` of
 * extensions asks for them; 0, or -ENOMEM when they could not be copied. */
static int diag_tcp(struct nl_build *b, uint32_t wanted, uint8_t ext)
{
    struct tcp_row *rows;
    struct diag_inet d;
    uint8_t msg[NL_MSG_MAX];
    uint8_t info[TCP_INFO_LEN];
    int i, n = tcp_snapshot(&rows);

    for (i = 0; i < n; i++) {
        d.state = (uint8_t)tcp_state_to_linux(rows[i].state);
        if (!(wanted & (1u << d.state))) {
            continue;
        }
        d.retrans = (uint8_t)(rows[i].retransmits > 255 ? 255 : rows[i].retransmits);
        d.sport = rows[i].local_port;
        d.dport = rows[i].remote_port;
        d.src = rows[i].local_ip;
        d.dst = rows[i].remote_ip;
        d.rqueue = rows[i].rxq;
        d.wqueue = d.state == DIAG_LISTEN ? rows[i].backlog : rows[i].txq;
        d.uid = rows[i].uid;
        d.inode = rows[i].inode;
        tcp_timer_of(&rows[i], &d);
        inet_diag_msg_of(b, msg, &d, -1);
        if (ext & (1u << (INET_DIAG_INFO - 1))) {
            tcp_info_of(&rows[i], info);
            msg_attr(msg, INET_DIAG_INFO, info, TCP_INFO_LEN);
        }
        if (ext & (1u << (INET_DIAG_CONG - 1))) {
            msg_attr(msg, INET_DIAG_CONG, "reno", 5);
        }
        nl_append(b, msg);
    }
    if (n < 0) {
        return -ENOMEM;
    }
    kfree(rows);
    return 0;
}

/* The UDP sockets, or with `type` SOCK_RAW the raw ones, whose states are
 * in the mask `wanted`: 1 for a connected socket and 7 for the rest; 0, or
 * -ENOMEM. */
static int diag_dgram(struct nl_build *b, int type, uint32_t wanted)
{
    struct socket_row *rows;
    struct diag_inet d;
    uint8_t msg[NL_MSG_MAX];
    int i, n = socket_snapshot(type, &rows);

    k_memset(&d, 0, sizeof(d));
    for (i = 0; i < n; i++) {
        d.state = rows[i].connected ? DIAG_ESTABLISHED : DIAG_CLOSE;
        if (!(wanted & (1u << d.state))) {
            continue;
        }
        d.sport = rows[i].local_port;
        if (type == SOCK_RAW && d.sport == 0) {
            d.sport = IPPROTO_RAW; /* the ioctl descriptor is opened with it */
        }
        d.src = rows[i].local_ip;
        d.dport = rows[i].connected ? rows[i].remote_port : 0;
        d.dst = rows[i].connected ? rows[i].remote_ip : 0;
        d.rqueue = rows[i].rxq;
        d.uid = rows[i].uid;
        d.inode = rows[i].inode;
        nl_append(b, inet_diag_msg_of(b, msg, &d, type == SOCK_RAW ? d.sport : -1));
    }
    if (n < 0) {
        return -ENOMEM;
    }
    kfree(rows);
    return 0;
}

/* The AF_UNIX sockets whose states are in the mask `wanted`, with the
 * attributes the mask `show` asks for; 0, or -ENOMEM. */
static int diag_unix(struct nl_build *b, uint32_t wanted, uint32_t show)
{
    struct unix_row *rows;
    uint8_t msg[NL_MSG_MAX];
    uint8_t queues[8];
    int i, n = unix_snapshot(&rows);

    for (i = 0; i < n; i++) {
        const struct unix_row *r = &rows[i];
        uint8_t state = r->listening ? DIAG_LISTEN : r->connected ? DIAG_ESTABLISHED : DIAG_CLOSE;
        uint8_t *body;

        if (!(wanted & (1u << state))) {
            continue;
        }
        body = msg_start(b, msg, SOCK_DIAG_BY_FAMILY, NLM_F_MULTI, DIAG_UNIX_MSG_LEN);
        body[0] = AF_UNIX;
        body[1] = (uint8_t)r->type;
        body[2] = state;
        put32(body + 4, r->inode);
        put32(body + 8, r->inode);
        if ((show & UDIAG_SHOW_NAME) && r->name_len > 0) {
            /* A path is given with its NUL, as Linux keeps it; an abstract
             * name has none. */
            msg_attr(msg, UNIX_DIAG_NAME, r->path,
                     (uint16_t)(r->path[0] ? r->name_len + 1 : r->name_len));
        }
        if ((show & UDIAG_SHOW_PEER) && r->peer != 0) {
            msg_attr32(msg, UNIX_DIAG_PEER, r->peer);
        }
        if (show & UDIAG_SHOW_RQLEN) {
            put32(queues, r->rqueue);
            put32(queues + 4, r->wqueue);
            msg_attr(msg, UNIX_DIAG_RQLEN, queues, 8);
        }
        if (show & UDIAG_SHOW_UID) {
            msg_attr32(msg, UNIX_DIAG_UID, r->uid);
        }
        nl_append(b, msg);
    }
    if (n < 0) {
        return -ENOMEM;
    }
    kfree(rows);
    return 0;
}

/* The protocol of an AF_INET request `r` of `body` bytes: the byte in the
 * request, or the INET_DIAG_REQ_PROTOCOL attribute after it, which a
 * protocol numbered past 255 is asked by. */
static uint32_t diag_protocol(const uint8_t *r, uint32_t body)
{
    uint32_t at = DIAG_INET_REQ_LEN;

    while (at + 4 <= body) {
        uint16_t alen = get16(r + at);

        if (alen < 4 || at + alen > body) {
            break;
        }
        if (get16(r + at + 2) == INET_DIAG_REQ_PROTOCOL && alen >= 8) {
            return get32(r + at + 4);
        }
        at += (alen + 3u) & ~3u;
    }
    return r[1];
}

/* Answer a NETLINK_SOCK_DIAG request of `type` and `flags`, whose header
 * is `req` and whose body of `body` bytes follows it: the dump and its end,
 * or ENOENT for a family or protocol with no sockets here, as Linux answers
 * one no module handles, or ENOMEM when the sockets could not be copied. */
static void diag_answer(struct nl_build *b, const uint8_t *req, uint16_t type, uint16_t flags,
                        uint32_t body)
{
    const uint8_t *r = req + NLMSG_HDRLEN;
    uint32_t protocol;
    int rc = -ENOENT;

    if (type != SOCK_DIAG_BY_FAMILY || body < 1) {
        end_msg(b, req, -EINVAL);
        return;
    }
    if (!net_ns_seen()) {
        end_msg(b, 0, 0); /* its sockets are in no table */
        return;
    }
    if ((flags & NLM_F_DUMP) != NLM_F_DUMP) {
        end_msg(b, req, -EOPNOTSUPP);
        return;
    }
    if (r[0] == AF_INET && body >= DIAG_INET_REQ_LEN) {
        protocol = diag_protocol(r, body);
        if (protocol == IP_PROTO_TCP) {
            rc = diag_tcp(b, get32(r + 4), r[2]);
        } else if (protocol == IP_PROTO_UDP) {
            rc = diag_dgram(b, SOCK_DGRAM, get32(r + 4));
        } else if (protocol == IPPROTO_RAW) {
            rc = diag_dgram(b, SOCK_RAW, get32(r + 4));
        }
    } else if (r[0] == AF_UNIX && body >= DIAG_UNIX_REQ_LEN) {
        rc = diag_unix(b, get32(r + 4), get32(r + 12));
    } else if (r[0] == AF_INET6) {
        rc = 0;
    }
    end_msg(b, rc != 0 ? req : 0, rc);
}

int32_t netlink_send(struct socket *sock, const uint8_t *buf, uint32_t len)
{
    struct nl_build *b;
    uint32_t at = 0;
    struct netlink_state *st = state_of(sock);
    int failed;

    if (!st->bound) {
        (void)netlink_bind(sock, 0, 0);
    }
    if (st->protocol == NETLINK_KOBJECT_UEVENT) {
        return (int32_t)len;
    }
    b = kmalloc(sizeof(*b));
    if (!b) {
        return -ENOMEM;
    }
    b->sock = sock;
    b->len = 0;
    b->failed = 0;
    b->pid = st->pid;
    while (at + NLMSG_HDRLEN <= len) {
        uint32_t mlen = get32(buf + at);

        if (mlen < NLMSG_HDRLEN || mlen > len - at) {
            break;
        }
        b->seq = get32(buf + at + 8);
        if ((get16(buf + at + 6) & NLM_F_REQUEST) && st->protocol == NETLINK_SOCK_DIAG) {
            diag_answer(b, buf + at, get16(buf + at + 4), get16(buf + at + 6), mlen - NLMSG_HDRLEN);
        } else if (get16(buf + at + 6) & NLM_F_REQUEST) {
            answer(b, buf + at, get16(buf + at + 4), get16(buf + at + 6), mlen - NLMSG_HDRLEN);
        }
        at += (mlen + 3u) & ~3u;
    }
    nl_flush(b);
    failed = b->failed;
    kfree(b);
    return failed ? -ENOMEM : (int32_t)len;
}

int netlink_pending(struct socket *sock)
{
    struct netlink_state *st = state_of(sock);

    return st && st->head ? (int)st->head->len : -1;
}

int netlink_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len)
{
    struct netlink_state *st = state_of(sock);
    uint32_t n;

    if (!st || !st->head) {
        return -1;
    }
    if (offset >= st->head->len || len <= 0) {
        return 0;
    }
    n = st->head->len - offset;
    if (n > (uint32_t)len) {
        n = (uint32_t)len;
    }
    k_memcpy(buf, (const uint8_t *)(st->head + 1) + offset, n);
    return (int)n;
}

void netlink_consume(struct socket *sock)
{
    struct netlink_state *st = state_of(sock);
    struct nl_dgram *d;

    if (!st || !st->head) {
        return;
    }
    d = st->head;
    st->head = d->next;
    if (!st->head) {
        st->tail = 0;
    }
    kfree(d);
}

int netlink_ready(struct socket *sock)
{
    struct netlink_state *st = state_of(sock);

    return st && st->head != 0;
}
