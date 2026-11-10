/**
 * restartprobe: what a system call that waits gives its caller when a
 * signal reaches the caller during the wait, as Linux 6.12 gives it, on a
 * 32-bit and a 64-bit kernel and for a 32-bit program on a 64-bit one.
 *
 * For each waiting call a child makes the call while the parent disturbs it
 * one of four ways: it stops the child with SIGSTOP, holds it 2 s and
 * continues it with SIGCONT, which runs no handler; it sends SIGWINCH and
 * SIGCONT, ignored by default, and SIGUSR2, which the child ignores; or it
 * sends SIGUSR1, whose handler the child installed with SA_RESTART or
 * without it. The calls with a timeout wait 3 s, those without one
 * until the parent wakes them: a byte down a pipe, a FUTEX_WAKE, a message,
 * a semaphore's post, or SIGUSR1 for pause and rt_sigsuspend.
 *
 * Linux's answer follows from the code each call returns when the signal
 * ends its wait (kernel/signal.c, arch/x86/kernel/signal.c). A signal that
 * is ignored is dropped when it is sent and wakes nothing. After a stop
 * and a continue, every call goes on but epoll_wait, semtimedop,
 * rt_sigtimedwait and semop, which return EINTR. After a handler with
 * SA_RESTART only the calls that return ERESTARTSYS go on, here the read,
 * and the futex wait without a timeout; the others return EINTR, as every
 * call does after a handler without SA_RESTART. A timed call that goes on
 * ends at its first deadline when it resumes through restart_syscall from
 * the restart block, as nanosleep, the relative clock_nanosleep, poll and
 * the timed futex wait do, or with its own arguments, as the absolute
 * clock_nanosleep does. ppoll, select and pselect6 write the time left back
 * to the caller's timeout on every return and restart with it, so after a
 * stop they end as much past their first deadline as the stop lasted.
 * nanosleep and the relative clock_nanosleep write the time left to their
 * remainder whenever a signal ends the wait, also when the call then
 * resumes.
 *
 * The rows, each an assertion of etude's report (probe_report.h) for the
 * case ETUDE_CASE names:
 *
 * `stop-timed`, `stop-untimed`: after a stop and a continue, each call with
 * a timeout, and each without one, gives Linux's result.
 * `ignored-timed`, `ignored-untimed`: the ignored signals leave each call
 * to end as it would have.
 * `restart-timed`, `restart-untimed`: after a handler with SA_RESTART.
 * `plain-timed`, `plain-untimed`: after a handler without it.
 * `direct`: restart_syscall made directly, and after a handler cut a
 * nanosleep short, returns EINTR, as Linux's do_no_restart_syscall does.
 * `trace`: a tracer reads -516, ERESTART_RESTARTBLOCK, at the exit stop of
 * a nanosleep that SIGWINCH cut short, then the signal's delivery stop,
 * then an entry stop of restart_syscall, 0 on i386 and 219 on x86-64, and
 * its exit stop with 0, and the program sees nanosleep return 0.
 * `trace-edits`: the tracer writes a restart code back, cancels at delivery,
 * changes its call number, saves and restores registers after orig -1,
 * and leaves injected codes unchanged with orig -1 or no pending signal.
 * `race`: a traced thread's pipe read, woken by a signal sent to its
 * process that only it does not block, is held at its exit stop with -512,
 * ERESTARTSYS, while a second thread unblocks the signal and runs its
 * handler; the read then restarts, as Linux restarts a call when get_signal
 * finds nothing, and never returns the restart code.
 *
 * Each row of a timed call that goes on checks CLOCK_MONOTONIC: the call
 * ends no sooner than its deadline, 3 s after it began or for ppoll, select
 * and pselect6 after a stop the hold past that, and less than a second past
 * it, where a deadline measured again from the continue would end at least
 * the 2 s hold past it. semtimedop may end up to a tick early, since
 * Linux's counts its timeout in jiffies. A call that returns EINTR is
 * judged by its result and the time it wrote, not by when it ended. The
 * probe sets no time limit of its own: a call that never ends is left to
 * etude's bound on the case.
 */
#include "probe.h"
#include "probe_report.h"

#define EINTR     4
#define EAGAIN    11
#define ETIMEDOUT 110

#define SIGTRAP   5
#define SIGUSR1   10
#define SIGUSR2   12
#define SIGCONT   18
#define SIGSTOP   19
#define SIGWINCH  28
/* The signal rt_sigtimedwait waits for, which nobody sends. */
#define SIGWAITED 40

#define SA_SIGINFO  0x00000004u
#define SA_RESTORER 0x04000000u
#define SA_RESTART  0x10000000u
#define SIG_IGN     1

#define CLOCK_MONOTONIC 1
#define TIMER_ABSTIME   1
#define FUTEX_WAIT      0
#define FUTEX_WAKE      1
#define FUTEX_PRIVATE   128
#define POLLIN          1
#define WUNTRACED       2
#define EPOLL_CTL_ADD   1
#define EPOLLIN         1
#define IPC_CREAT       01000
#define IPC_RMID        0

#define PTRACE_TRACEME  0
#define PTRACE_PEEKUSER 3
#define PTRACE_POKEUSER 6
#define PTRACE_GETREGS  12
#define PTRACE_SETREGS  13
#define PTRACE_CONT     7
#define PTRACE_KILL     8
#define PTRACE_SYSCALL  24
#define PTRACE_DETACH   17

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1

#define PROT_RW         3
#define MAP_SHARED_ANON 0x21

#if defined __x86_64__
#define SYS_RT_SIGTIMEDWAIT 128
#define SYS_RT_SIGSUSPEND   130
#define SYS_EPOLL_CREATE1   291
#define SYS_EPOLL_CTL       233
#define SYS_EPOLL_WAIT      232
#define SYS_SEMGET          64
#define SYS_SEMOP           65
#define SYS_SEMCTL          66
#define SYS_SEMTIMEDOP      220
#define SYS_MSGGET          68
#define SYS_MSGSND          69
#define SYS_MSGRCV          70
#define SYS_MSGCTL          71
#define SYS_RESTART_SYSCALL 219
/* nanosleep's number, which the tracer reads in orig_rax. */
#define NR_SLEEP            35
/* The offsets of orig_rax and rax in x86-64's struct user_regs_struct. */
#define USER_ORIG           (15 * 8)
#define USER_RET            (10 * 8)
#define USER_WORDS          27
#else
#define SYS_RT_SIGTIMEDWAIT 177
#define SYS_RT_SIGSUSPEND   179
#define SYS_EPOLL_CREATE1   329
#define SYS_EPOLL_CTL       255
#define SYS_EPOLL_WAIT      256
#define SYS_IPC             117
#define IPC_SEMOP           1
#define IPC_SEMGET          2
#define IPC_SEMCTL          3
#define IPC_SEMTIMEDOP      4
#define IPC_MSGSND          11
#define IPC_MSGRCV          12
#define IPC_MSGGET          13
#define IPC_MSGCTL          14
#define SYS_RESTART_SYSCALL 0
/* nanosleep's number, which the tracer reads in orig_eax. */
#define NR_SLEEP            162
/* The offsets of orig_eax and eax in i386's struct user_regs_struct. */
#define USER_ORIG           (11 * 4)
#define USER_RET            (6 * 4)
#define USER_WORDS          17
#endif

/* The timeout of a timed call in seconds, and in microseconds the
 * timeout, the hold of a stop, and how far past the deadline a call that
 * goes on may end: half the hold, so that a call measured again from the
 * continue misses by a second as well. The timeout outlasts the hold, so
 * the deadline is still ahead when the child continues. */
#define WAIT_S    3
#define WAIT_US   (WAIT_S * 1000000L)
#define HOLD_US   2000000L
#define MARGIN_US (HOLD_US / 2)
/* The tick by which Linux's semtimedop may end early. */
#define JIFFY_US  10000L

struct ts {
    long sec;
    long nsec;
};

/* rt_sigaction's record, its handler and restorer as words. */
struct k_sigaction {
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    unsigned mask[2];
};

struct pollfd {
    int fd;
    short events;
    short revents;
};

struct msgbuf {
    long type;
    char text[8];
};

/* What a child reports of its call: the result, the clock before and
 * after it, the handler's runs at its return, the time left the call wrote
 * (-1 when it wrote none), and restart_syscall's result in the row that
 * makes it. */
struct record {
    long ret;
    struct ts t0, t1;
    long hits;
    struct ts left;
    long direct;
};

/* The calls, the timed first. */
enum {
    C_NANOSLEEP,
    C_CLOCK_REL,
    C_CLOCK_ABS,
    C_POLL,
    C_PPOLL,
    C_SELECT,
    C_PSELECT,
    C_FUTEX,
    C_EPOLL,
    C_SEMTIMED,
    C_SIGTIMED,
    C_UNTIMED,
    C_PAUSE = C_UNTIMED,
    C_SIGSUSPEND,
    C_POLL_INF,
    C_FUTEX_INF,
    C_MSGRCV,
    C_READ,
    C_SEMOP,
    C_COUNT
};

/* The code Linux's call returns when a signal ends its wait: ERESTARTSYS,
 * ERESTARTNOHAND, ERESTART_RESTARTBLOCK, or EINTR, which never restarts. */
enum { K_SYS, K_NOHAND, K_BLOCK, K_INTR };

/* The four disturbances. */
enum { S_STOP, S_IGNORED, S_RESTART, S_PLAIN, S_COUNT };

struct call {
    const char *name;
    int kind;
    long natural; /* the result when nothing ends the wait early */
};

static const struct call calls[C_COUNT] = {
    {"nanosleep", K_BLOCK, 0},
    {"clock_nanosleep", K_BLOCK, 0},
    {"clock_nanosleep-abs", K_NOHAND, 0},
    {"poll", K_BLOCK, 0},
    {"ppoll", K_NOHAND, 0},
    {"select", K_NOHAND, 0},
    {"pselect6", K_NOHAND, 0},
    {"futex-wait", K_BLOCK, -ETIMEDOUT},
    {"epoll_wait", K_INTR, 0},
    {"semtimedop", K_INTR, -EAGAIN},
    {"rt_sigtimedwait", K_INTR, -EAGAIN},
    {"pause", K_NOHAND, -EINTR},
    {"rt_sigsuspend", K_NOHAND, -EINTR},
    {"poll-inf", K_BLOCK, 1},
    {"futex-wait-inf", K_SYS, 0},
    {"msgrcv", K_NOHAND, 1},
    {"read", K_SYS, 1},
    {"semop", K_INTR, 0},
};

static const char *const scenario_names[S_COUNT] = {"stop", "ignored", "restart", "plain"};

/* The rows: for each disturbance its timed and its untimed part. */
static const char *const part_ids[2 * S_COUNT] = {
    "stop-timed",    "stop-untimed",    "ignored-timed", "ignored-untimed",
    "restart-timed", "restart-untimed", "plain-timed",   "plain-untimed"};

/* The child's runs of the SIGUSR1 handler. */
static volatile long hits;
/* The shared word of the untimed futex wait, and the pipe the untimed
 * calls and the polls read. */
static volatile int *shared_word;
static int wake_fd[2];
static int sem_id, msg_id;
static int failed;

extern void restorer(void);
PROBE_RT_RESTORER(restorer);

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT_GROUP, status);
    __builtin_unreachable();
}

/** Write the decimal of `v` into `buf` and return it. */
static char *decimal(long v, char *buf)
{
    char tmp[24];
    unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
    int i = 0, j = 0;

    do {
        tmp[i++] = (char)('0' + u % 10);
        u /= 10;
    } while (u > 0);
    if (v < 0) {
        buf[j++] = '-';
    }
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
    return buf;
}

/** Append `s` to `buf`, which has room for 160 bytes. */
static void append(char *buf, const char *s)
{
    int n = 0;

    while (buf[n] != '\0') {
        n++;
    }
    while (*s != '\0' && n < 159) {
        buf[n++] = *s++;
    }
    buf[n] = '\0';
}

/** Record that a check of the open row failed: the call `name` in the
 * disturbance `scen` did `what`, `v`, where Linux gives `want`. */
static void miss(const char *name, int scen, const char *what, long v, long want)
{
    char line[160], num[24];

    line[0] = '\0';
    append(line, name);
    append(line, " ");
    append(line, scen >= 0 ? scenario_names[scen] : "");
    append(line, ": ");
    append(line, what);
    append(line, " ");
    append(line, decimal(v, num));
    append(line, ", Linux ");
    append(line, decimal(want, num));
    probe_line("restartprobe: fail: ", line, 0);
    report_miss(line);
    failed = 1;
}

/** Zero the `n` bytes at `p`, a buffer the kernel fills. */
static void clear(void *p, unsigned long n)
{
    char *d = p;

    while (n--) {
        *d++ = 0;
    }
}

/** Sleep `us` microseconds. */
static void nap(long us)
{
    struct ts t;

    t.sec = us / 1000000L;
    t.nsec = us % 1000000L * 1000L;
    /* interval: naps between looks at a child, and the stop's hold */
    syscall2(SYS_NANOSLEEP, (long)&t, 0);
}

/** The time on CLOCK_MONOTONIC. */
static struct ts now(void)
{
    struct ts t;

    syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t);
    return t;
}

/** The microseconds from `a` to `b`, which are less than 2000 s apart. */
static long micros(struct ts a, struct ts b)
{
    return (b.sec - a.sec) * 1000000L + (b.nsec - a.nsec) / 1000;
}

/** Install `h` for `signo` with the flags `flags`, or SIG_IGN for a NULL
 * `h`; SA_SIGINFO, since the restorer is rt_sigreturn, which an i386 frame
 * without it does not end with. */
static void install(int signo, void (*h)(int), unsigned long flags)
{
    struct k_sigaction ka;

    ka.handler = h ? (unsigned long)h : SIG_IGN;
    ka.flags = SA_SIGINFO | SA_RESTORER | flags;
    ka.restorer = (unsigned long)restorer;
    ka.mask[0] = 0;
    ka.mask[1] = 0;
    syscall4(SYS_RT_SIGACTION, signo, (long)&ka, 0, 8);
}

/** The SIGUSR1 handler: count the run. */
static void on_usr1(int signo)
{
    (void)signo;
    hits++;
}

/** The state letter of process `pid`, the field after the name in
 * /proc/PID/stat, or 0 when it cannot be read. */
static char state_of(int pid)
{
    char path[32] = "/proc/", digits[12], line[256];
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
    fd = (int)syscall3(SYS_OPEN, (long)path, 0, 0);
    if (fd < 0) {
        return 0;
    }
    clear(line, sizeof(line));
    n = (int)syscall3(SYS_READ, fd, (long)line, sizeof(line) - 1);
    syscall1(SYS_CLOSE, fd);
    for (i = n - 1; i > 0; i--) {
        if (line[i] == ')' && i + 2 < n) {
            return line[i + 2];
        }
    }
    return 0;
}

/** Wait until process `pid` sleeps in a wait; whether it does, 0 when it
 * has exited, stopped or cannot be read first, since none of those ends
 * in the sleep without the probe. */
static int wait_sleeping(int pid)
{
    char st;

    while ((st = state_of(pid)) != 'S') {
        if (st == 'Z' || st == 'X' || st == 'T' || st == 't' || st == 0) {
            return 0;
        }
        nap(5000L);
    }
    return 1;
}

/** Whether `fd` has something to read within `ms` milliseconds. */
static int readable(int fd, int ms)
{
    struct pollfd p;

    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    return syscall3(SYS_POLL, (long)&p, 1, ms) == 1;
}

#if defined __x86_64__
/** semtimedop, or semop for a NULL `t`, of the operation `op` on the set. */
static long sem_do(int id, const short *op, const struct ts *t)
{
    return t ? syscall4(SYS_SEMTIMEDOP, id, (long)op, 1, (long)t)
             : syscall3(SYS_SEMOP, id, (long)op, 1);
}

/** A new private set of one semaphore at 0. */
static int sem_new(void)
{
    return (int)syscall3(SYS_SEMGET, 0, 1, IPC_CREAT | 0600);
}

/** Remove the set `id`. */
static void sem_rm(int id)
{
    syscall4(SYS_SEMCTL, id, 0, IPC_RMID, 0);
}

/** A new private message queue. */
static int msg_new(void)
{
    return (int)syscall2(SYS_MSGGET, 0, IPC_CREAT | 0600);
}

/** Send the message `m` of one byte. */
static long msg_send(int id, struct msgbuf *m)
{
    return syscall4(SYS_MSGSND, id, (long)m, 1, 0);
}

/** Receive a message of any type into `m`. */
static long msg_recv(int id, struct msgbuf *m)
{
    return syscall5(SYS_MSGRCV, id, (long)m, sizeof(m->text), 0, 0);
}

/** Remove the queue `id`. */
static void msg_rm(int id)
{
    syscall3(SYS_MSGCTL, id, IPC_RMID, 0);
}
#else
/** semtimedop, or semop for a NULL `t`, through ipc(2). */
static long sem_do(int id, const short *op, const struct ts *t)
{
    return syscall6(SYS_IPC, t ? IPC_SEMTIMEDOP : IPC_SEMOP, id, 1, 0, (long)op, (long)t);
}

/** A new private set of one semaphore at 0, through ipc(2). */
static int sem_new(void)
{
    return (int)syscall6(SYS_IPC, IPC_SEMGET, 0, 1, IPC_CREAT | 0600, 0, 0);
}

/** Remove the set `id` through ipc(2), which reads SEMCTL's argument
 * through its pointer. */
static void sem_rm(int id)
{
    long arg = 0;

    syscall6(SYS_IPC, IPC_SEMCTL, id, 0, IPC_RMID, (long)&arg, 0);
}

/** A new private message queue, through ipc(2). */
static int msg_new(void)
{
    return (int)syscall6(SYS_IPC, IPC_MSGGET, 0, IPC_CREAT | 0600, 0, 0, 0);
}

/** Send the message `m` of one byte through ipc(2). */
static long msg_send(int id, struct msgbuf *m)
{
    return syscall6(SYS_IPC, IPC_MSGSND, id, 1, 0, (long)m, 0);
}

/** Receive a message of any type into `m` through ipc(2), whose MSGRCV of
 * version 1 takes the buffer and the type directly. */
static long msg_recv(int id, struct msgbuf *m)
{
    return syscall6(SYS_IPC, IPC_MSGRCV | (1 << 16), id, sizeof(m->text), 0, (long)m, 0);
}

/** Remove the queue `id` through ipc(2). */
static void msg_rm(int id)
{
    syscall6(SYS_IPC, IPC_MSGCTL, id, IPC_RMID, 0, 0, 0);
}
#endif

/** The fd set of select holding `fd`, which is below 64. */
static void fd_set1(unsigned long *set, int fd)
{
    int i;

    for (i = 0; i < 16; i++) {
        set[i] = 0;
    }
    set[fd / (8 * (int)sizeof(long))] |= 1UL << (fd % (8 * (int)sizeof(long)));
}

/** Make the timed call `c` of WAIT_S seconds, with `left` the time it
 * writes back; the epoll set `ep` serves C_EPOLL. */
static long timed_call(int c, struct ts *left, int ep)
{
    struct ts req = {WAIT_S, 0}, end;
    struct pollfd p = {0, POLLIN, 0};
    unsigned long set[16];
    const unsigned sigs[2] = {0, 1u << (SIGWAITED - 33)};
    const short down[3] = {0, -1, 0}; /* sembuf: the semaphore, the change, flags */
    char events[16];
    int word = 0;

    p.fd = wake_fd[0];
    clear(events, sizeof(events));
    *left = req;
    switch (c) {
    case C_NANOSLEEP:
        left->sec = left->nsec = -1;
        /* interval: the sleeps tested, timed against their deadlines */
        return syscall2(SYS_NANOSLEEP, (long)&req, (long)left);
    case C_CLOCK_REL:
        left->sec = left->nsec = -1;
        /* interval: the sleeps tested, timed against their deadlines */
        return syscall4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, 0, (long)&req, (long)left);
    case C_CLOCK_ABS:
        left->sec = left->nsec = -1;
        end = now();
        end.sec += WAIT_S;
        /* interval: the sleeps tested, timed against their deadlines */
        return syscall4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, TIMER_ABSTIME, (long)&end, 0);
    case C_POLL:
        left->sec = left->nsec = -1;
        return syscall3(SYS_POLL, (long)&p, 1, WAIT_S * 1000);
    case C_PPOLL:
        return syscall5(SYS_PPOLL, (long)&p, 1, (long)left, 0, 8);
    case C_SELECT: {
        long tv[2], r;

        tv[0] = WAIT_S;
        tv[1] = 0;
        fd_set1(set, wake_fd[0]);
        r = syscall5(SYS_SELECT, wake_fd[0] + 1, (long)set, 0, 0, (long)tv);
        left->sec = tv[0];
        left->nsec = tv[1] * 1000;
        return r;
    }
    case C_PSELECT:
        fd_set1(set, wake_fd[0]);
        return syscall6(SYS_PSELECT6, wake_fd[0] + 1, (long)set, 0, 0, (long)left, 0);
    case C_FUTEX:
        left->sec = left->nsec = -1;
        return syscall4(SYS_FUTEX, (long)&word, FUTEX_WAIT | FUTEX_PRIVATE, 0, (long)&req);
    case C_EPOLL:
        left->sec = left->nsec = -1;
        return syscall4(SYS_EPOLL_WAIT, ep, (long)events, 1, WAIT_S * 1000);
    case C_SEMTIMED:
        left->sec = left->nsec = -1;
        return sem_do(sem_id, down, &req);
    default:
        left->sec = left->nsec = -1;
        return syscall4(SYS_RT_SIGTIMEDWAIT, (long)sigs, 0, (long)&req, 8);
    }
}

/** Make the untimed call `c`. */
static long untimed_call(int c)
{
    struct pollfd p = {0, POLLIN, 0};
    const short down[3] = {0, -1, 0}; /* sembuf: the semaphore, the change, flags */
    struct msgbuf m;
    const unsigned none[2] = {0, 0};
    char byte;

    p.fd = wake_fd[0];
    switch (c) {
    case C_PAUSE:
        return syscall0(SYS_PAUSE);
    case C_SIGSUSPEND:
        return syscall2(SYS_RT_SIGSUSPEND, (long)none, 8);
    case C_POLL_INF:
        return syscall3(SYS_POLL, (long)&p, 1, -1);
    case C_FUTEX_INF:
        return syscall4(SYS_FUTEX, (long)shared_word, FUTEX_WAIT, 0, 0);
    case C_MSGRCV:
        return msg_recv(msg_id, &m);
    case C_READ:
        return syscall3(SYS_READ, wake_fd[0], (long)&byte, 1);
    default:
        return sem_do(sem_id, down, 0);
    }
}

/** The child of a row: make the call `c` under the handler the disturbance
 * `scen` wants, and write its record to `out`. */
static void child(int c, int scen, int out)
{
    struct record r;
    struct {
        unsigned events;
        unsigned data[2];
    } __attribute__((packed)) ev;
    int ep = -1;

    install(SIGUSR1, on_usr1, scen == S_RESTART ? SA_RESTART : 0);
    install(SIGUSR2, 0, 0);
    if (c == C_EPOLL) {
        ep = (int)syscall1(SYS_EPOLL_CREATE1, 0);
        ev.events = EPOLLIN;
        ev.data[0] = ev.data[1] = 0;
        syscall4(SYS_EPOLL_CTL, ep, EPOLL_CTL_ADD, wake_fd[0], (long)&ev);
    }
    r.direct = 0;
    syscall3(SYS_WRITE, out, (long)"r", 1);
    r.t0 = now();
    r.ret = c < C_UNTIMED ? timed_call(c, &r.left, ep) : untimed_call(c);
    r.t1 = now();
    r.hits = hits;
    if (c >= C_UNTIMED) {
        r.left.sec = r.left.nsec = -1;
    }
    if (c == C_NANOSLEEP && scen == S_PLAIN) {
        r.direct = syscall0(SYS_RESTART_SYSCALL);
    }
    syscall3(SYS_WRITE, out, (long)&r, sizeof(r));
    sys_exit(0);
}

/** Wake the untimed call `c` of the child `pid` as its waiter would. */
static void wake(int c, int pid)
{
    const short up[3] = {0, 1, 0};
    struct msgbuf m;

    switch (c) {
    case C_PAUSE:
    case C_SIGSUSPEND:
        syscall2(SYS_KILL, pid, SIGUSR1);
        break;
    case C_FUTEX_INF:
        *shared_word = 1;
        syscall4(SYS_FUTEX, (long)shared_word, FUTEX_WAKE, 1, 0);
        break;
    case C_MSGRCV:
        m.type = 1;
        m.text[0] = 'm';
        msg_send(msg_id, &m);
        break;
    case C_SEMOP:
        sem_do(sem_id, up, 0);
        break;
    default:
        syscall3(SYS_WRITE, wake_fd[1], (long)"w", 1);
        break;
    }
}

/** Disturb the child `pid` as `scen` says; whether it went as planned. */
static int disturb(int pid, int scen)
{
    int status = 0;

    switch (scen) {
    case S_STOP:
        syscall2(SYS_KILL, pid, SIGSTOP);
        if (syscall4(SYS_WAIT4, pid, (long)&status, WUNTRACED, 0) != pid ||
            (status & 0xFFFF) != ((SIGSTOP << 8) | 0x7F)) {
            return 0;
        }
        nap(HOLD_US);
        syscall2(SYS_KILL, pid, SIGCONT);
        return 1;
    case S_IGNORED:
        syscall2(SYS_KILL, pid, SIGWINCH);
        syscall2(SYS_KILL, pid, SIGCONT);
        syscall2(SYS_KILL, pid, SIGUSR2);
        return 1;
    default:
        syscall2(SYS_KILL, pid, SIGUSR1);
        return 1;
    }
}

/** Whether the call `c` goes on, as Linux's does, after the disturbance
 * `scen`. */
static int goes_on(int c, int scen)
{
    switch (scen) {
    case S_IGNORED:
        return 1;
    case S_STOP:
        return calls[c].kind != K_INTR;
    case S_RESTART:
        return calls[c].kind == K_SYS;
    default:
        return 0;
    }
}

/** Whether `t` is a time left within the timeout: more than none and at
 * most the timeout and a millisecond, since Linux's remainder counts to
 * the end of its timer's slack, 50 us past the deadline. */
static int within(struct ts t)
{
    return (t.sec >= 0 && t.sec < WAIT_S && t.nsec >= 0 && t.nsec < 1000000000L &&
            (t.sec > 0 || t.nsec > 0)) ||
           (t.sec == WAIT_S && t.nsec >= 0 && t.nsec <= 1000000L);
}

/** Check the time the timed call `c` took and the time left it wrote,
 * against Linux's for the disturbance `scen`. A ppoll, select or pselect6
 * that a stop cut short restarts with the time left at the stop, so it
 * ends the stop's hold past its first deadline. */
static void judge_time(int c, int scen, const struct record *r, int on)
{
    long took = micros(r->t0, r->t1);
    const char *name = calls[c].name;
    int rem = c == C_NANOSLEEP || c == C_CLOCK_REL;
    int back = c == C_PPOLL || c == C_SELECT || c == C_PSELECT;
    long due = WAIT_US + (back && scen == S_STOP ? HOLD_US : 0);
    long low = due - (c == C_SEMTIMED ? JIFFY_US : 0);

    if (on && (took < low || took >= due + MARGIN_US)) {
        miss(name, scen, "ended after us", took, due);
    }
    if (rem && scen == S_IGNORED && r->left.sec != -1) {
        miss(name, scen, "wrote a remainder of s", r->left.sec, -1);
    }
    if (rem && scen != S_IGNORED && !within(r->left)) {
        miss(name, scen, "wrote a remainder of ns", r->left.sec * 1000000000L + r->left.nsec, 1);
    }
    if (back && on && (r->left.sec != 0 || r->left.nsec != 0)) {
        miss(name, scen, "wrote back a time left of ns", r->left.nsec, 0);
    }
    if (back && !on && !within(r->left)) {
        miss(name, scen, "wrote back a time left of s", r->left.sec, WAIT_S);
    }
}

/** Check the record `r` of the call `c` against Linux's for the
 * disturbance `scen`. */
static void judge(int c, int scen, const struct record *r)
{
    int on = goes_on(c, scen);
    long want = on ? calls[c].natural : -EINTR;
    long want_hits = scen >= S_RESTART || c == C_PAUSE || c == C_SIGSUSPEND ? 1 : 0;

    if (r->ret != want && !(c == C_FUTEX_INF && on && r->ret == -EAGAIN)) {
        miss(calls[c].name, scen, "returned", r->ret, want);
    }
    if (r->hits != want_hits) {
        miss(calls[c].name, scen, "ran the handler times", r->hits, want_hits);
    }
    if (c < C_UNTIMED) {
        judge_time(c, scen, r, on);
    }
}

/** Close each acquired end of `fds`, and mark both invalid. */
static void close_pipe(int fds[2])
{
    int i;

    for (i = 0; i < 2; i++) {
        if (fds[i] >= 0) {
            syscall1(SYS_CLOSE, fds[i]);
            fds[i] = -1;
        }
    }
}

/** Run the call `c` in a child under the disturbance `scen` and check it;
 * the record goes to `*r`, and whether the child reported it. */
static int run(int c, int scen, struct record *r)
{
    int pipefd[2], pid = -1, status = 0, got = 0;
    char byte;

    pipefd[0] = pipefd[1] = -1;
    wake_fd[0] = wake_fd[1] = -1;
    sem_id = msg_id = -1;

    *shared_word = 0;
    sem_id = sem_new();
    msg_id = msg_new();
    if (syscall1(SYS_PIPE, (long)pipefd) != 0 || syscall1(SYS_PIPE, (long)wake_fd) != 0 ||
        sem_id < 0 || msg_id < 0) {
        miss(calls[c].name, scen, "could not make its pipes and objects", sem_id, 0);
        goto out;
    }
    pid = (int)syscall0(SYS_FORK);
    if (pid < 0) {
        miss(calls[c].name, scen, "could not fork", pid, 0);
        goto out;
    }
    if (pid == 0) {
        syscall1(SYS_CLOSE, pipefd[0]);
        child(c, scen, pipefd[1]);
    }
    syscall1(SYS_CLOSE, pipefd[1]);
    pipefd[1] = -1;
    got = syscall3(SYS_READ, pipefd[0], (long)&byte, 1) == 1 && wait_sleeping(pid);
    if (got && !disturb(pid, scen)) {
        miss(calls[c].name, scen, "did not stop", 0, 0);
        got = 0;
    }
    while (got && c >= C_UNTIMED && !readable(pipefd[0], 10)) {
        if (state_of(pid) == 'S') {
            wake(c, pid);
            break;
        }
    }
    if (got) {
        got = syscall3(SYS_READ, pipefd[0], (long)r, sizeof(*r)) == (long)sizeof(*r);
    }
out:
    if (pid > 0) {
        if (!got) {
            syscall2(SYS_KILL, pid, 9);
        }
        if (syscall4(SYS_WAIT4, pid, (long)&status, 0, 0) != pid || status != 0) {
            got = 0;
        }
    }
    close_pipe(pipefd);
    close_pipe(wake_fd);
    if (sem_id >= 0) {
        sem_rm(sem_id);
    }
    if (msg_id >= 0) {
        msg_rm(msg_id);
    }
    if (!got) {
        miss(calls[c].name, scen, "reported nothing; status", status, 0);
    }
    return got;
}

/** Run each call under the disturbance `scen`, in its two rows; the
 * record of the plain nanosleep goes to `*direct`. */
static void scenario(int scen, long *direct)
{
    struct record r;
    int c;

    for (c = 0; c < C_COUNT; c++) {
        if (c == 0 || c == C_UNTIMED) {
            report_part(part_ids[2 * scen + (c == C_UNTIMED)]);
        }
        if (run(c, scen, &r)) {
            judge(c, scen, &r);
            if (c == C_NANOSLEEP && scen == S_PLAIN) {
                *direct = r.direct;
            }
        }
    }
}

/** Peek the word at `off` of the tracee `pid`'s registers. */
static long peek(int pid, long off)
{
    long word = 0x5A5A;

    syscall4(SYS_PTRACE, PTRACE_PEEKUSER, pid, off, (long)&word);
    return word;
}

/* The register changes made by each tracing case. */
enum { T_BLOCK, T_CANCEL, T_NUMBER, T_RESTORE, T_NO_ORIG, T_NO_SIGNAL, T_COUNT };

static const char *const trace_names[T_COUNT] = {"trace-block",   "trace-cancel",
                                                 "trace-number",  "trace-restore",
                                                 "trace-no-orig", "trace-no-signal"};

/** Wait for `*pid` to stop and return its signal, or -1 for another end.
 * Clear the owned pid when wait reaps it, so cleanup cannot signal it. */
static int trace_stop(int *pid)
{
    int status = 0;

    if (syscall4(SYS_WAIT4, *pid, (long)&status, 0, 0) != *pid) {
        return -1;
    }
    if ((status & 0xFF) != 0x7F) {
        *pid = 0;
        return -1;
    }
    return (status >> 8) & 0xFF;
}

/** Resume `*pid` to its next system-call stop with `sig`; return the
 * stop's signal, or -1 when the request or wait fails. */
static int next_stop(int *pid, int sig)
{
    if (syscall4(SYS_PTRACE, PTRACE_SYSCALL, *pid, 0, sig) != 0) {
        return -1;
    }
    return trace_stop(pid);
}

/** Write `value` to the tracee's register at `off`; whether it succeeded. */
static int poke(int pid, long off, long value)
{
    return syscall4(SYS_PTRACE, PTRACE_POKEUSER, pid, off, value) == 0;
}

/** The tracee of `mode`: stop, make the call, and check its edited result. */
static void tracee(int mode)
{
    struct ts half = {0, 500000000L};
    long r, want = 0, me = syscall0(SYS_GETPID);

    hits = 0;
    install(SIGUSR1, on_usr1, SA_RESTART);
    if (syscall4(SYS_PTRACE, PTRACE_TRACEME, 0, 0, 0) != 0) {
        sys_exit(1);
    }
    syscall2(SYS_KILL, me, SIGSTOP);
    if (mode == T_NO_SIGNAL) {
        r = syscall0(SYS_GETPID);
    } else {
        /* interval: the sleep whose restart registers the tracer edits */
        r = syscall2(SYS_NANOSLEEP, (long)&half, 0);
    }
    if (mode == T_NUMBER) {
        want = me;
    } else if (mode == T_NO_ORIG || mode == T_NO_SIGNAL) {
        want = -512;
    }
    sys_exit(r == want && hits == (mode == T_CANCEL ? 1 : 0) ? 0 : 1);
}

/** Reach the entry stop of call `nr` in `*pid`; whether it was found. */
static int trace_entry(int *pid, long nr)
{
    int c;

    for (c = 0; c < 8; c++) {
        if (next_stop(pid, 0) != SIGTRAP) {
            return 0;
        }
        if ((long)(int)peek(*pid, USER_ORIG) == nr) {
            return 1;
        }
    }
    return 0;
}

/** Stop the nanosleep at its interrupted exit and its signal delivery.
 * Check and write back the restart code in the tracer's register width. */
static int trace_interrupt(int *pid, int mode)
{
    int sig = mode == T_CANCEL ? SIGUSR1 : SIGWINCH;
    long code = mode == T_CANCEL ? -512 : -516;
    long v;

    if (syscall4(SYS_PTRACE, PTRACE_SYSCALL, *pid, 0, 0) != 0 || !wait_sleeping(*pid) ||
        syscall2(SYS_KILL, *pid, sig) != 0 || trace_stop(pid) != SIGTRAP) {
        return 0;
    }
    v = (long)(int)peek(*pid, USER_RET);
    if (v != -516) {
        miss(trace_names[mode], -1, "had at its exit stop", v, -516);
        return 0;
    }
    if (!poke(*pid, USER_RET, code) || next_stop(pid, 0) != sig) {
        return 0;
    }
    v = (long)(int)peek(*pid, USER_RET);
    if (v != code) {
        miss(trace_names[mode], -1, "had at its signal stop", v, code);
        return 0;
    }
    return 1;
}

/** Cancel the restart at signal delivery, run the handler, and require
 * exit_group after sigreturn, with no repeated nanosleep between them. */
static int trace_cancel(int *pid)
{
    long v;

    if (!poke(*pid, USER_RET, 0) || next_stop(pid, SIGUSR1) != SIGTRAP ||
        (long)(int)peek(*pid, USER_ORIG) != SYS_RT_SIGRETURN || next_stop(pid, 0) != SIGTRAP ||
        (long)(int)peek(*pid, USER_ORIG) != -1 || next_stop(pid, 0) != SIGTRAP) {
        return 0;
    }
    v = (long)(int)peek(*pid, USER_ORIG);
    if (v != SYS_EXIT_GROUP) {
        miss(trace_names[T_CANCEL], -1, "called after its handler", v, SYS_EXIT_GROUP);
        return 0;
    }
    return 1;
}

/** Apply `mode` at the stopped tracee and check a restart's next entry.
 * The caller continues and reaps the child, or kills it on failure. */
static int trace_edit(int *pid, int mode)
{
    unsigned long saved[USER_WORDS];
    long nr, v;

    if (mode == T_NO_SIGNAL) {
        return next_stop(pid, 0) == SIGTRAP && poke(*pid, USER_RET, -512);
    }
    if (!trace_interrupt(pid, mode)) {
        return 0;
    }
    if (mode == T_CANCEL) {
        return trace_cancel(pid);
    }
    if (mode == T_NO_ORIG) {
        return poke(*pid, USER_ORIG, -1) && poke(*pid, USER_RET, -512);
    }
    if (mode == T_NUMBER) {
        if (!poke(*pid, USER_ORIG, SYS_GETPID) || !poke(*pid, USER_RET, -512)) {
            return 0;
        }
    } else if (mode == T_RESTORE) {
        if (syscall4(SYS_PTRACE, PTRACE_GETREGS, *pid, 0, (long)&saved) != 0 ||
            !poke(*pid, USER_ORIG, -1) || !poke(*pid, USER_RET, 0) ||
            syscall4(SYS_PTRACE, PTRACE_SETREGS, *pid, 0, (long)&saved) != 0) {
            return 0;
        }
    }
    nr = mode == T_NUMBER ? SYS_GETPID : SYS_RESTART_SYSCALL;
    v = next_stop(pid, 0) == SIGTRAP ? (long)(int)peek(*pid, USER_ORIG) : -1;
    if (v != nr) {
        miss(trace_names[mode], -1, "restarted with orig", v, nr);
        return 0;
    }
    if (mode == T_BLOCK) {
        v = next_stop(pid, 0) == SIGTRAP ? (long)(int)peek(*pid, USER_RET) : -1;
        if (v != 0) {
            miss(trace_names[mode], -1, "left restart_syscall with", v, 0);
            return 0;
        }
    }
    return 1;
}

/** Run one tracing case, cleaning up only its own unreaped child. */
static void trace_case(int mode)
{
    int pid, status = 0, ok = 0;
    long nr = mode == T_NO_SIGNAL ? SYS_GETPID : NR_SLEEP;

    pid = (int)syscall0(SYS_FORK);
    if (pid < 0) {
        miss(trace_names[mode], -1, "could not fork", pid, 0);
        return;
    }
    if (pid == 0) {
        tracee(mode);
    }
    if (trace_stop(&pid) != SIGSTOP || !trace_entry(&pid, nr) || !trace_edit(&pid, mode)) {
        miss(trace_names[mode], -1, "could not reach or edit its stop", 0, 1);
        goto out;
    }
    if (syscall4(SYS_PTRACE, PTRACE_CONT, pid, 0, 0) != 0) {
        goto out;
    }
    if (syscall4(SYS_WAIT4, pid, (long)&status, 0, 0) == pid && (status & 0xFF) != 0x7F) {
        pid = 0;
        ok = status == 0;
    }
out:
    if (pid > 0) {
        syscall2(SYS_KILL, pid, 9);
        syscall4(SYS_WAIT4, pid, (long)&status, 0, 0);
    }
    if (!ok) {
        miss(trace_names[mode], -1, "ended with status", status, 0);
    }
}

/** The row `trace`, including edits at both syscall and signal stops. */
static void trace_row(void)
{
    int mode;

    report_part("trace");
    trace_case(T_BLOCK);
    report_part("trace-edits");
    for (mode = 1; mode < T_COUNT; mode++) {
        trace_case(mode);
    }
}

/**
 * thread_spawn(fn, stack_top) starts a thread of the probe's process that
 * runs `fn` on the stack ending at `stack_top`, aligned to 16, and ends when
 * `fn` returns; the thread's id, or a negated errno. The flags are Linux's
 * CLONE_VM, CLONE_FS, CLONE_FILES, CLONE_SIGHAND, CLONE_THREAD and
 * CLONE_SYSVSEM, 0x50f00.
 */
long thread_spawn(void (*fn)(void), void *stack_top);

#if defined __x86_64__
__asm__(".text\n"
        ".globl thread_spawn\n"
        "thread_spawn:\n"
        "    subq $16, %rsi\n"
        "    movq %rdi, (%rsi)\n"
        "    movl $0x50f00, %edi\n"
        "    xorl %edx, %edx\n"
        "    xorl %r10d, %r10d\n"
        "    xorl %r8d, %r8d\n"
        "    movl $56, %eax\n"
        "    syscall\n"
        "    testq %rax, %rax\n"
        "    jnz 1f\n"
        "    popq %rax\n"
        "    andq $-16, %rsp\n"
        "    call *%rax\n"
        "    xorl %edi, %edi\n"
        "    movl $60, %eax\n"
        "    syscall\n"
        "1:  ret\n");
#else
__asm__(".text\n"
        ".globl thread_spawn\n"
        "thread_spawn:\n"
        "    pushl %ebx\n"
        "    pushl %esi\n"
        "    pushl %edi\n"
        "    movl 20(%esp), %ecx\n"
        "    subl $16, %ecx\n"
        "    movl 16(%esp), %eax\n"
        "    movl %eax, (%ecx)\n"
        "    movl $0x50f00, %ebx\n"
        "    xorl %edx, %edx\n"
        "    xorl %esi, %esi\n"
        "    xorl %edi, %edi\n"
        "    movl $120, %eax\n"
        "    int $0x80\n"
        "    testl %eax, %eax\n"
        "    jnz 1f\n"
        "    popl %eax\n"
        "    call *%eax\n"
        "    xorl %ebx, %ebx\n"
        "    movl $1, %eax\n"
        "    int $0x80\n"
        "1:  popl %edi\n"
        "    popl %esi\n"
        "    popl %ebx\n"
        "    ret\n");
#endif

/* The race row's pipes: the tracer's cue to the second thread, the second
 * thread's answer once its handler has run, and the pipe the traced thread
 * reads. The thread that ran the handler, by its id, and that thread's
 * stack. */
static int race_cue[2], race_done[2], race_data[2];
static volatile long taker;
static char race_stack[16384] __attribute__((aligned(16)));

/** The race row's SIGUSR1 handler: note the thread that ran it. */
static void on_race(int signo)
{
    (void)signo;
    taker = syscall0(SYS_GETTID);
}

/** The race row's second thread, which starts with SIGUSR1 blocked: at the
 * tracer's cue unblock it, so that the thread takes the signal pending for
 * its process and runs the handler, then answer. */
static void race_thread(void)
{
    unsigned int set[2];
    char byte = 0;

    set[0] = 1u << (SIGUSR1 - 1);
    set[1] = 0;
    if (syscall3(SYS_READ, race_cue[0], (long)&byte, 1) == 1) {
        syscall4(SYS_RT_SIGPROCMASK, SIG_UNBLOCK, (long)set, 0, 8);
    }
    syscall3(SYS_WRITE, race_done[1], (long)&byte, 1);
}

/** The race row's tracee: start the second thread with SIGUSR1 blocked,
 * stop for the tracer, and read a byte; exit 0 when the read gave it and
 * the second thread ran the handler, 4 when the read returned a restart
 * code, 5 for another result and 6 when the handler ran elsewhere. */
static void race_subject(void)
{
    unsigned int set[2];
    long me = syscall0(SYS_GETPID), second, r;
    char byte;

    set[0] = 1u << (SIGUSR1 - 1);
    set[1] = 0;
    install(SIGUSR1, on_race, 0);
    syscall4(SYS_RT_SIGPROCMASK, SIG_BLOCK, (long)set, 0, 8);
    second = thread_spawn(race_thread, race_stack + sizeof(race_stack));
    if (second < 0) {
        sys_exit(2);
    }
    syscall4(SYS_RT_SIGPROCMASK, SIG_UNBLOCK, (long)set, 0, 8);
    if (syscall4(SYS_PTRACE, PTRACE_TRACEME, 0, 0, 0) != 0) {
        sys_exit(3);
    }
    syscall3(SYS_TGKILL, me, me, SIGSTOP);
    r = syscall3(SYS_READ, race_data[0], (long)&byte, 1);
    if (r != 1) {
        sys_exit(r <= -512 && r >= -516 ? 4 : 5);
    }
    sys_exit(taker == second ? 0 : 6);
}

/** Drive the race in the stopped tracee `*pid`: let its read sleep, send
 * its process SIGUSR1, which only it does not block, and hold it at the
 * read's exit stop while the second thread takes the signal; then require
 * the read's entry stop again. Whether each step held. */
static int race_drive(int *pid)
{
    char byte = 'r';
    long v;

    if (trace_stop(pid) != SIGSTOP || !trace_entry(pid, SYS_READ) ||
        syscall4(SYS_PTRACE, PTRACE_SYSCALL, *pid, 0, 0) != 0 || !wait_sleeping(*pid) ||
        syscall2(SYS_KILL, *pid, SIGUSR1) != 0 || trace_stop(pid) != SIGTRAP) {
        miss("race", -1, "could not reach the read's exit stop", 0, 1);
        return 0;
    }
    v = (long)(int)peek(*pid, USER_RET);
    if (v != -512) {
        miss("race", -1, "had at the read's exit stop", v, -512);
        return 0;
    }
    if (syscall3(SYS_WRITE, race_cue[1], (long)&byte, 1) != 1 ||
        syscall3(SYS_READ, race_done[0], (long)&byte, 1) != 1) {
        miss("race", -1, "had no answer from the second thread", 0, 1);
        return 0;
    }
    v = next_stop(pid, 0) == SIGTRAP ? (long)(int)peek(*pid, USER_ORIG) : -1;
    if (v != SYS_READ) {
        miss("race", -1, "called after the signal another thread took", v, SYS_READ);
        return 0;
    }
    if (syscall4(SYS_PTRACE, PTRACE_DETACH, *pid, 0, 0) != 0 ||
        syscall3(SYS_WRITE, race_data[1], (long)&byte, 1) != 1) {
        miss("race", -1, "could not detach and wake the read", 0, 1);
        return 0;
    }
    return 1;
}

/** The row `race`: a read whose process-directed signal another thread of
 * its process took before the read returned restarts, and the program
 * never sees the restart code. */
static void race_row(void)
{
    int pid = -1, status = 0, ok = 0;

    report_part("race");
    race_cue[0] = race_cue[1] = race_done[0] = race_done[1] = -1;
    race_data[0] = race_data[1] = -1;
    if (syscall1(SYS_PIPE, (long)race_cue) != 0 || syscall1(SYS_PIPE, (long)race_done) != 0 ||
        syscall1(SYS_PIPE, (long)race_data) != 0) {
        miss("race", -1, "could not make its pipes", 0, 0);
        goto out;
    }
    pid = (int)syscall0(SYS_FORK);
    if (pid < 0) {
        miss("race", -1, "could not fork", pid, 0);
        goto out;
    }
    if (pid == 0) {
        race_subject();
    }
    if (!race_drive(&pid)) {
        goto out;
    }
    if (syscall4(SYS_WAIT4, pid, (long)&status, 0, 0) == pid) {
        pid = 0;
        ok = status == 0;
    }
    if (!ok) {
        miss("race", -1, "ended with status", status, 0);
    }
out:
    if (pid > 0) {
        syscall2(SYS_KILL, pid, 9);
        syscall4(SYS_WAIT4, pid, (long)&status, 0, 0);
    }
    close_pipe(race_cue);
    close_pipe(race_done);
    close_pipe(race_data);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run the rows in order; `_start` calls it with the initial stack. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    long direct = 1, mem;
    int scen;

    report_begin(report_case(argv + argc + 1, "guest.default.restartprobe"));
    mem = probe_mmap(0, 4096, PROT_RW, MAP_SHARED_ANON, -1, 0);
    shared_word = (volatile int *)mem;
    if (sys_failed(mem)) {
        probe_line("restartprobe: fail: mmap", 0, 0);
        sys_exit(1);
    }
    for (scen = 0; scen < S_COUNT; scen++) {
        scenario(scen, &direct);
    }
    report_part("direct");
    if (syscall0(SYS_RESTART_SYSCALL) != -EINTR) {
        miss("restart_syscall", -1, "made directly returned", syscall0(SYS_RESTART_SYSCALL),
             -EINTR);
    }
    if (direct != -EINTR) {
        miss("restart_syscall", S_PLAIN, "after a handler returned", direct, -EINTR);
    }
    trace_row();
    race_row();
    report_finish();
    probe_line(failed ? "RESTARTPROBE FAIL" : "RESTARTPROBE OK", 0, 0);
    sys_exit(failed);
}
