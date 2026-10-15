/**
 * hostpeer: the host end of a connection that a guest test needs.
 *
 * A test of the guest's network needs a process at the host end of QEMU's
 * user network: a peer for a connection the guest is to show in its tables,
 * or a server whose answers show what the guest sent. hostpeer is that
 * process, in one of two jobs.
 *
 *   hostpeer hold PORT SECONDS
 *       Connect to 127.0.0.1:PORT and keep the connection open for SECONDS.
 *       The procfiles suite keeps one open, through a forward to the
 *       guest's SSH server, while the machine's netstat reads its
 *       `/proc/net/tcp`, so the table has a live connection from outside
 *       to show.
 *
 *   hostpeer echo PORT SECONDS
 *       Listen on 127.0.0.1:PORT for both TCP and UDP for SECONDS and send
 *       back whatever arrives. The socket suite's probes reach it at the
 *       user network's gateway address, 10.0.2.2, send a string and require
 *       the same bytes back, which tests the guest's own transmit and
 *       receive paths. With a PORT of 0 the host chooses a port free for
 *       both transports, and the line that says the server is listening
 *       gives it.
 *
 * SECONDS is a completion bound and is multiplied by the set's deadline
 * factor. The exit status is 0 when the job was done, 1 when it could not
 * be, and 2 for a usage error; the caller decides whether a failure is
 * fatal. The end of an echo server's time is a success.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/** Fill `addr` with the loopback address and `port`. */
static void fill_local(struct sockaddr_in *addr, long port)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((unsigned short)port);
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

/**
 * The `hold` job: connect to the loopback's `port` and keep the connection
 * open for `seconds`. Returns 0, or 1 with the reason printed when the
 * connection cannot be made.
 */
static int hold(long port, long seconds)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("hostpeer: socket");
        return 1;
    }
    fill_local(&addr, port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("hostpeer: connect");
        close(fd);
        return 1;
    }
    sleep((unsigned)seconds);
    close(fd);
    return 0;
}

/** Seconds on the calendar clock, which `echo` measures its time on. */
static double now(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* The set's deadline factor, which multiplies every completion bound. */
static double deadline_factor = 1.0;

/**
 * Read the deadline factor. tests/run passes its `RACH2_TIMEOUT_FACTOR` to
 * every suite as `RACH2_DEADLINES=scaled` with `RACH2_DEADLINE_FACTOR=F`. A
 * process told nothing, told the mode without a factor, or given a factor
 * outside 1 to 64 keeps the factor 1.
 */
static void deadline_factor_read(void)
{
    const char *mode = getenv("RACH2_DEADLINES");
    const char *factor = getenv("RACH2_DEADLINE_FACTOR");
    double f;

    if (mode == NULL || strcmp(mode, "scaled") != 0 || factor == NULL)
        return;
    f = atof(factor);
    if (f >= 1.0 && f <= 64.0)
        deadline_factor = f;
}

/** A completion bound of `seconds` multiplied by the deadline factor. */
static double scaled(double seconds)
{
    return seconds * deadline_factor;
}

/**
 * Bind a TCP listener and a UDP socket to one port of the loopback address.
 *
 * With port 0 the kernel chooses a free TCP port, and the UDP socket is bound
 * to the same number. TCP and UDP have separate ports, so that number may be
 * in use for UDP; then a fresh pair of sockets is bound again, sixteen times
 * at most.
 *
 * @return the port bound, or 0 when none could be, with the reason printed.
 */
static long bind_pair(int *listener, int *dgram, long port)
{
    struct sockaddr_in addr;
    socklen_t len;
    int one = 1;
    int tries;

    for (tries = 0; tries < 16; tries++) {
        *listener = socket(AF_INET, SOCK_STREAM, 0);
        *dgram = socket(AF_INET, SOCK_DGRAM, 0);
        if (*listener < 0 || *dgram < 0) {
            perror("hostpeer: socket");
            return 0;
        }
        setsockopt(*listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        fill_local(&addr, port);
        len = sizeof(addr);
        if (bind(*listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(*listener, 4) != 0 ||
            getsockname(*listener, (struct sockaddr *)&addr, &len) != 0) {
            perror("hostpeer: tcp bind");
            return 0;
        }
        if (bind(*dgram, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return ntohs(addr.sin_port);
        if (port != 0 || errno != EADDRINUSE)
            break;
        close(*listener);
        close(*dgram);
    }
    perror("hostpeer: udp bind");
    return 0;
}

/**
 * The `echo` job: serve TCP and UDP on the loopback's `port`, or on a port
 * the host chooses when it is 0, for `seconds`, sending back what arrives.
 * Returns 0 when the time is up, or 1 with the reason printed when the
 * sockets cannot be bound or `select` fails.
 *
 * Both transports are served from one descriptor set, since the socket
 * suite starts one server for the two probes that need it, which run one
 * after the other in one boot. One TCP connection is served at a time; the
 * probes make one each, and a second waits in the listen queue. A TCP
 * write is repeated until the whole of a read is sent, since an echo of a
 * prefix would fail the probe for a fault on this side; when it cannot be,
 * the connection is closed. A close by the peer is printed, since a guest's
 * close that sends no FIN cannot be seen from inside the guest. `SIGPIPE`
 * is ignored, so a guest that resets its connection does not end the
 * server, which the suite would read as a guest that never answered.
 */
static int echo(long port, long seconds)
{
    struct sockaddr_in from;
    int listener, dgram, conn = -1;
    double deadline = now() + seconds;

    signal(SIGPIPE, SIG_IGN);
    port = bind_pair(&listener, &dgram, port);
    if (port == 0)
        return 1;
    printf("hostpeer: echoing tcp and udp on 127.0.0.1:%ld for %lds\n", port, seconds);
    fflush(stdout);

    while (now() < deadline) {
        char buf[2048];
        struct timeval tv;
        fd_set rfds;
        int max = listener > dgram ? listener : dgram;
        ssize_t n;

        FD_ZERO(&rfds);
        FD_SET(listener, &rfds);
        FD_SET(dgram, &rfds);
        if (conn >= 0) {
            FD_SET(conn, &rfds);
            if (conn > max)
                max = conn;
        }
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if (select(max + 1, &rfds, NULL, NULL, &tv) < 0) {
            if (errno == EINTR)
                continue;
            perror("hostpeer: select");
            return 1;
        }
        if (FD_ISSET(dgram, &rfds)) {
            socklen_t flen = sizeof(from);

            n = recvfrom(dgram, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
            if (n > 0)
                sendto(dgram, buf, (size_t)n, 0, (struct sockaddr *)&from, flen);
        }
        if (FD_ISSET(listener, &rfds) && conn < 0)
            conn = accept(listener, NULL, NULL);
        if (conn >= 0 && FD_ISSET(conn, &rfds)) {
            n = read(conn, buf, sizeof(buf));
            if (n > 0) {
                ssize_t off = 0;

                while (off < n) {
                    ssize_t w = write(conn, buf + off, (size_t)(n - off));

                    if (w <= 0)
                        break;
                    off += w;
                }
                if (off < n) {
                    close(conn);
                    conn = -1;
                }
            } else {
                if (n == 0)
                    printf("hostpeer: tcp: the peer closed its end\n");
                fflush(stdout);
                close(conn);
                conn = -1;
            }
        }
    }
    if (conn >= 0)
        close(conn);
    close(listener);
    close(dgram);
    return 0;
}

/**
 * Read `text` into `*out` as a decimal number. Returns 1, or 0 when `text`
 * is empty or has anything after the number: `strtol` alone reads `10junk`
 * as 10 and `junk` as 0.
 */
static int whole_number(const char *text, long *out)
{
    char *end;

    if (text[0] == '\0')
        return 0;
    *out = strtol(text, &end, 10);
    return *end == '\0';
}

/**
 * Check the arguments and run the job they name for its lifetime, which is
 * scaled as a completion bound is, since it covers the work of the machine
 * that uses the peer, and rounded up to a whole second.
 */
int main(int argc, char **argv)
{
    long port, seconds;

    if (argc != 4) {
        fprintf(stderr, "usage: hostpeer hold|echo PORT SECONDS\n");
        return 2;
    }
    if (!whole_number(argv[2], &port) || !whole_number(argv[3], &seconds) || port < 0 ||
        port > 65535 || seconds < 0 || (port == 0 && strcmp(argv[1], "echo") != 0)) {
        fprintf(stderr, "hostpeer: a port in 1..65535, or 0 for echo, and a whole "
                        "number of seconds\n");
        return 2;
    }
    deadline_factor_read();
    seconds = (long)(scaled((double)seconds) + 0.999);
    if (strcmp(argv[1], "hold") == 0)
        return hold(port, seconds);
    if (strcmp(argv[1], "echo") == 0)
        return echo(port, seconds);
    fprintf(stderr, "hostpeer: unknown job '%s'; hold or echo\n", argv[1]);
    return 2;
}
