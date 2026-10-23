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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET peer_socket;
typedef WSAPOLLFD peer_pollfd;
#define PEER_INVALID     INVALID_SOCKET
#define peer_close       closesocket
#define peer_error       WSAGetLastError()
#define PEER_INTERRUPTED WSAEINTR
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int peer_socket;
typedef struct pollfd peer_pollfd;
#define PEER_INVALID     (-1)
#define peer_close       close
#define peer_error       errno
#define PEER_INTERRUPTED EINTR
#endif

/** Wait for readable sockets using the host's native polling API. */
static int peer_poll(peer_pollfd *fds, unsigned count)
{
#ifdef _WIN32
    return WSAPoll(fds, (ULONG)count, -1);
#else
    return poll(fds, (nfds_t)count, -1);
#endif
}

/** Report a native socket error without changing its value. */
static void socket_error(const char *operation)
{
    int error = peer_error;

    fprintf(stderr, "sshpeer: %s: socket error %d\n", operation, error);
}

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
static peer_socket connect_to(unsigned port)
{
    struct sockaddr_in to;
    peer_socket fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd == PEER_INVALID)
        return PEER_INVALID;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&to, sizeof(to)) != 0) {
        peer_close(fd);
        return PEER_INVALID;
    }
    return fd;
}

/** Connect to `port` until a connection has the server's identification. */
static int wait_answer(unsigned port)
{
    unsigned long refused = 0;
    char buf[8];
    long n;
    peer_socket fd;

    for (;;) {
        fd = connect_to(port);
        if (fd == PEER_INVALID) {
            socket_error("connect");
            return 1;
        }
        do
            n = recv(fd, buf, 4, MSG_WAITALL);
        while (n < 0 && peer_error == PEER_INTERRUPTED);
        peer_close(fd);
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
    peer_pollfd fds[FLOOD_MAX];
    int answered[FLOOD_MAX];
    unsigned active = 0, heard = 0, ended = 0, i;
    peer_socket fd;
    char buf[512];
    long n, j;
    int status = 0;

    for (j = 0; j < count; j++) {
        fd = connect_to(port);
        if (fd == PEER_INVALID)
            continue;
        fds[active].fd = fd;
        fds[active].events = POLLIN;
        answered[active++] = 0;
    }
    printf("sshpeer: open %u\n", active);
    while (active > 0) {
        if (peer_poll(fds, active) < 0) {
            if (peer_error == PEER_INTERRUPTED)
                continue;
            socket_error("poll");
            status = 1;
            break;
        }
        for (i = 0; i < active;) {
            if (fds[i].revents == 0) {
                i++;
                continue;
            }
            n = recv(fds[i].fd, buf, sizeof(buf), 0);
            if (n > 0 && !answered[i]) {
                answered[i] = 1;
                printf("sshpeer: answered %u\n", ++heard);
            } else if (n == 0 || (n < 0 && peer_error != PEER_INTERRUPTED)) {
                peer_close(fds[i].fd);
                fds[i] = fds[--active];
                answered[i] = answered[active];
                printf("sshpeer: ended %u\n", ++ended);
                continue;
            }
            i++;
        }
    }
    for (i = 0; i < active; i++)
        peer_close(fds[i].fd);
    if (status == 0) {
        /* A flood stays owned until its controller stops it, even when
         * the server has closed every connection. */
#ifdef _WIN32
        Sleep(INFINITE);
#else
        while (poll(NULL, 0, -1) < 0 && errno == EINTR) {
        }
#endif
    }
    return status;
}

int main(int argc, char **argv)
{
    unsigned port = 0;
    long count = 0;
    int status;
#ifdef _WIN32
    WSADATA data;

    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        fprintf(stderr, "sshpeer: Winsock could not start\n");
        return 1;
    }
#endif

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 3)
        port = (unsigned)number(argv[2], 65535);
    if (argc == 3 && port != 0 && strcmp(argv[1], "wait") == 0)
        status = wait_answer(port);
    else if (argc == 4 && port != 0 && strcmp(argv[1], "flood") == 0 &&
             (count = number(argv[3], FLOOD_MAX)) != 0)
        status = flood(port, count);
    else {
        fprintf(stderr, "sshpeer: usage: sshpeer wait PORT | sshpeer flood PORT COUNT\n");
        status = 2;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return status;
}
