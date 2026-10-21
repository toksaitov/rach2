/*
 * clockpeer.c: the host's clock reference of the clock case
 * (../machine.clock.json), which clockloadmusl asks for the host's time over
 * UDP before and after the host stops its machine.
 *
 * Usage: clockpeer PORT
 *
 * It binds UDP port PORT of 127.0.0.1, which the guest reaches at the user
 * network's gateway, 10.0.2.2, prints `clockpeer: answering on PORT`, and
 * answers each datagram `CLOCKREF N` with `CLOCKREF N MONO REAL`, the host's
 * CLOCK_MONOTONIC and CLOCK_REALTIME in nanoseconds, read as the request is
 * answered, and prints `clockpeer: answered N`. Any other datagram is
 * dropped. It runs until a signal ends it.
 *
 * The exit status is 1 for a failure of the host's own calls and 2 for a
 * usage error.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/** Give the port `text` writes, from 1 to 65535, or 0. */
static unsigned port_of(const char *text)
{
    char *end;
    long n;

    errno = 0;
    n = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || n < 1 || n > 65535)
        return 0;
    return (unsigned)n;
}

/** Give clock `id` in nanoseconds, or -1. */
static long long clock_ns(clockid_t id)
{
    struct timespec t;

    if (clock_gettime(id, &t) != 0)
        return -1;
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/** Bind UDP port `port` of 127.0.0.1 and answer on it, as the usage says. */
static int serve(unsigned port)
{
    struct sockaddr_in at, peer;
    socklen_t size;
    char packet[160], tail;
    unsigned sequence;
    long long mono, real;
    ssize_t n;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        fprintf(stderr, "clockpeer: socket: %s\n", strerror(errno));
        return 1;
    }
    memset(&at, 0, sizeof(at));
    at.sin_family = AF_INET;
    at.sin_port = htons((unsigned short)port);
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&at, sizeof(at)) != 0) {
        fprintf(stderr, "clockpeer: bind: 127.0.0.1:%u: %s\n", port, strerror(errno));
        close(fd);
        return 1;
    }
    printf("clockpeer: answering on %u\n", port);
    for (;;) {
        size = sizeof(peer);
        n = recvfrom(fd, packet, sizeof(packet) - 1, 0, (struct sockaddr *)&peer, &size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            fprintf(stderr, "clockpeer: recvfrom: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
        packet[n] = '\0';
        if (sscanf(packet, "CLOCKREF %u %c", &sequence, &tail) != 1)
            continue;
        mono = clock_ns(CLOCK_MONOTONIC);
        real = clock_ns(CLOCK_REALTIME);
        if (mono < 0 || real < 0) {
            fprintf(stderr, "clockpeer: clock_gettime: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
        n = snprintf(packet, sizeof(packet), "CLOCKREF %u %lld %lld", sequence, mono, real);
        if (sendto(fd, packet, (size_t)n, 0, (struct sockaddr *)&peer, size) != n)
            fprintf(stderr, "clockpeer: sendto: %s\n", strerror(errno));
        else
            printf("clockpeer: answered %u\n", sequence);
    }
}

int main(int argc, char **argv)
{
    unsigned port = argc == 2 ? port_of(argv[1]) : 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (port == 0) {
        fprintf(stderr, "clockpeer: usage: clockpeer PORT\n");
        return 2;
    }
    return serve(port);
}
