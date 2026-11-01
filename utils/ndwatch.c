/**
 * Follow the router advertisements of one interface for what the kernel
 * leaves to a program: the DNS servers and search domains of their RDNSS
 * and DNSSL options (RFC 8106), and the M and O flags that ask a host to
 * use DHCPv6 (RFC 4861, 4.2).
 *
 * `ndwatch IFACE` runs in the foreground, as a service of init. The kernel
 * passes each RDNSS and DNSSL option to netlink's RTNLGRP_ND_USEROPT as
 * RTM_NEWNDUSEROPT, as Linux does; ndwatch keeps the servers and domains
 * with their lifetimes and gives them to the resolver merge,
 * `/usr/share/udhcpc/resolv.script`, under the source name `IFACE.ra`,
 * again whenever one is added, renewed away or expires. The flags it reads
 * from the advertisements themselves, on a raw ICMPv6 socket whose filter
 * passes nothing else: M starts BusyBox's `udhcpc6` for an address and
 * servers, O alone starts it with `-l` for servers only. When the link goes
 * down, the network that comes back may be another, so ndwatch stops the
 * client it started and forgets every server and domain.
 *
 * It sends one router solicitation when it starts, since the kernel's were
 * answered before it was listening.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
#include <arpa/inet.h>
#include <net/if.h>

#define BUSYBOX       "/bin/busybox"
#define RESOLV_SCRIPT "/usr/share/udhcpc/resolv.script"

/* Netlink's route family, which the ported musl has no header for: the
 * message header, the address of a socket, a link's message and the
 * notifications this program listens to (rtnetlink(7)). */
#define NETLINK_ROUTE      0
#define RTM_NEWLINK        16
#define RTM_DELLINK        17
#define RTM_NEWNDUSEROPT   68
#define RTMGRP_LINK        1u
#define RTNLGRP_ND_USEROPT 20
#define NLMSG_HEADER       16 /* length 4, type 2, flags 2, sequence 4, port 4 */
#define IFINFO_LEN         16 /* family 1, pad 1, type 2, index 4, flags 4, change 4 */
#define NDUSEROPT_LEN      16 /* family 1, pad 1, options' length 2, index 4, ... */
#define ND_OPT_RDNSS       25
#define ND_OPT_DNSSL       31

struct netlink_address {
    unsigned short family;
    unsigned short pad;
    uint32_t pid;
    uint32_t groups;
};

/* How many servers and domains are kept at once: resolvers read three
 * servers, and RFC 8106, 5.3.1, asks for at least that many. A new one
 * past the limit takes the place of the one that expires first. */
#define SERVERS 4
#define DOMAINS 6

/* A domain's longest text, and a server's: an IPv6 address's text, a `%`
 * and an interface's name. */
#define NAME_MAX_TEXT 254
#define SERVER_TEXT   (INET6_ADDRSTRLEN + IFNAMSIZ + 1)

/* An entry's lifetime when the option's is all ones: for ever. */
#define FOREVER ((time_t)-1)

/* A server or domain a router gave, with the second of the monotonic clock
 * at which it expires, or FOREVER; `text` empty is a free slot. */
struct entry {
    char text[NAME_MAX_TEXT + 1];
    time_t until;
};

/* The DHCPv6 modes, in the order of what they give: none, servers only
 * (the O flag), an address and servers (M). */
enum { DHCP6_NONE, DHCP6_INFO, DHCP6_LEASE };

static const char *iface;
static int ifindex;
static struct entry servers[SERVERS];
static struct entry domains[DOMAINS];
/* What the merge was last given, so an unchanged list is not given again. */
static char published[(NAME_MAX_TEXT + 2) * DOMAINS + (SERVER_TEXT + 12) * SERVERS + 16];
static int link_up;
static pid_t dhcp6_pid = -1;
static int dhcp6_mode = DHCP6_NONE;
static volatile sig_atomic_t stopping;

/** Record a request to stop, for SIGTERM and SIGINT. */
static void stop_handler(int sig)
{
    (void)sig;
    stopping = 1;
}

/** The seconds of the monotonic clock. */
static time_t now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ts.tv_sec;
}

/** A big-endian 32-bit field at `p`. */
static uint32_t get32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/** A host-order 16-bit field at `p`, as netlink lays them out. */
static unsigned host16(const unsigned char *p)
{
    uint16_t v;

    memcpy(&v, p, sizeof(v));
    return v;
}

/** A host-order 32-bit field at `p`. */
static uint32_t host32(const unsigned char *p)
{
    uint32_t v;

    memcpy(&v, p, sizeof(v));
    return v;
}

/**
 * Whether the interface is up and has a link, from SIOCGIFFLAGS on the
 * socket `fd`; 0 when the request fails, as for an interface that is gone.
 */
static int link_now(int fd)
{
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) {
        return 0;
    }
    return (ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING);
}

/**
 * Give `text`, lines of `nameserver` and `search`, to the resolver merge
 * under the name `IFACE.ra`, unless it is what was given last. The merge
 * runs in a child that reads the text on its standard input; a failure is
 * reported and left for the next change.
 */
static void publish(const char *text)
{
    char source[IFNAMSIZ + 4];
    int pipefd[2], status;
    size_t len = strlen(text);
    pid_t pid;

    if (strcmp(text, published) == 0) {
        return;
    }
    snprintf(source, sizeof(source), "%s.ra", iface);
    if (pipe(pipefd) != 0) {
        fprintf(stderr, "ndwatch: resolver: pipe: %s\n", strerror(errno));
        return;
    }
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "ndwatch: resolver: fork: %s\n", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    if (pid == 0) {
        close(pipefd[1]);
        if (dup2(pipefd[0], STDIN_FILENO) < 0) {
            _exit(126);
        }
        close(pipefd[0]);
        execl(RESOLV_SCRIPT, RESOLV_SCRIPT, source, (char *)0);
        fprintf(stderr, "ndwatch: resolver: cannot run %s: %s\n", RESOLV_SCRIPT, strerror(errno));
        _exit(127);
    }
    close(pipefd[0]);
    if (len > 0 && write(pipefd[1], text, len) != (ssize_t)len) {
        fprintf(stderr, "ndwatch: resolver: write: %s\n", strerror(errno));
    }
    close(pipefd[1]);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return;
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        strcpy(published, text);
    } else {
        fprintf(stderr, "ndwatch: resolver: %s failed (status 0x%x)\n", RESOLV_SCRIPT,
                (unsigned)status);
    }
}

/** Give the merge the servers and domains now kept: a `search` line with
 * the domains, then a `nameserver` line for each server. */
static void publish_entries(void)
{
    char text[sizeof(published)];
    size_t n = 0;
    int i, any = 0;

    text[0] = '\0';
    for (i = 0; i < DOMAINS; i++) {
        if (domains[i].text[0]) {
            n += (size_t)snprintf(text + n, sizeof(text) - n, "%s%s", any ? " " : "search ",
                                  domains[i].text);
            any = 1;
        }
    }
    if (any) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "\n");
    }
    for (i = 0; i < SERVERS; i++) {
        if (servers[i].text[0]) {
            n += (size_t)snprintf(text + n, sizeof(text) - n, "nameserver %s\n", servers[i].text);
        }
    }
    publish(text);
}

/**
 * Add, renew or remove `text` in the table `t` of `count` entries, for a
 * lifetime of `life` seconds from `t0`: 0 removes it, all ones keeps it for
 * ever. A new entry with no free slot replaces the one that expires first.
 */
static void keep(struct entry *t, int count, const char *text, uint32_t life, time_t t0)
{
    int i, slot = -1, first = -1;

    for (i = 0; i < count; i++) {
        if (strcmp(t[i].text, text) == 0) {
            slot = i;
            break;
        }
    }
    if (life == 0) {
        if (slot >= 0) {
            t[slot].text[0] = '\0';
        }
        return;
    }
    if (slot < 0) {
        for (i = 0; i < count && slot < 0; i++) {
            if (!t[i].text[0]) {
                slot = i;
            } else if (t[i].until != FOREVER && (first < 0 || t[i].until < t[first].until)) {
                first = i;
            }
        }
        if (slot < 0) {
            if (first < 0) {
                return; /* every one is kept for ever */
            }
            slot = first;
        }
        strcpy(t[slot].text, text);
    }
    t[slot].until = life == 0xffffffffu ? FOREVER : t0 + (time_t)life;
}

/** Forget the entries of `t` that expired by `t0`. */
static void expire(struct entry *t, int count, time_t t0)
{
    int i;

    for (i = 0; i < count; i++) {
        if (t[i].text[0] && t[i].until != FOREVER && t[i].until <= t0) {
            t[i].text[0] = '\0';
        }
    }
}

/** The seconds until the first entry of `t` expires, or `limit` when none
 * expires sooner. */
static time_t soonest(const struct entry *t, int count, time_t t0, time_t limit)
{
    int i;

    for (i = 0; i < count; i++) {
        if (t[i].text[0] && t[i].until != FOREVER && t[i].until - t0 < limit) {
            limit = t[i].until - t0 > 0 ? t[i].until - t0 : 0;
        }
    }
    return limit;
}

/**
 * Take the servers of the RDNSS option `opt`, `len` bytes, its type and
 * length octets included. A multicast, unspecified or loopback address is
 * skipped; a link-local one is given with its interface, as `fe80::1%eth0`,
 * which a resolver needs to reach it.
 */
static void take_rdnss(const unsigned char *opt, unsigned len, time_t t0)
{
    char text[SERVER_TEXT];
    uint32_t life = get32(opt + 4);
    unsigned off;

    for (off = 8; off + 16 <= len; off += 16) {
        const unsigned char *a = opt + off;
        static const unsigned char loop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        static const unsigned char none[16];

        if (a[0] == 0xff || memcmp(a, loop, 16) == 0 || memcmp(a, none, 16) == 0) {
            continue;
        }
        if (!inet_ntop(AF_INET6, a, text, INET6_ADDRSTRLEN)) {
            continue;
        }
        if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) {
            strcat(text, "%");
            strcat(text, iface);
        }
        keep(servers, SERVERS, text, life, t0);
    }
}

/**
 * Take the domains of the DNSSL option `opt`, `len` bytes, each a name in
 * DNS's encoding of labels (RFC 1035, 3.1), the last followed by padding of
 * zeros. A name is taken only when every label is letters, digits, `-` and
 * `_`, since it is written into the resolver's file as it is; one that is
 * not is skipped, and a name that runs past the option ends the list.
 */
static void take_dnssl(const unsigned char *opt, unsigned len, time_t t0)
{
    char name[NAME_MAX_TEXT + 1];
    uint32_t life = get32(opt + 4);
    unsigned off = 8;

    while (off < len) {
        size_t n = 0;
        int good = 1;

        if (opt[off] == 0) {
            off++; /* padding, or an empty name */
            continue;
        }
        while (off < len && opt[off] != 0) {
            unsigned label = opt[off++], k;

            if (label > 63 || off + label > len) {
                return;
            }
            if (n + label + 1 > NAME_MAX_TEXT) {
                good = 0;
            }
            for (k = 0; k < label && good; k++) {
                unsigned char c = opt[off + k];

                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_')) {
                    good = 0;
                }
            }
            if (good) {
                if (n > 0) {
                    name[n++] = '.';
                }
                memcpy(name + n, opt + off, label);
                n += label;
            }
            off += label;
        }
        if (off >= len) {
            return; /* no terminating zero */
        }
        off++;
        if (good && n > 0) {
            name[n] = '\0';
            keep(domains, DOMAINS, name, life, t0);
        }
    }
}

/**
 * Take one RTM_NEWNDUSEROPT message `m` of `len` bytes, its netlink header
 * included: an RDNSS or DNSSL option of a router advertisement that reached
 * this interface; any other message is ignored.
 */
static void take_useropt(const unsigned char *m, unsigned len)
{
    const unsigned char *body = m + NLMSG_HEADER;
    const unsigned char *opt = body + NDUSEROPT_LEN;
    unsigned optlen, unit;

    if (len < NLMSG_HEADER + NDUSEROPT_LEN || body[0] != AF_INET6) {
        return;
    }
    optlen = host16(body + 2);
    if ((int)host32(body + 4) != ifindex || body[8] != ND_ROUTER_ADVERT ||
        NLMSG_HEADER + NDUSEROPT_LEN + optlen > len || optlen < 8) {
        return;
    }
    unit = opt[1] * 8u;
    if (unit < 8 || unit > optlen) {
        return;
    }
    if (opt[0] == ND_OPT_RDNSS && unit >= 24) {
        take_rdnss(opt, unit, now());
    } else if (opt[0] == ND_OPT_DNSSL && unit >= 16) {
        take_dnssl(opt, unit, now());
    }
}

/** The pid of a live `udhcpc6` that holds this interface by its pid file,
 * as one an `up` line of an `inet6` stanza started; 0 when there is none. */
static pid_t other_dhcp6(void)
{
    char path[64];
    long pid = 0;
    FILE *f;

    snprintf(path, sizeof(path), "/var/run/udhcpc6.%s.pid", iface);
    f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    if (fscanf(f, "%ld", &pid) != 1) {
        pid = 0;
    }
    fclose(f);
    if (pid <= 0 || pid == (long)dhcp6_pid || (kill((pid_t)pid, 0) != 0 && errno == ESRCH)) {
        return 0;
    }
    return (pid_t)pid;
}

/**
 * Stop the `udhcpc6` this program started, if one runs, and wait for it.
 * It runs with `-R`, so on SIGTERM it gives back its lease and runs its
 * script's `deconfig`, which takes the address and the servers away. One
 * that has not ended within five seconds is killed.
 */
static void stop_dhcp6(void)
{
    int i;

    if (dhcp6_pid < 0) {
        return;
    }
    kill(dhcp6_pid, SIGTERM);
    for (i = 0; i < 50; i++) {
        pid_t r = waitpid(dhcp6_pid, NULL, WNOHANG);

        if (r == dhcp6_pid || (r < 0 && errno != EINTR)) {
            break;
        }
        usleep(100000);
    }
    if (i == 50) {
        kill(dhcp6_pid, SIGKILL);
        (void)waitpid(dhcp6_pid, NULL, 0);
    }
    dhcp6_pid = -1;
    dhcp6_mode = DHCP6_NONE;
}

/**
 * Run `udhcpc6` for `mode` unless one of that mode or a fuller one runs
 * already, this program's or another's. A client for servers alone that an
 * M flag finds is replaced by one for a lease.
 */
static void start_dhcp6(int mode)
{
    char pidfile[64];
    char *argv[10];
    int n = 0;
    pid_t pid;

    if (mode <= dhcp6_mode || other_dhcp6() != 0) {
        return;
    }
    stop_dhcp6();
    snprintf(pidfile, sizeof(pidfile), "/var/run/udhcpc6.%s.pid", iface);
    argv[n++] = (char *)"udhcpc6";
    argv[n++] = (char *)"-f";
    argv[n++] = (char *)"-R";
    argv[n++] = (char *)"-i";
    argv[n++] = (char *)iface;
    argv[n++] = (char *)"-p";
    argv[n++] = pidfile;
    if (mode == DHCP6_INFO) {
        argv[n++] = (char *)"-l";
    }
    argv[n] = 0;
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "ndwatch: dhcpv6: fork: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execv(BUSYBOX, argv);
        fprintf(stderr, "ndwatch: dhcpv6: cannot run %s udhcpc6: %s\n", BUSYBOX, strerror(errno));
        _exit(127);
    }
    fprintf(stderr, "ndwatch: dhcpv6: %s: started udhcpc6%s as pid %ld\n", iface,
            mode == DHCP6_INFO ? " -l" : "", (long)pid);
    dhcp6_pid = pid;
    dhcp6_mode = mode;
}

/** Forget what the network gave, for a link that went down: the client
 * this program started, and every server and domain. */
static void link_lost(void)
{
    stop_dhcp6();
    memset(servers, 0, sizeof(servers));
    memset(domains, 0, sizeof(domains));
    publish_entries();
}

/**
 * Take a router advertisement of `len` bytes from `from`: start DHCPv6 for
 * its M or O flag. RFC 4861, 6.1.2, has a host take one only from a
 * link-local source; the kernel has checked the rest before the copy.
 */
static void take_advert(const unsigned char *ra, ssize_t len, const struct sockaddr_in6 *from)
{
    const unsigned char *src = from->sin6_addr.s6_addr;

    if (len < 16 || ra[0] != ND_ROUTER_ADVERT || ra[1] != 0) {
        return;
    }
    if (!(src[0] == 0xfe && (src[1] & 0xc0) == 0x80)) {
        return;
    }
    if (from->sin6_scope_id != 0 && (int)from->sin6_scope_id != ifindex) {
        return;
    }
    if (ra[5] & ND_RA_FLAG_MANAGED) {
        start_dhcp6(DHCP6_LEASE);
    } else if (ra[5] & ND_RA_FLAG_OTHER) {
        start_dhcp6(DHCP6_INFO);
    }
}

/** Send a router solicitation to the routers of the link, by the raw
 * ICMPv6 socket `raw`; the kernel fills in the checksum. */
static void solicit(int raw)
{
    struct sockaddr_in6 to;
    unsigned char rs[8];

    memset(rs, 0, sizeof(rs));
    rs[0] = ND_ROUTER_SOLICIT;
    memset(&to, 0, sizeof(to));
    to.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "ff02::2", &to.sin6_addr);
    to.sin6_scope_id = (uint32_t)ifindex;
    if (sendto(raw, rs, sizeof(rs), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        fprintf(stderr, "ndwatch: %s: router solicitation: %s\n", iface, strerror(errno));
    }
}

/**
 * Read what the netlink socket `nl` has: link changes of this interface
 * and router options. A receive that fails with ENOBUFS lost messages, so
 * the link is read again with `fd` and a solicitation asks the routers to
 * repeat themselves.
 */
static void read_netlink(int nl, int fd, int raw)
{
    unsigned char buf[8192];

    for (;;) {
        ssize_t got = recv(nl, buf, sizeof(buf), MSG_DONTWAIT);
        unsigned off = 0;

        if (got < 0) {
            if (errno == ENOBUFS) {
                int up = link_now(fd);

                if (link_up && !up) {
                    link_lost();
                }
                link_up = up;
                if (up) {
                    solicit(raw);
                }
                continue;
            }
            return;
        }
        while (off + NLMSG_HEADER <= (unsigned)got) {
            const unsigned char *m = buf + off;
            unsigned len = host32(m), type = host16(m + 4);

            if (len < NLMSG_HEADER || off + len > (unsigned)got) {
                break;
            }
            if ((type == RTM_NEWLINK || type == RTM_DELLINK) && len >= NLMSG_HEADER + IFINFO_LEN &&
                (int)host32(m + NLMSG_HEADER + 4) == ifindex) {
                uint32_t flags = host32(m + NLMSG_HEADER + 8);
                int up = type == RTM_NEWLINK && (flags & IFF_UP) && (flags & IFF_RUNNING);

                if (link_up && !up) {
                    fprintf(stderr, "ndwatch: %s: link down: forgetting the network's servers\n",
                            iface);
                    link_lost();
                }
                link_up = up;
            } else if (type == RTM_NEWNDUSEROPT) {
                take_useropt(m, len);
            }
            off += (len + 3u) & ~3u;
        }
    }
}

/**
 * Open the three sockets: netlink joined to the link and router option
 * groups, raw ICMPv6 passing router advertisements alone with a hop limit
 * of 255 for solicitations, and a datagram socket for the flags' ioctl.
 * Returns 0, or -1 after a message.
 */
static int open_sockets(int *nl, int *raw, int *fd)
{
    struct netlink_address a;
    struct icmp6_filter filter;
    int hops = 255;

    *nl = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (*nl < 0) {
        fprintf(stderr, "ndwatch: netlink: %s\n", strerror(errno));
        return -1;
    }
    memset(&a, 0, sizeof(a));
    a.family = AF_NETLINK;
    a.groups = RTMGRP_LINK | (1u << (RTNLGRP_ND_USEROPT - 1));
    if (bind(*nl, (struct sockaddr *)&a, sizeof(a)) != 0) {
        fprintf(stderr, "ndwatch: netlink: bind: %s\n", strerror(errno));
        return -1;
    }
    *raw = socket(AF_INET6, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_ICMPV6);
    if (*raw < 0) {
        fprintf(stderr, "ndwatch: icmpv6: %s\n", strerror(errno));
        return -1;
    }
    ICMP6_FILTER_SETBLOCKALL(&filter);
    ICMP6_FILTER_SETPASS(ND_ROUTER_ADVERT, &filter);
    if (setsockopt(*raw, IPPROTO_ICMPV6, ICMP6_FILTER, &filter, sizeof(filter)) != 0 ||
        setsockopt(*raw, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) != 0) {
        fprintf(stderr, "ndwatch: icmpv6: setsockopt: %s\n", strerror(errno));
        return -1;
    }
    *fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (*fd < 0) {
        fprintf(stderr, "ndwatch: socket: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    int nl, raw, fd;

    if (argc != 2 || argv[1][0] == '\0' || argv[1][0] == '.' || strchr(argv[1], '/') ||
        strlen(argv[1]) >= IFNAMSIZ) {
        fprintf(stderr, "usage: ndwatch IFACE\n");
        return 2;
    }
    iface = argv[1];
    ifindex = (int)if_nametoindex(iface);
    if (ifindex <= 0) {
        fprintf(stderr, "ndwatch: %s: no such interface\n", iface);
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (open_sockets(&nl, &raw, &fd) != 0) {
        return 1;
    }
    link_up = link_now(fd);
    if (link_up) {
        solicit(raw);
    }
    while (!stopping) {
        struct pollfd p[2];
        time_t t0 = now(), wait;

        if (dhcp6_pid > 0 && waitpid(dhcp6_pid, NULL, WNOHANG) == dhcp6_pid) {
            fprintf(stderr, "ndwatch: dhcpv6: %s: udhcpc6 ended\n", iface);
            dhcp6_pid = -1;
            dhcp6_mode = DHCP6_NONE;
        }
        /* At most a second, so an ended client is noticed. */
        wait = soonest(domains, DOMAINS, t0, soonest(servers, SERVERS, t0, 1));
        p[0].fd = nl;
        p[0].events = POLLIN;
        p[1].fd = raw;
        p[1].events = POLLIN;
        if (poll(p, 2, (int)wait * 1000) < 0 && errno != EINTR) {
            fprintf(stderr, "ndwatch: poll: %s\n", strerror(errno));
            break;
        }
        if (p[0].revents & POLLIN) {
            read_netlink(nl, fd, raw);
        }
        if (p[1].revents & POLLIN) {
            unsigned char ra[1500];
            struct sockaddr_in6 from;
            socklen_t fl = sizeof(from);
            ssize_t got =
                recvfrom(raw, ra, sizeof(ra), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);

            if (got > 0 && link_up) {
                take_advert(ra, got, &from);
            }
        }
        t0 = now();
        expire(servers, SERVERS, t0);
        expire(domains, DOMAINS, t0);
        publish_entries();
    }
    stop_dhcp6();
    memset(servers, 0, sizeof(servers));
    memset(domains, 0, sizeof(domains));
    publish_entries();
    return 0;
}
