/**
 * hostpeer: the host end of a connection that a guest test needs.
 *
 * A test of the guest's network needs a process at the host end of QEMU's
 * user network: a peer for a connection the guest is to show in its tables,
 * or a server whose answers show what the guest sent. hostpeer is that
 * process, in these jobs.
 *
 *   hostpeer hold PORT SECONDS
 *       Connect to 127.0.0.1:PORT and keep the connection open for SECONDS,
 *       so that a guest's tables have a live connection from outside to
 *       show.
 *
 *   hostpeer echo PORT SECONDS
 *       Listen on 127.0.0.1:PORT for both TCP and UDP for SECONDS and send
 *       back whatever arrives. The socket suite's probes reach it at the
 *       user network's gateway address, 10.0.2.2, send a string and require
 *       the same bytes back, which tests the guest's own transmit and
 *       receive paths. With a PORT of 0 the host chooses a port free for
 *       both transports, and the line that says the server is listening
 *       gives it. With SECONDS 0 it serves until a signal ends it, as
 *       etude's socket cases stop their service.
 *
 *   hostpeer send PORT TEXT
 *       Send TEXT as one UDP datagram to 127.0.0.1:PORT. The dgram cases
 *       send it to the port QEMU forwards to the guest's 7502, once the
 *       guest's probe says it waits for it.
 *
 *   hostpeer send-repeat PORT TEXT
 *       Send TEXT as a UDP datagram once each second until a termination
 *       signal ends the peer. The native deployment owns and reaps it.
 *
 * With HOSTPEER_ADDRESS set to an IPv4 address, each job uses that address
 * in place of 127.0.0.1: a native run on the Lenovo (tests/native/Readme.md)
 * reaches the echo server at the controller's own address on the LAN, and
 * sends the dgram case's datagram to the machine's.
 *
 * The exit status is 0 when the job was done, 1 when it could not be, and 2
 * for a usage error; the caller decides whether a failure is fatal. The end
 * of an echo server's time is a success.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET peer_socket;
#define PEER_INVALID     INVALID_SOCKET
#define peer_close       closesocket
#define peer_error       WSAGetLastError()
#define PEER_INTERRUPTED WSAEINTR
#define PEER_INUSE       WSAEADDRINUSE
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
typedef int peer_socket;
#define PEER_INVALID     (-1)
#define peer_close       close
#define peer_error       errno
#define PEER_INTERRUPTED EINTR
#define PEER_INUSE       EADDRINUSE
#endif

#ifdef _WIN32
static void socket_cleanup(void)
{
    WSACleanup();
}
#endif

/** Report the host's socket error. */
static void socket_error(const char *operation)
{
#ifdef _WIN32
    fprintf(stderr, "%s: socket error %d\n", operation, peer_error);
#else
    perror(operation);
#endif
}

/** Sleep for a whole number of seconds without overflowing milliseconds. */
static void pause_seconds(unsigned long seconds)
{
#ifdef _WIN32
    while (seconds) {
        DWORD part = seconds > 86400 ? 86400 : (DWORD)seconds;
        Sleep(part * 1000);
        seconds -= part;
    }
#else
    sleep((unsigned)seconds);
#endif
}

/** Select on native socket handles; Windows ignores the first argument. */
static int ready_sockets(peer_socket a, peer_socket b, peer_socket c, fd_set *set,
                         struct timeval *tv)
{
#ifdef _WIN32
    (void)a;
    (void)b;
    (void)c;
    return select(0, set, NULL, NULL, tv);
#else
    int max = a > b ? a : b;
    if (c > max)
        max = c;
    return select(max + 1, set, NULL, NULL, tv);
#endif
}

/* The address of every job, the loopback's unless HOSTPEER_ADDRESS names
 * another. */
static struct in_addr peer_address;
static char peer_text[INET_ADDRSTRLEN] = "127.0.0.1";

/** Fill `addr` with the jobs' address and `port`. */
static void fill_local(struct sockaddr_in *addr, long port)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((unsigned short)port);
    addr->sin_addr = peer_address;
}

/**
 * The `hold` job: connect to the loopback's `port` and keep the connection
 * open for `seconds`. Returns 0, or 1 with the reason printed when the
 * connection cannot be made.
 */
static int hold(long port, long seconds)
{
    struct sockaddr_in addr;
    peer_socket fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd == PEER_INVALID) {
        socket_error("hostpeer: socket");
        return 1;
    }
    fill_local(&addr, port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        socket_error("hostpeer: connect");
        peer_close(fd);
        return 1;
    }
    pause_seconds((unsigned long)seconds);
    peer_close(fd);
    return 0;
}

/** Seconds on the calendar clock, which `echo` measures its time on. */
static double now(void)
{
#ifndef _WIN32
    struct timeval tv;
#endif

#ifdef _WIN32
    FILETIME time;
    GetSystemTimePreciseAsFileTime(&time);
    return ((double)time.dwHighDateTime * 4294967296.0 + time.dwLowDateTime) / 1e7;
#else
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
#endif
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
static long bind_pair(peer_socket *listener, peer_socket *dgram, long port)
{
    struct sockaddr_in addr;
    socklen_t len;
    int one = 1;
    int tries;

    for (tries = 0; tries < 16; tries++) {
        *listener = socket(AF_INET, SOCK_STREAM, 0);
        *dgram = socket(AF_INET, SOCK_DGRAM, 0);
        if (*listener == PEER_INVALID || *dgram == PEER_INVALID) {
            socket_error("hostpeer: socket");
            return 0;
        }
        setsockopt(*listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
        fill_local(&addr, port);
        len = sizeof(addr);
        if (bind(*listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(*listener, 4) != 0 ||
            getsockname(*listener, (struct sockaddr *)&addr, &len) != 0) {
            socket_error("hostpeer: tcp bind");
            return 0;
        }
        if (bind(*dgram, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return ntohs(addr.sin_port);
        if (port != 0 || peer_error != PEER_INUSE)
            break;
        peer_close(*listener);
        peer_close(*dgram);
    }
    socket_error("hostpeer: udp bind");
    return 0;
}

/**
 * The `echo` job: serve TCP and UDP on the loopback's `port`, or on a port
 * the host chooses when it is 0, for `seconds`, or until a signal ends it
 * when that is 0, sending back what arrives.
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
    peer_socket listener, dgram, conn = PEER_INVALID;
    double deadline = now() + seconds;

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    port = bind_pair(&listener, &dgram, port);
    if (port == 0)
        return 1;
    if (seconds == 0)
        printf("hostpeer: echoing tcp and udp on %s:%ld until stopped\n", peer_text, port);
    else
        printf("hostpeer: echoing tcp and udp on %s:%ld for %lds\n", peer_text, port, seconds);
    fflush(stdout);

    while (seconds == 0 || now() < deadline) {
        char buf[2048];
        struct timeval tv;
        fd_set rfds;
        ssize_t n;

        FD_ZERO(&rfds);
        FD_SET(listener, &rfds);
        FD_SET(dgram, &rfds);
        if (conn != PEER_INVALID) {
            FD_SET(conn, &rfds);
        }
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if (ready_sockets(listener, dgram, conn, &rfds, &tv) < 0) {
            if (peer_error == PEER_INTERRUPTED)
                continue;
            socket_error("hostpeer: select");
            return 1;
        }
        if (FD_ISSET(dgram, &rfds)) {
            socklen_t flen = sizeof(from);

            n = recvfrom(dgram, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
            if (n > 0)
                sendto(dgram, buf, (size_t)n, 0, (struct sockaddr *)&from, flen);
        }
        if (FD_ISSET(listener, &rfds) && conn == PEER_INVALID)
            conn = accept(listener, NULL, NULL);
        if (conn != PEER_INVALID && FD_ISSET(conn, &rfds)) {
            n = recv(conn, buf, sizeof(buf), 0);
            if (n > 0) {
                ssize_t off = 0;

                while (off < n) {
                    ssize_t w = send(conn, buf + off, (int)(n - off), 0);

                    if (w <= 0)
                        break;
                    off += w;
                }
                if (off < n) {
                    peer_close(conn);
                    conn = PEER_INVALID;
                }
            } else {
                if (n == 0)
                    printf("hostpeer: tcp: the peer closed its end\n");
                fflush(stdout);
                peer_close(conn);
                conn = PEER_INVALID;
            }
        }
    }
    if (conn != PEER_INVALID)
        peer_close(conn);
    peer_close(listener);
    peer_close(dgram);
    return 0;
}

/**
 * The `send` job: send `text` as one UDP datagram to the loopback's `port`.
 * Returns 0, or 1 with the reason printed when it was not sent whole.
 */
static int send_text(long port, const char *text)
{
    struct sockaddr_in addr;
    size_t n = strlen(text);
    peer_socket fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd == PEER_INVALID) {
        socket_error("hostpeer: socket");
        return 1;
    }
    fill_local(&addr, port);
    if (sendto(fd, text, n, 0, (struct sockaddr *)&addr, sizeof(addr)) != (ssize_t)n) {
        socket_error("hostpeer: sendto");
        peer_close(fd);
        return 1;
    }
    peer_close(fd);
    printf("hostpeer: sent %lu bytes to %s:%ld\n", (unsigned long)n, peer_text, port);
    return 0;
}

/* A repeated datagram peer belongs to its deployment process. */
static volatile sig_atomic_t repeat_stop;
static void stop_repeat(int sig)
{
    repeat_stop = sig;
}
static int send_repeat(long port, const char *text)
{
    struct sockaddr_in address;
#ifndef _WIN32
    struct sigaction action;
#endif
    peer_socket fd = socket(AF_INET, SOCK_DGRAM, 0);
    int status = 0;
    size_t size = strlen(text);
    if (fd == PEER_INVALID)
        return 1;
#ifdef _WIN32
    signal(SIGTERM, stop_repeat);
    signal(SIGINT, stop_repeat);
    signal(SIGBREAK, stop_repeat);
#else
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_repeat;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
#endif
    fill_local(&address, port);
    printf("hostpeer: repeating %lu bytes to %s:%ld until stopped\n", (unsigned long)size,
           peer_text, port);
    fflush(stdout);
    while (!repeat_stop) {
#ifndef _WIN32
        struct timespec interval = {1, 0};
#endif
        if (sendto(fd, text, size, 0, (struct sockaddr *)&address, sizeof(address)) !=
            (ssize_t)size) {
            if (peer_error == PEER_INTERRUPTED)
                continue;
            socket_error("hostpeer: sendto");
            status = 1;
            break;
        }
        /* interval: the outside probe receives one datagram each second */
#ifdef _WIN32
        Sleep(1000);
#else
        while (!repeat_stop && nanosleep(&interval, &interval) && errno == EINTR) {
        }
#endif
    }
    peer_close(fd);
    return status;
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

/** Check the arguments and run the job they name. */
int main(int argc, char **argv)
{
    const char *address = getenv("HOSTPEER_ADDRESS");
    long port, seconds;

#ifdef _WIN32
    WSADATA winsock;
    if (WSAStartup(MAKEWORD(2, 2), &winsock)) {
        fprintf(stderr, "hostpeer: WSAStartup failed\n");
        return 1;
    }
    atexit(socket_cleanup);
#endif
    peer_address.s_addr = htonl(INADDR_LOOPBACK);
    if (address != NULL && address[0] != '\0') {
        if (inet_pton(AF_INET, address, &peer_address) != 1) {
            fprintf(stderr, "hostpeer: HOSTPEER_ADDRESS is not an IPv4 address\n");
            return 2;
        }
        snprintf(peer_text, sizeof(peer_text), "%s", address);
    }
    if (argc != 4) {
        fprintf(stderr, "usage: hostpeer hold|echo PORT SECONDS\n"
                        "       hostpeer send|send-repeat PORT TEXT\n");
        return 2;
    }
    if (strcmp(argv[1], "send") == 0 || strcmp(argv[1], "send-repeat") == 0) {
        if (!whole_number(argv[2], &port) || port < 1 || port > 65535) {
            fprintf(stderr, "hostpeer: a port in 1..65535\n");
            return 2;
        }
        return strcmp(argv[1], "send-repeat") == 0 ? send_repeat(port, argv[3])
                                                   : send_text(port, argv[3]);
    }
    if (!whole_number(argv[2], &port) || !whole_number(argv[3], &seconds) || port < 0 ||
        port > 65535 || seconds < 0 || (port == 0 && strcmp(argv[1], "echo") != 0)) {
        fprintf(stderr, "hostpeer: a port in 1..65535, or 0 for echo, and a whole "
                        "number of seconds\n");
        return 2;
    }
    if (strcmp(argv[1], "hold") == 0)
        return hold(port, seconds);
    if (strcmp(argv[1], "echo") == 0)
        return echo(port, seconds);
    fprintf(stderr, "hostpeer: unknown job '%s'; hold, echo or send\n", argv[1]);
    return 2;
}
