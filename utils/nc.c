/**
 * Minimal TCP client (netcat-style): `nc <host> <port>`.
 *
 * Connects to `host:port` over TCP, then relays a line of input to the socket
 * and prints the reply, using `read`/`write` on the socket descriptor so the
 * connection is just another file descriptor. Half-duplex (send a line, read a
 * reply), which suits line-oriented servers and the TCP echo server.
 *
 * The host argument may be an IPv4 address or a name resolved through `/hosts`
 * and DNS (see `resolv.c`).
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "netdb.h" /* resolve_host */

static int parse_port(const char *s)
{
    int p = 0;

    while (*s >= '0' && *s <= '9') {
        p = p * 10 + (*s - '0');
        s++;
    }
    return p;
}

int main(int argc, char **argv)
{
    unsigned int ip;
    struct sockaddr_in sa;
    char buf[1024];
    int fd, port, n, m;

    if (argc < 3) {
        fputs("usage: nc <host> <port>\n", stderr);
        return 1;
    }

    port = parse_port(argv[2]);
    if (port <= 0) {
        fputs("nc: invalid port\n", stderr);
        return 1;
    }
    if (resolve_host(argv[1], &ip) != 0) {
        fputs("nc: cannot resolve host\n", stderr);
        return 1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fputs("nc: socket failed\n", stderr);
        return 1;
    }

    /* resolve_host yields a host-order address; htonl/htons put it on the
     * wire. */
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(ip);

    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fputs("nc: connect failed\n", stderr);
        close(fd);
        return 1;
    }
    fputs("nc: connected\n", stdout);
    fflush(stdout);

    while ((n = (int)read(STDIN_FILENO, buf, sizeof buf)) > 0) {
        if (write(fd, buf, (size_t)n) < 0) {
            break;
        }
        m = (int)read(fd, buf, sizeof buf);
        if (m <= 0) {
            break;
        }
        write(STDOUT_FILENO, buf, (size_t)m);
    }

    close(fd);
    return 0;
}
