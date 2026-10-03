/**
 * Exercises signal delivery from user space via direct system calls.
 *
 * Its checks:
 *   * install a SIGUSR1 handler (with a tiny rt_sigreturn restorer), raise it
 *     to ourselves, and confirm the handler runs and resumes after it;
 *   * block SIGUSR1, raise it (stays pending, handler must NOT run), then
 *     unblock and confirm the now-pending signal is delivered;
 *   * fork a child that spins; SIGTERM it and confirm the default action
 *     terminates it (parent reaps WTERMSIG == SIGTERM);
 *   * a handler that writes a selector naming no descriptor into the saved
 *     `gs` of its own frame, which the kernel loads at ring 0 on the way
 *     back, or on x86-64 into the saved `cs` and `ss`, which `iretq` loads:
 *     the machine must survive it, since a process may not choose what the
 *     kernel loads;
 *   * a one-shot handler of `SA_RESETHAND`, which must run once on its signal;
 *   * a signal sent to a sleeping child, which must end that child's sleep
 *     where it lands and not at its deadline. What says which happened is
 *     `sleeps_interrupted` in `/proc/stat`, the kernel's count of the sleeps
 *     a signal cut short: a sleep that ran its whole length does not raise
 *     it. The count is the assertion, not the seconds the sleep took;
 *   * the real-time interval timer of `setitimer`, `getitimer` and `alarm`,
 *     judged by what the kernel reports and counts: a blocked alarm that does
 *     not end a sleep and runs its handler once unblocked, an interval timer
 *     that ends three sleeps, a child with no timer, and the timers the
 *     kernel does not keep refused.
 *
 * Prints "SIGPROBE OK" on success. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define O_RDONLY 0

#define SIGUSR1 10
#define SIGALRM 14
#define SIGTERM 15

#define EINTR  4
#define EINVAL 22

#define ITIMER_REAL 0
#define ITIMER_PROF 2

#define SA_SIGINFO   0x00000004u
#define SA_RESTORER  0x04000000u
#define SA_RESETHAND 0x80000000u

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1

struct k_sigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned mask[2];
};

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
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"\n", 1);
}

static int fails = 0;

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, STDOUT_FILENO, (long)"  FAIL: ", 8);
        puts(msg);
    }
}

/**
 * The restorer trampoline, musl-style: when the handler returns to it, invoke
 * rt_sigreturn to restore the interrupted context.
 */
extern void sig_restorer(void);
PROBE_RT_RESTORER(sig_restorer);

static volatile int handler_ran;
static volatile int handler_sig;
static volatile int forge_ran;

/* A selector with a ring-3 RPL whose index names no descriptor: the GDT has
 * a handful of entries, so index 8191 is past its limit. A kernel that loaded
 * it with `mov %ax, %gs` at ring 0, or returned through it with `iretq`, would
 * take a general-protection fault in kernel mode and panic. */
#define BAD_SELECTOR 0xFFFFu

static void usr1_handler(int sig)
{
    handler_ran = 1;
    handler_sig = sig;
}

#if defined __x86_64__
/* The saved `cs` and `ss` in the ucontext the kernel built for an SA_SIGINFO
 * handler, its third argument: `uc_mcontext` starts 40 bytes in, and the
 * selectors are at 144 and 150 in it. The restorer's rt_sigreturn returns
 * through them with `iretq`. */
#define SAVED_CS(uc) ((volatile unsigned short *)((char *)(uc) + 40 + 144))
#define SAVED_SS(uc) ((volatile unsigned short *)((char *)(uc) + 40 + 150))

static void forge_siginfo_handler(int sig, void *info, void *uc)
{
    forge_ran = 1;
    (void)sig;
    (void)info;
    *SAVED_CS(uc) = BAD_SELECTOR;
    *SAVED_SS(uc) = BAD_SELECTOR;
}

/* The handler as install takes it: a handler's own type, whose SA_SIGINFO
 * call passes the other two arguments in RSI and RDX. */
#define forge_handler ((void (*)(int))(void (*)(void))forge_siginfo_handler)
#else
/* The saved `gs` in the Linux rt frame the kernel built for an SA_SIGINFO
 * handler: `&signo` is followed by the siginfo and ucontext pointers, and the
 * ucontext's `uc_mcontext` starts 20 bytes in with `gs` as its first word.
 * The restorer's rt_sigreturn reloads it from there. */
#define SAVED_GS_FROM_ARG(argp) \
    ((volatile unsigned *)(*(char *const *)((const char *)(argp) + 2 * 4) + 5 * 4))

static void forge_handler(int sig)
{
    forge_ran = 1;
    (void)sig;
    *SAVED_GS_FROM_ARG(&sig) = BAD_SELECTOR;
}
#endif

static void install_with(int signo, void (*h)(int), unsigned long extra)
{
    struct k_sigaction ka;

    ka.handler = h;
    ka.flags = SA_SIGINFO | SA_RESTORER | extra;
    ka.restorer = sig_restorer;
    ka.mask[0] = 0;
    ka.mask[1] = 0;
    check(syscall4(SYS_RT_SIGACTION, signo, (long)&ka, 0, 8) == 0, "rt_sigaction");
}

/** The decimal after `key` in `text`, or -1 if the key is not there. */
static int number_after(const char *text, int len, const char *key)
{
    int klen = (int)strlen(key);
    int i, j;

    for (i = 0; i + klen <= len; i++) {
        for (j = 0; j < klen && text[i + j] == key[j]; j++) {
        }
        if (j == klen) {
            int value = 0;
            i += klen;
            if (i >= len || text[i] < '0' || text[i] > '9') {
                return -1;
            }
            while (i < len && text[i] >= '0' && text[i] <= '9') {
                value = value * 10 + (text[i] - '0');
                i++;
            }
            return value;
        }
    }
    return -1;
}

/** The kernel's count of the sleeps a signal cut short, or -1. */
static int sleeps_cut_short(void)
{
    static char text[2048];
    int fd = syscall3(SYS_OPEN, (long)"/proc/stat", O_RDONLY, 0);
    int n;

    if (fd < 0) {
        return -1;
    }
    n = syscall3(SYS_READ, fd, (long)text, (int)sizeof(text));
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return -1;
    }
    return number_after(text, n, "sleeps_interrupted ");
}

/**
 * Whether process `pid` is asleep: `S` in the state field of its
 * `/proc/<pid>/stat`, the field after the parenthesised name.
 */
static int asleep(int pid)
{
    static char path[32], text[256];
    static const char head[] = "/proc/", tail[] = "/stat";
    char digits[12];
    int n = 0, i = 0, j, fd, len;

    do {
        digits[n++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0 && n < 11);
    for (j = 0; head[j]; j++) {
        path[i++] = head[j];
    }
    while (n > 0) {
        path[i++] = digits[--n];
    }
    for (j = 0; tail[j]; j++) {
        path[i++] = tail[j];
    }
    path[i] = '\0';
    fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    len = syscall3(SYS_READ, fd, (long)text, (int)sizeof(text));
    syscall1(SYS_CLOSE, fd);
    for (i = 0; i + 2 < len; i++) {
        if (text[i] == ')' && text[i + 1] == ' ') {
            return text[i + 2] == 'S';
        }
    }
    return 0;
}

/** Sleep `ms` milliseconds; returns what the call returned. */
static int sleep_ms(int ms)
{
    long req[2];

    req[0] = ms / 1000;
    req[1] = (ms % 1000) * 1000000;
    return syscall2(SYS_NANOSLEEP, (long)req, 0);
}

static void install(int signo, void (*h)(int))
{
    install_with(signo, h, 0);
}

static volatile int alarms;

static void alarm_handler(int sig)
{
    (void)sig;
    alarms++;
}

/**
 * Arm the real-time timer for `value_ms` and then every `interval_ms`; `old`,
 * when it is not null, receives what the timer held.
 */
static int itimer(int value_ms, int interval_ms, long *old)
{
    long set[4];

    set[0] = interval_ms / 1000;
    set[1] = (interval_ms % 1000) * 1000;
    set[2] = value_ms / 1000;
    set[3] = (value_ms % 1000) * 1000;
    return syscall3(SYS_SETITIMER, ITIMER_REAL, (long)set, (long)old);
}

/** Whether the real-time timer reads as disarmed, all four fields zero. */
static int itimer_disarmed(void)
{
    static long cur[4];

    return syscall2(SYS_GETITIMER, ITIMER_REAL, (long)cur) == 0 && cur[0] == 0 && cur[1] == 0 &&
           cur[2] == 0 && cur[3] == 0;
}

/**
 * The real-time timer: setitimer, getitimer and alarm.
 *
 * What is judged is what the kernel reports and counts, never how long a
 * wait took. A single alarm armed with SIGALRM blocked must not end a sleep
 * of a second, must leave the timer disarmed once it fires, and its handler
 * must run once when SIGALRM is unblocked; the alarm is due at the third
 * tick and the sleep ends at the hundred and first, so no scheduling can put
 * the one after the other. An interval
 * timer ends three sleeps of five seconds in turn, which the kernel counts
 * as sleeps cut short. getitimer reports the interval as set and a value no
 * longer than the one set; a fork's child has no timer; alarm returns the
 * seconds left before the alarm it replaces; and a timer other than the
 * three is refused though its time is valid, and arms nothing. Every row
 * leaves the timer disarmed.
 */
static void timer_rows(void)
{
    static long cur[4], old[4];
    static const long bad[4] = {0, 0, 0, 1000000};
    static const long second[4] = {0, 0, 1, 0};
    unsigned alrm[2];
    int i, ended = 0, pid, status, before;

    install(SIGALRM, alarm_handler);
    check(syscall3(SYS_SETITIMER, ITIMER_PROF + 1, (long)second, 0) == -EINVAL && itimer_disarmed(),
          "a timer other than the three is refused and arms nothing");
    check(syscall3(SYS_SETITIMER, ITIMER_REAL, (long)bad, 0) == -EINVAL,
          "a million microseconds is refused");

    alrm[0] = 1u << (SIGALRM - 1);
    alrm[1] = 0;
    alarms = 0;
    check(syscall4(SYS_RT_SIGPROCMASK, SIG_BLOCK, (long)alrm, 0, 8) == 0 && itimer(20, 0, 0) == 0,
          "arm a single alarm with SIGALRM blocked");
    check(sleep_ms(1000) == 0 && itimer_disarmed(),
          "a blocked alarm does not end a sleep and leaves the timer disarmed");
    check(syscall4(SYS_RT_SIGPROCMASK, SIG_UNBLOCK, (long)alrm, 0, 8) == 0 && alarms == 1,
          "and its handler runs once SIGALRM is unblocked");

    before = sleeps_cut_short();
    alarms = 0;
    check(itimer(20, 20, 0) == 0, "arm an interval timer");
    for (i = 0; i < 3; i++) {
        ended += sleep_ms(5000) == -EINTR;
    }
    check(itimer(0, 0, old) == 0 && ended == 3 && alarms >= 3,
          "the interval timer ends three sleeps in turn");
    check(old[0] == 0 && old[1] == 20000, "and reports its interval as it is disarmed");
    check(sleeps_cut_short() >= before + 3, "the kernel counted the sleeps the timer cut short");

    check(itimer(5000, 250, 0) == 0 && syscall2(SYS_GETITIMER, ITIMER_REAL, (long)cur) == 0 &&
              cur[0] == 0 && cur[1] == 250000 && (cur[2] > 0 || cur[3] > 0) &&
              (cur[2] < 5 || (cur[2] == 5 && cur[3] == 0)),
          "getitimer reports the interval set and no more than the value set");
    check(itimer(0, 0, 0) == 0 && itimer_disarmed(), "a value of zero disarms the timer");

    check(itimer(3600000, 0, 0) == 0, "arm an alarm an hour away");
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        sys_exit(itimer_disarmed() ? 0 : 1);
    }
    check(pid > 0 && syscall3(SYS_WAITPID, pid, (long)&status, 0) == pid && status == 0,
          "a fork's child has no timer");
    check(itimer(0, 0, 0) == 0, "and the alarm is disarmed again");

    check(syscall1(SYS_ALARM, 3600) == 0, "alarm returns 0 when none was armed");
    i = syscall1(SYS_ALARM, 0);
    check(i >= 1 && i <= 3600, "alarm returns the seconds left before the alarm it replaces");
    check(syscall1(SYS_ALARM, 0) == 0 && itimer_disarmed(), "and alarm(0) leaves none");
}

PROBE_START void _start(void)
{
    int pid, status;
    unsigned set[2];

    /* 1. A handler runs on a raised signal, and execution resumes after it. */
    install(SIGUSR1, usr1_handler);
    handler_ran = 0;
    handler_sig = 0;
    syscall2(SYS_KILL, syscall1(SYS_GETPID, 0), SIGUSR1);
    check(handler_ran == 1 && handler_sig == SIGUSR1, "SIGUSR1 handler ran and resumed");

    /* 2. A blocked signal stays pending until unblocked. */
    set[0] = 1u << (SIGUSR1 - 1);
    set[1] = 0;
    handler_ran = 0;
    check(syscall4(SYS_RT_SIGPROCMASK, SIG_BLOCK, (long)set, 0, 8) == 0, "sigprocmask block");
    syscall2(SYS_KILL, syscall1(SYS_GETPID, 0), SIGUSR1);
    check(handler_ran == 0, "blocked signal does not deliver");
    check(syscall4(SYS_RT_SIGPROCMASK, SIG_UNBLOCK, (long)set, 0, 8) == 0, "sigprocmask unblock");
    check(handler_ran == 1, "unblocking delivers the pending signal");

    /* 3. Default action terminates a process that does not handle it. */
    pid = syscall1(SYS_FORK, 0);
    check(pid >= 0, "fork");
    if (pid == 0) {
        /* Child: spin so the parent's SIGTERM is delivered on a timer tick. */
        for (;;) {
            __asm__ __volatile__("" ::: "memory"); /* a spin, meant to last */
        }
    }
    syscall2(SYS_KILL, pid, SIGTERM);
    check(syscall3(SYS_WAITPID, pid, (long)&status, 0) == pid, "reap killed child");
    check((status & 0x7f) == SIGTERM, "child terminated by SIGTERM (WTERMSIG)");

    /* 4. A forged selector in the frame the kernel restores. The i386 return
       path loads gs, fs, es and ds with `mov` while still at ring 0, and the
       x86-64 one returns through cs and ss with `iretq`, so a selector past
       the GDT's limit would fault the kernel and not the process; the kernel
       must coerce it. Reaching the next statement at all is the proof, since
       the alternative is a machine that stops. */
    install(SIGUSR1, forge_handler);
    forge_ran = 0;
    syscall2(SYS_KILL, syscall1(SYS_GETPID, 0), SIGUSR1);
    check(forge_ran == 1, "the forging handler ran");
    check(syscall1(SYS_GETPID, 0) > 0, "the machine survives a forged selector in the frame");

    /* 5. A one-shot handler runs once. `SA_RESETHAND` puts the disposition
       back for the next signal, not for this one: a delivery that entered the
       disposition it had just cleared would jump to address zero. */
    install_with(SIGUSR1, usr1_handler, SA_RESETHAND);
    handler_ran = 0;
    handler_sig = 0;
    syscall2(SYS_KILL, syscall1(SYS_GETPID, 0), SIGUSR1);
    check(handler_ran == 1 && handler_sig == SIGUSR1, "a one-shot handler runs on its signal");

    /* 6. A signal ends a sleep where it lands. The child sleeps far longer
       than this probe waits; the parent waits until /proc shows it asleep,
       so the signal lands in the sleep however late the child runs, then
       signals it and requires the kernel's count of the sleeps a signal cut
       short to rise, which a sleep that ran to its deadline would not do.
       The wait is at most 500 sleeps of 10 ms, each one or two ticks, and a
       child not seen asleep by then fails its own row. */
    {
        int before = sleeps_cut_short();
        int i;

        check(before >= 0, "/proc/stat reports sleeps_interrupted");
        install(SIGUSR1, usr1_handler);
        pid = syscall1(SYS_FORK, 0);
        check(pid >= 0, "fork the sleeper");
        if (pid == 0) {
            sys_exit(sleep_ms(30000) == -4 ? 0 : 1); /* -EINTR is -4 */
        }
        for (i = 0; i < 500 && !asleep(pid); i++) {
            sleep_ms(10);
        }
        check(i < 500, "the sleeper is seen asleep before it is signalled");
        syscall2(SYS_KILL, pid, SIGUSR1);
        check(syscall3(SYS_WAITPID, pid, (long)&status, 0) == pid, "reap the sleeper");
        check((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0,
              "the sleeper's own sleep returned EINTR");
        check(sleeps_cut_short() > before, "the kernel counted a sleep the signal cut short");
    }

    /* 7. The real-time interval timer. */
    timer_rows();

    if (fails == 0) {
        puts("SIGPROBE OK");
    } else {
        puts("SIGPROBE FAIL");
    }
    sys_exit(fails);
}
