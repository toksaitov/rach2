/**
 * tcpsendrecv: send a line over a connected TCP socket with musl's send and
 * read its echo back with recv.
 *
 * musl's send and recv call sendto and recvfrom with a null address, which
 * the kernel must handle as TCP on a connected stream socket. The program
 * connects through the SLIRP gateway, 10.0.2.2:PORT, to an echo server on the
 * host, and checks in order: a non-blocking recv before any data fails with
 * EAGAIN, since the server echoes only after it receives the line; send
 * writes the whole line; and recv reads it back.
 *
 * No suite runs it; `tests/probe-helpers` lists it as a manual test, and it
 * needs an echo server listening on the host's 127.0.0.1:PORT. It prints
 * "TCPSENDRECV OK" and exits 0 when the echo matches, or prints "TCPSENDRECV
 * FAIL: reason" and exits 1. It is linked statically against the ported musl;
 * see `tests/musl/build.sh`.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define GW_IP 0x0A000202u /* 10.0.2.2, the SLIRP gateway to the host */
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
