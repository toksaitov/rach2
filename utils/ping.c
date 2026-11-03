/**
 * Send ICMP and ICMPv6 echo requests and count the replies, as Linux's
 * iputils ping does.
 *
 * `ping [-nq46] [-c count] [-i interval] [-s size] [-t ttl] [-W timeout]
 * [-w deadline] host`, where `host` is an IPv4 address or a name, resolved
 * through `/etc/hosts`, or `/hosts` where there is no `/etc/hosts`, and then
 * DNS (see `resolv.c`), or an IPv6 address, with `%` and an interface's name
 * or index after a link-local one. `-6` requires an IPv6 address.
 *
 * The requests go through an ICMP datagram socket, socket(AF_INET,
 * SOCK_DGRAM, IPPROTO_ICMP) or its IPv6 form, which any user may open: the
 * kernel gives the socket an echo identifier of its own, writes it into
 * each request, and hands the socket only the replies that carry it, so any
 * number of pings run at once, to one host or to several. A request of
 * `-s` bytes of data, 56 by default, with the hop limit of `-t`, 64 by
 * default, goes every interval, one second unless `-i` says otherwise,
 * whether or not the last was answered, until the count is sent, the
 * deadline passes or an interrupt comes; a reply counts against its own
 * sequence number whenever it comes. The round trip is timed here, from the
 * send to the reply, on the monotonic clock, and the reply's hop limit comes
 * from IP_RECVTTL or IPV6_RECVHOPLIMIT. Once the count is sent, ping waits
 * for the replies still out: twice the longest round trip, or an interval
 * if that is longer, after the last request when a reply has come, else
 * the `-W` timeout, ten seconds by default and forever for 0. With `-w`,
 * ping waits instead for the deadline or the count's replies, whichever
 * comes first. Then the statistics are printed, in the form iputils prints
 * them.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "netdb.h" /* resolve_host */

/* The most data one request carries: over IPv4, the largest datagram less
 * the 20-byte IPv4 header and the 8-byte ICMP header; over IPv6, the largest
 * payload less the ICMPv6 header. Either is sent in fragments past one
 * datagram. */
#define DATA_MAX  65507
#define DATA_MAX6 65527

/* The echo messages' types, ICMP's and ICMPv6's. */
#define ECHO_REQUEST  8
#define ECHO_REPLY    0
#define ECHO_REQUEST6 128
#define ECHO_REPLY6   129

/* The requests whose send times are kept, a power of two that divides the
 * 65536 sequence numbers; a reply to an older one is not counted. */
#define WINDOW 4096

/* How long ping waits for the replies still out after the last request
 * when none has come and -W did not say, in microseconds: iputils' ten
 * seconds. */
#define LINGER_US 10000000LL

#define USAGE                                                                                     \
    "usage: ping [-nq46] [-c count] [-i interval] [-s size] [-t ttl] [-W timeout] [-w deadline] " \
    "host\n"

/* What is pinged: an IPv4 address, or with `v6` set an IPv6 one and the
 * index of the interface it is reached by, 0 for none; `text` is the
 * address as a reply's line names it. */
struct target {
    int v6;
    unsigned int ip;
    unsigned char ip6[16];
    unsigned long scope;
    char text[INET6_ADDRSTRLEN];
};

/* Set by the handler of SIGINT, which ends the run. */
static volatile sig_atomic_t interrupted;

/** The SIGINT handler: end the run. */
static void on_interrupt(int sig)
{
    (void)sig;
    interrupted = 1;
}

/** Microseconds on the monotonic clock. */
static long long now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/**
 * Seconds with an optional fraction, as `-i`, `-W` and `-w` take them, in
 * microseconds; -1 for anything else or a negative time.
 */
static long long parse_seconds(const char *s)
{
    char *end;
    double v = strtod(s, &end);

    if (*s == '\0' || *end != '\0' || v < 0 || v > 1e9) {
        return -1;
    }
    return (long long)(v * 1000000.0 + 0.5);
}

/** The integer square root of `v`, for the deviation of the round trips. */
static long long isqrt(long long v)
{
    long long r = 0, bit = 1LL << 62;

    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/** Print `us` microseconds as milliseconds with three places. */
static void print_ms(long long us)
{
    printf("%lld.%03lld", us / 1000, us % 1000);
}

/* The options as given, in microseconds where a time: a deadline of 0 for
 * none, a timeout of -1 for none and 0 for forever, and a count of -1 for
 * none. */
struct options {
    long count, size, ttl;
    long long interval, timeout, deadline;
    int quiet, v6;
};

/* What the run has seen: the requests sent and answered, and the round
 * trips' least, greatest, sum and sum of squares, in microseconds. */
struct tally {
    long sent, received;
    long long min, max, sum, sum2;
};

/* The requests in flight: for each of the last WINDOW, by its sequence
 * number modulo WINDOW, its number counted from 1, 0 for none, when it
 * left, on the monotonic clock, and whether its reply has come. */
static long sent_no[WINDOW];
static long long sent_at[WINDOW];
static char answered[WINDOW];

/**
 * Read the options into `o` and leave `optind` at the host; 0, or the exit
 * status 2 after the diagnostic.
 */
static int parse(int argc, char **argv, struct options *o)
{
    int opt;
    char *end;

    while ((opt = getopt(argc, argv, "c:i:s:t:W:w:nq46")) != -1) {
        switch (opt) {
        case 'c':
            o->count = strtol(optarg, &end, 10);
            if (*optarg == '\0' || *end != '\0' || o->count < 1) {
                fputs("ping: count must be a positive number\n", stderr);
                return 2;
            }
            break;
        case 'i':
            o->interval = parse_seconds(optarg);
            break;
        case 's':
            o->size = strtol(optarg, &end, 10);
            if (*optarg == '\0' || *end != '\0' || o->size < 0) {
                fprintf(stderr, "ping: invalid argument: '%s'\n", optarg);
                return 2;
            }
            break;
        case 't':
            o->ttl = strtol(optarg, &end, 10);
            if (*optarg == '\0' || *end != '\0' || o->ttl < 1 || o->ttl > 255) {
                fprintf(stderr, "ping: invalid argument: '%s': out of range: 1 <= value <= 255\n",
                        optarg);
                return 2;
            }
            break;
        case 'W':
            o->timeout = parse_seconds(optarg);
            if (o->timeout < 0) {
                fputs("ping: a time must be a number of seconds\n", stderr);
                return 2;
            }
            break;
        case 'w':
            o->deadline = parse_seconds(optarg);
            break;
        case 'q':
            o->quiet = 1;
            break;
        case '6':
            o->v6 = 1;
            break;
        case 'n':
        case '4':
            break;
        default:
            fputs(USAGE, stderr);
            return 2;
        }
        if (o->interval < 0 || o->deadline < 0) {
            fputs("ping: a time must be a number of seconds\n", stderr);
            return 2;
        }
    }
    if (argc - optind != 1) {
        fputs(USAGE, stderr);
        return 2;
    }
    return 0;
}

/**
 * The index of the interface named `name`, a number or a name that
 * /sys/class/net knows, or 0 for neither.
 */
static unsigned long interface_index(const char *name)
{
    char path[64], buf[16];
    char *end;
    unsigned long n = strtoul(name, &end, 10);
    int fd, got;

    if (*name != '\0' && *end == '\0') {
        return n;
    }
    if (strlen(name) > 16 || strchr(name, '/')) {
        return 0;
    }
    snprintf(path, sizeof(path), "/sys/class/net/%s/ifindex", name);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    got = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got <= 0) {
        return 0;
    }
    buf[got] = '\0';
    return strtoul(buf, NULL, 10);
}

/**
 * Fill `t` with the address `host` names: an IPv6 address, with `%` and an
 * interface after it, when it has a colon or `v6` asks for one, else an
 * IPv4 address or a name. Returns 0, or the exit status 2 after the
 * diagnostic.
 */
static int resolve(const char *host, int v6, struct target *t)
{
    char addr[INET6_ADDRSTRLEN];
    const char *pct = strchr(host, '%');
    size_t n = pct ? (size_t)(pct - host) : strlen(host);

    memset(t, 0, sizeof(*t));
    if (v6 || strchr(host, ':')) {
        if (n >= sizeof(addr)) {
            n = sizeof(addr) - 1;
        }
        memcpy(addr, host, n);
        addr[n] = '\0';
        if (inet_pton(AF_INET6, addr, t->ip6) != 1) {
            fprintf(stderr, "ping: %s: not an IPv6 address\n", host);
            return 2;
        }
        if (pct && (t->scope = interface_index(pct + 1)) == 0) {
            fprintf(stderr, "ping: %s: no such interface\n", pct + 1);
            return 2;
        }
        t->v6 = 1;
        inet_ntop(AF_INET6, t->ip6, t->text, sizeof(t->text));
        return 0;
    }
    if (resolve_host(host, &t->ip) != 0) {
        fprintf(stderr, "ping: %s: cannot resolve the name\n", host);
        return 2;
    }
    snprintf(t->text, sizeof(t->text), "%u.%u.%u.%u", (t->ip >> 24) & 0xFF, (t->ip >> 16) & 0xFF,
             (t->ip >> 8) & 0xFF, t->ip & 0xFF);
    return 0;
}

/**
 * Open the ICMP datagram socket that pings `t` with the options `o`: its
 * hop limit set, and its receives asked to report each reply's. Returns
 * the descriptor, or -1 after the diagnostic.
 */
static int open_socket(const struct target *t, const struct options *o)
{
    int on = 1, hops = (int)o->ttl;
    int fd = socket(t->v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, t->v6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP);

    if (fd < 0) {
        fprintf(stderr, "ping: socket: %s\n", strerror(errno));
        return -1;
    }
    if (t->v6) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops, sizeof(hops));
        setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));
        setsockopt(fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &on, sizeof(on));
    } else {
        setsockopt(fd, IPPROTO_IP, IP_TTL, &hops, sizeof(hops));
        setsockopt(fd, IPPROTO_IP, IP_RECVTTL, &on, sizeof(on));
    }
    return fd;
}

/**
 * Send request number `n` of `len` bytes in `pkt` to `t` through `fd`, as
 * the kernel takes it: the type and sequence are written here, and the
 * identifier and checksum by the kernel. Records when it left in the
 * window. Returns 0, or the send's errno.
 */
static int send_request(int fd, const struct target *t, unsigned char *pkt, size_t len, long n)
{
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
    unsigned int seq = (unsigned int)n & 0xFFFF;
    long long at = now_us();
    ssize_t r;

    pkt[0] = t->v6 ? ECHO_REQUEST6 : ECHO_REQUEST;
    pkt[1] = 0;
    pkt[2] = pkt[3] = pkt[4] = pkt[5] = 0;
    pkt[6] = (unsigned char)(seq >> 8);
    pkt[7] = (unsigned char)seq;
    if (t->v6) {
        memset(&sin6, 0, sizeof(sin6));
        sin6.sin6_family = AF_INET6;
        memcpy(&sin6.sin6_addr, t->ip6, 16);
        sin6.sin6_scope_id = (unsigned int)t->scope;
        r = sendto(fd, pkt, len, 0, (struct sockaddr *)&sin6, sizeof(sin6));
    } else {
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(t->ip);
        r = sendto(fd, pkt, len, 0, (struct sockaddr *)&sin, sizeof(sin));
    }
    sent_no[seq % WINDOW] = n;
    sent_at[seq % WINDOW] = at;
    answered[seq % WINDOW] = 0;
    return r < 0 ? errno : 0;
}

/** Count the round trip `rtt` in `t`, and print its line, of reply number
 * `n` from `from` of `size` ICMP bytes that arrived with the hop limit
 * `ttl`, -1 for unknown, unless `quiet`. */
static void count_reply(struct tally *t, const char *from, long n, long long rtt, long size,
                        int ttl, int quiet)
{
    t->received++;
    t->sum += rtt;
    t->sum2 += rtt * rtt;
    if (t->received == 1 || rtt < t->min) {
        t->min = rtt;
    }
    if (rtt > t->max) {
        t->max = rtt;
    }
    if (!quiet) {
        printf("%ld bytes from %s: icmp_seq=%ld", size, from, n);
        if (ttl >= 0) {
            printf(" ttl=%d", ttl);
        }
        fputs(" time=", stdout);
        print_ms(rtt);
        puts(" ms");
        fflush(stdout);
    }
}

/**
 * Take every reply waiting on `fd`, for the target `to`, into `t`, the
 * buffer `buf` holding `cap` bytes. A reply to a request no longer in the
 * window, a second reply to one, and anything not an echo reply are passed
 * over.
 */
static void take_replies(int fd, const struct target *to, const struct options *o, struct tally *t,
                         unsigned char *buf, size_t cap)
{
    for (;;) {
        struct sockaddr_in6 from;
        union {
            struct cmsghdr align;
            char bytes[CMSG_SPACE(sizeof(int)) * 2];
        } control;
        struct iovec iov;
        struct msghdr msg;
        struct cmsghdr *c;
        char text[INET6_ADDRSTRLEN];
        unsigned int seq;
        long long now;
        ssize_t got;
        int ttl = -1;

        memset(&msg, 0, sizeof(msg));
        iov.iov_base = buf;
        iov.iov_len = cap;
        msg.msg_name = &from;
        msg.msg_namelen = sizeof(from);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = &control;
        msg.msg_controllen = sizeof(control);
        got = recvmsg(fd, &msg, MSG_DONTWAIT);
        if (got < 0) {
            return;
        }
        now = now_us();
        if (got < 8 || buf[0] != (to->v6 ? ECHO_REPLY6 : ECHO_REPLY)) {
            continue;
        }
        for (c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if ((c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TTL) ||
                (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_HOPLIMIT)) {
                memcpy(&ttl, CMSG_DATA(c), sizeof(ttl));
            }
        }
        seq = ((unsigned int)buf[6] << 8) | buf[7];
        if (sent_no[seq % WINDOW] == 0 || ((unsigned long)sent_no[seq % WINDOW] & 0xFFFF) != seq ||
            answered[seq % WINDOW]) {
            continue;
        }
        answered[seq % WINDOW] = 1;
        if (to->v6) {
            inet_ntop(AF_INET6, &from.sin6_addr, text, sizeof(text));
        } else {
            inet_ntop(AF_INET, &((struct sockaddr_in *)&from)->sin_addr, text, sizeof(text));
        }
        count_reply(t, text, sent_no[seq % WINDOW], now - sent_at[seq % WINDOW], (long)got, ttl,
                    o->quiet);
    }
}

/**
 * When, on the monotonic clock, the wait for the replies still out ends
 * once the last request has gone at `last`: -1 for never, as with -W 0 and
 * no reply yet.
 */
static long long linger_end(const struct options *o, const struct tally *t, long long last)
{
    long long wait;

    if (t->received > 0) {
        wait = 2 * t->max;
        if (wait < o->interval) {
            wait = o->interval;
        }
        return last + wait;
    }
    if (o->timeout == 0) {
        return -1;
    }
    return last + (o->timeout > 0 ? o->timeout : LINGER_US);
}

/**
 * Ping `to` with the options `o` through `fd`, counting in `t`, from
 * `start` on the monotonic clock. A request goes every interval until the
 * count is sent; then the run waits for the replies still out (`linger_end`),
 * or with a deadline for the deadline. The replies waiting are taken after
 * each request, as iputils does, so that an interval no longer than a send
 * still counts them. Returns 0, or 2 when the first
 * request cannot be sent at all, as to an IPv6 address with no route.
 */
static int run(int fd, const struct target *to, const struct options *o, struct tally *t,
               long long start)
{
    size_t len = 8 + (size_t)o->size;
    size_t cap = (to->v6 ? DATA_MAX6 : DATA_MAX) + 8 + 1;
    unsigned char *pkt = malloc(len);
    unsigned char *buf = malloc(cap);
    long long next = start, last = start, now, wake;
    long i;

    if (!pkt || !buf) {
        fputs("ping: out of memory\n", stderr);
        free(pkt);
        free(buf);
        return 2;
    }
    for (i = 0; i < o->size; i++) {
        pkt[8 + i] = (unsigned char)i;
    }
    for (;;) {
        struct pollfd pfd;
        int more = o->count < 0 || t->sent < o->count;
        int ms, err;

        now = now_us();
        if (interrupted || (o->deadline && now - start >= o->deadline) ||
            (o->count >= 0 && t->received >= o->count)) {
            break;
        }
        if (more && now >= next) {
            err = send_request(fd, to, pkt, len, t->sent + 1);
            if (err != 0) {
                fprintf(stderr, "ping: sendmsg: %s\n", strerror(err));
                if (t->sent == 0) {
                    free(pkt);
                    free(buf);
                    return 2;
                }
            }
            t->sent++;
            last = now;
            next = now + o->interval;
            take_replies(fd, to, o, t, buf, cap);
            continue;
        }
        wake = more ? next : (o->deadline ? -1 : linger_end(o, t, last));
        if (!more && !o->deadline && wake >= 0 && now >= wake) {
            break;
        }
        if (o->deadline && (wake < 0 || wake > start + o->deadline)) {
            wake = start + o->deadline;
        }
        ms = wake < 0 ? -1 : (int)((wake - now + 999) / 1000);
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, ms) > 0 && (pfd.revents & POLLIN)) {
            take_replies(fd, to, o, t, buf, cap);
        }
    }
    free(pkt);
    free(buf);
    return 0;
}

/** Print the statistics of `t` for `host`, the run having lasted `us`. */
static void report(const char *host, const struct tally *t, long long us)
{
    printf("\n--- %s ping statistics ---\n", host);
    printf("%ld packets transmitted, %ld received, %ld%% packet loss, time %lldms\n", t->sent,
           t->received, t->sent ? (t->sent - t->received) * 100 / t->sent : 0, us / 1000);
    if (t->received > 0) {
        long long avg = t->sum / t->received;

        fputs("rtt min/avg/max/mdev = ", stdout);
        print_ms(t->min);
        putchar('/');
        print_ms(avg);
        putchar('/');
        print_ms(t->max);
        putchar('/');
        print_ms(isqrt(t->sum2 / t->received - avg * avg));
        puts(" ms");
    }
}

/**
 * Resolve the host, ping it, and print the statistics. The status is 0 when
 * a reply came and, with both a count and a deadline given, the count's
 * worth came; 1 when fewer came; and 2 for a usage error, a name that does
 * not resolve, or a socket or first request the kernel refuses.
 */
int main(int argc, char **argv)
{
    struct options o;
    struct tally t;
    struct sigaction sa;
    struct target to;
    long long start;
    int rc, fd;

    memset(&o, 0, sizeof(o));
    memset(&t, 0, sizeof(t));
    o.count = -1;
    o.interval = 1000000;
    o.timeout = -1;
    o.size = 56;
    o.ttl = 64;
    rc = parse(argc, argv, &o);
    if (rc != 0) {
        return rc;
    }
    rc = resolve(argv[optind], o.v6, &to);
    if (rc != 0) {
        return rc;
    }
    if (o.size > (to.v6 ? DATA_MAX6 : DATA_MAX)) {
        fprintf(stderr, "ping: invalid packet size: -s is at most %d\n",
                to.v6 ? DATA_MAX6 : DATA_MAX);
        return 2;
    }
    fd = open_socket(&to, &o);
    if (fd < 0) {
        return 2;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_interrupt;
    sigaction(SIGINT, &sa, NULL);

    if (to.v6) {
        printf("PING %s(%s) %ld data bytes\n", argv[optind], to.text, o.size);
    } else {
        printf("PING %s (%s) %ld(%ld) bytes of data.\n", argv[optind], to.text, o.size,
               o.size + 28);
    }
    fflush(stdout);
    start = now_us();
    rc = run(fd, &to, &o, &t, start);
    close(fd);
    if (rc != 0) {
        return rc;
    }
    report(argv[optind], &t, now_us() - start);
    return t.received > 0 && (o.count < 0 || o.deadline == 0 || t.received >= o.count) ? 0 : 1;
}
