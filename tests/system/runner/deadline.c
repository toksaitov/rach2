/**
 * deadline: run a command under a wall-clock limit, killing its whole
 * process group when the limit passes.
 *
 * Usage: `deadline SECONDS COMMAND [ARG ...]`. The command runs in a new
 * process group. When it exits in time, its exit status (or 128 plus its
 * fatal signal) is returned. When the limit passes, the group receives
 * `SIGTERM`, then `SIGKILL` two seconds later if anything survives, and the
 * status is 124, the same contract as timeout(1). Killing the group rather
 * than the child matters because a test that hangs often hangs in a
 * descendant (a QEMU it started, a shell it spawned), and a signal to the
 * child alone would leave the descendant holding the port or the disk.
 *
 * This replaces coreutils' gtimeout on macOS, so the suites need no
 * coreutils package. The wait loop polls at 100 ms with `waitpid`; the
 * limit is read from `CLOCK_MONOTONIC`, so a clock step cannot stretch or
 * cut the budget.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    long limit;
    char *end;
    pid_t child;
    double start, term_at;
    int killed = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: deadline SECONDS COMMAND [ARG ...]\n");
        return 125;
    }
    limit = strtol(argv[1], &end, 10);
    if (*end != '\0' || limit <= 0) {
        fprintf(stderr, "deadline: '%s' is not a positive second count\n", argv[1]);
        return 125;
    }

    child = fork();
    if (child < 0) {
        perror("deadline: fork");
        return 125;
    }
    if (child == 0) {
        setpgid(0, 0);
        execvp(argv[2], argv + 2);
        fprintf(stderr, "deadline: cannot run %s\n", argv[2]);
        _exit(127);
    }
    setpgid(child, child);

    start = now();
    term_at = 0;
    for (;;) {
        int status;
        pid_t got = waitpid(child, &status, WNOHANG);

        if (got == child) {
            if (!killed && WIFEXITED(status))
                return WEXITSTATUS(status);
            if (!killed && WIFSIGNALED(status))
                return 128 + WTERMSIG(status);
            /* The group may still hold survivors after the child died to
               our signal; sweep once more, then report the timeout. */
            kill(-child, SIGKILL);
            return 124;
        }
        if (got < 0 && errno != EINTR) {
            perror("deadline: waitpid");
            return 125;
        }
        if (!killed && now() - start >= (double)limit) {
            fprintf(stderr, "deadline: %lds passed; stopping the group\n", limit);
            kill(-child, SIGTERM);
            killed = 1;
            term_at = now();
        } else if (killed && now() - term_at >= 2.0) {
            kill(-child, SIGKILL);
        }
        {
            struct timespec nap = {0, 100000000L};

            nanosleep(&nap, NULL);
        }
    }
}
