/**
 * tcpsendrecv: send a line over a connected TCP socket with musl's send and
 * read its echo back with recv.
 *
 * musl's send and recv call sendto and recvfrom with a null address, which
 * the kernel must handle as TCP on a connected stream socket. The program
 * connects through the SLIRP gateway, 10.0.2.2:PORT, to an echo server on the
 * host, or to the echo server the argument `peer=ADDRESS:PORT` names, as a
 * native run gives it (tests/native/Readme.md), and checks in order: a
 * non-blocking recv before any data fails with EAGAIN, since the server echoes
 * only after it receives the line; send writes the whole line; and recv reads
 * it back.
 *
 * No suite runs it; `tests/probe-helpers` lists it as a manual test, and
 * without `peer` it needs an echo server listening on the host's
 * 127.0.0.1:PORT. It prints "TCPSENDRECV OK" and exits 0 when the echo
 * matches, or prints "TCPSENDRECV FAIL: reason" and exits 1. It is linked
 * statically against the ported musl; see `tests/musl/build.sh`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "../probe_args.h"

#define GW_IP 0x0A000202u /* 10.0.2.2, the SLIRP gateway to the host */
#define PORT  18079

/**
 * Fill `sa` with the echo server's address: the one `peer=ADDRESS:PORT`
 * names, or the gateway's at `PORT`. Returns 0, or -1 for a peer argument
 * that names no IPv4 address and port.
 */
static int server(struct sockaddr_in *sa)
{
    const char *peer = probe_arg("peer", NULL);
    const char *colon;
    char host[64];
    long port;

    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    if (peer == NULL) {
        sa->sin_port = htons(PORT);
        sa->sin_addr.s_addr = htonl(GW_IP);
        return 0;
    }
    colon = strrchr(peer, ':');
    if (colon == NULL || (size_t)(colon - peer) >= sizeof host) {
        return -1;
    }
    memcpy(host, peer, (size_t)(colon - peer));
    host[colon - peer] = '\0';
    port = strtol(colon + 1, NULL, 10);
    if (port < 1 || port > 65535 || inet_pton(AF_INET, host, &sa->sin_addr) != 1) {
        return -1;
    }
    sa->sin_port = htons((unsigned short)port);
    return 0;
}

int main(int argc, char **argv)
{
    struct sockaddr_in sa;
    const char *msg = "rach2-tcp-send-recv\n";
    char buf[64];
    int fd;
    ssize_t n;
    size_t mlen = strlen(msg);

    probe_args(argc, argv);
    if (server(&sa) != 0) {
        printf("TCPSENDRECV FAIL: peer argument\n");
        return 1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("TCPSENDRECV FAIL: socket\n");
        return 1;
    }

    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        printf("TCPSENDRECV FAIL: connect\n");
        return 1;
    }

    /* Non-blocking recv before any data: EAGAIN, not a block or a read. */
    errno = 0;
    if (recv(fd, buf, sizeof buf - 1, MSG_DONTWAIT) != -1 || errno != EAGAIN) {
        printf("TCPSENDRECV FAIL: nonblocking recv (errno=%d)\n", errno);
        return 1;
    }

    if (send(fd, msg, mlen, 0) != (ssize_t)mlen) {
        printf("TCPSENDRECV FAIL: send\n");
        return 1;
    }

    n = recv(fd, buf, sizeof buf - 1, 0);
    if (n <= 0) {
        printf("TCPSENDRECV FAIL: recv n=%d\n", (int)n);
        return 1;
    }
    buf[n] = '\0';

    if ((size_t)n < mlen || strncmp(buf, msg, mlen) != 0) {
        printf("TCPSENDRECV FAIL: echo mismatch\n");
        return 1;
    }

    printf("TCPSENDRECV OK\n");
    close(fd);
    return 0;
}
