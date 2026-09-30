/**
 * netlink: the NETLINK_ROUTE dumps of the machine's links, addresses and
 * routes, the request of one link, the refusals, the port ids a bind
 * takes, the interface ioctls on an AF_UNIX socket and SIOCGIFCONF, and a
 * NETLINK_KOBJECT_UEVENT socket that hears nothing.
 *
 * A socket's first port id is the process ID and a second socket's is
 * another; a bind of a port id another socket has is EADDRINUSE. Each
 * dump's messages repeat the request's sequence number and the socket's
 * port id, have NLM_F_MULTI, and end with NLMSG_DONE: every link has a
 * name, every address is the one SIOCGIFADDR gives for its interface, and
 * every interface with an address has a route to its subnet. RTM_GETLINK of
 * the first link's index answers with that link alone, of no link with
 * ENODEV, and RTM_NEWADDR, a change, with EOPNOTSUPP. if_nametoindex, which
 * asks an AF_UNIX socket, finds the first link, and SIOCGIFCONF lists the
 * interfaces with an address.
 *
 * Prints "NETLINKMUSL OK" on success and "NETLINKMUSL FAIL (detail)"
 * otherwise.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* Linux's netlink ABI, which the musl build has no uapi headers for:
 * <linux/netlink.h> and <linux/rtnetlink.h>, the parts used here. */
#define NETLINK_ROUTE          0
#define NETLINK_KOBJECT_UEVENT 15
#define NLM_F_REQUEST          0x001
#define NLM_F_MULTI            0x002
#define NLM_F_CREATE           0x400
#define NLM_F_DUMP             0x300
#define NLMSG_ERROR            2
#define NLMSG_DONE             3
#define RTM_NEWLINK            16
#define RTM_GETLINK            18
#define RTM_NEWADDR            20
#define RTM_GETADDR            22
#define RTM_NEWROUTE           24
#define RTM_GETROUTE           26
#define IFLA_IFNAME            3
#define IFA_LOCAL              2

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

struct nlmsgerr {
    int error;
    struct nlmsghdr msg;
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
#define NLMSG_OK(h, len)                                                              \
    ((len) >= (int)sizeof(struct nlmsghdr) && (h)->nlmsg_len >= sizeof(struct nlmsghdr) && \
     (int)(h)->nlmsg_len <= (len))
#define NLMSG_NEXT(h, len)                                   \
    ((len) -= (int)NLMSG_ALIGN((h)->nlmsg_len),              \
     (struct nlmsghdr *)((char *)(h) + NLMSG_ALIGN((h)->nlmsg_len)))
#define RTA_ALIGN(len)  (((len) + 3u) & ~3u)
#define RTA_OK(a, len)  ((len) >= (int)sizeof(struct rtattr) && (a)->rta_len >= sizeof(struct rtattr) && \
                        (int)(a)->rta_len <= (len))
#define RTA_NEXT(a, len) \
    ((len) -= (int)RTA_ALIGN((a)->rta_len), (struct rtattr *)((char *)(a) + RTA_ALIGN((a)->rta_len)))
#define RTA_DATA(a)       ((void *)((char *)(a) + RTA_ALIGN(sizeof(struct rtattr))))
#define PAYLOAD(h, body)  ((int)(h)->nlmsg_len - NLMSG_HDRLEN - (int)NLMSG_ALIGN(sizeof(body)))
#define FIRST_RTA(p)      ((struct rtattr *)((char *)(p) + NLMSG_ALIGN(sizeof(*(p)))))

static int fail(const char *what)
{
    printf("NETLINKMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/* `fail` for a function whose answer is a count or -1. */
static int failed(const char *what)
{
    fail(what);
    return -1;
}

/* What the dumps found. */
static int first_index;
static char first_name[IF_NAMESIZE];
static unsigned int port;

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

/* The address SIOCGIFADDR gives for the interface of index `index`. */
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
 * Read a dump of `type` to its NLMSG_DONE and check each message: the
 * sequence number, the port id, NLM_F_MULTI, and what the type's check
 * says. Returns the count of messages before the end, or -1.
 */
static int dump(int fd, int type, unsigned int seq)
{
    char buf[8192] __attribute__((aligned(4)));
    struct rtgenmsg g;
    int n, msgs = 0;

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
            if (h->nlmsg_seq != seq || h->nlmsg_pid != port || !(h->nlmsg_flags & NLM_F_MULTI)) {
                return failed("a dump's message without its sequence, port id or NLM_F_MULTI")
                           ? -1
                           : -1;
            }
            if (h->nlmsg_type == NLMSG_DONE) {
                return msgs;
            }
            msgs++;
            if (h->nlmsg_type == RTM_NEWLINK) {
                struct ifinfomsg *ifi = NLMSG_DATA(h);
                int l = PAYLOAD(h, struct ifinfomsg);
                struct rtattr *a;
                const char *name = NULL;

                for (a = FIRST_RTA(ifi); RTA_OK(a, l); a = RTA_NEXT(a, l)) {
                    if (a->rta_type == IFLA_IFNAME) {
                        name = RTA_DATA(a);
                    }
                }
                if (!name) {
                    return failed("a link without a name");
                }
                if (msgs == 1) {
                    first_index = ifi->ifi_index;
                    snprintf(first_name, sizeof first_name, "%s", name);
                }
            } else if (h->nlmsg_type == RTM_NEWADDR) {
                struct ifaddrmsg *ifa = NLMSG_DATA(h);
                int l = PAYLOAD(h, struct ifaddrmsg);
                struct rtattr *a;
                struct in_addr want;

                if (ioctl_addr((int)ifa->ifa_index, &want) != 0) {
                    return failed("SIOCGIFADDR of an address's interface");
                }
                for (a = FIRST_RTA(ifa); RTA_OK(a, l); a = RTA_NEXT(a, l)) {
                    if (a->rta_type == IFA_LOCAL &&
                        memcmp(RTA_DATA(a), &want, sizeof want) != 0) {
                        return failed("an address that is not its interface's");
                    }
                }
            } else if (h->nlmsg_type != RTM_NEWROUTE) {
                return failed("a dump's message of another type");
            }
        }
    }
}

/* The answer to a request that is not a dump: one message, whose type is
 * returned, with its error in `*error` for an NLMSG_ERROR. */
static int one_answer(int fd, int *index, int *error)
{
    char buf[4096] __attribute__((aligned(4)));
    struct nlmsghdr *h = (struct nlmsghdr *)buf;
    int n = (int)recv(fd, buf, sizeof buf, 0);

    if (n <= 0 || !NLMSG_OK(h, n) || (int)NLMSG_ALIGN(h->nlmsg_len) != n) {
        return -1;
    }
    if (h->nlmsg_type == NLMSG_ERROR) {
        *error = ((struct nlmsgerr *)NLMSG_DATA(h))->error;
    } else if (h->nlmsg_type == RTM_NEWLINK) {
        *index = ((struct ifinfomsg *)NLMSG_DATA(h))->ifi_index;
    }
    return h->nlmsg_type;
}

/* A socket of NETLINK_ROUTE into `*out`, and the port ids of it and of
 * two more. */
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
        me.nl_family != AF_NETLINK || me.nl_pid != (unsigned int)getpid()) {
        return fail("the first socket's port id is not the process ID");
    }
    port = me.nl_pid;
    second = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
    memset(&me, 0, sizeof me);
    me.nl_family = AF_NETLINK;
    if (second < 0 || bind(second, (struct sockaddr *)&me, sizeof me) != 0 ||
        getsockname(second, (struct sockaddr *)&me, &len) != 0 || me.nl_pid == port) {
        return fail("a second socket's port id");
    }
    close(second);
    second = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    memset(&me, 0, sizeof me);
    me.nl_family = AF_NETLINK;
    me.nl_pid = port;
    if (bind(second, (struct sockaddr *)&me, sizeof me) != -1 || errno != EADDRINUSE) {
        return fail("a bind of a port id another socket has was not EADDRINUSE");
    }
    close(second);
    return 0;
}

static int requests(int fd)
{
    struct ifinfomsg ifi;
    struct ifaddrmsg ifa;
    int index = -1, error = 0;

    if (dump(fd, RTM_GETLINK, 1) < 1 || dump(fd, RTM_GETADDR, 2) < 0) {
        return 1;
    }
    if (dump(fd, RTM_GETROUTE, 3) < 1) {
        return fail("no route");
    }
    memset(&ifi, 0, sizeof ifi);
    ifi.ifi_index = first_index;
    if (request(fd, RTM_GETLINK, 0, 4, &ifi, sizeof ifi) != 0 ||
        one_answer(fd, &index, &error) != RTM_NEWLINK || index != first_index) {
        return fail("RTM_GETLINK of the first link");
    }
    ifi.ifi_index = 9999;
    if (request(fd, RTM_GETLINK, 0, 5, &ifi, sizeof ifi) != 0 ||
        one_answer(fd, &index, &error) != NLMSG_ERROR || error != -ENODEV) {
        return fail("RTM_GETLINK of no link was not ENODEV");
    }
    memset(&ifa, 0, sizeof ifa);
    ifa.ifa_family = AF_INET;
    ifa.ifa_index = (unsigned int)first_index;
    if (request(fd, RTM_NEWADDR, NLM_F_CREATE, 6, &ifa, sizeof ifa) != 0 ||
        one_answer(fd, &index, &error) != NLMSG_ERROR || error != -EOPNOTSUPP) {
        return fail("RTM_NEWADDR was not EOPNOTSUPP");
    }
    return 0;
}

static int interfaces(void)
{
    struct ifconf ifc;
    struct ifreq reqs[8];
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int want;

    if (if_nametoindex(first_name) != (unsigned int)first_index) {
        return fail("if_nametoindex of the first link");
    }
    memset(&ifc, 0, sizeof ifc);
    if (s < 0 || ioctl(s, SIOCGIFCONF, &ifc) != 0 || ifc.ifc_len <= 0 ||
        ifc.ifc_len % (int)sizeof(struct ifreq) != 0) {
        return fail("SIOCGIFCONF with no buffer");
    }
    want = ifc.ifc_len;
    ifc.ifc_len = sizeof reqs;
    ifc.ifc_req = reqs;
    if (ioctl(s, SIOCGIFCONF, &ifc) != 0 || ifc.ifc_len != want ||
        reqs[0].ifr_addr.sa_family != AF_INET) {
        return fail("SIOCGIFCONF with a buffer");
    }
    close(s);
    return 0;
}

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
    close(fd);
    if (socket(AF_NETLINK, SOCK_RAW, 99) != -1 || errno != EPROTONOSUPPORT) {
        return fail("a netlink protocol the kernel has not was not EPROTONOSUPPORT");
    }
    return 0;
}

int main(void)
{
    int fd;

    if (route_socket(&fd) || requests(fd) || interfaces() || uevents()) {
        return 1;
    }
    close(fd);
    printf("NETLINKMUSL OK\n");
    return 0;
}
