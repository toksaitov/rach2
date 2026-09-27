/**
 * The creation flags of a socket, SOCK_CLOEXEC and SOCK_NONBLOCK, taken by
 * the kernel in the type of socket(2) and the flags of accept4(2), as Linux
 * takes them.
 *
 * The calls go through socketcall by number and not through musl, which sets
 * the flags itself with fcntl when the kernel refuses them and so would hide
 * a kernel that does. A type with another bit above its low four, and an
 * accept4 flag other than the two, are EINVAL. A socket made with
 * SOCK_CLOEXEC alone is close-on-exec and blocking; a listener made with both
 * flags has both, and an accept on it with nothing queued is EAGAIN. A
 * connection to it over 127.0.0.1 accepted with both flags gives a descriptor
 * with both, whose read with nothing to read is EAGAIN, and one accepted with
 * no flags has neither, and reports no IP options, which OpenSSH's server
 * reads before it talks to a client and ends a connection that has any.
 *
 * Prints "SOCKFLAGMUSL OK" on success and "SOCKFLAGMUSL FAIL (detail)"
 * otherwise.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

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

/** Make the socket operation `call` with the arguments `a` to `d`; its
 * result, or -1 with errno set. */
static long sc(int call, long a, long b, long c, long d)
{
    long args[4];

    args[0] = a;
    args[1] = b;
    args[2] = c;
    args[3] = d;
    return syscall(SYS_socketcall, call, args);
}

/** Whether `fd` is close-on-exec and non-blocking exactly as `cloexec` and
 * `nonblock` say. */
static int has(int fd, int cloexec, int nonblock)
{
    int fdflags = fcntl(fd, F_GETFD);
    int flflags = fcntl(fd, F_GETFL);

    return fdflags >= 0 && flflags >= 0 && ((fdflags & FD_CLOEXEC) != 0) == cloexec &&
           ((flflags & O_NONBLOCK) != 0) == nonblock;
}

/** Accept a connection on the non-blocking listener `ls` with `flags`,
 * trying again every 10 ms for at most a second while the handshake's last
 * segment is on its way through the loopback ring. */
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

/** A client connected to the listener's port on 127.0.0.1, or -1. */
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

/** A listener on the probe's port made with both flags, or -1. */
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

int main(void)
{
    unsigned char opts[40];
    socklen_t len = sizeof(opts);
    char c;
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

    printf("SOCKFLAGMUSL OK\n");

    return 0;
}
