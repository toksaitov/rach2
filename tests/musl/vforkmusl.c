/**
 * vforkmusl: check clone(CLONE_VM|CLONE_VFORK) and vfork, the calls glibc's
 * posix_spawn, system and popen start their children with.
 *
 * A child made with CLONE_VM|CLONE_VFORK on a stack of its own shares the
 * parent's memory, and the parent does not run again until the child has
 * called execve: a word the child stores before its exec is there when
 * clone returns, and the parent's memory is whole after the exec replaced
 * the child's image. The parent runs again at the exec, not at the end of
 * the program the child becomes, which waits for a byte the parent writes
 * once it runs. vfork's child shares the memory the same way until it
 * calls _exit. A child's exec that fails comes back to the child, whose
 * store of the error the parent then reads. system and popen run a command.
 *
 * The program runs itself as the program the child becomes, with the
 * arguments `wait FD`, by the path /musl/vforkmusl or, when it was started by
 * an absolute path, as a native run starts it (tests/native/Readme.md), by
 * that path. It prints "VFORKMUSL OK" and exits 0 on success, or
 * prints "VFORKMUSL FAIL (detail: errno N)" and exits 1 at the first failure.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, an assertion for each check: `clone-vfork`, `shared-store`,
 * `parent-memory`, `resume-at-exec`, `exec-fail-errno`, `exec-fail-status`,
 * `vfork-shared`, `vfork-reap`, `system` and `popen`. A failed check fails
 * its assertion and leaves those after it not run.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

extern char **environ;

/* The path this program execs itself by: /musl/vforkmusl, or the absolute
 * path it was started by. */
static const char *self = "/musl/vforkmusl";

/* A word a child that shares the parent's memory stores before its exec. */
static volatile int word;

/* The errno of the child's failed exec, stored in the shared memory. */
static volatile int exec_errno;

/* The stack of the clone children; the parent does not run while one does. */
static char stack[16384];

/* Bytes the parent checks after a child's exec, to see its memory is whole. */
static char guard[64] = "the parent's bytes";

/* The pipe the program the child becomes waits on, and the number of its
 * read end as that program's argument. */
static int msg[2];
static char msg_arg[16];

/** Report one failed check with errno, end the report at the assertion that
 * `what` fails, and return 1, the failure status. */
static int fail(const char *what)
{
    printf("VFORKMUSL FAIL (%s: errno %d)\n", what, errno);
    report_stop(what);
    return 1;
}

/**
 * Store 42 in `word` and exec this program as the waiter. Runs in the clone
 * child, in the parent's memory; returns only through `_exit(127)` when the
 * exec fails.
 */
static int run_waiter(void *arg)
{
    (void)arg;
    word = 42;
    execl(self, "vforkmusl", "wait", msg_arg, (char *)0);
    _exit(127);
}

/**
 * Run as the program the child becomes: wait for the byte the parent writes
 * on `fd` once it runs again. Returns 5 when the byte came and 6 when the
 * poll or the read failed, the exit status the parent reads.
 */
static int wait_for_parent(int fd)
{
    struct pollfd pfd;
    char c;

    pfd.fd = fd;
    pfd.events = POLLIN;
    return poll(&pfd, 1, -1) == 1 && read(fd, &c, 1) == 1 ? 5 : 6;
}

/**
 * Exec a program that does not exist, store the errno in `exec_errno` and
 * exit with status 126. Runs in the clone child, in the parent's memory.
 */
static int run_missing(void *arg)
{
    (void)arg;
    execl("/no/such/program", "none", (char *)0);
    exec_errno = errno;
    _exit(126);
}

int main(int argc, char **argv)
{
    char line[16];
    FILE *p;
    pid_t pid;
    int status;

    if (argc == 3 && strcmp(argv[1], "wait") == 0) {
        return wait_for_parent(atoi(argv[2]));
    }
    probe_args(argc, argv);
    report_begin(report_case(environ, "guest.default.vforkmusl"));
    report_plan("clone-vfork");
    report_plan("shared-store");
    report_plan("parent-memory");
    report_plan("resume-at-exec");
    report_plan("exec-fail-errno");
    report_plan("exec-fail-status");
    report_plan("vfork-shared");
    report_plan("vfork-reap");
    report_plan("system");
    report_plan("popen");
    self = probe_self(self);
    if (pipe(msg) != 0) {
        return fail("the pipe the child's program waits on");
    }
    snprintf(msg_arg, sizeof(msg_arg), "%d", msg[0]);
    pid = clone(run_waiter, stack + sizeof(stack), CLONE_VM | CLONE_VFORK | SIGCHLD, NULL);
    if (pid < 0 || write(msg[1], "g", 1) != 1) {
        return fail("clone CLONE_VM|CLONE_VFORK, or the byte for the child's program");
    }
    report_held();
    if (word != 42) {
        return fail("the parent ran before the child's exec, or the memory was not shared");
    }
    report_held();
    if (strcmp(guard, "the parent's bytes") != 0) {
        return fail("the child's exec changed the parent's memory");
    }
    report_held();
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return fail("the program the child became");
    }
    if (WEXITSTATUS(status) != 5) {
        return fail(WEXITSTATUS(status) == 6
                        ? "the parent ran again at the child's exit, not its exec"
                        : "the program the child became");
    }
    report_held();
    close(msg[0]);
    close(msg[1]);

    pid = clone(run_missing, stack + sizeof(stack), CLONE_VM | CLONE_VFORK | SIGCHLD, NULL);
    if (pid < 0 || exec_errno != ENOENT) {
        return fail("a failed exec did not come back to the child");
    }
    report_held();
    if (waitpid(pid, &status, 0) != pid || WEXITSTATUS(status) != 126) {
        return fail("the child whose exec failed");
    }
    report_held();

    pid = vfork();
    if (pid == 0) {
        word = 7;
        _exit(3);
    }
    if (pid < 0 || word != 7) {
        return fail("vfork did not share the memory until _exit");
    }
    report_held();
    if (waitpid(pid, &status, 0) != pid || WEXITSTATUS(status) != 3) {
        return fail("the vfork child");
    }
    report_held();

    status = system("exit 4");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 4) {
        return fail("system");
    }
    report_held();
    p = popen("echo hi", "r");
    if (!p || !fgets(line, sizeof(line), p) || strcmp(line, "hi\n") != 0 || pclose(p) != 0) {
        return fail("popen");
    }
    report_held();
    report_end();
    printf("VFORKMUSL OK\n");
    return 0;
}
