/**
 * Minimal TCP client (netcat-style): `nc <host> <port>`.
 *
 * Alternate one read of up to 1024 bytes from standard input with one read
 * from the connection. Input chunks need not contain complete lines. A reply
 * that spans several reads is printed one read per chunk of input, and what
 * the peer sends after standard input ends is not read. Both outputs complete
 * partial writes. The loop ends with status 0 when a read from either side
 * returns 0 or fails, and with status 1 when a write fails.
 *
 * The port is decimal digits from 1 through 65535. The host is an IPv4
 * address or a name looked up in `/etc/hosts`, or `/hosts`, and then through
 * DNS (see `resolv.c`).
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "netdb.h" /* resolve_host */

/**
 * Return the TCP port in `s`, or 0 when `s` is not one.
 *
 * A port is decimal digits and nothing else, from 1 through 65535; a sign, a
 * trailing character or a value past the range is refused rather than cut to
 * 16 bits. The range is checked at each digit, so a long operand cannot
 * overflow the sum.
 */
static int parse_port(const char *s)
{
    long p = 0;

    if (*s == '\0') {
        return 0;
    }
    while (*s >= '0' && *s <= '9') {
        p = p * 10 + (*s - '0');
        if (p > 65535) {
            return 0;
        }
        s++;
    }
    return *s == '\0' ? (int)p : 0;
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
    if (port == 0) {
        fprintf(stderr, "nc: invalid port: %s: not a number from 1 to 65535\n", argv[2]);
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

    /* resolve_host yields host order; the socket address is network order. */
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
