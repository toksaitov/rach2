/**
 * Minimal TCP client (netcat-style): `nc <host> <port>`.
 *
 * Alternate one read of up to 1024 bytes from standard input with one read
 * from the connection. Input chunks need not contain complete lines, and a
 * reply may span several reads. Complete partial writes on both outputs.
 *
 * The host argument may be an IPv4 address or a name resolved through `/hosts`
 * and DNS (see `resolv.c`).
 */

#include <stdio.h>
#include <errno.h>
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

/** Write every byte, retrying interruptions. Return -1 on failure. */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    unsigned int ip;
    struct sockaddr_in sa;
    char buf[1024];
    int fd, port, n, m, failed = 0;

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
        if (write_all(fd, buf, (size_t)n) < 0) {
            failed = 1;
            break;
        }
        m = (int)read(fd, buf, sizeof buf);
        if (m <= 0) {
            break;
        }
        if (write_all(STDOUT_FILENO, buf, (size_t)m) < 0) {
            failed = 1;
            break;
        }
    }

    close(fd);
    if (failed) {
        fputs("nc: write failed\n", stderr);
    }
    return failed;
}
