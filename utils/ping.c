/**
 * Send ICMP and ICMPv6 echo requests through the Rach2 network stack.
 *
 * `ping [-nq46] [-c count] [-i interval] [-s size] [-t ttl] [-W timeout]
 * [-w deadline] host`, where `host` is an IPv4 address or a name, resolved
 * through `/etc/hosts`, or `/hosts` where there is no `/etc/hosts`, and then
 * DNS (see `resolv.c`), or an IPv6 address, with `%` and an interface's name
 * or index after a link-local one. `-6` requires an IPv6 address.
 *
 * Each request is the `SC_PING_DATA` socketcall, or `SC_PING6` for IPv6, a
 * Rach2 operation with no libc wrapper, so it is issued through a direct
 * system call (`pingcall.h`): socketcall(2) on i686, and Rach2's own number
 * on x86-64. The kernel sends an echo of `-s` bytes of data, 56 by default,
 * with the hop limit of `-t`, 64 by default, and sleeps the caller until
 * the reply, for up to four
 * attempts of a second each; a signal ends the wait. It returns the reply's
 * ICMP length in the low 16 bits and the hop limit the reply arrived with
 * above them, and no time, so the round trip is timed here, on the monotonic
 * clock. The timeout of `-W` and the deadline of `-w` are an interval timer
 * whose signal ends the wait. Requests go one a second, or one an interval,
 * until the count is sent, the deadline passes or an interrupt comes, and
 * then the statistics are printed, in the form Linux's iputils prints them.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "netdb.h"    /* resolve_host */
#include "pingcall.h" /* SYS_PINGCALL, SC_PING_DATA */

/* The most data one request carries: over IPv4, a datagram of the 1500-byte
 * MTU less the 20-byte IPv4 header and the 8-byte ICMP header, since that
 * stack does not fragment; over IPv6, the largest payload less the ICMPv6
 * header, sent in fragments past one datagram. The kernel's limits are the
 * same (`ICMP_PING_DATA_MAX` and `ICMP6_PING_DATA_MAX`). */
#define DATA_MAX  1472
#define DATA_MAX6 65527

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

/* Set by the handlers: an interrupt ends the run, an alarm ends one wait. */
static volatile sig_atomic_t interrupted;
static volatile sig_atomic_t alarmed;

/** The SIGINT handler: end the run. */
static void on_interrupt(int sig)
{
    (void)sig;
    interrupted = 1;
}

/** The SIGALRM handler: end the wait in progress. */
static void on_alarm(int sig)
{
    (void)sig;
    alarmed = 1;
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

/** Arm the interval timer for `us` microseconds, or disarm it with 0. */
static void arm(long long us)
{
    struct itimerval t;

    memset(&t, 0, sizeof(t));
    t.it_value.tv_sec = (long)(us / 1000000);
    t.it_value.tv_usec = (long)(us % 1000000);
    setitimer(ITIMER_REAL, &t, NULL);
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

/* The options as given, in microseconds where a time; 0 for a timeout or
 * deadline not given, and a count of -1 for none. */
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
        if (o->interval < 0 || o->timeout < 0 || o->deadline < 0) {
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
 * Send one request to `t` and wait at most `wait` microseconds, or the
 * kernel's own time with `wait` 0, for its reply, whose ICMP length goes to
 * `*len` and hop limit to `*ttl`; the round trip in microseconds, or -errno:
 * -EINTR for an interrupt or the wait's end.
 */
static long long ping_once(const struct target *t, const struct options *o, long long wait,
                           long *len, int *ttl)
{
    unsigned long args[4];
    long long t0, rtt;

    alarmed = 0;
    arm(wait);
    if (t->v6) {
        args[0] = (unsigned long)t->ip6;
        args[1] = t->scope;
        args[2] = (unsigned long)o->size;
        args[3] = (unsigned long)o->ttl;
    } else {
        args[0] = t->ip;
        args[1] = (unsigned long)o->size;
        args[2] = (unsigned long)o->ttl;
    }
    t0 = now_us();
    rtt = syscall(SYS_PINGCALL, t->v6 ? SC_PING6 : SC_PING_DATA, args);
    if (rtt >= 0) {
        *len = (long)(rtt & 0xFFFF);
        *ttl = (int)((rtt >> 16) & 0xFF);
        rtt = now_us() - t0;
    } else {
        rtt = -errno;
    }
    arm(0);
    return rtt;
}

/** Count the round trip `rtt` in `t`, and print its line, of a reply from
 * `from` of `size` ICMP bytes that arrived with the hop limit `ttl`, unless
 * `quiet`. */
static void count_reply(struct tally *t, const char *from, long long rtt, long size, int ttl,
                        int quiet)
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
        printf("%ld bytes from %s: icmp_seq=%ld ttl=%d time=", size, from, t->sent, ttl);
        print_ms(rtt);
        puts(" ms");
        fflush(stdout);
    }
}

/** Sleep until `us` microseconds after `from`, unless interrupted. */
static void pause_until(long long from, long long us)
{
    long long wait = us - (now_us() - from);
    struct timespec ts;

    if (wait > 0 && !interrupted) {
        ts.tv_sec = (time_t)(wait / 1000000);
        ts.tv_nsec = (long)(wait % 1000000) * 1000;
        nanosleep(&ts, NULL);
    }
}

/**
 * Ping `to` with the options `o`, counting in `t`, from `start` on the
 * monotonic clock; 1 when another ping is in flight on this machine, which
 * the kernel answers with EBUSY, 2 when the kernel cannot send a request at
 * all, as for an IPv6 destination with no route, else 0.
 */
static int run(const struct target *to, const struct options *o, struct tally *t, long long start)
{
    long long rtt, wait, left, t0;
    long len = 0;
    int ttl = 0;

    while (!interrupted && (o->count < 0 || t->sent < o->count)) {
        left = o->deadline ? o->deadline - (now_us() - start) : 0;
        if (o->deadline && left <= 0) {
            break;
        }
        wait = o->timeout;
        if (o->deadline && (wait == 0 || left < wait)) {
            wait = left;
        }
        t0 = now_us();
        rtt = ping_once(to, o, wait, &len, &ttl);
        if (rtt == -EBUSY) {
            fputs("ping: another ping is in flight on this machine\n", stderr);
            return 1;
        }
        if (rtt < 0 && rtt != -EINTR && rtt != -ETIMEDOUT) {
            fprintf(stderr, "ping: sendmsg: %s\n", strerror((int)-rtt));
            return 2;
        }
        if (rtt == -EINTR && !alarmed) {
            break;
        }
        t->sent++;
        if (rtt >= 0) {
            count_reply(t, to->text, rtt, len, ttl, o->quiet);
        }
        if (o->count >= 0 && t->sent >= o->count) {
            break;
        }
        pause_until(t0, o->interval);
    }
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
 * not resolve, a request the kernel cannot send, or another ping in flight.
 */
int main(int argc, char **argv)
{
    struct options o;
    struct tally t;
    struct sigaction sa;
    struct target to;
    long long start;
    int rc, busy;

    memset(&o, 0, sizeof(o));
    memset(&t, 0, sizeof(t));
    o.count = -1;
    o.interval = 1000000;
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
        fprintf(stderr,
                "ping: local error: message too long, mtu=1500: this stack "
                "does not fragment, so -s is at most %d\n",
                to.v6 ? DATA_MAX6 : DATA_MAX);
        return 2;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_interrupt;
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    if (to.v6) {
        printf("PING %s(%s) %ld data bytes\n", argv[optind], to.text, o.size);
    } else {
        printf("PING %s (%s) %ld(%ld) bytes of data.\n", argv[optind], to.text, o.size,
               o.size + 28);
    }
    fflush(stdout);
    start = now_us();
    busy = run(&to, &o, &t, start);
    if (busy == 2) {
        return 2;
    }
    report(argv[optind], &t, now_us() - start);
    if (busy) {
        return 2;
    }
    return t.received > 0 && (o.count < 0 || o.deadline == 0 || t.received >= o.count) ? 0 : 1;
}
