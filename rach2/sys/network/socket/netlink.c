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

/* Attributes of a link, an address and a route. */
#define IFLA_ADDRESS   1
#define IFLA_BROADCAST 2
#define IFLA_IFNAME    3
#define IFLA_MTU       4
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

#define ARPHRD_ETHER       1
#define IFF_LOWER_UP       0x10000u
#define IF_OPER_UP         6
#define IF_OPER_DOWN       2
#define IFA_F_PERMANENT    0x80u
#define RT_SCOPE_UNIVERSE  0
#define RT_SCOPE_LINK      253
#define RT_TABLE_MAIN      254
#define RTPROT_KERNEL      2
#define RTPROT_BOOT        3
#define RTN_UNICAST        1

/* Policy: an answer datagram is at most a page, which every reader's buffer
 * holds: Chromium reads with 4096 bytes and BusyBox with 8192. A message is
 * at most 128 bytes. */
#define NL_DGRAM_MAX 4096u
#define NL_MSG_MAX   128u

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

static void link_msg(struct nl_build *b, const struct netdev *nd, uint16_t flags)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *ifi = msg_start(b, msg, RTM_NEWLINK, flags, 16);
    static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t up = netif_is_up() ? IFF_UP | IFF_RUNNING | IFF_LOWER_UP : 0;
    uint8_t oper = up ? IF_OPER_UP : IF_OPER_DOWN;

    put16(ifi + 2, ARPHRD_ETHER);
    put32(ifi + 4, (uint32_t)nd->index);
    put32(ifi + 8, up | IFF_BROADCAST | IFF_MULTICAST);
    msg_attr(msg, IFLA_IFNAME, nd->name, (uint16_t)(k_strlen(nd->name) + 1));
    msg_attr(msg, IFLA_ADDRESS, nd->mac, 6);
    msg_attr(msg, IFLA_BROADCAST, broadcast, 6);
    msg_attr32(msg, IFLA_MTU, 1500);
    msg_attr(msg, IFLA_OPERSTATE, &oper, 1);
    nl_append(b, msg);
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
    uint32_t index;

    if (type == RTM_GETLINK && (flags & NLM_F_DUMP) != NLM_F_DUMP && body >= 16) {
        index = get32(req + NLMSG_HDRLEN + 4);
        for (i = 0; i < n; i++) {
            const struct netdev *nd = netdev_at(i);

            if (nd && (uint32_t)nd->index == index) {
                link_msg(b, nd, 0);
                return;
            }
        }
        end_msg(b, req, -ENODEV);
        return;
    }
    if ((flags & NLM_F_DUMP) != NLM_F_DUMP ||
        (type != RTM_GETLINK && type != RTM_GETADDR && type != RTM_GETROUTE)) {
        end_msg(b, req, -EOPNOTSUPP);
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
            kfree(b);
            return -EINVAL;
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
