/**
 * forktest: fork a child that exits with a known status, and check that the
 * parent's waitpid returns the child's pid and reads that status back.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, with the assertions `fork`, `waitpid`, that it returned the
 * child's pid, and `exit-status`. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define STDOUT_FILENO 1

#define CHILD_EXIT_STATUS 42

/* Linux wait-status decoding (the kernel encodes a normal exit as code<<8). */
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

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

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/** Write `s` to standard output. */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

/** Write `n` in decimal to standard output. */
static void print_num(int n)
{
    char buf[12];
    int i = 0;
    int neg = 0;

    if (n < 0) {
        neg = 1;
        n = -n;
    }
    if (n == 0) {
        buf[i++] = '0';
    } else {
        while (n > 0) {
            buf[i++] = '0' + (n % 10);
            n /= 10;
        }
    }
    if (neg) {
        buf[i++] = '-';
    }

    while (i > 0) {
        sys_write(STDOUT_FILENO, &buf[--i], 1);
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe, which `_start` calls with the initial stack: the parent forks
 * a child that exits with `CHILD_EXIT_STATUS`, then waits and checks that
 * `WIFEXITED` and `WEXITSTATUS` report that status. It exits 0 when they do
 * and 1 otherwise.
 */
void probe_main(unsigned long *sp)
{
    int pid;
    int status;
    int child_pid;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.forktest"));
    report_plan("fork");
    report_plan("waitpid");
    report_plan("exit-status");
    puts("Fork test starting...");

    print("Calling fork()... ");
    pid = sys_fork();

    if (pid < 0) {
        puts("FAILED");
        report_stop("fork failed");
        sys_exit(1);
    }

    if (pid == 0) {
        puts("OK (child)");
        print("Child: PID is 0 (as expected), exiting with status ");
        print_num(CHILD_EXIT_STATUS);
        puts("");
        sys_exit(CHILD_EXIT_STATUS);
    }

    print("OK (parent, child PID=");
    print_num(pid);
    puts(")");
    report_held();

    print("Parent: Calling waitpid()... ");
    child_pid = sys_waitpid(pid, &status, 0);

    if (child_pid != pid) {
        print("FAILED (returned ");
        print_num(child_pid);
        puts(", not the child)");
        report_stop("waitpid did not return the child's pid");
        sys_exit(1);
    }
    puts("OK");
    report_held();

    print("Parent: waitpid returned PID=");
    print_num(child_pid);
    print(", exit status=");
    print_num(WEXITSTATUS(status));
    puts("");

    if (WIFEXITED(status) && WEXITSTATUS(status) == CHILD_EXIT_STATUS) {
        report_held();
        report_end();
        puts("All fork/waitpid syscalls passed!");
        sys_exit(0);
    } else {
        report_stop("the child did not exit with status 42");
        print("FAILED: Expected exit status ");
        print_num(CHILD_EXIT_STATUS);
        print(", got ");
        print_num(WEXITSTATUS(status));
        puts("");
        sys_exit(1);
    }
}
