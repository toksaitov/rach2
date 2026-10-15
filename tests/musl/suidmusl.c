/**
 * suidmusl: check the set-id exec through the ported musl: libc sees the ids
 * and the AT_SECURE flag the kernel sets on a set-id exec, and the kernel
 * ignores the set-id bit when a process without root could change the new
 * image.
 *
 * Run with no argument, the program forks a child, drops it to uid 1000, and
 * execs the setuid-root copy of itself (`/musl/suidmusl.suid`) with the
 * argument `child`. That instance, entered through the C runtime, checks that
 * getresuid reports real 1000 and effective and saved 0, that
 * getauxval(AT_SECURE) is 1, and that the saved uid lets it shed root to euid
 * 1000 and take it back. A child traced by the probe, whose tracer is root,
 * gets the same. A child traced by a helper that is uid 1000 too, and one
 * such a helper makes with CLONE_FS, sharing its directories and umask,
 * exec the copy with the argument `plain`, which must find every id 1000,
 * AT_SECURE 0 and itself dumpable. A child that is uid 1000 execs the
 * execute-only copy, mode 0111 (`/musl/suidmusl.xonly`), with the argument
 * `xonly`, which must find itself not dumpable, since its caller may not
 * read it; a helper that is uid 1000, tracing such a child, must get EIO
 * from PTRACE_PEEKTEXT at the exec's stop. The parent relays each status.
 *
 * Prints "SUIDMUSL OK" on success, "SUIDMUSL FAIL ..." otherwise. Linked
 * against the ported musl.
 */

#define _GNU_SOURCE /* getresuid, setresuid, setresgid, clone */
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#define SUID_COPY  "/musl/suidmusl.suid"
#define XONLY_COPY "/musl/suidmusl.xonly"

/**
 * Check, in the setuid-root copy run by uid 1000, that the real uid is 1000,
 * the effective and saved uids are 0, AT_SECURE is 1, and the saved uid lets
 * the process set its effective uid to 1000 and back to 0. Returns 0 when all
 * hold, and 1 after printing the failure.
 */
static int run_child(void)
{
    uid_t r, e, s;

    if (getresuid(&r, &e, &s) != 0) {
        printf("SUIDMUSL FAIL (getresuid)\n");
        return 1;
    }
    if (r != 1000 || e != 0 || s != 0) {
        printf("SUIDMUSL FAIL (ids r=%ld e=%ld s=%ld)\n", (long)r, (long)e, (long)s);
        return 1;
    }
    if (getauxval(AT_SECURE) != 1) {
        printf("SUIDMUSL FAIL (AT_SECURE not 1)\n");
        return 1;
    }
    if (seteuid(1000) != 0 || geteuid() != 1000 || seteuid(0) != 0 || geteuid() != 0) {
        printf("SUIDMUSL FAIL (saved-id dance)\n");
        return 1;
    }
    return 0;
}

/**
 * Check, in a copy whose set-id bit the kernel ignored, that every uid is
 * 1000, AT_SECURE is 0 and the process is dumpable. Returns 0 when all hold,
 * and 1 after printing the failure.
 */
static int run_plain(void)
{
    uid_t r, e, s;

    if (getresuid(&r, &e, &s) != 0 || r != 1000 || e != 1000 || s != 1000) {
        printf("SUIDMUSL FAIL (the set-id bit applied: e=%ld s=%ld)\n", (long)e, (long)s);
        return 1;
    }
    if (getauxval(AT_SECURE) != 0 || prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 1) {
        printf("SUIDMUSL FAIL (AT_SECURE, or not dumpable, with the bit ignored)\n");
        return 1;
    }
    return 0;
}

/**
 * Check, in the execute-only copy, which its caller may run and not read,
 * that every uid is 1000 and the process is not dumpable. Returns 0 when both
 * hold, and 1 after printing the failure.
 */
static int run_xonly(void)
{
    uid_t r, e, s;

    if (getresuid(&r, &e, &s) != 0 || r != 1000 || e != 1000 || s != 1000) {
        printf("SUIDMUSL FAIL (the execute-only copy's ids)\n");
        return 1;
    }
    if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 0) {
        printf("SUIDMUSL FAIL (the execute-only copy is dumpable)\n");
        return 1;
    }
    return 0;
}

/** Set every uid and gid of the calling process to 1000; 0, or -1. */
static int drop(void)
{
    return setresgid(1000, 1000, 1000) == 0 && setresuid(1000, 1000, 1000) == 0 ? 0 : -1;
}

/**
 * Fork a child that drops to uid 1000, asks to be traced by the caller when
 * `traced` is nonzero, and execs the copy at `path` with the argument `mode`.
 * A traced child is continued past the SIGTRAP its exec stops with. Returns
 * the child's exit status, or 1 when the fork, the stop or the wait fails.
 */
static int exec_copy(const char *path, const char *mode, int traced)
{
    pid_t pid = fork();
    int status;

    if (pid < 0) {
        return 1;
    }
    if (pid == 0) {
        if (drop() != 0 || (traced && ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)) {
            _exit(1);
        }
        execl(path, "suidmusl", mode, (char *)0);
        _exit(1);
    }
    if (traced && (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status) ||
                   WSTOPSIG(status) != SIGTRAP || ptrace(PTRACE_CONT, pid, NULL, NULL) != 0)) {
        return 1;
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return 1;
    }
    return WEXITSTATUS(status);
}

/**
 * Run the setuid-root copy as `plain` in a child traced by the caller, which
 * `as_user` has made uid 1000. Returns the child's status, or 1.
 */
static int traced_row(void)
{
    return exec_copy(SUID_COPY, "plain", 1);
}

/* The stack of the CLONE_FS child of `shared_row`. */
static char clone_stack[16384] __attribute__((aligned(16)));

/** Exec the setuid-root copy as `plain`; the CLONE_FS child of `shared_row`. */
static int exec_plain(void *arg)
{
    (void)arg;
    execl(SUID_COPY, "suidmusl", "plain", (char *)0);
    return 1;
}

/**
 * Run the setuid-root copy as `plain` in a child made with CLONE_FS, which
 * shares the caller's working directory, root and umask; the caller is uid
 * 1000 under `as_user`. Returns the child's exit status, or 1.
 */
static int shared_row(void)
{
    pid_t pid = clone(exec_plain, clone_stack + sizeof(clone_stack), CLONE_FS | SIGCHLD, NULL);
    int status;

    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return 1;
    }
    return WEXITSTATUS(status);
}

/**
 * Run `row` in a forked helper that has dropped to uid 1000. Returns the
 * helper's exit status, which is what `row` returned, or 1.
 */
static int as_user(int (*row)(void))
{
    pid_t pid = fork();
    int status;

    if (pid < 0) {
        return 1;
    }
    if (pid == 0) {
        _exit(drop() != 0 ? 1 : row());
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return 1;
    }
    return WEXITSTATUS(status);
}

/**
 * Check that a tracer that is uid 1000, the caller under `as_user`, cannot
 * read the execute-only copy: at the exec's stop, PTRACE_PEEKTEXT of the
 * copy's text must fail with EIO. The address read is that of `exec_copy`,
 * since the copy is this program. Returns 0 when the read fails so, and 1
 * otherwise; the child is killed either way.
 */
static int xonly_peek_row(void)
{
    pid_t pid = fork();
    int status, ok;
    long word;

    if (pid < 0) {
        return 1;
    }
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0) {
            _exit(1);
        }
        execl(XONLY_COPY, "suidmusl", "xonly", (char *)0);
        _exit(1);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
        return 1;
    }
    errno = 0;
    word = ptrace(PTRACE_PEEKTEXT, pid, (void *)exec_copy, NULL);
    ok = word == -1 && errno == EIO;
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return ok ? 0 : 1;
}

/**
 * Run as the copy named by the argument `child`, `plain` or `xonly`, or with
 * no argument run every row and print the result.
 */
int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "child") == 0) {
        return run_child();
    }
    if (argc >= 2 && strcmp(argv[1], "plain") == 0) {
        return run_plain();
    }
    if (argc >= 2 && strcmp(argv[1], "xonly") == 0) {
        return run_xonly();
    }
    if (exec_copy(SUID_COPY, "child", 0) != 0) {
        printf("SUIDMUSL FAIL (child status)\n");
        return 1;
    }
    if (exec_copy(SUID_COPY, "child", 1) != 0) {
        printf("SUIDMUSL FAIL (the exec traced by root)\n");
        return 1;
    }
    if (as_user(traced_row) != 0) {
        printf("SUIDMUSL FAIL (the exec traced by uid 1000)\n");
        return 1;
    }
    if (as_user(shared_row) != 0) {
        printf("SUIDMUSL FAIL (the exec whose directories uid 1000 shares)\n");
        return 1;
    }
    if (exec_copy(XONLY_COPY, "xonly", 0) != 0) {
        printf("SUIDMUSL FAIL (the execute-only copy)\n");
        return 1;
    }
    if (as_user(xonly_peek_row) != 0) {
        printf("SUIDMUSL FAIL (a uid 1000 tracer read the execute-only copy)\n");
        return 1;
    }
    printf("SUIDMUSL OK\n");
    return 0;
}
