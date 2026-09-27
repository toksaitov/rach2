/**
 * RLIMIT_STACK, kept and used by execve, read through musl.
 *
 * The limit starts at 8 MiB soft and unlimited hard, is kept as set, refuses
 * a soft value past the hard one, refuses an unprivileged caller a higher
 * hard one, and passes to a child. An exec keeps it and gives the new image
 * a stack that may grow to its size: this program runs itself, and its `deep`
 * half recurses through a number of mebibytes of stack. Twelve fit under a
 * limit of 16 MiB, past the 8 MiB the stack once had whatever the limit;
 * one does not fit under 256 KiB, and the child dies of SIGSEGV. The new
 * image reads back the limit it was started with.
 *
 * Prints "STACKMUSL OK" on success and "STACKMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define MIB (1024UL * 1024UL)

static const char *const SELF = "/musl/stackmusl";

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("STACKMUSL FAIL (%s)\n", what);

    return 1;
}

/** Use `kib` KiB of stack, 64 at a time; returns a byte of the last frame,
 * so no frame is folded away. */
static int deep(unsigned long kib)
{
    volatile char frame[65536];

    frame[0] = 1;
    frame[sizeof(frame) - 1] = 2;
    if (kib <= 64) {
        return frame[0];
    }
    return deep(kib - 64) + frame[sizeof(frame) - 1] - 2;
}

/** Set the stack limit to `soft` and `hard` bytes. */
static int set_stack(rlim_t soft, rlim_t hard)
{
    struct rlimit rl;

    rl.rlim_cur = soft;
    rl.rlim_max = hard;
    return setrlimit(RLIMIT_STACK, &rl);
}

/** A child of root that lowers its hard limit and becomes user 1000 may not
 * raise the hard limit again. */
static int unprivileged(void)
{
    if (set_stack(1 * MIB, 2 * MIB) != 0) {
        return 1;
    }
    if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
        return 2;
    }
    if (set_stack(1 * MIB, 4 * MIB) != -1 || errno != EPERM) {
        return 3;
    }
    return 0;
}

/** Run this program's `deep` half through `mib` MiB with the stack limit at
 * `soft`, and return its wait status, or -1. */
static int run_deep(rlim_t soft, unsigned long mib)
{
    char arg[24];
    char limit[24];
    pid_t pid;
    int status;

    snprintf(arg, sizeof(arg), "%lu", mib);
    snprintf(limit, sizeof(limit), "%lu", (unsigned long)soft);
    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        char *argv[5];

        if (set_stack(soft, RLIM_INFINITY) != 0) {
            _exit(90);
        }
        argv[0] = (char *)SELF;
        argv[1] = (char *)"deep";
        argv[2] = arg;
        argv[3] = limit;
        argv[4] = NULL;
        execv(SELF, argv);
        _exit(91);
    }
    if (waitpid(pid, &status, 0) != pid) {
        return -1;
    }
    return status;
}

int main(int argc, char *argv[])
{
    struct rlimit rl;
    pid_t pid;
    int status;

    if (argc == 4 && strcmp(argv[1], "deep") == 0) {
        if (getrlimit(RLIMIT_STACK, &rl) != 0 ||
            rl.rlim_cur != (rlim_t)strtoul(argv[3], NULL, 10)) {
            return 2;
        }
        return deep(strtoul(argv[2], NULL, 10) * 1024UL) == 1 ? 0 : 3;
    }

    if (getrlimit(RLIMIT_STACK, &rl) != 0 || rl.rlim_cur != 8 * MIB ||
        rl.rlim_max != RLIM_INFINITY) {
        return fail("the stack limit does not start at 8 MiB soft and unlimited hard");
    }
    if (set_stack(16 * MIB, RLIM_INFINITY) != 0 || getrlimit(RLIMIT_STACK, &rl) != 0 ||
        rl.rlim_cur != 16 * MIB) {
        return fail("a stack limit of 16 MiB was not kept");
    }
    if (set_stack(32 * MIB, 16 * MIB) != -1 || errno != EINVAL) {
        return fail("a soft limit past the hard one was not refused with EINVAL");
    }
    pid = fork();
    if (pid == 0) {
        _exit(getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur == 16 * MIB ? 0 : 1);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail("a child did not inherit the stack limit");
    }
    pid = fork();
    if (pid == 0) {
        _exit(unprivileged());
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail("an unprivileged caller raised its hard stack limit");
    }

    status = run_deep(16 * MIB, 12);
    if (status < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        char detail[80];

        snprintf(detail, sizeof(detail), "12 MiB of stack under a 16 MiB limit ended as 0x%x",
                 status);
        return fail(detail);
    }
    status = run_deep(256 * 1024, 1);
    if (status < 0 || !WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        return fail("1 MiB of stack under a 256 KiB limit did not die of SIGSEGV");
    }

    printf("STACKMUSL OK\n");

    return 0;
}
