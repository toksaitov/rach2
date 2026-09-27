/**
 * AF_UNIX sockets as GNU libc and its programs use them.
 *
 * A stream pair carries bytes both ways, ends with the end of input when one
 * end closes, and gives EPIPE to a write after that; shutdown(SHUT_WR) ends
 * the peer's input only. A datagram pair and a sequenced-packet pair keep
 * the boundaries of messages, and a datagram too long for the buffer is cut
 * with MSG_TRUNC. A descriptor sent with SCM_RIGHTS reaches a child that has
 * closed its own ends of the pipe, and the child writes through it. A child
 * that becomes user and group 1000 reads the pair's creator from SO_PEERCRED,
 * and the byte it writes reaches the creator's end, which has SO_PASSCRED, with
 * the child's pid, uid and gid. A listener bound to a path makes a socket file,
 * accepts a connection queued before the accept, and both ends report the
 * names; a second bind of the path fails with EADDRINUSE, and a connect to the
 * file after the listener is gone fails with ECONNREFUSED. An abstract name
 * works without a file. A datagram sent to a name arrives with the sender's
 * name. A nonblocking receive on an empty socket fails with EAGAIN, and poll
 * reports the socket readable once a byte arrives. A descriptor sent with
 * SCM_RIGHTS reaches a child, which writes through it. SO_PEERCRED reports the
 * peer, and SO_PASSCRED brings the sender's credentials. An accept given an
 * address and no length pointer fails with EFAULT. A listener shut down for
 * reading gives up the connection queued before, then fails an accept with
 * EINVAL and refuses a connect. A connect waiting for room in a full queue
 * finishes when listen makes the backlog larger. A datagram socket refuses a
 * connect with EPERM while it is connected to another. A datagram socket
 * refuses a connect with EPERM while it is connected to another; one that
 * connects elsewhere or disconnects drops its queued messages, and the peer it
 * left gets ECONNRESET.
 *
 * Prints "UNIXMUSL OK" on success and "UNIXMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SOCK_PATH  "/tmp/unixmusl.sock"
#define DGRAM_PATH "/tmp/unixmusl.dg"
#define FROM_PATH  "/tmp/unixmusl.from"

static int fail(const char *what)
{
    printf("UNIXMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

static socklen_t path_addr(struct sockaddr_un *a, const char *path)
{
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    strcpy(a->sun_path, path);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
}

/* Send the descriptor `fd` over `s` with one byte of data. */
static int send_fd(int s, int fd)
{
    union {
        struct cmsghdr h;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctl;
    struct msghdr msg;
    struct iovec iov;
    struct cmsghdr *c;
    char byte = 'F';

    memset(&msg, 0, sizeof(msg));
    memset(&ctl, 0, sizeof(ctl));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl.buf;
    msg.msg_controllen = sizeof(ctl.buf);
    c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return sendmsg(s, &msg, 0) == 1 ? 0 : -1;
}

/* The descriptor sent over `s`, or -1. */
static int recv_fd(int s)
{
    union {
        struct cmsghdr h;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctl;
    struct msghdr msg;
    struct iovec iov;
    struct cmsghdr *c;
    char byte;
    int fd;

    memset(&msg, 0, sizeof(msg));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl.buf;
    msg.msg_controllen = sizeof(ctl.buf);
    if (recvmsg(s, &msg, 0) != 1) {
        return -1;
    }
    c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
        return -1;
    }
    memcpy(&fd, CMSG_DATA(c), sizeof(int));
    return fd;
}

static int stream_pair(void)
{
    int sv[2];
    char buf[16];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return fail("socketpair stream");
    }
    if (write(sv[0], "hello", 5) != 5 || read(sv[1], buf, sizeof(buf)) != 5 ||
        memcmp(buf, "hello", 5) != 0) {
        return fail("stream one way");
    }
    if (write(sv[1], "back", 4) != 4 || read(sv[0], buf, sizeof(buf)) != 4 ||
        memcmp(buf, "back", 4) != 0) {
        return fail("stream the other way");
    }
    close(sv[1]);
    if (read(sv[0], buf, sizeof(buf)) != 0) {
        return fail("no end of input after the peer closed");
    }
    if (write(sv[0], "x", 1) != -1 || errno != EPIPE) {
        return fail("a write after the peer closed was not EPIPE");
    }
    close(sv[0]);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 || shutdown(sv[0], SHUT_WR) != 0) {
        return fail("shutdown");
    }
    if (read(sv[1], buf, sizeof(buf)) != 0) {
        return fail("no end of input after SHUT_WR");
    }
    if (write(sv[1], "r", 1) != 1 || read(sv[0], buf, sizeof(buf)) != 1) {
        return fail("the other direction after SHUT_WR");
    }
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int message_pairs(void)
{
    struct msghdr msg;
    struct iovec iov;
    char buf[16];
    int sv[2];

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        return fail("socketpair dgram");
    }
    if (send(sv[0], "one", 3, 0) != 3 || send(sv[0], "second", 6, 0) != 6) {
        return fail("dgram send");
    }
    if (recv(sv[1], buf, 2, 0) != 2 || recv(sv[1], buf, sizeof(buf), 0) != 6 ||
        memcmp(buf, "second", 6) != 0) {
        return fail("a datagram's boundary");
    }
    if (send(sv[0], "abcdef", 6, 0) != 6) {
        return fail("dgram send 2");
    }
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = buf;
    iov.iov_len = 3;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (recvmsg(sv[1], &msg, 0) != 3 || !(msg.msg_flags & MSG_TRUNC)) {
        return fail("MSG_TRUNC");
    }
    close(sv[0]);
    close(sv[1]);

    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        return fail("socketpair seqpacket");
    }
    if (send(sv[0], "ab", 2, 0) != 2 || send(sv[0], "cde", 3, 0) != 3 ||
        recv(sv[1], buf, sizeof(buf), 0) != 2 || recv(sv[1], buf, sizeof(buf), 0) != 3) {
        return fail("a packet's boundary");
    }
    close(sv[0]);
    if (recv(sv[1], buf, sizeof(buf), 0) != 0) {
        return fail("no end of input on a packet socket");
    }
    close(sv[1]);
    return 0;
}

/**
 * In a child of the pair's creator, whose effective ids are `uid` and `gid`:
 * become user and group 1000, read the creator's credentials from `mine`
 * with SO_PEERCRED, which a pair saves when it is made, and write a byte for
 * the creator to read with the child's own. 0, or the step that failed.
 */
static int peer_child(int mine, pid_t creator, uid_t uid, gid_t gid)
{
    struct ucred cr;
    socklen_t len = sizeof(cr);

    if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
        return 2;
    }
    if (getsockopt(mine, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0 || len != sizeof(cr) ||
        cr.pid != creator || cr.uid != uid || cr.gid != gid) {
        return 3;
    }
    return write(mine, "c", 1) == 1 ? 0 : 4;
}

/**
 * The credentials a pair carries, between its creator and a child of other
 * ids: the child's SO_PEERCRED reports the creator, and the byte the child
 * writes reaches the creator's end, which has SO_PASSCRED, with the child's
 * pid, uid and gid in SCM_CREDENTIALS.
 */
static int creds(void)
{
    struct ucred cr;
    union {
        struct cmsghdr h;
        char buf[CMSG_SPACE(sizeof(struct ucred))];
    } ctl;
    struct msghdr msg;
    struct iovec iov;
    struct cmsghdr *c;
    pid_t creator = getpid();
    pid_t pid;
    int sv[2], n, status = 0;
    int on = 1;
    char buf[8];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 ||
        setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) != 0) {
        return fail("the pair for credentials");
    }
    pid = fork();
    if (pid == 0) {
        close(sv[0]);
        _exit(peer_child(sv[1], creator, geteuid(), getegid()));
    }
    close(sv[1]);
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = buf;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl.buf;
    msg.msg_controllen = sizeof(ctl.buf);
    n = pid < 0 ? -1 : (int)recvmsg(sv[0], &msg, 0);
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 3
                        ? "the child's SO_PEERCRED did not report the pair's creator"
                        : "the child of other ids");
    }
    c = CMSG_FIRSTHDR(&msg);
    if (n != 1 || !c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_CREDENTIALS) {
        return fail("no SCM_CREDENTIALS with the child's byte");
    }
    memcpy(&cr, CMSG_DATA(c), sizeof(cr));
    if (cr.pid != pid || cr.uid != 1000 || cr.gid != 1000) {
        return fail("the SCM_CREDENTIALS values are not the sender's");
    }
    close(sv[0]);
    return 0;
}

static int rights_and_creds(void)
{
    int sv[2], p[2];
    char buf[8];
    int status;
    pid_t pid;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 || pipe(p) != 0) {
        return fail("setup for SCM_RIGHTS");
    }
    pid = fork();
    if (pid == 0) {
        int fd;

        close(sv[0]);
        close(p[0]);
        close(p[1]);
        fd = recv_fd(sv[1]);
        _exit(fd >= 0 && write(fd, "via", 3) == 3 ? 0 : 1);
    }
    close(sv[1]);
    if (send_fd(sv[0], p[1]) != 0) {
        return fail("sendmsg SCM_RIGHTS");
    }
    close(p[1]);
    if (read(p[0], buf, sizeof(buf)) != 3 || memcmp(buf, "via", 3) != 0) {
        return fail("the child did not write through the descriptor it received");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the receiving child");
    }
    close(p[0]);
    close(sv[0]);
    return creds();
}

static int named(void)
{
    struct sockaddr_un a, got;
    socklen_t alen, glen;
    struct stat st;
    char buf[8];
    int l, c, s, extra;

    unlink(SOCK_PATH);
    alen = path_addr(&a, SOCK_PATH);
    l = socket(AF_UNIX, SOCK_STREAM, 0);
    if (l < 0 || bind(l, (struct sockaddr *)&a, alen) != 0 || listen(l, 4) != 0) {
        return fail("bind and listen on a path");
    }
    if (stat(SOCK_PATH, &st) != 0 || !S_ISSOCK(st.st_mode)) {
        return fail("bind made no socket file");
    }
    extra = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(extra, (struct sockaddr *)&a, alen) != -1 || errno != EADDRINUSE) {
        return fail("a second bind of the path was not EADDRINUSE");
    }
    close(extra);
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c < 0 || connect(c, (struct sockaddr *)&a, alen) != 0) {
        return fail("connect to the path");
    }
    glen = sizeof(got);
    s = accept(l, (struct sockaddr *)&got, &glen);
    if (s < 0 || glen != offsetof(struct sockaddr_un, sun_path)) {
        return fail("accept, or the unnamed client's address");
    }
    glen = sizeof(got);
    if (getsockname(s, (struct sockaddr *)&got, &glen) != 0 ||
        strcmp(got.sun_path, SOCK_PATH) != 0) {
        return fail("getsockname of the accepted socket");
    }
    glen = sizeof(got);
    if (getpeername(c, (struct sockaddr *)&got, &glen) != 0 ||
        strcmp(got.sun_path, SOCK_PATH) != 0) {
        return fail("getpeername of the client");
    }
    if (write(c, "ping", 4) != 4 || read(s, buf, sizeof(buf)) != 4) {
        return fail("data over the named connection");
    }
    close(s);
    close(c);
    close(l);
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(c, (struct sockaddr *)&a, alen) != -1 || errno != ECONNREFUSED) {
        return fail("a connect to a socket file with no listener was not ECONNREFUSED");
    }
    close(c);
    unlink(SOCK_PATH);

    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path + 1, "unixmusl", 8);
    alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 9);
    l = socket(AF_UNIX, SOCK_STREAM, 0);
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(l, (struct sockaddr *)&a, alen) != 0 || listen(l, 1) != 0 ||
        connect(c, (struct sockaddr *)&a, alen) != 0) {
        return fail("an abstract name");
    }
    s = accept(l, NULL, NULL);
    if (s < 0 || write(c, "a", 1) != 1 || read(s, buf, 1) != 1) {
        return fail("data over the abstract connection");
    }
    close(s);
    close(c);
    close(l);
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(c, (struct sockaddr *)&a, alen) != -1 || errno != ECONNREFUSED) {
        return fail("an abstract name outlived its socket");
    }
    close(c);
    return 0;
}

/* The address of the abstract name `name`, and its length. */
static socklen_t abstract_addr(struct sockaddr_un *a, const char *name)
{
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    memcpy(a->sun_path + 1, name, strlen(name));
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name));
}

/* A listener on the abstract name `name`, and a client connected to it. */
static int abstract_pair(const char *name, int *l, int *c)
{
    struct sockaddr_un a;
    socklen_t alen = abstract_addr(&a, name);

    *l = socket(AF_UNIX, SOCK_STREAM, 0);
    *c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (*l < 0 || *c < 0 || bind(*l, (struct sockaddr *)&a, alen) != 0 || listen(*l, 4) != 0 ||
        connect(*c, (struct sockaddr *)&a, alen) != 0) {
        return -1;
    }
    return 0;
}

static int listening(void)
{
    struct sockaddr_un got;
    socklen_t glen;
    int l, c, s, late;

    if (abstract_pair("unixmusl.acc", &l, &c) != 0) {
        return fail("a listener for accept");
    }
    if (accept(l, (struct sockaddr *)&got, NULL) != -1 || errno != EFAULT) {
        return fail("an accept with an address and no length was not EFAULT");
    }
    close(c);
    close(l);

    if (abstract_pair("unixmusl.shut", &l, &c) != 0 || shutdown(l, SHUT_RD) != 0) {
        return fail("a listener shut down for reading");
    }
    s = accept(l, NULL, NULL);
    if (s < 0) {
        return fail("the connection queued before SHUT_RD was not accepted");
    }
    if (accept(l, NULL, NULL) != -1 || errno != EINVAL) {
        return fail("an accept with none queued after SHUT_RD was not EINVAL");
    }
    glen = abstract_addr(&got, "unixmusl.shut");
    late = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(late, (struct sockaddr *)&got, glen) != -1 || errno != ECONNREFUSED) {
        return fail("a connect to a listener shut down for reading was not ECONNREFUSED");
    }
    close(late);
    close(s);
    close(c);
    close(l);
    return 0;
}

/* A connect waiting for room in a listener's queue finishes when listen
 * makes the backlog larger; SO_SNDTIMEO bounds the wait of a kernel that
 * does not wake it. */
static int backlog_growth(void)
{
    struct timeval limit = {5, 0};
    struct timespec pause = {0, 100000000};
    struct sockaddr_un a;
    socklen_t alen = abstract_addr(&a, "unixmusl.backlog");
    int l = socket(AF_UNIX, SOCK_STREAM, 0);
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    int status;
    pid_t pid;

    if (bind(l, (struct sockaddr *)&a, alen) != 0 || listen(l, 0) != 0 ||
        connect(c, (struct sockaddr *)&a, alen) != 0) {
        return fail("a listener with a backlog of 0");
    }
    pid = fork();
    if (pid == 0) {
        int w = socket(AF_UNIX, SOCK_STREAM, 0);

        setsockopt(w, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
        _exit(connect(w, (struct sockaddr *)&a, alen) == 0 ? 0 : 1);
    }
    nanosleep(&pause, NULL);
    if (listen(l, 10) != 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail("a larger backlog did not let a waiting connect finish");
    }
    close(c);
    close(l);
    return 0;
}

static int datagram_peers(void)
{
    struct sockaddr_un a;
    socklen_t alen = abstract_addr(&a, "unixmusl.pair");
    char buf[8];
    int sv[2];
    int other;

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0 ||
        bind(sv[0], (struct sockaddr *)&a, alen) != 0) {
        return fail("a named datagram pair");
    }
    other = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (connect(other, (struct sockaddr *)&a, alen) != -1 || errno != EPERM) {
        return fail("a connect to a datagram socket connected elsewhere was not EPERM");
    }
    alen = abstract_addr(&a, "unixmusl.other");
    if (bind(other, (struct sockaddr *)&a, alen) != 0 || send(sv[0], "old", 3, 0) != 3 ||
        connect(sv[1], (struct sockaddr *)&a, alen) != 0) {
        return fail("a datagram socket connected to another peer");
    }
    if (recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT) != -1 || errno != EAGAIN) {
        return fail("a reconnected datagram socket kept its old peer's message");
    }
    if (recv(sv[0], buf, sizeof(buf), MSG_DONTWAIT) != -1 || errno != ECONNRESET) {
        return fail("the peer left with a message lost was not ECONNRESET");
    }
    close(other);
    close(sv[0]);
    close(sv[1]);

    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNSPEC;
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0 || send(sv[0], "old", 3, 0) != 3 ||
        connect(sv[1], (struct sockaddr *)&a, sizeof(a.sun_family)) != 0) {
        return fail("a datagram socket disconnected with AF_UNSPEC");
    }
    if (recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT) != -1 || errno != EAGAIN) {
        return fail("a disconnected datagram socket kept its old peer's message");
    }
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int datagram_names(void)
{
    struct sockaddr_un to, from, got;
    socklen_t tolen, fromlen, glen;
    char buf[8];
    int r, s;

    unlink(DGRAM_PATH);
    unlink(FROM_PATH);
    tolen = path_addr(&to, DGRAM_PATH);
    fromlen = path_addr(&from, FROM_PATH);
    r = socket(AF_UNIX, SOCK_DGRAM, 0);
    s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (bind(r, (struct sockaddr *)&to, tolen) != 0 ||
        bind(s, (struct sockaddr *)&from, fromlen) != 0) {
        return fail("bind the datagram sockets");
    }
    if (sendto(s, "dg", 2, 0, (struct sockaddr *)&to, tolen) != 2) {
        return fail("sendto a name");
    }
    glen = sizeof(got);
    if (recvfrom(r, buf, sizeof(buf), 0, (struct sockaddr *)&got, &glen) != 2 ||
        strcmp(got.sun_path, FROM_PATH) != 0) {
        return fail("recvfrom did not report the sender's name");
    }
    close(r);
    close(s);
    unlink(DGRAM_PATH);
    unlink(FROM_PATH);
    return 0;
}

static int waiting(void)
{
    struct pollfd pfd;
    char buf[4];
    int sv[2];

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) != 0) {
        return fail("socketpair with SOCK_NONBLOCK");
    }
    if (recv(sv[0], buf, 1, 0) != -1 || errno != EAGAIN) {
        return fail("a nonblocking receive on an empty socket was not EAGAIN");
    }
    pfd.fd = sv[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) != 0) {
        return fail("an empty socket polled readable");
    }
    if (write(sv[1], "p", 1) != 1 || poll(&pfd, 1, 5000) != 1 || !(pfd.revents & POLLIN)) {
        return fail("poll did not report the byte");
    }
    close(sv[0]);
    close(sv[1]);
    return 0;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    if (stream_pair() || message_pairs() || rights_and_creds() || named() || listening() ||
        backlog_growth() || datagram_peers() || datagram_names() || waiting()) {
        return 1;
    }
    printf("UNIXMUSL OK\n");
    return 0;
}
