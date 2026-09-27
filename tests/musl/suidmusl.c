/**
 * The setuid-exec contract through the ported musl: prove libc sees the ids
 * and the AT_SECURE flag the kernel set on a set-id exec, and that the
 * kernel ignores the set-id bit when the new image could be changed by a
 * process without root.
 *
 * Run with no argument, the program forks a child, drops it to uid 1000, and
 * execs the setuid-root copy of itself (`/musl/suidmusl.suid`) with the
 * argument `child`. That instance, entered through the C runtime, checks that
 * getresuid reports real 1000 and effective and saved 0, that
 * getauxval(AT_SECURE) is 1, and that the saved uid lets it shed root to euid
 * 1000 and take it back. A child traced by the probe, whose tracer is root,
 * gets the same. A child traced by a helper that is uid 1000 too execs the
 * copy with the argument `plain`, which must find every id 1000, AT_SECURE
 * 0 and itself dumpable. The parent relays each status.
 *
 * Prints "SUIDMUSL OK" on success, "SUIDMUSL FAIL ..." otherwise. Linked
 * against the ported musl.
 */

#define _GNU_SOURCE /* getresuid, setresuid, setresgid */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

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
    /* Shed the acquired root and take it back: the saved uid is 0. */
    if (seteuid(1000) != 0 || geteuid() != 1000 || seteuid(0) != 0 || geteuid() != 0) {
        printf("SUIDMUSL FAIL (saved-id dance)\n");
        return 1;
    }
    return 0;
}

/** The copy where its set-id bit is ignored: every id 1000, no AT_SECURE,
 * and a dumpable process. */
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

/** Drop the calling process to uid and gid 1000; 0, or -1. */
static int drop(void)
{
    return setresgid(1000, 1000, 1000) == 0 && setresuid(1000, 1000, 1000) == 0 ? 0 : -1;
}

/**
 * Fork a child that drops to uid 1000, asks with `traced` to be traced by
 * the caller, and execs the setuid copy with the argument `mode`; continue
 * a traced one past the SIGTRAP its exec stops with. The child's exit
 * status, or 1.
 */
static int exec_copy(const char *mode, int traced)
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
        execl("/musl/suidmusl.suid", "suidmusl", mode, (char *)0);
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

/** Run `exec_copy` from a helper that is uid 1000 itself; its status, or 1. */
static int exec_copy_as_user(const char *mode, int traced)
{
    pid_t pid = fork();
    int status;

    if (pid < 0) {
        return 1;
    }
    if (pid == 0) {
        _exit(drop() != 0 ? 1 : exec_copy(mode, traced));
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return 1;
    }
    return WEXITSTATUS(status);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "child") == 0) {
        return run_child();
    }
    if (argc >= 2 && strcmp(argv[1], "plain") == 0) {
        return run_plain();
    }
    if (exec_copy("child", 0) != 0) {
        printf("SUIDMUSL FAIL (child status)\n");
        return 1;
    }
    if (exec_copy("child", 1) != 0) {
        printf("SUIDMUSL FAIL (the exec traced by root)\n");
        return 1;
    }
    if (exec_copy_as_user("plain", 1) != 0) {
        printf("SUIDMUSL FAIL (the exec traced by uid 1000)\n");
        return 1;
    }
    printf("SUIDMUSL OK\n");
    return 0;
}
