/*
 * ping.c - send ICMP echo requests through the Rach2 network stack.
 *
 * Usage: ping [host]
 *   host: IPv4 address or name (default 10.0.2.2, the SLIRP gateway). Names are
 *   resolved through /hosts and DNS (see resolv.c).
 *
 * Sends a few echo requests via the SC_PING socketcall and reports the
 * round-trip time in kernel ticks (10 ms each) or a timeout.
 */

#include "netdb.h"

typedef unsigned int size_t;
typedef unsigned int uint32_t;

#define STDOUT_FILENO 1

#define SYS_SOCKETCALL 102
#define SC_PING        15

#define DEFAULT_IP 0x0A000202UL   /* 10.0.2.2 */
#define PING_COUNT 4

static int sys_write(int fd, const void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(4), "b"(fd), "c"(buf), "d"(count) : "memory");
    return r;
}

static int socketcall(int call, uint32_t *args)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_SOCKETCALL), "b"(call), "c"(args) : "memory");
    return r;
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *s)  { sys_write(STDOUT_FILENO, s, strlen(s)); }

static void print_num(int n)
{
    char buf[12];
    int i = 0;
    if (n < 0) { sys_write(STDOUT_FILENO, "-", 1); n = -n; }
    if (n == 0) buf[i++] = '0';
    while (n > 0) { buf[i++] = (char)('0' + n % 10); n /= 10; }
    while (i > 0) sys_write(STDOUT_FILENO, &buf[--i], 1);
}

int main(int argc, char **argv)
{
    uint32_t ip, args[1];
    int i, rtt, replies = 0;

    ip = DEFAULT_IP;
    if (argc > 1) {
        if (resolve_host(argv[1], &ip) != 0) {
            print("ping: cannot resolve host\n");
            return 1;
        }
    }

    print("PING ");
    print_num((int)((ip >> 24) & 0xFF)); print(".");
    print_num((int)((ip >> 16) & 0xFF)); print(".");
    print_num((int)((ip >>  8) & 0xFF)); print(".");
    print_num((int)( ip        & 0xFF)); print("\n");

    for (i = 0; i < PING_COUNT; i++) {
        args[0] = ip;
        rtt = socketcall(SC_PING, args);
        if (rtt >= 0) {
            print("  reply: seq="); print_num(i);
            print(" time="); print_num(rtt * 10); print(" ms\n");
            replies++;
        } else {
            print("  seq="); print_num(i); print(" timeout\n");
        }
    }

    print(""); print_num(replies); print("/"); print_num(PING_COUNT);
    print(" replies\n");
    return replies > 0 ? 0 : 1;
}
