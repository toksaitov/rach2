/*
 * nettput.c: the TCP peer and client of the netguest suite, one program for
 * Rach2, Linux and macOS, whose tests end at a count of bytes, exchanges or
 * attempts, never at a time. It speaks IPv4 only and takes dotted addresses.
 *
 *   nettput -s [-b ADDR] [-p PORT]
 *   nettput -c HOST [-I IFACE] [-p PORT] [-n COUNT] [-l LEN] send|recv|rr|crr
 *   nettput -c HOST [-I IFACE] [-p PORT] [-n ATTEMPTS] wait|drop
 *
 * The server listens on PORT (5201) of ADDR, every address unless -b names
 * one, prints `nettput: serving on PORT` on standard output once it
 * listens, and serves each client in a process of its own until it is
 * killed; a client's process ends when its test does or when the server
 * has ended. With -I, every socket of the client is bound to the interface
 * IFACE (SO_BINDTODEVICE, where the system has it), so that its connections
 * leave by IFACE or not at all. The client runs one test:
 *
 *   send  the client writes COUNT bytes, LEN (65536) at a time, and shuts
 *         its side down; the server counts what arrived and answers with
 *         the count, which must be COUNT.
 *   recv  the server writes COUNT bytes, LEN at a time, and closes; the
 *         client counts what arrived up to the end of the stream, which
 *         must be COUNT.
 *   rr    COUNT exchanges of a 1-byte request and its echo on one
 *         connection with TCP_NODELAY, each request the exchange's number
 *         modulo 256.
 *   crr   COUNT transactions, each a connection of its own: connect, a
 *         1-byte request, its echo and the server's close, so that the
 *         client's ports do not wait in TIME_WAIT.
 *   wait  how many attempts the path to the server takes to answer: an
 *         attempt is a connect, which ends when the server answers, refuses
 *         or the kernel gives the handshake up, and on a connection one crr
 *         transaction; a failed attempt is counted and the next starts 50
 *         ms after it began, up to ATTEMPTS (2400).
 *   drop  ATTEMPTS (10) connects at once, each of which must go unanswered:
 *         no answer and no refusal, until the kernel gives its handshake up
 *         with ETIMEDOUT after its own retransmissions of the SYN.
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
 *   nettput: result: drop attempts=N
 *
 * the first for send and recv, the second for rr and crr. `seconds` is the
 * client's monotonic clock over the test, for the record alone. The status
 * is 0 when the test reached its count with every answer as sent and the
 * stream's end where the test puts it, 1 when it did not, with the reason on
 * standard error as `nettput: detail`, and 2 for a usage error. No side
 * waits for its peer against a clock: a path that dies in a test leaves the
 * client waiting until its caller ends it.
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HEADER        64         /* the bytes of the header and of the server's count */
#define PORT          5201       /* the default port */
#define LEN_MAX       (1u << 20) /* the longest write a test asks for */
#define COUNT_MAX     4294967295ul
#define WAIT_EVERY_MS 50 /* `wait`: from one attempt's start to the next's */
#define DROP_MAX      64 /* `drop`: the most attempts at once */

enum mode { MODE_SEND, MODE_RECV, MODE_RR, MODE_CRR, MODE_WAIT, MODE_DROP, MODE_NONE };

static const char *const mode_names[] = {"send", "recv", "rr", "crr", "wait", "drop"};

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

/* In a server's process for one client, the read end of a pipe whose write
 * end only the server holds, so that it reads as ended once the server has;
 * -1 elsewhere. */
static int server_alive = -1;

/** Wait until FD is ready for EVENTS, with no bound: 1, or 0 with errno
 * set. A server's process for one client exits once the server has. */
static int ready_in(int fd, short events)
{
    struct pollfd p[2];
    int r;

    p[0].fd = fd;
    p[0].events = events;
    p[0].revents = 0;
    p[1].fd = server_alive;
    p[1].events = POLLIN;
    p[1].revents = 0;
    do {
        r = poll(p, server_alive >= 0 ? 2 : 1, -1);
    } while (r < 0 && errno == EINTR);
    if (r > 0 && server_alive >= 0 && p[1].revents != 0) {
        _exit(0);
    }
    return r > 0;
}

/** Read up to LEN bytes from FD once it is readable: the count, 0 at the
 * end of the stream, or -1 with errno set. */
static ssize_t take(int fd, void *buf, size_t len)
{
    ssize_t n;

    if (!ready_in(fd, POLLIN)) {
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

/** Write LEN bytes, each part once FD is writable: 0, or -1 with errno set.
 * A blocking FD sends no more than what one poll found room for at a time,
 * which MSG_DONTWAIT asks of the kernel. */
static int give_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    ssize_t n;

    while (len > 0) {
        if (!ready_in(fd, POLLOUT)) {
            return -1;
        }
        n = send(fd, p, len, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
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
    if (t->mode == MODE_NONE || t->mode >= MODE_WAIT || len == 0 || len > LEN_MAX) {
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

/** Serve the client on the accepted connection FD from FROM, in the
 * process of its own that `server` made, and exit. */
static void serve_client(int fd, const struct sockaddr_in *from)
{
    char header[HEADER + 1], peer[INET_ADDRSTRLEN];
    struct test t;
    char *buf = malloc(LEN_MAX);

    inet_ntop(AF_INET, &from->sin_addr, peer, sizeof(peer));
    memset(header, 0, sizeof(header));
    if (buf == NULL) {
        complain("serve");
    } else if (take_all(fd, header, HEADER) < 0 || header_of(header, &t) < 0) {
        fprintf(stderr, "nettput: %s sent no test\n", peer);
    } else {
        serve(fd, peer, &t, buf);
    }
    close(fd);
    _exit(0);
}

/** Reap every client's process that has ended, at SIGCHLD. */
static void reap(int sig)
{
    int saved = errno;

    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    errno = saved;
}

/** Listen on PORT of BIND and serve each client in a process of its own,
 * for ever: 1 when the socket cannot be made. */
static int server(struct in_addr bind_addr, unsigned port)
{
    struct sockaddr_in addr, from;
    struct sigaction sa;
    socklen_t fromlen;
    int s, fd, life[2], one = 1;
    pid_t pid;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0 || pipe(life) < 0) {
        complain("socket");
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr = bind_addr;
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
        pid = fork();
        if (pid == 0) {
            close(s);
            close(life[1]);
            server_alive = life[0];
            serve_client(fd, &from);
        }
        if (pid < 0) {
            complain("fork");
        }
        close(fd);
    }
}

/* The interface -I names, or NULL. */
static const char *bind_iface;

/** A TCP socket, bound to -I's interface when -I names one: the socket, or
 * -1 with errno set. */
static int tcp_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0 || bind_iface == NULL) {
        return fd;
    }
#ifdef SO_BINDTODEVICE
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, bind_iface, (socklen_t)strlen(bind_iface)) ==
        0) {
        return fd;
    }
#else
    errno = ENOSYS;
#endif
    complain("-I");
    close(fd);
    return -1;
}

/** Open a connection to ADDR: the socket, or -1. */
static int dial(const struct sockaddr_in *addr)
{
    int fd = tcp_socket();

    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/** One crr transaction on the connected FD after HEADER: the request, its
 * echo and the end of the stream; 0, or -1. */
static int transact(int fd, const char *header)
{
    char c = 'q';

    nodelay(fd);
    if (give_all(fd, header, HEADER) < 0 || give_all(fd, &c, 1) < 0 || take_all(fd, &c, 1) < 0) {
        return -1;
    }
    if (c != 'q') {
        errno = EPROTO;
        return -1;
    }
    return take(fd, &c, 1) == 0 ? 0 : -1;
}

/** One `wait` attempt at ADDR: 0 when the server answered, -1 otherwise. */
static int attempt(const struct sockaddr_in *addr, const char *header)
{
    int fd, ok;

    fd = dial(addr);
    if (fd < 0) {
        return -1;
    }
    ok = transact(fd, header);
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

/** The `drop` test: ATTEMPTS connects to ADDR at once, each of which must
 * end with ETIMEDOUT; the exit status. */
static int drop_test(const struct sockaddr_in *addr, unsigned long attempts)
{
    struct pollfd p[DROP_MAX];
    unsigned long i, left;
    socklen_t len;
    int err, r, bad = 0;

    if (attempts > DROP_MAX) {
        fprintf(stderr, "nettput: drop: at most %d attempts\n", DROP_MAX);
        return 2;
    }
    for (i = 0; i < attempts; i++) {
        p[i].fd = tcp_socket();
        p[i].events = POLLOUT;
        p[i].revents = 0;
        if (p[i].fd < 0) {
            complain("drop: socket");
            return 1;
        }
        (void)fcntl(p[i].fd, F_SETFL, fcntl(p[i].fd, F_GETFL) | O_NONBLOCK);
        if (connect(p[i].fd, (const struct sockaddr *)addr, sizeof(*addr)) == 0) {
            fprintf(stderr, "nettput: drop: attempt %lu was answered\n", i + 1);
            return 1;
        }
        if (errno != EINPROGRESS) {
            fprintf(stderr, "nettput: drop: attempt %lu: %s\n", i + 1, strerror(errno));
            return 1;
        }
    }
    for (left = attempts; left > 0;) {
        do {
            r = poll(p, (nfds_t)attempts, -1);
        } while (r < 0 && errno == EINTR);
        if (r < 0) {
            complain("drop: poll");
            return 1;
        }
        for (i = 0; i < attempts; i++) {
            if (p[i].fd < 0 || p[i].revents == 0) {
                continue;
            }
            err = 0;
            len = sizeof(err);
            if (getsockopt(p[i].fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
                err = errno;
            }
            if (err == 0) {
                fprintf(stderr, "nettput: drop: attempt %lu was answered\n", i + 1);
                bad = 1;
            } else if (err != ETIMEDOUT) {
                fprintf(stderr, "nettput: drop: attempt %lu: %s\n", i + 1, strerror(err));
                bad = 1;
            }
            close(p[i].fd);
            p[i].fd = -1;
            left--;
        }
    }
    if (bad) {
        return 1;
    }
    printf("nettput: result: drop attempts=%lu\n", attempts);
    return 0;
}

/** Print the result of a test of MODE that reached VALUE, begun at START. */
static void result(enum mode mode, unsigned long long value, uint64_t start)
{
    double seconds = (double)(now_us() - start) / 1e6;

    printf("nettput: result: %s %s=%llu seconds=%.3f\n", mode_names[mode],
           mode == MODE_SEND || mode == MODE_RECV ? "bytes" : "count", value, seconds);
}

/** The client's `send` on the connected FD: COUNT bytes from BUF, LEN at a
 * time, then the server's count into *TOTAL; 1 when the count was read as a
 * number, 0 otherwise. */
static int send_test(int fd, char *buf, unsigned long count, unsigned len,
                     unsigned long long *total)
{
    char answer[HEADER + 1], *end;
    unsigned long long left;
    size_t part;

    memset(buf, 's', len);
    for (left = count; left > 0; left -= part) {
        part = left < len ? (size_t)left : len;
        if (give_all(fd, buf, part) < 0) {
            complain("send");
            return 0;
        }
    }
    shutdown(fd, SHUT_WR);
    memset(answer, 0, sizeof(answer));
    if (take_all(fd, answer, HEADER) < 0) {
        complain("send: the server's count");
        return 0;
    }
    errno = 0;
    *total = strtoull(answer, &end, 10);
    if (errno != 0 || end == answer || answer[0] < '0' || answer[0] > '9') {
        fprintf(stderr, "nettput: send: the server's count is no number\n");
        return 0;
    }
    for (; end < answer + HEADER; end++) {
        if (*end != '\0') {
            fprintf(stderr, "nettput: send: the server's count is followed by other bytes\n");
            return 0;
        }
    }
    if (take(fd, answer, 1) != 0) {
        fprintf(stderr, "nettput: send: the stream goes on after the server's count\n");
        return 0;
    }
    return 1;
}

/** The client's `recv` on the connected FD into BUF, LEN at a time, the
 * bytes up to the stream's end counted in *TOTAL: 1 when the stream ended,
 * 0 at an error. */
static int recv_test(int fd, char *buf, unsigned len, unsigned long long *total)
{
    ssize_t got;

    while ((got = take(fd, buf, len)) > 0) {
        *total += (unsigned long long)got;
    }
    if (got < 0) {
        complain("recv");
        return 0;
    }
    return 1;
}

/** The client's `rr` on the connected FD: up to COUNT exchanges, each
 * request the exchange's number modulo 256 and its echo; the exchanges
 * answered as sent. */
static unsigned long long rr_test(int fd, unsigned long count)
{
    unsigned long n;
    char c;

    nodelay(fd);
    for (n = 0; n < count; n++) {
        c = (char)(n & 0xFFu);
        if (give_all(fd, &c, 1) < 0 || take_all(fd, &c, 1) < 0) {
            complain("rr");
            break;
        }
        if (c != (char)(n & 0xFFu)) {
            fprintf(stderr, "nettput: rr: exchange %lu was answered with another byte\n", n);
            break;
        }
    }
    if (n == count) {
        shutdown(fd, SHUT_WR);
        if (take(fd, &c, 1) != 0) {
            fprintf(stderr, "nettput: rr: the server sent more than the echoes\n");
            n = 0;
        }
    }
    return n;
}

/** The client's `crr` against ADDR with HEADER: COUNT transactions, each on
 * a connection of its own, begun at START; the exit status. */
static int crr_test(const struct sockaddr_in *addr, const char *header, unsigned long count,
                    uint64_t start)
{
    unsigned long n;
    int fd;

    for (n = 0; n < count; n++) {
        fd = dial(addr);
        if (fd < 0 || transact(fd, header) < 0) {
            complain("crr");
            if (fd >= 0) {
                close(fd);
            }
            result(MODE_CRR, n, start);
            return 1;
        }
        close(fd);
    }
    result(MODE_CRR, n, start);
    return 0;
}

/** Run the client's test of MODE against ADDR: the exit status. */
static int client(const struct sockaddr_in *addr, enum mode mode, unsigned long count, unsigned len)
{
    unsigned long long total = 0;
    char header[HEADER];
    uint64_t start;
    char *buf;
    int fd, ok = 1;

    if (mode == MODE_WAIT) {
        return wait_for(addr, count);
    }
    if (mode == MODE_DROP) {
        return drop_test(addr, count);
    }
    memset(header, 0, sizeof(header));
    snprintf(header, sizeof(header), "nettput 2 %s %lu %u", mode_names[mode], count, len);
    start = now_us();
    if (mode == MODE_CRR) {
        return crr_test(addr, header, count, start);
    }
    buf = malloc(len);
    fd = dial(addr);
    if (buf == NULL || fd < 0 || give_all(fd, header, HEADER) < 0) {
        complain("connect");
        return 1;
    }
    if (mode == MODE_SEND) {
        ok = send_test(fd, buf, count, len, &total);
    } else if (mode == MODE_RECV) {
        ok = recv_test(fd, buf, len, &total);
    } else {
        total = rr_test(fd, count);
    }
    close(fd);
    result(mode, total, start);
    if (total != count) {
        fprintf(stderr, "nettput: %s: %llu of %lu\n", mode_names[mode], total, count);
        return 1;
    }
    return ok ? 0 : 1;
}

/** Print the usage and exit with status 2. */
static void usage(void)
{
    fprintf(stderr, "usage: nettput -s [-b ADDR] [-p PORT]\n"
                    "       nettput -c HOST [-I IFACE] [-p PORT] [-n COUNT] [-l LEN]\n"
                    "               send|recv|rr|crr|wait|drop\n");
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
    static const unsigned long counts[] = {8388608ul, 8388608ul, 1000ul, 1000ul, 2400ul, 10ul};
    struct sockaddr_in addr;
    const char *host = NULL, *bind_text = NULL;
    unsigned long count = 0;
    unsigned port = PORT, len = 65536;
    enum mode mode;
    int serving = 0, o;

    signal(SIGPIPE, SIG_IGN);
    while ((o = getopt(argc, argv, "sb:c:I:p:n:l:")) != -1) {
        switch (o) {
        case 's':
            serving = 1;
            break;
        case 'b':
            bind_text = optarg;
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
        case 'I':
            bind_iface = optarg;
            break;
        case 'l':
            len = (unsigned)number(optarg, LEN_MAX);
            break;
        default:
            usage();
        }
    }
    memset(&addr, 0, sizeof(addr));
    if (serving) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (host != NULL || bind_iface != NULL || optind != argc ||
            (bind_text != NULL && inet_pton(AF_INET, bind_text, &addr.sin_addr) != 1)) {
            usage();
        }
        return server(addr.sin_addr, port);
    }
    if (host == NULL || bind_text != NULL || optind != argc - 1) {
        usage();
    }
    mode = mode_of(argv[optind]);
    if (mode == MODE_NONE) {
        usage();
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        usage();
    }
    return client(&addr, mode, count != 0 ? count : counts[mode], len);
}
