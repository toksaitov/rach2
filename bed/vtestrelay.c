/*
 * vtestrelay.c: carry Mesa's vtest protocol from its local socket to a vtest
 * server over TCP, hiding the version query so that both sides keep to
 * protocol 0, whose commands carry every byte on the stream.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define RELAY_VERSION "1"

/* The socket Mesa's vtest client connects to when VTEST_SOCKET_NAME is unset. */
#define VTEST_DEFAULT_SOCKET "/tmp/.virgl_test"

/*
 * A command is a header of two native 32-bit words, its length and its id,
 * then its payload. The length counts 32-bit words, except for
 * VCMD_CREATE_RENDERER, whose length counts the bytes of the name after it,
 * and VCMD_TRANSFER_PUT, whose data follows its eleven words and is counted
 * by the last of them in bytes (vtest_protocol.h).
 */
#define VTEST_HDR_BYTES            8
#define VCMD_GET_CAPS              1
#define VCMD_RESOURCE_CREATE       2
#define VCMD_RESOURCE_UNREF        3
#define VCMD_TRANSFER_GET          4
#define VCMD_TRANSFER_PUT          5
#define VCMD_SUBMIT_CMD            6
#define VCMD_RESOURCE_BUSY_WAIT    7
#define VCMD_CREATE_RENDERER       8
#define VCMD_GET_CAPS2             9
#define VCMD_PING_PROTOCOL_VERSION 10
#define VCMD_PROTOCOL_0_LAST       VCMD_PING_PROTOCOL_VERSION
#define VCMD_TRANSFER_HDR_WORDS    11
#define VCMD_TRANSFER_DATA_SIZE    10

/* The bytes each direction holds between a read and the write after it. */
#define RELAY_BUFFER 65536

/** One direction's bytes read and not yet written, d[lo] to d[hi - 1]. */
struct buffer {
    unsigned char d[RELAY_BUFFER];
    size_t lo;
    size_t hi;
    int eof;                  /* the source has ended */
    int shut;                 /* the destination's write side is shut */
    unsigned long long total; /* bytes written to the destination */
};

/** Where the parse of the client's commands stands. */
enum phase {
    PHASE_HEADER,   /* collecting a header */
    PHASE_TRANSFER, /* collecting a put's eleven words */
    PHASE_BODY      /* passing a payload on */
};

/** The relay of one connection: the client's commands and both buffers. */
struct relay {
    struct buffer up;   /* the client's commands, to the server */
    struct buffer down; /* the server's replies, to the client */
    enum phase phase;
    unsigned char part[VCMD_TRANSFER_HDR_WORDS * 4]; /* a header or put */
    size_t have;                                     /* bytes of part collected */
    size_t need;                                     /* bytes of part the phase waits for */
    unsigned long long body;                         /* payload bytes still to pass on */
    int started;                                     /* VCMD_CREATE_RENDERER has passed */
    unsigned long counts[VCMD_PROTOCOL_0_LAST + 1];  /* commands by id */
};

/* The names -v prints the counts under, by command id. */
static const char *const command_names[VCMD_PROTOCOL_0_LAST + 1] = {
    NULL,
    "caps",
    "create",
    "unref",
    "get",
    "put",
    "submit",
    "wait",
    "renderer",
    "caps2",
    "hidden version query",
};

static volatile sig_atomic_t stopping;
static int verbose;

/** Note a signal that ends the relay; accept() returns EINTR after it. */
static void on_stop(int sig)
{
    (void)sig;
    stopping = 1;
}

/** Do nothing, so that accept() returns EINTR and the children are reaped. */
static void on_child(int sig)
{
    (void)sig;
}

/** Print the usage line to standard error and return status 2. */
static int usage(void)
{
    fprintf(stderr, "usage: vtestrelay [-1v] [-s SOCKET] HOST PORT\n"
                    "       vtestrelay --version\n");
    return 2;
}

/** Read a native 32-bit word from P, which need not be aligned. */
static uint32_t word_at(const unsigned char *p)
{
    uint32_t w;

    memcpy(&w, p, sizeof(w));
    return w;
}

/** Return the bytes B can take after moving its contents to the front. */
static size_t buffer_room(struct buffer *b)
{
    if (b->lo > 0) {
        memmove(b->d, b->d + b->lo, b->hi - b->lo);
        b->hi -= b->lo;
        b->lo = 0;
    }
    return sizeof(b->d) - b->hi;
}

/** Append N bytes at P to B, which the caller has made room in. */
static void buffer_put(struct buffer *b, const void *p, size_t n)
{
    memcpy(b->d + b->hi, p, n);
    b->hi += n;
}

/** Start collecting the next command's header. */
static void expect_header(struct relay *r)
{
    r->phase = PHASE_HEADER;
    r->have = 0;
    r->need = VTEST_HDR_BYTES;
}

/** Pass on BODY payload bytes, or expect the next header when there are none. */
static void expect_body(struct relay *r, unsigned long long body)
{
    r->body = body;
    if (body > 0)
        r->phase = PHASE_BODY;
    else
        expect_header(r);
}

/**
 * Act on the header in r->part: drop a version query, pass any other command
 * of protocol 0 on and set what follows it.
 *
 * Return 0, or -1 with a diagnostic for a command that protocol 0 has not,
 * which only a client that was told of a newer protocol sends.
 */
static int begin_command(struct relay *r)
{
    uint32_t len = word_at(r->part);
    uint32_t id = word_at(r->part + 4);

    if (id == 0 || id > VCMD_PROTOCOL_0_LAST) {
        fprintf(stderr, "vtestrelay: refused: command %lu is not of protocol 0\n",
                (unsigned long)id);
        return -1;
    }
    if (!r->started && id != VCMD_CREATE_RENDERER) {
        fprintf(stderr, "vtestrelay: refused: command %lu before the renderer\n",
                (unsigned long)id);
        return -1;
    }
    r->counts[id]++;
    switch (id) {
    case VCMD_PING_PROTOCOL_VERSION:
        /*
         * A server that has no such command answers the busy wait the client
         * sends behind it first, and the client takes protocol 0 from that.
         */
        if (len != 0) {
            fprintf(stderr, "vtestrelay: refused: a version query of %lu words\n",
                    (unsigned long)len);
            return -1;
        }
        expect_header(r);
        return 0;
    case VCMD_CREATE_RENDERER:
        if (r->started) {
            fprintf(stderr, "vtestrelay: refused: a second renderer\n");
            return -1;
        }
        r->started = 1;
        buffer_put(&r->up, r->part, VTEST_HDR_BYTES);
        expect_body(r, len);
        return 0;
    case VCMD_TRANSFER_PUT:
        buffer_put(&r->up, r->part, VTEST_HDR_BYTES);
        r->phase = PHASE_TRANSFER;
        r->have = 0;
        r->need = VCMD_TRANSFER_HDR_WORDS * 4;
        return 0;
    default:
        buffer_put(&r->up, r->part, VTEST_HDR_BYTES);
        expect_body(r, 4ULL * len);
        return 0;
    }
}

/**
 * Parse N bytes of the client's commands at P and append what the server is
 * to see to r->up, whose room the caller has made at least N plus r->have.
 *
 * Return 0, or -1 when a command is refused.
 */
static int feed_commands(struct relay *r, const unsigned char *p, size_t n)
{
    size_t k;

    while (n > 0) {
        if (r->phase == PHASE_BODY) {
            k = n < r->body ? n : (size_t)r->body;
            buffer_put(&r->up, p, k);
            p += k;
            n -= k;
            r->body -= k;
            if (r->body == 0)
                expect_header(r);
            continue;
        }
        k = r->need - r->have;
        if (k > n)
            k = n;
        memcpy(r->part + r->have, p, k);
        r->have += k;
        p += k;
        n -= k;
        if (r->have < r->need)
            break;
        if (r->phase == PHASE_HEADER) {
            if (begin_command(r) < 0)
                return -1;
        } else {
            buffer_put(&r->up, r->part, r->need);
            expect_body(r, word_at(r->part + 4 * VCMD_TRANSFER_DATA_SIZE));
        }
    }
    return 0;
}

/**
 * Read what the client has sent, as much as r->up can take, and parse it.
 *
 * Return 0, or -1 on a read error or a refused command.
 */
static int read_client(struct relay *r, int fd)
{
    unsigned char chunk[RELAY_BUFFER];
    size_t room = buffer_room(&r->up);
    ssize_t n;

    /* Bytes held in r->part are written before or with the next ones. */
    if (room <= r->have)
        return 0;
    n = read(fd, chunk, room - r->have);
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return 0;
        perror("vtestrelay: read from the client");
        return -1;
    }
    if (n == 0) {
        r->up.eof = 1;
        return 0;
    }
    return feed_commands(r, chunk, (size_t)n);
}

/** Read what the server has sent into r->down; return 0, or -1 on an error. */
static int read_server(struct relay *r, int fd)
{
    size_t room = buffer_room(&r->down);
    ssize_t n;

    if (room == 0)
        return 0;
    n = read(fd, r->down.d + r->down.hi, room);
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return 0;
        perror("vtestrelay: read from the server");
        return -1;
    }
    if (n == 0)
        r->down.eof = 1;
    else
        r->down.hi += (size_t)n;
    return 0;
}

/**
 * Write what B holds to FD, and shut FD's write side once B's source has ended
 * and B is empty. Return 0, or -1 on an error.
 */
static int drain(struct buffer *b, int fd)
{
    ssize_t n;

    if (b->hi > b->lo) {
        n = write(fd, b->d + b->lo, b->hi - b->lo);
        if (n < 0)
            return errno == EAGAIN || errno == EINTR ? 0 : -1;
        b->lo += (size_t)n;
        b->total += (unsigned long long)n;
    }
    if (b->eof && b->hi == b->lo && !b->shut) {
        shutdown(fd, SHUT_WR);
        b->shut = 1;
    }
    return 0;
}

/** Print the connection's counts, as -v asks, in one line. */
static void report(const struct relay *r)
{
    char line[512];
    size_t n;
    unsigned long all = 0;
    int i;

    for (i = 1; i <= VCMD_PROTOCOL_0_LAST; i++)
        all += r->counts[i];
    n = (size_t)snprintf(line, sizeof(line), "%lu commands", all);
    for (i = 1; i <= VCMD_PROTOCOL_0_LAST && n < sizeof(line); i++)
        if (r->counts[i] > 0)
            n += (size_t)snprintf(line + n, sizeof(line) - n, ", %lu %s", r->counts[i],
                                  command_names[i]);
    fprintf(stderr, "vtestrelay: result: %s; %llu bytes to the server, %llu to the client\n", line,
            r->up.total, r->down.total);
}

/** Set O_NONBLOCK on FD's open file. */
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * Connect to HOST's PORT over TCP, with Nagle's delay off, since vtest is
 * a run of small requests that each wait for a reply.
 *
 * Return the socket, or -1 with a diagnostic.
 */
static int connect_server(const char *host, const char *port)
{
    struct addrinfo hints, *list, *a;
    int fd = -1, one = 1, err, saved = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    err = getaddrinfo(host, port, &hints, &list);
    if (err != 0) {
        fprintf(stderr, "vtestrelay: %s: %s\n", host, gai_strerror(err));
        return -1;
    }
    for (a = list; a != NULL; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            saved = errno;
            continue;
        }
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0)
            break;
        saved = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    if (fd < 0) {
        fprintf(stderr, "vtestrelay: %s:%s: %s\n", host, port, strerror(saved));
        return -1;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

/**
 * Set what PFD waits for on the client, its first entry, and the server, its
 * second: a read where the buffer behind it has room, a write where the
 * buffer before it holds bytes. A side with nothing to wait for is left out,
 * so that its hangup cannot make poll() spin.
 */
static void watch_sides(struct relay *r, struct pollfd *pfd, int client, int server)
{
    pfd[0].fd = client;
    pfd[0].events = 0;
    if (!r->up.eof && buffer_room(&r->up) > r->have)
        pfd[0].events |= POLLIN;
    if (r->down.hi > r->down.lo)
        pfd[0].events |= POLLOUT;
    pfd[1].fd = server;
    pfd[1].events = 0;
    if (!r->down.eof && buffer_room(&r->down) > 0)
        pfd[1].events |= POLLIN;
    if (r->up.hi > r->up.lo)
        pfd[1].events |= POLLOUT;
    if (pfd[0].events == 0)
        pfd[0].fd = -1;
    if (pfd[1].events == 0)
        pfd[1].fd = -1;
}

/**
 * Read from each side that poll() found ready in PFD and write what each
 * buffer holds. Return 0, or -1 with a diagnostic on an error or a refused
 * command.
 */
static int step(struct relay *r, const struct pollfd *pfd, int client, int server)
{
    if ((pfd[0].revents & POLLERR) || (pfd[1].revents & POLLERR)) {
        fprintf(stderr, "vtestrelay: error: a socket failed\n");
        return -1;
    }
    if ((pfd[0].revents & (POLLIN | POLLHUP)) && (pfd[0].events & POLLIN) &&
        read_client(r, client) < 0)
        return -1;
    if ((pfd[1].revents & (POLLIN | POLLHUP)) && (pfd[1].events & POLLIN) &&
        read_server(r, server) < 0)
        return -1;
    if (drain(&r->up, server) < 0 || drain(&r->down, client) < 0) {
        perror("vtestrelay: write");
        return -1;
    }
    return 0;
}

/**
 * Relay one client on CLIENT to the server at HOST's PORT until both
 * directions have ended. Closes CLIENT.
 *
 * Return 0 when both sides ended cleanly, or 1 on an error or a refused
 * command, which closes both.
 */
static int relay_client(int client, const char *host, const char *port)
{
    struct relay *r;
    struct pollfd pfd[2];
    int server, status = 1;

    r = calloc(1, sizeof(*r));
    server = connect_server(host, port);
    if (r == NULL || server < 0 || set_nonblocking(client) < 0 || set_nonblocking(server) < 0)
        goto out;
    expect_header(r);
    while (!(r->up.shut && r->down.shut)) {
        watch_sides(r, pfd, client, server);
        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("vtestrelay: poll");
            goto out;
        }
        if (step(r, pfd, client, server) < 0)
            goto out;
    }
    if (r->phase != PHASE_HEADER || r->have != 0)
        fprintf(stderr, "vtestrelay: warning: the client ended inside a command\n");
    status = 0;
out:
    if (verbose && r != NULL)
        report(r);
    free(r);
    if (server >= 0)
        close(server);
    close(client);
    return status;
}

/**
 * Bind and listen on the Unix socket at PATH. A socket file left by a relay
 * that has ended is removed first; a path where a server answers, or that
 * is not a socket, is refused.
 *
 * Return the listening socket, or -1 with a diagnostic.
 */
static int listen_unix(const char *path)
{
    struct sockaddr_un un;
    struct stat st;
    int fd;

    if (strlen(path) >= sizeof(un.sun_path)) {
        fprintf(stderr, "vtestrelay: %s: the path is too long for a socket\n", path);
        return -1;
    }
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    strcpy(un.sun_path, path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("vtestrelay: socket");
        return -1;
    }
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            fprintf(stderr, "vtestrelay: %s: exists and is not a socket\n", path);
            close(fd);
            return -1;
        }
        if (connect(fd, (struct sockaddr *)&un, sizeof(un)) == 0) {
            fprintf(stderr, "vtestrelay: %s: a server answers there\n", path);
            close(fd);
            return -1;
        }
        unlink(path);
    }
    if (bind(fd, (struct sockaddr *)&un, sizeof(un)) < 0 || listen(fd, 16) < 0) {
        fprintf(stderr, "vtestrelay: %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/** Install HANDLER for SIG without SA_RESTART, so that accept() returns EINTR. */
static void catch_signal(int sig, void (*handler)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

/** Reap every child that has ended. */
static void reap(void)
{
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/**
 * Accept clients on LISTENER and relay each in a child of its own until a
 * signal stops the relay, or relay only the first, in this process, when ONCE
 * is set. Return the exit status.
 */
static int serve(int listener, const char *host, const char *port, int once)
{
    pid_t pid;
    int client, err;

    while (!stopping) {
        client = accept(listener, NULL, NULL);
        err = errno;
        reap();
        if (client < 0) {
            if (err == EINTR || err == ECONNABORTED)
                continue;
            fprintf(stderr, "vtestrelay: accept: %s\n", strerror(err));
            return 1;
        }
        if (once) {
            close(listener);
            return relay_client(client, host, port);
        }
        pid = fork();
        if (pid == 0) {
            close(listener);
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGHUP, SIG_DFL);
            _exit(relay_client(client, host, port));
        }
        if (pid < 0)
            perror("vtestrelay: fork");
        close(client);
    }
    close(listener);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = getenv("VTEST_SOCKET_NAME");
    int once = 0, listener, status, c;

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("vtestrelay %s, vtest protocol 0\n", RELAY_VERSION);
        return 0;
    }
    if (path == NULL || *path == '\0')
        path = VTEST_DEFAULT_SOCKET;
    while ((c = getopt(argc, argv, "1s:v")) != -1) {
        switch (c) {
        case '1':
            once = 1;
            break;
        case 's':
            path = optarg;
            break;
        case 'v':
            verbose = 1;
            break;
        default:
            return usage();
        }
    }
    if (argc - optind != 2)
        return usage();
    signal(SIGPIPE, SIG_IGN);
    catch_signal(SIGTERM, on_stop);
    catch_signal(SIGINT, on_stop);
    catch_signal(SIGHUP, on_stop);
    catch_signal(SIGCHLD, on_child);
    listener = listen_unix(path);
    if (listener < 0)
        return 1;
    if (verbose)
        fprintf(stderr, "vtestrelay: listening on %s for %s:%s\n", path, argv[optind],
                argv[optind + 1]);
    status = serve(listener, argv[optind], argv[optind + 1], once);
    unlink(path);
    return status;
}
