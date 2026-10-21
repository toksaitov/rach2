/*
 * sshpeer.c: the host's side of the network cases' SSH connections that
 * are not logins: a wait for the guest's server to answer, and a flood of
 * connections held without a word.
 *
 * Usage: sshpeer wait PORT
 *        sshpeer flood PORT COUNT
 *
 * PORT is a port of 127.0.0.1 that QEMU forwards to the guest's server.
 * QEMU takes a connection on the host before the guest sees it, and closes
 * it once the guest has refused it, so only the server's first bytes, its
 * identification, say that the server itself accepted a connection.
 *
 * `wait` connects until a connection has the server's identification,
 * prints `sshpeer: answered after N refused` and exits 0. Each try follows
 * the guest's refusal of the one before, so the tries go at the pace the
 * guest answers and not the host's.
 *
 * `flood` opens COUNT connections and prints `sshpeer: open N`, N those
 * that connected, then `sshpeer: answered K` each time another has had the
 * server's identification, and `sshpeer: ended K` each time another was
 * closed from the other side. It sends nothing, reads and drops what
 * arrives, and runs until a signal ends it, which closes them all.
 *
 * The exit status is 1 for a failure of the host's own calls and 2 for a
 * usage error.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The most connections a flood holds. */
#define FLOOD_MAX 256

/** Give the number `text` writes, between 1 and `max`, or 0. */
static long number(const char *text, long max)
{
    char *end;
    long n;

    errno = 0;
    n = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || n < 1 || n > max)
        return 0;
    return n;
}

/** Connect a new socket to 127.0.0.1:`port`, and give it, or -1. */
static int connect_to(unsigned port)
{
    struct sockaddr_in to;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&to, sizeof(to)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/** Connect to `port` until a connection has the server's identification. */
static int wait_answer(unsigned port)
{
    unsigned long refused = 0;
    char buf[8];
    ssize_t n;
    int fd;

    for (;;) {
        fd = connect_to(port);
        if (fd < 0) {
            fprintf(stderr, "sshpeer: connect: %s\n", strerror(errno));
            return 1;
        }
        do
            n = recv(fd, buf, 4, MSG_WAITALL);
        while (n < 0 && errno == EINTR);
        close(fd);
        if (n == 4 && memcmp(buf, "SSH-", 4) == 0) {
            printf("sshpeer: answered after %lu refused\n", refused);
            return 0;
        }
        refused++;
    }
}

/** Open `count` connections to `port` and hold them, as `flood` says. */
static int flood(unsigned port, long count)
{
    struct pollfd fds[FLOOD_MAX];
    int answered[FLOOD_MAX];
    int open = 0, heard = 0, ended = 0;
    char buf[512];
    ssize_t n;
    long i;

    for (i = 0; i < count; i++) {
        fds[i].fd = connect_to(port);
        fds[i].events = POLLIN;
        answered[i] = 0;
        if (fds[i].fd >= 0)
            open++;
    }
    printf("sshpeer: open %d\n", open);
    for (;;) {
        if (poll(fds, (nfds_t)count, -1) < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "sshpeer: poll: %s\n", strerror(errno));
            return 1;
        }
        for (i = 0; i < count; i++) {
            if (fds[i].fd < 0 || fds[i].revents == 0)
                continue;
            n = recv(fds[i].fd, buf, sizeof(buf), 0);
            if (n > 0 && !answered[i]) {
                answered[i] = 1;
                printf("sshpeer: answered %d\n", ++heard);
            } else if (n == 0 || (n < 0 && errno != EINTR)) {
                close(fds[i].fd);
                fds[i].fd = -1;
                printf("sshpeer: ended %d\n", ++ended);
            }
        }
    }
}

int main(int argc, char **argv)
{
    unsigned port = 0;
    long count = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 3)
        port = (unsigned)number(argv[2], 65535);
    if (argc == 3 && port != 0 && strcmp(argv[1], "wait") == 0)
        return wait_answer(port);
    if (argc == 4 && port != 0 && strcmp(argv[1], "flood") == 0 &&
        (count = number(argv[3], FLOOD_MAX)) != 0)
        return flood(port, count);
    fprintf(stderr, "sshpeer: usage: sshpeer wait PORT | sshpeer flood PORT COUNT\n");
    return 2;
}
