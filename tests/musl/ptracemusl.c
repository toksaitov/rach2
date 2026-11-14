/**
 * ptracemusl: check ptrace as GDB and strace drive it.
 *
 * A child that calls PTRACE_TRACEME and stops itself is reported stopped to its
 * parent. The parent reads and changes a word of the child's memory with
 * PEEKDATA and POKEDATA, and through /proc/PID/mem, where a read and a write
 * that run from a page into a hole do the bytes before the hole. It reads the
 * registers with GETREGS and the floating-point state with GETFPXREGS,
 * GETFPREGS on x86-64; steps one instruction with SINGLESTEP, which moves the
 * instruction pointer; plants a breakpoint with POKETEXT on a read-only text
 * page, continues to it, finds the child stopped by SIGTRAP just past it, puts
 * the text back, moves the instruction pointer back with SETREGS and continues.
 * A /proc/PID/mem descriptor keeps none of the memory of a child that has
 * ended: once a child that touched 16 MiB has ended, and before it is reaped, a
 * read through a descriptor of its memory returns 0 and the free memory
 * /proc/meminfo reports is back.
 *
 * PTRACE_SYSCALL stops the child at the entry and the exit of getpid, with the
 * call's number in orig_eax and its result in eax at the exit (orig_rax and rax
 * on x86-64), and bit 0x80 on the stop signal with PTRACE_O_TRACESYSGOOD. At an
 * entry stop the tracer changes the call through orig_eax: getpid made getppid
 * returns the parent's id, and a write made -1 is not run and returns the eax
 * the tracer set; on x86-64 a DS selector the tracer sets there is the
 * child's at the next stop, and the kernel's data selector is refused. A
 * tracee that SIGKILL reaches at the entry stop of a write writes nothing. The
 * exit stop of a read a signal interrupted has -ERESTARTSYS, -512, in eax, as
 * Linux's, whose exit stop comes before signal delivery; -1 in orig_eax there
 * keeps the read from being restarted after a handler with SA_RESTART, and the
 * read returns the code the tracer left, errno 512.
 * PTRACE_O_TRACEEXEC stops the child after an execve with the exec event, which
 * waitid reports in si_status above the signal, and a /proc/PID/mem descriptor
 * opened before the execve reads nothing of the new image. A debug register set
 * through POKEUSER reads back through PEEKUSER, and DR7 enables a breakpoint on
 * a user address, the last of which a breakpoint may end at, and fails with
 * EINVAL for one below user space. PTRACE_ATTACH stops a running process, which
 * runs on after PTRACE_DETACH, and PTRACE_SEIZE stops for the tracer one that a
 * stop signal has already stopped, which stays stopped after PTRACE_DETACH
 * until SIGCONT. PTRACE_LISTEN keeps a seized tracee in the PTRACE_EVENT_STOP
 * of its group stop until the stop ends, whether the SIGCONT comes before the
 * listen or after it, and the end makes a new PTRACE_EVENT_STOP with SIGTRAP;
 * at a signal-delivery stop it fails with EIO. PTRACE_INTERRUPT stops a seized
 * tracee blocked in a read, which is restarted after. The child a seized tracee
 * forks under PTRACE_O_TRACEFORK first stops with PTRACE_EVENT_STOP and
 * SIGTRAP. A signal sent to a process whose first thread its tracer holds
 * stopped wakes the other thread, asleep in pause, which takes it. The usage a
 * tracer's wait4 reports for the end of a traced thread that is not the first
 * has the time of the children its process reaped. A process another process
 * traces, which ends, is reported to its tracer before its parent may reap it,
 * and then to the parent; the tracer's next wait finds nothing left to wait
 * for. A tracee that SIGKILL ends sends its tracer, here its parent, a SIGCHLD
 * of CLD_KILLED with the signal. A tracer that moves a stopped tracee's thread
 * pointer, by PTRACE_ARCH_PRCTL on x86-64 and PTRACE_SET_THREAD_AREA on i386,
 * reads it back, and the tracee runs with it; on x86-64 a base past user space
 * fails with EPERM or EIO.
 *
 * The rows of the debug exception: a single step over a system call instruction
 * stops after it with the call's result; a system call made on an invalid user
 * stack returns; and a watchpoint that the kernel's access to user memory hits,
 * in a read, a poll or a sigtimedwait, stops nothing and posts nothing, as on
 * Linux, while the tracee's own write of the word after it still traps.
 * PTRACE_INTERRUPT of a tracee in PTRACE_LISTEN stops it before its next
 * instruction. A SIGCONT with no group stop to end lets a seized tracee out
 * of PTRACE_LISTEN at an interrupt stop into a new PTRACE_EVENT_STOP. A
 * thread its tracer resumed during its process's stop, asleep in a pipe
 * read, takes the stop when the tracer ends without PTRACE_O_EXITKILL.
 *
 * The program takes no positional argument and runs every row. It prints
 * "PTRACEMUSL OK" and exits 0 when the rows pass, or prints
 * "PTRACEMUSL FAIL (detail: errno N)" and exits 1 at the first failure.
 *
 * Given `machine=native`
 * (tests/native/Readme.md), the row of a /proc/PID/mem descriptor leaves out
 * its comparison of MemFree, since the machine's services move free memory
 * meanwhile.
 *
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each row in order, a setup step joined to the row after it:
 * `traced-stop`, `peekdata`, `pokedata`, `proc-mem-read`, `proc-mem-hole`,
 * `getregs`, `fpregs`, `singlestep`, `breakpoint-reached`, `breakpoint-ip`,
 * `breakpoint-restore`, `poked-word-read`, `ended-mem-live-read`,
 * `ended-mem-child-exits`, `ended-meminfo`, `ended-mem-read-zero`,
 * `ended-mem-freed`, `setoptions`, `debugreg-peekuser`, `dr7-kernel-einval`,
 * `dr7-last-user`, `dr7-bit12-einval`, `getpid-entry`, `getpid-exit`,
 * `exec-waitid`, `exec-event`, `exec-mem-empty`, `exec-runs`,
 * `changes-getpid-entry`, `changes-write-entry`, `changes-exit`,
 * `changes-getppid`, `changes-write-result`, `changes-write-not-run`,
 * `kill-entry-stop`, `kill-entry-sigkill`, `kill-entry-no-write`,
 * `restart-read-entry`, `restart-read-eintr`, `restart-signal`,
 * `restart-cancelled`, `attach-stops`, `detach`, `detach-runs-on`,
 * `seize-stopped-event`, `seize-detach`, `seize-stays-stopped`,
 * `seize-sigcont-runs`, `listen-group-stop`, `listen-interrupt`,
 * `listen-cont-before`, `listen-sigcont-stop`, `listen-delivery-eio`,
 * `listen-second-sigstop`, `listen-second-group-stop`, `listen-cont-during`,
 * `listen-second-sigcont`, `listen-runs-on`, `interrupt-stops`,
 * `interrupt-read-restarted`, `fork-event`, `fork-child-event-stop`,
 * `fork-child-ends`, `fork-parent-ends`, `leader-signal-wakes`,
 * `leader-signal-taken`, `thread-attach`, `thread-usage`, `thread-child-ends`,
 * `tracer-seize`, `tracer-first`, `tracer-then-parent`, `tracer-sees-end`,
 * `tracer-echild`, `killed-sigchld`, `killed-sigchld-info`,
 * `killed-wait-status`, `step-syscall-trap`, `step-syscall-result`,
 * `step-next-trap`, `step-next-one`, `entry-bad-stack-returns`,
 * `entry-bad-stack-result`, `copy-watch-no-stop`, `copy-watch-user-trap`,
 * `copy-watch-result`, `poll-watch-no-stop`, `poll-watch-returns`,
 * `sigwait-watch`, `interrupt-pause-stop`,
 * `listen-interrupt-before-instruction`, `listen-sigcont-event-stop`,
 * `listen-sigcont-delivery`, `tracer-gone-resumed`, `tracer-gone-group-stop`,
 * `tracer-gone-runs-on`, `tp-set-readback` and
 * `tp-tracee-runs`. x86-64 adds `dr7-bit32-einval`,
 * `setoptions-wide-einval`, `ds-kernel-eio`, `ds-kept`, `seize-wide-einval`,
 * `tp-arch-get-fs`, `tp-peekuser-fs`, `tp-arch-prctl-errors`,
 * `tp-arch-set-eperm`, `tp-pokeuser-eio`, `tp-setregs-eio`, `tp-setregset-eio`
 * and `tp-refused-kept`; i386 adds `tp-get-thread-area`, `tp-entries-unused`,
 * `tp-entries-write-back`, `tp-entries-no-base` and
 * `tp-entries-outside-einval`. The branch of the group `arch`, `i686` or
 * `x86-64`, says which ran. A native run reports `ended-mem-freed` not run. The
 * first failure fails its assertion and leaves the rest not run, though
 * `ended-mem-read-zero` and `ended-mem-freed` are both reported.
 */

#define _GNU_SOURCE
#include <elf.h>
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
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

/*
 * The fields of each architecture's user_regs_struct the rows read: the
 * call's number, its result, the instruction pointer and the code segment,
 * which must be the selector the tracer runs with (`running_cs`). The
 * floating-point
 * state is the fxsave image, which i386 reads with GETFPXREGS and x86-64 with
 * GETFPREGS. LAST_USER_WORD is the last four bytes of user space: below the
 * kernel at 3 GiB on i386, and below the last page of the lower half on
 * x86-64, which user space does not reach.
 */
#if defined __x86_64__
#define R_BX(r)        ((r).rbx)
#define R_SP(r)        ((r).rsp)
#define R_NR(r)        ((r).orig_rax)
#define R_RET(r)       ((r).rax)
#define R_IP(r)        ((r).rip)
#define R_CS(r)        ((r).cs)
#define PTRACE_GETFX   PTRACE_GETFPREGS
#define LAST_USER_WORD 0x00007FFFFFFFEFFCUL
typedef struct user_fpregs_struct fxregs_t;
#else
#define R_BX(r)        ((r).ebx)
#define R_SP(r)        ((r).esp)
#define R_NR(r)        ((r).orig_eax)
#define R_RET(r)       ((r).eax)
#define R_IP(r)        ((r).eip)
#define R_CS(r)        ((r).xcs)
#define PTRACE_GETFX   PTRACE_GETFPXREGS
#define LAST_USER_WORD 0xbffffffcUL
typedef struct user_fpxregs_struct fxregs_t;
#endif

/**
 * The code selector the caller runs with, which its forked tracee has too.
 * An i386 program's depends on the kernel: 0x1b on Rach2's i686 kernel and
 * 0x23, Linux's __USER32_CS, on an x86-64 kernel.
 */
static unsigned long running_cs(void)
{
    unsigned short cs;

    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

/* The word the parent reads and changes in its traced child. */
static volatile long target = 1234;

/* The assertions of etude's report in the order they are checked, more than
 * probe_report.h's path holds, and how many are planned and reported. A run
 * of one named row plans none. */
static const char *rows[112];
static int rows_planned, rows_reported;

/** Add the assertion `id` to the end of the planned rows. */
static void plan_row(const char *id)
{
    rows[rows_planned++] = id;
}

/** Report that the next planned row held, when one is left. */
static void held(void)
{
    if (rows_reported < rows_planned) {
        report_pass(rows[rows_reported++]);
    }
}

/** Report the planned rows before the one at index `row` held. */
static void held_to(int row)
{
    while (rows_reported < row && rows_reported < rows_planned) {
        held();
    }
}

/** Report that the next planned row held, and return 0, for a chain of
 * checks joined by `||`. */
static int passed(void)
{
    held();
    return 0;
}

/** Return 1 in the process that began the report before its end, and 0
 * elsewhere. */
static int reporting(void)
{
    return report_owner != 0 && !report_ended && report_getpid() == report_owner;
}

/** Report that the next planned row failed with `why`. */
static void missed(const char *why)
{
    if (reporting() && rows_reported < rows_planned) {
        report_fail(rows[rows_reported++], why);
    }
}

/** Report the rows not yet reported not run, and the report's end. */
static void stop(void)
{
    if (!reporting()) {
        return;
    }
    while (rows_reported < rows_planned) {
        report_notrun(rows[rows_reported++], "an earlier check failed");
    }
    report_end();
}

/** Print one failed check with errno, report it on the next planned row and
 * return 1, the failure status. */
static int miss(const char *what)
{
    char why[200];

    snprintf(why, sizeof(why), "%s: errno %d", what, errno);
    printf("PTRACEMUSL FAIL (%s)\n", why);
    missed(why);
    return 1;
}

/** Report one failed check with errno, and the rows after it not run, and
 * return 1, the failure status. */
static int fail(const char *what)
{
    miss(what);
    stop();
    return 1;
}

/* The function the breakpoint goes on; noinline, so it has an address. */
__attribute__((noinline)) static int marked(int x)
{
    return x * 3;
}

/** Wait for `pid` and return 1 when it is stopped by `sig`, and 0 otherwise. */
static int stopped_by(pid_t pid, int sig)
{
    int status;

    return waitpid(pid, &status, 0) == pid && WIFSTOPPED(status) && WSTOPSIG(status) == sig;
}

/**
 * Check a write and a read of 256 bytes through `path`, /proc/PID/mem of a
 * child that has the page `page` and a hole after it, from 128 bytes before the
 * hole: each does the 128 before the hole. Returns 0, or 1 after the failure is
 * printed.
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

/**
 * Check PEEKDATA, POKEDATA, /proc/PID/mem and the rows of `into_hole`,
 * GETREGS, the floating-point state, SINGLESTEP and a breakpoint planted with
 * POKETEXT, in a child that ends with 0 only when it reads the word the parent
 * wrote. Returns 0, or 1 after the failure is printed.
 */
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
    held();
    errno = 0;
    word = ptrace(PTRACE_PEEKDATA, pid, (void *)&target, NULL);
    if (errno != 0 || word != 1234) {
        return fail("PEEKDATA");
    }
    held();
    if (ptrace(PTRACE_POKEDATA, pid, (void *)&target, (void *)5678L) != 0) {
        return fail("POKEDATA");
    }
    held();
    snprintf(path, sizeof(path), "/proc/%d/mem", (int)pid);
    fd = open(path, O_RDONLY);
    if (fd < 0 || pread(fd, &word, sizeof(int), (off_t)(uintptr_t)&target) != sizeof(int) ||
        (int)word != 5678) {
        return fail("/proc/PID/mem");
    }
    held();
    close(fd);
    if (into_hole(path, page) != 0) {
        return 1;
    }
    held();
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 || R_CS(regs) != running_cs()) {
        return fail("GETREGS");
    }
    held();
    if (ptrace(PTRACE_GETFX, pid, NULL, &fpx) != 0 || fpx.cwd == 0) {
        return fail("GETFPXREGS");
    }
    held();
    if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGTRAP) ||
        ptrace(PTRACE_GETREGS, pid, NULL, &after) != 0 || R_IP(after) == R_IP(regs)) {
        return fail("SINGLESTEP");
    }
    held();

    {
        long text = ptrace(PTRACE_PEEKTEXT, pid, (void *)marked, NULL);
        long trap = (text & ~0xFFL) | 0xCC;

        if (ptrace(PTRACE_POKETEXT, pid, (void *)marked, (void *)trap) != 0 ||
            ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGTRAP)) {
            return fail("the breakpoint was not reached");
        }
        held();
        if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 ||
            (uintptr_t)R_IP(regs) != (uintptr_t)marked + 1) {
            return fail("the stop was not just past the breakpoint");
        }
        held();
        R_IP(regs) = (uintptr_t)marked;
        if (ptrace(PTRACE_POKETEXT, pid, (void *)marked, (void *)text) != 0 ||
            ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0 ||
            ptrace(PTRACE_CONT, pid, NULL, NULL) != 0) {
            return fail("putting the text back");
        }
        held();
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the traced child did not finish with the word the parent wrote");
    }
    held();
    return 0;
}

/**
 * Wait for the exec event stop of `pid`, which waitid reports with the event
 * above the signal in si_status and waitpid with it in the status.
 * Returns 0, or 1 after the failure is printed.
 */
static int exec_event(pid_t pid)
{
    siginfo_t si;
    int status;

    if (waitid(P_PID, (id_t)pid, &si, WSTOPPED | WNOWAIT) != 0 || si.si_code != CLD_TRAPPED ||
        si.si_status != (SIGTRAP | (PTRACE_EVENT_EXEC << 8))) {
        return fail("waitid's si_status of the exec event");
    }
    held();
    if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status) ||
        status >> 8 != (SIGTRAP | (PTRACE_EVENT_EXEC << 8))) {
        return fail("the exec event");
    }
    held();
    return 0;
}

/**
 * Set debug register `n` of the stopped `pid` to `v`, and return ptrace's
 * result. The registers are words of struct user, 4 bytes on i386 and 8 on
 * x86-64.
 */
static long debug_set(pid_t pid, int n, unsigned long v)
{
    struct user u;

    return ptrace(PTRACE_POKEUSER, pid,
                  (void *)(offsetof(struct user, u_debugreg) + sizeof(u.u_debugreg[0]) * n),
                  (void *)v);
}

/**
 * Check that DR7 enables a breakpoint only on user addresses: an execute
 * breakpoint at 0x1000 in DR0, which is below user space, fails with EINVAL,
 * and a 4-byte write breakpoint whose last byte is the last user address is
 * taken; DR7 is then cleared again. DR7 with reserved bit 12, or on x86-64 bit
 * 32, fails with EINVAL; the processor raises a general-protection fault when a
 * context switch loads DR7 with bit 32 set. Returns 0, or 1 after the failure
 * is printed.
 */
static int debug_bounds(pid_t pid)
{
    if (debug_set(pid, 0, 0x1000) != 0 || debug_set(pid, 7, 0x1) != -1 || errno != EINVAL) {
        return fail("an execute breakpoint on a kernel address was not refused");
    }
    held();
    if (debug_set(pid, 0, LAST_USER_WORD) != 0 ||
        debug_set(pid, 7, 0x1 | 0x1UL << 16 | 0x3UL << 18) != 0 || debug_set(pid, 7, 0) != 0) {
        return fail("a breakpoint ending at the last user address was refused");
    }
    held();
    if (debug_set(pid, 7, 0x1000) != -1 || errno != EINVAL) {
        return fail("DR7 with reserved bit 12 was not refused");
    }
    held();
#if defined __x86_64__
    if (debug_set(pid, 7, 0x1UL << 32) != -1 || errno != EINVAL) {
        return fail("DR7 with reserved bit 32 was not refused");
    }
    held();
#endif
    return 0;
}

/**
 * Check the stopped `pid`'s debug registers through POKEUSER and PEEKUSER, the
 * bounds of `debug_bounds`, and on x86-64 that PTRACE_SETOPTIONS fails with
 * EINVAL for an option bit past 32. Returns 0, or 1 after the failure is
 * printed.
 */
static int debug_and_options(pid_t pid)
{
    unsigned long dr = 0x1000;

    if (ptrace(PTRACE_POKEUSER, pid, (void *)offsetof(struct user, u_debugreg[0]), (void *)dr) !=
            0 ||
        ptrace(PTRACE_PEEKUSER, pid, (void *)offsetof(struct user, u_debugreg[0]), NULL) !=
            0x1000) {
        return fail("a debug register through POKEUSER and PEEKUSER");
    }
    held();
    if (debug_bounds(pid) != 0) {
        return 1;
    }
#if defined __x86_64__
    if (ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(1UL << 32)) != -1 || errno != EINVAL) {
        return fail("PTRACE_SETOPTIONS accepted an upper option bit");
    }
    held();
#endif
    return 0;
}

/**
 * Run `pid` through syscall stops to getpid's entry, then to its exit with
 * the pid as its result. Returns 0, or 1 after the failure is printed.
 */
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
    held();
    if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGTRAP | 0x80) ||
        ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 || R_NR(regs) != SYS_getpid ||
        (long)R_RET(regs) != pid) {
        return fail("the exit stop of getpid");
    }
    held();
    return 0;
}

/**
 * Check, in a child that calls getpid and then runs `sh -c "exit 6"`, the
 * rows of `debug_and_options` and `getpid_stops`, the exec event of
 * PTRACE_O_TRACEEXEC, and that a /proc/PID/mem descriptor opened before the
 * exec reads nothing after it. Returns 0, or 1 after the failure is printed.
 */
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
    held();
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
    held();
    close(mem);
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 6) {
        return fail("the exec'd program");
    }
    held();
    return 0;
}

/**
 * Run the stopped `pid` through syscall stops to the first whose orig_eax is
 * `nr`, which is that call's entry, and store its registers in `*regs`.
 * Returns 0, or -1.
 */
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

/**
 * Check that at a system call's entry stop the tracer may change the call
 * through orig_eax: getpid made getppid returns the parent's id, and a write
 * made -1 is not run and returns what the tracer left in eax, so the pipe
 * stays empty. On x86-64 SETREGS fails with EIO for the kernel's data
 * selector in DS, and takes the user data selector, which DS still has at
 * the next stop. Returns 0, or 1 after the failure is printed.
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
    held();
#if defined __x86_64__
    regs.ds = 0x18;
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != -1 || errno != EIO) {
        return fail("the kernel's data selector was taken for DS");
    }
    held();
    regs.ds = 0x2b;
#endif
    R_NR(regs) = SYS_getppid;
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0 ||
        syscall_entry(pid, SYS_write, &regs) != 0) {
        return fail("the entry stop of write");
    }
    held();
#if defined __x86_64__
    if (regs.ds != 0x2b) {
        return fail("a DS selector set by SETREGS was not kept");
    }
    held();
#endif
    R_NR(regs) = -1;
    R_RET(regs) = 42;
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0 ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status)) {
        return fail("the child whose calls were changed did not exit");
    }
    held();
    if (WEXITSTATUS(status) != 0) {
        if (WEXITSTATUS(status) != 2) {
            held();
        }
        return fail(WEXITSTATUS(status) == 2 ? "getpid changed to getppid ran as getpid"
                                             : "a write changed to -1 did not return 42");
    }
    held();
    held();
    if (read(p[0], &c, 1) != 0) {
        return fail("a write changed to -1 was run");
    }
    held();
    close(p[0]);
    return 0;
}

/**
 * Do nothing, as the handler of SIGUSR1, so that the signal only interrupts
 * the call it comes during.
 */
static void interrupt_only(int sig)
{
    (void)sig;
}

/* Linux's restart code for a call a handler with SA_RESTART restarts, which
 * a tracer reads at the exit stop (include/linux/errno.h); no user header
 * defines it. */
#define ERESTARTSYS 512

/**
 * Check that a tracer reads -ERESTARTSYS at the exit stop of a read a signal
 * interrupted, as on Linux 6.12, whose exit stop (kernel/entry/common.c:176)
 * comes before the signal's delivery decides the restart, and that its -1 in
 * orig_eax there keeps the read from being restarted, though the handler that
 * runs has SA_RESTART: Linux's handle_signal then leaves the result register
 * alone (arch/x86/kernel/signal.c:264), so the read returns -1 with errno
 * ERESTARTSYS, and a byte written after it is not read. Returns 0, or 1 after
 * the failure is printed.
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
        _exit(read(p[0], &c, 1) == -1 && errno == ERESTARTSYS ? 0 : 1);
    }
    if (!stopped_by(pid, SIGSTOP) || syscall_entry(pid, SYS_read, &regs) != 0) {
        return fail("the entry stop of read");
    }
    held();
    if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) != 0 || kill(pid, SIGUSR1) != 0 ||
        !stopped_by(pid, SIGTRAP) || ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 ||
        R_NR(regs) != SYS_read || (long)R_RET(regs) != -ERESTARTSYS) {
        return fail("the exit stop of the interrupted read");
    }
    held();
    R_NR(regs) = -1;
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0 ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGUSR1) ||
        ptrace(PTRACE_CONT, pid, NULL, (void *)SIGUSR1) != 0 || write(p[1], "x", 1) != 1) {
        return fail("the signal of the interrupted read");
    }
    held();
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("a read whose restart the tracer cancelled was restarted");
    }
    held();
    close(p[0]);
    close(p[1]);
    return 0;
}

/**
 * Check that PTRACE_ATTACH stops a process blocked in a read, and that it reads
 * the byte written after PTRACE_DETACH. Returns 0, or 1 after the failure is
 * printed.
 */
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
    held();
    if (ptrace(PTRACE_DETACH, pid, NULL, NULL) != 0 || write(p[1], "g", 1) != 1) {
        return fail("PTRACE_DETACH");
    }
    held();
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the process did not run on after the detach");
    }
    held();
    close(p[1]);
    return 0;
}

/**
 * Kill and reap the child `pid` of a row that failed, so that nothing of the
 * row runs on, and report `what`. Returns 1.
 */
static int abandon(pid_t pid, const char *what)
{
    kill(pid, SIGKILL);
    waitpid(pid, NULL, __WALL);
    return fail(what);
}

/** Return MemFree of /proc/meminfo, in KiB, or -1 when it cannot be read. */
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

/**
 * Check that a /proc/PID/mem descriptor keeps none of the memory of a child
 * that has ended. A child touches 16 MiB and waits; the parent opens its
 * /proc/PID/mem, reads a word of it, and lets it end, which waitid with WNOWAIT
 * reports and leaves the child unreaped. Before the reap, with the descriptor
 * still open, a read through it must return 0 and the free memory /proc/meminfo
 * reports must be back within 8 MiB of what it was before the fork: the child's
 * address space went at its end, as Linux's exit_mm drops it, and the
 * descriptor keeps only its record. Both of the last two checks are reported,
 * as both may fail. Returns 0, or 1 after the failure is printed.
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
    held();
    if (waitid(P_PID, (id_t)pid, &si, WEXITED | WNOWAIT) != 0 || si.si_code != CLD_EXITED ||
        si.si_status != 0) {
        return abandon(pid, "the child that touched 16 MiB");
    }
    held();
    got = pread(fd, &word, sizeof(word), (off_t)(uintptr_t)&target);
    after = mem_free_kib();
    close(fd);
    close(ready[0]);
    close(go[1]);
    waitpid(pid, NULL, 0);
    if (before < 0 || after < 0) {
        return fail("/proc/meminfo");
    }
    held();
    if (got != 0) {
        rc = miss("a /proc/PID/mem read reached a child that had ended");
    } else {
        held();
    }
    if (!probe_native() && after < before - 8192) {
        rc = miss("a /proc/PID/mem descriptor kept the memory of a child that ended");
    } else if (!probe_native()) {
        held();
    }
    if (rc != 0) {
        stop();
    }
    return rc;
}

/* Set to 1 by on_usr1 when the child's handler has taken a SIGUSR1. */
static volatile sig_atomic_t usr1_taken;

/** Record a SIGUSR1 in `usr1_taken`. */
static void on_usr1(int sig)
{
    (void)sig;
    usr1_taken = 1;
}

/**
 * Run as the second thread of the child `stopped_leader` makes: write the
 * thread's id to the descriptor `*arg`, sleep until the handler has taken a
 * SIGUSR1, and end the process with 5, or with 1 when the write fails.
 */
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

/**
 * Return the state letter the stat file at `path` gives, or 0 when it cannot
 * be read.
 */
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

/**
 * Return 1 when thread `t` of process `pid` is asleep, state S in
 * /proc/PID/task/T/stat, and 0 otherwise.
 */
static int sleeping(pid_t pid, pid_t t)
{
    char path[64];

    snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", (int)pid, (int)t);
    return stat_state(path) == 'S';
}

/**
 * Wait for process `pid` to reach the state `want` in /proc/PID/stat,
 * looking every 10 ms. Returns the state the wait ends at: `want`; Z when the
 * process ends first; or 0 when its stat file is gone.
 */
static char await_state(pid_t pid, char want)
{
    char path[64];
    char state;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    for (;;) {
        state = stat_state(path);
        if (state == want || state == 'Z' || state == 0) {
            return state;
        }
        /* interval: a process's state is looked at again every 10 ms */
        usleep(10000);
    }
}

/**
 * Check a SIGUSR1 sent to a child whose first thread the parent has stopped
 * with PTRACE_ATTACH: the thread the signal names cannot take it, so the other
 * thread, asleep in pause with the signal unblocked, is woken to, and its
 * handler runs and it ends the process with 5. Returns 0, or 1 after the
 * failure is printed.
 */
static int stopped_leader(void)
{
    pthread_t t;
    pid_t pid, sleeper = 0;
    int p[2], status = 0;

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
    while (!sleeping(pid, sleeper)) {
        /* interval: the second thread's state is looked at again every 10 ms */
        usleep(10000);
    }
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0 || !stopped_by(pid, SIGSTOP)) {
        return abandon(pid, "PTRACE_ATTACH to the child's first thread");
    }
    kill(pid, SIGUSR1);
    if (waitpid(pid, &status, 0) != pid) {
        return abandon(pid, "the child of a process signal beside a stopped thread");
    }
    held();
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 5) {
        return fail("the thread beside the stopped one did not take the signal");
    }
    held();
    return 0;
}

/*
 * The kernel's struct rusage, which the raw wait4 writes: two timevals of a
 * long's seconds and microseconds, then fourteen counters, each a long of the
 * architecture's width.
 */
struct rusage32 {
    long utime_sec;
    long utime_usec;
    long stime_sec;
    long stime_usec;
    long counters[14];
};

/* The pipes on which usage_thread reports to the parent and waits for it. */
static int usage_pipe[2], go_pipe[2];

/**
 * Spend user time until the kernel has charged `ticks` of it to the process.
 * Returns the sum the loop computed, so that the compiler keeps the loop.
 */
static unsigned int spin_ticks(long ticks)
{
    volatile unsigned int x = 0;
    struct tms tm;
    unsigned int i;

    times(&tm);
    while ((long)tm.tms_utime < ticks) {
        for (i = 0; i < 200000u; i++) {
            x += i;
        }
        times(&tm);
    }
    return x;
}

/**
 * Run as the second thread of the child `traced_thread_usage` makes: write
 * the thread's id and its process's reaped children's user time to
 * `usage_pipe`, then end when a byte comes on `go_pipe`. Returns null, or 1
 * when the write or the read fails.
 */
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
 * Check the usage reported for the end of a traced thread. A child reaps a
 * grandchild that spent user time, then starts a second thread, which the
 * parent attaches to and lets end. The tracer's wait4 implicitly selects every
 * tracee, with ordinary flags or __WCLONE, and reports the
 * thread's end with at least the reaped grandchild's time, which the process
 * keeps, not the thread. Returns 0, or 1 after the failure is printed.
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
        waitpid((pid_t)got[0], &status, 0) != (pid_t)got[0] || !WIFSTOPPED(status) ||
        ptrace(PTRACE_CONT, (pid_t)got[0], NULL, NULL) != 0 || write(go_pipe[1], "g", 1) != 1) {
        return abandon(pid, "attaching to the child's second thread");
    }
    held();
    memset(&ru, 0xFF, sizeof(ru));
    if (syscall(SYS_wait4, got[0], &status, __WCLONE, &ru) != got[0] || !WIFEXITED(status) ||
        ru.utime_sec * 100 + ru.utime_usec / 10000 < got[1]) {
        return abandon(pid, "a traced thread's end lacks the reaped children's time");
    }
    held();
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the child with the traced thread");
    }
    held();
    return 0;
}

/**
 * Run as the tracer of `tracer_first`: seize the child `c`, which is not the
 * caller's own, tell the parent through `to_m`, wait for the byte `from_m`
 * brings, then wait for the child's end, which must have status 9, and wait
 * again, which must find nothing. Returns 0, or the number of the step that
 * failed.
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
 * Check that a traced process's end is reported to its tracer first. A
 * child, which a second child traces, ends with 9. Its parent's wait does not
 * reap it while the tracer has not seen the end; once the tracer's wait has
 * reported it, the parent reaps it with its status, and the tracer's next
 * wait answers ECHILD rather than waiting on a tracee that is gone. The
 * tracer's exit status says which step failed: 4 is the wait after the end.
 * Returns 0, or 1 after the failure is printed.
 */
static int tracer_first(void)
{
    int to_c[2], to_m[2], to_x[2], status = 0;
    pid_t c, x, got = 0;
    char why[64];
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
    held();
    if (await_state(c, 'Z') != 'Z') {
        kill(x, SIGKILL);
        waitpid(x, &status, 0);
        return abandon(c, "the first child never became a zombie");
    }
    if (waitpid(c, &status, WNOHANG) != 0 || write(to_x[1], "g", 1) != 1) {
        kill(x, SIGKILL);
        return fail("the parent reaped a child before its tracer saw its end");
    }
    held();
    got = waitpid(c, &status, 0);
    if (got != c || !WIFEXITED(status) || WEXITSTATUS(status) != 9) {
        kill(x, SIGKILL);
        return fail("the parent did not reap the child once its tracer saw the end");
    }
    held();
    got = waitpid(x, &status, 0);
    close(to_c[1]);
    close(to_m[0]);
    close(to_x[1]);
    if (got != x) {
        return abandon(x, "the tracer could not be reaped");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(why, sizeof(why), "the tracer's step %d", WEXITSTATUS(status));
        printf("PTRACEMUSL FAIL (%s)\n", why);
        if (WEXITSTATUS(status) == 4) {
            held();
        }
        missed(why);
        stop();
        return 1;
    }
    held();
    held();
    return 0;
}

/**
 * Check that a tracee that SIGKILL reaches at the entry stop of a write to a
 * pipe writes nothing, since the call does not run: the pipe then reads its
 * end of input. Returns 0, or 1 after the failure is printed.
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
    held();
    kill(pid, SIGKILL);
    if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        return fail("the tracee killed at an entry stop did not end by SIGKILL");
    }
    held();
    if (read(p[0], &c, 1) != 0) {
        return fail("a write the tracee was killed at the entry of ran");
    }
    held();
    close(p[0]);
    return 0;
}

/**
 * Check that PTRACE_SEIZE of a process a stop signal has stopped, whose stop
 * its parent has already waited for, stops the thread for the tracer with
 * PTRACE_EVENT_STOP and the stop signal, that the process stays stopped after
 * PTRACE_DETACH, its state in /proc going from the tracer's stop to the stop
 * of its process (T) and not to its end, and that it runs on after SIGCONT.
 * Returns 0, or 1 after the failure is printed.
 */
static int attach_stopped(void)
{
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
        return abandon(pid, "seizing a stopped process did not stop it for the tracer");
    }
    held();
    if (ptrace(PTRACE_DETACH, pid, NULL, NULL) != 0) {
        return abandon(pid, "PTRACE_DETACH of the seized process");
    }
    held();
    if (await_state(pid, 'T') != 'T' || waitpid(pid, &status, WNOHANG) != 0) {
        return abandon(pid, "the detached process ran while its stop was in effect");
    }
    held();
    kill(pid, SIGCONT);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return abandon(pid, "the seized process did not run on after the detach");
    }
    held();
    return 0;
}

/**
 * Wait for `pid` to change state, and check that it stopped with `want` as
 * its status shifted right by 8 bits. Returns 0, or 1 after the child is
 * killed and the failure `what` is printed.
 */
static int expect_stop(pid_t pid, int want, const char *what)
{
    int status;

    if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status) || status >> 8 != want) {
        return abandon(pid, what);
    }
    return 0;
}

/**
 * Check that a request on the child `pid` returned 0; otherwise kill the
 * child and report `what`. Returns 0, or 1 after the failure is printed.
 */
static int granted(pid_t pid, long rc, const char *what)
{
    return rc == 0 ? 0 : abandon(pid, what);
}

/**
 * Check the seize of a stopped `pid`, PTRACE_INTERRUPT ending its
 * PTRACE_LISTEN, a SIGCONT sent before a second PTRACE_LISTEN, the new event
 * stop the stop's end makes, and the SIGCONT's own stop. Returns 0, or 1
 * after the child is killed and the failure is printed.
 */
static int listen_first(pid_t pid)
{
    if (granted(pid, ptrace(PTRACE_SEIZE, pid, NULL, NULL),
                "PTRACE_SEIZE of the stopped process") ||
        expect_stop(pid, SIGSTOP | (PTRACE_EVENT_STOP << 8),
                    "the seize of the stopped process was not a group stop") ||
        passed() ||
        granted(pid, ptrace(PTRACE_LISTEN, pid, NULL, NULL), "PTRACE_LISTEN of the group stop") ||
        granted(pid, ptrace(PTRACE_INTERRUPT, pid, NULL, NULL), "PTRACE_INTERRUPT of the listen") ||
        expect_stop(pid, SIGSTOP | (PTRACE_EVENT_STOP << 8),
                    "PTRACE_INTERRUPT did not end PTRACE_LISTEN") ||
        passed() || granted(pid, kill(pid, SIGCONT), "the SIGCONT before the second listen") ||
        granted(pid, ptrace(PTRACE_LISTEN, pid, NULL, NULL), "the second PTRACE_LISTEN") ||
        expect_stop(pid, SIGTRAP | (PTRACE_EVENT_STOP << 8),
                    "PTRACE_LISTEN after the stop ended made no new event stop") ||
        passed() ||
        granted(pid, ptrace(PTRACE_CONT, pid, NULL, NULL), "PTRACE_CONT of the event stop") ||
        expect_stop(pid, SIGCONT, "the SIGCONT made no signal-delivery stop") || passed()) {
        return 1;
    }
    return 0;
}

/**
 * Check that PTRACE_LISTEN fails with EIO at a signal-delivery stop, then the
 * second SIGSTOP's group stop, ended by a SIGCONT sent during PTRACE_LISTEN,
 * and let the process run on. Returns 0, or 1 after the child is killed and
 * the failure is printed.
 */
static int listen_second(pid_t pid)
{
    if (ptrace(PTRACE_LISTEN, pid, NULL, NULL) != -1 || errno != EIO) {
        return abandon(pid, "PTRACE_LISTEN of a signal-delivery stop was taken");
    }
    held();
    if (granted(pid, ptrace(PTRACE_CONT, pid, NULL, NULL), "PTRACE_CONT of the SIGCONT's stop") ||
        expect_stop(pid, SIGSTOP, "the second SIGSTOP made no signal-delivery stop") || passed() ||
        granted(pid, ptrace(PTRACE_CONT, pid, NULL, (void *)(long)SIGSTOP),
                "PTRACE_CONT with the second SIGSTOP") ||
        expect_stop(pid, SIGSTOP | (PTRACE_EVENT_STOP << 8),
                    "the second stop was not a group stop") ||
        passed() ||
        granted(pid, ptrace(PTRACE_LISTEN, pid, NULL, NULL), "PTRACE_LISTEN of the second stop") ||
        granted(pid, kill(pid, SIGCONT), "the SIGCONT during the listen") ||
        expect_stop(pid, SIGTRAP | (PTRACE_EVENT_STOP << 8),
                    "the stop's end during PTRACE_LISTEN made no new event stop") ||
        passed() ||
        granted(pid, ptrace(PTRACE_CONT, pid, NULL, NULL),
                "PTRACE_CONT of the second event stop") ||
        expect_stop(pid, SIGCONT, "the second SIGCONT made no signal-delivery stop") || passed() ||
        granted(pid, ptrace(PTRACE_CONT, pid, NULL, NULL), "PTRACE_CONT to the process's end")) {
        return 1;
    }
    return 0;
}

/**
 * Check that PTRACE_LISTEN keeps a seized tracee in the PTRACE_EVENT_STOP of
 * its group stop until the stop ends, and that the end makes a new
 * PTRACE_EVENT_STOP with SIGTRAP, with the SIGCONT sent before the listen, as
 * strace's start can order them on two processors, and after it. The SIGCONT
 * is then a signal-delivery stop, at which PTRACE_LISTEN fails with EIO. On
 * x86-64 PTRACE_SEIZE fails with EINVAL for an option bit past 32.
 * Returns 0, or 1 after the failure is printed.
 */
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
    held();
#endif
    if (listen_first(pid) != 0 || listen_second(pid) != 0) {
        return 1;
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return abandon(pid, "the listened-to process did not run on");
    }
    held();
    return 0;
}

/**
 * Check that PTRACE_INTERRUPT stops a seized tracee seen asleep in a read of
 * an empty pipe, with PTRACE_EVENT_STOP and SIGTRAP, and that the read,
 * restarted when the tracer resumes it, reads the byte written after.
 * Returns 0, or 1 after the failure is printed.
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
    if (await_state(pid, 'S') != 'S') {
        return abandon(pid, "the tracee was never seen asleep in its read");
    }
    if (ptrace(PTRACE_SEIZE, pid, NULL, NULL) != 0 ||
        ptrace(PTRACE_INTERRUPT, pid, NULL, NULL) != 0 || waitpid(pid, &status, 0) != pid ||
        !WIFSTOPPED(status) || status >> 8 != (SIGTRAP | (PTRACE_EVENT_STOP << 8))) {
        return abandon(pid, "PTRACE_INTERRUPT did not stop a tracee blocked in a read");
    }
    held();
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || write(p[1], "g", 1) != 1) {
        return abandon(pid, "resuming the interrupted tracee");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return abandon(pid, "the interrupted read was not restarted");
    }
    held();
    close(p[1]);
    return 0;
}

/**
 * Check a seized tracee with PTRACE_O_TRACEFORK that forks: its stop for the
 * event gives the new child's id, the child's first stop is PTRACE_EVENT_STOP
 * with SIGTRAP, as strace's -f waits for it, and both run on to their ends, the
 * tracee through the stop its child's SIGCHLD makes. Returns 0, or 1 after the
 * failure is printed.
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
        if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) {
            return abandon(pid, "the seized process did not stop at its fork");
        }
        if (status >> 8 == (SIGTRAP | (PTRACE_EVENT_FORK << 8))) {
            break;
        }
        ptrace(PTRACE_CONT, pid, NULL, NULL);
    }
    held();
    ptrace(PTRACE_GETEVENTMSG, pid, NULL, &child);
    if (child == 0 || waitpid((pid_t)child, &status, 0) != (pid_t)child || !WIFSTOPPED(status) ||
        status >> 8 != (SIGTRAP | (PTRACE_EVENT_STOP << 8))) {
        return abandon(pid, "the child of a seized fork did not first stop with SIGTRAP");
    }
    held();
    if (ptrace(PTRACE_CONT, (pid_t)child, NULL, NULL) != 0 ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0) {
        return abandon(pid, "resuming the seized process and its child");
    }
    if (waitpid((pid_t)child, &status, 0) != (pid_t)child || !WIFEXITED(status)) {
        return abandon(pid, "the child of the seized process did not run to its end");
    }
    held();
    while (waitpid(pid, &status, 0) == pid && WIFSTOPPED(status)) {
        ptrace(PTRACE_CONT, pid, NULL, (void *)(long)WSTOPSIG(status));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return abandon(pid, "the seized process did not reap its child and end");
    }
    held();
    return 0;
}

/**
 * Check that a tracee that SIGKILL ends tells its tracer, which is its parent
 * here, with a SIGCHLD whose siginfo is CLD_KILLED and the signal, and that
 * the wait reports it killed. The parent blocks SIGCHLD and takes the one the
 * stop sent before the kill, so the siginfo it reads is that of the end.
 * Returns 0, or 1 after the failure is printed.
 */
static int killed_tracee(void)
{
    struct timespec now = {0, 0};
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
    if (kill(pid, SIGKILL) != 0 || sigwaitinfo(&chld, &si) != SIGCHLD) {
        return fail("no SIGCHLD came of the killed tracee");
    }
    held();
    if (si.si_code != CLD_KILLED || si.si_status != SIGKILL || si.si_pid != pid) {
        return fail("the SIGCHLD of a killed tracee was not CLD_KILLED with SIGKILL");
    }
    held();
    if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        return fail("the killed tracee was not reported killed by SIGKILL");
    }
    held();
    sigprocmask(SIG_UNBLOCK, &chld, NULL);
    return 0;
}

/* The system call and the instruction after it, for register checks. */
extern const char step_call[], step_return[], step_next[];
#if defined __x86_64__
__asm__(".text\n"
        ".globl step_call, step_return, step_next\n"
        "step_call: syscall\n"
        "step_return: incq %rbx\n"
        "step_next: int3\n");
#else
__asm__(".text\n"
        ".globl step_call, step_return, step_next\n"
        "step_call: int $0x80\n"
        "step_return: incl %ebx\n"
        "step_next: int3\n");
#endif

/**
 * Fork a traced child that stops itself with SIGSTOP, for a row that sets its
 * registers. Returns the child's id, or -1 after the failure of the fork or
 * the stop is printed.
 */
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
    if (pid < 0) {
        fail("fork of a register child");
        return -1;
    }
    if (expect_stop(pid, SIGSTOP, "register child did not stop")) {
        return -1;
    }
    return pid;
}

/**
 * Kill and reap a row's child after its last stop, so that the code the row
 * put in its registers does not run.
 */
static void finish_register_child(pid_t pid)
{
    int status;

    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

/**
 * Check, twice, that PTRACE_INTERRUPT of a tracee in PTRACE_LISTEN stops it
 * before its next instruction. The tracer points IP at step_return, which
 * increments BX, so a stop that came late finds BX or IP changed. The child is
 * killed either way. Returns 0, or 1 after the failure is printed.
 */
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
    held();
    finish_register_child(pid);
    return 0;
}

/**
 * Check PTRACE_LISTEN at an interrupt stop outside a group stop: a child
 * asleep in pause is seized and interrupted, and then runs the rows of
 * `interrupt_listening`. Returns 0, or 1 after the failure is printed.
 */
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
    if (granted(pid, ptrace(PTRACE_SEIZE, pid, NULL, NULL),
                "seizing the child for a listening interrupt") ||
        granted(pid, ptrace(PTRACE_INTERRUPT, pid, NULL, NULL),
                "the initial interrupt of the listening child") ||
        expect_stop(pid, SIGTRAP | (PTRACE_EVENT_STOP << 8),
                    "initial interrupt made no event stop")) {
        return 1;
    }
    held();
    return interrupt_listening(pid);
}

/**
 * Check that a SIGCONT, with no group stop to end, lets a seized tracee out
 * of PTRACE_LISTEN at an interrupt stop, as Linux's prepare_signal tells each
 * seized thread of every SIGCONT (ptrace_trap_notify): a child asleep in
 * pause is seized, interrupted and listened to, and the SIGCONT gives a new
 * PTRACE_EVENT_STOP with SIGTRAP and then, resumed, the SIGCONT's own
 * signal-delivery stop. The child is killed either way. Returns 0, or 1
 * after the failure is printed.
 */
static int listen_sigcont(void)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        for (;;) {
            pause();
        }
    }
    if (pid < 0) {
        return fail("fork for the listening SIGCONT");
    }
    if (granted(pid, ptrace(PTRACE_SEIZE, pid, NULL, NULL),
                "seizing the child for the listening SIGCONT") ||
        granted(pid, ptrace(PTRACE_INTERRUPT, pid, NULL, NULL),
                "the interrupt before the listening SIGCONT") ||
        expect_stop(pid, SIGTRAP | (PTRACE_EVENT_STOP << 8),
                    "the interrupt before the listening SIGCONT made no event stop") ||
        granted(pid, ptrace(PTRACE_LISTEN, pid, NULL, NULL),
                "PTRACE_LISTEN of the interrupt stop") ||
        granted(pid, kill(pid, SIGCONT), "the SIGCONT during the listen") ||
        expect_stop(pid, SIGTRAP | (PTRACE_EVENT_STOP << 8),
                    "a SIGCONT outside a group stop did not end PTRACE_LISTEN") ||
        passed() ||
        granted(pid, ptrace(PTRACE_CONT, pid, NULL, NULL),
                "PTRACE_CONT of the SIGCONT's event stop") ||
        expect_stop(pid, SIGCONT, "the listening SIGCONT made no signal-delivery stop") ||
        passed()) {
        return 1;
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return 0;
}

/**
 * As the tracer of `c`, asleep in a read of an empty pipe: seize it, stop
 * its process with SIGSTOP, let the stop's signal-delivery stop through to
 * the group stop's PTRACE_EVENT_STOP, resume it in the stop, which leaves it
 * to sleep in its read again, and see it asleep. Returns 0, for the caller
 * to end without PTRACE_O_EXITKILL, or the number of the step that failed.
 */
static int stop_resumer(pid_t c)
{
    int status;

    if (ptrace(PTRACE_SEIZE, c, NULL, NULL) != 0) {
        return 1;
    }
    if (kill(c, SIGSTOP) != 0 || waitpid(c, &status, 0) != c || !WIFSTOPPED(status) ||
        status >> 8 != SIGSTOP) {
        return 2;
    }
    if (ptrace(PTRACE_CONT, c, NULL, (void *)(long)SIGSTOP) != 0 || waitpid(c, &status, 0) != c ||
        !WIFSTOPPED(status) || status >> 8 != (SIGSTOP | (PTRACE_EVENT_STOP << 8))) {
        return 3;
    }
    if (ptrace(PTRACE_CONT, c, NULL, NULL) != 0 || await_state(c, 'S') != 'S') {
        return 4;
    }
    return 0;
}

/**
 * Check that a thread its tracer resumed during its process's stop, and
 * which sleeps in a pipe read, takes the stop when the tracer ends without
 * PTRACE_O_EXITKILL, as Linux's __ptrace_unlink makes the stop pending again
 * and wakes it: a second child traces the first (`stop_resumer`) and ends;
 * the first is then seen stopped (T), and after SIGCONT reads the byte
 * written to it and ends with 0. Returns 0, or 1 after the failure is
 * printed.
 */
static int tracer_gone_stop(void)
{
    int p[2], status = 0;
    char why[64];
    char ch;
    pid_t c, x;

    if (pipe(p) != 0) {
        return fail("pipe");
    }
    c = fork();
    if (c == 0) {
        close(p[1]);
        _exit(read(p[0], &ch, 1) == 1 && ch == 'g' ? 0 : 1);
    }
    close(p[0]);
    if (c < 0) {
        close(p[1]);
        return fail("fork of the sleeper");
    }
    x = fork();
    if (x == 0) {
        close(p[1]);
        _exit(stop_resumer(c));
    }
    if (x < 0 || waitpid(x, &status, 0) != x || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        close(p[1]);
        snprintf(why, sizeof(why), "the tracer that resumes in a stop, step %d",
                 x > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return abandon(c, why);
    }
    held();
    if (await_state(c, 'T') != 'T') {
        close(p[1]);
        return abandon(c, "a sleeper whose tracer ended did not take its group stop");
    }
    held();
    if (write(p[1], "g", 1) != 1 || kill(c, SIGCONT) != 0 || waitpid(c, &status, 0) != c ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        close(p[1]);
        return abandon(c, "the sleeper did not read its byte and end after SIGCONT");
    }
    held();
    close(p[1]);
    return 0;
}

/**
 * Check that PTRACE_SINGLESTEP over a system call instruction stops at
 * step_return with the call's result and TRAP_TRACE, before the increment of
 * BX runs, and that the next step runs exactly that one instruction.
 * Returns 0, or 1 after the failure is printed.
 */
static int step_system_call(void)
{
    struct user_regs_struct regs;
    siginfo_t info;
    pid_t pid = register_child();

    if (pid < 0) {
        return 1;
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
        expect_stop(pid, SIGTRAP, "syscall step did not trap") || passed() ||
        ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 ||
        ptrace(PTRACE_GETSIGINFO, pid, NULL, &info) != 0 ||
        (unsigned long)R_IP(regs) != (unsigned long)step_return || R_BX(regs) != 41 ||
        (unsigned long)R_RET(regs) != (unsigned long)pid || info.si_code != TRAP_TRACE) {
        return abandon(pid, "syscall step ran the next instruction or lost its result");
    }
    held();
    if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) != 0 ||
        expect_stop(pid, SIGTRAP, "step after syscall did not trap") || passed() ||
        ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 ||
        (unsigned long)R_IP(regs) != (unsigned long)step_next || R_BX(regs) != 42) {
        return abandon(pid, "step after syscall did not execute exactly one instruction");
    }
    held();
    finish_register_child(pid);
    return 0;
}

/**
 * Check that a system call made with the user stack pointer 1 returns its
 * result and runs on to the int3 at step_next, since the kernel's entry changes
 * to a kernel stack before it uses one. Returns 0, or 1 after the failure is
 * printed.
 */
static int entry_system_call(void)
{
    struct user_regs_struct regs;
    pid_t pid = register_child();

    if (pid < 0) {
        return 1;
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
        expect_stop(pid, SIGTRAP, "syscall on an invalid user stack did not return") || passed() ||
        ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0 ||
        (unsigned long)R_IP(regs) != (unsigned long)step_next + 1 || R_BX(regs) != 42 ||
        (unsigned long)R_RET(regs) != (unsigned long)pid) {
        return abandon(pid, "syscall entry lost its frame or result");
    }
    held();
    finish_register_child(pid);
    return 0;
}

/**
 * Check that a write watchpoint on `byte`, which the kernel's copy of a read
 * into it hits, stops nothing, as on Linux, whose ptrace breakpoints exclude
 * the kernel: the first stop is the child's own write of `byte` after the
 * read, which still traps with TRAP_HWBKPT, and the read returns its byte.
 * A child that exits untrapped held copy-watch-no-stop and fails
 * copy-watch-user-trap, since its breakpoint was not loaded again after the
 * kernel's hit. Returns 0, or 1 after the failure is printed.
 */
static int watched_copy(void)
{
    static char byte;
    static volatile int read_done;
    int p[2], status;
    long word;
    pid_t pid;
    siginfo_t info;

    if (pipe(p) != 0 || write(p[1], "g", 1) != 1) {
        return fail("pipe for watched copy");
    }
    pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        if (read(p[0], &byte, 1) != 1 || byte != 'g') {
            _exit(1);
        }
        read_done = 1;
        *(volatile char *)&byte = 'h';
        _exit(0);
    }
    close(p[0]);
    close(p[1]);
    if (pid < 0) {
        return fail("fork for watched copy");
    }
    if (expect_stop(pid, SIGSTOP, "watched copy child did not stop")) {
        return 1;
    }
    if (debug_set(pid, 0, (unsigned long)&byte) != 0 || debug_set(pid, 7, 0x10001) != 0 ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || waitpid(pid, &status, 0) != pid) {
        return abandon(pid, "watched copy child did not resume");
    }
    if (WIFEXITED(status)) {
        held();
        return fail("watched write after the copy did not trap; the child exited");
    }
    if (!WIFSTOPPED(status)) {
        return fail("watched copy child was killed");
    }
    if (status >> 8 != SIGTRAP) {
        return abandon(pid, "watched copy child stopped without SIGTRAP");
    }
    errno = 0;
    word = ptrace(PTRACE_PEEKDATA, pid, (void *)&read_done, NULL);
    if (errno != 0 || (int)word != 1) {
        return abandon(pid, "kernel copy through a watchpoint stopped the tracee");
    }
    held();
    if (ptrace(PTRACE_GETSIGINFO, pid, NULL, &info) != 0 || info.si_code != TRAP_HWBKPT) {
        return abandon(pid, "watched write after the copy lost TRAP_HWBKPT");
    }
    held();
    if (debug_set(pid, 7, 0) != 0 || ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 ||
        waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return abandon(pid, "watched kernel copy lost its result");
    }
    held();
    return 0;
}

/**
 * Check that the kernel's read of a watched word in poll stops nothing, as on
 * Linux: a poll of one negative descriptor with a zero timeout returns 0, and
 * the child exits with no stop between. Returns 0, or 1 after the failure is
 * printed.
 */
static int watched_poll(void)
{
    static struct pollfd pfd = {-1, POLLIN, 0};
    int status;
    pid_t pid = fork();

    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        _exit(syscall(SYS_poll, &pfd, 1, 0) == 0 ? 0 : 1);
    }
    if (pid < 0) {
        return fail("fork for watched poll");
    }
    if (expect_stop(pid, SIGSTOP, "watched poll child did not stop")) {
        return 1;
    }
    if (debug_set(pid, 0, (unsigned long)&pfd.fd) != 0 || debug_set(pid, 7, 0xf0001) != 0 ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status)) {
        return abandon(pid, "watched poll stopped the tracee");
    }
    held();
    if (WEXITSTATUS(status) != 0) {
        return fail("watched poll did not return 0");
    }
    held();
    return 0;
}

/**
 * Block SIGTRAP, stop for the tracer, then look, in the raw rt_sigtimedwait
 * with a zero timeout, for a SIGTRAP from the kernel's read of the watched
 * set at `set`. Runs in the forked child and exits 0 when the wait finds
 * none (EAGAIN), and 1 otherwise.
 */
static void sigwait_child(sigset_t *set)
{
    struct timespec zero = {0, 0};
    siginfo_t info;

    sigemptyset(set);
    sigaddset(set, SIGTRAP);
    memset(&info, 0, sizeof(info));
    if (sigprocmask(SIG_BLOCK, set, NULL) != 0 || ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0 ||
        raise(SIGSTOP) != 0) {
        _exit(1);
    }
    _exit(syscall(SYS_rt_sigtimedwait, set, &info, &zero, 8) == -1 && errno == EAGAIN ? 0 : 1);
}

/**
 * Check that the kernel's read of a watched signal set posts no SIGTRAP, as
 * on Linux: a wait for it finds none, and the tracer sees no stop. Returns 0,
 * or 1 after the failure is printed.
 */
static int watched_sigwait(void)
{
    static sigset_t set;
    int status;
    pid_t pid = fork();

    if (pid == 0) {
        sigwait_child(&set);
    }
    if (pid < 0) {
        return fail("fork for watched sigwait");
    }
    if (expect_stop(pid, SIGSTOP, "watched sigwait child did not stop")) {
        return 1;
    }
    if (debug_set(pid, 0, (unsigned long)&set) != 0 || debug_set(pid, 7, 0xf0001) != 0 ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return abandon(pid, "kernel read of a watched sigwait set posted SIGTRAP or stopped");
    }
    held();
    return 0;
}

/*
 * The thread pointer through ptrace. Each row's tracee is a fork of the
 * tracer, so its thread pointer is the tracer's, pthread_self. The tracer
 * moves it to tp_copy, a copy of the thread's first words with TP_MARK
 * after them, and the tracee, continued, must read the mark through its
 * segment. On x86-64 the requests are PTRACE_ARCH_PRCTL and the fs_base and
 * gs_base words of struct user. A base at or above USER_ADDR_MAX, the end of
 * user space, fails with EPERM under ARCH_SET_FS and ARCH_SET_GS and with EIO
 * under POKEUSER, SETREGS and SETREGSET, and leaves the bases as they were.
 * On i386 the requests are PTRACE_GET_THREAD_AREA and PTRACE_SET_THREAD_AREA:
 * the first TLS entry, 6 on an i386 kernel and 12 on an x86-64 one, is the
 * thread pointer's, the two after it read as unused and take only an unused
 * entry, and the others are EINVAL.
 */
#define TP_WORDS 16
#define TP_MARK  0x5A17C0DEUL

static unsigned long tp_copy[TP_WORDS + 1];

/**
 * Return the word after the copied ones, read through the thread pointer's
 * segment, FS on x86-64 and GS on i386.
 */
static unsigned long tp_mark_word(void)
{
    unsigned long v;

#if defined __x86_64__
    __asm__ __volatile__("movq %%fs:%c1, %0" : "=r"(v) : "i"(TP_WORDS * sizeof(long)));
#else
    __asm__ __volatile__("movl %%gs:%c1, %0" : "=r"(v) : "i"(TP_WORDS * sizeof(long)));
#endif
    return v;
}

#if defined __x86_64__
#define ARCH_SET_GS   0x1001
#define ARCH_SET_FS   0x1002
#define ARCH_GET_FS   0x1003
#define ARCH_GET_GS   0x1004
#define USER_ADDR_MAX 0x00007FFFFFFFF000UL
#define GS_MARK       0x6B28D1EFUL

/* The block the tracer moves GS's base to; its first word is GS_MARK. */
static unsigned long gs_block[2] = {GS_MARK};

/** Return the first word GS's base addresses. */
static unsigned long gs_word(void)
{
    unsigned long v;

    __asm__ __volatile__("movq %%gs:0, %0" : "=r"(v));
    return v;
}

/*
 * The offset in struct user of the register `field`, for PEEKUSER and POKEUSER.
 */
#define USER_WORD(field) ((void *)offsetof(struct user, regs.field))

/**
 * Read the tracee's FS and GS bases through PTRACE_ARCH_PRCTL into `*fs` and
 * `*gs`. Returns 0, or -1.
 */
static int bases_of(pid_t pid, unsigned long *fs, unsigned long *gs)
{
    return ptrace(PTRACE_ARCH_PRCTL, pid, fs, (void *)ARCH_GET_FS) == 0 &&
                   ptrace(PTRACE_ARCH_PRCTL, pid, gs, (void *)ARCH_GET_GS) == 0
               ? 0
               : -1;
}

/**
 * Check that each write of a base at or above USER_ADDR_MAX fails, and that the
 * bases `fs` and `gs` of the tracee `pid` stay as they were. Returns 0, or 1
 * after the failure is printed.
 */
static int refused_bases(pid_t pid, unsigned long fs, unsigned long gs)
{
    static const unsigned long refused[] = {USER_ADDR_MAX, USER_ADDR_MAX + 1, ~0UL};
    struct user_regs_struct regs, high;
    struct iovec iov;
    unsigned long now_fs, now_gs;
    int base = rows_reported;
    unsigned int i;

    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0) {
        return abandon(pid, "GETREGS before the refused bases");
    }
    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        if (ptrace(PTRACE_ARCH_PRCTL, pid, (void *)refused[i], (void *)ARCH_SET_FS) != -1 ||
            errno != EPERM ||
            ptrace(PTRACE_ARCH_PRCTL, pid, (void *)refused[i], (void *)ARCH_SET_GS) != -1 ||
            errno != EPERM) {
            held_to(base);
            return abandon(pid, "ARCH_SET_FS or ARCH_SET_GS took a base past user space");
        }
        if (ptrace(PTRACE_POKEUSER, pid, USER_WORD(fs_base), (void *)refused[i]) != -1 ||
            errno != EIO ||
            ptrace(PTRACE_POKEUSER, pid, USER_WORD(gs_base), (void *)refused[i]) != -1 ||
            errno != EIO) {
            held_to(base + 1);
            return abandon(pid, "POKEUSER took a base past user space");
        }
        high = regs;
        high.fs_base = refused[i];
        if (ptrace(PTRACE_SETREGS, pid, NULL, &high) != -1 || errno != EIO) {
            held_to(base + 2);
            return abandon(pid, "SETREGS took an FS base past user space");
        }
        high = regs;
        high.gs_base = refused[i];
        iov.iov_base = &high;
        iov.iov_len = sizeof(high);
        if (ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov) != -1 || errno != EIO) {
            held_to(base + 3);
            return abandon(pid, "SETREGSET took a GS base past user space");
        }
    }
    held_to(base + 4);
    if (bases_of(pid, &now_fs, &now_gs) != 0 || now_fs != fs || now_gs != gs) {
        return abandon(pid, "a refused base changed the bases");
    }
    held();
    return 0;
}

/**
 * Check the thread pointer on x86-64: ARCH_GET_FS and PEEKUSER read
 * pthread_self, PTRACE_ARCH_PRCTL fails with EFAULT for a null address and
 * with EINVAL for an unknown code, the rows of `refused_bases`, and a tracee
 * continued with its bases moved reads TP_MARK and GS_MARK through them.
 * Returns 0, or 1 after the failure is printed.
 */
static int thread_pointer(void)
{
    unsigned long fs, gs, peek;
    int status;
    pid_t pid;

    memcpy(tp_copy, (void *)pthread_self(), TP_WORDS * sizeof(long));
    tp_copy[TP_WORDS] = TP_MARK;
    pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        _exit(tp_mark_word() == TP_MARK && gs_word() == GS_MARK ? 0 : 1);
    }
    if (pid < 0) {
        return fail("fork for the thread pointer");
    }
    if (expect_stop(pid, SIGSTOP, "the thread pointer's child did not stop")) {
        return 1;
    }
    if (bases_of(pid, &fs, &gs) != 0 || fs != (unsigned long)pthread_self()) {
        return abandon(pid, "ARCH_GET_FS did not read the thread pointer");
    }
    held();
    errno = 0;
    peek = (unsigned long)ptrace(PTRACE_PEEKUSER, pid, USER_WORD(fs_base), NULL);
    if (errno != 0 || peek != fs) {
        return abandon(pid, "PEEKUSER of fs_base did not read the thread pointer");
    }
    held();
    if (ptrace(PTRACE_ARCH_PRCTL, pid, NULL, (void *)ARCH_GET_FS) != -1 || errno != EFAULT ||
        ptrace(PTRACE_ARCH_PRCTL, pid, NULL, (void *)0x1005) != -1 || errno != EINVAL) {
        return abandon(pid, "PTRACE_ARCH_PRCTL took a bad address or code");
    }
    held();
    if (refused_bases(pid, fs, gs) != 0) {
        return 1;
    }
    if (ptrace(PTRACE_ARCH_PRCTL, pid, (void *)tp_copy, (void *)ARCH_SET_FS) != 0 ||
        ptrace(PTRACE_POKEUSER, pid, USER_WORD(gs_base), (void *)gs_block) != 0 ||
        bases_of(pid, &fs, &gs) != 0 || fs != (unsigned long)tp_copy ||
        gs != (unsigned long)gs_block) {
        return abandon(pid, "a base set did not read back");
    }
    held();
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return abandon(pid, "the tracee did not run with the bases set");
    }
    held();
    return 0;
}
#else
/*
 * The GDT entry of the thread pointer's segment, the first of the three TLS
 * entries, which musl's set_thread_area was given and GS selects: 6 on an
 * i386 kernel and 12 on an x86-64 kernel, as Linux's GDT_ENTRY_TLS_MIN.
 */
static unsigned int tls_entry(void)
{
    unsigned short gs;

    __asm__ volatile("mov %%gs, %0" : "=r"(gs));
    return gs >> 3;
}

#define TLS_ENTRY tls_entry()

/* Linux i386's struct user_desc: the flags are its bit fields. */
struct tp_desc {
    unsigned int entry_number;
    unsigned int base_addr;
    unsigned int limit;
    unsigned int flags;
};

#define UD_READ_EXEC_ONLY  0x08u
#define UD_SEG_NOT_PRESENT 0x20u

/**
 * Check that the two entries after the thread pointer's, 7 and 8 on an i386
 * kernel, read as unused and take only an unused entry, and that the entries
 * just outside the three fail with EINVAL.
 * Returns 0, or 1 after the failure is printed.
 */
static int other_entries(pid_t pid)
{
    struct tp_desc d;
    int base = rows_reported;
    unsigned int i;

    for (i = TLS_ENTRY + 1; i <= TLS_ENTRY + 2; i++) {
        if (ptrace(PTRACE_GET_THREAD_AREA, pid, (void *)i, &d) != 0 || d.entry_number != i ||
            d.base_addr != 0 || d.limit != 0 ||
            (d.flags & 0x7Fu) != (UD_READ_EXEC_ONLY | UD_SEG_NOT_PRESENT)) {
            held_to(base);
            return abandon(pid, "an entry beside the thread pointer's did not read as unused");
        }
        if (ptrace(PTRACE_SET_THREAD_AREA, pid, (void *)i, &d) != 0) {
            held_to(base + 1);
            return abandon(pid, "an unused entry could not be written back");
        }
        d.base_addr = 0x1000;
        if (ptrace(PTRACE_SET_THREAD_AREA, pid, (void *)i, &d) != -1 || errno != EINVAL) {
            held_to(base + 2);
            return abandon(pid, "an entry beside the thread pointer's took a base");
        }
    }
    held_to(base + 3);
    if (ptrace(PTRACE_GET_THREAD_AREA, pid, (void *)(TLS_ENTRY - 1), &d) != -1 || errno != EINVAL ||
        ptrace(PTRACE_GET_THREAD_AREA, pid, (void *)(TLS_ENTRY + 3), &d) != -1 || errno != EINVAL) {
        return abandon(pid, "an entry outside the three TLS entries was not EINVAL");
    }
    held();
    return 0;
}

/**
 * Check the thread pointer on i386: PTRACE_GET_THREAD_AREA reads
 * pthread_self in the first TLS entry, the rows of `other_entries`, and a
 * tracee continued with the entry's base moved reads TP_MARK through it.
 * Returns 0, or 1 after the failure is printed.
 */
static int thread_pointer(void)
{
    struct tp_desc d, back;
    int status;
    pid_t pid;

    memcpy(tp_copy, (void *)pthread_self(), TP_WORDS * sizeof(long));
    tp_copy[TP_WORDS] = TP_MARK;
    pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        _exit(tp_mark_word() == TP_MARK ? 0 : 1);
    }
    if (pid < 0) {
        return fail("fork for the thread pointer");
    }
    if (expect_stop(pid, SIGSTOP, "the thread pointer's child did not stop")) {
        return 1;
    }
    if (ptrace(PTRACE_GET_THREAD_AREA, pid, (void *)TLS_ENTRY, &d) != 0 ||
        d.entry_number != TLS_ENTRY || d.base_addr != (unsigned int)pthread_self()) {
        return abandon(pid, "PTRACE_GET_THREAD_AREA did not read the thread pointer");
    }
    held();
    if (other_entries(pid) != 0) {
        return 1;
    }
    d.base_addr = (unsigned int)tp_copy;
    if (ptrace(PTRACE_SET_THREAD_AREA, pid, (void *)TLS_ENTRY, &d) != 0 ||
        ptrace(PTRACE_GET_THREAD_AREA, pid, (void *)TLS_ENTRY, &back) != 0 ||
        back.base_addr != (unsigned int)tp_copy) {
        return abandon(pid, "a thread pointer set did not read back");
    }
    held();
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return abandon(pid, "the tracee did not run with the thread pointer set");
    }
    held();
    return 0;
}
#endif

/**
 * Run the rows of memory, system calls and attaching, and stop at the first
 * that fails. Returns 0, or 1.
 */
static int first_rows(void)
{
    return memory_and_registers() || mem_after_end() || syscalls_and_exec() || syscall_changes() ||
           killed_at_entry() || restart_cancelled() || attach() || attach_stopped() ||
           listen_and_continue() || interrupt_read();
}

/**
 * Run the rows of forks, threads, killed tracees and the debug exception, and
 * stop at the first that fails. Returns 0, or 1.
 */
static int second_rows(void)
{
    return seized_fork() || stopped_leader() || traced_thread_usage() || tracer_first() ||
           killed_tracee() || step_system_call() || entry_system_call() || watched_copy() ||
           watched_poll() || watched_sigwait() || listen_user_return() || listen_sigcont() ||
           tracer_gone_stop() || thread_pointer();
}

/* The assertions after `ended-mem-freed`, in the order they are checked. */
static const char *const later_rows[] = {
    "setoptions",
    "debugreg-peekuser",
    "dr7-kernel-einval",
    "dr7-last-user",
    "dr7-bit12-einval",
#if defined __x86_64__
    "dr7-bit32-einval",
    "setoptions-wide-einval",
#endif
    "getpid-entry",
    "getpid-exit",
    "exec-waitid",
    "exec-event",
    "exec-mem-empty",
    "exec-runs",
    "changes-getpid-entry",
#if defined __x86_64__
    "ds-kernel-eio",
#endif
    "changes-write-entry",
#if defined __x86_64__
    "ds-kept",
#endif
    "changes-exit",
    "changes-getppid",
    "changes-write-result",
    "changes-write-not-run",
    "kill-entry-stop",
    "kill-entry-sigkill",
    "kill-entry-no-write",
    "restart-read-entry",
    "restart-read-eintr",
    "restart-signal",
    "restart-cancelled",
    "attach-stops",
    "detach",
    "detach-runs-on",
    "seize-stopped-event",
    "seize-detach",
    "seize-stays-stopped",
    "seize-sigcont-runs",
#if defined __x86_64__
    "seize-wide-einval",
#endif
    "listen-group-stop",
    "listen-interrupt",
    "listen-cont-before",
    "listen-sigcont-stop",
    "listen-delivery-eio",
    "listen-second-sigstop",
    "listen-second-group-stop",
    "listen-cont-during",
    "listen-second-sigcont",
    "listen-runs-on",
    "interrupt-stops",
    "interrupt-read-restarted",
    "fork-event",
    "fork-child-event-stop",
    "fork-child-ends",
    "fork-parent-ends",
    "leader-signal-wakes",
    "leader-signal-taken",
    "thread-attach",
    "thread-usage",
    "thread-child-ends",
    "tracer-seize",
    "tracer-first",
    "tracer-then-parent",
    "tracer-sees-end",
    "tracer-echild",
    "killed-sigchld",
    "killed-sigchld-info",
    "killed-wait-status",
    "step-syscall-trap",
    "step-syscall-result",
    "step-next-trap",
    "step-next-one",
    "entry-bad-stack-returns",
    "entry-bad-stack-result",
    "copy-watch-no-stop",
    "copy-watch-user-trap",
    "copy-watch-result",
    "poll-watch-no-stop",
    "poll-watch-returns",
    "sigwait-watch",
    "interrupt-pause-stop",
    "listen-interrupt-before-instruction",
    "listen-sigcont-event-stop",
    "listen-sigcont-delivery",
    "tracer-gone-resumed",
    "tracer-gone-group-stop",
    "tracer-gone-runs-on",
#if defined __x86_64__
    "tp-arch-get-fs",
    "tp-peekuser-fs",
    "tp-arch-prctl-errors",
    "tp-arch-set-eperm",
    "tp-pokeuser-eio",
    "tp-setregs-eio",
    "tp-setregset-eio",
    "tp-refused-kept",
#else
    "tp-get-thread-area",
    "tp-entries-unused",
    "tp-entries-write-back",
    "tp-entries-no-base",
    "tp-entries-outside-einval",
#endif
    "tp-set-readback",
    "tp-tracee-runs",
};

/**
 * Begin the report, with the branch of the architecture, and plan its
 * assertions in the order they are checked; a native run reports
 * `ended-mem-freed` not run.
 */
static void plan(void)
{
    static const char *const first[] = {
        "traced-stop",
        "peekdata",
        "pokedata",
        "proc-mem-read",
        "proc-mem-hole",
        "getregs",
        "fpregs",
        "singlestep",
        "breakpoint-reached",
        "breakpoint-ip",
        "breakpoint-restore",
        "poked-word-read",
        "ended-mem-live-read",
        "ended-mem-child-exits",
        "ended-meminfo",
        "ended-mem-read-zero",
    };
    unsigned int i;

    report_begin(report_case(environ, "guest.default.ptracemusl"));
#if defined __x86_64__
    report_branch("arch", "x86-64");
#else
    report_branch("arch", "i686");
#endif
    for (i = 0; i < sizeof(first) / sizeof(first[0]); i++) {
        plan_row(first[i]);
    }
    if (probe_native()) {
        report_notrun("ended-mem-freed", "the machine's services move free memory");
    } else {
        plan_row("ended-mem-freed");
    }
    for (i = 0; i < sizeof(later_rows) / sizeof(later_rows[0]); i++) {
        plan_row(later_rows[i]);
    }
}

int main(int argc, char **argv)
{
    probe_args(argc, argv);
    if (probe_words() != 0) {
        return fail("unexpected argument count");
    }
    plan();
    if (first_rows() || second_rows()) {
        return 1;
    }
    report_end();
    printf("PTRACEMUSL OK\n");
    return 0;
}
