/**
 * How the kernel names and schedules a process, read through musl.
 *
 * `gettid` returns the process id, in the process and in a forked child
 * alike, since each process is one thread.
 *
 * Prints "SCHEDMUSL OK" on success and "SCHEDMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("SCHEDMUSL FAIL (%s)\n", what);

    return 1;
}

/** The thread id: the process id, here and in a child. */
static int thread_ids(void)
{
    pid_t child;
    int status;

    if (gettid() != getpid()) {
        return fail("gettid is not the process id");
    }
    child = fork();
    if (child < 0) {
        return fail("fork failed");
    }
    if (child == 0) {
        _exit(gettid() == getpid() ? 0 : 1);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("a child's gettid is not its process id");
    }
    return 0;
}

int main(void)
{
    if (thread_ids() != 0) {
        return 1;
    }
    printf("SCHEDMUSL OK\n");

    return 0;
}
