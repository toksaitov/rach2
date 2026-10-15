/**
 * spawnmusl: check `system(3)` and `popen(3)`, which start their child through
 * `posix_spawn`.
 *
 * Upstream musl's `posix_spawn` starts the child with `clone` and
 * CLONE_VM|CLONE_VFORK, so the child runs in the parent's address space
 * until it execs. A patch to the vendored musl (`vendor/musl/patches/`) makes
 * it fork instead. The child function never returns and reports a failure
 * through a pipe rather than through shared memory, so a forked child serves
 * it as a cloned one does, in a copy of the parent's memory. A make runs
 * every recipe this way.
 *
 * The checks, in order:
 *
 * A command runs and its status arrives: `system("exit 3")` reports 3
 * through the wait encoding, not as a raw return, and `system("exit 0")`
 * reports 0. The second is a builtin rather than `true`, a program the image
 * need not have; a shell that did not run reports 127, so the check still
 * tells the two apart.
 *
 * A command's output arrives: `popen` reads back the line `/bin/echo` prints
 * and `pclose` reports its status, the pair `$(shell ...)` and a `!=`
 * assignment need. The command is named by its full path, so that the check
 * is of the pipe and not of the search of `PATH`.
 *
 * The parent's memory is unchanged: canaries on its stack and heap are whole
 * after the calls, where a child that shared the address space would show.
 *
 * A spawn that fails reports why: `posix_spawn` of a missing program returns
 * ENOENT, the errno the child wrote through the pipe, and leaves no child to
 * reap. The commands before it all exec, so none of them reaches the pipe.
 *
 * A command that fails through `popen` reports its status, 7, to `pclose`;
 * without it a `!=` assignment in a makefile cannot tell a command that
 * worked from one that did not.
 *
 * Twenty `system` and twenty `popen` calls in a row each succeed, and leave
 * as many descriptors open as before them and no child to reap, so that a
 * descriptor or a zombie leaked by each call shows whatever the sizes of the
 * kernel's tables.
 *
 * It prints "SPAWNMUSL OK" and exits 0 on success, or prints "SPAWNMUSL FAIL
 * (detail)" and exits 1 at the first failure.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#define CANARY_BYTES 512
#define CANARY_VALUE 0x5A

/* The heap canary, freed by every exit path through `fail`. */
static unsigned char *heap_canary;

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    free(heap_canary);
    heap_canary = NULL;
    printf("SPAWNMUSL FAIL (%s)\n", what);

    return 1;
}

/**
 * Return 1 when each of the `count` bytes at `bytes` still has
 * `CANARY_VALUE`, and 0 otherwise.
 */
static int canary_intact(const unsigned char *bytes, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        if (bytes[i] != CANARY_VALUE) {
            return 0;
        }
    }

    return 1;
}

/**
 * Return the count of the entries of /proc/self/fd, this process's open
 * descriptors and the one the count opens, or -1 when it cannot be read.
 */
static int open_fds(void)
{
    DIR *dir = opendir("/proc/self/fd");
    struct dirent *e;
    int n = 0;

    if (dir == NULL) {
        return -1;
    }
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] != '.') {
            n++;
        }
    }
    closedir(dir);

    return n;
}

/**
 * Run twenty `system` and twenty `popen` calls in a row, and check that each
 * succeeds and that together they leave as many descriptors open as before
 * them and no child to reap. Returns 0, or 1 after the failure is printed.
 */
static int check_rounds(void)
{
    FILE *pipe;
    char line[128];
    int round, status, fds = open_fds();

    if (fds < 0) {
        return fail("/proc/self/fd could not be read");
    }
    for (round = 0; round < 20; round++) {
        status = system("exit 0");
        if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            char detail[80];

            snprintf(detail, sizeof(detail), "system() failed on round %d of 20", round);

            return fail(detail);
        }
    }
    for (round = 0; round < 20; round++) {
        pipe = popen("/bin/echo x", "r");
        if (pipe == NULL || fgets(line, sizeof(line), pipe) == NULL) {
            if (pipe != NULL) {
                pclose(pipe);
            }
            {
                char detail[80];

                snprintf(detail, sizeof(detail), "popen() failed on round %d of 20", round);

                return fail(detail);
            }
        }
        if (pclose(pipe) != 0) {
            return fail("pclose reported a failure for a command that worked");
        }
    }
    if (open_fds() != fds) {
        return fail("the rounds of system() and popen() left descriptors open");
    }
    if (waitpid(-1, NULL, WNOHANG) != -1 || errno != ECHILD) {
        return fail("the rounds of system() and popen() left a child");
    }

    return 0;
}

int main(void)
{
    unsigned char stack_canary[CANARY_BYTES];
    char line[128];
    FILE *pipe;
    int status;

    memset(stack_canary, CANARY_VALUE, sizeof(stack_canary));
    heap_canary = malloc(CANARY_BYTES);
    if (heap_canary == NULL) {
        return fail("out of memory");
    }
    memset(heap_canary, CANARY_VALUE, CANARY_BYTES);

    /* A command runs, and its status comes back through the wait encoding. */
    status = system("exit 3");
    if (status == -1) {
        return fail("system() could not run a command at all");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 3) {
        char detail[80];

        snprintf(detail, sizeof(detail), "system() reported status 0x%x, wanted an exit of 3",
                 status);

        return fail(detail);
    }

    /* A command that succeeds reports zero. */
    status = system("exit 0");
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("a command that succeeds did not report success");
    }

    /* Output comes back, and pclose reports the status. */
    pipe = popen("/bin/echo spawned-line", "r");
    if (pipe == NULL) {
        return fail("popen() could not start a command");
    }
    if (fgets(line, sizeof(line), pipe) == NULL) {
        pclose(pipe);

        return fail("popen() gave no output");
    }
    if (strncmp(line, "spawned-line", 12) != 0) {
        pclose(pipe);

        return fail("popen() read back the wrong line");
    }
    status = pclose(pipe);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("pclose() did not report the command's status");
    }

    /* The parent's stack and heap are unchanged. */
    if (!canary_intact(stack_canary, sizeof(stack_canary))) {
        return fail("the parent's stack was written by the spawn");
    }
    if (!canary_intact(heap_canary, CANARY_BYTES)) {
        return fail("the parent's heap was written by the spawn");
    }
    free(heap_canary);
    heap_canary = NULL;

    /* A spawn that fails: ENOENT through the error pipe, and no child. */
    {
        pid_t spawned = 0;
        char *const args[] = {(char *)"/no/such/program", (char *)0};
        char *const spawn_env[] = {(char *)0};
        int rc = posix_spawn(&spawned, "/no/such/program", NULL, NULL, args, spawn_env);

        if (rc == 0) {
            return fail("spawning a program that does not exist reported success");
        }
        if (rc != ENOENT) {
            char detail[80];

            snprintf(detail, sizeof(detail), "a missing program reported %d, wanted ENOENT", rc);

            return fail(detail);
        }
        if (waitpid(-1, &status, WNOHANG) > 0) {
            return fail("a failed spawn left a child to be reaped");
        }
    }

    /* A command that fails through popen reports its status. */
    pipe = popen("exit 7", "r");
    if (pipe == NULL) {
        return fail("popen() could not start a failing command");
    }
    if (fgets(line, sizeof(line), pipe) != NULL) {
        pclose(pipe);

        return fail("a command that only exits printed something");
    }
    status = pclose(pipe);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 7) {
        char detail[80];

        snprintf(detail, sizeof(detail), "pclose reported 0x%x for a command that exited 7",
                 status);

        return fail(detail);
    }

    if (check_rounds() != 0) {
        return 1;
    }

    printf("SPAWNMUSL OK\n");

    return 0;
}
