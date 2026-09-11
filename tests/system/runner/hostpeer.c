/**
 * hostpeer: the host side of a connection a guest test needs.
 *
 * Two jobs, one program, because both are the same thing: something real
 * on the host end of QEMU's user network, which otherwise accepts and
 * discards whatever the guest sends.
 *
 *   hostpeer hold PORT SECONDS
 *       Connect to 127.0.0.1:PORT and keep the connection for SECONDS.
 *       The procfiles suite needs a connection that exists while the
 *       machine reads its own `/proc/net/tcp`, so the row it looks for is
 *       a live one. QEMU accepts the host side of a forwarded connection
 *       whether or not anything inside the guest is listening, so without
 *       a real peer the row would never appear and the check would fail
 *       for a reason that has nothing to do with netstat.
 *
 *   hostpeer echo PORT SECONDS
 *       Listen on 127.0.0.1:PORT for both TCP and UDP and send back
 *       whatever arrives, for SECONDS. The socket suite needs it: the
 *       guest reaches the host at the user network's gateway address
 *       (10.0.2.2), and the probes there send a string and require the
 *       same bytes back, which proves the guest's own transmit and
 *       receive paths rather than QEMU's willingness to accept.
 *
 * Exits nonzero when it could not do the job at all; the caller decides
 * whether that is fatal. In echo mode the run ends when the time is up,
 * which is a success: a server has no work of its own to finish.
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

static void fill_local(struct sockaddr_in *addr, long port)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((unsigned short) port);
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

static int hold(long port, long seconds)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("hostpeer: socket");
        return 1;
    }
    fill_local(&addr, port);
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        perror("hostpeer: connect");
        close(fd);
        return 1;
    }
    sleep((unsigned) seconds);
    close(fd);
    return 0;
}

static double now(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* ---- the deadline factor ----
 *
 * A set's runner measures the emulator's rate under the load the set makes
 * and passes `RACH2_DEADLINES=scaled` with `RACH2_DEADLINE_FACTOR=F` to
 * every suite; a deadline that bounds completion is multiplied by F here.
 * A process told nothing, or told the mode without a factor, keeps its
 * numbers as written. */
static double deadline_factor = 1.0;

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

static double scaled(double seconds)
{
    return seconds * deadline_factor;
}

/* Both transports on one descriptor set, because the two probes that need
   this run one after the other in the same boot and the suite starts one
   server for both. One TCP connection is served at a time; the probes make
   one each, and a second would only queue. */
static int echo(long port, long seconds)
{
    struct sockaddr_in addr, from;
    int listener, dgram, conn = -1;
    int one = 1;
    double deadline = now() + seconds;

    /* A guest that resets its connection would otherwise end this process
       with SIGPIPE, and the suite would read a dead server as a guest that
       never answered. */
    signal(SIGPIPE, SIG_IGN);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    dgram = socket(AF_INET, SOCK_DGRAM, 0);
    if (listener < 0 || dgram < 0) {
        perror("hostpeer: socket");
        return 1;
    }
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    fill_local(&addr, port);
    if (bind(listener, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
        listen(listener, 4) != 0) {
        perror("hostpeer: tcp bind");
        return 1;
    }
    if (bind(dgram, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        perror("hostpeer: udp bind");
        return 1;
    }
    printf("hostpeer: echoing tcp and udp on 127.0.0.1:%ld for %lds\n",
           port, seconds);
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

            n = recvfrom(dgram, buf, sizeof(buf), 0,
                         (struct sockaddr *) &from, &flen);
            if (n > 0)
                sendto(dgram, buf, (size_t) n, 0,
                       (struct sockaddr *) &from, flen);
        }
        if (FD_ISSET(listener, &rfds) && conn < 0)
            conn = accept(listener, NULL, NULL);
        if (conn >= 0 && FD_ISSET(conn, &rfds)) {
            n = read(conn, buf, sizeof(buf));
            if (n > 0) {
                /* Written to the end rather than once: a short write would
                   echo a prefix, and the probe comparing what came back
                   would fail for a reason on this side of the wire. */
                ssize_t off = 0;

                while (off < n) {
                    ssize_t w = write(conn, buf + off, (size_t) (n - off));

                    if (w <= 0)
                        break;
                    off += w;
                }
                if (off < n) {
                    close(conn);
                    conn = -1;
                }
            } else {
                /* Printed, because a graceful close is a thing the guest has
                   to do and nothing inside it can prove it did: a `close`
                   that returned zero without sending a FIN looks the same
                   from in there. */
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

/* A whole number and nothing else. `strtol` alone would read `10junk` as 10
   and `junk` as 0, so a mistyped argument would become a run of no seconds
   against a port nobody meant. */
static int whole_number(const char *text, long *out)
{
    char *end;

    if (text[0] == '\0')
        return 0;
    *out = strtol(text, &end, 10);
    return *end == '\0';
}

int main(int argc, char **argv)
{
    long port, seconds;

    if (argc != 4) {
        fprintf(stderr, "usage: hostpeer hold|echo PORT SECONDS\n");
        return 2;
    }
    if (!whole_number(argv[2], &port) || !whole_number(argv[3], &seconds) ||
        port <= 0 || port > 65535 || seconds < 0) {
        fprintf(stderr, "hostpeer: a port in 1..65535 and a whole number "
                "of seconds\n");
        return 2;
    }
    /* A lifetime covers the work of the machine that uses the peer, so
       it is scaled as a completion deadline is. */
    deadline_factor_read();
    seconds = (long)(scaled((double)seconds) + 0.999);
    if (strcmp(argv[1], "hold") == 0)
        return hold(port, seconds);
    if (strcmp(argv[1], "echo") == 0)
        return echo(port, seconds);
    fprintf(stderr, "hostpeer: unknown job '%s'; hold or echo\n", argv[1]);
    return 2;
}
