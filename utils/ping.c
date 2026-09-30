/**
 * Send ICMP echo requests through the Rach2 network stack.
 *
 * `ping [-c count] [host]`, where `host` is an IPv4 address or name (default
 * 10.0.2.2, the SLIRP gateway) and `count` the number of requests, four by
 * default. Names are resolved through `/etc/hosts`, or `/hosts` where there
 * is no `/etc/hosts`, and then DNS (see `resolv.c`).
 *
 * Sends a few echo requests via the `SC_PING` socketcall and reports each
 * round-trip time in milliseconds, counted in kernel ticks of 10 ms, or a
 * timeout. `SC_PING` is a Rach2-specific operation with no libc wrapper, so it
 * is issued through a direct `socketcall(2)`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "netdb.h" /* resolve_host */

/* Rach2's socketcall for an ICMP echo, numbered clear of the BSD sub-calls. */
#define SC_PING    0x1000
#define DEFAULT_IP 0x0A000202UL /* 10.0.2.2 */
#define PING_COUNT 4

/**
 * Send one echo to `ip` (host byte order) and return the RTT in ticks, or -1
 * with `errno` set.
 *
 * `ETIMEDOUT` is nobody answering and `EBUSY` is another ping holding the
 * machine's one in-flight request, which is a different thing to tell a user:
 * counting a refusal as a timeout would report a live host as dead.
 */
static int ping_once(unsigned int ip)
{
    unsigned int args[1];

    args[0] = ip;
    return (int)syscall(SYS_socketcall, SC_PING, args);
}

int main(int argc, char **argv)
{
    unsigned int ip = DEFAULT_IP;
    int i, opt, replies = 0, count = PING_COUNT;
    char *end;

    while ((opt = getopt(argc, argv, "c:")) != -1) {
        if (opt != 'c') {
            fputs("usage: ping [-c count] [host]\n", stderr);
            return 2;
        }
        count = (int)strtol(optarg, &end, 10);
        if (*end != '\0' || count < 1) {
            fputs("ping: count must be a positive number\n", stderr);
            return 2;
        }
    }
    if (argc - optind > 1) {
        fputs("usage: ping [-c count] [host]\n", stderr);
        return 2;
    }
    if (optind < argc) {
        if (resolve_host(argv[optind], &ip) != 0) {
            fputs("ping: cannot resolve host\n", stderr);
            return 1;
        }
    }

    printf("PING %u.%u.%u.%u\n", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);

    for (i = 0; i < count; i++) {
        int rtt = ping_once(ip);
        if (rtt >= 0) {
            printf("  reply: seq=%d time=%d ms\n", i, rtt * 10);
            replies++;
        } else if (errno == EBUSY) {
            fputs("ping: another ping in flight\n", stderr);

            return 1;
        } else {
            printf("  seq=%d timeout\n", i);
        }
    }

    printf("%d/%d replies\n", replies, count);
    return replies > 0 ? 0 : 1;
}
