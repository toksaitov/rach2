/**
 * Check that dns_query of utils/resolv.c takes its answer only from the
 * nameserver it asked, through a model of a connected UDP socket.
 *
 * The model discards datagrams outside the connected peer. It does not test
 * the kernel's UDP delivery; the guest UDP probes cover that implementation.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

/*
 * connected: set by a successful connect. connect_error: nonzero to fail the
 * connect with EIO. closes, sends, receives: the calls of each kind made.
 * foreign_nxdomain: the foreign datagram is an NXDOMAIN, not an answer with
 * another address. foreign_port: the foreign datagram comes from the
 * nameserver's address on port 54, not from 10.0.2.4 on port 53. packet: the
 * datagrams offered so far; the first is the foreign one.
 */
static int failures, connected, connect_error, closes, sends, receives;
static int foreign_nxdomain, foreign_port, packet;
static struct sockaddr_in peer;  /* the address connect was given */
static unsigned char reply[512]; /* the nameserver's answer to the query */
static size_t reply_len;

/** Record a failed assertion without stopping the remaining cases. */
static void check(const char *detail, int ok)
{
    if (!ok) {
        fprintf(stderr, "resolv_hosttest: fail: %s\n", detail);
        failures++;
    }
}

/** Check that the resolver opens an IPv4 datagram socket, and return 42 as
 * its descriptor. */
static int model_socket(int domain, int type, int protocol)
{
    check("an ipv4 datagram socket is opened",
          domain == AF_INET && type == SOCK_DGRAM && protocol == 0);
    return 42;
}

/** Check that the socket is connected to the nameserver's port 53 and record
 * the peer; with `connect_error` set, fail with EIO instead. */
static int model_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    check("connect uses the query socket", fd == 42 && length == sizeof(peer));
    memcpy(&peer, address, sizeof(peer));
    check("connect selects the nameserver and dns port",
          peer.sin_family == AF_INET && ntohl(peer.sin_addr.s_addr) == 0x0a000203U &&
              ntohs(peer.sin_port) == 53);
    if (connect_error) {
        errno = EIO;
        return -1;
    }
    connected = 1;
    return 0;
}

/** Check that the query goes to the nameserver, and build in `reply` its
 * answer: the query with the response flags and one A record for 192.0.2.7. */
static ssize_t model_sendto(int fd, const void *buf, size_t size, int flags,
                            const struct sockaddr *address, socklen_t length)
{
    static const unsigned char answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 1, 0, 4, 192, 0, 2, 7};
    const struct sockaddr_in *to = (const struct sockaddr_in *)address;

    check("send uses the nameserver", fd == 42 && flags == 0 && length == sizeof(*to) &&
                                          ntohl(to->sin_addr.s_addr) == 0x0a000203U &&
                                          ntohs(to->sin_port) == 53);
    check("the query fits the reply", size + sizeof(answer) <= sizeof(reply));
    memcpy(reply, buf, size);
    reply[2] = 0x81;
    reply[3] = 0x80;
    reply[7] = 1;
    memcpy(reply + size, answer, sizeof(answer));
    reply_len = size + sizeof(answer);
    sends++;
    return (ssize_t)size;
}

/**
 * Give the next datagram. The first comes from a foreign sender and has an
 * NXDOMAIN or a wrong address, as `foreign_nxdomain` says; each later one is
 * the nameserver's answer. A connected socket drops a datagram from any
 * sender but its peer, and the call then fails with EAGAIN.
 */
static ssize_t model_recvfrom(int fd, void *buf, size_t size, int flags, struct sockaddr *address,
                              socklen_t *length)
{
    struct sockaddr_in from;
    unsigned char *out = buf;
    int foreign = packet++ == 0;

    (void)address;
    (void)length;
    receives++;
    check("receive uses the query socket", fd == 42 && flags == MSG_DONTWAIT && size >= reply_len);
    memset(&from, 0, sizeof(from));
    from.sin_addr.s_addr = htonl(foreign && !foreign_port ? 0x0a000204U : 0x0a000203U);
    from.sin_port = htons(foreign && foreign_port ? 54 : 53);
    if (connected &&
        (from.sin_addr.s_addr != peer.sin_addr.s_addr || from.sin_port != peer.sin_port)) {
        errno = EAGAIN;
        return -1;
    }
    memcpy(out, reply, reply_len);
    if (foreign) {
        if (foreign_nxdomain) {
            out[3] = 0x83;
            out[7] = 0;
        } else {
            out[reply_len - 1] = 99;
        }
    }
    return (ssize_t)reply_len;
}

/** Check that the query socket is the one closed, and count the close. */
static int model_close(int fd)
{
    check("the query socket is closed", fd == 42);
    closes++;
    return 0;
}

/** Advance the resolver's clock by 100 ms at each call, without a delay. */
static int model_gettimeofday(struct timeval *tv, void *zone)
{
    static long tick;

    (void)zone;
    tick += 100000;
    tv->tv_sec = tick / 1000000;
    tv->tv_usec = tick % 1000000;
    return 0;
}

#define socket       model_socket
#define connect      model_connect
#define sendto       model_sendto
#define recvfrom     model_recvfrom
#define close        model_close
#define gettimeofday model_gettimeofday
#include "../../utils/resolv.c"

/**
 * Run the rows: for each foreign sender and each foreign reply, the query
 * resolves to the nameserver's address and closes its socket once; a failed
 * connect fails the query before it sends. Returns 0 when every row passes.
 */
int main(void)
{
    unsigned int ip;

    for (foreign_port = 0; foreign_port < 2; foreign_port++) {
        for (foreign_nxdomain = 0; foreign_nxdomain < 2; foreign_nxdomain++) {
            connected = closes = sends = receives = packet = 0;
            ip = 0;
            check("a foreign reply cannot decide resolution",
                  dns_query("example.com", 0x0a000203U, &ip) == 0 && ip == 0xc0000207U);
            check("the nameserver answer is consumed", receives == 2);
            check("the socket is closed once", closes == 1);
        }
    }
    connect_error = 1;
    connected = closes = sends = receives = 0;
    check("a failed connect fails resolution", dns_query("example.com", 0x0a000203U, &ip) == -1);
    check("a failed connect closes without sending", closes == 1 && sends == 0 && receives == 0);
    if (failures == 0) {
        puts("resolv_hosttest: ok");
    }
    return failures != 0;
}
