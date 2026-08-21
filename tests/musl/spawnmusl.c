/**
 * `system(3)` and `popen(3)`, which this kernel could not run at all.
 *
 * musl reaches a child from both of them only through `posix_spawn`, and
 * upstream `posix_spawn` issues `clone` with CLONE_VM|CLONE_VFORK: the child
 * runs in the PARENT's address space until it execs. This kernel cannot share
 * an address space, so every program that shelled out failed with a bare -1 and
 * a line on the console about syscall 120. A make runs every recipe that way.
 *
 * The fix is one pinned patch to the vendored musl (see
 * vendor/musl/patches/): posix_spawn forks. The child function never returns
 * and reports failure through a pipe rather than through shared memory, so a
 * forked child serves it exactly as a cloned one did. Syscall 120 stays
 * unimplemented, which is the correct answer from a kernel with no threads and
 * no shared address spaces: a shim that accepted those flags and handed back a
 * private copy would be reporting semantics it does not provide.
 *
 * What is asserted:
 *
 * A command RUNS and its status arrives. `system("exit 3")` must report 3
 * through the wait encoding, not 3 as a raw return.
 *
 * A command's OUTPUT arrives. `popen` reads a line back and `pclose` reports
 * the status, which is the pair `$(shell ...)` and a `!=` assignment need.
 *
 * The PARENT survives intact. The child is a fork now, with a copy of the
 * parent's memory rather than a share of it, so this writes a canary into its
 * own stack and heap around the calls and requires both to be untouched.
 *
 * And `clone` is still REFUSED. A kernel that cannot share an address space
 * must not answer the call that asks for one: `clone` with the spawn's own
 * flags must come back ENOSYS, not a process with the wrong semantics.
 *
 * Prints "SPAWNMUSL OK" on success and "SPAWNMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* The flags upstream musl's posix_spawn passes to `clone`. This kernel cannot
 * share an address space, so syscall 120 is unimplemented whatever the flags
 * are (`ENOSYS`, asserted below) and the port patches posix_spawn to fork. */
#define CLONE_VM      0x00000100
#define CLONE_VFORK   0x00004000
#define CLONE_SIGCHLD 0x00000011

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

/** Whether every byte of a canary still has its value. */
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

    /* A command that succeeds reports zero. Spelled as a builtin rather than
     * as `true`, which is a program this image need not carry: a shell that
     * never ran at all reports 127, so this still tells the two apart. */
    status = system("exit 0");
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("a command that succeeds did not report success");
    }

    /* Output comes back, and pclose reports the status. */
    /* Named absolutely: this shell searches no PATH, and the point here is the
     * pipe rather than the lookup. */
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

    /* The parent's own memory is where a shared-address-space substitution
     * would show, so it is checked rather than assumed. */
    if (!canary_intact(stack_canary, sizeof(stack_canary))) {
        return fail("the parent's stack was written by the spawn");
    }
    if (!canary_intact(heap_canary, CANARY_BYTES)) {
        return fail("the parent's heap was written by the spawn");
    }
    free(heap_canary);
    heap_canary = NULL;

    /* Even upstream spawn's own flags are refused: this kernel implements no
     * `clone` at all, whatever is asked for. A thread library wanting a shared
     * address space must be told no, not handed a copy. */
    {
        /* A REAL stack, so a kernel that answered this call would get a
         * usable one and the refusal is about the call rather than about a
         * null pointer. */
        unsigned char *stack = malloc(4096);
        unsigned char *stack_top;

        if (stack == NULL) {
            return fail("out of memory for the clone stack");
        }
        stack_top = stack + 4096;
        if (syscall(SYS_clone, CLONE_VM | CLONE_VFORK | CLONE_SIGCHLD, stack_top, (void *)0,
                    (void *)0, (void *)0) != -1) {
            free(stack);

            return fail("clone was answered by a kernel that cannot share an address space");
        }
        if (errno != ENOSYS) {
            char detail[80];

            snprintf(detail, sizeof(detail), "clone reported errno %d, wanted ENOSYS", errno);
            free(stack);

            return fail(detail);
        }
        free(stack);
    }

    /* A spawn that FAILS. Nothing above this line exercises the error pipe:
     * every command so far execs. A missing program must come back as the
     * errno the child wrote through that pipe, and must leave no child behind
     * for anyone to wait on. */
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

    /* A command that FAILS through popen: the status has to come back, or a
     * `!=` assignment in a makefile cannot tell a command that worked from one
     * that did not. */
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

    /* And enough of them to cross this machine's ceilings. MAX_PROCS and
     * MAX_FDS are both 16, so a descriptor or a zombie leaked per call would
     * stop the twentieth of these; a make runs hundreds. */
    {
        int round;

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
    }

    printf("SPAWNMUSL OK\n");

    return 0;
}
