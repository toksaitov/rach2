/*
 * nettput.c: the TCP peer and client of the netguest suite, one program for
 * Rach2, Linux and macOS, whose tests end at a count of bytes, exchanges or
 * attempts, never at a time. It speaks IPv4 only and takes dotted addresses.
 *
 *   nettput -s [-p PORT]
 *   nettput -c HOST [-p PORT] [-n COUNT] [-l LEN] send|recv|rr|crr
 *   nettput -c HOST [-p PORT] [-n ATTEMPTS] wait
 *
 * The server listens on PORT (5201) of every address, prints `nettput:
 * serving on PORT` on standard output once it listens, and serves one
 * client at a time until it is killed. The client runs one test:
 *
 *   send  the client writes COUNT bytes, LEN (65536) at a time, and shuts
 *         its side down; the server counts what arrived and answers with
 *         the count, which must be COUNT.
 *   recv  the server writes COUNT bytes, LEN at a time, and closes; the
 *         client counts what arrived, which must be COUNT.
 *   rr    COUNT exchanges of a 1-byte request and a 1-byte answer on one
 *         connection with TCP_NODELAY.
 *   crr   COUNT transactions, each a connection of its own: connect, a
 *         1-byte request, a 1-byte answer and the server's close, so that
 *         the client's ports do not wait in TIME_WAIT.
 *   wait  how many attempts the path to the server takes to answer: an
 *         attempt is a connect that does not block, bounded at 200 ms, and
 *         on a connection one crr transaction, bounded at a second; a
 *         refused, unreachable or silent attempt is counted and the next
 *         starts 50 ms after it began, up to ATTEMPTS (2400). The bounds
 *         pace the attempts and judge nothing.
 *
 * COUNT is 8388608 for send and recv and 1000 for rr and crr. A connection
 * begins with HEADER bytes from the client, `nettput 2 MODE COUNT LEN`
 * padded with zeros, so the server takes no option for the test.
 *
 * The client prints one line on standard output:
 *
 *   nettput: result: MODE bytes=B seconds=S
 *   nettput: result: MODE count=N seconds=S
 *   nettput: result: wait attempts=N
 *   nettput: result: wait failed attempts=N
 *
 * the first for send and recv, the second for rr and crr. `seconds` is the
 * client's monotonic clock over the test, for the record alone. The status
 * is 0 when the test reached its count, 1 when it did not, with the reason
 * on standard error as `nettput: detail`, and 2 for a usage error. Either
 * side gives up on a peer that makes no progress for STALL_MS, so that a
 * path that dies in a test ends it.
 *
 * Building: `cc -O2 -o nettput nettput.c`, with `-static` for Rach2.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define HEADER          64         /* the bytes of the header and of the server's count */
#define PORT            5201       /* the default port */
#define LEN_MAX         (1u << 20) /* the longest write a test asks for */
#define COUNT_MAX       4294967295ul
#define STALL_MS        15000 /* a peer silent this long is given up */
#define WAIT_EVERY_MS   50    /* `wait`: from one attempt's start to the next's */
#define WAIT_CONNECT_MS 200   /* `wait`: the bound of an attempt's connect */
#define WAIT_ANSWER_MS  1000  /* `wait`: the bound of its answer */

enum mode { MODE_SEND, MODE_RECV, MODE_RR, MODE_CRR, MODE_WAIT, MODE_NONE };

static const char *const mode_names[] = {"send", "recv", "rr", "crr", "wait"};

/** Print `nettput: WHAT: the error's text` to standard error. */
static void complain(const char *what)
{
    fprintf(stderr, "nettput: %s: %s\n", what, strerror(errno));
}

/** The monotonic clock, in microseconds. */
static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/** The mode NAME names, or MODE_NONE. */
static enum mode mode_of(const char *name)
{
    int i;

    for (i = 0; i < MODE_NONE; i++) {
        if (strcmp(name, mode_names[i]) == 0) {
            return (enum mode)i;
        }
    }
    return MODE_NONE;
}

/** Wait until FD is ready for EVENTS for at most MS milliseconds: 1, or 0
 * with errno set, ETIMEDOUT when the time passed. */
static int ready_in(int fd, short events, int ms)
{
    struct pollfd p;
    int r;

    p.fd = fd;
    p.events = events;
    p.revents = 0;
    do {
        r = poll(&p, 1, ms);
    } while (r < 0 && errno == EINTR);
    if (r == 0) {
        errno = ETIMEDOUT;
    }
    return r > 0;
}

/** Read up to LEN bytes from FD once it is readable within STALL_MS: the
 * count, 0 at the end of the stream, or -1 with errno set. */
static ssize_t take(int fd, void *buf, size_t len)
{
    ssize_t n;

    if (!ready_in(fd, POLLIN, STALL_MS)) {
        return -1;
    }
    do {
        n = recv(fd, buf, len, 0);
    } while (n < 0 && errno == EINTR);
    return n;
}

/** Read exactly LEN bytes: 0, or -1 at an error or an early end. */
static int take_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    ssize_t n;

    while (len > 0) {
        n = take(fd, p, len);
        if (n <= 0) {
            if (n == 0) {
                errno = ECONNRESET;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/** Write LEN bytes, each part once FD is writable within STALL_MS: 0, or -1
 * with errno set. */
static int give_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    ssize_t n;

    while (len > 0) {
        if (!ready_in(fd, POLLOUT, STALL_MS)) {
            return -1;
        }
        n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/** Turn off Nagle's algorithm on FD, so that a 1-byte write goes at once. */
static void nodelay(int fd)
{
    int one = 1;

    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

/** One test as the client's header names it. */
struct test {
    enum mode mode;
    unsigned long count;
    unsigned len;
};

/** Parse a header into T: 0, or -1 for a header that names no test. */
static int header_of(const char *text, struct test *t)
{
    char name[16];
    unsigned long count;
    unsigned len;

    if (sscanf(text, "nettput 2 %15s %lu %u", name, &count, &len) != 3) {
        return -1;
    }
    t->mode = mode_of(name);
    if (t->mode == MODE_NONE || t->mode == MODE_WAIT || len == 0 || len > LEN_MAX) {
        return -1;
    }
    t->count = count;
    t->len = len;
    return 0;
}

/** Serve the test T on the accepted connection FD from PEER. */
static void serve(int fd, const char *peer, const struct test *t, char *buf)
{
    unsigned long long total = 0, left;
    char line[HEADER];
    unsigned long n = 0;
    ssize_t got;
    size_t part;

    switch (t->mode) {
    case MODE_SEND:
        while ((got = take(fd, buf, LEN_MAX)) > 0) {
            total += (unsigned long long)got;
        }
        if (got < 0) {
            complain("send: receive");
            return;
        }
        memset(line, 0, sizeof(line));
        snprintf(line, sizeof(line), "%llu", total);
        if (give_all(fd, line, sizeof(line)) < 0) {
            complain("send: answer");
        }
        fprintf(stderr, "nettput: served: send from %s bytes=%llu\n", peer, total);
        break;
    case MODE_RECV:
        memset(buf, 'r', t->len);
        for (left = t->count; left > 0; left -= part) {
            part = left < t->len ? (size_t)left : t->len;
            if (give_all(fd, buf, part) < 0) {
                complain("recv: send");
                return;
            }
        }
        fprintf(stderr, "nettput: served: recv to %s bytes=%lu\n", peer, t->count);
        break;
    case MODE_RR:
    case MODE_CRR:
        nodelay(fd);
        while ((got = take(fd, buf, 1)) == 1) {
            if (give_all(fd, buf, 1) < 0) {
                complain("rr: answer");
                return;
            }
            n++;
            if (t->mode == MODE_CRR) {
                break;
            }
        }
        if (got < 0) {
            complain("rr: request");
            return;
        }
        if (t->mode == MODE_RR) {
            fprintf(stderr, "nettput: served: rr with %s count=%lu\n", peer, n);
        }
        break;
    default:
        break;
    }
}

/** Listen on PORT and serve one client at a time, for ever: 1 when the
 * socket cannot be made. */
static int server(unsigned port)
{
    struct sockaddr_in addr, from;
    socklen_t fromlen;
    char header[HEADER + 1], peer[INET_ADDRSTRLEN];
    unsigned long crr = 0;
    struct test t;
    char *buf;
    int s, fd, one = 1;

    buf = malloc(LEN_MAX);
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (buf == NULL || s < 0) {
        complain("socket");
        return 1;
    }
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(s, 64) < 0) {
        complain("bind");
        return 1;
    }
    printf("nettput: serving on %u\n", port);
    fflush(stdout);
    for (;;) {
        fromlen = sizeof(from);
        fd = accept(s, (struct sockaddr *)&from, &fromlen);
        if (fd < 0) {
            if (errno != EINTR && errno != ECONNABORTED) {
                complain("accept");
            }
            continue;
        }
        inet_ntop(AF_INET, &from.sin_addr, peer, sizeof(peer));
        memset(header, 0, sizeof(header));
        if (take_all(fd, header, HEADER) < 0 || header_of(header, &t) < 0) {
            fprintf(stderr, "nettput: %s sent no test\n", peer);
        } else {
            serve(fd, peer, &t, buf);
            if (t.mode == MODE_CRR && ++crr % 100 == 0) {
                fprintf(stderr, "nettput: served: crr with %s count=%lu\n", peer, crr);
            }
        }
        close(fd);
    }
}

/** Open a connection to ADDR: the socket, or -1. */
static int dial(const struct sockaddr_in *addr)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/** One crr transaction on the connected FD after HEADER: 0, or -1. */
static int transact(int fd, const char *header)
{
    char c = 'q';

    nodelay(fd);
    if (give_all(fd, header, HEADER) < 0 || give_all(fd, &c, 1) < 0 || take_all(fd, &c, 1) < 0) {
        return -1;
    }
    return take(fd, &c, 1) == 0 ? 0 : -1;
}

/** One `wait` attempt at ADDR, bounded as the header comment says: 0 when
 * the server answered, -1 otherwise. */
static int attempt(const struct sockaddr_in *addr, const char *header)
{
    socklen_t len = sizeof(int);
    char c = 'q';
    int fd, err = 0, ok = -1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (ready_in(fd, POLLOUT, WAIT_CONNECT_MS) &&
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
        nodelay(fd);
        if (send(fd, header, HEADER, 0) == HEADER && send(fd, &c, 1, 0) == 1 &&
            ready_in(fd, POLLIN, WAIT_ANSWER_MS) && recv(fd, &c, 1, 0) == 1) {
            ok = 0;
        }
    }
    close(fd);
    return ok;
}

/** Sleep MS milliseconds. */
static void pause_ms(unsigned ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000l;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

/** The `wait` test: 0 once an attempt is answered, 1 after ATTEMPTS. */
static int wait_for(const struct sockaddr_in *addr, unsigned long attempts)
{
    char header[HEADER];
    unsigned long n;
    uint64_t began, spent;

    memset(header, 0, sizeof(header));
    snprintf(header, sizeof(header), "nettput 2 crr 1 1");
    for (n = 1; n <= attempts; n++) {
        began = now_us();
        if (attempt(addr, header) == 0) {
            printf("nettput: result: wait attempts=%lu\n", n);
            return 0;
        }
        spent = (now_us() - began) / 1000u;
        if (spent < WAIT_EVERY_MS) {
            pause_ms((unsigned)(WAIT_EVERY_MS - spent));
        }
    }
    printf("nettput: result: wait failed attempts=%lu\n", attempts);
    return 1;
}

/** Print the result of a test of MODE that reached VALUE, begun at START. */
static void result(enum mode mode, unsigned long long value, uint64_t start)
{
    double seconds = (double)(now_us() - start) / 1e6;

    printf("nettput: result: %s %s=%llu seconds=%.3f\n", mode_names[mode],
           mode == MODE_SEND || mode == MODE_RECV ? "bytes" : "count", value, seconds);
}

/** Run the client's test of MODE against ADDR: the exit status. */
static int client(const struct sockaddr_in *addr, enum mode mode, unsigned long count, unsigned len)
{
    unsigned long long total = 0, left;
    char header[HEADER], answer[HEADER + 1];
    unsigned long n;
    uint64_t start;
    ssize_t got;
    size_t part;
    char *buf, c = 'q';
    int fd;

    if (mode == MODE_WAIT) {
        return wait_for(addr, count);
    }
    memset(header, 0, sizeof(header));
    snprintf(header, sizeof(header), "nettput 2 %s %lu %u", mode_names[mode], count, len);
    start = now_us();
    if (mode == MODE_CRR) {
        for (n = 0; n < count; n++) {
            fd = dial(addr);
            if (fd < 0 || transact(fd, header) < 0) {
                complain("crr");
                if (fd >= 0) {
                    close(fd);
                }
                result(mode, n, start);
                return 1;
            }
            close(fd);
        }
        result(mode, n, start);
        return 0;
    }
    buf = malloc(len);
    fd = dial(addr);
    if (buf == NULL || fd < 0 || give_all(fd, header, HEADER) < 0) {
        complain("connect");
        return 1;
    }
    switch (mode) {
    case MODE_SEND:
        memset(buf, 's', len);
        for (left = count; left > 0; left -= part) {
            part = left < len ? (size_t)left : len;
            if (give_all(fd, buf, part) < 0) {
                complain("send");
                return 1;
            }
        }
        shutdown(fd, SHUT_WR);
        memset(answer, 0, sizeof(answer));
        if (take_all(fd, answer, HEADER) < 0) {
            complain("send: the server's count");
            return 1;
        }
        total = strtoull(answer, NULL, 10);
        break;
    case MODE_RECV:
        while ((got = take(fd, buf, len)) > 0) {
            total += (unsigned long long)got;
        }
        if (got < 0) {
            complain("recv");
        }
        break;
    case MODE_RR:
        nodelay(fd);
        for (n = 0; n < count; n++) {
            if (give_all(fd, &c, 1) < 0 || take_all(fd, &c, 1) < 0) {
                complain("rr");
                break;
            }
        }
        total = n;
        break;
    default:
        break;
    }
    close(fd);
    result(mode, total, start);
    if (total != count) {
        fprintf(stderr, "nettput: %s: %llu of %lu\n", mode_names[mode], total, count);
        return 1;
    }
    return 0;
}

/** Print the usage and exit with status 2. */
static void usage(void)
{
    fprintf(stderr, "usage: nettput -s [-p PORT]\n"
                    "       nettput -c HOST [-p PORT] [-n COUNT] [-l LEN] send|recv|rr|crr|wait\n");
    exit(2);
}

/** TEXT as a number from 1 to MAX, or the usage. */
static unsigned long number(const char *text, unsigned long max)
{
    unsigned long v;
    char *end;

    errno = 0;
    v = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || v == 0 || v > max) {
        usage();
    }
    return v;
}

int main(int argc, char **argv)
{
    static const unsigned long counts[] = {8388608ul, 8388608ul, 1000ul, 1000ul, 2400ul};
    struct sockaddr_in addr;
    const char *host = NULL;
    unsigned long count = 0;
    unsigned port = PORT, len = 65536;
    enum mode mode;
    int serving = 0, o;

    signal(SIGPIPE, SIG_IGN);
    while ((o = getopt(argc, argv, "sc:p:n:l:")) != -1) {
        switch (o) {
        case 's':
            serving = 1;
            break;
        case 'c':
            host = optarg;
            break;
        case 'p':
            port = (unsigned)number(optarg, 65535);
            break;
        case 'n':
            count = number(optarg, COUNT_MAX);
            break;
        case 'l':
            len = (unsigned)number(optarg, LEN_MAX);
            break;
        default:
            usage();
        }
    }
    if (serving) {
        if (host != NULL || optind != argc) {
            usage();
        }
        return server(port);
    }
    if (host == NULL || optind != argc - 1) {
        usage();
    }
    mode = mode_of(argv[optind]);
    if (mode == MODE_NONE) {
        usage();
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        usage();
    }
    return client(&addr, mode, count != 0 ? count : counts[mode], len);
}
