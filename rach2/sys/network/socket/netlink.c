/*
 * netlink.c: AF_NETLINK sockets, as netlink.h describes them: the reading of
 * requests, the changes they ask for, and the answers, built from the
 * interface records, the IPv4 and IPv6 route tables, the ARP and neighbour
 * caches and the sockets.
 */

#include "../../idle_timer.h"
#include "netlink.h"
#include "genetlink.h"
#include "socket.h"
#include "../net.h"
#include "../netif.h"
#include "../netconf.h"
#include "../driver/netdev.h"
#include "../arp/arp.h"
#include "../ip/ip.h"
#include "../ip6/ip6.h"
#include "../ip6/nd6.h"
#include "../ip6/route6.h"
#include "../tcp/tcp.h"
#include "../../ipc/unix.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../proc/process.h"
#include "../../proc/wait_queue.h"
#include "../../timer.h"

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
#define RTM_DELLINK   17
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

/* The configuration of an interface, which `ip netconf` asks for. */
#define RTM_NEWNETCONF 80
#define RTM_GETNETCONF 82

/* A neighbour discovery option the kernel leaves to a program (RFC 8106's
 * RDNSS and DNSSL), the `struct nduseroptmsg` before it, and the attribute
 * that gives the router's address after it. */
#define RTM_NEWNDUSEROPT  68
#define NDUSEROPT_MSG_LEN 16
#define NDUSEROPT_SRCADDR 1
#define ND_ROUTER_ADVERT  134

/* The multicast groups of NETLINK_ROUTE that notifications go to, Linux's
 * RTNLGRP_ numbers; a socket is in group N when bit N - 1 of its groups is
 * set, by bind or by NETLINK_ADD_MEMBERSHIP. */
#define RTNLGRP_LINK           1
#define RTNLGRP_IPV4_IFADDR    5
#define RTNLGRP_IPV4_ROUTE     7
#define RTNLGRP_IPV6_IFADDR    9
#define RTNLGRP_IPV6_ROUTE     11
#define RTNLGRP_ND_USEROPT     20
#define GROUP_BIT(g)           (1u << ((g) - 1))
/* The group of NETLINK_KOBJECT_UEVENT the kernel's uevents go to; udev
 * sends its own, after its rules, to group 2. */
#define NL_UEVENT_GROUP_KERNEL 1u
#define WATCH_GROUPS                                                                            \
    (GROUP_BIT(RTNLGRP_LINK) | GROUP_BIT(RTNLGRP_IPV4_IFADDR) | GROUP_BIT(RTNLGRP_IPV4_ROUTE) | \
     GROUP_BIT(RTNLGRP_IPV6_IFADDR) | GROUP_BIT(RTNLGRP_IPV6_ROUTE))

/* The options of SOL_NETLINK that change something here. */
#define NETLINK_ADD_MEMBERSHIP  1
#define NETLINK_DROP_MEMBERSHIP 2
#define NETLINK_NO_ENOBUFS      5
#define NETLINK_CAP_ACK         10
#define NETLINK_EXT_ACK         11

/* Attributes of a link, an address and a route. */
#define IFLA_ADDRESS        1
#define IFLA_BROADCAST      2
#define IFLA_IFNAME         3
#define IFLA_MTU            4
#define IFLA_STATS          7
#define IFLA_OPERSTATE      16
#define IFLA_LINKMODE       17
#define IFLA_STATS64        23
#define IFA_ADDRESS         1
#define IFA_LOCAL           2
#define IFA_LABEL           3
#define IFA_BROADCAST       4
#define IFA_CACHEINFO       6
#define IFA_FLAGS           8
#define IFA_RT_PRIORITY     9
#define IFA_F_NOPREFIXROUTE 0x200u
#define IFA_PROTO           11
#define RTA_DST             1
#define RTA_OIF             4
#define RTA_GATEWAY         5
#define RTA_PREFSRC         7
#define RTA_PRIORITY        6
#define RTA_CACHEINFO       12
#define RTA_TABLE           15
#define RTA_PREF            20
#define NDA_DST             1
#define NDA_LLADDR          2
#define NDA_IFINDEX         8
#define NTF_PROXY           0x08u
#define NTF_ROUTER          0x80u

/* Attributes of an interface's configuration, and the indexes Linux gives
 * the configuration of all interfaces and the default for new ones. */
#define NETCONFA_IFINDEX                     1
#define NETCONFA_FORWARDING                  2
#define NETCONFA_RP_FILTER                   3
#define NETCONFA_IGNORE_ROUTES_WITH_LINKDOWN 6
#define NETCONFA_IFINDEX_ALL                 0xFFFFFFFFu
#define NETCONFA_IFINDEX_DEFAULT             0xFFFFFFFEu

#define IFF_LOWER_UP      0x10000u
#define IF_OPER_UP        6
#define IF_OPER_DORMANT   5
#define IF_OPER_DOWN      2
#define IF_OPER_UNKNOWN   0
#define IFA_F_PERMANENT   0x80u
#define IFA_F_SECONDARY   0x01u
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK     253
#define RT_TABLE_MAIN     254
#define RT_TABLE_DEFAULT  253
#define RT_TABLE_LOCAL    255
#define RT_SCOPE_HOST     254
#define RTN_LOCAL         2
#define RTN_BROADCAST     3
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
#define DIAG_TICK_US           TICK_US
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

/* An answer datagram packs messages as Linux's dump does: up to
 * NL_DGRAM_GOOD bytes, Linux's NLMSG_GOODSIZE of a page less its buffer's
 * overhead, or, once the socket has read with a larger buffer, up to that
 * read's size and at most NL_DGRAM_MAX, Linux's 32 KB less the same
 * overhead. So a reader is never handed a datagram longer than its buffers
 * after the first: Chromium reads with 4096 bytes, BusyBox with 8192 and
 * iproute2 with 32768. A message is at most 512 bytes, a link's with its two
 * sets of counters among them, so a datagram holds seven at least. */
#define NL_DGRAM_GOOD 3776u
#define NL_DGRAM_MAX  (32768u - 320u)
#define NL_MSG_MAX    512u

/* Policy: the bytes of notifications a socket may have queued. One that
 * would pass it is dropped and the socket's next receive fails with ENOBUFS,
 * as Linux's does when its receive buffer, 208 KiB by default, is full. An
 * answer to a request is queued whatever is there. */
#define NL_NOTIFY_QUEUE_MAX 65536u

/* A user option of a router advertisement as a message: the header, the
 * `nduseroptmsg`, the option, at most 255 units of 8 bytes, and the
 * router's address as an attribute. */
#define NL_USEROPT_MAX (NLMSG_HDRLEN + NDUSEROPT_MSG_LEN + 255u * 8u + 4u + 16u)

/* One queued datagram; its bytes follow the record. `group` is the
 * multicast group a notification went to, which the receiver's sockaddr_nl
 * gives as Linux does, and 0 for an answer. */
struct nl_dgram {
    struct nl_dgram *next;
    uint32_t len;
    uint32_t group;
    uint32_t portid; /* the sender's port, 0 for the kernel */
    int32_t cred[3]; /* the sender's process, user and group ids */
};

struct nl_group {
    struct nl_group *next;
    uint32_t id;
};

struct netlink_state {
    struct netlink_state *next; /* every netlink socket's, for the port ids */
    struct socket *sock;
    int protocol;
    /* The port id, and the multicast groups 1 to 32 as bits 0 to 31, which
     * the bind set and NETLINK_ADD_MEMBERSHIP adds to; `bound` is 1 once
     * there has been a bind. */
    uint32_t pid;
    uint32_t groups;
    int bound;
    uint32_t max_recv; /* the largest read so far, in bytes */
    /* Whether the socket was made in the first network namespace, whose
     * interfaces alone notifications are of. */
    int seen;
    /* Whether a notification dropped for room is kept from failing the next
     * receive, NETLINK_NO_ENOBUFS. */
    int no_enobufs;
    int cap_ack, ext_ack;
    struct nl_group *extra_groups; /* memberships numbered above 32 */
    /* The datagrams queued, oldest first, answers and notifications, and
     * their bytes. The queue changes under net_lock, since the timer tick
     * and the receive path queue notifications. */
    struct nl_dgram *head, *tail;
    uint32_t queued;
    uint32_t last_group; /* the `group` of the datagram read last */
    uint32_t last_portid;
    int32_t last_cred[3];
};

static struct netlink_state *states;

/* An answer being built: the datagram so far, the request's sequence
 * number and the socket's port id, which every message of the answer
 * repeats, and whether a datagram of it could not be queued. A
 * notification is built the same way, with `group` the group it goes to,
 * sequence number and port id 0, and no socket or datagram: each message
 * goes to the group's sockets as it is made, as a deletion when `del` is
 * set. */
struct nl_build {
    struct socket *sock;
    uint8_t buf[NL_DGRAM_MAX];
    uint32_t cap; /* the most bytes a datagram of this answer holds */
    uint32_t len;
    uint32_t seq;
    uint32_t pid;
    int failed;
    uint32_t group;
    int del;
};

/* The port ids handed to sockets bound with 0 after their process's first:
 * above 2^31, where no process id is. */
static uint32_t next_port = 0x80000000u;

/** Netlink's integers are in the host's order, which is little-endian on
 * x86; these store and load them at any alignment. */
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

/** The netlink state of `sock`, NULL once it is closed. */
static struct netlink_state *state_of(struct socket *sock)
{
    return (struct netlink_state *)sock->netlink;
}

int netlink_open(struct socket *sock, int protocol)
{
    struct netlink_state *st;
    uint32_t flags;

    if (protocol != NETLINK_ROUTE && protocol != NETLINK_SOCK_DIAG &&
        protocol != NETLINK_KOBJECT_UEVENT && protocol != NETLINK_GENERIC) {
        return -EPROTONOSUPPORT;
    }
    st = kzalloc(sizeof(*st));
    if (!st) {
        return -ENOMEM;
    }
    st->protocol = protocol;
    st->sock = sock;
    st->seen = net_ns_seen();
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
    if (st->protocol == NETLINK_GENERIC && st->pid) {
        genl_release(st->pid);
    }
    while (st->head) {
        struct nl_dgram *d = st->head;

        st->head = d->next;
        kfree(d);
    }
    while (st->extra_groups) {
        struct nl_group *g = st->extra_groups;

        st->extra_groups = g->next;
        kfree(g);
    }
    kfree(st);
}

static void watch_start(void);

/** Whether a netlink socket other than `self` is bound to port id `pid`.
 * The caller holds `net_lock`. */
static int port_taken(const struct netlink_state *self, uint32_t pid)
{
    const struct netlink_state *o;

    for (o = states; o; o = o->next) {
        if (o != self && o->protocol == self->protocol && o->bound && o->pid == pid) {
            return 1;
        }
    }
    return 0;
}

int netlink_bind(struct socket *sock, uint32_t pid, uint32_t groups)
{
    struct netlink_state *st = state_of(sock);
    uint32_t flags = net_lock();

    if (st->protocol == NETLINK_GENERIC) {
        unsigned i;

        for (i = 1; i <= 32; ++i) {
            if ((groups & GROUP_BIT(i)) && !genl_group_exists(i)) {
                net_unlock(flags);
                return -EINVAL;
            }
        }
    }
    if (st->bound && pid && st->pid != pid) {
        net_unlock(flags);
        return -EINVAL;
    }
    if (pid == 0 && st->bound) {
        pid = st->pid;
    }
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
    watch_start();
    net_unlock(flags);
    return 0;
}

uint32_t netlink_memberships(struct socket *sock, uint32_t *words, uint32_t max)
{
    const struct netlink_state *st = state_of(sock);
    const struct nl_group *g;
    uint32_t i, n, flags;

    for (i = 0; i < max; i++) {
        words[i] = 0;
    }
    flags = net_lock();
    n = st->groups != 0;
    if (max > 0) {
        words[0] = st->groups;
    }
    for (g = st->extra_groups; g; g = g->next) {
        i = (g->id - 1) / 32;
        if (i + 1 > n) {
            n = i + 1;
        }
        if (i < max) {
            words[i] |= 1u << ((g->id - 1) % 32);
        }
    }
    net_unlock(flags);
    return n;
}

int32_t netlink_setsockopt(struct socket *sock, int optname, int val, int optlen)
{
    struct netlink_state *st = state_of(sock);
    uint32_t flags;

    struct nl_group **link, *g;

    if (optname != NETLINK_ADD_MEMBERSHIP && optname != NETLINK_DROP_MEMBERSHIP &&
        optname != NETLINK_NO_ENOBUFS && optname != NETLINK_CAP_ACK && optname != NETLINK_EXT_ACK) {
        return 0;
    }
    if (optlen < 4 ||
        ((optname == NETLINK_ADD_MEMBERSHIP || optname == NETLINK_DROP_MEMBERSHIP) && val <= 0)) {
        return -EINVAL;
    }
    flags = net_lock();
    if (optname == NETLINK_NO_ENOBUFS) {
        st->no_enobufs = val != 0;
    } else if (optname == NETLINK_CAP_ACK) {
        st->cap_ack = val != 0;
    } else if (optname == NETLINK_EXT_ACK) {
        st->ext_ack = val != 0;
    } else if (st->protocol == NETLINK_GENERIC && !genl_group_exists((uint32_t)val)) {
        net_unlock(flags);
        return -EINVAL;
    } else if (val <= 32) {
        if (optname == NETLINK_ADD_MEMBERSHIP) {
            st->groups |= GROUP_BIT((uint32_t)val);
            watch_start();
        } else {
            st->groups &= ~GROUP_BIT((uint32_t)val);
        }
    } else {
        for (link = &st->extra_groups; *link && (*link)->id != (uint32_t)val;
             link = &(*link)->next) {
        }
        if (optname == NETLINK_DROP_MEMBERSHIP && *link) {
            g = *link;
            *link = g->next;
            kfree(g);
        } else if (optname == NETLINK_ADD_MEMBERSHIP && !*link) {
            g = kmalloc(sizeof(*g));
            if (!g) {
                net_unlock(flags);
                return -ENOMEM;
            }
            g->id = (uint32_t)val;
            g->next = 0;
            *link = g;
        }
    }
    net_unlock(flags);
    return 0;
}

void netlink_name(struct socket *sock, uint8_t *sa)
{
    struct netlink_state *st = state_of(sock);

    if (!st->bound) {
        (void)netlink_bind(sock, 0, st->groups);
    }
    k_memset(sa, 0, SOCKADDR_NL_LEN);
    put16(sa, AF_NETLINK);
    put32(sa + 4, st->pid);
    put32(sa + 8, st->groups);
}

/** Queue a copy of the `len` bytes at `data` on `st` as one datagram sent
 * to the multicast `group`, 0 for an answer, and wake a reader; 0, or
 * -ENOMEM. The caller holds net_lock. */
static int enqueue_group(struct netlink_state *st, const uint8_t *data, uint32_t len,
                         uint32_t group)
{
    struct nl_dgram *d = kmalloc(sizeof(*d) + len);

    if (!d) {
        return -ENOMEM;
    }
    d->next = 0;
    d->len = len;
    d->group = group;
    d->portid = 0;
    d->cred[0] = d->cred[1] = d->cred[2] = 0;
    k_memcpy(d + 1, data, len);
    if (st->tail) {
        st->tail->next = d;
    } else {
        st->head = d;
    }
    st->tail = d;
    st->queued += len;
    wq_wake(&st->sock->waiters, WQ_DATA);
    return 0;
}

/** Queue an answer, as `enqueue_group` with no group. */
static int enqueue(struct netlink_state *st, const uint8_t *data, uint32_t len)
{
    return enqueue_group(st, data, len, 0);
}

/** Queue a generic reply for the same socket and port that sent the request.
 * Copy data under net_lock; return 0 or a negative errno. */
int netlink_genl_reply(struct socket *sock, uint32_t portid, const uint8_t *data, unsigned len)
{
    struct netlink_state *st = state_of(sock);
    uint32_t flags = net_lock();
    int rc = -ENOENT;

    if (st && st->protocol == NETLINK_GENERIC && st->pid == portid) {
        rc = len <= NL_DGRAM_MAX ? enqueue(st, data, len) : -EMSGSIZE;
        if (!rc) {
            uint8_t *copy = (uint8_t *)(st->tail + 1);
            unsigned at;

            for (at = 0; at + NLMSG_HDRLEN <= len;) {
                unsigned size = get32(copy + at);

                if (size < NLMSG_HDRLEN || size > len - at) {
                    break;
                }
                put32(copy + at + 12, portid);
                at += (size + 3u) & ~3u;
            }
        }
    }
    net_unlock(flags);
    return rc;
}

/** Deliver one generic datagram to all members, recording each overrun.
 * Membership and queue walks run under net_lock; return 0 or -ENOBUFS. */
int netlink_genl_multicast(uint32_t group, const uint8_t *data, unsigned len)
{
    struct netlink_state *st;
    uint32_t flags;
    int rc = 0;

    if (!group || len > NL_DGRAM_MAX) {
        return -EMSGSIZE;
    }
    flags = net_lock();
    for (st = states; st; st = st->next) {
        struct nl_group *g;
        int joined = group <= 32 && (st->groups & GROUP_BIT(group));

        if (st->protocol != NETLINK_GENERIC || !st->seen) {
            continue;
        }
        for (g = st->extra_groups; g && !joined; g = g->next) {
            joined = g->id == group;
        }
        if (joined && (st->queued > NL_NOTIFY_QUEUE_MAX || len > NL_NOTIFY_QUEUE_MAX - st->queued ||
                       enqueue_group(st, data, len, group) != 0)) {
            rc = -ENOBUFS;
            if (!st->no_enobufs) {
                st->sock->so_error = ENOBUFS;
                wq_wake(&st->sock->waiters, WQ_DATA);
            }
        }
    }
    net_unlock(flags);
    return rc;
}

/** Queue one generic datagram for the socket bound to `portid`, as a family
 * sends an event to the one listener that asked for it. Return 0,
 * -ECONNREFUSED when no generic socket has that port, -EMSGSIZE, or -ENOBUFS
 * when its queue is full, which is also recorded on the socket. */
int netlink_genl_unicast(uint32_t portid, const uint8_t *data, unsigned len)
{
    struct netlink_state *st;
    uint32_t flags;
    int rc = -ECONNREFUSED;

    if (len > NL_DGRAM_MAX) {
        return -EMSGSIZE;
    }
    flags = net_lock();
    for (st = states; st; st = st->next) {
        if (st->protocol != NETLINK_GENERIC || !portid || st->pid != portid) {
            continue;
        }
        if (st->queued > NL_NOTIFY_QUEUE_MAX || len > NL_NOTIFY_QUEUE_MAX - st->queued ||
            enqueue(st, data, len) != 0) {
            rc = -ENOBUFS;
            if (!st->no_enobufs) {
                st->sock->so_error = ENOBUFS;
                wq_wake(&st->sock->waiters, WQ_DATA);
            }
        } else {
            rc = 0;
        }
        break;
    }
    net_unlock(flags);
    return rc;
}

/** Build Linux's error envelope, with optional echo and extended attributes.
 * req contains its complete bounded nlmsg_len bytes. Return delivery status. */
int netlink_ack(struct socket *sock, const uint8_t *req, int error, const char *detail,
                unsigned bad_offset)
{
    struct netlink_state *st = state_of(sock);
    struct genl_msg msg;
    unsigned echo, base, cap;
    uint8_t *buf;
    int rc;

    if (!st) {
        return -ENOENT;
    }
    echo = error && !st->cap_ack ? get32(req) : NLMSG_HDRLEN;
    base = (20u + echo + 3u) & ~3u;
    cap = base + 512u;
    buf = kzalloc(cap);
    if (!buf) {
        return -ENOMEM;
    }
    genl_msg_init(&msg, buf, cap);
    msg.len = base;
    put16(buf + 4, NLMSG_ERROR);
    put16(buf + 6, echo == NLMSG_HDRLEN ? 0x100u : 0);
    put32(buf + 8, get32(req + 8));
    put32(buf + 12, st->pid);
    put32(buf + 16, (uint32_t)error);
    k_memcpy(buf + 20, req, echo);
    if (st->ext_ack) {
        if (detail) {
            nla_put_string(&msg, 1, detail);
        }
        if (bad_offset) {
            nla_put_u32(&msg, 2, bad_offset);
        }
        if (msg.len > base) {
            put16(buf + 6, (uint16_t)(get16(buf + 6) | 0x200u));
        }
    }
    put32(buf, msg.len);
    {
        uint32_t flags = net_lock();

        rc = msg.error ? msg.error : enqueue(st, buf, msg.len);
        net_unlock(flags);
    }
    kfree(buf);
    return rc;
}

/** Queue the datagram built so far on the socket, if it has anything, and
 * wake a reader. A datagram that cannot be allocated marks the answer
 * failed. */
static void nl_flush(struct nl_build *b)
{
    uint32_t flags;
    int rc;

    if (b->len == 0 || b->failed) {
        return;
    }
    flags = net_lock();
    rc = enqueue(state_of(b->sock), b->buf, b->len);
    net_unlock(flags);
    if (rc != 0) {
        b->failed = 1;
        return;
    }
    b->len = 0;
}

/**
 * Send the message `msg` to every NETLINK_ROUTE socket of the first
 * network namespace in group `group`, each in a datagram of its own, as a
 * notification: with no flags and, for `del`, the type of the deletion,
 * which is one past the type of the addition for links, addresses, routes
 * and neighbours. A socket with no room for it, or for which no memory is
 * left, misses it and has its error set to ENOBUFS, which its next receive
 * returns, unless it asked for NETLINK_NO_ENOBUFS. Takes net_lock; may run
 * in an interrupt.
 */
static void notify(uint32_t group, int del, uint8_t *msg)
{
    struct netlink_state *st;
    uint32_t len = get32(msg), flags = net_lock();

    put16(msg + 6, 0);
    if (del) {
        put16(msg + 4, (uint16_t)(get16(msg + 4) + 1u));
    }
    for (st = states; st; st = st->next) {
        if (st->protocol != NETLINK_ROUTE || !st->seen || !(st->groups & GROUP_BIT(group))) {
            continue;
        }
        if (st->queued > NL_NOTIFY_QUEUE_MAX || len > NL_NOTIFY_QUEUE_MAX - st->queued ||
            enqueue_group(st, msg, len, group) != 0) {
            if (!st->no_enobufs) {
                st->sock->so_error = ENOBUFS;
                wq_wake(&st->sock->waiters, WQ_DATA);
            }
        }
    }
    net_unlock(flags);
}

/** Start a message of `type` and `flags` in `msg`, NL_MSG_MAX bytes that
 * this zeroes, with the answer's sequence number and port id and `body`
 * bytes of body; returns where the body starts. */
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

/** Append attribute `type` of `len` bytes to the message `msg`, padded to
 * four, and add it to the message's length. */
static void msg_attr(uint8_t *msg, uint16_t type, const void *data, uint16_t len)
{
    uint32_t at = get32(msg);

    put16(msg + at, (uint16_t)(4 + len));
    put16(msg + at + 2, type);
    k_memcpy(msg + at + 4, data, len);
    put32(msg, at + ((4u + len + 3u) & ~3u));
}

/** A 32-bit attribute, in the host's order. */
static void msg_attr32(uint8_t *msg, uint16_t type, uint32_t v)
{
    uint8_t le[4];

    put32(le, v);
    msg_attr(msg, type, le, 4);
}

/** An IPv4 address attribute: the address in network order, as it goes on
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

/** Add the finished message `msg` to the answer, in a new datagram when the
 * current one has no room. An answer whose datagram could not be queued
 * takes nothing more: the bytes it holds are still there, with no room
 * after them. */
static void nl_append(struct nl_build *b, uint8_t *msg)
{
    uint32_t len = get32(msg);

    if (b->group != 0) {
        notify(b->group, b->del, msg);
        return;
    }
    if (b->len + len > b->cap) {
        nl_flush(b);
    }
    if (b->failed) {
        return;
    }
    k_memcpy(b->buf + b->len, msg, len);
    b->len += len;
}

/** The prefix length of the netmask `mask`: its leading one bits. */
static uint32_t prefix_of(uint32_t mask)
{
    uint32_t n = 0;

    while (n < 32 && (mask & (0x80000000u >> n))) {
        n++;
    }
    return n;
}

/* Userspace's link policy is separate from the driver's carrier state. */
struct link_policy {
    const struct netdev *dev;
    int index; /* distinguish a newly registered interface at a reused address */
    uint8_t mode, authorized;
};
static struct link_policy link_policies[NETDEV_MAX + 1];

/** Find the first namespace's link policy, allocating a slot only on change.
 * The caller holds net_lock; other namespaces have no userspace link policy. */
static struct link_policy *policy_of(const struct netdev *nd, int create)
{
    unsigned i;
    struct link_policy *empty = 0;

    for (i = 0; i < NETDEV_MAX + 1; ++i) {
        if (link_policies[i].dev == nd && link_policies[i].index == nd->index) {
            return &link_policies[i];
        }
        if (!link_policies[i].dev || !netdev_by_index(link_policies[i].index)) {
            empty = &link_policies[i];
        }
    }
    if (create && empty) {
        empty->dev = nd;
        empty->index = nd->index;
        empty->mode = 0;
        empty->authorized = 1;
        return empty;
    }
    return 0;
}

/** Derive operstate from administrative state, carrier and userspace policy.
 * A loss of carrier revokes an authorization in userspace link mode. */
static uint8_t operstate(const struct netdev *nd)
{
    uint32_t flags = net_lock();
    struct link_policy *p = policy_of(nd, 0);
    int running = nd->up && netdev_running(nd);
    uint8_t oper;

    if (p && !running) {
        p->authorized = (uint8_t)!p->mode;
    }
    oper = nd->loopback          ? IF_OPER_UNKNOWN
           : !running            ? IF_OPER_DOWN
           : p && !p->authorized ? IF_OPER_DORMANT
                                 : IF_OPER_UP;
    net_unlock(flags);
    return oper;
}

/** Read the userspace link mode under network exclusion. */
static uint8_t linkmode(const struct netdev *nd)
{
    uint32_t flags = net_lock();
    struct link_policy *p = policy_of(nd, 0);
    uint8_t mode = p ? p->mode : 0;

    net_unlock(flags);
    return mode;
}

/**
 * Append the message of the link `nd`, with `flags`. IFF_UP is `nd`'s own
 * state, as an administrator left it; IFF_RUNNING, IFF_LOWER_UP and the
 * operational state up also need its card to report its link up
 * (`netdev_running`), as on Linux, where a link that is down has neither
 * flag. `lo` reports the operational state unknown. Its counters, copied
 * inside `net_lock` (netdev.h), go whole in Linux's `struct
 * rtnl_link_stats64`, 25 eight-byte words, seven of which this interface
 * keeps: the packets, bytes and drops each way, and the receive errors.
 * iproute2 reads it first; without it iproute2 prints a counter that only
 * that set has, the drops for other hosts, from memory it never set. They
 * go again in `struct rtnl_link_stats`, 24 words of 32 bits, truncated to
 * their low halves as Linux truncates them.
 */
static void link_msg(struct nl_build *b, const struct netdev *nd, uint16_t flags)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *ifi = msg_start(b, msg, RTM_NEWLINK, flags, 16);
    static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    int running = nd->up && netdev_running(nd);
    uint8_t oper = operstate(nd);
    uint8_t mode = linkmode(nd);
    uint32_t up = (nd->up ? IFF_UP : 0) | (running ? IFF_LOWER_UP : 0) |
                  (running && oper != IF_OPER_DORMANT ? IFF_RUNNING : 0);
    uint8_t stats[96];
    uint8_t stats64[200];
    /* rx_packets, tx_packets, rx_bytes, tx_bytes, rx_errors, tx_errors,
     * rx_dropped, tx_dropped: the first eight words of both sets. */
    uint64_t v[8];
    uint32_t lock;
    int i;

    put16(ifi + 2, (uint16_t)NETDEV_TYPE(nd));
    put32(ifi + 4, (uint32_t)nd->index);
    put32(ifi + 8, up | NETIF_KIND_FLAGS(nd));
    msg_attr(msg, IFLA_IFNAME, nd->name, (uint16_t)(k_strlen(nd->name) + 1));
    if (NETDEV_ADDR_LEN(nd)) {
        msg_attr(msg, IFLA_ADDRESS, nd->mac, 6);
        msg_attr(msg, IFLA_BROADCAST, nd->loopback ? nd->mac : broadcast, 6);
    }
    msg_attr32(msg, IFLA_MTU, nd->mtu);
    msg_attr(msg, IFLA_OPERSTATE, &oper, 1);
    msg_attr(msg, IFLA_LINKMODE, &mode, 1);
    lock = net_lock();
    v[0] = nd->rx_packets;
    v[1] = nd->tx_packets;
    v[2] = nd->rx_bytes;
    v[3] = nd->tx_bytes;
    v[4] = nd->rx_errors;
    v[5] = 0;
    v[6] = nd->rx_dropped;
    v[7] = nd->tx_dropped;
    net_unlock(lock);
    k_memset(stats, 0, sizeof(stats));
    k_memset(stats64, 0, sizeof(stats64));
    for (i = 0; i < 8; i++) {
        put32(stats + 4 * i, (uint32_t)v[i]);
        put32(stats64 + 8 * i, (uint32_t)v[i]);
        put32(stats64 + 8 * i + 4, (uint32_t)(v[i] >> 32));
    }
    msg_attr(msg, IFLA_STATS, stats, sizeof(stats));
    msg_attr(msg, IFLA_STATS64, stats64, sizeof(stats64));
    nl_append(b, msg);
}

/** Append a neighbour, the ARP cache's mapping `row`, on the interface of
 * index `index`. The cache keeps no age, so every mapping is reachable, but
 * for one set by hand as permanent. */
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

/** Whether the interface name `name`, in an array of `cap` bytes, is the
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

/** Whether `nd` is the interface of index `index`, or with that 0 of the
 * name of `name_len` bytes at `name`. */
static int link_is(const struct netdev *nd, uint32_t index, const char *name, uint32_t name_len)
{
    return nd && (index != 0 ? (uint32_t)nd->index == index
                             : name && same_name(nd->name, sizeof(nd->name), name, name_len));
}

/**
 * The interface a request for one link names, or NULL: by the index of its
 * `ifinfomsg`, or with that 0, by its IFLA_IFNAME attribute, which is how
 * `ip link show eth0` asks. `body` is the request's length past its header.
 * In a network namespace other than the first, only its `lo` is found.
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
    if (!net_ns_seen()) {
        return link_is(netif_ns_lo(), index, name, name_len) ? netif_ns_lo() : 0;
    }
    for (i = 0; i < n; i++) {
        struct netdev *nd = netdev_at(i);

        if (link_is(nd, index, name, name_len)) {
            return nd;
        }
    }
    return 0;
}

/** Append an IPv6 neighbour, the neighbour cache's `row`, in its state
 * of RFC 4861, which the cache keeps in Linux's NUD_ bits, with NTF_ROUTER
 * for one that said it is a router; one still asked for, or that failed,
 * has no station address to report. */
static void neigh6_msg(struct nl_build *b, const struct nd6_row *row)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *ndm = msg_start(b, msg, RTM_NEWNEIGH, NLM_F_MULTI, 12);

    ndm[0] = AF_INET6;
    put32(ndm + 4, (uint32_t)row->ifindex);
    put16(ndm + 8, row->state);
    ndm[10] = row->router ? NTF_ROUTER : 0;
    ndm[11] = RTN_UNICAST;
    msg_attr(msg, NDA_DST, row->addr, IP6_ADDR_LEN);
    if (!(row->state & (ND6_INCOMPLETE | ND6_FAILED))) {
        msg_attr(msg, NDA_LLADDR, row->mac, 6);
    }
    nl_append(b, msg);
}

/**
 * Append the neighbours a dump request `req` of `body` bytes asks for: of
 * IPv4 from a snapshot of the ARP cache, and of IPv6 from one of the
 * neighbour cache, by the family of its `ndmsg`, both for 0; none for
 * another family, or for the proxy table, which this kernel has none of;
 * and only those of one interface when its `ndmsg` or an NDA_IFINDEX
 * attribute names one.
 */
static void neigh_dump(struct nl_build *b, const uint8_t *req, uint32_t body)
{
    struct arp_row rows[ARP_CACHE_SIZE];
    struct nd6_row rows6[ND6_CACHE_SIZE];
    const uint8_t *ndm = req + NLMSG_HDRLEN;
    const uint8_t *attr = ndm + 12;
    const uint8_t *end = ndm + body;
    uint32_t want = body >= 8 ? get32(ndm + 4) : 0;
    uint8_t family = body >= 1 ? ndm[0] : 0;
    int k, i, m, n = netdev_seen_count();

    if ((family != 0 && family != AF_INET && family != AF_INET6) ||
        (body >= 11 && (ndm[10] & NTF_PROXY))) {
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
    m = family == AF_INET6 ? 0 : arp_snapshot(rows, ARP_CACHE_SIZE);
    for (k = 0; k < m; k++) {
        for (i = 0; i < n; i++) {
            const struct netdev *nd = netdev_at(i);

            if (nd && rows[k].iface && (want == 0 || (uint32_t)nd->index == want) &&
                same_name(nd->name, sizeof(nd->name), rows[k].iface, sizeof(nd->name))) {
                neigh_msg(b, &rows[k], nd->index);
            }
        }
    }
    m = family == AF_INET ? 0 : nd6_snapshot(rows6, ND6_CACHE_SIZE);
    for (k = 0; k < m; k++) {
        if (want == 0 || (uint32_t)rows6[k].ifindex == want) {
            neigh6_msg(b, &rows6[k]);
        }
    }
}

struct watch_addr4 {
    int index;
    uint32_t ip, dst, mask;
    uint32_t brd, flags, metric;
    struct addr4_routes opts;
};

/** Copy the IPv4 address at slot i and the attributes used by rtnetlink. */
static void read_addr4(const struct netdev *nd, int i, struct watch_addr4 *a)
{
    a->index = nd->index;
    a->ip = i < 0 ? nd->ip : nd->ip2[i];
    a->mask = i < 0 ? nd->mask : nd->mask2[i];
    a->dst = netdev_addr_dst(nd, i);
    a->brd = netdev_addr_brd(nd, i);
    a->metric = nd->addr4[i + 1].metric;
    a->opts = nd->addr4[i + 1];
    a->flags = (a->opts.valid_until ? 0 : IFA_F_PERMANENT) |
               (a->opts.deprecated ? NETDEV_A6_DEPRECATED : 0) |
               (netdev_secondary(nd, i) ? IFA_F_SECONDARY : 0) |
               (nd->addr4[i + 1].noprefix ? IFA_F_NOPREFIXROUTE : 0);
}

/** Seconds remaining to an IPv4 address deadline, rounded up. */
static uint32_t addr4_left(uint64_t until)
{
    uint64_t now = tick_count();
    return !until        ? NETDEV_LIFETIME_FOREVER
           : until > now ? (uint32_t)tick_div(until - now + TICK_HZ - 1, TICK_HZ, 0)
                         : 0;
}

/** Append an IPv4 address as captured before or after a change. */
static void addr_msg(struct nl_build *b, const struct netdev *nd, const struct watch_addr4 *a)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *ifa = msg_start(b, msg, RTM_NEWADDR, NLM_F_MULTI, 8);
    uint8_t ci[16];
    ifa[0] = AF_INET;
    ifa[1] = (uint8_t)prefix_of(a->mask);
    ifa[2] = (uint8_t)a->flags;
    ifa[3] = nd->loopback ? RT_SCOPE_HOST : RT_SCOPE_UNIVERSE;
    put32(ifa + 4, (uint32_t)a->index);
    msg_attr_ip(msg, IFA_ADDRESS, a->dst);
    msg_attr_ip(msg, IFA_LOCAL, a->ip);
    if (a->brd)
        msg_attr_ip(msg, IFA_BROADCAST, a->brd);
    put32(ci, a->opts.deprecated ? 0 : addr4_left(a->opts.preferred_until));
    put32(ci + 4, addr4_left(a->opts.valid_until));
    put32(ci + 8, a->opts.created);
    put32(ci + 12, a->opts.updated);
    msg_attr(msg, IFA_CACHEINFO, ci, sizeof(ci));
    if (a->opts.proto)
        msg_attr(msg, IFA_PROTO, &a->opts.proto, 1);
    msg_attr32(msg, IFA_FLAGS, a->flags);
    if (a->metric)
        msg_attr32(msg, IFA_RT_PRIORITY, a->metric);
    msg_attr(msg, IFA_LABEL, nd->name, (uint16_t)(k_strlen(nd->name) + 1));
    nl_append(b, msg);
}

/** The seconds left of the lifetime `life` of an address given or renewed
 * `age` seconds ago, NETDEV_LIFETIME_FOREVER for none. */
static uint32_t life_left(uint32_t life, uint32_t age)
{
    return life == NETDEV_LIFETIME_FOREVER ? life : life > age ? life - age : 0;
}

/**
 * Append the IPv6 address `a` of `nd`: its prefix, its state in Linux's
 * IFA_F_ bits, its scope and who made it; and in IFA_CACHEINFO the seconds
 * left of its preferred and valid lifetimes, 2^32 - 1 for forever, with the
 * time it was given or renewed in hundredths of a second since boot as both
 * stamps, which `ip -6 addr` prints as `valid_lft` and `preferred_lft`.
 */
static void addr6_msg(struct nl_build *b, const struct netdev *nd, const struct netdev_addr6 *a)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *ifa = msg_start(b, msg, RTM_NEWADDR, NLM_F_MULTI, 8);
    uint8_t ci[16];
    uint64_t secs = tick_div(tick_count() - a->since, TICK_HZ, 0);
    uint32_t age = secs < 0xFFFFFFFEu ? (uint32_t)secs : 0xFFFFFFFEu;
    uint32_t stamp = (uint32_t)TICKS_TO_CLOCK(a->since);

    ifa[0] = AF_INET6;
    ifa[1] = a->prefix;
    ifa[2] = a->flags;
    ifa[3] = a->scope;
    put32(ifa + 4, (uint32_t)nd->index);
    if (ip6_is_unspecified(a->peer)) {
        msg_attr(msg, IFA_ADDRESS, a->addr, IP6_ADDR_LEN);
    } else {
        msg_attr(msg, IFA_LOCAL, a->addr, IP6_ADDR_LEN);
        msg_attr(msg, IFA_ADDRESS, a->peer, IP6_ADDR_LEN);
    }
    put32(ci, life_left(a->preferred, age));
    put32(ci + 4, life_left(a->valid, age));
    put32(ci + 8, stamp);
    put32(ci + 12, stamp);
    msg_attr(msg, IFA_CACHEINFO, ci, sizeof(ci));
    msg_attr32(msg, IFA_FLAGS, a->flags | (a->noprefix ? IFA_F_NOPREFIXROUTE : 0));
    if (a->metric)
        msg_attr32(msg, IFA_RT_PRIORITY, a->metric);
    if (a->proto != NETDEV_PROTO_UNSPEC) {
        msg_attr(msg, IFA_PROTO, &a->proto, 1);
    }
    nl_append(b, msg);
}

/** Append every IPv6 address of `nd`, from a copy taken under net_lock,
 * since a removal moves the ones after it. */
static void addr6_msgs(struct nl_build *b, const struct netdev *nd)
{
    struct netdev_addr6 addrs[NETDEV_ADDRS6];
    uint32_t flags = net_lock();
    int i, n = nd->naddr6;

    k_memcpy(addrs, nd->addr6, (uint32_t)n * sizeof(addrs[0]));
    net_unlock(flags);
    for (i = 0; i < n; i++) {
        addr6_msg(b, nd, &addrs[i]);
    }
}

/** Append every IPv4 address of `nd`, its first then the others. */
static void addr_msgs(struct nl_build *b, const struct netdev *nd)
{
    struct watch_addr4 a;
    int i;
    for (i = -1; i < nd->naddr2; i++) {
        read_addr4(nd, i, &a);
        if (a.ip)
            addr_msg(b, nd, &a);
    }
}

/** The linkdown flag of a route whose next hop follows physical carrier. */
static uint32_t route_link_flag(int ifindex, int watches_carrier)
{
    const struct netdev *nd = netdev_by_index(ifindex);
    return watches_carrier && nd && !netdev_running(nd) ? 16u : 0;
}

/** Append the route `r` of the table: a subnet an address implies, reached
 * on the link, an interface's gateway, or a route a program added. */
static void route_msg(struct nl_build *b, const struct netdev_rt *r)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *rtm = msg_start(b, msg, RTM_NEWROUTE, NLM_F_MULTI, 12);

    rtm[0] = AF_INET;
    rtm[1] = (uint8_t)prefix_of(r->mask);
    rtm[4] = r->kind >= NETDEV_RT_LOCAL ? RT_TABLE_LOCAL : RT_TABLE_MAIN;
    rtm[5] = r->proto;
    rtm[6] = r->scope;
    rtm[7] = r->kind == NETDEV_RT_LOCAL       ? RTN_LOCAL
             : r->kind == NETDEV_RT_BROADCAST ? RTN_BROADCAST
                                              : RTN_UNICAST;
    put32(rtm + 8, route_link_flag(r->ifindex, r->scope != RT_SCOPE_HOST));
    msg_attr32(msg, RTA_TABLE, rtm[4]);
    if (r->mask != 0) {
        msg_attr_ip(msg, RTA_DST, r->dst);
    }
    if (r->gw) {
        msg_attr_ip(msg, RTA_GATEWAY, r->gw);
    }
    if (r->prefsrc) {
        msg_attr_ip(msg, RTA_PREFSRC, r->prefsrc);
    }
    if (r->metric != 0) {
        msg_attr32(msg, RTA_PRIORITY, r->metric);
    }
    msg_attr32(msg, RTA_OIF, (uint32_t)r->ifindex);
    nl_append(b, msg);
}

/** The payload of attribute `type` in the request `req` of `body` bytes past
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

/** Get the table selector from the extended attribute or the rtmsg. */
static uint32_t route_table(const uint8_t *req, uint32_t body)
{
    uint16_t len;
    const uint8_t *attr = req_attr(req, body, 12, RTA_TABLE, &len);
    if (attr && len == 4)
        return get32(attr);
    return body >= 12 ? req[NLMSG_HDRLEN + 4] : 0;
}

/** Append the IPv4 tables selected by a route dump. */
static void route4_dump(struct nl_build *b, const uint8_t *req, uint32_t body)
{
    uint32_t table = route_table(req, body);
    struct netdev_rt r;
    int i;

    if (!table || table == RT_TABLE_MAIN)
        for (i = 0; netdev_route_at(i, &r); i++)
            route_msg(b, &r);
    if (!table || table == RT_TABLE_LOCAL)
        for (i = 0; netdev_local_route_at(i, &r); i++)
            route_msg(b, &r);
}

/**
 * Append the IPv6 route `r` with `flags`: a prefix a card's address
 * implies, one a router advertised or the router itself, or one a program
 * added, by who made it, in the main table. Every IPv6 route has the
 * universe's scope and, as on Linux, a router preference, medium; one that
 * expires has the time left in RTA_CACHEINFO, in hundredths of a second,
 * which `ip -6 route` prints as `expires`.
 */
static void route6_msg(struct nl_build *b, const struct route6_row *r, uint16_t flags)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *rtm = msg_start(b, msg, RTM_NEWROUTE, flags, 12);
    uint8_t ci[32];
    uint8_t pref = 0;

    rtm[0] = AF_INET6;
    rtm[1] = r->plen;
    rtm[4] = r->local ? RT_TABLE_LOCAL : RT_TABLE_MAIN;
    rtm[5] = r->proto;
    rtm[6] = RT_SCOPE_UNIVERSE;
    rtm[7] = r->local ? RTN_LOCAL : RTN_UNICAST;
    put32(rtm + 8, route_link_flag(r->ifindex, !r->local));
    msg_attr32(msg, RTA_TABLE, rtm[4]);
    if (r->plen != 0) {
        msg_attr(msg, RTA_DST, r->dst, IP6_ADDR_LEN);
    }
    msg_attr32(msg, RTA_PRIORITY, r->metric);
    if (!ip6_is_unspecified(r->prefsrc))
        msg_attr(msg, RTA_PREFSRC, r->prefsrc, IP6_ADDR_LEN);
    if (!ip6_is_unspecified(r->gw)) {
        msg_attr(msg, RTA_GATEWAY, r->gw, IP6_ADDR_LEN);
    }
    msg_attr32(msg, RTA_OIF, (uint32_t)r->ifindex);
    if (r->expires != 0) {
        k_memset(ci, 0, sizeof(ci));
        put32(ci + 8, r->expires < 0x7FFFFFFFu / USER_HZ ? r->expires * USER_HZ : 0x7FFFFFFFu);
        msg_attr(msg, RTA_CACHEINFO, ci, sizeof(ci));
    }
    msg_attr(msg, RTA_PREF, &pref, 1);
    nl_append(b, msg);
}

/** Append every IPv6 route, each copied out of the table under net_lock
 * (`route6_at`). */
static void route6_dump(struct nl_build *b, uint32_t table)
{
    struct route6_row r;
    uint32_t flags;
    int i, more;

    for (i = 0;; i++) {
        flags = net_lock();
        more = route6_at(i, &r);
        net_unlock(flags);
        if (!more) {
            break;
        }
        if (!table || table == (r.local ? RT_TABLE_LOCAL : RT_TABLE_MAIN))
            route6_msg(b, &r, NLM_F_MULTI);
    }
}

/*
 * The notifications of links, addresses and routes. The state they are made
 * from, each interface's flags and link, its IPv4 and IPv6 addresses and the
 * two route tables, is compared with what it was at the last comparison,
 * ten times a second (`netlink_tick`) and after each change a request makes,
 * and each difference goes to its group: a link whose flags, link state or
 * MTU changed, an address or route that went or came, and an IPv6 address whose
 * state or lifetimes changed, as Linux announces one when its duplicate
 * detection ends, it is deprecated or a router renews it. Comparing the
 * state, rather than calling out from each place that changes it, catches
 * every change, the timers' and the receive path's among them. A change
 * undone before the next comparison is not seen. The comparison runs only
 * while a socket is in one of these groups; the first that joins takes the
 * state the next comparison starts from. Everything here runs with
 * interrupts off.
 */

/* The interfaces, and the routes of each family, the watch keeps. */
#define WATCH_IFS    (NETDEV_MAX + 1)
#define WATCH_ROUTES (32 + WATCH_IFS * (NETDEV_ADDRS + 1) * 4 + WATCH_IFS)

struct watch_link {
    int index;
    uint8_t up, running, oper, mode;
    uint32_t mtu;
};

struct watch_addr6 {
    int index;
    struct netdev_addr6 a;
};

struct watch_state {
    int nlink, naddr4, naddr6, nroute4, nroute6;
    struct watch_link link[WATCH_IFS];
    struct watch_addr4 addr4[WATCH_IFS * (NETDEV_ADDRS + 1)];
    struct watch_addr6 addr6[WATCH_IFS * NETDEV_ADDRS6];
    struct netdev_rt route4[WATCH_ROUTES];
    struct route6_row *route6;
    int cap6; /* allocated route rows */
};

/* The state at the last comparison is `watch_buf[watch_cur]`; the other
 * takes the state now. `watching` is 0 while no socket listens. */
static struct watch_state watch_buf[2];
static int watch_cur;
static int watching;
static uint32_t watch_next;
static struct nl_build watch_b;

/** The interface of index `index`, of any namespace's caller, since the
 * comparison runs in the timer tick; NULL for none. */
static struct netdev *watch_dev(int index)
{
    return netdev_by_index(index);
}

/** Whether a NETLINK_ROUTE socket of the first namespace is in a group the
 * comparison sends to. */
static int listening(void)
{
    const struct netlink_state *st;

    for (st = states; st; st = st->next) {
        if (st->protocol == NETLINK_ROUTE && st->seen && (st->groups & WATCH_GROUPS)) {
            return 1;
        }
    }
    return 0;
}

/** Snapshot the IPv6 table, including prefixes retained after address removal.
 * Return zero on allocation failure so a partial snapshot sends no deletes. */
static int watch_routes6(struct watch_state *w)
{
    struct route6_row row;
    while (route6_at(w->nroute6, &row)) {
        if (w->nroute6 == w->cap6) {
            int cap;
            struct route6_row *more;
            if ((unsigned)w->cap6 > 0x7fffffffu / sizeof(row) / 2)
                return 0;
            cap = w->cap6 ? w->cap6 * 2 : ROUTE6_MAX;
            more = kmalloc((size_t)cap * sizeof(row));
            if (!more)
                return 0;
            if (w->nroute6)
                k_memcpy(more, w->route6, (size_t)w->nroute6 * sizeof(row));
            kfree(w->route6);
            w->cap6 = cap;
            w->route6 = more;
        }
        w->route6[w->nroute6++] = row;
    }
    return 1;
}

/** Take the state now into `w`. */
static int watch_take(struct watch_state *w)
{
    int i, j, n = netdev_listed();

    w->nlink = w->naddr4 = w->naddr6 = w->nroute4 = w->nroute6 = 0;
    for (i = 0; i < n && i < WATCH_IFS; i++) {
        const struct netdev *nd = netdev_at(i);
        struct watch_link *l = &w->link[w->nlink++];

        l->index = nd->index;
        l->up = (uint8_t)(nd->up != 0);
        l->running = (uint8_t)(nd->up && netdev_running(nd));
        l->mtu = nd->mtu;
        l->oper = operstate(nd);
        l->mode = linkmode(nd);
        for (j = -1; j < nd->naddr2; j++) {
            struct watch_addr4 *a = &w->addr4[w->naddr4];

            read_addr4(nd, j, a);
            if (a->ip) {
                w->naddr4++;
            }
        }
        for (j = 0; j < nd->naddr6; j++) {
            w->addr6[w->naddr6].index = nd->index;
            w->addr6[w->naddr6++].a = nd->addr6[j];
        }
    }
    while (w->nroute4 < WATCH_ROUTES && netdev_route_at(w->nroute4, &w->route4[w->nroute4])) {
        w->nroute4++;
    }
    for (i = 0; w->nroute4 < WATCH_ROUTES && netdev_local_route_at(i, &w->route4[w->nroute4]); i++)
        w->nroute4++;
    return watch_routes6(w);
}

static int same_link(const struct watch_link *a, const struct watch_link *b)
{
    return a->index == b->index && a->up == b->up && a->running == b->running && a->mtu == b->mtu &&
           a->oper == b->oper && a->mode == b->mode;
}

static int same_addr4(const struct watch_addr4 *a, const struct watch_addr4 *b)
{
    return a->index == b->index && a->ip == b->ip && a->dst == b->dst && a->mask == b->mask &&
           a->metric == b->metric && a->brd == b->brd && a->flags == b->flags &&
           a->opts.valid_until == b->opts.valid_until &&
           a->opts.preferred_until == b->opts.preferred_until && a->opts.proto == b->opts.proto;
}

/** Whether two IPv6 addresses are one, and with `whole`, in the same
 * state, with the same lifetimes. */
static int same_addr6(const struct watch_addr6 *a, const struct watch_addr6 *b, int whole)
{
    if (a->index != b->index || !ip6_equal(a->a.addr, b->a.addr)) {
        return 0;
    }
    return !whole || (ip6_equal(a->a.peer, b->a.peer) && a->a.noprefix == b->a.noprefix &&
                      a->a.metric == b->a.metric && a->a.prefix == b->a.prefix &&
                      a->a.flags == b->a.flags && a->a.valid == b->a.valid &&
                      a->a.preferred == b->a.preferred && a->a.since == b->a.since);
}

static int same_route4(const struct netdev_rt *a, const struct netdev_rt *b)
{
    return a->dst == b->dst && a->mask == b->mask && a->gw == b->gw && a->metric == b->metric &&
           a->ifindex == b->ifindex && a->kind == b->kind && a->proto == b->proto &&
           a->scope == b->scope && a->prefsrc == b->prefsrc;
}

static int same_route6(const struct route6_row *a, const struct route6_row *b)
{
    return a->ifindex == b->ifindex && a->plen == b->plen && a->metric == b->metric &&
           a->proto == b->proto && a->local == b->local && ip6_equal(a->prefsrc, b->prefsrc) &&
           ip6_equal(a->dst, b->dst) && ip6_equal(a->gw, b->gw);
}

/**
 * Send what differs between the state `was` and the state `now`: the links
 * that changed; then, as deletions, the addresses and routes `now` has not;
 * then, as additions, those `was` had not, and the IPv6 addresses whose
 * state or lifetimes changed.
 */
static void watch_compare(const struct watch_state *was, const struct watch_state *now)
{
    struct nl_build *b = &watch_b;
    struct netdev *nd;
    int i, j, pass;

    b->sock = 0;
    b->seq = b->pid = 0;
    b->del = 0;
    b->group = RTNLGRP_LINK;
    for (i = 0; i < now->nlink; i++) {
        for (j = 0; j < was->nlink && !same_link(&was->link[j], &now->link[i]); j++) {
        }
        if (j == was->nlink && (nd = watch_dev(now->link[i].index)) != 0) {
            link_msg(b, nd, 0);
        }
    }
    for (pass = 0; pass < 2; pass++) {
        const struct watch_state *from = pass == 0 ? was : now;
        const struct watch_state *in = pass == 0 ? now : was;

        b->del = pass == 0;
        b->group = RTNLGRP_IPV4_IFADDR;
        for (i = 0; i < from->naddr4; i++) {
            const struct watch_addr4 *a = &from->addr4[i];

            for (j = 0; j < in->naddr4 && !same_addr4(&in->addr4[j], a); j++) {
            }
            if (j == in->naddr4 && (nd = watch_dev(a->index)) != 0) {
                addr_msg(b, nd, a);
            }
        }
        b->group = RTNLGRP_IPV6_IFADDR;
        for (i = 0; i < from->naddr6; i++) {
            const struct watch_addr6 *a = &from->addr6[i];

            for (j = 0; j < in->naddr6 && !same_addr6(&in->addr6[j], a, pass); j++) {
            }
            if (j == in->naddr6 && (nd = watch_dev(a->index)) != 0) {
                addr6_msg(b, nd, &a->a);
            }
        }
        b->group = RTNLGRP_IPV4_ROUTE;
        for (i = 0; i < from->nroute4; i++) {
            for (j = 0; j < in->nroute4 && !same_route4(&in->route4[j], &from->route4[i]); j++) {
            }
            if (j == in->nroute4) {
                route_msg(b, &from->route4[i]);
            }
        }
        b->group = RTNLGRP_IPV6_ROUTE;
        for (i = 0; i < from->nroute6; i++) {
            for (j = 0; j < in->nroute6 && !same_route6(&in->route6[j], &from->route6[i]); j++) {
            }
            if (j == in->nroute6) {
                route6_msg(b, &from->route6[i], 0);
            }
        }
    }
}

/** Start the watch, with the state now, when a socket listens and it has
 * not started. The caller holds net_lock. */
static void watch_start(void)
{
    if (!watching && listening()) {
        watching = watch_take(&watch_buf[watch_cur]);
    }
}

/** Compare the state now with the last, and send the differences; stop
 * the watch while nobody listens. The caller holds net_lock. */
static void watch_run(void)
{
    if (!listening()) {
        int i;
        for (i = 0; i < 2; i++) {
            kfree(watch_buf[i].route6);
            watch_buf[i].route6 = 0;
            watch_buf[i].cap6 = 0;
        }
        watching = 0;
        return;
    }
    if (!watching) {
        watch_start();
        return;
    }
    if (!watch_take(&watch_buf[!watch_cur]))
        return;
    watch_compare(&watch_buf[watch_cur], &watch_buf[!watch_cur]);
    watch_cur = !watch_cur;
}

void netlink_tick(void)
{
    if ((int32_t)((uint32_t)kernel_ticks - watch_next) < 0) {
        return;
    }
    watch_next = (uint32_t)kernel_ticks + MS_TO_TICKS(100);
    netconf_expire_addresses(tick_count());
    watch_run();
}

void netlink_nd_useropt(const struct netdev *nd, const uint8_t *src, const uint8_t *opt,
                        uint32_t len)
{
    static uint8_t msg[NL_USEROPT_MAX];
    uint8_t *m = msg + NLMSG_HDRLEN;

    if (len == 0 || len > 255u * 8u) {
        return;
    }
    k_memset(msg, 0, NLMSG_HDRLEN + NDUSEROPT_MSG_LEN);
    put32(msg, NLMSG_HDRLEN + NDUSEROPT_MSG_LEN + len);
    put16(msg + 4, RTM_NEWNDUSEROPT);
    m[0] = AF_INET6;
    put16(m + 2, (uint16_t)len);
    put32(m + 4, (uint32_t)nd->index);
    m[8] = ND_ROUTER_ADVERT;
    k_memcpy(m + NDUSEROPT_MSG_LEN, opt, len);
    msg_attr(msg, NDUSEROPT_SRCADDR, src, IP6_ADDR_LEN);
    notify(RTNLGRP_ND_USEROPT, 0, msg);
}

/** The destination of an RTM_GETROUTE request, its RTA_DST, or 0 when it
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

/** Append the answer to `ip route get`: the route `dst` takes, as Linux
 * reports a route it looked up (RTM_F_CLONED), through the interface and
 * next hop `netdev_route` chooses, or for an address of this machine or of
 * 127/8 a local route on `lo`, from 127.0.0.1 for 127/8 and from `dst`
 * itself otherwise. Returns 0, or -ENETUNREACH when nothing reaches it. */
static int route_get_msg(struct nl_build *b, uint32_t dst)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *rtm;
    const struct netdev *nd;
    struct netdev_rt selected = {0}, r;
    int i, local = 0, broadcast = 0;

    nd = netdev_route_lookup(dst, 0, &selected);
    for (i = 0; netdev_local_route_at(i, &r); i++) {
        if ((dst & r.mask) == r.dst &&
            (!broadcast || r.kind == NETDEV_RT_LOCAL || r.mask > selected.mask)) {
            nd = watch_dev(r.ifindex);
            local = r.kind == NETDEV_RT_LOCAL;
            broadcast = !local;
            selected = r;
            if (local)
                break;
        }
    }
    if ((local || IP_IS_LOOPBACK(dst)) && netdev_loopback()) {
        nd = netdev_loopback();
        local = 1;
    }
    if (!nd) {
        return -ENETUNREACH;
    }
    rtm = msg_start(b, msg, RTM_NEWROUTE, 0, 12);
    rtm[0] = AF_INET;
    rtm[1] = 32;
    rtm[4] = local || broadcast ? RT_TABLE_LOCAL : RT_TABLE_MAIN;
    rtm[6] = local ? RT_SCOPE_HOST : RT_SCOPE_UNIVERSE;
    rtm[7] = local ? RTN_LOCAL : broadcast ? RTN_BROADCAST : RTN_UNICAST;
    put32(rtm + 8, RTM_F_CLONED);
    msg_attr32(msg, RTA_TABLE, local || broadcast ? RT_TABLE_LOCAL : RT_TABLE_MAIN);
    msg_attr_ip(msg, RTA_DST, dst);
    msg_attr32(msg, RTA_OIF, (uint32_t)nd->index);
    msg_attr_ip(msg, RTA_PREFSRC,
                local ? (IP_IS_LOOPBACK(dst) ? IP_LOOPBACK_ADDR : dst) : selected.src);
    if (!local && selected.gw) {
        msg_attr_ip(msg, RTA_GATEWAY, selected.gw);
    }
    nl_append(b, msg);
    return 0;
}

/** Append the rules every Linux machine starts with, which are what route
 * lookups here amount to: for IPv4, of `family` AF_INET or 0, the local
 * addresses, then the main table, then the empty default table; for IPv6,
 * of AF_INET6 or 0, the first two. */
static void rule_dump(struct nl_build *b, uint8_t family)
{
    static const uint32_t rules[3][2] = {
        {0, RT_TABLE_LOCAL}, {32766, RT_TABLE_MAIN}, {32767, RT_TABLE_DEFAULT}};
    int i;

    for (i = 0; i < 5; i++) {
        uint8_t msg[NL_MSG_MAX];
        uint8_t *frh;
        int v6 = i >= 3;

        if (v6 ? family != 0 && family != AF_INET6 : family != 0 && family != AF_INET) {
            continue;
        }
        frh = msg_start(b, msg, RTM_NEWRULE, NLM_F_MULTI, 12);
        frh[0] = v6 ? AF_INET6 : AF_INET;
        frh[4] = (uint8_t)rules[i % 3][1];
        frh[7] = FR_ACT_TO_TBL;
        msg_attr32(msg, FRA_TABLE, rules[i % 3][1]);
        if (rules[i % 3][0] != 0) {
            msg_attr32(msg, FRA_PRIORITY, rules[i % 3][0]);
        }
        nl_append(b, msg);
    }
}

/** Append a queueing discipline for each interface: `noqueue`, since a
 * frame here goes to the card when it is sent, with no queue in front of
 * it. */
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
 * The changes a privileged caller asks for, which `ip` sends, made through
 * the operations the interface ioctls use (netif.c, netconf.c, arp.c): each
 * answers 0 or a negative errno, which `change_answer` acknowledges. What an
 * interface here cannot hold is refused with EOPNOTSUPP rather than accepted
 * and dropped: a rename, and on `lo` being taken down, an MTU but its own
 * and any change of its addresses, routes or neighbours.
 */

/** An IPv4 attribute's address in the host's order, or 0 for none or for an
 * attribute that is not four bytes. */
static uint32_t req_ip(const uint8_t *req, uint32_t body, uint32_t fixed, uint16_t type)
{
    uint16_t len;
    const uint8_t *a = req_attr(req, body, fixed, type, &len);

    return a && len == 4
               ? ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3]
               : 0;
}

/** The interface of index `index`, or NULL; in a network namespace other
 * than the first, its `lo` for 1. */
static struct netdev *dev_of_index(uint32_t index)
{
    int i, n = netdev_seen_count();

    if (!net_ns_seen()) {
        return index == 1 ? netif_ns_lo() : 0;
    }
    for (i = 0; i < n; i++) {
        struct netdev *nd = netdev_at(i);

        if (nd && (uint32_t)nd->index == index) {
            return nd;
        }
    }
    return 0;
}

/**
 * Read a link change's IFLA_LINKMODE and IFLA_OPERSTATE into *mode and
 * *oper, each 0 when absent, and when either is there find in *policy the
 * link policy of nd, made on first use. Returns 0; -EINVAL for a mode other
 * than 0 or 1 or an operstate other than IF_OPER_UP or IF_OPER_DORMANT, or
 * either not one byte long; -EOPNOTSUPP for a loopback interface or one
 * outside the first namespace; or -ENOSPC when no policy slot is free.
 */
static int32_t link_policy_asked(const uint8_t *req, uint32_t body, const struct netdev *nd,
                                 const uint8_t **mode, const uint8_t **oper,
                                 struct link_policy **policy)
{
    uint16_t len;
    uint32_t lock;

    *policy = 0;
    *mode = req_attr(req, body, 16, IFLA_LINKMODE, &len);
    if (*mode && (len != 1 || (*mode)[0] > 1)) {
        return -EINVAL;
    }
    *oper = req_attr(req, body, 16, IFLA_OPERSTATE, &len);
    if (*oper && (len != 1 || ((*oper)[0] != IF_OPER_UP && (*oper)[0] != IF_OPER_DORMANT))) {
        return -EINVAL;
    }
    if (!*mode && !*oper) {
        return 0;
    }
    if (!net_ns_seen() || nd->loopback) {
        return -EOPNOTSUPP;
    }
    lock = net_lock();
    *policy = policy_of(nd, 1);
    net_unlock(lock);
    return *policy ? 0 : -ENOSPC;
}

/** Apply a link change's mode and operstate, either 0 when absent, to nd's
 * policy: a new mode sets it, dormant mode leaving the link unauthorized
 * until an operstate of IF_OPER_UP authorizes it, and IF_OPER_DORMANT takes
 * the authorization back. Takes net_lock. */
static void link_policy_set(const struct netdev *nd, struct link_policy *policy,
                            const uint8_t *mode, const uint8_t *oper)
{
    uint32_t lock = net_lock();

    if (mode && policy->mode != mode[0]) {
        policy->mode = mode[0];
        policy->authorized = (uint8_t)!mode[0];
    }
    if (oper) {
        policy->authorized = oper[0] == IF_OPER_UP;
    }
    (void)operstate(nd);
    net_unlock(lock);
}

/** RTM_NEWLINK and RTM_SETLINK: up or down by IFF_UP in the change mask, an
 * MTU by IFLA_MTU (`netdev_set_mtu`), a link mode by IFLA_LINKMODE and an
 * operstate by IFLA_OPERSTATE (`link_policy_asked`), and a name only as it
 * is. The request is checked whole before anything changes. Returns 0,
 * -ENODEV for no such interface, -EINVAL for an MTU out of the interface's
 * range or an IFLA_MTU not four bytes long, or -EOPNOTSUPP for another name
 * or for the machine's `lo` taken down, since it is always up, and the
 * errors of `link_policy_asked`. A namespace's `lo` goes up and down
 * (`netif_ns_lo_set_up`). */
static int32_t link_change(const uint8_t *req, uint32_t body)
{
    struct netdev *nd = body >= 16 ? link_asked(req, body) : 0;
    uint16_t len;
    const uint8_t *a, *mtu, *mode, *oper;
    struct link_policy *policy;
    int32_t rc;

    if (!nd) {
        return -ENODEV;
    }
    mtu = req_attr(req, body, 16, IFLA_MTU, &len);
    if (mtu && (len != 4 || !netdev_mtu_ok(nd, get32(mtu)))) {
        return -EINVAL;
    }
    a = req_attr(req, body, 16, IFLA_IFNAME, &len);
    if (a && get32(req + NLMSG_HDRLEN + 4) != 0 &&
        !same_name(nd->name, sizeof(nd->name), (const char *)a, len)) {
        return -EOPNOTSUPP;
    }
    if ((get32(req + NLMSG_HDRLEN + 12) & IFF_UP) && nd->loopback && net_ns_seen() &&
        !(get32(req + NLMSG_HDRLEN + 8) & IFF_UP)) {
        return -EOPNOTSUPP;
    }
    rc = link_policy_asked(req, body, nd, &mode, &oper, &policy);
    if (rc) {
        return rc;
    }
    if (mtu) {
        netdev_set_mtu(nd, get32(mtu));
    }
    if (get32(req + NLMSG_HDRLEN + 12) & IFF_UP) {
        int up = (get32(req + NLMSG_HDRLEN + 8) & IFF_UP) != 0;

        if (!net_ns_seen()) {
            netif_ns_lo_set_up(nd, up);
        } else {
            rc = netconf_set_up(nd, up);
            if (rc) {
                return rc;
            }
        }
    }
    if (policy) {
        link_policy_set(nd, policy, mode, oper);
    }
    return 0;
}

/** The 16-byte attribute `type` of a request, or NULL when it has none or
 * one of another length. */
static const uint8_t *req_ip6(const uint8_t *req, uint32_t body, uint32_t fixed, uint16_t type)
{
    uint16_t len;
    const uint8_t *a = req_attr(req, body, fixed, type, &len);

    return a && len == IP6_ADDR_LEN ? a : 0;
}

/**
 * Append the answer to `ip -6 route get`: the route the destination of the
 * request `req`, its RTA_DST, takes, in the scope of its RTA_OIF, as
 * `ip6_route` and `ip6_source` choose the card, the next hop and the
 * source, reported as a route looked up (RTM_F_CLONED); or for ::1 or an
 * address of this machine a local route on `lo`, from that address.
 * Returns 0, -EINVAL for a request with no IPv6 destination, -ENETUNREACH
 * when nothing reaches it, or the error of `ip6_source`.
 */
static int route6_get_msg(struct nl_build *b, const uint8_t *req, uint32_t body)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t via[IP6_ADDR_LEN], src[IP6_ADDR_LEN];
    struct route6_row route = {0};
    const uint8_t *dst = req_ip6(req, body, 12, RTA_DST);
    uint16_t len;
    const uint8_t *oif = req_attr(req, body, 12, RTA_OIF, &len);
    uint8_t *rtm;
    struct netdev *nd;
    uint32_t lock;
    int local, rc = 0;

    if (!dst) {
        return -EINVAL;
    }
    lock = net_lock();
    local = ip6_is_loopback(dst) || (!ip6_is_multicast(dst) && netdev_owner6(dst) != 0);
    if (local)
        nd = netdev_loopback();
    else if (ip6_link_scoped(dst) || ip6_is_multicast(dst))
        nd = ip6_route(dst, oif && len == 4 ? (int)get32(oif) : 0, via);
    else
        nd = route6_resolve(dst, oif && len == 4 ? (int)get32(oif) : 0, &route);
    if (nd && local) {
        k_memcpy(src, dst, IP6_ADDR_LEN);
    } else if (nd) {
        rc = ip6_source(nd, dst, src);
    }
    net_unlock(lock);
    if (!nd) {
        return -ENETUNREACH;
    }
    if (rc != 0) {
        return rc;
    }
    rtm = msg_start(b, msg, RTM_NEWROUTE, 0, 12);
    rtm[0] = AF_INET6;
    rtm[1] = 128;
    rtm[4] = local ? RT_TABLE_LOCAL : RT_TABLE_MAIN;
    rtm[6] = RT_SCOPE_UNIVERSE;
    rtm[7] = local ? RTN_LOCAL : RTN_UNICAST;
    put32(rtm + 8, RTM_F_CLONED);
    msg_attr32(msg, RTA_TABLE, local ? RT_TABLE_LOCAL : RT_TABLE_MAIN);
    msg_attr(msg, RTA_DST, dst, IP6_ADDR_LEN);
    msg_attr32(msg, RTA_OIF, (uint32_t)nd->index);
    msg_attr(msg, RTA_PREFSRC, src, IP6_ADDR_LEN);
    if (!ip6_is_unspecified(route.gw)) {
        msg_attr(msg, RTA_GATEWAY, route.gw, IP6_ADDR_LEN);
    }
    nl_append(b, msg);
    return 0;
}

/* Attributes of an IPv6 address request after decoding the netlink message. */
struct addr6_options {
    const uint8_t *peer;
    uint32_t flags, valid, preferred, metric;
};

/** Read the optional lifetime, prefix metric and full flag word. */
static void read_addr6_options(const uint8_t *req, uint32_t body, struct addr6_options *opts)
{
    uint16_t len;
    const uint8_t *a = req_attr(req, body, 8, IFA_FLAGS, &len);
    opts->flags = a && len == 4 ? get32(a) : req[NLMSG_HDRLEN + 2];
    a = req_attr(req, body, 8, IFA_CACHEINFO, &len);
    opts->preferred = a && len >= 8 ? get32(a) : NETDEV_LIFETIME_FOREVER;
    opts->valid = a && len >= 8 ? get32(a + 4) : NETDEV_LIFETIME_FOREVER;
    a = req_attr(req, body, 8, IFA_RT_PRIORITY, &len);
    opts->metric = a && len == 4 ? get32(a) : 0;
    opts->peer = req_ip6(req, body, 8, IFA_ADDRESS);
}

/** Apply an accepted address change while its table slot is locked. */
static void update_addr6(struct netdev *nd, struct netdev_addr6 *held,
                         const struct addr6_options *opts, int replace)
{
    held->noprefix = (opts->flags & IFA_F_NOPREFIXROUTE) != 0;
    if (replace)
        route6_peer_change(nd, held, opts->peer);
    else if (opts->peer && !ip6_equal(held->addr, opts->peer))
        k_memcpy(held->peer, opts->peer, IP6_ADDR_LEN);
    held->peer_prefix = replace && !held->noprefix && !ip6_is_unspecified(held->peer);
    held->deleted_routes &= NETDEV_A6_PEER_ROUTE | NETDEV_A6_LOCAL_ROUTE;
    held->since = tick_count();
    held->preferred = opts->preferred;
    held->valid = opts->valid;
    held->flags &= ~(NETDEV_A6_DEPRECATED | NETDEV_A6_PERMANENT | NETDEV_A6_NODAD);
    held->flags |= (opts->flags & NETDEV_A6_NODAD) | (!opts->preferred ? NETDEV_A6_DEPRECATED : 0) |
                   (opts->valid == NETDEV_LIFETIME_FOREVER ? NETDEV_A6_PERMANENT : 0);
    if (opts->metric)
        held->metric = opts->metric;
}

/** Change the IPv6 address selected by its local address and, on deletion,
 * its prefix length. A distinct IFA_ADDRESS supplies its peer. */
static int32_t addr6_change(const uint8_t *req, uint16_t type, uint16_t flags, uint32_t body)
{
    struct netdev *nd = dev_of_index(get32(req + NLMSG_HDRLEN + 4));
    const uint8_t *addr = req_ip6(req, body, 8, IFA_LOCAL);
    struct addr6_options opts;
    struct netdev_addr6 *held;
    uint32_t lock;
    int rc = 0, prefix = req[NLMSG_HDRLEN + 1];
    read_addr6_options(req, body, &opts);
    if (!addr)
        addr = opts.peer;
    if (!nd)
        return -ENODEV;
    if (!addr || prefix > 128)
        return -EINVAL;
    lock = net_lock();
    held = netdev_addr6_of(nd, addr);
    if (type == RTM_DELADDR) {
        rc = held && prefix == held->prefix ? netconf_del_address6(nd, addr) : -EADDRNOTAVAIL;
    } else if (!opts.valid || opts.preferred > opts.valid) {
        rc = -EINVAL;
    } else if (held) {
        if ((flags & NLM_F_EXCL) || !(flags & NLM_F_REPLACE))
            rc = -EEXIST;
        else
            update_addr6(nd, held, &opts, 1);
    } else {
        rc = netconf_add_address6(nd, addr, prefix, (int)(opts.flags & NETDEV_A6_NODAD), opts.valid,
                                  opts.preferred);
        if (!rc)
            update_addr6(nd, netdev_addr6_of(nd, addr), &opts, 0);
    }
    net_unlock(lock);
    return rc;
}

/** RTM_NEWADDR and RTM_DELADDR on a network namespace's `lo` `nd`, whose one
 * address is 127.0.0.1/8: added, with NLM_F_EXCL -EEXIST when it is there,
 * or removed, -EADDRNOTAVAIL when it is not. Any other is -EOPNOTSUPP. */
static int32_t ns_addr_change(struct netdev *nd, uint32_t ip, uint32_t prefix, uint16_t type,
                              uint16_t flags)
{
    if (ip != 0x7F000001u || (type == RTM_NEWADDR && prefix != 8)) {
        return -EOPNOTSUPP;
    }
    if (type == RTM_DELADDR) {
        if (nd->ip != ip) {
            return -EADDRNOTAVAIL;
        }
        nd->ip = nd->mask = 0;
        return 0;
    }
    if (nd->ip == ip) {
        return (flags & NLM_F_EXCL) ? -EEXIST : 0;
    }
    nd->ip = ip;
    nd->mask = 0xFF000000u;
    return 0;
}

/** Set creation attributes on insertion and renew the address's lifetimes. */
static void set_addr4_options(struct addr4_routes *opts, const uint8_t *req, uint32_t body,
                              const uint8_t *ci, int fresh)
{
    uint16_t len;
    const uint8_t *at = req_attr(req, body, 8, IFA_FLAGS, &len);
    uint32_t preferred = ci ? get32(ci) : NETDEV_LIFETIME_FOREVER;
    uint32_t valid = ci ? get32(ci + 4) : NETDEV_LIFETIME_FOREVER;
    uint64_t now = tick_count();
    uint32_t metric;
    opts->updated = (uint32_t)TICKS_TO_CLOCK(now);
    if (fresh) {
        opts->given_broadcast = 1;
        opts->broadcast = req_ip(req, body, 8, IFA_BROADCAST);
        opts->noprefix = at && len == 4 && (get32(at) & IFA_F_NOPREFIXROUTE);
        opts->created = opts->updated;
    }
    opts->valid_until = valid == NETDEV_LIFETIME_FOREVER ? 0 : now + (uint64_t)valid * TICK_HZ;
    opts->preferred_until =
        preferred == NETDEV_LIFETIME_FOREVER ? 0 : now + (uint64_t)preferred * TICK_HZ;
    opts->deprecated = !preferred;
    at = req_attr(req, body, 8, IFA_PROTO, &len);
    opts->proto = at && len == 1 ? *at : 0;
    at = req_attr(req, body, 8, IFA_RT_PRIORITY, &len);
    metric = at && len == 4 ? get32(at) : 0;
    if (opts->metric != metric) {
        opts->metric = metric;
        opts->deleted &= ~ADDR4_PREFIX;
    }
}

/** Set the prefix metric and broadcast attributes of a netlink address. */
static int addr4_add(const uint8_t *req, uint32_t body, struct netdev *nd, uint32_t ip,
                     uint32_t mask, uint32_t peer)
{
    uint16_t flags = get16(req + 6), len;
    const uint8_t *ci = req_attr(req, body, 8, IFA_CACHEINFO, &len);
    uint32_t lock;
    int r;
    if (ci && (len != 16 || !get32(ci + 4) || get32(ci) > get32(ci + 4)))
        return -EINVAL;
    lock = net_lock();
    r = netconf_add_address(nd, ip, mask, peer);
    if (!r || (r == -EEXIST && (flags & NLM_F_REPLACE) && !(flags & NLM_F_EXCL))) {
        int slot = netconf_addr_find(nd, ip, mask, peer ? peer : ip);
        if (slot < nd->naddr2)
            set_addr4_options(&nd->addr4[slot + 1], req, body, ci, !r);
        r = 0;
    }
    net_unlock(lock);
    return r;
}

/** RTM_NEWADDR and RTM_DELADDR: an address of an interface and its prefix,
 * its first or another. Returns the result of `netconf_add_address` or
 * `netconf_del_address`; -ENODEV for no such interface; -EINVAL for an
 * address of 0 to add; and to add one the interface has, 0, or -EEXIST
 * with NLM_F_EXCL. On `lo` only that last is answered, and the rest is
 * EOPNOTSUPP. A request of AF_INET6 is `addr6_change`'s, and EOPNOTSUPP in
 * a network namespace other than the first. */
static int32_t addr_change(const uint8_t *req, uint16_t type, uint16_t flags, uint32_t body)
{
    struct netdev *nd;
    uint32_t prefix, ip, peer, mask;

    if (body < 8) {
        return -ENODEV;
    }
    if (req[NLMSG_HDRLEN] == AF_INET6) {
        return net_ns_seen() ? addr6_change(req, type, flags, body) : -EOPNOTSUPP;
    }
    nd = dev_of_index(get32(req + NLMSG_HDRLEN + 4));
    if (!nd) {
        return -ENODEV;
    }
    prefix = req[NLMSG_HDRLEN + 1];
    ip = req_ip(req, body, 8, IFA_LOCAL);
    peer = req_ip(req, body, 8, IFA_ADDRESS);
    if (!ip && type != RTM_DELADDR) {
        ip = peer;
    }
    if (!net_ns_seen()) {
        return ns_addr_change(nd, ip, prefix, type, flags);
    }
    if (nd->loopback) {
        return type == RTM_NEWADDR && netdev_holds(nd, ip) ? ((flags & NLM_F_EXCL) ? -EEXIST : 0)
                                                           : -EOPNOTSUPP;
    }
    if (prefix > 32) {
        return -EINVAL;
    }
    mask = prefix ? 0xFFFFFFFFu << (32 - prefix) : 0;
    if (type == RTM_DELADDR) {
        return netconf_del_address(nd, ip, mask, peer);
    }
    if (ip == 0) {
        return -EINVAL;
    }
    return addr4_add(req, body, nd, ip, mask, peer);
}

/** The interface a route request is for: the one RTA_OIF names, NULL when
 * that is no interface, or, with no RTA_OIF, the one whose subnet has the
 * gateway `gw`, or the first. */
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

        if (at && netdev_gateway_reachable(at, gw)) {
            return at;
        }
    }
    return netdev_first();
}

/** Apply an IPv6 route to the named interface, or resolve its gateway.
 * Unspecified delete selectors match any route in the selected table. */
static int32_t route6_change(const uint8_t *req, uint16_t type, uint16_t flags, uint32_t body)
{
    struct route6_row row = {0};
    uint8_t via[IP6_ADDR_LEN];
    const uint8_t *dst = req_ip6(req, body, 12, RTA_DST);
    const uint8_t *gw = req_ip6(req, body, 12, RTA_GATEWAY);
    const uint8_t *src = req_ip6(req, body, 12, RTA_PREFSRC);
    struct netdev *nd = 0;
    uint16_t len;
    const uint8_t *at = req_attr(req, body, 12, RTA_PRIORITY, &len);
    int del = type == RTM_DELROUTE;
    row.metric = at && len == 4 ? get32(at) : 0;
    row.proto = req[NLMSG_HDRLEN + 5];
    row.plen = req[NLMSG_HDRLEN + 1];
    row.local = route_table(req, body) == RT_TABLE_LOCAL;
    if (src)
        k_memcpy(row.prefsrc, src, IP6_ADDR_LEN);
    if (dst)
        k_memcpy(row.dst, dst, IP6_ADDR_LEN);
    if (gw)
        k_memcpy(row.gw, gw, IP6_ADDR_LEN);
    at = req_attr(req, body, 12, RTA_OIF, &len);
    if (at) {
        nd = len == 4 ? dev_of_index(get32(at)) : 0;
        if (!nd)
            return -ENODEV;
    } else if (!del) {
        uint32_t lock = net_lock();
        nd = gw ? ip6_route(gw, 0, via) : netdev_first();
        net_unlock(lock);
        if (!nd)
            return -ENODEV;
    }
    return route6_change_row(nd, &row, del, (flags & NLM_F_REPLACE) != 0);
}

/** Whether the request selects a device route in the main or local table. */
static int supported_route(const uint8_t *req, uint32_t body)
{
    uint32_t table = route_table(req, body);
    uint8_t kind = req[NLMSG_HDRLEN + 7];
    if (table == RT_TABLE_LOCAL)
        return !kind || kind == RTN_LOCAL ||
               (kind == RTN_BROADCAST && req[NLMSG_HDRLEN] == AF_INET);
    return (!table || table == RT_TABLE_MAIN) && (!kind || kind == RTN_UNICAST);
}

/** RTM_NEWROUTE and RTM_DELROUTE: the default route of an interface, which
 * is its gateway, the route a subnet implies, which is there while its
 * address is, and any other route, which goes in the table. Returns 0 or a
 * negative errno: -ENODEV for no interface, -EOPNOTSUPP on `lo`, -EINVAL for
 * a prefix past 32, or that of the change. A request of AF_INET6 is
 * `route6_change`'s. */
static int32_t route_change(const uint8_t *req, uint16_t type, uint16_t flags, uint32_t body)
{
    struct netdev_rt r = {0};
    int rc;
    uint32_t lock;
    struct netdev *nd;
    uint32_t dst_len = body >= 12 ? req[NLMSG_HDRLEN + 1] : 0;
    uint16_t len;
    const uint8_t *prio = req_attr(req, body, 12, RTA_PRIORITY, &len);

    if (body >= 12 && !supported_route(req, body))
        return -EOPNOTSUPP;
    if (body >= 12 && req[NLMSG_HDRLEN] == AF_INET6) {
        return route6_change(req, type, flags, body);
    }
    r.gw = req_ip(req, body, 12, RTA_GATEWAY);
    r.dst = req_ip(req, body, 12, RTA_DST);
    r.metric = prio && len == 4 ? get32(prio) : 0;
    r.mask = dst_len == 0 || dst_len > 32 ? 0 : 0xFFFFFFFFu << (32 - dst_len);
    nd = body >= 12 ? route_dev(req, body, r.gw) : 0;
    if (!nd) {
        return -ENODEV;
    }
    if (nd->loopback) {
        return -EOPNOTSUPP;
    }
    if (dst_len > 32) {
        return -EINVAL;
    }
    r.proto = req[NLMSG_HDRLEN + 5];
    r.scope = req[NLMSG_HDRLEN + 6];
    r.prefsrc = req_ip(req, body, 12, RTA_PREFSRC);
    if (type == RTM_DELROUTE && !req_attr(req, body, 12, RTA_OIF, &len))
        nd = 0;
    lock = net_lock();
    if (route_table(req, body) == RT_TABLE_LOCAL) {
        r.kind = req[NLMSG_HDRLEN + 7] == RTN_LOCAL       ? NETDEV_RT_LOCAL
                 : req[NLMSG_HDRLEN + 7] == RTN_BROADCAST ? NETDEV_RT_BROADCAST
                                                          : 0;
        rc = netdev_local_route_change(nd, &r, type == RTM_DELROUTE, (flags & NLM_F_REPLACE) != 0);
    } else {
        rc = netdev_route_change(nd, &r, type == RTM_DELROUTE, (flags & NLM_F_REPLACE) != 0);
    }
    net_unlock(lock);
    return rc;
}

/** RTM_DELNEIGH: forget a mapping, which `ip neigh flush` asks for each;
 * RTM_NEWNEIGH: store one by hand, permanent when its state says so, which
 * `ip neigh add ... nud permanent` asks; of the ARP cache, or of the IPv6
 * neighbour cache for AF_INET6. Returns the result of `arp_forget` or
 * `arp_set`, or of `nd6_forget` or `nd6_set`; -EINVAL for a request
 * without an address of its family, with an index no interface has, or, to
 * store, without an interface or a six-byte MAC; or -EOPNOTSUPP on `lo`. */
static int32_t neigh_change(const uint8_t *req, uint16_t type, uint32_t body)
{
    int v6 = body >= 12 && req[NLMSG_HDRLEN] == AF_INET6;
    uint32_t ip = v6 ? 0 : req_ip(req, body, 12, NDA_DST);
    const uint8_t *ip6 = v6 ? req_ip6(req, body, 12, NDA_DST) : 0;
    uint32_t index = body >= 12 ? get32(req + NLMSG_HDRLEN + 4) : 0;
    struct netdev *nd = index != 0 ? dev_of_index(index) : 0;
    uint16_t len;
    const uint8_t *mac = req_attr(req, body, 12, NDA_LLADDR, &len);
    int permanent = (get16(req + NLMSG_HDRLEN + 8) & NUD_PERMANENT) != 0;

    if ((v6 ? !ip6 : ip == 0) || (index != 0 && !nd)) {
        return -EINVAL;
    }
    if (nd && nd->loopback) {
        return -EOPNOTSUPP;
    }
    if (type == RTM_DELNEIGH) {
        return v6 ? nd6_forget(nd, ip6) : arp_forget(nd, ip);
    }
    if (!nd || !mac || len != 6) {
        return -EINVAL;
    }
    return v6 ? nd6_set(nd, ip6, mac, permanent) : arp_set(nd, ip, mac, permanent);
}

/** Whether `type` asks for a change rather than a reading. */
static int is_change(uint16_t type)
{
    return type == RTM_NEWLINK || type == RTM_SETLINK || type == RTM_NEWADDR ||
           type == RTM_DELADDR || type == RTM_NEWROUTE || type == RTM_DELROUTE ||
           type == RTM_NEWNEIGH || type == RTM_DELNEIGH;
}

/** Make the change `req` asks for: -EPERM for a caller without privilege
 * over its network namespace, -EOPNOTSUPP for a route or a neighbour in a
 * namespace other than the first, else its handler's result. */
static int32_t change(const uint8_t *req, uint16_t type, uint16_t flags, uint32_t body)
{
    if (!net_admin()) {
        return -EPERM;
    }
    if (!net_ns_seen() && type != RTM_NEWLINK && type != RTM_SETLINK && type != RTM_NEWADDR &&
        type != RTM_DELADDR) {
        return -EOPNOTSUPP;
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

/** Append the message that ends a dump, with `req` NULL, or an NLMSG_ERROR
 * of `error` answering the request whose header is `req`, 0 being an
 * acknowledgement. */
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

/** Make the change `req` asks for and acknowledge it: with its error, or
 * with 0 when NLM_F_ACK asks for an acknowledgement. */
static void change_answer(struct nl_build *b, const uint8_t *req, uint16_t type, uint16_t flags,
                          uint32_t body)
{
    int32_t rc = change(req, type, flags, body);
    uint32_t lock = net_lock();

    watch_run();
    net_unlock(lock);
    if (rc != 0 || (flags & NLM_F_ACK)) {
        end_msg(b, req, rc);
    }
}

/**
 * Append the configuration of `family` of the interface of index `index`,
 * or of all and of the default with Linux's -1 and -2, as `ip netconf`
 * reads it: forwarding, the reverse-path filter, which IPv6 has not,
 * multicast forwarding, proxy ARP or neighbour discovery, and the routes
 * through a link that is down. Each is 0, since this stack does none of
 * them, but IPv6's forwarding, which is `net.ipv6.conf.*.forwarding`
 * (`nd6_conf_get`).
 */
static void netconf_msg(struct nl_build *b, uint8_t family, uint32_t index, uint16_t flags)
{
    uint8_t msg[NL_MSG_MAX];
    uint8_t *ncm = msg_start(b, msg, RTM_NEWNETCONF, flags, 4);
    int32_t forwarding = 0;
    uint16_t a;

    if (family == AF_INET6) {
        (void)nd6_conf_get(index == NETCONFA_IFINDEX_ALL       ? ND6_CONF_ALL
                           : index == NETCONFA_IFINDEX_DEFAULT ? ND6_CONF_DEFAULT
                                                               : (int)index,
                           ND6_FORWARDING, &forwarding);
    }
    ncm[0] = family;
    msg_attr32(msg, NETCONFA_IFINDEX, index);
    for (a = NETCONFA_FORWARDING; a <= NETCONFA_IGNORE_ROUTES_WITH_LINKDOWN; a++) {
        if (family != AF_INET6 || a != NETCONFA_RP_FILTER) {
            msg_attr32(msg, a, a == NETCONFA_FORWARDING ? (uint32_t)forwarding : 0);
        }
    }
    nl_append(b, msg);
}

/** RTM_GETNETCONF: with NLM_F_DUMP, a message for each interface, then all
 * and the default, for IPv4, for IPv6, or for both with any family;
 * otherwise the one its NETCONFA_IFINDEX names, of IPv6 for AF_INET6 and
 * of IPv4 for any other, EINVAL with none and ENODEV for an index no
 * interface has. */
static void netconf_answer(struct nl_build *b, const uint8_t *req, uint16_t flags, uint32_t body)
{
    uint16_t len;
    const uint8_t *at;
    uint32_t index;
    uint8_t family = body >= 1 ? req[NLMSG_HDRLEN] : 0, f;
    int i;

    if ((flags & NLM_F_DUMP) == NLM_F_DUMP) {
        for (f = AF_INET; f <= AF_INET6; f = (uint8_t)(f == AF_INET ? AF_INET6 : 255)) {
            if (family != 0 && family != f) {
                continue;
            }
            for (i = 0; i < netdev_seen_count(); i++) {
                const struct netdev *nd = netdev_at(i);

                if (nd) {
                    netconf_msg(b, f, (uint32_t)nd->index, NLM_F_MULTI);
                }
            }
            netconf_msg(b, f, NETCONFA_IFINDEX_ALL, NLM_F_MULTI);
            netconf_msg(b, f, NETCONFA_IFINDEX_DEFAULT, NLM_F_MULTI);
        }
        end_msg(b, 0, 0);
        return;
    }
    at = req_attr(req, body, 4, NETCONFA_IFINDEX, &len);
    if (!at || len != 4) {
        end_msg(b, req, -EINVAL);
        return;
    }
    index = get32(at);
    if (index != NETCONFA_IFINDEX_ALL && index != NETCONFA_IFINDEX_DEFAULT &&
        !dev_of_index(index)) {
        end_msg(b, req, -ENODEV);
        return;
    }
    netconf_msg(b, family == AF_INET6 ? AF_INET6 : AF_INET, index, 0);
}

/** Append `nd`'s link for RTM_GETLINK, else its addresses. */
static void link_or_addrs(struct nl_build *b, uint16_t type, const struct netdev *nd)
{
    if (type == RTM_GETLINK) {
        link_msg(b, nd, NLM_F_MULTI);
    } else {
        addr_msgs(b, nd);
    }
}

/** Answer the NETLINK_ROUTE request of `type` and `flags`, whose header is
 * `req` and whose body of `body` bytes follows it. In a network namespace
 * with no interface but its `lo`, `lo` is listed, asked about and changed
 * as `link_change` and `ns_addr_change` say; a question about a route or
 * anything else finds none, and another dump is empty. A dump of
 * addresses, routes, neighbours, rules or configurations is of the family
 * the first byte of the request's body names, AF_INET or AF_INET6, or of
 * both for 0, IPv4 first. */
static void answer(struct nl_build *b, const uint8_t *req, uint16_t type, uint16_t flags,
                   uint32_t body)
{
    int i, n = netdev_seen_count();
    uint8_t family = body >= 1 ? req[NLMSG_HDRLEN] : 0;
    int dump = (flags & NLM_F_DUMP) == NLM_F_DUMP;

    if (!net_ns_seen() && !is_change(type) && !(type == RTM_GETLINK && (dump || body >= 16)) &&
        !(type == RTM_GETADDR && dump)) {
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
        int rc = body >= 12 && req[NLMSG_HDRLEN] == AF_INET6
                     ? route6_get_msg(b, req, body)
                     : route_get_msg(b, route_asked(req, body));

        if (rc != 0) {
            end_msg(b, req, rc);
        }
        return;
    }
    if (type == RTM_GETNETCONF) {
        netconf_answer(b, req, flags, body);
        return;
    }
    if ((flags & NLM_F_DUMP) == NLM_F_DUMP && (type == RTM_GETRULE || type == RTM_GETQDISC)) {
        if (type == RTM_GETRULE) {
            rule_dump(b, family);
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
        if (family != AF_INET6)
            route4_dump(b, req, body);
        if (family != AF_INET) {
            route6_dump(b, route_table(req, body));
        }
        end_msg(b, 0, 0);
        return;
    }
    for (i = 0; type == RTM_GETLINK && i < n; i++) {
        if (netdev_at(i)) {
            link_msg(b, netdev_at(i), NLM_F_MULTI);
        }
    }
    for (i = 0; type == RTM_GETADDR && family != AF_INET6 && i < n; i++) {
        if (netdev_at(i)) {
            addr_msgs(b, netdev_at(i));
        }
    }
    for (i = 0; type == RTM_GETADDR && family != AF_INET && i < n; i++) {
        if (netdev_at(i)) {
            addr6_msgs(b, netdev_at(i));
        }
    }
    if (!net_ns_seen() && netif_ns_lo() && (type == RTM_GETLINK || family != AF_INET6)) {
        link_or_addrs(b, type, netif_ns_lo());
    }
    end_msg(b, 0, 0);
}

/* An AF_INET or AF_INET6 socket as `inet_diag_msg_of` writes it, in the
 * host's order: with a timer, its kind and the milliseconds left. An
 * AF_INET6 socket's IPv4 addresses are written mapped. */
struct diag_inet {
    uint8_t family;
    uint8_t state;
    uint8_t retrans;
    uint8_t timer;
    uint16_t sport, dport;
    net_addr_t src, dst;
    uint32_t expires;
    uint32_t rqueue, wqueue, uid, inode;
};

/** Store a 16-bit or 32-bit value in network order, as the ports and
 * addresses of the diagnostic messages are. */
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

/** Write `d` into `msg` as an inet_diag_msg, the ports and addresses in
 * network order, the interface a link-local address is scoped to, and the
 * cookie its inode; with `protocol` not -1, a raw socket's, the
 * INET_DIAG_PROTOCOL attribute too. Returns `msg`, for the caller to add
 * attributes to and append. */
static uint8_t *inet_diag_msg_of(const struct nl_build *b, uint8_t *msg, const struct diag_inet *d,
                                 int protocol)
{
    uint8_t *body = msg_start(b, msg, SOCK_DIAG_BY_FAMILY, NLM_F_MULTI, DIAG_INET_MSG_LEN);
    uint8_t proto = (uint8_t)protocol;

    body[0] = d->family;
    body[1] = d->state;
    body[2] = d->timer;
    body[3] = d->retrans;
    put_be16(body + 4, d->sport);
    put_be16(body + 6, d->dport);
    if (d->family == AF_INET6) {
        net_addr_to6(d->src, body + 8);
        net_addr_to6(d->dst, body + 24);
        put32(body + 40, d->src.family == AF_INET6 ? d->src.scope : 0);
    } else {
        put_be32(body + 8, d->src.v4);
        put_be32(body + 24, d->dst.v4);
    }
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

/**
 * The first TCP_INFO_LEN bytes of Linux's `struct tcp_info` for `r`, which
 * `ss -i` prints: the state, the loss recovery, the timeouts in a row, the
 * options and window shifts, the retransmission timeout and the round trip
 * in microseconds, the segment size, the segments in flight, the window and
 * threshold in segments, and the segments sent again. The timeout counts in
 * ticks of TICK_HZ; the round trip and its variance are TCP's own samples
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

/** The timer of `r` that `ss -o` shows, into `d`: TIME_WAIT's end, a
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

/** Append the TCP connections of sockets of `family` whose states are in
 * the mask `wanted`, with the TCP information and the congestion control's
 * name when the mask `ext` of extensions asks for them; 0, or -ENOMEM when
 * they could not be copied. */
static int diag_tcp(struct nl_build *b, int family, uint32_t wanted, uint8_t ext)
{
    struct tcp_row *rows;
    struct diag_inet d;
    uint8_t msg[NL_MSG_MAX];
    uint8_t info[TCP_INFO_LEN];
    int i, n = tcp_snapshot(&rows);

    k_memset(&d, 0, sizeof(d));
    d.family = (uint8_t)family;
    for (i = 0; i < n; i++) {
        d.state = (uint8_t)tcp_state_to_linux(rows[i].state);
        if (!(wanted & (1u << d.state)) || (rows[i].family == AF_INET6) != (family == AF_INET6)) {
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

/** Append the UDP sockets of `family`, or with `type` SOCK_RAW the raw ones,
 * whose states are in the mask `wanted`: 1 for a connected socket and 7 for
 * the rest. A raw socket's port is its protocol, IPPROTO_RAW for the ioctl
 * descriptor, which is opened with it. Returns 0, or -ENOMEM. */
static int diag_dgram(struct nl_build *b, int family, int type, uint32_t wanted)
{
    struct socket_row *rows;
    struct diag_inet d;
    uint8_t msg[NL_MSG_MAX];
    int i, n = socket_snapshot(family, type, &rows);

    k_memset(&d, 0, sizeof(d));
    d.family = (uint8_t)family;
    for (i = 0; i < n; i++) {
        d.state = rows[i].connected ? DIAG_ESTABLISHED : DIAG_CLOSE;
        if (!(wanted & (1u << d.state))) {
            continue;
        }
        d.sport = rows[i].local_port;
        if (type == SOCK_RAW && d.sport == 0) {
            d.sport = IPPROTO_RAW;
        }
        d.src = rows[i].local_ip;
        d.dport = rows[i].connected ? rows[i].remote_port : 0;
        d.dst = rows[i].connected ? rows[i].remote_ip : net_addr4(0);
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

/** Append the AF_UNIX sockets whose states are in the mask `wanted`, with
 * the attributes the mask `show` asks for; 0, or -ENOMEM. A path is given
 * with its NUL, as Linux keeps it; an abstract name has none. */
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

/** The protocol of an inet request `r` of `body` bytes: the byte in the
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

/** Answer a NETLINK_SOCK_DIAG request of `type` and `flags`, whose header
 * is `req` and whose body of `body` bytes follows it: the dump and its end,
 * or ENOENT for a family or protocol with no sockets here, as Linux answers
 * one no module handles, or ENOMEM when the sockets could not be copied. A
 * type other than SOCK_DIAG_BY_FAMILY is EINVAL and a request that is not a
 * dump EOPNOTSUPP. In a network namespace other than the first the dump is
 * empty, since its sockets are in no table. */
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
        end_msg(b, 0, 0);
        return;
    }
    if ((flags & NLM_F_DUMP) != NLM_F_DUMP) {
        end_msg(b, req, -EOPNOTSUPP);
        return;
    }
    if ((r[0] == AF_INET || r[0] == AF_INET6) && body >= DIAG_INET_REQ_LEN) {
        protocol = diag_protocol(r, body);
        if (protocol == IP_PROTO_TCP) {
            rc = diag_tcp(b, r[0], get32(r + 4), r[2]);
        } else if (protocol == IP_PROTO_UDP) {
            rc = diag_dgram(b, r[0], SOCK_DGRAM, get32(r + 4));
        } else if (protocol == IPPROTO_RAW) {
            rc = diag_dgram(b, r[0], SOCK_RAW, get32(r + 4));
        }
    } else if (r[0] == AF_UNIX && body >= DIAG_UNIX_REQ_LEN) {
        rc = diag_unix(b, get32(r + 4), get32(r + 12));
    }
    end_msg(b, rc != 0 ? req : 0, rc);
}

/**
 * Pass the datagram of `len` bytes at `buf` from the uevent socket `from`
 * to `dst`'s group or port, as `netlink_send_to` says; a socket whose
 * queue would pass NL_NOTIFY_QUEUE_MAX misses it and is told ENOBUFS.
 */
static int32_t uevent_pass(struct netlink_state *from, const uint8_t *buf, uint32_t len,
                           const struct nl_dest *dst)
{
    struct netlink_state *st;
    uint32_t flags;
    int found = 0;

    if (!dst->privileged) {
        return -EPERM;
    }
    if (len > NL_DGRAM_MAX) {
        return -EMSGSIZE;
    }
    flags = net_lock();
    for (st = states; st; st = st->next) {
        int unicast = dst->pid != 0 && st->pid == dst->pid;

        if (st->protocol != NETLINK_KOBJECT_UEVENT || st->seen != from->seen ||
            (!unicast &&
             !(dst->group && dst->group <= 32 && (st->groups & GROUP_BIT(dst->group))))) {
            continue;
        }
        found |= unicast;
        if (st->queued > NL_NOTIFY_QUEUE_MAX || len > NL_NOTIFY_QUEUE_MAX - st->queued ||
            enqueue_group(st, buf, len, unicast ? 0 : dst->group) != 0) {
            if (!st->no_enobufs) {
                st->sock->so_error = ENOBUFS;
                wq_wake(&st->sock->waiters, WQ_DATA);
            }
            continue;
        }
        st->tail->portid = from->pid;
        k_memcpy(st->tail->cred, dst->cred, sizeof(st->tail->cred));
    }
    net_unlock(flags);
    return dst->pid != 0 && !found ? -ECONNREFUSED : (int32_t)len;
}

int32_t netlink_send(struct socket *sock, const uint8_t *buf, uint32_t len)
{
    return netlink_send_to(sock, buf, len, 0);
}

int32_t netlink_send_to(struct socket *sock, const uint8_t *buf, uint32_t len,
                        const struct nl_dest *dst)
{
    struct nl_build *b;
    uint32_t at = 0;
    struct netlink_state *st = state_of(sock);
    int failed;

    if (!st->bound) {
        (void)netlink_bind(sock, 0, st->groups);
    }
    if (st->protocol == NETLINK_KOBJECT_UEVENT) {
        if (!dst || (dst->pid == 0 && dst->group == 0)) {
            return (int32_t)len;
        }
        return uevent_pass(st, buf, len, dst);
    }
    if (st->protocol == NETLINK_GENERIC) {
        while (at + NLMSG_HDRLEN <= len) {
            uint32_t mlen = get32(buf + at);
            int rc;

            if (mlen < NLMSG_HDRLEN || mlen > len - at) {
                break;
            }
            if (get16(buf + at + 6) & NLM_F_REQUEST) {
                rc = genl_dispatch(sock, st->pid, buf + at, mlen);
                if (rc < 0) {
                    return rc;
                }
            }
            at += (mlen + 3u) & ~3u;
        }
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
    b->cap = st->max_recv > NL_DGRAM_GOOD ? st->max_recv : NL_DGRAM_GOOD;
    if (b->cap > NL_DGRAM_MAX) {
        b->cap = NL_DGRAM_MAX;
    }
    b->group = 0;
    b->del = 0;
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
    uint32_t flags = net_lock();
    int n = st && st->head ? (int)st->head->len : -1;

    net_unlock(flags);
    return n;
}

int netlink_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len)
{
    struct netlink_state *st = state_of(sock);
    uint32_t n, flags = net_lock();

    if (!st || !st->head) {
        net_unlock(flags);
        return -1;
    }
    if (offset >= st->head->len || len <= 0) {
        net_unlock(flags);
        return 0;
    }
    n = st->head->len - offset;
    if (n > (uint32_t)len) {
        n = (uint32_t)len;
    }
    k_memcpy(buf, (const uint8_t *)(st->head + 1) + offset, n);
    st->last_group = st->head->group;
    st->last_portid = st->head->portid;
    k_memcpy(st->last_cred, st->head->cred, sizeof(st->last_cred));
    net_unlock(flags);
    return (int)n;
}

uint32_t netlink_last_sender(struct socket *sock, int32_t cred[3])
{
    struct netlink_state *st = state_of(sock);

    if (!st) {
        cred[0] = cred[1] = cred[2] = 0;
        return 0;
    }
    k_memcpy(cred, st->last_cred, sizeof(st->last_cred));
    return st->last_portid;
}

uint32_t netlink_last_group(struct socket *sock)
{
    struct netlink_state *st = state_of(sock);
    uint32_t group = st ? st->last_group : 0;

    return group >= 1 && group <= 32 ? GROUP_BIT(group) : 0;
}

void netlink_uevent(const uint8_t *msg, uint32_t len)
{
    struct netlink_state *st;
    uint32_t flags;

    if (len > NL_DGRAM_MAX) {
        return;
    }
    flags = net_lock();
    for (st = states; st; st = st->next) {
        if (st->protocol != NETLINK_KOBJECT_UEVENT || !st->seen ||
            !(st->groups & GROUP_BIT(NL_UEVENT_GROUP_KERNEL))) {
            continue;
        }
        if (st->queued > NL_NOTIFY_QUEUE_MAX || len > NL_NOTIFY_QUEUE_MAX - st->queued ||
            enqueue_group(st, msg, len, NL_UEVENT_GROUP_KERNEL) != 0) {
            if (!st->no_enobufs) {
                st->sock->so_error = ENOBUFS;
                wq_wake(&st->sock->waiters, WQ_DATA);
            }
        }
    }
    net_unlock(flags);
}

void netlink_consume(struct socket *sock)
{
    struct netlink_state *st = state_of(sock);
    struct nl_dgram *d;
    uint32_t flags = net_lock();

    if (!st || !st->head) {
        net_unlock(flags);
        return;
    }
    d = st->head;
    st->head = d->next;
    if (!st->head) {
        st->tail = 0;
    }
    st->queued -= d->len;
    net_unlock(flags);
    kfree(d);
}

int netlink_ready(struct socket *sock)
{
    struct netlink_state *st = state_of(sock);

    return st && st->head != 0;
}

void netlink_note_recv(struct socket *sock, uint32_t len)
{
    struct netlink_state *st = state_of(sock);

    if (st && len > st->max_recv) {
        st->max_recv = len;
    }
}

/** Return the next service deadline for the stopped idle tick. */
uint32_t netlink_idle_ticks(void)
{
    uint32_t wait = TICK_HZ;

    if (listening()) {
        IDLE_TIMER_MIN(wait, watch_next, (uint32_t)kernel_ticks);
    }
    return wait;
}
