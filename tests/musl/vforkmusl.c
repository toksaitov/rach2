/**
 * clone(CLONE_VM|CLONE_VFORK) and vfork, as glibc's posix_spawn, system and
 * popen start their children.
 *
 * A child made with CLONE_VM|CLONE_VFORK on a stack of its own shares the
 * parent's memory, and the parent does not run again until the child has
 * called execve: a word the child stores before its exec is there when
 * clone returns, and the parent's memory is whole after the exec replaced
 * the child's image. vfork's child shares the memory the same way until it
 * calls _exit. A child's exec that fails comes back to the child, whose
 * store of the error the parent then reads. system and popen run a command.
 *
 * Prints "VFORKMUSL OK" on success and "VFORKMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile int word;
static volatile int exec_errno;
static char stack[16384];
static char guard[64] = "the parent's bytes";

static int fail(const char *what)
{
    printf("VFORKMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

static int run_shell(void *arg)
{
    (void)arg;
    word = 42;
    execl("/bin/sh", "sh", "-c", "exit 5", (char *)0);
    _exit(127);
}

static int run_missing(void *arg)
{
    (void)arg;
    execl("/no/such/program", "none", (char *)0);
    exec_errno = errno;
    _exit(126);
}

int main(void)
{
    char line[16];
    FILE *p;
    pid_t pid;
    int status;

    pid = clone(run_shell, stack + sizeof(stack), CLONE_VM | CLONE_VFORK | SIGCHLD, NULL);
    if (pid < 0) {
        return fail("clone CLONE_VM|CLONE_VFORK");
    }
    if (word != 42) {
        return fail("the parent ran before the child's exec, or the memory was not shared");
    }
    if (strcmp(guard, "the parent's bytes") != 0) {
        return fail("the child's exec changed the parent's memory");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 5) {
        return fail("the shell the child became");
    }

    pid = clone(run_missing, stack + sizeof(stack), CLONE_VM | CLONE_VFORK | SIGCHLD, NULL);
    if (pid < 0 || exec_errno != ENOENT) {
        return fail("a failed exec did not come back to the child");
    }
    if (waitpid(pid, &status, 0) != pid || WEXITSTATUS(status) != 126) {
        return fail("the child whose exec failed");
    }

    pid = vfork();
    if (pid == 0) {
        word = 7;
        _exit(3);
    }
    if (pid < 0 || word != 7) {
        return fail("vfork did not share the memory until _exit");
    }
    if (waitpid(pid, &status, 0) != pid || WEXITSTATUS(status) != 3) {
        return fail("the vfork child");
    }

    status = system("exit 4");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 4) {
        return fail("system");
    }
    p = popen("echo hi", "r");
    if (!p || !fgets(line, sizeof(line), p) || strcmp(line, "hi\n") != 0 || pclose(p) != 0) {
        return fail("popen");
    }
    printf("VFORKMUSL OK\n");
    return 0;
}
