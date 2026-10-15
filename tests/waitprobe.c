/**
 * Exercises the descriptor layer's status flags, its numbering and its
 * blocking waits, from user space.
 *
 * Three things are checked: (a) O_NONBLOCK is a real, settable OFD status flag,
 * so F_SETFL sets it, F_GETFL reads it back, and clearing it sticks; (b) close
 * of 0, 1, or 2 is legal and the freed number reopens, so closing stdin then
 * opening a file hands back fd 0, and closing stderr returns 0, not -EBADF;
 * (c) a blocking wait is signal-interruptible, so a child blocked in a
 * recvfrom() on the socket wait queue unwinds with -EINTR when the parent,
 * once it sees the child asleep, signals it, instead of hanging or dying.
 *
 * The multi-waiter wake (wq_wake_all makes every blocked waiter runnable) and
 * the lost-wakeup safety are proved by the host test
 * (tests/host/wait_queue_hosttest.c) and by the ssh suites, where Dropbear's
 * accept and read block on the wait queue and are woken by the card's
 * interrupt, since a datagram cannot be delivered hermetically here.
 *
 * Prints "WAITPROBE OK". Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define O_RDONLY   0
#define O_NONBLOCK 04000

#define F_GETFL 3
#define F_SETFL 4

#define SIGUSR1 10
#define EINTR   4

#define AF_INET    2
#define SOCK_DGRAM 2

#define HELLO "/hello.txt" /* staged "hello, world\n" */

/** exit(2) with `status`; does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"\n", 1);
}

static int fails = 0;

/** Count a failure and print `msg` when `cond` is 0. */
static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, STDOUT_FILENO, (long)"  FAIL: ", 8);
        puts(msg);
    }
}

/** Whether the first `n` bytes of `a` and `b` are equal. */
static int eqn(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * Read up to `n` bytes from `fd` into `buf`: the byte count, or a negated
 * errno. Passing buf as a char*, not an inline int cast, lets the analyzer
 * see it written.
 */
static int rd(int fd, char *buf, int n)
{
    return syscall3(SYS_READ, fd, (long)buf, n);
}

/** Whether process `pid` sleeps, by the state its /proc/<pid>/stat gives. */
static int sleeping(int pid)
{
    char path[32] = "/proc/";
    char digits[12];
    char line[256];
    int i = 0, j = 6, fd, n;

    do {
        digits[i++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0);
    while (i > 0) {
        path[j++] = digits[--i];
    }
    for (i = 0; "/stat"[i] != '\0'; i++) {
        path[j++] = "/stat"[i];
    }
    path[j] = '\0';
    fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    n = rd(fd, line, (int)sizeof(line) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return 0;
    }
    for (i = n - 1; i >= 0 && line[i] != ')'; i--) {
    }
    return i >= 0 && i + 2 < n && line[i + 1] == ' ' && line[i + 2] == 'S';
}

/**
 * Signal restorer trampoline: the handler returns here, which invokes
 * rt_sigreturn to restore the interrupted context (musl's __restore_rt).
 */
extern void sig_restorer(void);
PROBE_RT_RESTORER(sig_restorer);

/* The sigaction rt_sigaction takes, with a 64-bit mask. */
struct k_sigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long long mask;
};

static volatile int got_usr1;

/** The SIGUSR1 handler: record that it ran. */
static void usr1_handler(int sig)
{
    (void)sig;
    got_usr1 = 1;
}

#define SA_SIGINFO  0x00000004
#define SA_RESTORER 0x04000000

/**
 * The child: install a SIGUSR1 handler, block in a recvfrom() on a UDP socket
 * that never receives, and exit 0 when the parent's signal unwinds it with
 * -EINTR after the handler ran, 1 otherwise, and 2 or 3 when the handler or
 * the socket cannot be set up.
 */
static void eintr_child(void)
{
    struct k_sigaction ka;
    unsigned long args[6];
    int sfd, rc;
    char buf[16];

    ka.handler = usr1_handler;
    ka.flags = SA_SIGINFO | SA_RESTORER; /* an rt frame, for rt_sigreturn */
    ka.restorer = sig_restorer;
    ka.mask = 0;
    if (syscall4(SYS_RT_SIGACTION, SIGUSR1, (long)&ka, 0, 8) != 0) {
        sys_exit(2);
    }

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    sfd = sock_call(SC_SOCKET, args);
    if (sfd < 0) {
        sys_exit(3);
    }

    args[0] = (unsigned long)sfd;
    args[1] = (unsigned long)(size_t)buf;
    args[2] = sizeof(buf);
    args[3] = 0; /* flags */
    args[4] = 0;
    args[5] = 0;
    rc = sock_call(SC_RECVFROM, args);

    sys_exit((rc == -EINTR && got_usr1) ? 0 : 1);
}

/** Run cases (a) to (c), and exit with the count of the checks that
 * failed. */
PROBE_START void _start(void)
{
    char buf[16];
    int fd, r, n, pid, status;

    fd = syscall3(SYS_OPEN, (long)HELLO, O_RDONLY, 0);
    check(fd >= 0, "open hello.txt");
    check(syscall3(SYS_FCNTL, fd, F_SETFL, O_NONBLOCK) == 0, "F_SETFL O_NONBLOCK");
    check((syscall3(SYS_FCNTL, fd, F_GETFL, 0) & O_NONBLOCK) != 0, "F_GETFL reports O_NONBLOCK");
    check(syscall3(SYS_FCNTL, fd, F_SETFL, 0) == 0, "F_SETFL clear");
    check((syscall3(SYS_FCNTL, fd, F_GETFL, 0) & O_NONBLOCK) == 0, "F_GETFL no longer O_NONBLOCK");
    syscall1(SYS_CLOSE, fd);

    r = syscall1(SYS_CLOSE, STDERR_FILENO);
    check(r == 0, "close(stderr) is legal");
    fd = syscall3(SYS_OPEN, (long)HELLO, O_RDONLY, 0);
    check(fd == STDERR_FILENO, "reopen lands on the freed fd 2");
    syscall1(SYS_CLOSE, fd);

    check(syscall1(SYS_CLOSE, STDIN_FILENO) == 0, "close(stdin) is legal");
    fd = syscall3(SYS_OPEN, (long)HELLO, O_RDONLY, 0);
    check(fd == STDIN_FILENO, "reopen lands on the freed fd 0");
    n = rd(fd, buf, 5);
    check(n == 5 && eqn(buf, "hello", 5), "read through reopened fd 0");
    syscall1(SYS_CLOSE, fd);

    pid = syscall1(SYS_FORK, 0);
    check(pid >= 0, "fork");
    if (pid == 0) {
        eintr_child();
    } else {
        struct {
            long tv_sec;
            long tv_nsec;
        } ts;
        int tries;

        ts.tv_sec = 0;
        ts.tv_nsec = 10000000; /* 10 ms */
        for (tries = 0; tries < 300 && !sleeping(pid); tries++) {
            syscall2(SYS_NANOSLEEP, (long)&ts, 0);
        }
        check(tries < 300, "the child was seen asleep in its recvfrom");
        syscall2(SYS_KILL, pid, SIGUSR1);
        r = syscall3(SYS_WAITPID, pid, (long)&status, 0);
        check(r == pid && status == 0, "child recvfrom interrupted with -EINTR (no hang)");
    }

    if (fails == 0) {
        puts("WAITPROBE OK");
    } else {
        puts("WAITPROBE FAIL");
    }
    sys_exit(fails);
}
