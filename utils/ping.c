/*
 * ping.c - send ICMP echo requests through the Rach2 network stack.
 *
 * Usage: ping [host]
 *   host: IPv4 address or name (default 10.0.2.2, the SLIRP gateway). Names are
 *   resolved through /hosts and DNS (see resolv.c).
 *
 * Sends a few echo requests via the SC_PING socketcall and reports the
 * round-trip time in kernel ticks (10 ms each) or a timeout.
 *
 * Ported to musl: <stdio.h> for output, the shared resolver, and the one
 * Rach2-specific operation (SC_PING -- there is no libc wrapper for it) via a
 * direct socketcall(2). No hand-rolled int-0x80 stubs or crt0.
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "netdb.h" /* resolve_host */

#define SC_PING    0x1000       /* Rach2 socketcall extension: ICMP echo (renumbered off Linux getsockopt=15) */
#define DEFAULT_IP 0x0A000202UL /* 10.0.2.2 */
#define PING_COUNT 4

/* One echo to ip (host byte order); returns the RTT in ticks, or < 0 on timeout. */
static int ping_once(unsigned int ip)
{
    unsigned int args[1];

    args[0] = ip;
    return (int)syscall(SYS_socketcall, SC_PING, args);
}

int main(int argc, char **argv)
{
    unsigned int ip = DEFAULT_IP;
    int i, replies = 0;

    if (argc > 1) {
        if (resolve_host(argv[1], &ip) != 0) {
            fputs("ping: cannot resolve host\n", stderr);
            return 1;
        }
    }

    printf("PING %u.%u.%u.%u\n", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF,
           ip & 0xFF);

    for (i = 0; i < PING_COUNT; i++) {
        int rtt = ping_once(ip);
        if (rtt >= 0) {
            printf("  reply: seq=%d time=%d ms\n", i, rtt * 10);
            replies++;
        } else {
            printf("  seq=%d timeout\n", i);
        }
    }

    printf("%d/%d replies\n", replies, PING_COUNT);
    return replies > 0 ? 0 : 1;
}
