/**
 * Send ICMP echo requests through the Rach2 network stack.
 *
 * `ping [-nq4] [-c count] [-i interval] [-W timeout] [-w deadline] host`,
 * where `host` is an IPv4 address or a name, resolved through `/etc/hosts`,
 * or `/hosts` where there is no `/etc/hosts`, and then DNS (see `resolv.c`).
 *
 * Each request is the `SC_PING` socketcall, a Rach2 operation with no libc
 * wrapper, so it is issued through a direct `socketcall(2)`. The kernel sends
 * an echo of no data and sleeps the caller until the reply, for up to four
 * attempts of a second each; a signal ends the wait. So the round trip is
 * timed here, on the monotonic clock, and the timeout of `-W` and the deadline
 * of `-w` are an interval timer whose signal ends the wait. Requests go one a
 * second, or one an interval, until the count is sent, the deadline passes or
 * an interrupt comes, and then the statistics are printed, in the form Linux's
 * iputils prints them.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "netdb.h" /* resolve_host */

/* Rach2's socketcall for an ICMP echo, numbered clear of the BSD sub-calls. */
#define SC_PING 0x1000

#define USAGE "usage: ping [-nq4] [-c count] [-i interval] [-W timeout] [-w deadline] host\n"

/* Set by the handlers: an interrupt ends the run, an alarm ends one wait. */
static volatile sig_atomic_t interrupted;
static volatile sig_atomic_t alarmed;

static void on_interrupt(int sig)
{
    (void)sig;
    interrupted = 1;
}

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

/* What the options asked for, in microseconds where a time; 0 for a
 * timeout or deadline not given, and a count of -1 for none. */
struct options {
    long count;
    long long interval, timeout, deadline;
    int quiet;
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

    while ((opt = getopt(argc, argv, "c:i:W:w:nq4")) != -1) {
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
        case 'W':
            o->timeout = parse_seconds(optarg);
            break;
        case 'w':
            o->deadline = parse_seconds(optarg);
            break;
        case 'q':
            o->quiet = 1;
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
 * Send one request to `ip` and wait at most `wait` microseconds, or the
 * kernel's own time with `wait` 0, for its reply; the round trip in
 * microseconds, or -errno: -EINTR for an interrupt or the wait's end.
 */
static long long ping_once(unsigned int ip, long long wait)
{
    unsigned int args[1];
    long long t0, rtt;

    alarmed = 0;
    arm(wait);
    args[0] = ip;
    t0 = now_us();
    rtt = syscall(SYS_socketcall, SC_PING, args);
    rtt = rtt >= 0 ? now_us() - t0 : -errno;
    arm(0);
    return rtt;
}

/** Count the round trip `rtt` in `t`, and print its line unless `quiet`. */
static void count_reply(struct tally *t, unsigned int ip, long long rtt, int quiet)
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
        printf("8 bytes from %u.%u.%u.%u: icmp_seq=%ld time=", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
               (ip >> 8) & 0xFF, ip & 0xFF, t->sent);
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
 * Ping `ip` as `o` asks, counting in `t`, from `start` on the monotonic
 * clock; 1 when another ping held the machine's one request, else 0.
 */
static int run(unsigned int ip, const struct options *o, struct tally *t, long long start)
{
    long long rtt, wait, left, t0;

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
        rtt = ping_once(ip, wait);
        if (rtt == -EBUSY) {
            fputs("ping: another ping is in flight on this machine\n", stderr);
            return 1;
        }
        if (rtt == -EINTR && !alarmed) {
            break;
        }
        t->sent++;
        if (rtt >= 0) {
            count_reply(t, ip, rtt, o->quiet);
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

int main(int argc, char **argv)
{
    struct options o;
    struct tally t;
    struct sigaction sa;
    unsigned int ip;
    long long start;
    int rc, busy;

    memset(&o, 0, sizeof(o));
    memset(&t, 0, sizeof(t));
    o.count = -1;
    o.interval = 1000000;
    rc = parse(argc, argv, &o);
    if (rc != 0) {
        return rc;
    }
    if (resolve_host(argv[optind], &ip) != 0) {
        fprintf(stderr, "ping: %s: cannot resolve the name\n", argv[optind]);
        return 2;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_interrupt;
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    printf("PING %s (%u.%u.%u.%u) 0(28) bytes of data.\n", argv[optind], (ip >> 24) & 0xFF,
           (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    fflush(stdout);
    start = now_us();
    busy = run(ip, &o, &t, start);
    report(argv[optind], &t, now_us() - start);
    if (busy) {
        return 2;
    }
    return t.received > 0 && (o.count < 0 || o.deadline == 0 || t.received >= o.count) ? 0 : 1;
}
