/**
 * netlinkmusl: check the NETLINK_ROUTE dumps of the machine's links,
 * addresses and routes, the request of one link, the errors, the port ids a
 * bind takes, the interface ioctls on an AF_UNIX socket and SIOCGIFCONF, and
 * a NETLINK_KOBJECT_UEVENT socket that hears nothing.
 *
 * A socket's first port id is the process ID and a second socket's is
 * another; a bind of a port id another socket has is EADDRINUSE. Each
 * dump's messages repeat the request's sequence number and the socket's
 * port id, have NLM_F_MULTI, and end with NLMSG_DONE: every link has a
 * name, every address is the one SIOCGIFADDR gives for its interface, and
 * every interface with an address has a route to its subnet. RTM_GETLINK of
 * the first link's index answers with that link alone, of no link with
 * ENODEV, and RTM_NEWADDR with no address with EINVAL. A dump request with
 * zeros after it in its buffer is answered, as iproute2's ip sends one, and
 * recvmsg with MSG_PEEK and MSG_TRUNC and no buffer gives an answer's
 * length, as ip asks it, and a read into a shorter buffer is cut and says so
 * with MSG_TRUNC in msg_flags. if_nametoindex, which asks an AF_UNIX socket,
 * finds the first link, and SIOCGIFCONF lists the interfaces with an
 * address. RTM_GETROUTE of one address, as `ip route get` asks, answers a
 * local route on lo for the machine's own address and a unicast one on the
 * first link for a neighbour's; `ip rule`'s dump lists Linux's three rules and
 * `tc qdisc`'s a discipline for each interface.
 *
 * The changes `ip` asks for are acknowledged: the first link's address is
 * EEXIST to add again, is taken away, which SIOCGIFADDR sees, and is given
 * back; its default route through SLIRP's gateway is set, EEXIST to set
 * again, removed, ESRCH to remove again, and set as it was; the link is set
 * up; an MTU of 9000 is EINVAL and forgetting a neighbour the cache does
 * not have is ENOENT. A second address is added, a local route to `ip route
 * get`, and taken away; a route to another network through the gateway is
 * added, EEXIST to add again, in /proc/net/route, and removed, and one
 * through a gateway no subnet has is ENETUNREACH; a route on the link to
 * that network is the one `ip route get` takes, before the default route
 * through the gateway; and a neighbour set by hand as permanent is in
 * /proc/net/arp with the flags 0x6, in lowercase hexadecimal, until it is
 * forgotten. A netlink protocol the kernel does not have fails with
 * EPROTONOSUPPORT.
 *
 * Given `machine=native` (tests/native/Readme.md), it leaves out the changes,
 * since they take away the address and the route of
 * the interface a native run reaches the machine by, and use the addresses of
 * QEMU's user network.
 *
 * It prints "NETLINKMUSL OK" and exits 0 on success, or prints "NETLINKMUSL
 * FAIL (detail: errno N)" and exits 1 at the first failure.
 *
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each row in order, a setup step joined to the row after it:
 * `socket-port-pid`, `second-port`, `bind-eaddrinuse`, `dump-links`,
 * `dump-addrs`, `dump-routes`, `getlink-one`, `getlink-enodev`,
 * `newaddr-einval`, `addr-eexist`, `addr-del`, `addr-restore`,
 * `default-route-add`, `default-route-del`, `link-up`, `mtu-einval`,
 * `delneigh-enoent`, `route-get-local`, `route-get-unicast`, `rule-dump`,
 * `qdisc-dump`, `second-addr`, `neigh-permanent`, `neigh-forget`, `route-add`,
 * `proc-net-route`, `route-del`, `route-enetunreach`, `route-on-link`,
 * `padded-dump`, `peek-trunc`, `cut-trunc`, `nametoindex`, `ifconf-length`,
 * `ifconf-buffer`, `uevent-silent`, `eprotonosupport`, and the TUN peer and
 * packet socket checks. A native run reports
 * the changes, `addr-eexist` to `delneigh-enoent` and `second-addr` to
 * `route-on-link`, not run, and a first link with no address leaves the four
 * lookups, `route-get-local` to `qdisc-dump`, not run. The first failure fails
 * its assertion and leaves the rest not run.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

/* Linux's netlink ABI, which the musl build has no uapi headers for:
 * <linux/netlink.h> and <linux/rtnetlink.h>, the parts used here. */
#define NETLINK_ROUTE          0
#define NETLINK_KOBJECT_UEVENT 15
#define NLM_F_REQUEST          0x001
#define NLM_F_MULTI            0x002
#define NLM_F_ACK              0x004
#define NLM_F_EXCL             0x200
#define NLM_F_CREATE           0x400
#define NLM_F_DUMP             0x300
#define NLMSG_ERROR            2
#define NLMSG_DONE             3
#define RTM_NEWLINK            16
#define RTM_GETLINK            18
#define RTM_NEWADDR            20
#define RTM_DELADDR            21
#define RTM_GETADDR            22
#define RTM_NEWROUTE           24
#define RTM_DELROUTE           25
#define RTM_GETROUTE           26
#define RTM_NEWNEIGH           28
#define RTM_DELNEIGH           29
#define RTM_NEWRULE            32
#define RTM_GETRULE            34
#define RTM_NEWQDISC           36
#define RTM_GETQDISC           38
#define RTA_DST                1
#define RTA_OIF                4
#define RTA_GATEWAY            5
#define IFLA_MTU               4
#define IFLA_STATS64           23
#define NDA_DST                1
#define NDA_LLADDR             2
#define NUD_PERMANENT          0x80
#define RT_TABLE_MAIN          254
#define RTN_UNICAST            1
#define RTN_LOCAL              2
#define IFLA_IFNAME            3
#define IFA_ADDRESS            1
#define IFA_LOCAL              2
#define TUNSETIFF              0x400454caU
#define TUN_IFF_TUN            0x0001
#define TUN_IFF_NO_PI          0x1000
#define ETH_TYPE_IP            0x0800
#define ARPHRD_NONE            65534

struct sockaddr_nl {
    unsigned short nl_family;
    unsigned short nl_pad;
    unsigned int nl_pid;
    unsigned int nl_groups;
};

struct nlmsghdr {
    unsigned int nlmsg_len;
    unsigned short nlmsg_type;
    unsigned short nlmsg_flags;
    unsigned int nlmsg_seq;
    unsigned int nlmsg_pid;
};

/* An NLMSG_ERROR's body: the error, then the request's header, which the
 * probe does not read. */
struct nlmsgerr {
    int error;
};

struct rtgenmsg {
    unsigned char rtgen_family;
};

struct ifinfomsg {
    unsigned char ifi_family;
    unsigned char ifi_pad;
    unsigned short ifi_type;
    int ifi_index;
    unsigned int ifi_flags;
    unsigned int ifi_change;
};

struct ifaddrmsg {
    unsigned char ifa_family;
    unsigned char ifa_prefixlen;
    unsigned char ifa_flags;
    unsigned char ifa_scope;
    unsigned int ifa_index;
};

struct rtattr {
    unsigned short rta_len;
    unsigned short rta_type;
};

#define NLMSG_ALIGN(len)  (((len) + 3u) & ~3u)
#define NLMSG_HDRLEN      ((int)NLMSG_ALIGN(sizeof(struct nlmsghdr)))
#define NLMSG_LENGTH(len) ((unsigned int)(len) + NLMSG_HDRLEN)
#define NLMSG_DATA(h)     ((void *)((char *)(h) + NLMSG_HDRLEN))
#define NLMSG_OK(h, len)                                                                   \
    ((len) >= (int)sizeof(struct nlmsghdr) && (h)->nlmsg_len >= sizeof(struct nlmsghdr) && \
     (int)(h)->nlmsg_len <= (len))
#define NLMSG_NEXT(h, len)                      \
    ((len) -= (int)NLMSG_ALIGN((h)->nlmsg_len), \
     (struct nlmsghdr *)((char *)(h) + NLMSG_ALIGN((h)->nlmsg_len)))
#define RTA_ALIGN(len) (((len) + 3u) & ~3u)
#define RTA_OK(a, len)                                                               \
    ((len) >= (int)sizeof(struct rtattr) && (a)->rta_len >= sizeof(struct rtattr) && \
     (int)(a)->rta_len <= (len))
#define RTA_NEXT(a, len)                    \
    ((len) -= (int)RTA_ALIGN((a)->rta_len), \
     (struct rtattr *)((char *)(a) + RTA_ALIGN((a)->rta_len)))
#define RTA_DATA(a)      ((void *)((char *)(a) + RTA_ALIGN(sizeof(struct rtattr))))
#define PAYLOAD(h, body) ((int)(h)->nlmsg_len - NLMSG_HDRLEN - (int)NLMSG_ALIGN(sizeof(body)))
#define FIRST_RTA(p)     ((struct rtattr *)((char *)(p) + NLMSG_ALIGN(sizeof(*(p)))))

/* The assertions of etude's report in the order they are checked, more than
 * probe_report.h's path holds, and how many are planned and reported. */
static const char *rows[40];
static int rows_planned, rows_reported;

/** Add the assertion `id` to the end of the planned rows. */
static void plan_row(const char *id)
{
    rows[rows_planned++] = id;
}

/** Report that the next planned row held. */
static void held(void)
{
    report_pass(rows[rows_reported++]);
}

/** In the process that began the report, report that the next planned row
 * failed with `why`, that those after it were not run, and the report's
 * end; after the end, nothing. */
static void stop(const char *why)
{
    if (report_owner == 0 || report_ended || report_getpid() != report_owner) {
        return;
    }
    if (rows_reported < rows_planned) {
        report_fail(rows[rows_reported++], why);
    }
    while (rows_reported < rows_planned) {
        report_notrun(rows[rows_reported++], "an earlier check failed");
    }
    report_end();
}

/** Report the next `n` planned rows not run, for `why`. */
static void skip(int n, const char *why)
{
    while (n-- > 0 && rows_reported < rows_planned) {
        report_notrun(rows[rows_reported++], why);
    }
}

/** Report one failed check with errno and return 1, the failure status. */
static int fail(const char *what)
{
    char why[160];

    snprintf(why, sizeof(why), "%s: errno %d", what, errno);
    printf("NETLINKMUSL FAIL (%s)\n", why);
    stop(why);
    return 1;
}

/** Report a failure as `fail` does, for a function that returns -1 for one. */
static int failed(const char *what)
{
    fail(what);
    return -1;
}

/* The first link the dump found, and the first socket's port id. */
static int first_index;
static char first_name[IF_NAMESIZE];
static unsigned int port;

/**
 * Send a request of `type` with `flags` and NLM_F_REQUEST, sequence number
 * `seq`, and the `len` bytes of `body`, at most 64. Returns 0, or -1 when the
 * send was short or failed.
 */
static int request(int fd, int type, int flags, unsigned int seq, const void *body, int len)
{
    struct {
        struct nlmsghdr h;
        char body[64];
    } req;

    memset(&req, 0, sizeof req);
    req.h.nlmsg_len = NLMSG_LENGTH(len);
    req.h.nlmsg_type = (unsigned short)type;
    req.h.nlmsg_flags = (unsigned short)(NLM_F_REQUEST | flags);
    req.h.nlmsg_seq = seq;
    memcpy(req.body, body, (size_t)len);
    return send(fd, &req, req.h.nlmsg_len, 0) == (ssize_t)req.h.nlmsg_len ? 0 : -1;
}

/**
 * Read into `*out` the address SIOCGIFADDR gives for the interface of index
 * `index`. Returns 0, or -1.
 */
static int ioctl_addr(int index, struct in_addr *out)
{
    struct ifreq ifr;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int rc;

    memset(&ifr, 0, sizeof ifr);
    ifr.ifr_ifindex = index;
    rc = s < 0 || ioctl(s, SIOCGIFNAME, &ifr) != 0 || ioctl(s, SIOCGIFADDR, &ifr) != 0 ? -1 : 0;
    if (rc == 0) {
        *out = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
    }
    if (s >= 0) {
        close(s);
    }
    return rc;
}

/**
 * Check a link of a dump: it has a name and the 25 counters of Linux's struct
 * rtnl_link_stats64, which iproute2 reads. With `first` set the link is kept
 * in `first_index` and `first_name` for the rows after the dumps. Returns 0,
 * or -1 after the failure is printed.
 */
static int link_ok(struct nlmsghdr *h, int first)
{
    struct ifinfomsg *ifi = NLMSG_DATA(h);
    int l = PAYLOAD(h, struct ifinfomsg);
    struct rtattr *a;
    const char *name = NULL;
    int stats64 = 0;

    for (a = FIRST_RTA(ifi); RTA_OK(a, l); a = RTA_NEXT(a, l)) {
        if (a->rta_type == IFLA_IFNAME) {
            name = RTA_DATA(a);
        }
        if (a->rta_type == IFLA_STATS64) {
            stats64 = (int)a->rta_len - (int)sizeof(struct rtattr);
        }
    }
    if (!name) {
        return failed("a link without a name");
    }
    if (stats64 < 25 * 8) {
        return failed("a link without the 64-bit counters iproute2 reads");
    }
    if (first) {
        first_index = ifi->ifi_index;
        snprintf(first_name, sizeof first_name, "%s", name);
    }
    return 0;
}

/**
 * Check an address of a dump: its local address is the one SIOCGIFADDR gives
 * for its interface. Returns 0, or -1 after the failure is printed.
 */
static int addr_ok(struct nlmsghdr *h)
{
    struct ifaddrmsg *ifa = NLMSG_DATA(h);
    int l = PAYLOAD(h, struct ifaddrmsg);
    struct rtattr *a;
    struct in_addr want;

    if (ioctl_addr((int)ifa->ifa_index, &want) != 0) {
        return failed("SIOCGIFADDR of an address's interface");
    }
    for (a = FIRST_RTA(ifa); RTA_OK(a, l); a = RTA_NEXT(a, l)) {
        if (a->rta_type == IFA_LOCAL && memcmp(RTA_DATA(a), &want, sizeof want) != 0) {
            return failed("an address that is not its interface's");
        }
    }
    return 0;
}

/**
 * Check the `count`-th message of the dump of sequence `seq`. Returns 1 for
 * its end, 0 for a message that checks, and -1, after the failure is
 * printed, for one that does not.
 */
static int dump_msg(struct nlmsghdr *h, unsigned int seq, int count)
{
    if (h->nlmsg_seq != seq || h->nlmsg_pid != port || !(h->nlmsg_flags & NLM_F_MULTI)) {
        return failed("a dump's message without its sequence, port id or NLM_F_MULTI");
    }
    if (h->nlmsg_type == NLMSG_DONE) {
        return 1;
    }
    if (h->nlmsg_type == RTM_NEWLINK) {
        return link_ok(h, count == 1);
    }
    if (h->nlmsg_type == RTM_NEWADDR) {
        return addr_ok(h);
    }
    return h->nlmsg_type == RTM_NEWROUTE || h->nlmsg_type == RTM_NEWRULE ||
                   h->nlmsg_type == RTM_NEWQDISC
               ? 0
               : failed("a dump's message of another type");
}

/**
 * Read a dump of `type` to its NLMSG_DONE and check each message
 * (`dump_msg`). Returns the count of messages before the end, or -1.
 */
static int dump(int fd, int type, unsigned int seq)
{
    char buf[8192] __attribute__((aligned(4)));
    struct rtgenmsg g;
    int n, rc, msgs = 0;

    g.rtgen_family = AF_INET;
    if (request(fd, type, NLM_F_DUMP, seq, &g, sizeof g) != 0) {
        return failed("send of a dump");
    }
    for (;;) {
        struct nlmsghdr *h;

        n = (int)recv(fd, buf, sizeof buf, 0);
        if (n <= 0) {
            return failed("recv of a dump");
        }
        for (h = (struct nlmsghdr *)buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
            rc = dump_msg(h, seq, msgs + 1);
            if (rc != 0) {
                return rc > 0 ? msgs : -1;
            }
            msgs++;
        }
    }
}

/**
 * Read the answer to a request that is not a dump: one message, with its
 * error in `*error` for an NLMSG_ERROR and the link's index in `*index` for
 * an RTM_NEWLINK. Returns the message's type, or -1 when the read is not one
 * whole message.
 */
static int one_answer(int fd, int *index, int *error)
{
    char buf[4096] __attribute__((aligned(4)));
    struct nlmsghdr *h;
    int n = (int)recv(fd, buf, sizeof buf, 0);

    if (n < (int)sizeof(*h)) {
        return -1;
    }
    h = (struct nlmsghdr *)buf;
    if (!NLMSG_OK(h, n) || (int)NLMSG_ALIGN(h->nlmsg_len) != n) {
        return -1;
    }
    if (h->nlmsg_type == NLMSG_ERROR) {
        *error = ((struct nlmsgerr *)NLMSG_DATA(h))->error;
    } else if (h->nlmsg_type == RTM_NEWLINK) {
        *index = ((struct ifinfomsg *)NLMSG_DATA(h))->ifi_index;
    }
    return h->nlmsg_type;
}

/**
 * Open a socket of NETLINK_ROUTE into `*out`, keep its port id in `port`, and
 * check the port ids of it and of two more sockets. Returns 0, or 1 after the
 * failure is printed.
 */
static int route_socket(int *out)
{
    struct sockaddr_nl me;
    socklen_t len = sizeof me;
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    int second;

    *out = fd;
    if (fd < 0) {
        return fail("socket of NETLINK_ROUTE");
    }
    if (getsockname(fd, (struct sockaddr *)&me, &len) != 0 || len != sizeof me ||
        me.nl_family != AF_NETLINK || me.nl_pad != 0 || me.nl_pid != (unsigned int)getpid()) {
        return fail("the first socket's port id is not the process ID");
    }
    held();
    port = me.nl_pid;
    second = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
    memset(&me, 0, sizeof me);
    me.nl_family = AF_NETLINK;
    if (second < 0 || bind(second, (struct sockaddr *)&me, sizeof me) != 0 ||
        getsockname(second, (struct sockaddr *)&me, &len) != 0 || me.nl_pid == port) {
        return fail("a second socket's port id");
    }
    held();
    close(second);
    second = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    memset(&me, 0, sizeof me);
    me.nl_family = AF_NETLINK;
    me.nl_pid = port;
    if (bind(second, (struct sockaddr *)&me, sizeof me) != -1 || errno != EADDRINUSE) {
        return fail("a bind of a port id another socket has was not EADDRINUSE");
    }
    held();
    close(second);
    return 0;
}

/**
 * Check the dumps of links, addresses and routes, and the answers to
 * RTM_GETLINK and RTM_NEWADDR that are not dumps. Returns 0, or 1 after the
 * failure is printed.
 */
static int requests(int fd)
{
    struct ifinfomsg ifi;
    struct ifaddrmsg ifa;
    int index = -1, error = 0, links;

    links = dump(fd, RTM_GETLINK, 1);
    if (links == 0) {
        return fail("no link");
    }
    if (links < 0) {
        return 1;
    }
    held();
    if (dump(fd, RTM_GETADDR, 2) < 0) {
        return 1;
    }
    held();
    if (dump(fd, RTM_GETROUTE, 3) < 1) {
        return fail("no route");
    }
    held();
    memset(&ifi, 0, sizeof ifi);
    ifi.ifi_index = first_index;
    if (request(fd, RTM_GETLINK, 0, 4, &ifi, sizeof ifi) != 0 ||
        one_answer(fd, &index, &error) != RTM_NEWLINK || index != first_index) {
        return fail("RTM_GETLINK of the first link");
    }
    held();
    ifi.ifi_index = 9999;
    if (request(fd, RTM_GETLINK, 0, 5, &ifi, sizeof ifi) != 0 ||
        one_answer(fd, &index, &error) != NLMSG_ERROR || error != -ENODEV) {
        return fail("RTM_GETLINK of no link was not ENODEV");
    }
    held();
    memset(&ifa, 0, sizeof ifa);
    ifa.ifa_family = AF_INET;
    ifa.ifa_index = (unsigned int)first_index;
    if (request(fd, RTM_NEWADDR, NLM_F_CREATE, 6, &ifa, sizeof ifa) != 0 ||
        one_answer(fd, &index, &error) != NLMSG_ERROR || error != -EINVAL) {
        return fail("RTM_NEWADDR with no address was not EINVAL");
    }
    held();
    return 0;
}

/**
 * Append a four-byte attribute of `type` with the value at `value` to `body`
 * of `*len` bytes, and add its eight bytes to `*len`.
 */
static void attr32(char *body, int *len, int type, const void *value)
{
    struct rtattr a;

    a.rta_len = 8;
    a.rta_type = (unsigned short)type;
    memcpy(body + *len, &a, sizeof a);
    memcpy(body + *len + 4, value, 4);
    *len += 8;
}

/**
 * Send a change with NLM_F_ACK and read its acknowledgement. Returns its
 * error, 0 when the change was made, or 1 for no acknowledgement. errno is
 * set to the acknowledgement's error, or 0, so that `fail` prints the answer.
 */
static int ask(int fd, int type, int flags, unsigned int seq, const void *body, int len)
{
    int index = -1, error = 1;

    if (request(fd, type, NLM_F_ACK | flags, seq, body, len) != 0 ||
        one_answer(fd, &index, &error) != NLMSG_ERROR) {
        return 1;
    }
    errno = error < 0 ? -error : 0;
    return error;
}

/**
 * Check that the first link's address is EEXIST to add again, and is taken
 * away and given back through RTM_DELADDR and RTM_NEWADDR with SLIRP's
 * prefix. Returns 0, or 1 after the failure is printed.
 */
static int address_changes(int fd)
{
    char body[48];
    struct ifaddrmsg ifa;
    struct in_addr addr, now;
    int len;

    if (ioctl_addr(first_index, &addr) != 0) {
        return fail("SIOCGIFADDR of the first link");
    }
    memset(&ifa, 0, sizeof ifa);
    ifa.ifa_family = AF_INET;
    ifa.ifa_prefixlen = 24;
    ifa.ifa_index = (unsigned int)first_index;
    memcpy(body, &ifa, sizeof ifa);
    len = sizeof ifa;
    attr32(body, &len, IFA_LOCAL, &addr);
    if (ask(fd, RTM_NEWADDR, NLM_F_CREATE | NLM_F_EXCL, 40, body, len) != -EEXIST) {
        return fail("RTM_NEWADDR of the address the link has was not EEXIST");
    }
    held();
    if (ask(fd, RTM_DELADDR, 0, 41, body, len) != 0 || ioctl_addr(first_index, &now) != 0 ||
        now.s_addr != 0) {
        return fail("RTM_DELADDR did not take the address away");
    }
    held();
    if (ask(fd, RTM_NEWADDR, NLM_F_CREATE | NLM_F_EXCL, 42, body, len) != 0 ||
        ioctl_addr(first_index, &now) != 0 || now.s_addr != addr.s_addr) {
        return fail("RTM_NEWADDR did not give the address back");
    }
    held();
    return 0;
}

/**
 * Check the default route through SLIRP's gateway, which the address's
 * removal may have taken with it, set and removed and set again; the link set
 * up; and the errors for an MTU of 9000 and for forgetting a neighbour the
 * cache does not have. Returns 0, or 1 after the failure is printed.
 */
static int route_changes(int fd)
{
    char body[48];
    unsigned char rtm[12]; /* struct rtmsg: family, dst_len, src_len, tos, table, ... type */
    unsigned char ndm[12]; /* struct ndmsg: family, padding, then the index at 4 */
    struct ifinfomsg ifi;
    struct in_addr gw, net, other;
    unsigned int oif = (unsigned int)first_index, mtu = 9000;
    int len;

    gw.s_addr = inet_addr("10.0.2.2");
    net.s_addr = inet_addr("10.99.0.0");
    other.s_addr = inet_addr("10.0.2.99");
    memset(rtm, 0, sizeof rtm);
    rtm[0] = AF_INET;
    rtm[4] = RT_TABLE_MAIN;
    rtm[7] = RTN_UNICAST;
    memcpy(body, rtm, sizeof rtm);
    len = sizeof rtm;
    attr32(body, &len, RTA_GATEWAY, &gw);
    attr32(body, &len, RTA_OIF, &oif);
    (void)ask(fd, RTM_DELROUTE, 0, 43, body, len);
    if (ask(fd, RTM_NEWROUTE, NLM_F_CREATE | NLM_F_EXCL, 44, body, len) != 0 ||
        ask(fd, RTM_NEWROUTE, NLM_F_CREATE | NLM_F_EXCL, 45, body, len) != -EEXIST) {
        return fail("RTM_NEWROUTE of the default route, then again");
    }
    held();
    if (ask(fd, RTM_DELROUTE, 0, 46, body, len) != 0 ||
        ask(fd, RTM_DELROUTE, 0, 47, body, len) != -ESRCH ||
        ask(fd, RTM_NEWROUTE, NLM_F_CREATE | NLM_F_EXCL, 48, body, len) != 0) {
        return fail("RTM_DELROUTE of the default route, then again, and setting it back");
    }
    held();
    (void)net;
    memset(&ifi, 0, sizeof ifi);
    ifi.ifi_index = first_index;
    ifi.ifi_flags = IFF_UP;
    ifi.ifi_change = IFF_UP;
    memcpy(body, &ifi, sizeof ifi);
    len = sizeof ifi;
    if (ask(fd, RTM_NEWLINK, 0, 50, body, len) != 0) {
        return fail("RTM_NEWLINK setting the link up");
    }
    held();
    attr32(body, &len, IFLA_MTU, &mtu);
    if (ask(fd, RTM_NEWLINK, 0, 51, body, len) != -EINVAL) {
        return fail("RTM_NEWLINK of an MTU of 9000 was not EINVAL");
    }
    held();
    memset(ndm, 0, sizeof ndm);
    ndm[0] = AF_INET;
    memcpy(ndm + 4, &first_index, 4);
    memcpy(body, ndm, sizeof ndm);
    len = sizeof ndm;
    attr32(body, &len, NDA_DST, &other);
    if (ask(fd, RTM_DELNEIGH, 0, 52, body, len) != -ENOENT) {
        return fail("RTM_DELNEIGH of a neighbour the cache does not have was not ENOENT");
    }
    held();
    return 0;
}

static int route_get(int fd, unsigned int seq, struct in_addr dst, int *type, int *oif,
                     unsigned int *gw);

/**
 * Return 1 when the first 4 KiB of the file `path` contain `needle`, and 0
 * otherwise.
 */
static int file_has(const char *path, const char *needle)
{
    static char text[4096];
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;

    if (f) {
        fclose(f);
    }
    text[n] = '\0';
    return strstr(text, needle) != NULL;
}

/**
 * Check a route to another network through SLIRP's gateway, added, listed in
 * /proc/net/route, and removed; one through a gateway no subnet has, which is
 * ENETUNREACH; and a route on the link to that network, which `ip route get`
 * takes. Returns 0, or 1 after the failure is printed.
 */
static int table_routes(int fd)
{
    char body[64];
    unsigned char rtm[12] = {AF_INET, 16, 0, 0, RT_TABLE_MAIN, 0, 0, RTN_UNICAST};
    struct in_addr gw, net, far;
    unsigned int oif = (unsigned int)first_index, got_gw;
    int len = sizeof rtm, type = -1, got_oif = -1;

    gw.s_addr = inet_addr("10.0.2.2");
    net.s_addr = inet_addr("10.99.0.0");
    far.s_addr = inet_addr("10.77.0.1");
    memcpy(body, rtm, sizeof rtm);
    attr32(body, &len, RTA_DST, &net);
    attr32(body, &len, RTA_GATEWAY, &gw);
    attr32(body, &len, RTA_OIF, &oif);
    if (ask(fd, RTM_NEWROUTE, NLM_F_CREATE | NLM_F_EXCL, 60, body, len) != 0 ||
        ask(fd, RTM_NEWROUTE, NLM_F_CREATE | NLM_F_EXCL, 61, body, len) != -EEXIST) {
        return fail("RTM_NEWROUTE of another network through the gateway, then again");
    }
    held();
    if (!file_has("/proc/net/route", "\t0000630A\t0202000A\t0003\t0\t0\t0\t0000FFFF")) {
        return fail("/proc/net/route did not list the route added");
    }
    held();
    if (ask(fd, RTM_DELROUTE, 0, 62, body, len) != 0 ||
        ask(fd, RTM_DELROUTE, 0, 63, body, len) != -ESRCH) {
        return fail("RTM_DELROUTE of the route added, then again");
    }
    held();
    len = sizeof rtm;
    attr32(body, &len, RTA_DST, &net);
    attr32(body, &len, RTA_GATEWAY, &far);
    attr32(body, &len, RTA_OIF, &oif);
    if (ask(fd, RTM_NEWROUTE, NLM_F_CREATE | NLM_F_EXCL, 64, body, len) != -ENETUNREACH) {
        return fail("RTM_NEWROUTE through a gateway no subnet has was not ENETUNREACH");
    }
    held();
    len = sizeof rtm;
    attr32(body, &len, RTA_DST, &net);
    attr32(body, &len, RTA_OIF, &oif);
    far.s_addr = inet_addr("10.99.1.1");
    if (ask(fd, RTM_NEWROUTE, NLM_F_CREATE | NLM_F_EXCL, 72, body, len) != 0 ||
        route_get(fd, 73, far, &type, &got_oif, &got_gw) != 0 || type != RTN_UNICAST ||
        got_gw != 0 || ask(fd, RTM_DELROUTE, 0, 74, body, len) != 0) {
        return fail("a route on the link to another network, which ip route get takes");
    }
    held();
    return 0;
}

/**
 * Check a second address and a permanent neighbour, each made and taken away,
 * then the routes of `table_routes`. Returns 0, or 1 after the failure is
 * printed.
 */
static int table_changes(int fd)
{
    char body[64];
    struct ifaddrmsg ifa;
    unsigned char ndm[12] = {AF_INET};
    const unsigned char mac[8] = {0x02, 0, 0, 0, 0, 0xab};
    struct in_addr second, neigh;
    int len, type = -1, oif = -1;
    unsigned int gw;
    unsigned short state = NUD_PERMANENT;

    second.s_addr = inet_addr("10.0.2.77");
    neigh.s_addr = inet_addr("10.0.2.99");
    memset(&ifa, 0, sizeof ifa);
    ifa.ifa_family = AF_INET;
    ifa.ifa_prefixlen = 24;
    ifa.ifa_index = (unsigned int)first_index;
    memcpy(body, &ifa, sizeof ifa);
    len = sizeof ifa;
    attr32(body, &len, IFA_LOCAL, &second);
    if (ask(fd, RTM_NEWADDR, NLM_F_CREATE | NLM_F_EXCL, 65, body, len) != 0 ||
        route_get(fd, 66, second, &type, &oif, &gw) != 0 || type != RTN_LOCAL ||
        ask(fd, RTM_DELADDR, 0, 67, body, len) != 0 ||
        ask(fd, RTM_DELADDR, 0, 68, body, len) != -EADDRNOTAVAIL) {
        return fail("a second address added, a local route, and taken away");
    }
    held();
    memcpy(ndm + 4, &first_index, 4);
    memcpy(ndm + 8, &state, 2);
    memcpy(body, ndm, sizeof ndm);
    len = sizeof ndm;
    attr32(body, &len, NDA_DST, &neigh);
    {
        struct rtattr a;

        a.rta_len = 10;
        a.rta_type = NDA_LLADDR;
        memcpy(body + len, &a, sizeof a);
        memcpy(body + len + 4, mac, 8);
        len += 12;
    }
    if (ask(fd, RTM_NEWNEIGH, NLM_F_CREATE, 69, body, len) != 0 ||
        !file_has("/proc/net/arp", "0x6         02:00:00:00:00:ab")) {
        return fail("a neighbour set by hand as permanent, in /proc/net/arp with 0x6 "
                    "in lowercase");
    }
    held();
    len = sizeof ndm;
    attr32(body, &len, NDA_DST, &neigh);
    if (ask(fd, RTM_DELNEIGH, 0, 70, body, len) != 0 ||
        ask(fd, RTM_DELNEIGH, 0, 71, body, len) != -ENOENT) {
        return fail("forgetting the permanent neighbour, then again");
    }
    held();
    return table_routes(fd);
}

/**
 * Send `ip route get ADDRESS`'s request, an rtmsg and the RTA_DST of `dst`,
 * and read the answer's route type, output interface and gateway, 0 for
 * none, into `*type`, `*oif` and `*gw`. Returns 0, or -1 for no route
 * message.
 */
static int route_get(int fd, unsigned int seq, struct in_addr dst, int *type, int *oif,
                     unsigned int *gw)
{
    struct {
        unsigned char rtm[12]; /* struct rtmsg: family, dst_len, then zeros */
        struct rtattr a;
        struct in_addr dst;
    } req;
    char buf[4096] __attribute__((aligned(4)));
    struct nlmsghdr *h;
    struct rtattr *a;
    int n, left;

    memset(&req, 0, sizeof req);
    req.rtm[0] = AF_INET;
    req.rtm[1] = 32;
    req.a.rta_len = sizeof(req.a) + sizeof(req.dst);
    req.a.rta_type = RTA_DST;
    req.dst = dst;
    if (request(fd, RTM_GETROUTE, 0, seq, &req, sizeof req) != 0) {
        return -1;
    }
    n = (int)recv(fd, buf, sizeof buf, 0);
    h = (struct nlmsghdr *)buf;
    if (n < (int)sizeof(*h) || !NLMSG_OK(h, n) || h->nlmsg_type != RTM_NEWROUTE) {
        return -1;
    }
    *type = ((unsigned char *)NLMSG_DATA(h))[7];
    *oif = 0;
    *gw = 0;
    left = (int)h->nlmsg_len - NLMSG_HDRLEN - 12;
    for (a = (struct rtattr *)((char *)NLMSG_DATA(h) + 12); RTA_OK(a, left);
         a = RTA_NEXT(a, left)) {
        if (a->rta_type == RTA_OIF) {
            memcpy(oif, RTA_DATA(a), sizeof(*oif));
        }
        if (a->rta_type == RTA_GATEWAY) {
            memcpy(gw, RTA_DATA(a), sizeof(*gw));
        }
    }
    return 0;
}

/**
 * Check `ip route get` of the first link's address and of an address it
 * reaches, and the dumps of `ip rule` and `tc qdisc`. Returns 0, also when
 * the first link has no address, or 1 after the failure is printed.
 */
static int lookups(int fd)
{
    struct in_addr own, other;
    int type = -1, oif = -1;
    unsigned int gw;

    if (ioctl_addr(first_index, &own) != 0) {
        skip(4, "the first link has no address to look up");
        return 0;
    }
    if (route_get(fd, 20, own, &type, &oif, &gw) != 0 || type != RTN_LOCAL ||
        oif != (int)if_nametoindex("lo")) {
        return fail("ip route get of the machine's own address was not a local route on lo");
    }
    held();
    other.s_addr = own.s_addr ^ htonl(1);
    if (route_get(fd, 21, other, &type, &oif, &gw) != 0 || type != RTN_UNICAST ||
        oif != first_index) {
        return fail("ip route get of a neighbour was not a unicast route on the first link");
    }
    held();
    if (dump(fd, RTM_GETRULE, 22) != 3) {
        return fail("ip rule did not list Linux's three rules");
    }
    held();
    if (dump(fd, RTM_GETQDISC, 23) < 1) {
        return fail("tc qdisc listed nothing");
    }
    held();
    return 0;
}

/**
 * Check a dump request sent in a buffer longer than it, as iproute2's ip
 * sends RTM_GETROUTE: the zeros after it are no message, and the dump is
 * answered to its NLMSG_DONE. Returns 0, or 1 after the failure is printed.
 */
static int padded(int fd)
{
    union {
        struct nlmsghdr h;
        char bytes[NLMSG_LENGTH(sizeof(struct rtgenmsg)) + 128];
    } req;
    struct rtgenmsg *g = NLMSG_DATA(&req.h);
    char buf[8192] __attribute__((aligned(4)));
    struct nlmsghdr *h;
    int n, routes = 0;

    memset(req.bytes, 0, sizeof req.bytes);
    req.h.nlmsg_len = NLMSG_LENGTH(sizeof *g);
    req.h.nlmsg_type = RTM_GETROUTE;
    req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.h.nlmsg_seq = 8;
    g->rtgen_family = AF_INET;
    if (send(fd, req.bytes, sizeof req.bytes, 0) != (ssize_t)sizeof req.bytes) {
        return fail("a dump request with zeros after it was refused");
    }
    for (;;) {
        n = (int)recv(fd, buf, sizeof buf, 0);
        if (n <= 0) {
            return fail("the dump of a request with zeros after it");
        }
        for (h = (struct nlmsghdr *)buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE && routes == 0) {
                return fail("the dump of a request with zeros after it");
            }
            if (h->nlmsg_type == NLMSG_DONE) {
                held();
                return 0;
            }
            routes += h->nlmsg_type == RTM_NEWROUTE && h->nlmsg_seq == 8;
        }
    }
}

/**
 * Check the length of an answer before it is read, and an answer cut short.
 * Returns 0, or 1 after the failure is printed.
 */
static int truncation(int fd)
{
    struct ifinfomsg ifi;
    struct msghdr msg;
    struct iovec iov;
    char buf[16];
    ssize_t whole, n;

    memset(&ifi, 0, sizeof ifi);
    ifi.ifi_index = first_index;
    if (request(fd, RTM_GETLINK, 0, 7, &ifi, sizeof ifi) != 0) {
        return fail("RTM_GETLINK for the length of its answer");
    }
    memset(&msg, 0, sizeof msg);
    iov.iov_base = NULL;
    iov.iov_len = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    whole = recvmsg(fd, &msg, MSG_PEEK | MSG_TRUNC);
    if (whole <= (ssize_t)sizeof buf) {
        return fail("MSG_PEEK and MSG_TRUNC did not give the answer's length");
    }
    held();
    iov.iov_base = buf;
    iov.iov_len = sizeof buf;
    n = recvmsg(fd, &msg, 0);
    if (n != (ssize_t)sizeof buf || !(msg.msg_flags & MSG_TRUNC)) {
        return fail("an answer longer than the buffer was not cut with MSG_TRUNC");
    }
    held();
    return 0;
}

/**
 * Check if_nametoindex of the first link, and SIOCGIFCONF without and with a
 * buffer. Returns 0, or 1 after the failure is printed.
 */
static int interfaces(void)
{
    struct ifconf ifc;
    struct ifreq reqs[8];
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int want;

    if (if_nametoindex(first_name) != (unsigned int)first_index) {
        return fail("if_nametoindex of the first link");
    }
    held();
    memset(&ifc, 0, sizeof ifc);
    if (s < 0 || ioctl(s, SIOCGIFCONF, &ifc) != 0 || ifc.ifc_len <= 0 ||
        ifc.ifc_len % (int)sizeof(struct ifreq) != 0) {
        return fail("SIOCGIFCONF with no buffer");
    }
    held();
    want = ifc.ifc_len;
    ifc.ifc_len = sizeof reqs;
    ifc.ifc_req = reqs;
    if (ioctl(s, SIOCGIFCONF, &ifc) != 0 || ifc.ifc_len != want ||
        reqs[0].ifr_addr.sa_family != AF_INET) {
        return fail("SIOCGIFCONF with a buffer");
    }
    held();
    close(s);
    return 0;
}

/**
 * Check that a NETLINK_KOBJECT_UEVENT socket hears nothing, and that an unknown
 * netlink protocol fails with EPROTONOSUPPORT. Returns 0, or 1 after the
 * failure is printed.
 */
static int uevents(void)
{
    struct sockaddr_nl sa;
    struct pollfd pf;
    char c;
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);

    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1;
    if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        return fail("a socket of NETLINK_KOBJECT_UEVENT");
    }
    pf.fd = fd;
    pf.events = POLLIN;
    if (poll(&pf, 1, 0) != 0 || recv(fd, &c, 1, 0) != -1 || errno != EAGAIN) {
        return fail("a socket of NETLINK_KOBJECT_UEVENT heard something");
    }
    held();
    close(fd);
    if (socket(AF_NETLINK, SOCK_RAW, 99) != -1 || errno != EPROTONOSUPPORT) {
        return fail("a netlink protocol the kernel has not was not EPROTONOSUPPORT");
    }
    held();
    return 0;
}

/** Whether the IPv4 address dump reports `local` and `peer` on `index`
 * with a /32 prefix. Drain the dump to NLMSG_DONE. */
static int tun_pair(int fd, int index, struct in_addr local, struct in_addr peer)
{
    struct rtgenmsg g = {AF_INET};
    char buf[8192] __attribute__((aligned(4)));
    int found = 0;

    if (request(fd, RTM_GETADDR, NLM_F_DUMP, 81, &g, sizeof(g)) != 0) {
        return 0;
    }
    for (;;) {
        struct nlmsghdr *h;
        int n = (int)recv(fd, buf, sizeof(buf), 0);

        if (n <= 0) {
            return 0;
        }
        for (h = (struct nlmsghdr *)buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
            struct ifaddrmsg *ifa = NLMSG_DATA(h);
            struct in_addr got_local = {0}, got_peer = {0};
            int left = PAYLOAD(h, struct ifaddrmsg);
            struct rtattr *a;

            if (h->nlmsg_type == NLMSG_DONE) {
                return found;
            }
            if (h->nlmsg_type == NLMSG_ERROR) {
                return 0;
            }
            if (h->nlmsg_type != RTM_NEWADDR || left < 0 || ifa->ifa_index != (unsigned int)index ||
                ifa->ifa_prefixlen != 32) {
                continue;
            }
            for (a = FIRST_RTA(ifa); RTA_OK(a, left); a = RTA_NEXT(a, left)) {
                if (a->rta_len != sizeof(*a) + sizeof(local)) {
                    continue;
                }
                if (a->rta_type == IFA_LOCAL) {
                    memcpy(&got_local, RTA_DATA(a), sizeof(got_local));
                }
                if (a->rta_type == IFA_ADDRESS) {
                    memcpy(&got_peer, RTA_DATA(a), sizeof(got_peer));
                }
            }
            found |= got_local.s_addr == local.s_addr && got_peer.s_addr == peer.s_addr;
        }
    }
}

/** Ask for a route to `peer`. Is it a direct unicast route on `index`? */
static int tun_route(int fd, unsigned int seq, struct in_addr peer, int index)
{
    int type, oif;
    unsigned int gw;

    return route_get(fd, seq, peer, &type, &oif, &gw) == 0 && type == RTN_UNICAST && oif == index &&
           gw == 0;
}

/** Send an IPv4 datagram to the TUN's reported packet socket address and
 * match it on the TUN descriptor. */
static int tun_packet(int tun, int index)
{
    const unsigned char datagram[20] = {0x45, 0, 0,   20, 0, 0, 0,   0,  64,  17,
                                        0,    0, 192, 0,  2, 1, 198, 51, 100, 3};
    unsigned char received[1500];
    struct sockaddr_ll sa;
    socklen_t len = sizeof(sa);
    int packet = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_TYPE_IP));
    int ok = 0;

    if (packet < 0) {
        return 0;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sll_family = AF_PACKET;
    sa.sll_protocol = htons(ETH_TYPE_IP);
    sa.sll_ifindex = index;
    if (bind(packet, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
        getsockname(packet, (struct sockaddr *)&sa, &len) == 0 && sa.sll_halen == 0 &&
        sa.sll_hatype == ARPHRD_NONE &&
        sendto(packet, datagram, sizeof(datagram), 0, (struct sockaddr *)&sa, sizeof(sa)) ==
            sizeof(datagram)) {
        ssize_t n;

        while ((n = read(tun, received, sizeof(received))) > 0) {
            if (n == sizeof(datagram) && memcmp(received, datagram, sizeof(datagram)) == 0) {
                ok = 1;
                break;
            }
        }
    }
    close(packet);
    return ok;
}

/** Read the TUN peer with SIOCGIFDSTADDR and replace it with SIOCSIFDSTADDR.
 * Return 0 on success or -1 if an ioctl fails or the old peer differs. */
static int tun_replace_peer(int s, struct ifreq *ifr, struct in_addr old, struct in_addr next)
{
    struct sockaddr_in sin;

    if (ioctl(s, SIOCGIFDSTADDR, ifr) != 0) {
        return -1;
    }
    memcpy(&sin, &ifr->ifr_dstaddr, sizeof(sin));
    if (sin.sin_family != AF_INET || sin.sin_addr.s_addr != old.s_addr) {
        return -1;
    }
    sin.sin_addr = next;
    memcpy(&ifr->ifr_dstaddr, &sin, sizeof(sin));
    return ioctl(s, SIOCSIFDSTADDR, ifr);
}

/** Check a TUN's distinct local and peer IPv4 addresses, the peer route
 * through an ioctl change and deletion, and a packet socket send. */
static int tun_changes(int fd)
{
    struct ifreq ifr;
    struct ifaddrmsg ifa = {AF_INET, 32, 0, 0, 0};
    struct in_addr local = {inet_addr("192.0.2.1")};
    struct in_addr peer = {inet_addr("198.51.100.2")};
    struct in_addr next = {inet_addr("198.51.100.3")};
    char body[sizeof(ifa) + 16];
    int tun = -1, s = -1, index, len, rc;
    const char *failure = "make tunprob and set it up";

    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "tunprob");
    ifr.ifr_flags = TUN_IFF_TUN | TUN_IFF_NO_PI;
    tun = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (tun < 0 || s < 0 || ioctl(tun, TUNSETIFF, &ifr) != 0 || ioctl(s, SIOCGIFINDEX, &ifr) != 0) {
        goto out;
    }
    index = ifr.ifr_ifindex;
    if (ioctl(s, SIOCGIFFLAGS, &ifr) != 0) {
        goto out;
    }
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(s, SIOCSIFFLAGS, &ifr) != 0) {
        goto out;
    }
    held();

    failure = "RTM_NEWADDR or RTM_GETADDR did not give the TUN its peer";
    ifa.ifa_index = (unsigned int)index;
    memcpy(body, &ifa, sizeof(ifa));
    len = sizeof(ifa);
    attr32(body, &len, IFA_LOCAL, &local);
    attr32(body, &len, IFA_ADDRESS, &peer);
    if (ask(fd, RTM_NEWADDR, NLM_F_CREATE | NLM_F_EXCL, 80, body, len) != 0 ||
        !tun_pair(fd, index, local, peer)) {
        goto out;
    }
    held();

    failure = "the peer has no direct IPv4 route through the TUN";
    if (!tun_route(fd, 82, peer, index)) {
        goto out;
    }
    held();

    failure = "the destination ioctls did not read and replace the peer and its route";
    if (tun_replace_peer(s, &ifr, peer, next) != 0 || !tun_pair(fd, index, local, next) ||
        tun_route(fd, 83, peer, index) || !tun_route(fd, 84, next, index)) {
        goto out;
    }
    held();

    failure = "the TUN did not transmit the packet socket's datagram with sll_halen 0";
    if (!tun_packet(tun, index)) {
        goto out;
    }
    held();

    failure = "could not give the same local address a second peer";
    len = sizeof(ifa);
    attr32(body, &len, IFA_LOCAL, &local);
    attr32(body, &len, IFA_ADDRESS, &peer);
    if (ask(fd, RTM_NEWADDR, NLM_F_CREATE | NLM_F_EXCL, 85, body, len) != 0 ||
        !tun_route(fd, 86, peer, index)) {
        goto out;
    }
    held();

    failure = "deleting the first peer did not remove only its route";
    len = sizeof(ifa);
    attr32(body, &len, IFA_LOCAL, &local);
    attr32(body, &len, IFA_ADDRESS, &next);
    if (ask(fd, RTM_DELADDR, 0, 87, body, len) != 0 || tun_route(fd, 88, next, index) ||
        !tun_route(fd, 89, peer, index) || !tun_pair(fd, index, local, peer)) {
        goto out;
    }
    held();
    failure = NULL;
out:
    rc = failure ? fail(failure) : 0;
    if (tun >= 0) {
        close(tun);
    }
    if (s >= 0) {
        close(s);
    }
    return rc;
}

/** Plan each assertion of `ids`, `n` of them. */
static void plan_rows(const char *const *ids, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        plan_row(ids[i]);
    }
}

#define PLAN(ids) plan_rows(ids, sizeof(ids) / sizeof(ids[0]))

/** Plan each assertion of `ids`, `n` of them, or with `left_out` report each
 * not run. */
static void plan_changes(const char *const *ids, unsigned n, int left_out)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (left_out) {
            report_notrun(ids[i], "a native run leaves out the changes");
        } else {
            plan_row(ids[i]);
        }
    }
}

#define PLAN_CHANGES(ids) plan_changes(ids, sizeof(ids) / sizeof(ids[0]), probe_native())

/**
 * Begin the report and plan its assertions in the order they are checked; a
 * native run reports those of the changes not run. `envp` is the
 * environment, which gives the case.
 */
static void plan(char **envp)
{
    static const char *const before[] = {"socket-port-pid", "second-port",    "bind-eaddrinuse",
                                         "dump-links",      "dump-addrs",     "dump-routes",
                                         "getlink-one",     "getlink-enodev", "newaddr-einval"};
    static const char *const changes[] = {"addr-eexist",       "addr-del",          "addr-restore",
                                          "default-route-add", "default-route-del", "link-up",
                                          "mtu-einval",        "delneigh-enoent"};
    static const char *const lookup_ids[] = {"route-get-local", "route-get-unicast", "rule-dump",
                                             "qdisc-dump"};
    static const char *const table[] = {"second-addr",       "neigh-permanent", "neigh-forget",
                                        "route-add",         "proc-net-route",  "route-del",
                                        "route-enetunreach", "route-on-link"};
    static const char *const after[] = {"padded-dump",   "peek-trunc",     "cut-trunc",
                                        "nametoindex",   "ifconf-length",  "ifconf-buffer",
                                        "uevent-silent", "eprotonosupport"};
    static const char *const tun[] = {"tun-up",         "tun-peer",   "tun-route",
                                      "tun-peer-ioctl", "tun-packet", "tun-second-peer",
                                      "tun-peer-delete"};

    report_begin(report_case(envp, "guest.default.netlinkmusl"));
    PLAN(before);
    PLAN_CHANGES(changes);
    PLAN(lookup_ids);
    PLAN_CHANGES(table);
    PLAN(after);
    PLAN_CHANGES(tun);
}

int main(int argc, char **argv, char **envp)
{
    int changes;
    int fd;

    probe_args(argc, argv);
    plan(envp);
    changes = !probe_native();
    if (route_socket(&fd) || requests(fd) ||
        (changes && (address_changes(fd) || route_changes(fd))) || lookups(fd) ||
        (changes && table_changes(fd)) || padded(fd) || truncation(fd) || interfaces() ||
        uevents() || (changes && tun_changes(fd))) {
        return 1;
    }
    close(fd);
    report_end();
    printf("NETLINKMUSL OK\n");
    return 0;
}
