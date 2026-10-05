/**
 * ptrace, as GDB drives it.
 *
 * A child that calls PTRACE_TRACEME and stops itself is reported stopped to its
 * parent. The parent reads and changes a word of the child's memory with
 * PEEKDATA and POKEDATA, and through /proc/PID/mem, where a read and a write
 * that run from a page into a hole do the bytes before the hole, and whose
 * descriptor keeps none of the memory of a child that has ended: once a child
 * that touched 16 MiB has ended, and before it is reaped, a read through a
 * descriptor of its memory returns 0 and the free memory /proc/meminfo reports
 * is back; reads the registers with GETREGS and the floating-point state with
 * GETFPXREGS, GETFPREGS on x86-64; steps one instruction with SINGLESTEP, which
 * moves the instruction pointer; plants a breakpoint with POKETEXT on a
 * read-only text page, continues to it, finds the child stopped by SIGTRAP just
 * past it, puts the text back, moves the instruction pointer back with SETREGS
 * and continues. PTRACE_SYSCALL stops the child at the entry and the exit of
 * getpid, with the call's number in orig_eax and its result in eax at the exit
 * (orig_rax and rax on x86-64), and bit 0x80 on the stop signal with
 * PTRACE_O_TRACESYSGOOD. At an entry stop the tracer changes the call through
 * orig_eax: getpid made getppid returns the parent's id, and a write made -1 is
 * not run and returns the eax the tracer set. A tracee SIGKILL reaches at the
 * entry stop of a write writes nothing. At the exit stop of a read a signal
 * interrupted, -1 in orig_eax keeps the read from being restarted after a
 * handler with SA_RESTART. PTRACE_O_TRACEEXEC stops the child after an execve
 * with the exec event, which waitid reports in si_status above the signal, and
 * a /proc/PID/mem descriptor opened before the execve reads nothing of the new
 * image. A debug register set through POKEUSER reads back through PEEKUSER, and
 * DR7 enables a breakpoint on a user address, the last of which a breakpoint
 * may end at, and not on a kernel address. PTRACE_ATTACH stops a running
 * process, which runs on after PTRACE_DETACH, and PTRACE_SEIZE stops for the
 * tracer one that a stop signal has already stopped, which stays stopped after
 * PTRACE_DETACH until SIGCONT. PTRACE_INTERRUPT stops a seized tracee blocked
 * in a read, which is restarted after. The child a seized tracee forks under
 * PTRACE_O_TRACEFORK first stops with PTRACE_EVENT_STOP and SIGTRAP. A signal
 * sent to a process whose first thread its tracer holds stopped wakes the other
 * thread, asleep in pause, which takes it. The usage a tracer's wait4 reports
 * for the end of a traced thread that is not the first has the time of the
 * children its process reaped. A process another process traces, which ends, is
 * reported to its tracer before its parent may reap it, and then to the parent;
 * the tracer's next wait finds nothing left to wait for. A tracee SIGKILL ends
 * sends its tracer, here its parent, a SIGCHLD of CLD_KILLED with the signal.
 *
 * Prints "PTRACEMUSL OK" on success and "PTRACEMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * The fields of each architecture's user_regs_struct the rows read: the
 * call's number, its result, the instruction pointer and the code segment,
 * whose user selector is 0x1b on i386 and 0x33 on x86-64. The floating-point
 * state is the fxsave image, which i386 reads with GETFPXREGS and x86-64 with
 * GETFPREGS. LAST_USER_WORD is the last four bytes of user space: below the
 * kernel at 3 GiB on i386, and below the last page of the lower half on
 * x86-64, which user space does not reach.
 */
#if defined __x86_64__
#define R_BX(r)        ((r).rbx)
#define R_PTR(r)       ((r).rdi)
#define R_SP(r)        ((r).rsp)
#define R_NR(r)        ((r).orig_rax)
#define R_RET(r)       ((r).rax)
#define R_IP(r)        ((r).rip)
#define R_CS(r)        ((r).cs)
#define USER_CS        0x33
#define PTRACE_GETFX   PTRACE_GETFPREGS
#define LAST_USER_WORD 0x00007FFFFFFFEFFCUL
typedef struct user_fpregs_struct fxregs_t;
#else
#define R_BX(r)        ((r).ebx)
#define R_PTR(r)       ((r).esi)
#define R_SP(r)        ((r).esp)
#define R_NR(r)        ((r).orig_eax)
#define R_RET(r)       ((r).eax)
#define R_IP(r)        ((r).eip)
#define R_CS(r)        ((r).xcs)
#define USER_CS        0x1b
#define PTRACE_GETFX   PTRACE_GETFPXREGS
#define LAST_USER_WORD 0xbffffffcUL
typedef struct user_fpxregs_struct fxregs_t;
#endif

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

/**
 * A write and a read of 256 bytes through `path`, /proc/PID/mem of a child
 * that has the page `page` and a hole after it, from 128 bytes before the
 * hole: each does the 128 before the hole.
 */
static int into_hole(const char *path, char *page)
{
    char out[256], in[256];
    off_t at = (off_t)(uintptr_t)(page + 4096 - 128);
    int fd = open(path, O_RDWR);
    int ok;

    memset(out, 'b', sizeof(out));
    memset(in, 0, sizeof(in));
    ok = fd >= 0 && pwrite(fd, out, sizeof(out), at) == 128 &&
         pread(fd, in, sizeof(in), at) == 128 && memcmp(in, out, 128) == 0;
    if (fd >= 0) {
        close(fd);
    }
    return ok ? 0 : fail("a transfer through /proc/PID/mem that runs into a hole");
}

static int memory_and_registers(void)
{
    struct user_regs_struct regs, after;
    fxregs_t fpx;
    char path[32];
    long word;
    int status, fd;
    char *page = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pid_t pid;

    if (page == MAP_FAILED || munmap(page + 4096, 4096) != 0) {
        return fail("a page with a hole after it");
    }
    memset(page, 'a', 4096);
    pid = fork();

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
    if (into_hole(path, page) != 0) {
        return 1;
    }
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 || R_CS(regs) != USER_CS) {
        return fail("GETREGS");
    }
    if (ptrace(PTRACE_GETFX, pid, NULL, &fpx) != 0 || fpx.cwd == 0) {
        return fail("GETFPXREGS");
    }
    if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGTRAP) ||
        ptrace(PTRACE_GETREGS, pid, NULL, &after) != 0 || R_IP(after) == R_IP(regs)) {
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
            (uintptr_t)R_IP(regs) != (uintptr_t)marked + 1) {
            return fail("the stop was not just past the breakpoint");
        }
        R_IP(regs) = (uintptr_t)marked;
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

/* Set debug register `n` of the stopped `pid` to `v`; the raw result. The
 * registers are words of struct user, 4 bytes on i386 and 8 on x86-64. */
static long debug_set(pid_t pid, int n, unsigned long v)
{
    struct user u;

    return ptrace(PTRACE_POKEUSER, pid,
                  (void *)(offsetof(struct user, u_debugreg) + sizeof(u.u_debugreg[0]) * n),
                  (void *)v);
}

/*
 * DR7 enables a breakpoint only on user addresses: an execute breakpoint
 * at 0x1000 in DR0, which is not a user address, is refused, and a 4-byte
 * write breakpoint whose last byte is the last user address is taken; DR7
 * is then cleared again. A reserved bit, 12 or on x86-64 32, is refused,
 * since the processor faults on the second when the switch loads it.
 */
static int debug_bounds(pid_t pid)
{
    if (debug_set(pid, 0, 0x1000) != 0 || debug_set(pid, 7, 0x1) != -1 || errno != EINVAL) {
        return fail("an execute breakpoint on a kernel address was not refused");
    }
    if (debug_set(pid, 0, LAST_USER_WORD) != 0 ||
        debug_set(pid, 7, 0x1 | 0x1UL << 16 | 0x3UL << 18) != 0 || debug_set(pid, 7, 0) != 0) {
        return fail("a breakpoint ending at the last user address was refused");
    }
    if (debug_set(pid, 7, 0x1000) != -1 || errno != EINVAL) {
        return fail("DR7 with reserved bit 12 was not refused");
    }
#if defined __x86_64__
    if (debug_set(pid, 7, 0x1UL << 32) != -1 || errno != EINVAL) {
        return fail("DR7 with reserved bit 32 was not refused");
    }
#endif
    return 0;
}

/* The stopped `pid`'s debug registers through POKEUSER and PEEKUSER, their
 * bounds, and on x86-64 an option bit past 32 refused; 0, or 1. */
static int debug_and_options(pid_t pid)
{
    unsigned long dr = 0x1000;

    if (ptrace(PTRACE_POKEUSER, pid, (void *)offsetof(struct user, u_debugreg[0]), (void *)dr) !=
            0 ||
        ptrace(PTRACE_PEEKUSER, pid, (void *)offsetof(struct user, u_debugreg[0]), NULL) !=
            0x1000) {
        return fail("a debug register through POKEUSER and PEEKUSER");
    }
    if (debug_bounds(pid) != 0) {
        return 1;
    }
#if defined __x86_64__
    if (ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(1UL << 32)) != -1 || errno != EINVAL) {
        return fail("PTRACE_SETOPTIONS accepted an upper option bit");
    }
#endif
    return 0;
}

/* Run `pid` through syscall stops to getpid's entry, then to its exit with
 * the pid as its result; 0, or 1. */
static int getpid_stops(pid_t pid)
{
    struct user_regs_struct regs;

    for (;;) {
        if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGTRAP | 0x80) ||
            ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0) {
            return fail("a syscall stop");
        }
        if (R_NR(regs) == SYS_getpid) {
            break;
        }
    }
    if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGTRAP | 0x80) ||
        ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 || R_NR(regs) != SYS_getpid ||
        (long)R_RET(regs) != pid) {
        return fail("the exit stop of getpid");
    }
    return 0;
}

static int syscalls_and_exec(void)
{
    char path[32];
    long word;
    int status, mem;
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
    if (debug_and_options(pid) != 0 || getpid_stops(pid) != 0) {
        return 1;
    }
    snprintf(path, sizeof(path), "/proc/%d/mem", (int)pid);
    mem = open(path, O_RDONLY);
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0) {
        return fail("PTRACE_CONT before the exec");
    }
    if (exec_event(pid) != 0) {
        return 1;
    }
    if (mem < 0 || pread(mem, &word, sizeof(word), (off_t)(uintptr_t)&target) != 0) {
        return fail("a /proc/PID/mem descriptor opened before the exec");
    }
    close(mem);
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
    } while ((long)R_NR(*regs) != nr);
    return 0;
}

/*
 * At a system call's entry stop the tracer may change the call through
 * orig_eax: getpid made getppid returns the parent's id, and a write made -1
 * is not run and returns what the tracer left in eax, so the pipe stays
 * empty. On x86-64 a data segment selector other than the null one the
 * kernel keeps is refused with EIO, and not taken without being kept.
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
#if defined __x86_64__
    regs.ds = 0x2b;
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != -1 || errno != EIO) {
        return fail("a data segment selector the kernel does not keep was taken");
    }
    regs.ds = 0;
#endif
    R_NR(regs) = SYS_getppid;
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0 ||
        syscall_entry(pid, SYS_write, &regs) != 0) {
        return fail("the entry stop of write");
    }
    R_NR(regs) = -1;
    R_RET(regs) = 42;
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
        R_NR(regs) != SYS_read || (long)R_RET(regs) != -EINTR) {
        return fail("the exit stop of the interrupted read");
    }
    R_NR(regs) = -1;
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

/** MemFree of /proc/meminfo, in KiB, or -1 when it cannot be read. */
static long mem_free_kib(void)
{
    char text[1024];
    char *at;
    long kib = -1;
    ssize_t n;
    int fd = open("/proc/meminfo", O_RDONLY);

    if (fd < 0) {
        return -1;
    }
    n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    text[n] = '\0';
    at = strstr(text, "MemFree:");
    if (at == NULL || sscanf(at + 8, "%ld", &kib) != 1) {
        return -1;
    }
    return kib;
}

/*
 * A child touches 16 MiB and waits; the parent opens its /proc/PID/mem, reads
 * a word of it, and lets it end, which waitid with WNOWAIT reports and leaves
 * the child unreaped. Before the reap, with the descriptor still open, a read
 * through it must return 0 and the free memory /proc/meminfo reports must be
 * back within 8 MiB of what it was before the fork: the child's address space
 * went at its end, as Linux's exit_mm drops it, and the descriptor keeps only
 * its record.
 */
static int mem_after_end(void)
{
    int ready[2], go[2];
    long before, after;
    char path[32];
    siginfo_t si;
    int fd = -1, word = 0, rc = 0;
    ssize_t got;
    char c;
    pid_t pid;

    if (pipe(ready) != 0 || pipe(go) != 0) {
        return fail("pipe");
    }
    before = mem_free_kib();
    pid = fork();
    if (pid < 0) {
        return fail("fork");
    }
    if (pid == 0) {
        size_t size = (size_t)16 << 20;
        char *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        close(ready[0]);
        close(go[1]);
        if (m == MAP_FAILED) {
            _exit(1);
        }
        memset(m, 1, size);
        _exit(write(ready[1], "r", 1) == 1 && read(go[0], &c, 1) == 1 ? 0 : 1);
    }
    close(ready[1]);
    close(go[0]);
    snprintf(path, sizeof(path), "/proc/%d/mem", (int)pid);
    if (read(ready[0], &c, 1) == 1) {
        fd = open(path, O_RDONLY);
    }
    if (fd < 0 || pread(fd, &word, sizeof(word), (off_t)(uintptr_t)&target) != sizeof(word) ||
        write(go[1], "g", 1) != 1) {
        return abandon(pid, "a child whose memory was opened");
    }
    if (waitid(P_PID, (id_t)pid, &si, WEXITED | WNOWAIT) != 0 || si.si_code != CLD_EXITED ||
        si.si_status != 0) {
        return abandon(pid, "the child that touched 16 MiB");
    }
    got = pread(fd, &word, sizeof(word), (off_t)(uintptr_t)&target);
    after = mem_free_kib();
    close(fd);
    close(ready[0]);
    close(go[1]);
    waitpid(pid, NULL, 0);
    if (before < 0 || after < 0) {
        return fail("/proc/meminfo");
    }
    if (got != 0) {
        rc = fail("a /proc/PID/mem read reached a child that had ended");
    }
    if (after < before - 8192) {
        rc = fail("a /proc/PID/mem descriptor kept the memory of a child that ended");
    }
    return rc;
}

/** Wait up to ten seconds for a change in the child `pid` that waitpid
 * reports: the child's id with its status in `*status`, or 0 when none
 * came, so that a stop that never comes fails its row. */
static pid_t wait_within(pid_t pid, int *status)
{
    pid_t got = 0;
    int i;

    for (i = 0; i < 1000 && (got = waitpid(pid, status, WNOHANG)) == 0; i++) {
        usleep(10000);
    }
    return got;
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
    if (ptrace(PTRACE_SEIZE, pid, NULL, NULL) != 0 || wait_within(pid, &status) != pid ||
        !WIFSTOPPED(status) || status >> 8 != (SIGSTOP | (PTRACE_EVENT_STOP << 8))) {
        return abandon(pid, "seizing a stopped process did not stop it for the tracer");
    }
    if (ptrace(PTRACE_DETACH, pid, NULL, NULL) != 0) {
        return abandon(pid, "PTRACE_DETACH of the seized process");
    }
    usleep(100000);
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    if (waitpid(pid, &status, WNOHANG) != 0 || stat_state(path) != 'T') {
        return abandon(pid, "the detached process ran while its stop was in effect");
    }
    kill(pid, SIGCONT);
    if (wait_within(pid, &status) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return abandon(pid, "the seized process did not run on after the detach");
    }
    return 0;
}

/*
 * PTRACE_LISTEN holds a seized tracee in its group stop's PTRACE_EVENT_STOP
 * until the stop ends, and the end makes a new PTRACE_EVENT_STOP with
 * SIGTRAP, whichever comes first. A SIGCONT before the listen is strace's
 * start on two processors, which left the tracee stopped for good while the
 * listen did nothing; a SIGCONT after it is the order one processor gave.
 * The SIGCONT is then a signal-delivery stop, which PTRACE_LISTEN refuses
 * with EIO.
 */
static int expect_stop(pid_t pid, int want, const char *what)
{
    int status;

    if (wait_within(pid, &status) != pid || !WIFSTOPPED(status) || status >> 8 != want) {
        return abandon(pid, what);
    }
    return 0;
}

/* The seize of a stopped `pid`, PTRACE_INTERRUPT ending its PTRACE_LISTEN,
 * a new event stop when the stop ends, and the SIGCONT's own stop. */
static int listen_first(pid_t pid)
{
    if (ptrace(PTRACE_SEIZE, pid, NULL, NULL) != 0 ||
        expect_stop(pid, SIGSTOP | (PTRACE_EVENT_STOP << 8),
                    "the seize of the stopped process was not a group stop") ||
        ptrace(PTRACE_LISTEN, pid, NULL, NULL) != 0 ||
        ptrace(PTRACE_INTERRUPT, pid, NULL, NULL) != 0 ||
        expect_stop(pid, SIGSTOP | (PTRACE_EVENT_STOP << 8),
                    "PTRACE_INTERRUPT did not end PTRACE_LISTEN") ||
        kill(pid, SIGCONT) != 0 || ptrace(PTRACE_LISTEN, pid, NULL, NULL) != 0 ||
        expect_stop(pid, SIGTRAP | (PTRACE_EVENT_STOP << 8),
                    "PTRACE_LISTEN after the stop ended made no new event stop") ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 ||
        expect_stop(pid, SIGCONT, "the SIGCONT made no signal-delivery stop")) {
        return 1;
    }
    return 0;
}

/* PTRACE_LISTEN refused at a signal-delivery stop, then the second SIGSTOP's
 * group stop, ended during PTRACE_LISTEN, and the process let run on. */
static int listen_second(pid_t pid)
{
    if (ptrace(PTRACE_LISTEN, pid, NULL, NULL) != -1 || errno != EIO) {
        return abandon(pid, "PTRACE_LISTEN of a signal-delivery stop was taken");
    }
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 ||
        expect_stop(pid, SIGSTOP, "the second SIGSTOP made no signal-delivery stop") ||
        ptrace(PTRACE_CONT, pid, NULL, (void *)(long)SIGSTOP) != 0 ||
        expect_stop(pid, SIGSTOP | (PTRACE_EVENT_STOP << 8),
                    "the second stop was not a group stop") ||
        ptrace(PTRACE_LISTEN, pid, NULL, NULL) != 0 || kill(pid, SIGCONT) != 0 ||
        expect_stop(pid, SIGTRAP | (PTRACE_EVENT_STOP << 8),
                    "the stop's end during PTRACE_LISTEN made no new event stop") ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 ||
        expect_stop(pid, SIGCONT, "the second SIGCONT made no signal-delivery stop") ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0) {
        return 1;
    }
    return 0;
}

static int listen_and_continue(void)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        raise(SIGSTOP);
        raise(SIGSTOP);
        _exit(0);
    }
    if (waitpid(pid, &status, WUNTRACED) != pid || !WIFSTOPPED(status)) {
        return fail("the child to be listened to did not stop");
    }
#if defined __x86_64__
    if (ptrace(PTRACE_SEIZE, pid, NULL, (void *)(1UL << 32)) != -1 || errno != EINVAL) {
        return abandon(pid, "PTRACE_SEIZE accepted an upper option bit");
    }
#endif
    if (listen_first(pid) != 0 || listen_second(pid) != 0) {
        return 1;
    }
    if (wait_within(pid, &status) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return abandon(pid, "the listened-to process did not run on");
    }
    return 0;
}

/*
 * PTRACE_INTERRUPT stops a seized tracee seen asleep in a read of an empty
 * pipe, with PTRACE_EVENT_STOP and SIGTRAP, and the read, restarted when the
 * tracer resumes it, reads the byte written after.
 */
static int interrupt_read(void)
{
    char path[64];
    int p[2];
    int status;
    int i;
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
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    for (i = 0; i < 500 && stat_state(path) != 'S'; i++) {
        usleep(10000);
    }
    if (i == 500) {
        return abandon(pid, "the tracee was never seen asleep in its read");
    }
    if (ptrace(PTRACE_SEIZE, pid, NULL, NULL) != 0 ||
        ptrace(PTRACE_INTERRUPT, pid, NULL, NULL) != 0 || wait_within(pid, &status) != pid ||
        !WIFSTOPPED(status) || status >> 8 != (SIGTRAP | (PTRACE_EVENT_STOP << 8))) {
        return abandon(pid, "PTRACE_INTERRUPT did not stop a tracee blocked in a read");
    }
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || write(p[1], "g", 1) != 1) {
        return abandon(pid, "resuming the interrupted tracee");
    }
    if (wait_within(pid, &status) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return abandon(pid, "the interrupted read was not restarted");
    }
    close(p[1]);
    return 0;
}

/*
 * A seized tracee with PTRACE_O_TRACEFORK forks: its stop for the event
 * names the new child, whose first stop is PTRACE_EVENT_STOP with SIGTRAP,
 * as strace's -f waits for it, and both run on to their ends, the tracee
 * through the stop its child's SIGCHLD makes.
 */
static int seized_fork(void)
{
    unsigned long child = 0;
    int status;
    pid_t pid, grand;

    pid = fork();
    if (pid == 0) {
        raise(SIGSTOP);
        grand = fork();
        if (grand == 0) {
            _exit(0);
        }
        _exit(waitpid(grand, &status, 0) == grand && WIFEXITED(status) ? 0 : 1);
    }
    if (waitpid(pid, &status, WUNTRACED) != pid || !WIFSTOPPED(status) ||
        ptrace(PTRACE_SEIZE, pid, NULL, PTRACE_O_TRACEFORK) != 0 || kill(pid, SIGCONT) != 0) {
        return abandon(pid, "seizing a stopped process with PTRACE_O_TRACEFORK");
    }
    for (;;) {
        if (wait_within(pid, &status) != pid || !WIFSTOPPED(status)) {
            return abandon(pid, "the seized process did not stop at its fork");
        }
        if (status >> 8 == (SIGTRAP | (PTRACE_EVENT_FORK << 8))) {
            break;
        }
        ptrace(PTRACE_CONT, pid, NULL, NULL);
    }
    ptrace(PTRACE_GETEVENTMSG, pid, NULL, &child);
    if (child == 0 || wait_within((pid_t)child, &status) != (pid_t)child || !WIFSTOPPED(status) ||
        status >> 8 != (SIGTRAP | (PTRACE_EVENT_STOP << 8))) {
        return abandon(pid, "the child of a seized fork did not first stop with SIGTRAP");
    }
    if (ptrace(PTRACE_CONT, (pid_t)child, NULL, NULL) != 0 ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0) {
        return abandon(pid, "resuming the seized process and its child");
    }
    if (wait_within((pid_t)child, &status) != (pid_t)child || !WIFEXITED(status)) {
        return abandon(pid, "the child of the seized process did not run to its end");
    }
    while (wait_within(pid, &status) == pid && WIFSTOPPED(status)) {
        ptrace(PTRACE_CONT, pid, NULL, (void *)(long)WSTOPSIG(status));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return abandon(pid, "the seized process did not reap its child and end");
    }
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

/* The labels bound exactly one system call and the following instruction. */
extern const char step_call[], step_return[], step_next[], watched_ss[], watched_call[],
    watched_return[];
#if defined __x86_64__
__asm__(".text\n"
        ".globl step_call, step_return, step_next, watched_ss, watched_call, watched_return\n"
        "step_call: syscall\n"
        "step_return: incq %rbx\n"
        "step_next: int3\n"
        "watched_ss: movw (%rdi), %ss\n"
        "watched_call: syscall\n"
        "watched_return: int3\n");
static unsigned short user_ss = 0x2b;
#else
__asm__(".text\n"
        ".globl step_call, step_return, step_next, watched_ss, watched_call, watched_return\n"
        "step_call: int $0x80\n"
        "step_return: incl %ebx\n"
        "step_next: int3\n"
        "watched_ss: movw (%esi), %ss\n"
        "watched_call: int $0x80\n"
        "watched_return: int3\n");
static unsigned short user_ss = 0x23;
#endif

/** Stop a child for a row that supplies its registers. */
static pid_t register_child(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0) {
            _exit(1);
        }
        raise(SIGSTOP);
        _exit(2);
    }
    if (pid < 0 || expect_stop(pid, SIGSTOP, "register child did not stop")) {
        return -1;
    }
    return pid;
}

/** Reap a row's child after its last stop, without running injected code. */
static void finish_register_child(pid_t pid)
{
    int status;

    kill(pid, SIGKILL);
    wait_within(pid, &status);
}

/** Each interrupt of a listening tracee stops before its next instruction.
 * The chosen instruction increments BX; a late stop changes BX or IP. */
static int interrupt_listening(pid_t pid)
{
    struct user_regs_struct regs;
    int i;

    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0) {
        return abandon(pid, "registers before listening interrupt");
    }
    R_IP(regs) = (unsigned long)step_return;
    R_BX(regs) = 41;
    R_NR(regs) = -1;
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0) {
        return abandon(pid, "setting the instruction after the listening stop");
    }
    for (i = 0; i < 2; i++) {
        if (ptrace(PTRACE_LISTEN, pid, NULL, NULL) != 0 ||
            ptrace(PTRACE_INTERRUPT, pid, NULL, NULL) != 0 ||
            expect_stop(pid, SIGTRAP | (PTRACE_EVENT_STOP << 8),
                        "listening interrupt made no new event stop") ||
            ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 ||
            (unsigned long)R_IP(regs) != (unsigned long)step_return || R_BX(regs) != 41) {
            return abandon(pid, "listening interrupt ran a user instruction before stopping");
        }
    }
    finish_register_child(pid);
    return 0;
}

/** Listen at an interrupt stop outside a group stop, then interrupt again. */
static int listen_user_return(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        for (;;) {
            pause();
        }
    }
    if (pid < 0) {
        return fail("fork for listening interrupt");
    }
    if (ptrace(PTRACE_SEIZE, pid, NULL, NULL) != 0 ||
        ptrace(PTRACE_INTERRUPT, pid, NULL, NULL) != 0 ||
        expect_stop(pid, SIGTRAP | (PTRACE_EVENT_STOP << 8),
                    "initial interrupt made no event stop")) {
        return abandon(pid, "seizing the child for a listening interrupt");
    }
    return interrupt_listening(pid);
}

/** A syscall step stops at its return, before the next user instruction. */
static int step_system_call(void)
{
    struct user_regs_struct regs;
    siginfo_t info;
    pid_t pid = register_child();

    if (pid < 0) {
        return fail("fork for syscall step");
    }
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0) {
        return abandon(pid, "registers before syscall step");
    }
    R_IP(regs) = (unsigned long)step_call;
    R_RET(regs) = SYS_getpid;
    R_NR(regs) = -1;
    R_BX(regs) = 41;
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0 ||
        ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)PTRACE_O_TRACESYSGOOD) != 0 ||
        ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) != 0 ||
        expect_stop(pid, SIGTRAP, "syscall step did not trap") ||
        ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 ||
        ptrace(PTRACE_GETSIGINFO, pid, NULL, &info) != 0 ||
        (unsigned long)R_IP(regs) != (unsigned long)step_return || R_BX(regs) != 41 ||
        (unsigned long)R_RET(regs) != (unsigned long)pid || info.si_code != TRAP_TRACE) {
        return abandon(pid, "syscall step ran the next instruction or lost its result");
    }
    if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) != 0 ||
        expect_stop(pid, SIGTRAP, "step after syscall did not trap") ||
        ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 ||
        (unsigned long)R_IP(regs) != (unsigned long)step_next || R_BX(regs) != 42) {
        return abandon(pid, "step after syscall did not execute exactly one instruction");
    }
    finish_register_child(pid);
    return 0;
}

/** A syscall can use an invalid user stack because entry changes stacks. */
static int entry_system_call(void)
{
    struct user_regs_struct regs;
    pid_t pid = register_child();

    if (pid < 0) {
        return fail("fork for syscall entry");
    }
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0) {
        return abandon(pid, "registers before syscall entry");
    }
    R_IP(regs) = (unsigned long)step_call;
    R_RET(regs) = SYS_getpid;
    R_NR(regs) = -1;
    R_BX(regs) = 41;
    R_SP(regs) = 1;
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0 ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 ||
        expect_stop(pid, SIGTRAP, "syscall on an invalid user stack did not return") ||
        ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 ||
        (unsigned long)R_IP(regs) != (unsigned long)step_next + 1 || R_BX(regs) != 42 ||
        (unsigned long)R_RET(regs) != (unsigned long)pid) {
        return abandon(pid, "syscall entry lost its frame or result");
    }
    finish_register_child(pid);
    return 0;
}

/** Check a watched MOV SS followed by a syscall with an invalid user stack.
 * QEMU TCG can report the watchpoint before the syscall. That result checks
 * only the user debug entry; the injected-entry proof checks the kernel
 * entry even on a processor model that does not defer the exception. */
static int delayed_debug(void)
{
    struct user_regs_struct regs;
    siginfo_t info;
    pid_t pid = register_child();

    if (pid < 0) {
        return fail("fork for delayed debug exception");
    }
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0) {
        return abandon(pid, "registers before delayed debug exception");
    }
    R_IP(regs) = (unsigned long)watched_ss;
    R_RET(regs) = SYS_getpid;
    R_NR(regs) = -1;
    R_PTR(regs) = (unsigned long)&user_ss;
    R_SP(regs) = 1;
    if (debug_set(pid, 0, (unsigned long)&user_ss) != 0 || debug_set(pid, 7, 0x70001) != 0 ||
        ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0 ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 ||
        expect_stop(pid, SIGTRAP, "delayed debug exception did not trap") ||
        ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 ||
        ptrace(PTRACE_GETSIGINFO, pid, NULL, &info) != 0 || info.si_code != TRAP_HWBKPT) {
        return abandon(pid, "delayed debug exception lost its status");
    }
    if ((unsigned long)R_IP(regs) == (unsigned long)watched_call && R_RET(regs) == SYS_getpid) {
        printf("ptracemusl: not run: debug exception preceded system call\n");
    } else if ((unsigned long)R_IP(regs) != (unsigned long)watched_return ||
               (unsigned long)R_RET(regs) != (unsigned long)pid) {
        return abandon(pid, "delayed debug exception lost its frame or result");
    }
    finish_register_child(pid);
    return 0;
}

/** A watchpoint on a kernel copy still stops its tracee at the user return. */
static int watched_copy(void)
{
    static char byte;
    int p[2], status;
    pid_t pid;
    siginfo_t info;

    if (pipe(p) != 0 || write(p[1], "g", 1) != 1) {
        return fail("pipe for watched copy");
    }
    pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        _exit(read(p[0], &byte, 1) == 1 && byte == 'g' ? 0 : 1);
    }
    close(p[0]);
    close(p[1]);
    if (pid < 0 || expect_stop(pid, SIGSTOP, "watched copy child did not stop")) {
        return fail("fork for watched copy");
    }
    if (debug_set(pid, 0, (unsigned long)&byte) != 0 || debug_set(pid, 7, 0x10001) != 0 ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 ||
        expect_stop(pid, SIGTRAP, "kernel copy did not trap") ||
        ptrace(PTRACE_GETSIGINFO, pid, NULL, &info) != 0 || info.si_code != TRAP_HWBKPT ||
        debug_set(pid, 7, 0) != 0 || ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 ||
        wait_within(pid, &status) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return abandon(pid, "watched kernel copy lost its trap or result");
    }
    return 0;
}

/** A kernel read of watched memory interrupts an otherwise endless poll.
 * The negative descriptor never becomes ready, and there is no timeout or
 * other signal. The stop must report the watchpoint and the EINTR result. */
static int watched_poll(void)
{
    static struct pollfd pfd = {-1, POLLIN, 0};
    struct user_regs_struct regs;
    siginfo_t info;
    int status;
    pid_t pid = fork();

    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        _exit(syscall(SYS_poll, &pfd, 1, -1) == -1 && errno == EINTR ? 0 : 1);
    }
    if (pid < 0 || expect_stop(pid, SIGSTOP, "watched poll child did not stop")) {
        return fail("fork for watched poll");
    }
    if (debug_set(pid, 0, (unsigned long)&pfd.fd) != 0 || debug_set(pid, 7, 0xf0001) != 0 ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 ||
        expect_stop(pid, SIGTRAP, "watched infinite poll did not trap") ||
        ptrace(PTRACE_GETSIGINFO, pid, NULL, &info) != 0 || info.si_code != TRAP_HWBKPT ||
        ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 || (long)R_RET(regs) != -EINTR) {
        return abandon(pid, "watched poll lost its trap or EINTR result");
    }
    if (debug_set(pid, 7, 0) != 0 || ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 ||
        wait_within(pid, &status) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return abandon(pid, "watched poll did not return EINTR");
    }
    return 0;
}

/** Run the ordinary suite, or one debug row for a compiled fault proof. */
/* The rows a suite may run alone, by the name it passes. */
static const struct {
    const char *name;
    int (*row)(void);
} named_rows[] = {
    {"step", step_system_call}, {"entry", entry_system_call}, {"delayed", delayed_debug},
    {"copy", watched_copy},     {"poll", watched_poll},       {"listen", listen_user_return},
};

/* The rows of memory, system calls and attaching; 0, or 1. */
static int first_rows(void)
{
    return memory_and_registers() || mem_after_end() || syscalls_and_exec() || syscall_changes() ||
           killed_at_entry() || restart_cancelled() || attach() || attach_stopped() ||
           listen_and_continue() || interrupt_read();
}

/* The rows of forks, threads, killed tracees and the debug exception; 0, or
 * 1. */
static int second_rows(void)
{
    return seized_fork() || stopped_leader() || traced_thread_usage() || tracer_first() ||
           killed_tracee() || step_system_call() || entry_system_call() || delayed_debug() ||
           watched_copy() || watched_poll() || listen_user_return();
}

int main(int argc, char **argv)
{
    unsigned int i;

    if (argc == 2) {
        for (i = 0; i < sizeof(named_rows) / sizeof(named_rows[0]); i++) {
            if (strcmp(argv[1], named_rows[i].name) == 0) {
                if (named_rows[i].row() != 0) {
                    return 1;
                }
                printf("PTRACEMUSL OK\n");
                return 0;
            }
        }
        return fail("unknown row");
    }
    if (argc != 1) {
        return fail("unexpected argument count");
    }
    if (first_rows() || second_rows()) {
        return 1;
    }
    printf("PTRACEMUSL OK\n");
    return 0;
}
