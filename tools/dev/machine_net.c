/**
 * Measure a TCP transfer on the host or Rach2 with the same receiver.
 *
 * local MIB WRITE-BYTES TIMEOUT forks a sender on 127.0.0.1. receive PORT
 * MIB TIMEOUT and send ADDRESS PORT MIB WRITE-BYTES split the ends across
 * machines. TIMEOUT is select's timeout in seconds; zero means no timeout.
 * The receiver starts its clock after accept, before releasing the sender
 * with one byte. Time spent starting the two commands is excluded.
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned char buffer[65536];
static volatile sig_atomic_t stopped;
static pid_t sender = -1;

/** Interrupt blocking calls so the parent can reap its local sender. */
static void stop(int sig)
{
    stopped = sig;
}

/** Report a failed operation and stop any sender this process created. */
static void fail(const char *what)
{
    int saved = errno;

    if (sender > 0) {
        kill(sender, SIGKILL);
        while (waitpid(sender, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    fprintf(stderr, "machine-net: %s: %s\n", what,
            stopped ? "interrupted or timed out" : strerror(saved));
    exit(1);
}

/** Require an entire decimal argument within the stated bounds. */
static unsigned number(const char *s, unsigned low, unsigned high)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(s, &end, 10);
    if (*s < '0' || *s > '9' || *end || errno || value < low || value > high) {
        fprintf(stderr, "machine-net: expected %u to %u, got %s\n", low, high, s);
        exit(2);
    }
    return (unsigned)value;
}

/** Read the monotonic clock in microseconds. */
static uint64_t usec(void)
{
    struct timespec t;

    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        fail("clock_gettime");
    return (uint64_t)t.tv_sec * 1000000 + (unsigned long)t.tv_nsec / 1000;
}

/** Preserve the requested write size across short writes. */
static void write_all(int fd, const void *data, size_t count)
{
    const unsigned char *p = data;

    while (count) {
        ssize_t n = write(fd, p, count);

        if (n < 0 && errno == EINTR && !stopped)
            continue;
        if (n <= 0 || stopped)
            fail("write");
        p += n;
        count -= (size_t)n;
    }
}

/** Retry an unrelated signal, but leave when the run's bound expires. */
static ssize_t read_some(int fd, void *data, size_t count)
{
    ssize_t n;

    do {
        n = read(fd, data, count);
    } while (n < 0 && errno == EINTR && !stopped);
    if (n < 0 || stopped)
        fail("read");
    return n;
}

/** Listen on an explicit port; Rach2 does not assign one at TCP listen. */
static int listen_on(struct sockaddr_in *address, unsigned port, int local)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;

    if (fd < 0)
        fail("socket");
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0)
        fail("setsockopt SO_REUSEADDR");
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(local ? 0x7f000001u : INADDR_ANY);
    address->sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)address, sizeof(*address)) != 0)
        fail("bind");
    if (listen(fd, 1) != 0)
        fail("listen");
    return fd;
}

/** Send only after the receiver has started its clock. */
static void transmit(const struct sockaddr_in *address, uint64_t bytes, unsigned chunk)
{
    unsigned char ready;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0 || connect(fd, (const struct sockaddr *)address, sizeof(*address)) != 0)
        fail("connect");
    if (read_some(fd, &ready, 1) != 1 || ready != 'R') {
        errno = EPROTO;
        fail("receiver handshake");
    }
    while (bytes) {
        size_t n = bytes < chunk ? (size_t)bytes : chunk;

        write_all(fd, buffer, n);
        bytes -= n;
    }
    if (close(fd) != 0)
        fail("close sender");
}

/** Measure complete reads, then require EOF at exactly the declared size. */
static void receive(int listener, uint64_t bytes, unsigned timeout)
{
    uint64_t start, elapsed, total = 0;
    unsigned long reads = 0;
    int fd = accept(listener, NULL, NULL);

    if (fd < 0)
        fail("accept");
    close(listener);
    if (fd >= FD_SETSIZE) {
        errno = EINVAL;
        fail("descriptor exceeds FD_SETSIZE");
    }
    start = usec();
    write_all(fd, "R", 1);
    while (total < bytes) {
        fd_set set;
        struct timeval tv;
        ssize_t n;
        int rc;

        FD_ZERO(&set);
        FD_SET(fd, &set);
        tv.tv_sec = timeout;
        tv.tv_usec = 0;
        rc = select(fd + 1, &set, NULL, NULL, timeout ? &tv : NULL);
        if (rc < 0 && errno == EINTR && !stopped)
            continue;
        if (rc == 0)
            errno = ETIMEDOUT;
        if (rc <= 0 || stopped)
            fail("select");
        n = read_some(fd, buffer, sizeof(buffer));
        if (n == 0 || (uint64_t)n > bytes - total) {
            errno = EPROTO;
            fail("transfer length");
        }
        total += (size_t)n;
        reads++;
    }
    elapsed = usec() - start;
    if (read_some(fd, buffer, 1) != 0) {
        errno = EPROTO;
        fail("data past transfer length");
    }
    close(fd);
    if (sender > 0) {
        int status;
        pid_t got;

        do {
            got = waitpid(sender, &status, 0);
        } while (got < 0 && errno == EINTR && !stopped);
        if (got < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            errno = ECHILD;
            fail("sender status");
        }
        sender = -1;
    }
    printf("machine-net: result: %llu bytes %llu us %lu reads select-timeout %u\n",
           (unsigned long long)total, (unsigned long long)elapsed, reads, timeout);
}

/** Bound every run, including a receiver whose peer never connects. */
int main(int argc, char **argv)
{
    struct sockaddr_in address;
    struct sigaction action;
    uint64_t bytes;
    unsigned chunk, timeout, port;
    int fd;

    memset(&action, 0, sizeof(action));
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    alarm(180);
    if (argc == 5 && strcmp(argv[1], "local") == 0) {
        bytes = (uint64_t)number(argv[2], 1, 4096) * 1048576;
        chunk = number(argv[3], 1, sizeof(buffer));
        timeout = number(argv[4], 0, 60);
        fd = listen_on(&address, 27157, 1);
        sender = fork();
        if (sender < 0)
            fail("fork");
        if (sender == 0) {
            close(fd);
            alarm(180);
            transmit(&address, bytes, chunk);
            _exit(0);
        }
        receive(fd, bytes, timeout);
    } else if (argc == 5 && strcmp(argv[1], "receive") == 0) {
        port = number(argv[2], 1024, 65535);
        bytes = (uint64_t)number(argv[3], 1, 4096) * 1048576;
        timeout = number(argv[4], 0, 60);
        fd = listen_on(&address, port, 0);
        printf("machine-net: listening: port %u\n", port);
        fflush(stdout);
        receive(fd, bytes, timeout);
    } else if (argc == 6 && strcmp(argv[1], "send") == 0) {
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        if (inet_pton(AF_INET, argv[2], &address.sin_addr) != 1) {
            fprintf(stderr, "machine-net: invalid IPv4 address: %s\n", argv[2]);
            return 2;
        }
        address.sin_port = htons((uint16_t)number(argv[3], 1024, 65535));
        bytes = (uint64_t)number(argv[4], 1, 4096) * 1048576;
        chunk = number(argv[5], 1, sizeof(buffer));
        transmit(&address, bytes, chunk);
    } else {
        fprintf(stderr, "machine-net: usage: local MIB WRITE-BYTES TIMEOUT\n"
                        "machine-net: usage: receive PORT MIB TIMEOUT\n"
                        "machine-net: usage: send ADDRESS PORT MIB WRITE-BYTES\n");
        return 2;
    }
    return 0;
}
