/**
 * sockflagmusl: check that the kernel takes the socket creation flags,
 * SOCK_CLOEXEC and SOCK_NONBLOCK, in the type of socket(2) and the flags of
 * accept4(2), as Linux does.
 *
 * The calls go through socketcall by number, or on x86-64, which has no
 * socketcall, through the calls of their own, and not through musl's
 * wrappers: when the kernel fails a call with EINVAL for the flags, musl makes
 * it again without them and sets them with fcntl, which would hide a kernel
 * that lacks them. A type with another bit above its low four, and an
 * accept4 flag other than the two, are EINVAL. A socket made with
 * SOCK_CLOEXEC alone is close-on-exec and blocking; a listener made with both
 * flags has both, and an accept on it with nothing queued is EAGAIN. A
 * connection to it over 127.0.0.1 accepted with both flags gives a descriptor
 * with both, whose read with nothing to read is EAGAIN, and one accepted with
 * no flags has neither, and reports no IP options, which OpenSSH's server
 * reads before it talks to a client and ends a connection that has any.
 * FIONBIO sets and clears that connection's O_NONBLOCK, as OpenSSL's server
 * sets its sockets'. A client that sent its request and shut its sending half
 * reads the server's answer and then the end of input, as `nc` does, and a
 * send after the shutdown is EPIPE.
 *
 * It prints "SOCKFLAGMUSL OK" and exits 0 on success, or prints
 * "SOCKFLAGMUSL FAIL (detail)" and exits 1 at the first failure.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* The socketcall numbers of the calls; on x86-64, sc maps them to its calls. */
#define SC_SOCKET  1
#define SC_BIND    2
#define SC_CONNECT 3
#define SC_LISTEN  4
#define SC_ACCEPT4 18
#define PORT       7461

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("SOCKFLAGMUSL FAIL (%s)\n", what);

    return 1;
}

#if defined SYS_socketcall
/**
 * Make the socket operation `call`, an `SC_` number, with the arguments `a`
 * to `d` through socketcall. Returns the call's result, or -1 with errno set.
 */
static long sc(int call, long a, long b, long c, long d)
{
    long args[4];

    args[0] = a;
    args[1] = b;
    args[2] = c;
    args[3] = d;
    return syscall(SYS_socketcall, call, args);
}
#else
/**
 * Make the socket operation `call`, an `SC_` number, with the arguments `a`
 * to `d` through the call x86-64 has for it. Returns the call's result, or -1
 * with errno set.
 */
static long sc(int call, long a, long b, long c, long d)
{
    static const long nr[] = {0, SYS_socket, SYS_bind, SYS_connect, SYS_listen};

    return syscall(call == SC_ACCEPT4 ? SYS_accept4 : nr[call], a, b, c, d);
}
#endif

/**
 * Return 1 when the close-on-exec and non-blocking flags of `fd` match
 * `cloexec` and `nonblock`, each 0 or 1, and 0 when they do not or fcntl
 * fails.
 */
static int has(int fd, int cloexec, int nonblock)
{
    int fdflags = fcntl(fd, F_GETFD);
    int flflags = fcntl(fd, F_GETFL);

    return fdflags >= 0 && flflags >= 0 && ((fdflags & FD_CLOEXEC) != 0) == cloexec &&
           ((flflags & O_NONBLOCK) != 0) == nonblock;
}

/**
 * Accept a connection on the non-blocking listener `ls` with the accept4
 * `flags`, trying again every 10 ms for at most a second while the
 * handshake's last segment is on its way through the loopback ring. Returns
 * the new descriptor, or -1 with errno set.
 */
static int accept_within(int ls, int flags)
{
    struct timespec tick = {0, 10000000};
    long fd = -1;
    int tries;

    for (tries = 0; tries < 100; tries++) {
        fd = sc(SC_ACCEPT4, ls, 0, 0, flags);
        if (fd >= 0 || errno != EAGAIN) {
            break;
        }
        nanosleep(&tick, NULL);
    }
    return (int)fd;
}

/**
 * Connect a new client socket to `PORT` on 127.0.0.1. Returns its descriptor,
 * or -1.
 */
static int connect_one(void)
{
    struct sockaddr_in sa;
    int cs = (int)sc(SC_SOCKET, AF_INET, SOCK_STREAM, 0, 0);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (cs < 0 || sc(SC_CONNECT, cs, (long)&sa, sizeof(sa), 0) != 0) {
        return -1;
    }
    return cs;
}

/**
 * Make a listener on `PORT` with both creation flags. Returns its descriptor,
 * or -1.
 */
static int listener(void)
{
    struct sockaddr_in sa;
    int ls = (int)sc(SC_SOCKET, AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, 0);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (ls < 0 || sc(SC_BIND, ls, (long)&sa, sizeof(sa), 0) != 0 ||
        sc(SC_LISTEN, ls, 4, 0, 0) != 0) {
        return -1;
    }
    return ls;
}

/**
 * Check a half close over the loopback on the listener `ls`: the client sends
 * its request and shuts its sending half, a send after that fails with EPIPE,
 * the server reads the request and the end of input and only then answers,
 * and the client reads the answer and the end of input. Returns 0, or 1 after
 * the failure is printed. SIGPIPE is ignored from here on.
 */
static int half_close(int ls)
{
    char buf[16];
    size_t got = 0;
    ssize_t n;
    int cs = connect_one();
    int as = cs < 0 ? -1 : accept_within(ls, 0);

    signal(SIGPIPE, SIG_IGN);
    if (as < 0 || send(cs, "ask\n", 4, 0) != 4 || shutdown(cs, SHUT_WR) != 0) {
        return fail("a request and the shutdown of the sending half");
    }
    if (send(cs, "x", 1, 0) != -1 || errno != EPIPE) {
        return fail("a send after shutdown(SHUT_WR) was not EPIPE");
    }
    while ((n = recv(as, buf + got, sizeof(buf) - got, 0)) > 0) {
        got += (size_t)n;
    }
    if (n != 0 || got != 4 || send(as, "answer", 6, 0) != 6) {
        return fail("the server did not read the request to its end and answer");
    }
    close(as);
    got = 0;
    while ((n = recv(cs, buf + got, sizeof(buf) - got, 0)) > 0) {
        got += (size_t)n;
    }
    if (n != 0 || got != 6 || memcmp(buf, "answer", 6) != 0) {
        return fail("the answer after shutdown(SHUT_WR) did not arrive");
    }
    close(cs);
    return 0;
}

int main(void)
{
    unsigned char opts[40];
    socklen_t len = sizeof(opts);
    char c;
    int on;
    int ds;
    int ls;
    int as;

    if (sc(SC_SOCKET, AF_INET, SOCK_STREAM | 0x10, 0, 0) != -1 || errno != EINVAL) {
        return fail("a type bit other than SOCK_CLOEXEC and SOCK_NONBLOCK was not EINVAL");
    }
    ds = (int)sc(SC_SOCKET, AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0, 0);
    if (ds < 0 || !has(ds, 1, 0)) {
        return fail("a socket made with SOCK_CLOEXEC is not close-on-exec and blocking");
    }
    ls = listener();
    if (ls < 0 || !has(ls, 1, 1)) {
        return fail("a listener made with both flags does not have both");
    }
    if (sc(SC_ACCEPT4, ls, 0, 0, 0) != -1 || errno != EAGAIN) {
        return fail("an accept on an empty non-blocking listener was not EAGAIN");
    }
    if (sc(SC_ACCEPT4, ls, 0, 0, 0x1) != -1 || errno != EINVAL) {
        return fail("an accept4 flag other than SOCK_CLOEXEC and SOCK_NONBLOCK was not EINVAL");
    }
    as = connect_one() < 0 ? -1 : accept_within(ls, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (as < 0 || !has(as, 1, 1)) {
        return fail("a connection accepted with both flags does not have both");
    }
    if (read(as, &c, 1) != -1 || errno != EAGAIN) {
        return fail("a read with nothing to read on a SOCK_NONBLOCK connection was not EAGAIN");
    }
    as = connect_one() < 0 ? -1 : accept_within(ls, 0);
    if (as < 0 || !has(as, 0, 0)) {
        return fail("a connection accepted with no flags has one");
    }
    if (getsockopt(as, IPPROTO_IP, IP_OPTIONS, opts, &len) != 0 || len != 0) {
        return fail("an accepted connection reported IP options");
    }
    on = 1;
    if (ioctl(as, FIONBIO, &on) != 0 || !has(as, 0, 1)) {
        return fail("FIONBIO did not make a connection non-blocking");
    }
    on = 0;
    if (ioctl(as, FIONBIO, &on) != 0 || !has(as, 0, 0)) {
        return fail("FIONBIO did not make a connection blocking again");
    }
    if (half_close(ls) != 0) {
        return 1;
    }

    printf("SOCKFLAGMUSL OK\n");

    return 0;
}
