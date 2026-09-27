/**
 * ptrace, as GDB drives it.
 *
 * A child that calls PTRACE_TRACEME and stops itself is reported stopped to its
 * parent. The parent reads and changes a word of the child's memory with
 * PEEKDATA and POKEDATA, and through /proc/PID/mem; reads the registers with
 * GETREGS and the floating-point state with GETFPXREGS; steps one instruction
 * with SINGLESTEP, which moves the instruction pointer; plants a breakpoint
 * with POKETEXT on a read-only text page, continues to it, finds the child
 * stopped by SIGTRAP just past it, puts the text back, moves the instruction
 * pointer back with SETREGS and continues. PTRACE_SYSCALL stops the child at
 * the entry and the exit of getpid, with the call's number in orig_eax and its
 * result in eax at the exit, and bit 0x80 on the stop signal with
 * PTRACE_O_TRACESYSGOOD. At an entry stop the tracer changes the call through
 * orig_eax: getpid made getppid returns the parent's id, and a write made -1 is
 * not run and returns the eax the tracer set. A tracee SIGKILL reaches at the
 * entry stop of a write writes nothing. At the exit stop of a read a signal
 * interrupted, -1 in orig_eax keeps the read from being restarted after a
 * handler with SA_RESTART. PTRACE_O_TRACEEXEC stops the child after an execve
 * with the exec event, which waitid reports in si_status above the signal. A
 * debug register set through POKEUSER reads back through PEEKUSER, and DR7
 * enables a breakpoint on a user address, the last of which a breakpoint may
 * end at, and not on a kernel address. PTRACE_ATTACH stops a running process,
 * which runs on after PTRACE_DETACH, and PTRACE_SEIZE stops for the tracer one
 * that a stop signal has already stopped, which stays stopped after
 * PTRACE_DETACH until SIGCONT. PTRACE_INTERRUPT stops a seized tracee blocked
 * in a read, which is restarted after. A signal sent to a process whose first
 * thread its tracer holds stopped wakes the other thread, asleep in pause,
 * which takes it. The usage a tracer's wait4 reports for the end of a traced
 * thread that is not the first has the time of the children its process reaped.
 * A process another process traces, which ends, is reported to its tracer
 * before its parent may reap it, and then to the parent; the tracer's next wait
 * finds nothing left to wait for. A tracee SIGKILL ends sends its tracer, here
 * its parent, a SIGCHLD of CLD_KILLED with the signal.
 *
 * Prints "PTRACEMUSL OK" on success and "PTRACEMUSL FAIL (detail)"
 * otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile int target = 1234;

static int fail(const char *what)
{
    printf("PTRACEMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/* The function the breakpoint goes on; noinline, so it has an address. */
__attribute__((noinline)) static int marked(int x)
{
    return x * 3;
}

static int stopped_by(pid_t pid, int sig)
{
    int status;

    return waitpid(pid, &status, 0) == pid && WIFSTOPPED(status) && WSTOPSIG(status) == sig;
}

static int memory_and_registers(void)
{
    struct user_regs_struct regs, after;
    struct user_fpxregs_struct fpx;
    char path[32];
    long word;
    int status, fd;
    pid_t pid = fork();

    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        _exit(marked(target) == 3 * 5678 ? 0 : 1);
    }
    if (!stopped_by(pid, SIGSTOP)) {
        return fail("the child was not reported stopped");
    }
    errno = 0;
    word = ptrace(PTRACE_PEEKDATA, pid, (void *)&target, NULL);
    if (errno != 0 || word != 1234) {
        return fail("PEEKDATA");
    }
    if (ptrace(PTRACE_POKEDATA, pid, (void *)&target, (void *)5678L) != 0) {
        return fail("POKEDATA");
    }
    snprintf(path, sizeof(path), "/proc/%d/mem", (int)pid);
    fd = open(path, O_RDONLY);
    if (fd < 0 || pread(fd, &word, sizeof(int), (off_t)(uintptr_t)&target) != sizeof(int) ||
        (int)word != 5678) {
        return fail("/proc/PID/mem");
    }
    close(fd);
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 || regs.xcs != 0x1b) {
        return fail("GETREGS");
    }
    if (ptrace(PTRACE_GETFPXREGS, pid, NULL, &fpx) != 0 || fpx.cwd == 0) {
        return fail("GETFPXREGS");
    }
    if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGTRAP) ||
        ptrace(PTRACE_GETREGS, pid, NULL, &after) != 0 || after.eip == regs.eip) {
        return fail("SINGLESTEP");
    }

    {
        long text = ptrace(PTRACE_PEEKTEXT, pid, (void *)marked, NULL);
        long trap = (text & ~0xFFL) | 0xCC;

        if (ptrace(PTRACE_POKETEXT, pid, (void *)marked, (void *)trap) != 0 ||
            ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGTRAP)) {
            return fail("the breakpoint was not reached");
        }
        if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 ||
            regs.eip != (long)(uintptr_t)marked + 1) {
            return fail("the stop was not just past the breakpoint");
        }
        regs.eip = (long)(uintptr_t)marked;
        if (ptrace(PTRACE_POKETEXT, pid, (void *)marked, (void *)text) != 0 ||
            ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0 ||
            ptrace(PTRACE_CONT, pid, NULL, NULL) != 0) {
            return fail("putting the text back");
        }
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the traced child did not finish with the word the parent wrote");
    }
    return 0;
}

/** The exec event stop of `pid`, which waitid reports with the event above
 * the signal in si_status and waitpid with it in the status; 0, or 1 after
 * the failure is reported. */
static int exec_event(pid_t pid)
{
    siginfo_t si;
    int status;

    if (waitid(P_PID, (id_t)pid, &si, WSTOPPED | WNOWAIT) != 0 || si.si_code != CLD_TRAPPED ||
        si.si_status != (SIGTRAP | (PTRACE_EVENT_EXEC << 8))) {
        return fail("waitid's si_status of the exec event");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status) ||
        status >> 8 != (SIGTRAP | (PTRACE_EVENT_EXEC << 8))) {
        return fail("the exec event");
    }
    return 0;
}

/* Set debug register `n` of the stopped `pid` to `v`; the raw result. */
static long debug_set(pid_t pid, int n, unsigned long v)
{
    return ptrace(PTRACE_POKEUSER, pid, (void *)(offsetof(struct user, u_debugreg) + 4 * n),
                  (void *)v);
}

/*
 * DR7 enables a breakpoint only on user addresses: an execute breakpoint
 * at the kernel's 0x1000 in DR0 is refused, and a 4-byte write breakpoint
 * whose last byte is the last user address, 0xbfffffff, is taken; DR7 is
 * then cleared again.
 */
static int debug_bounds(pid_t pid)
{
    if (debug_set(pid, 0, 0x1000) != 0 || debug_set(pid, 7, 0x1) != -1 || errno != EINVAL) {
        return fail("an execute breakpoint on a kernel address was not refused");
    }
    if (debug_set(pid, 0, 0xbffffffcUL) != 0 ||
        debug_set(pid, 7, 0x1 | 0x1UL << 16 | 0x3UL << 18) != 0 || debug_set(pid, 7, 0) != 0) {
        return fail("a breakpoint ending at the last user address was refused");
    }
    return 0;
}

static int syscalls_and_exec(void)
{
    struct user_regs_struct regs;
    unsigned long dr;
    int status;
    pid_t pid = fork();

    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        syscall(SYS_getpid);
        execl("/bin/sh", "sh", "-c", "exit 6", (char *)0);
        _exit(1);
    }
    if (!stopped_by(pid, SIGSTOP) ||
        ptrace(PTRACE_SETOPTIONS, pid, NULL,
               (void *)(long)(PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXEC)) != 0) {
        return fail("PTRACE_SETOPTIONS");
    }
    dr = 0x1000;
    if (ptrace(PTRACE_POKEUSER, pid, (void *)offsetof(struct user, u_debugreg[0]), (void *)dr) !=
            0 ||
        ptrace(PTRACE_PEEKUSER, pid, (void *)offsetof(struct user, u_debugreg[0]), NULL) !=
            0x1000) {
        return fail("a debug register through POKEUSER and PEEKUSER");
    }
    if (debug_bounds(pid) != 0) {
        return 1;
    }
    for (;;) {
        if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGTRAP | 0x80) ||
            ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0) {
            return fail("a syscall stop");
        }
        if (regs.orig_eax == SYS_getpid) {
            break;
        }
    }
    if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGTRAP | 0x80) ||
        ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 || regs.orig_eax != SYS_getpid ||
        regs.eax != pid) {
        return fail("the exit stop of getpid");
    }
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0) {
        return fail("PTRACE_CONT before the exec");
    }
    if (exec_event(pid) != 0) {
        return 1;
    }
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 6) {
        return fail("the exec'd program");
    }
    return 0;
}

/* Run the stopped `pid` through syscall stops to the first whose orig_eax
 * is `nr`, which is that call's entry, into `regs`; 0, or -1. */
static int syscall_entry(pid_t pid, long nr, struct user_regs_struct *regs)
{
    do {
        if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGTRAP) ||
            ptrace(PTRACE_GETREGS, pid, NULL, regs) != 0) {
            return -1;
        }
    } while (regs->orig_eax != nr);
    return 0;
}

/*
 * At a system call's entry stop the tracer may change the call through
 * orig_eax: getpid made getppid returns the parent's id, and a write made -1
 * is not run and returns what the tracer left in eax, so the pipe stays
 * empty.
 */
static int syscall_changes(void)
{
    struct user_regs_struct regs;
    int p[2];
    int status;
    char c;
    pid_t pid;

    if (pipe(p) != 0) {
        return fail("pipe");
    }
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        if (syscall(SYS_getpid) != getppid()) {
            _exit(2);
        }
        _exit(syscall(SYS_write, p[1], "x", 1) == 42 ? 0 : 3);
    }
    close(p[1]);
    if (!stopped_by(pid, SIGSTOP) || syscall_entry(pid, SYS_getpid, &regs) != 0) {
        return fail("the entry stop of getpid");
    }
    regs.orig_eax = SYS_getppid;
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0 ||
        syscall_entry(pid, SYS_write, &regs) != 0) {
        return fail("the entry stop of write");
    }
    regs.orig_eax = -1;
    regs.eax = 42;
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0 ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status)) {
        return fail("the child whose calls were changed did not exit");
    }
    if (WEXITSTATUS(status) != 0) {
        return fail(WEXITSTATUS(status) == 2 ? "getpid changed to getppid ran as getpid"
                                             : "a write changed to -1 did not return 42");
    }
    if (read(p[0], &c, 1) != 0) {
        return fail("a write changed to -1 was run");
    }
    close(p[0]);
    return 0;
}

/** A handler of SIGUSR1 that does nothing, so the signal only interrupts
 * the call it comes during. */
static void interrupt_only(int sig)
{
    (void)sig;
}

/*
 * A tracer's -1 in orig_eax at the exit stop of a read a signal interrupted
 * keeps the read from being restarted, though the handler that runs has
 * SA_RESTART: the read returns EINTR, and a byte written after it is not
 * read.
 */
static int restart_cancelled(void)
{
    struct user_regs_struct regs;
    int p[2];
    int status;
    pid_t pid;

    if (pipe(p) != 0) {
        return fail("pipe");
    }
    pid = fork();
    if (pid == 0) {
        struct sigaction sa;
        char c;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = interrupt_only;
        sa.sa_flags = SA_RESTART;
        sigaction(SIGUSR1, &sa, NULL);
        close(p[1]);
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        _exit(read(p[0], &c, 1) == -1 && errno == EINTR ? 0 : 1);
    }
    if (!stopped_by(pid, SIGSTOP) || syscall_entry(pid, SYS_read, &regs) != 0) {
        return fail("the entry stop of read");
    }
    if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) != 0 || kill(pid, SIGUSR1) != 0 ||
        !stopped_by(pid, SIGTRAP) || ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 ||
        regs.orig_eax != SYS_read || regs.eax != -EINTR) {
        return fail("the exit stop of the interrupted read");
    }
    regs.orig_eax = -1;
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0 ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGUSR1) ||
        ptrace(PTRACE_CONT, pid, NULL, (void *)SIGUSR1) != 0 || write(p[1], "x", 1) != 1) {
        return fail("the signal of the interrupted read");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("a read whose restart the tracer cancelled was restarted");
    }
    close(p[0]);
    close(p[1]);
    return 0;
}

static int attach(void)
{
    int p[2];
    int status;
    char c;
    pid_t pid;

    if (pipe(p) != 0) {
        return fail("pipe");
    }
    pid = fork();
    if (pid == 0) {
        close(p[1]);
        _exit(read(p[0], &c, 1) == 1 && c == 'g' ? 0 : 1);
    }
    close(p[0]);
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGSTOP)) {
        return fail("PTRACE_ATTACH");
    }
    if (ptrace(PTRACE_DETACH, pid, NULL, NULL) != 0 || write(p[1], "g", 1) != 1) {
        return fail("PTRACE_DETACH");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the process did not run on after the detach");
    }
    close(p[1]);
    return 0;
}

/** End the child `pid` of a row that failed, so that nothing of the row runs
 * on, and report `what`. */
static int abandon(pid_t pid, const char *what)
{
    kill(pid, SIGKILL);
    waitpid(pid, NULL, __WALL);
    return fail(what);
}

static volatile sig_atomic_t usr1_taken;

static void on_usr1(int sig)
{
    (void)sig;
    usr1_taken = 1;
}

/** The second thread of the child `stopped_leader` makes: it writes its id
 * to the descriptor `*arg`, sleeps until its handler has taken a SIGUSR1,
 * and ends the process with 5. */
static void *usr1_sleeper(void *arg)
{
    pid_t self = (pid_t)syscall(SYS_gettid);

    if (write(*(int *)arg, &self, sizeof(self)) != sizeof(self)) {
        _exit(1);
    }
    while (!usr1_taken) {
        pause();
    }
    _exit(5);
}

/** The state letter the stat file at `path` gives, 0 when it cannot be
 * read. */
static char stat_state(const char *path)
{
    char line[256], *state;
    int fd, n;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    n = (int)read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    line[n] = '\0';
    state = strrchr(line, ')');
    return state && state[1] == ' ' ? state[2] : 0;
}

/** Whether thread `t` of process `pid` sleeps, by its state in
 * /proc/PID/task/T/stat. */
static int sleeping(pid_t pid, pid_t t)
{
    char path[64];

    snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", (int)pid, (int)t);
    return stat_state(path) == 'S';
}

/** Whether process `pid` has ended and waits to be reaped, by its state in
 * /proc/PID/stat. */
static int zombie(pid_t pid)
{
    char path[64];

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    return stat_state(path) == 'Z';
}

/**
 * A SIGUSR1 sent to a child whose first thread the parent has stopped with
 * PTRACE_ATTACH: the thread the signal names cannot take it, so the other
 * thread, asleep in pause with the signal unblocked, is woken to, and its
 * handler runs and it ends the process with 5.
 */
static int stopped_leader(void)
{
    pthread_t t;
    pid_t pid, sleeper = 0;
    int p[2], status = 0, i;

    if (pipe(p) != 0) {
        return fail("pipe");
    }
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        signal(SIGUSR1, on_usr1);
        if (pthread_create(&t, NULL, usr1_sleeper, &p[1]) != 0) {
            _exit(1);
        }
        for (;;) {
            pause();
        }
    }
    close(p[1]);
    if (read(p[0], &sleeper, sizeof(sleeper)) != sizeof(sleeper)) {
        return abandon(pid, "the child's second thread");
    }
    close(p[0]);
    for (i = 0; i < 500 && !sleeping(pid, sleeper); i++) {
        usleep(10000);
    }
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGSTOP)) {
        return abandon(pid, "PTRACE_ATTACH to the child's first thread");
    }
    kill(pid, SIGUSR1);
    for (i = 0; i < 1000 && waitpid(pid, &status, WNOHANG) == 0; i++) {
        usleep(10000);
    }
    if (i == 1000) {
        return abandon(pid, "a process signal did not wake the thread beside the stopped one");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 5) {
        return fail("the thread beside the stopped one did not take the signal");
    }
    return 0;
}

/* The i386 kernel's struct rusage, which the raw wait4 writes: two timevals
 * of 32-bit seconds and microseconds, then fourteen counters. */
struct rusage32 {
    long utime_sec;
    long utime_usec;
    long stime_sec;
    long stime_usec;
    long counters[14];
};

static int usage_pipe[2], go_pipe[2];

/** Spend user time until the kernel has charged `ticks` of it to the
 * process, or for a thousand ticks; the sum the loop computed, returned so
 * the compiler keeps the loop. */
static unsigned int spin_ticks(long ticks)
{
    volatile unsigned int x = 0;
    struct tms tm;
    clock_t start = times(&tm);
    unsigned int i;

    while ((long)tm.tms_utime < ticks && (long)(times(&tm) - start) < 1000) {
        for (i = 0; i < 200000u; i++) {
            x += i;
        }
    }
    return x;
}

/** The second thread of the child `traced_thread_usage` makes: it writes its
 * id and its process's reaped children's user time, then ends when told. */
static void *usage_thread(void *arg)
{
    struct tms tm;
    long out[2];
    char c;

    (void)arg;
    times(&tm);
    out[0] = (long)syscall(SYS_gettid);
    out[1] = (long)tm.tms_cutime;
    if (write(usage_pipe[1], out, sizeof(out)) != sizeof(out) || read(go_pipe[0], &c, 1) != 1) {
        return (void *)1L;
    }
    return NULL;
}

/**
 * A child reaps a grandchild that spent user time, then starts a second
 * thread, which the parent attaches to and lets end. The parent's wait4 with
 * __WALL reports the thread's end with at least the reaped grandchild's time,
 * which the process keeps, not the thread.
 */
static int traced_thread_usage(void)
{
    struct rusage32 ru;
    long got[2];
    pthread_t t;
    int status;
    pid_t pid, g;

    if (pipe(usage_pipe) != 0 || pipe(go_pipe) != 0) {
        return fail("pipe");
    }
    pid = fork();
    if (pid == 0) {
        g = fork();
        if (g == 0) {
            spin_ticks(20);
            _exit(0);
        }
        if (waitpid(g, &status, 0) != g || pthread_create(&t, NULL, usage_thread, NULL) != 0) {
            _exit(1);
        }
        pthread_join(t, NULL);
        _exit(0);
    }
    if (read(usage_pipe[0], got, sizeof(got)) != sizeof(got) || got[1] < 2) {
        return abandon(pid, "the child's thread or its reaped grandchild's time");
    }
    if (ptrace(PTRACE_ATTACH, (pid_t)got[0], NULL, NULL) != 0 ||
        waitpid((pid_t)got[0], &status, __WALL) != (pid_t)got[0] || !WIFSTOPPED(status) ||
        ptrace(PTRACE_CONT, (pid_t)got[0], NULL, NULL) != 0 || write(go_pipe[1], "g", 1) != 1) {
        return abandon(pid, "attaching to the child's second thread");
    }
    memset(&ru, 0xFF, sizeof(ru));
    if (syscall(SYS_wait4, got[0], &status, __WALL, &ru) != got[0] || !WIFEXITED(status) ||
        ru.utime_sec * 100 + ru.utime_usec / 10000 < got[1]) {
        return abandon(pid, "a traced thread's end lacks the reaped children's time");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the child with the traced thread");
    }
    return 0;
}

/**
 * The tracer of `tracer_first`: it seizes the child `c`, which is not its
 * own, tells the parent through `to_m`, waits for the word `from_m` brings,
 * then waits for the child's end, which it must see with status 9, and waits
 * again, which must find nothing. Returns 0, or the step that failed.
 */
static int tracer_side(pid_t c, int to_m, int from_m)
{
    int status;
    char ch;

    if (ptrace(PTRACE_SEIZE, c, NULL, NULL) != 0 || write(to_m, "a", 1) != 1) {
        return 1;
    }
    if (read(from_m, &ch, 1) != 1) {
        return 2;
    }
    if (waitpid(c, &status, 0) != c || !WIFEXITED(status) || WEXITSTATUS(status) != 9) {
        return 3;
    }
    if (waitpid(-1, &status, __WALL) != -1 || errno != ECHILD) {
        return 4;
    }
    return 0;
}

/**
 * A child, which a second child traces, ends with 9. Its parent's wait does
 * not reap it while the tracer has not seen the end; once the tracer's wait
 * has reported it, the parent reaps it with its status, and the tracer's
 * next wait answers ECHILD rather than waiting on a tracee that is gone.
 */
static int tracer_first(void)
{
    int to_c[2], to_m[2], to_x[2], status = 0, i;
    pid_t c, x, got = 0;
    char ch;

    if (pipe(to_c) != 0) {
        return fail("pipe");
    }
    c = fork();
    if (c == 0) {
        close(to_c[1]);
        _exit(read(to_c[0], &ch, 1) == 1 ? 9 : 1);
    }
    close(to_c[0]);
    if (pipe(to_m) != 0 || pipe(to_x) != 0) {
        return abandon(c, "pipe");
    }
    x = fork();
    if (x == 0) {
        close(to_c[1]);
        close(to_m[0]);
        close(to_x[1]);
        _exit(tracer_side(c, to_m[1], to_x[0]));
    }
    close(to_m[1]);
    close(to_x[0]);
    if (read(to_m[0], &ch, 1) != 1 || write(to_c[1], "x", 1) != 1) {
        kill(x, SIGKILL);
        return abandon(c, "the second child did not seize the first");
    }
    for (i = 0; i < 500 && !zombie(c); i++) {
        usleep(10000);
    }
    if (waitpid(c, &status, WNOHANG) != 0 || write(to_x[1], "g", 1) != 1) {
        kill(x, SIGKILL);
        return fail("the parent reaped a child before its tracer saw its end");
    }
    for (i = 0; i < 1000 && (got = waitpid(c, &status, WNOHANG)) == 0; i++) {
        usleep(10000);
    }
    if (got != c || !WIFEXITED(status) || WEXITSTATUS(status) != 9) {
        kill(x, SIGKILL);
        return fail("the parent did not reap the child once its tracer saw the end");
    }
    for (i = 0; i < 1000 && waitpid(x, &status, WNOHANG) == 0; i++) {
        usleep(10000);
    }
    close(to_c[1]);
    close(to_m[0]);
    close(to_x[1]);
    if (i == 1000) {
        return abandon(x, "the tracer's wait after the end did not return");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("PTRACEMUSL FAIL (the tracer's step %d)\n", WEXITSTATUS(status));
        return 1;
    }
    return 0;
}

/*
 * A tracee SIGKILL reaches at the entry stop of a write to a pipe writes
 * nothing, since the call does not run: the pipe then reads its end of
 * input.
 */
static int killed_at_entry(void)
{
    struct user_regs_struct regs;
    int p[2];
    int status;
    char c;
    pid_t pid;

    if (pipe(p) != 0) {
        return fail("pipe");
    }
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        _exit(write(p[1], "k", 1) == 1 ? 0 : 1);
    }
    close(p[1]);
    if (!stopped_by(pid, SIGSTOP) || syscall_entry(pid, SYS_write, &regs) != 0) {
        return abandon(pid, "the entry stop of the write to kill");
    }
    kill(pid, SIGKILL);
    if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        return fail("the tracee killed at an entry stop did not end by SIGKILL");
    }
    if (read(p[0], &c, 1) != 0) {
        return fail("a write the tracee was killed at the entry of ran");
    }
    close(p[0]);
    return 0;
}

/*
 * PTRACE_SEIZE of a process a stop signal has stopped, whose stop its
 * parent has already waited for, stops the thread for the tracer with
 * PTRACE_EVENT_STOP and the stop signal, and the process runs on after
 * PTRACE_DETACH and SIGCONT.
 */
static int attach_stopped(void)
{
    char path[64];
    int status;
    pid_t pid = fork();

    if (pid == 0) {
        raise(SIGSTOP);
        _exit(0);
    }
    if (waitpid(pid, &status, WUNTRACED) != pid || !WIFSTOPPED(status)) {
        return fail("the child to be seized did not stop");
    }
    if (ptrace(PTRACE_SEIZE, pid, NULL, NULL) != 0 || waitpid(pid, &status, 0) != pid ||
        !WIFSTOPPED(status) || status >> 8 != (SIGSTOP | (PTRACE_EVENT_STOP << 8))) {
        return fail("seizing a stopped process did not stop it for the tracer");
    }
    if (ptrace(PTRACE_DETACH, pid, NULL, NULL) != 0) {
        return fail("PTRACE_DETACH of the seized process");
    }
    usleep(100000);
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    if (waitpid(pid, &status, WNOHANG) != 0 || stat_state(path) != 'T') {
        return fail("the detached process ran while its stop was in effect");
    }
    kill(pid, SIGCONT);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the seized process did not run on after the detach");
    }
    return 0;
}

/*
 * PTRACE_INTERRUPT stops a seized tracee blocked in a read of an empty
 * pipe, with PTRACE_EVENT_STOP and SIGTRAP, and the read, restarted when the
 * tracer resumes it, reads the byte written after.
 */
static int interrupt_read(void)
{
    int p[2];
    int status;
    char c;
    pid_t pid;

    if (pipe(p) != 0) {
        return fail("pipe");
    }
    pid = fork();
    if (pid == 0) {
        close(p[1]);
        _exit(read(p[0], &c, 1) == 1 && c == 'g' ? 0 : 1);
    }
    close(p[0]);
    if (ptrace(PTRACE_SEIZE, pid, NULL, NULL) != 0 ||
        ptrace(PTRACE_INTERRUPT, pid, NULL, NULL) != 0 || waitpid(pid, &status, 0) != pid ||
        !WIFSTOPPED(status) || status >> 8 != (SIGTRAP | (PTRACE_EVENT_STOP << 8))) {
        return fail("PTRACE_INTERRUPT did not stop a tracee blocked in a read");
    }
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || write(p[1], "g", 1) != 1) {
        return fail("resuming the interrupted tracee");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the interrupted read was not restarted");
    }
    close(p[1]);
    return 0;
}

/*
 * A tracee that SIGKILL ends tells its tracer, which is its parent here,
 * with a SIGCHLD whose siginfo is CLD_KILLED and the signal, and the wait
 * reports it killed. SIGCHLD is blocked, and the one the stop sent taken,
 * so the siginfo read is the end's.
 */
static int killed_tracee(void)
{
    struct timespec now = {0, 0};
    struct timespec bound = {5, 0};
    sigset_t chld;
    siginfo_t si;
    int status;
    pid_t pid;

    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    sigprocmask(SIG_BLOCK, &chld, NULL);
    pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        _exit(0);
    }
    if (!stopped_by(pid, SIGSTOP)) {
        return fail("the tracee to be killed did not stop");
    }
    while (sigtimedwait(&chld, &si, &now) == SIGCHLD) {
    }
    if (kill(pid, SIGKILL) != 0 || sigtimedwait(&chld, &si, &bound) != SIGCHLD) {
        return fail("no SIGCHLD came of the killed tracee");
    }
    if (si.si_code != CLD_KILLED || si.si_status != SIGKILL || si.si_pid != pid) {
        return fail("the SIGCHLD of a killed tracee was not CLD_KILLED with SIGKILL");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        return fail("the killed tracee was not reported killed by SIGKILL");
    }
    sigprocmask(SIG_UNBLOCK, &chld, NULL);
    return 0;
}

int main(void)
{
    if (memory_and_registers() || syscalls_and_exec() || syscall_changes() || killed_at_entry() ||
        restart_cancelled() || attach() || attach_stopped() || interrupt_read() ||
        stopped_leader() || traced_thread_usage() || tracer_first() || killed_tracee()) {
        return 1;
    }
    printf("PTRACEMUSL OK\n");
    return 0;
}
