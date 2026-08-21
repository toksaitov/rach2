/**
 * Exercises the fd-layer blocking and dispatch substrate from user space.
 *
 * Three things are checked: (a) O_NONBLOCK is a real, settable OFD status flag,
 * so F_SETFL sets it, F_GETFL reads it back, and clearing it sticks; (b) close
 * of 0, 1, or 2 is legal and the freed number reopens, so closing stdin then
 * opening a file hands back fd 0, and closing stderr returns 0, not -EBADF;
 * (c) a blocking wait is signal-interruptible, so a child blocked in a
 * recvfrom() on the socket wait queue unwinds with -EINTR when the parent
 * signals it, rather than hanging or dying.
 *
 * The multi-waiter wake (wq_wake_all makes every blocked waiter runnable) and
 * the lost-wakeup safety live in the host unit test (tests/host/
 * wait_queue_hosttest.c) and the live network suite (httpd's accept and recv
 * block on the wait queue and are woken by NIC IRQs), since a datagram cannot
 * be delivered hermetically here.
 *
 * Prints "WAITPROBE OK". Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SYS_EXIT         1
#define SYS_FORK         2
#define SYS_READ         3
#define SYS_WRITE        4
#define SYS_OPEN         5
#define SYS_CLOSE        6
#define SYS_WAITPID      7
#define SYS_KILL         37
#define SYS_SOCKETCALL   102
#define SYS_NANOSLEEP    162
#define SYS_RT_SIGACTION 174
#define SYS_FCNTL64      221

#define O_RDONLY   0
#define O_NONBLOCK 04000

#define F_GETFL 3
#define F_SETFL 4

#define SIGUSR1 10
#define EINTR   4

#define SC_SOCKET   1
#define SC_RECVFROM 12
#define AF_INET     2
#define SOCK_DGRAM  2

#define HELLO "/hello.txt" /* staged "hello, world\n" */

static int syscall1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int syscall2(int n, int a, int b)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static int syscall4(int n, int a, int b, int c, int d)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
    return r;
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void puts(const char *s)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)s, (int)strlen(s));
    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)"\n", 1);
}

static int fails = 0;

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)"  FAIL: ", 8);
        puts(msg);
    }
}

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
 * Reads n bytes from fd into buf; returns the byte count. Passing buf as a
 * char*, not an inline int cast, lets the analyzer see it written.
 */
static int rd(int fd, char *buf, int n)
{
    return syscall3(SYS_READ, fd, (int)(size_t)buf, n);
}

/**
 * Signal restorer trampoline: the handler returns here, which invokes
 * rt_sigreturn (173) to restore the interrupted context (musl's __restore_rt).
 */
extern void sig_restorer(void);
__asm__(".globl sig_restorer\n"
        "sig_restorer:\n"
        "    movl $173, %eax\n"
        "    int $0x80\n");

struct k_sigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long long mask;
};

static volatile int got_usr1;

static void usr1_handler(int sig)
{
    (void)sig;
    got_usr1 = 1;
}

#define SA_RESTORER 0x04000000

/**
 * The child: install a SIGUSR1 handler, block in a recvfrom() on a UDP socket
 * that never receives, and assert the parent's signal unwinds it with -EINTR.
 */
static void eintr_child(void)
{
    struct k_sigaction ka;
    unsigned long args[6];
    int sfd, rc;
    char buf[16];

    ka.handler = usr1_handler;
    ka.flags = SA_RESTORER;
    ka.restorer = sig_restorer;
    ka.mask = 0;
    if (syscall4(SYS_RT_SIGACTION, SIGUSR1, (int)(size_t)&ka, 0, 8) != 0) {
        sys_exit(2);
    }

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    sfd = syscall2(SYS_SOCKETCALL, SC_SOCKET, (int)(size_t)args);
    if (sfd < 0) {
        sys_exit(3);
    }

    /* Blocks on the socket wait queue; the parent's SIGUSR1 must break it. */
    args[0] = (unsigned long)sfd;
    args[1] = (unsigned long)(size_t)buf;
    args[2] = sizeof(buf);
    args[3] = 0; /* flags */
    args[4] = 0;
    args[5] = 0;
    rc = syscall2(SYS_SOCKETCALL, SC_RECVFROM, (int)(size_t)args);

    /* Expect -EINTR, and the handler must have run. */
    sys_exit((rc == -EINTR && got_usr1) ? 0 : 1);
}

void _start(void)
{
    char buf[16];
    int fd, r, n, pid, status;

    /* (a) O_NONBLOCK round-trips through F_SETFL/F_GETFL on the OFD. */
    fd = syscall3(SYS_OPEN, (int)(size_t)HELLO, O_RDONLY, 0);
    check(fd >= 0, "open hello.txt");
    check(syscall3(SYS_FCNTL64, fd, F_SETFL, O_NONBLOCK) == 0, "F_SETFL O_NONBLOCK");
    check((syscall3(SYS_FCNTL64, fd, F_GETFL, 0) & O_NONBLOCK) != 0, "F_GETFL reports O_NONBLOCK");
    check(syscall3(SYS_FCNTL64, fd, F_SETFL, 0) == 0, "F_SETFL clear");
    check((syscall3(SYS_FCNTL64, fd, F_GETFL, 0) & O_NONBLOCK) == 0,
          "F_GETFL no longer O_NONBLOCK");
    syscall1(SYS_CLOSE, fd);

    /* (b) close(2) is legal. fd 0/1 stay open, so the freed lowest number is
     * 2 and the reopen lands there. */
    r = syscall1(SYS_CLOSE, STDERR_FILENO);
    check(r == 0, "close(stderr) is legal");
    fd = syscall3(SYS_OPEN, (int)(size_t)HELLO, O_RDONLY, 0);
    check(fd == STDERR_FILENO, "reopen lands on the freed fd 2");
    syscall1(SYS_CLOSE, fd);

    /* close(0) is legal, and with fd 0 freed the reopen lands on 0. */
    check(syscall1(SYS_CLOSE, STDIN_FILENO) == 0, "close(stdin) is legal");
    fd = syscall3(SYS_OPEN, (int)(size_t)HELLO, O_RDONLY, 0);
    check(fd == STDIN_FILENO, "reopen lands on the freed fd 0");
    n = rd(fd, buf, 5);
    check(n == 5 && eqn(buf, "hello", 5), "read through reopened fd 0");
    syscall1(SYS_CLOSE, fd);

    /* (c) a blocking recvfrom is interruptible: the child unwinds with
     * -EINTR. */
    pid = syscall1(SYS_FORK, 0);
    check(pid >= 0, "fork");
    if (pid == 0) {
        eintr_child();
    } else {
        /* Give the child time to reach the blocking recvfrom, then signal
         * it. */
        struct {
            int tv_sec;
            int tv_nsec;
        } ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 50000000; /* 50 ms */
        syscall2(SYS_NANOSLEEP, (int)(size_t)&ts, 0);
        syscall2(SYS_KILL, pid, SIGUSR1);
        syscall3(SYS_WAITPID, pid, (int)(size_t)&status, 0);
        check(((status >> 8) & 0xff) == 0, "child recvfrom interrupted with -EINTR (no hang)");
    }

    if (fails == 0) {
        puts("WAITPROBE OK");
    } else {
        puts("WAITPROBE FAIL");
    }
    sys_exit(fails);
}
