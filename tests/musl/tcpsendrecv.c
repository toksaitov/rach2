/*
 * tcpsendrecv.c - Phase 10: round-trip a line over a connected TCP socket using
 * send()/recv() (NOT read()/write()).
 *
 * musl's send()/recv() compile to sendto()/recvfrom() with a NULL address, which
 * the kernel must route to the TCP path on a connected stream socket (not the UDP
 * path). This connects out through the SLIRP gateway 10.0.2.2:PORT to a host echo
 * server, sends a known line with send(), reads the echo with recv(), and prints
 * "TCPSENDRECV OK" only if the echo matches -- locking the regression that broke
 * the musl httpd.
 *
 * Needs a host echo server on 127.0.0.1:PORT (the test harness starts one).
 * Linked statically against the ported musl; see tests/musl/build.sh.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define GW_IP 0x0A000202u /* 10.0.2.2, the SLIRP gateway (== the host) */
#define PORT  18079

int main(void)
{
    struct sockaddr_in sa;
    const char *msg = "rach2-tcp-send-recv\n";
    char buf[64];
    int fd;
    ssize_t n;
    size_t mlen = strlen(msg);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("TCPSENDRECV FAIL: socket\n");
        return 1;
    }

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr.s_addr = htonl(GW_IP);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        printf("TCPSENDRECV FAIL: connect\n");
        return 1;
    }

    /* Nothing sent yet, so a non-blocking recv must return EAGAIN, not block or
     * read (the server only echoes after it receives our line). */
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
