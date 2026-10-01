/*
 * netlink.c: NETLINK_ROUTE sockets. The contract is in netlink.h; what is
 * here is the reading of requests and the building of the answers from the
 * interface records.
 */

#include "netlink.h"
#include "socket.h"
#include "../net.h"
#include "../netif.h"
#include "../driver/netdev.h"
#include "../arp/arp.h"
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
#define NLM_F_DUMP    0x300u
#define RTM_NEWLINK   16
#define RTM_GETLINK   18
#define RTM_NEWADDR   20
#define RTM_GETADDR   22
#define RTM_NEWROUTE  24
#define RTM_GETROUTE  26
#define RTM_NEWNEIGH  28
#define RTM_GETNEIGH  30

/* Attributes of a link, an address and a route. */
#define IFLA_ADDRESS   1
#define IFLA_BROADCAST 2
#define IFLA_IFNAME    3
#define IFLA_MTU       4
#define IFLA_STATS     7
#define IFLA_OPERSTATE 16
#define IFA_ADDRESS    1
#define IFA_LOCAL      2
#define IFA_LABEL      3
#define IFA_BROADCAST  4
#define RTA_DST        1
#define RTA_OIF        4
#define RTA_GATEWAY    5
#define RTA_PREFSRC    7
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
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK     253
#define RT_TABLE_MAIN     254
#define RTPROT_KERNEL     2
#define RTPROT_BOOT       3
#define RTN_UNICAST       1
#define NUD_REACHABLE     0x02u

/* Policy: an answer datagram is at most a page, which every reader's buffer
 * holds: Chromium reads with 4096 bytes and BusyBox with 8192. A message is
 * at most 256 bytes, a link's with its counters among them. */
#define NL_DGRAM_MAX 4096u
#define NL_MSG_MAX   256u

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

    if (protocol != NETLINK_ROUTE && protocol != NETLINK_KOBJECT_UEVENT) {
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
 * current one has no room. */
static void nl_append(struct nl_build *b, const uint8_t *msg)
{
    uint32_t len = get32(msg);

    if (b->len + len > NL_DGRAM_MAX) {
        nl_flush(b);
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
 * way, and the receive errors. */
static void link_msg(struct nl_build *b, const struct netdev *nd, uint16_t flags)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *ifi = msg_start(b, msg, RTM_NEWLINK, flags, 16);
    static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t up = netif_is_up() ? IFF_UP | IFF_RUNNING | IFF_LOWER_UP : 0;
    uint8_t oper = up ? IF_OPER_UP : IF_OPER_DOWN;
    uint8_t stats[96];

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
    nl_append(b, msg);
}

/* A neighbour, the ARP cache's mapping `row`, on the interface of index
 * `index`. The cache keeps no age, so every mapping is reachable. */
static void neigh_msg(struct nl_build *b, const struct arp_row *row, int index)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *ndm = msg_start(b, msg, RTM_NEWNEIGH, NLM_F_MULTI, 12);

    ndm[0] = AF_INET;
    put32(ndm + 4, (uint32_t)index);
    put16(ndm + 8, NUD_REACHABLE);
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
static const struct netdev *link_asked(const uint8_t *req, uint32_t body)
{
    const uint8_t *attr = req + NLMSG_HDRLEN + 16;
    const uint8_t *end = req + NLMSG_HDRLEN + body;
    uint32_t index = get32(req + NLMSG_HDRLEN + 4);
    const char *name = 0;
    uint32_t name_len = 0;
    int i, n = netdev_count();

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
        const struct netdev *nd = netdev_at(i);

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
    int k, i, m, n = netdev_count();

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

static void addr_msg(struct nl_build *b, const struct netdev *nd)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *ifa = msg_start(b, msg, RTM_NEWADDR, NLM_F_MULTI, 8);

    ifa[0] = AF_INET;
    ifa[1] = (uint8_t)prefix_of(nd->mask);
    ifa[2] = IFA_F_PERMANENT;
    ifa[3] = RT_SCOPE_UNIVERSE;
    put32(ifa + 4, (uint32_t)nd->index);
    msg_attr_ip(msg, IFA_ADDRESS, nd->ip);
    msg_attr_ip(msg, IFA_LOCAL, nd->ip);
    msg_attr_ip(msg, IFA_BROADCAST, nd->ip | ~nd->mask);
    msg_attr(msg, IFA_LABEL, nd->name, (uint16_t)(k_strlen(nd->name) + 1));
    nl_append(b, msg);
}

/* A route of `nd`: the default through its gateway when `gateway`, else its
 * own subnet, reached on the link. */
static void route_msg(struct nl_build *b, const struct netdev *nd, int gateway)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *rtm = msg_start(b, msg, RTM_NEWROUTE, NLM_F_MULTI, 12);

    rtm[0] = AF_INET;
    rtm[1] = gateway ? 0 : (uint8_t)prefix_of(nd->mask);
    rtm[4] = RT_TABLE_MAIN;
    rtm[5] = gateway ? RTPROT_BOOT : RTPROT_KERNEL;
    rtm[6] = gateway ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
    rtm[7] = RTN_UNICAST;
    msg_attr32(msg, RTA_TABLE, RT_TABLE_MAIN);
    if (gateway) {
        msg_attr_ip(msg, RTA_GATEWAY, nd->gateway);
    } else {
        msg_attr_ip(msg, RTA_DST, nd->ip & nd->mask);
        msg_attr_ip(msg, RTA_PREFSRC, nd->ip);
    }
    msg_attr32(msg, RTA_OIF, (uint32_t)nd->index);
    nl_append(b, msg);
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

/* Answer the request of `type` and `flags`, whose header is `req` and whose
 * body of `body` bytes follows it. */
static void answer(struct nl_build *b, const uint8_t *req, uint16_t type, uint16_t flags,
                   uint32_t body)
{
    int i, n = netdev_count();

    if (type == RTM_GETLINK && (flags & NLM_F_DUMP) != NLM_F_DUMP && body >= 16) {
        const struct netdev *nd = link_asked(req, body);

        if (nd) {
            link_msg(b, nd, 0);
        } else {
            end_msg(b, req, -ENODEV);
        }
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
    for (i = 0; i < n; i++) {
        const struct netdev *nd = netdev_at(i);

        if (!nd) {
            continue;
        }
        if (type == RTM_GETLINK) {
            link_msg(b, nd, NLM_F_MULTI);
        } else if (type == RTM_GETADDR && nd->ip) {
            addr_msg(b, nd);
        } else if (type == RTM_GETROUTE && nd->ip) {
            route_msg(b, nd, 0);
            if (nd->gateway) {
                route_msg(b, nd, 1);
            }
        }
    }
    end_msg(b, 0, 0);
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
        if (get16(buf + at + 6) & NLM_F_REQUEST) {
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
