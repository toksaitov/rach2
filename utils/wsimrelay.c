/**
 * Carry a software radio's frames between /dev/wsim and a peer over UDP,
 * in version 1 of the relay format (wsimrelay(8), wsim(4)).
 *
 * `wsimrelay [-d DEVICE] [-b ADDRESS] [-l PORT] [-n NUMBER] [-s DBM]
 * [-t MS] PEER PORT` opens the device, which gives it a radio, binds a UDP
 * socket and runs in the foreground until a signal ends it. Each frame the
 * radio sends goes to the peer, and the peer's status of it comes back to
 * the radio; a frame that has no status within the timeout fails. Each
 * frame the peer sends goes to the radio, which receives it if it is
 * running and tuned to its channel, and the relay answers the peer with
 * its status: acknowledged when the radio received a frame addressed to it.
 * Datagrams from any address but the peer's are ignored.
 *
 * The peer is another wsimrelay, whose radio is then on the same medium,
 * or the Linux guest's hwsimrelay. The address bound is meant to be on a
 * card that /etc/network/interfaces and ifplugd leave alone, so that the
 * medium outlasts a pulled cable on eth0.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "wsimmedium.h"

/* wsim(4)'s request and its answer, sixteen bytes in the host's order. */
#define WSIM_IOC_INFO 0x80107701u
struct wsim_info {
    unsigned short radio;
    unsigned short frequency;
    unsigned char mac[6];
    unsigned char started;
    unsigned char filters;
    unsigned char reserved[4];
};

#define DEFAULT_PORT    5550
#define DEFAULT_SIGNAL  (-50) /* dBm, as mac80211_hwsim reports */
#define DEFAULT_TIMEOUT 1000  /* ms a frame waits for the peer's status */

static volatile sig_atomic_t stopping;

/** Note a request to stop; the loop ends at its next turn. */
static void on_signal(int sig)
{
    (void)sig;
    stopping = 1;
}

/** Milliseconds of the monotonic clock. */
static unsigned long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000ul + (unsigned long)(ts.tv_nsec / 1000000);
}

/** Print the usage on stderr and exit 2. */
static void usage(void)
{
    fprintf(stderr, "usage: wsimrelay [-d device] [-b address] [-l port] [-n number] [-s dbm] "
                    "[-t ms] peer peer-port\n");
    exit(2);
}

/** The decimal number `s`, which must lie in [lo, hi]; exits through usage
 * otherwise. */
static long number(const char *s, long lo, long hi)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi) {
        usage();
    }
    return v;
}

/** Fill `sa` with the IPv4 address `host` and `port`, or exit through
 * usage for an address that is not dotted decimal. */
static void address(struct sockaddr_in *sa, const char *host, long port)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &sa->sin_addr) != 1) {
        fprintf(stderr, "wsimrelay: %s: not an IPv4 address\n", host);
        usage();
    }
}

/** Write one status record of MEDIUM_HEADER bytes to the radio. A status
 * the radio no longer awaits (ENOENT) is not an error. */
static void to_radio(int dev, const unsigned char *r, unsigned len)
{
    if (write(dev, r, len) < 0 && errno != ENOENT) {
        fprintf(stderr, "wsimrelay: write: %s\n", strerror(errno));
    }
}

int main(int argc, char **argv)
{
    static unsigned char buf[MEDIUM_RECORD_MAX];
    const char *device = "/dev/wsim", *bind_to = "0.0.0.0";
    long port = DEFAULT_PORT, endpoint = 0, signal_dbm = DEFAULT_SIGNAL, timeout = DEFAULT_TIMEOUT;
    struct sockaddr_in local, peer, from;
    struct wsim_info info;
    struct medium m;
    struct sigaction sa;
    struct pollfd fds[2];
    int opt, dev, sock;

    while ((opt = getopt(argc, argv, "d:b:l:n:s:t:")) != -1) {
        switch (opt) {
        case 'd':
            device = optarg;
            break;
        case 'b':
            bind_to = optarg;
            break;
        case 'l':
            port = number(optarg, 1, 65535);
            break;
        case 'n':
            endpoint = number(optarg, 0, 65535);
            break;
        case 's':
            signal_dbm = number(optarg, -100, -1);
            break;
        case 't':
            timeout = number(optarg, 1, 60000);
            break;
        default:
            usage();
        }
    }
    if (argc - optind != 2) {
        usage();
    }
    address(&local, bind_to, port);
    address(&peer, argv[optind], number(argv[optind + 1], 1, 65535));

    dev = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (dev < 0) {
        fprintf(stderr, "wsimrelay: %s: %s\n", device, strerror(errno));
        return 1;
    }
    if (ioctl(dev, WSIM_IOC_INFO, &info) != 0) {
        fprintf(stderr, "wsimrelay: %s: not a software radio: %s\n", device, strerror(errno));
        return 1;
    }
    sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0 || bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        fprintf(stderr, "wsimrelay: %s:%ld: %s\n", bind_to, port, strerror(errno));
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    medium_init(&m, info.radio, (unsigned)endpoint, info.mac, (int)signal_dbm,
                (unsigned long)timeout);
    fprintf(stderr,
            "wsimrelay: wsim%u %02x:%02x:%02x:%02x:%02x:%02x: relaying to %s:%u from %s:%ld\n",
            info.radio, info.mac[0], info.mac[1], info.mac[2], info.mac[3], info.mac[4],
            info.mac[5], argv[optind], ntohs(peer.sin_port), bind_to, port);

    fds[0].fd = dev;
    fds[0].events = POLLIN;
    fds[1].fd = sock;
    fds[1].events = POLLIN;
    while (!stopping) {
        unsigned char status[MEDIUM_HEADER];
        struct medium_reply reply;
        socklen_t from_len = sizeof(from);
        unsigned long now = now_ms();
        long wait = medium_next(&m, now);
        ssize_t n;

        if (poll(fds, 2, wait < 0 ? -1 : (int)wait) < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "wsimrelay: poll: %s\n", strerror(errno));
            return 1;
        }
        now = now_ms();
        if (fds[0].revents & (POLLHUP | POLLERR)) {
            fprintf(stderr, "wsimrelay: %s: the radio is gone\n", device);
            return 1;
        }
        if (fds[0].revents & POLLIN) {
            n = read(dev, buf, sizeof(buf));
            if (n < 0 && errno != EAGAIN && errno != EINTR) {
                fprintf(stderr, "wsimrelay: read: %s\n", strerror(errno));
                return 1;
            }
            if (n > 0) {
                switch (medium_from_radio(&m, buf, (unsigned)n, now)) {
                case MEDIUM_TO_PEER:
                    /* A frame lost here fails when its status is due. */
                    sendto(sock, buf, (size_t)n, 0, (struct sockaddr *)&peer, sizeof(peer));
                    break;
                case MEDIUM_TO_RADIO:
                    to_radio(dev, buf, MEDIUM_HEADER);
                    break;
                default:
                    break;
                }
            }
        }
        if (fds[1].revents & POLLIN) {
            n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
            if (n > 0 && from.sin_addr.s_addr == peer.sin_addr.s_addr &&
                from.sin_port == peer.sin_port &&
                medium_from_peer(&m, buf, (unsigned)n, &reply) == MEDIUM_TO_RADIO) {
                if (reply.valid) {
                    medium_answer(&m, &reply, write(dev, buf, (size_t)n) == n, status);
                    sendto(sock, status, sizeof(status), 0, (struct sockaddr *)&peer, sizeof(peer));
                } else {
                    to_radio(dev, buf, MEDIUM_HEADER);
                }
            }
        }
        while (medium_expire(&m, now, status)) {
            to_radio(dev, status, MEDIUM_HEADER);
        }
    }
    fprintf(stderr,
            "wsimrelay: result: %lu sent, %lu received, %lu failed for want of status, "
            "%lu refused\n",
            m.sent, m.received, m.expired, m.refused);
    close(sock);
    close(dev);
    return 0;
}
