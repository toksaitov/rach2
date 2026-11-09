/**
 * sockettest: the reference count of a socket. A socket is a file
 * descriptor, so close releases it, dup2 and fork share it, and the exit of
 * a process releases any still open. Each test reads the kernel's count of
 * UDP sockets in /proc/net/sockstat and requires one more while its socket
 * is open and the count it started from once the last reference has gone,
 * so a leaked reference shows as a socket still counted, and a double free
 * as one gone too soon or a kernel fault. A fifth test receives two
 * datagrams of an AF_UNIX pair with one recvmmsg and a timeout, which it
 * requires given back with the time left, and on i386 again through
 * recvmmsg_time64. It exits 0 when the five tests pass and 1 otherwise.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names: `sockstat`, that the count can be read, and an assertion for each
 * test, `create-close`, `dup2-share`, `fork-share`, `exit-release` and
 * `recvmmsg-timeout`, which are not run when the count cannot be read.
 */

#include "probe.h"
#include "probe_report.h"

typedef unsigned int uint32_t;

#define STDOUT_FILENO 1

#define AF_INET    2
#define AF_UNIX    1
#define SOCK_DGRAM 2

/* The struct msghdr and mmsghdr of the probe's ABI, whose natural layout is
 * Linux's on both: 28 and 32 bytes on i386, 56 and 64 on x86-64. */
struct iovec {
    void *iov_base;
    size_t iov_len;
};

struct msghdr {
    void *msg_name;
    int msg_namelen;
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

struct mmsghdr {
    struct msghdr msg_hdr;
    unsigned int msg_len;
};

/** The write call: the count written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** The close call: 0, or a negated errno. */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/** The fork call: the child's pid, 0 in the child, or a negated errno. */
static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

/** Wait for the child `pid`: the pid reaped, or a negated errno. */
static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
}

/** The dup2 call: `newfd`, or a negated errno. */
static int sys_dup2(int oldfd, int newfd)
{
    return syscall2(SYS_DUP2, (long)oldfd, (long)newfd);
}

/** The socket operation `call`, an SC_ number, through `sock_call`. */
static int socketcall(int call, unsigned long *args)
{
    return (int)sock_call(call, args);
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

/** Write `s` to standard output. */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}
/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    print(s);
    sys_write(STDOUT_FILENO, "\n", 1);
}

/** Write `n` in decimal to standard output. */
static void print_num(int n)
{
    char buf[12];
    int i = 0, neg = 0;
    if (n < 0) {
        neg = 1;
        n = -n;
    }
    if (n == 0)
        buf[i++] = '0';
    while (n > 0) {
        buf[i++] = (char)('0' + n % 10);
        n /= 10;
    }
    if (neg)
        sys_write(STDOUT_FILENO, "-", 1);
    while (i > 0)
        sys_write(STDOUT_FILENO, &buf[--i], 1);
}

/** A UDP socket: its descriptor, or a negated errno. */
static int make_socket(void)
{
    unsigned long args[3];
    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    return socketcall(SC_SOCKET, args);
}

/** The UDP sockets the kernel holds, as /proc/net/sockstat counts them, or
 * 0xFFFFFFFF when it cannot be read. */
static unsigned udp_inuse(void)
{
    return probe_procfield("/proc/net/sockstat", "UDP: inuse");
}

/* The count of UDP sockets before the first test, which each test returns
 * to once its sockets are closed. */
static unsigned base;

/** Print `ok` as OK or FAILED, with the attempt `at` when it is not -1, and
 * return `ok`. */
static int verdict(int ok, int at)
{
    if (ok) {
        puts("OK");
    } else if (at < 0) {
        puts("FAILED");
    } else {
        print("FAILED (at ");
        print_num(at);
        puts(")");
    }
    return ok;
}

/** Test 1: each socket is counted while open and freed by its close. */
static int create_close(void)
{
    int i, fd;

    print("Test 1: 30x create+close... ");
    for (i = 0; i < 30; i++) {
        fd = make_socket();
        if (fd < 0 || udp_inuse() != base + 1) {
            return verdict(0, i);
        }
        sys_close(fd);
        if (udp_inuse() != base) {
            return verdict(0, i);
        }
    }
    return verdict(1, -1);
}

/** Test 2: dup2 shares the socket; closing both releases it once. */
static int dup2_share(void)
{
    int ok, fd;

    print("Test 2: dup2 share + close... ");
    fd = make_socket();
    ok = fd >= 0 && sys_dup2(fd, 10) == 10;
    if (ok) {
        sys_close(fd); /* socket stays alive via the dup */
        ok = udp_inuse() == base + 1;
        sys_close(10); /* last reference, released here */
        ok = ok && udp_inuse() == base;
    }
    return verdict(ok, -1);
}

/** Test 3: fork shares the socket; parent and child each close it. */
static int fork_share(void)
{
    int ok, fd, pid, status;

    print("Test 3: fork share + close... ");
    fd = make_socket();
    if (fd < 0) {
        return verdict(0, -1);
    }
    pid = sys_fork();
    if (pid == 0) {
        sys_exit(sys_close(fd) == 0 ? 0 : 1);
    }
    ok = pid > 0 && sys_waitpid(pid, &status, 0) == pid && status == 0;
    ok = ok && udp_inuse() == base + 1;
    sys_close(fd); /* the last reference, after the child's close */
    return verdict(ok && udp_inuse() == base, -1);
}

/** Test 4: a child that exits without closing does not leak its socket. */
static int exit_release(void)
{
    int i, pid, status;

    print("Test 4: 30x exit-without-close... ");
    for (i = 0; i < 30; i++) {
        pid = sys_fork();
        if (pid == 0) {
            int s = make_socket();
            sys_exit(s < 0 ? 1 : 0); /* no close: the exit releases it */
        }
        if (pid < 0 || sys_waitpid(pid, &status, 0) != pid || status != 0 || udp_inuse() != base) {
            return verdict(0, i);
        }
    }
    return verdict(1, -1);
}

/** Send the datagrams "a" and "bc" from `tx`; 1 when both went. */
static int send_two(int tx)
{
    unsigned long args[4];

    args[0] = (unsigned long)tx;
    args[1] = (unsigned long)"a";
    args[2] = 1;
    args[3] = 0;
    if (socketcall(SC_SEND, args) != 1) {
        return 0;
    }
    args[1] = (unsigned long)"bc";
    args[2] = 2;
    return socketcall(SC_SEND, args) == 2;
}

/** Point the two headers of `v` at the buffers of `buf`, 8 bytes each. */
static void two_headers(struct mmsghdr *v, struct iovec *iov, char *buf)
{
    int i;

    for (i = 0; i < 2; i++) {
        iov[i].iov_base = buf + 8 * i;
        iov[i].iov_len = 8;
        v[i].msg_hdr.msg_name = 0;
        v[i].msg_hdr.msg_namelen = 0;
        v[i].msg_hdr.msg_iov = &iov[i];
        v[i].msg_hdr.msg_iovlen = 1;
        v[i].msg_hdr.msg_control = 0;
        v[i].msg_hdr.msg_controllen = 0;
        v[i].msg_hdr.msg_flags = 0;
        v[i].msg_len = 0xFFFFFFFFu;
    }
}

/** Whether the two headers of `v` got "a" and "bc" in `buf`. */
static int got_two(const struct mmsghdr *v, const char *buf)
{
    return v[0].msg_len == 1 && v[1].msg_len == 2 && buf[0] == 'a' && buf[8] == 'b' &&
           buf[9] == 'c';
}

/**
 * Test 5: one recvmmsg with a timeout of 5 s takes the two datagrams queued
 * on an AF_UNIX pair, stores each count, and gives the timeout back with the
 * time left, more than 0 and at most 5 s; a timeout of 10^9 nanoseconds
 * fails with EINVAL. On i386 recvmmsg_time64 does the same with a 64-bit
 * timespec whose high word of nanoseconds, set, is ignored and given back
 * 0. The probe's own ABI is the one checked: its i686 build checks the i386
 * calls on either kernel.
 */
static int recvmmsg_timeout(void)
{
    unsigned long args[4];
    int sv[2];
    struct mmsghdr v[2];
    struct iovec iov[2];
    char buf[16];
    long ts[2];
    int ok;

    print("Test 5: recvmmsg with a timeout... ");
    args[0] = AF_UNIX;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    args[3] = (unsigned long)sv;
    if (socketcall(SC_SOCKETPAIR, args) != 0) {
        return verdict(0, 0);
    }
    two_headers(v, iov, buf);
    ts[0] = 5;
    ts[1] = 0;
    ok = send_two(sv[0]) && syscall5(SYS_RECVMMSG, sv[1], (long)v, 2, 0, (long)ts) == 2 &&
         got_two(v, buf);
    ok = ok && ts[1] >= 0 && ts[1] < 1000000000L && ts[0] >= 0 &&
         (ts[0] < 5 || (ts[0] == 5 && ts[1] == 0)) && (ts[0] > 0 || ts[1] > 0);
    if (!ok) {
        return verdict(0, 1);
    }
    ts[0] = 0;
    ts[1] = 1000000000L;
    if (syscall5(SYS_RECVMMSG, sv[1], (long)v, 2, 0, (long)ts) != -22) {
        return verdict(0, 2);
    }
#if !defined __x86_64__
    {
        unsigned int t64[4];

        two_headers(v, iov, buf);
        t64[0] = 5;
        t64[1] = 0;
        t64[2] = 0;
        t64[3] = 0xFFFFFFFFu;
        ok = send_two(sv[0]) &&
             syscall5(SYS_RECVMMSG_TIME64, sv[1], (long)v, 2, 0, (long)t64) == 2 &&
             got_two(v, buf) && t64[1] == 0 && t64[3] == 0 && t64[0] <= 5 && t64[2] < 1000000000u &&
             (t64[0] > 0 || t64[2] > 0);
        if (!ok) {
            return verdict(0, 3);
        }
    }
#endif
    sys_close(sv[0]);
    sys_close(sv[1]);
    return verdict(1, -1);
}

/** Report the test `id`: held when `ok`, or failed with `why`; return `ok`. */
static int report_test(const char *id, int ok, const char *why)
{
    if (ok) {
        report_pass(id);
    } else {
        report_fail(id, why);
    }
    return ok;
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe: the five tests in order, each printed OK or FAILED. `_start`
 * calls it with the initial stack, whose environment gives the case's
 * identity. */
void probe_main(unsigned long *sp)
{
    int passed, failed;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.sockettest"));
    puts("Socket lifecycle test starting...");
    puts("");

    base = udp_inuse();
    if (base == 0xFFFFFFFFu) {
        puts("FAILED: /proc/net/sockstat has no UDP count");
        report_plan("sockstat");
        report_plan("create-close");
        report_plan("dup2-share");
        report_plan("fork-share");
        report_plan("exit-release");
        report_plan("recvmmsg-timeout");
        report_stop("/proc/net/sockstat has no UDP count");
        sys_exit(1);
    }
    report_pass("sockstat");
    passed = report_test("create-close", create_close(),
                         "a socket was not counted while open or after its close");
    passed += report_test("dup2-share", dup2_share(),
                          "the dup2 copy did not keep the socket, or the closes did not free it");
    passed += report_test("fork-share", fork_share(),
                          "the socket shared by fork was not freed once by the two closes");
    passed += report_test("exit-release", exit_release(),
                          "a child's exit without a close left its socket counted");
    passed += report_test("recvmmsg-timeout", recvmmsg_timeout(),
                          "recvmmsg did not take both datagrams, give back the time left or "
                          "refuse a timeout out of range");
    report_end();
    failed = 5 - passed;

    puts("");
    print("Results: ");
    print_num(passed);
    print(" passed, ");
    print_num(failed);
    puts(" failed");

    if (failed == 0) {
        puts("All socket tests passed!");
        sys_exit(0);
    }
    puts("Some tests failed!");
    sys_exit(1);
}
