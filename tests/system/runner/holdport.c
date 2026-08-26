/**
 * holdport: open a TCP connection to a local port and hold it open.
 *
 * `holdport PORT SECONDS` connects to 127.0.0.1:PORT and keeps the
 * connection for SECONDS before closing. The procfiles suite needs a
 * connection that exists while the machine reads its own `/proc/net/tcp`,
 * so the row it is checking for is a live one: QEMU's user network accepts
 * the host side of a connection whether or not anything inside is
 * listening, so without a real peer the row would never appear and the
 * check would fail for a reason that has nothing to do with netstat.
 *
 * A connection that cannot be made is reported and exits nonzero; the
 * caller decides whether that is fatal.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    struct sockaddr_in addr;
    long port, seconds;
    int fd;

    if (argc != 3) {
        fprintf(stderr, "usage: holdport PORT SECONDS\n");
        return 2;
    }
    port = strtol(argv[1], NULL, 10);
    seconds = strtol(argv[2], NULL, 10);
    if (port <= 0 || port > 65535 || seconds < 0) {
        fprintf(stderr, "holdport: a port in 1..65535 and a whole number "
                "of seconds\n");
        return 2;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("holdport: socket");
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short) port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        perror("holdport: connect");
        close(fd);
        return 1;
    }
    sleep((unsigned) seconds);
    close(fd);
    return 0;
}
