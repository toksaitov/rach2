/**
 * ndwatchmusl: check that ndwatch(8) takes a router advertisement only with
 * the hop limit 255, and goes on after its netlink socket overflows, and
 * that a raw ICMPv6 socket takes no message whose checksum fails.
 *
 * The rows, each judged by what the kernel or ndwatch reports:
 *
 * Over ::1, an echo request with a bad checksum, sent whole through a raw
 * socket of IPPROTO_RAW, does not reach a raw ICMPv6 socket, and the one
 * sent after it with a good checksum does.
 *
 * ndwatch runs on eth0 with its standard error on a pipe to the probe, and
 * its first router solicitation, of 8 bytes, comes back to the probe, which
 * joined ff02::2 on eth0; the kernel's own solicitations carry a link-layer
 * option and are 16 bytes. An advertisement to ff02::1 on eth0 with the M
 * flag and the hop limit 64, then one with the O flag alone and 255, both
 * from the card's link-local address, start one client: ndwatch reports
 * `udhcpc6 -l`, for the second.
 *
 * With ndwatch stopped, 48 advertisements, each with an RDNSS option of 89
 * servers of lifetime 0, give each netlink socket in RTNLGRP_ND_USEROPT more
 * than its 64 KiB of notifications: the probe's own socket in that group
 * then fails a receive with ENOBUFS. Continued, ndwatch reads the ENOBUFS,
 * reads the link again and sends another solicitation of 8 bytes; an
 * advertisement with the M flag then starts `udhcpc6` for a lease; and
 * ndwatch goes back to sleep in poll, the state 'S' of /proc/PID/stat,
 * instead of running without end.
 *
 * Datagrams to ::1 and the copies of a group's datagrams go round the
 * loopback ring in the order they were sent, so an absence is judged by the
 * datagram that arrives in its place. The card's link-local address is
 * tentative for a second after boot, and the first send to a group waits
 * for it, polling without a time limit of its own. The test image has no
 * BusyBox, so each client ndwatch starts fails at once; ndwatch reports the
 * start before the exec, which is what the rows read.
 *
 * It prints "NDWATCHMUSL OK" and exits 0 on success, or prints "NDWATCHMUSL
 * FAIL (detail)" and exits 1 at the first failure, after killing ndwatch.
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each row in order: `checksum`, `started`, `hop-limit`,
 * `overflow`, `recovers` and `sleeps`.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_report.h"

#define NDWATCH "/bin/ndwatch"

/* A port of no service, for the datagram that waits out the card's
 * tentative address. */
#define PORT_READY 47021

/* An echo request's identifier, apart from the kernel's ping. */
#define ECHO_ID 0x6e02

/* RTNLGRP_ND_USEROPT, the netlink group of router options. */
#define GROUP_USEROPT 20

/* The flood: advertisements, each with an RDNSS option of SERVERS
 * addresses, 1432 bytes, which with its netlink header and the router's
 * address is a notification of 1484 bytes; 48 of them pass the 64 KiB a
 * socket may have queued, and each advertisement fits one 1500-byte frame. */
#define FLOOD   48
#define SERVERS 89

/* AF_NETLINK's address, as linux/netlink.h lays it out. */
struct netlink_address {
    unsigned short family;
    unsigned short pad;
    unsigned int pid;
    unsigned int groups;
};

static pid_t ndwatch = -1;
static int ndwatch_err = -1;
static unsigned char msg[1500];

/** Kill ndwatch, if it runs, and wait for it. */
static void stop_ndwatch(void)
{
    if (ndwatch > 0) {
        kill(ndwatch, SIGKILL);
        kill(ndwatch, SIGCONT);
        (void)waitpid(ndwatch, NULL, 0);
        ndwatch = -1;
    }
}

/** Report one failed check, stop ndwatch, and return 1, the probe's
 * failure status. */
static int fail(const char *what)
{
    stop_ndwatch();
    printf("NDWATCHMUSL FAIL (%s)\n", what);
    report_stop(what);
    return 1;
}

/** The one's-complement sum of the ICMPv6 pseudo-header for `src` and `dst`
 * and the `len` bytes at `p`, folded and inverted, as the checksum field
 * takes it. */
static unsigned short icmp6_sum(const struct in6_addr *src, const struct in6_addr *dst,
                                const unsigned char *p, unsigned len)
{
    unsigned long sum = 0;
    unsigned i;

    for (i = 0; i < 16; i += 2) {
        sum += (unsigned)(src->s6_addr[i] << 8 | src->s6_addr[i + 1]);
        sum += (unsigned)(dst->s6_addr[i] << 8 | dst->s6_addr[i + 1]);
    }
    sum += len + IPPROTO_ICMPV6;
    for (i = 0; i + 1 < len; i += 2) {
        sum += (unsigned)(p[i] << 8 | p[i + 1]);
    }
    if (len & 1) {
        sum += (unsigned)(p[len - 1] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (unsigned short)~sum;
}

/** Send from the IPPROTO_RAW socket `fd` to ::1 an echo request of
 * sequence `seq` whose checksum is good, or off by one when `bad`. */
static int send_echo(int fd, unsigned seq, int bad)
{
    struct sockaddr_in6 to;
    unsigned char *icmp = msg + 40;
    unsigned short sum;

    memset(msg, 0, 48);
    msg[0] = 0x60;
    msg[5] = 8;
    msg[6] = IPPROTO_ICMPV6;
    msg[7] = 64;
    memcpy(msg + 8, &in6addr_loopback, 16);
    memcpy(msg + 24, &in6addr_loopback, 16);
    icmp[0] = ICMP6_ECHO_REQUEST;
    icmp[4] = ECHO_ID >> 8;
    icmp[5] = ECHO_ID & 0xff;
    icmp[6] = (unsigned char)(seq >> 8);
    icmp[7] = (unsigned char)seq;
    sum = (unsigned short)(icmp6_sum(&in6addr_loopback, &in6addr_loopback, icmp, 8) + bad);
    icmp[2] = (unsigned char)(sum >> 8);
    icmp[3] = (unsigned char)sum;
    memset(&to, 0, sizeof(to));
    to.sin6_family = AF_INET6;
    to.sin6_addr = in6addr_loopback;
    return sendto(fd, msg, 48, 0, (struct sockaddr *)&to, sizeof(to)) == 48 ? 0 : -1;
}

/** A raw ICMPv6 socket whose filter passes `type` alone, or -1. A new
 * socket passes every type until the filter is set, and the card's own
 * neighbour discovery and MLD come back to it early in the boot, so what
 * it took before is read away. */
static int raw_passing(int type)
{
    struct icmp6_filter f;
    int fd = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);

    ICMP6_FILTER_SETBLOCKALL(&f);
    ICMP6_FILTER_SETPASS(type, &f);
    if (fd < 0 || setsockopt(fd, IPPROTO_ICMPV6, ICMP6_FILTER, &f, sizeof(f)) != 0) {
        return -1;
    }
    while (recv(fd, msg, sizeof(msg), MSG_DONTWAIT) >= 0) {
    }
    return fd;
}

/** An echo request with a bad checksum, then a good one, over ::1. */
static int checksum(void)
{
    int rx = raw_passing(ICMP6_ECHO_REQUEST);
    int tx = socket(AF_INET6, SOCK_RAW, IPPROTO_RAW);
    int n;

    if (rx < 0 || tx < 0) {
        return fail("checksum: sockets");
    }
    if (send_echo(tx, 1, 1) != 0 || send_echo(tx, 2, 0) != 0) {
        return fail("checksum: send");
    }
    n = (int)recv(rx, msg, sizeof(msg), 0);
    if (n != 8 || msg[0] != ICMP6_ECHO_REQUEST || (msg[4] << 8 | msg[5]) != ECHO_ID) {
        char why[80];

        snprintf(why, sizeof(why), "checksum: %d bytes of type %d came, not the echo request", n,
                 n > 0 ? msg[0] : -1);
        return fail(why);
    }
    if (msg[7] != 2) {
        return fail("checksum: the request with a bad checksum came");
    }
    report_held();
    close(rx);
    close(tx);
    return 0;
}

/** Read the next line of ndwatch's standard error into `line`, of `room`
 * bytes, without its newline. Returns 0, or -1 at its end. */
static int next_line(char *line, size_t room)
{
    size_t n = 0;
    char c;

    for (;;) {
        ssize_t r = read(ndwatch_err, &c, 1);

        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        if (c == '\n') {
            break;
        }
        if (n + 1 < room) {
            line[n++] = c;
        }
    }
    line[n] = '\0';
    printf("ndwatchmusl: ndwatch said: %s\n", line);
    return 0;
}

/** Read ndwatch's standard error until it reports a client started, into
 * `line`. Returns 0, or -1 at its end. */
static int next_start(char *line, size_t room)
{
    do {
        if (next_line(line, room) != 0) {
            return -1;
        }
    } while (!strstr(line, "started udhcpc6"));
    return 0;
}

/** Take messages on the raw socket `rs` until ndwatch's router solicitation
 * of 8 bytes. Returns 0, or -1 when a receive fails. */
static int next_solicit(int rs)
{
    int n;

    do {
        n = (int)recv(rs, msg, sizeof(msg), 0);
        if (n < 0 && errno != EINTR) {
            return -1;
        }
    } while (n != 8 || msg[0] != ND_ROUTER_SOLICIT);
    return 0;
}

/** The state letter of ndwatch in /proc/PID/stat, or '?'. */
static char ndwatch_state(void)
{
    char path[32], buf[256], *p;
    int fd, n;

    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)ndwatch);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return '?';
    }
    n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return '?';
    }
    buf[n] = '\0';
    p = strrchr(buf, ')');
    return p && p[1] == ' ' ? p[2] : '?';
}

/** Yield until ndwatch's state is `want`, at most 1000 times. Returns 0
 * once it is, or -1. */
static int await_state(char want)
{
    int i;

    for (i = 0; i < 1000; i++) {
        if (ndwatch_state() == want) {
            return 0;
        }
        sched_yield();
    }
    return -1;
}

/** Send from `fd` to ff02::1 on the card a router advertisement with the
 * flags `flags` and, when `servers` is not 0, an RDNSS option of that many
 * servers of lifetime 0. The socket's IPV6_MULTICAST_HOPS gives its hop
 * limit. Returns 0 or the errno. */
static int send_advert(int fd, int eth, int flags, int servers)
{
    struct sockaddr_in6 to;
    unsigned len = 16;

    memset(msg, 0, sizeof(msg));
    msg[0] = ND_ROUTER_ADVERT;
    msg[5] = (unsigned char)flags;
    if (servers) {
        int i;

        msg[16] = 25; /* RDNSS (RFC 8106) */
        msg[17] = (unsigned char)(1 + 2 * servers);
        for (i = 0; i < servers; i++) {
            msg[24 + 16 * i] = 0xfd;
            msg[24 + 16 * i + 15] = (unsigned char)(i + 1);
        }
        len += 8u + 16u * (unsigned)servers;
    }
    memset(&to, 0, sizeof(to));
    to.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "ff02::1", &to.sin6_addr);
    to.sin6_scope_id = (unsigned)eth;
    return sendto(fd, msg, len, 0, (struct sockaddr *)&to, sizeof(to)) == (ssize_t)len ? 0 : errno;
}

/** Set IPV6_MULTICAST_HOPS on `fd` to `n`: 0, or -1. */
static int hops(int fd, int n)
{
    return setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &n, sizeof(n));
}

/** Wait until the card's link-local address is no longer tentative: a
 * datagram to ff02::1 on it fails with EADDRNOTAVAIL until then. Returns 0
 * or the errno. */
static int link_local_ready(int eth)
{
    struct sockaddr_in6 to;
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    int r = 0;

    if (fd < 0) {
        return errno;
    }
    memset(&to, 0, sizeof(to));
    to.sin6_family = AF_INET6;
    to.sin6_port = htons(PORT_READY);
    inet_pton(AF_INET6, "ff02::1", &to.sin6_addr);
    to.sin6_scope_id = (unsigned)eth;
    while (sendto(fd, "", 1, 0, (struct sockaddr *)&to, sizeof(to)) != 1) {
        if (errno != EADDRNOTAVAIL) {
            r = errno;
            break;
        }
        usleep(100000);
    }
    close(fd);
    return r;
}

/** Start ndwatch on eth0 with its standard error on a pipe. Returns 0, or
 * -1. */
static int start_ndwatch(void)
{
    int p[2];

    if (pipe(p) != 0) {
        return -1;
    }
    ndwatch = fork();
    if (ndwatch < 0) {
        return -1;
    }
    if (ndwatch == 0) {
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        execl(NDWATCH, "ndwatch", "eth0", (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    ndwatch_err = p[0];
    return 0;
}

/** ndwatch on the card: its start, the hop limit, and the overflow. */
static int watch(void)
{
    struct ipv6_mreq mreq;
    struct netlink_address a;
    char line[256], why[96];
    int eth = (int)if_nametoindex("eth0");
    int rs = raw_passing(ND_ROUTER_SOLICIT);
    int ra = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    int nl = socket(AF_NETLINK, SOCK_RAW, 0);
    int i, r;

    if (eth <= 0 || rs < 0 || ra < 0 || nl < 0) {
        return fail("started: sockets");
    }
    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET6, "ff02::2", &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = (unsigned)eth;
    if (setsockopt(rs, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) != 0 ||
        setsockopt(ra, IPPROTO_IPV6, IPV6_MULTICAST_IF, &eth, sizeof(eth)) != 0) {
        return fail("started: join ff02::2");
    }
    memset(&a, 0, sizeof(a));
    a.family = AF_NETLINK;
    a.groups = 1u << (GROUP_USEROPT - 1);
    if (bind(nl, (struct sockaddr *)&a, sizeof(a)) != 0) {
        return fail("started: netlink bind");
    }
    if ((r = link_local_ready(eth)) != 0) {
        snprintf(why, sizeof(why), "started: link-local address: %s", strerror(r));
        return fail(why);
    }
    if (start_ndwatch() != 0) {
        return fail("started: fork");
    }
    if (next_solicit(rs) != 0) {
        return fail("started: no router solicitation came");
    }
    report_held();

    if (hops(ra, 64) != 0 || send_advert(ra, eth, ND_RA_FLAG_MANAGED, 0) != 0 ||
        hops(ra, 255) != 0 || send_advert(ra, eth, ND_RA_FLAG_OTHER, 0) != 0) {
        return fail("hop-limit: send");
    }
    if (next_start(line, sizeof(line)) != 0) {
        return fail("hop-limit: ndwatch ended");
    }
    if (!strstr(line, "udhcpc6 -l ")) {
        return fail("hop-limit: an advertisement with hop limit 64 started a client");
    }
    report_held();

    while (recv(nl, msg, sizeof(msg), MSG_DONTWAIT) >= 0) {
    }
    if (kill(ndwatch, SIGSTOP) != 0 || await_state('T') != 0) {
        return fail("overflow: ndwatch did not stop");
    }
    for (i = 0; i < FLOOD; i++) {
        if ((r = send_advert(ra, eth, 0, SERVERS)) != 0) {
            snprintf(why, sizeof(why), "overflow: send: %s", strerror(r));
            return fail(why);
        }
    }
    if (recv(nl, msg, sizeof(msg), MSG_DONTWAIT) >= 0 || errno != ENOBUFS) {
        return fail("overflow: the probe's netlink socket did not overflow");
    }
    report_held();

    if (kill(ndwatch, SIGCONT) != 0 || next_solicit(rs) != 0) {
        return fail("recovers: no router solicitation after the overflow");
    }
    if (send_advert(ra, eth, ND_RA_FLAG_MANAGED, 0) != 0) {
        return fail("recovers: send");
    }
    do {
        if (next_start(line, sizeof(line)) != 0) {
            return fail("recovers: ndwatch ended");
        }
    } while (strstr(line, "udhcpc6 -l "));
    report_held();

    if (await_state('S') != 0) {
        return fail("sleeps: ndwatch did not go back to sleep");
    }
    report_held();

    kill(ndwatch, SIGTERM);
    close(ndwatch_err);
    (void)waitpid(ndwatch, NULL, 0);
    ndwatch = -1;
    close(rs);
    close(ra);
    close(nl);
    return 0;
}

/** Begin the report and plan its assertions, in the order they are
 * checked. */
static void plan(void)
{
    static const char *const ids[] = {"checksum", "started",  "hop-limit",
                                      "overflow", "recovers", "sleeps"};
    unsigned i;

    report_begin(report_case(environ, "guest.default.ndwatchmusl"));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        report_plan(ids[i]);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    plan();
    if (checksum() != 0 || watch() != 0) {
        return 1;
    }
    report_end();
    printf("NDWATCHMUSL OK\n");
    return 0;
}
