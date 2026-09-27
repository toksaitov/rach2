/**
 * ptrace, as GDB drives it.
 *
 * A child that calls PTRACE_TRACEME and stops itself is reported stopped to
 * its parent. The parent reads and changes a word of the child's memory with
 * PEEKDATA and POKEDATA, and through /proc/PID/mem; reads the registers with
 * GETREGS and the floating-point state with GETFPXREGS; steps one
 * instruction with SINGLESTEP, which moves the instruction pointer; plants a
 * breakpoint with POKETEXT on a read-only text page, continues to it, finds
 * the child stopped by SIGTRAP just past it, puts the text back, moves the
 * instruction pointer back with SETREGS and continues. PTRACE_SYSCALL stops
 * the child at the entry and the exit of getpid, with the call's number in
 * orig_eax and its result in eax at the exit, and bit 0x80 on the stop
 * signal with PTRACE_O_TRACESYSGOOD. PTRACE_O_TRACEEXEC stops the child
 * after an execve with the exec event. A debug register set through
 * POKEUSER reads back through PEEKUSER. PTRACE_ATTACH stops a running
 * process, which runs on after PTRACE_DETACH.
 *
 * Prints "PTRACEMUSL OK" on success and "PTRACEMUSL FAIL (detail)"
 * otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
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
    if (ptrace(PTRACE_POKEUSER, pid, (void *)offsetof(struct user, u_debugreg[0]),
               (void *)dr) != 0 ||
        ptrace(PTRACE_PEEKUSER, pid, (void *)offsetof(struct user, u_debugreg[0]), NULL) !=
            0x1000) {
        return fail("a debug register through POKEUSER and PEEKUSER");
    }
    for (;;) {
        if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) != 0 ||
            !stopped_by(pid, SIGTRAP | 0x80) || ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0) {
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
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || waitpid(pid, &status, 0) != pid ||
        !WIFSTOPPED(status) || status >> 8 != (SIGTRAP | (PTRACE_EVENT_EXEC << 8))) {
        return fail("the exec event");
    }
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0 || waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 6) {
        return fail("the exec'd program");
    }
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

int main(void)
{
    if (memory_and_registers() || syscalls_and_exec() || attach()) {
        return 1;
    }
    printf("PTRACEMUSL OK\n");
    return 0;
}
