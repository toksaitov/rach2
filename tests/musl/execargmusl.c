/**
 * How much of a command line this machine's `execve` will carry.
 *
 * A link line is why this matters: a compiler, `-static`, `-o`, an output name
 * and a dozen object files is more than fifteen arguments and more than five
 * hundred bytes, which is what the old limits were. A build tool that cannot
 * exec its own recipe is not a build tool.
 *
 * Every case here is on a BOUNDARY, because a limit is only tested at its edge:
 * the largest vector that must work and the smallest that must not, for the
 * entry count of each vector and for the byte budget the two of them share.
 *
 * The byte cases are the ones worth explaining. The strings of both vectors are
 * laid into ONE 4096-byte page by `build_user_stack`, and that runs after the
 * old address space is freed, so a vector that passes the copy-in and fails
 * there kills the process instead of failing the call. The over-budget case
 * therefore asserts two things at once: that `execve` returns `E2BIG`, and that
 * this program is still running to see it. Before the budget was checked at
 * copy-in, that case did not return at all.
 *
 * The child half asserts rather than reports: it exits nonzero unless it was
 * handed exactly the count and the exact last string the parent sent, so a
 * vector delivered short is a failure and not a line in a log.
 *
 * Prints "EXECARGMUSL OK" on success and "EXECARGMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * What the kernel holds: EXEC_MAX_ENTRIES arguments in each vector, and
 * EXEC_MAX_STRINGS bytes of strings across argv and envp TOGETHER, each string
 * counted with its terminator (rach2/sys/syscall.c). Written out here rather
 * than derived, so a change to either is a change to this test.
 */
#define KERNEL_MAX_ENTRIES 63
#define KERNEL_MAX_STRINGS 3072

/* The last argument of the full vector, which the child compares against. */
#define LAST_ARG "tail-of-the-line"

static const char *const SELF = "/musl/execargmusl";

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("EXECARGMUSL FAIL (%s)\n", what);

    return 1;
}

/** Wait for `pid` and return its exit status, or -1 if it did not exit. */
static int status_of(pid_t pid)
{
    int status = 0;
    pid_t seen = waitpid(pid, &status, 0);

    if (seen != pid || !WIFEXITED(status)) {
        printf("EXECARG WAIT pid %d seen %d errno %d status 0x%x\n", (int)pid, (int)seen, errno,
               status);

        return -1; /* a wait that did not report a normal exit */
    }

    return WEXITSTATUS(status);
}

/**
 * Run `argv` and `envp` through a forked exec and return the child's status.
 *
 * Forked, so a vector this kernel refuses leaves the probe running to report
 * it; the child reports a refused exec as status 9.
 */
static int exec_child(char *const argv[], char *const envp[])
{
    pid_t pid = fork();

    if (pid < 0) {
        printf("EXECARG FORK failed, errno %d\n", errno);

        return -1;
    }
    if (pid == 0) {
        execve(SELF, argv, envp);
        _exit(9);
    }

    return status_of(pid);
}

int main(int argc, char *argv[])
{
    static char storage[KERNEL_MAX_ENTRIES + 2][24];
    static char big_arg[KERNEL_MAX_STRINGS];
    static char big_env[KERNEL_MAX_STRINGS];
    char *args[KERNEL_MAX_ENTRIES + 3];
    char *env[KERNEL_MAX_ENTRIES + 3];
    char *two_env[2];
    int i;

    /* The child half. Handed the full vector, it must have received all of
     * it. */
    if (argc > 1 && strcmp(argv[1], "full") == 0) {
        if (argc != KERNEL_MAX_ENTRIES) {
            printf("EXECARG CHILD saw %d arguments, wanted %d\n", argc, KERNEL_MAX_ENTRIES);

            return 1;
        }
        if (strcmp(argv[argc - 1], LAST_ARG) != 0) {
            printf("EXECARG CHILD last argument is '%s'\n", argv[argc - 1]);

            return 1;
        }

        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "ok") == 0) {
        return 0;
    }

    two_env[0] = (char *)"PATH=/bin";
    two_env[1] = (char *)0;

    /* The largest argv that must work: every entry the kernel holds. */
    args[0] = (char *)SELF;
    args[1] = (char *)"full";
    for (i = 2; i < KERNEL_MAX_ENTRIES - 1; i++) {
        snprintf(storage[i], sizeof(storage[i]), "arg%02d", i);
        args[i] = storage[i];
    }
    args[KERNEL_MAX_ENTRIES - 1] = (char *)LAST_ARG;
    args[KERNEL_MAX_ENTRIES] = (char *)0;
    {
        int rc = exec_child(args, two_env);

        if (rc != 0) {
            char detail[96];

            snprintf(detail, sizeof(detail),
                     "the largest allowed argv came back as status %d (9 means the exec "
                     "was refused)",
                     rc);

            return fail(detail);
        }
    }

    /* One more entry than that must be refused. */
    args[0] = (char *)SELF;
    args[1] = (char *)"ok";
    for (i = 2; i <= KERNEL_MAX_ENTRIES; i++) {
        args[i] = (char *)"x";
    }
    args[KERNEL_MAX_ENTRIES + 1] = (char *)0;
    if (execve(SELF, args, two_env) == 0) {
        return fail("an exec with one argument too many was accepted");
    }
    if (errno != E2BIG) {
        return fail("one argument too many did not report E2BIG");
    }

    /* The same boundary for the environment, which has an array of its own. */
    args[0] = (char *)SELF;
    args[1] = (char *)"ok";
    args[2] = (char *)0;
    for (i = 0; i < KERNEL_MAX_ENTRIES; i++) {
        snprintf(storage[i], sizeof(storage[i]), "E%02d=v", i);
        env[i] = storage[i];
    }
    env[KERNEL_MAX_ENTRIES] = (char *)0;
    if (exec_child(args, env) != 0) {
        return fail("an environment of exactly the largest allowed size was refused");
    }
    env[KERNEL_MAX_ENTRIES] = (char *)"E99=v";
    env[KERNEL_MAX_ENTRIES + 1] = (char *)0;
    if (execve(SELF, args, env) == 0) {
        return fail("an exec with one environment entry too many was accepted");
    }
    if (errno != E2BIG) {
        return fail("one environment entry too many did not report E2BIG");
    }

    /*
     * The byte budget, at its edge, split across both vectors so that only the
     * check adding them together can refuse it. Each string costs its length
     * plus a terminator, and argv[0] is a string like any other.
     */
    {
        int arg_text = 1500;
        int spent = (int)strlen(SELF) + 1 + (int)strlen("ok") + 1 + arg_text + 1;
        int env_text = KERNEL_MAX_STRINGS - spent - 1;

        memset(big_arg, 'y', (size_t)arg_text);
        big_arg[arg_text] = '\0';
        big_env[0] = 'E';
        big_env[1] = '=';
        memset(big_env + 2, 'z', (size_t)(env_text - 2));
        big_env[env_text] = '\0';

        /* The MODE goes first and the bulk second: a child that did not
         * recognise its first argument would run this whole sequence again,
         * forking as it went, which is a fork bomb rather than a test. */
        args[0] = (char *)SELF;
        args[1] = (char *)"ok";
        args[2] = big_arg;
        args[3] = (char *)0;
        env[0] = big_env;
        env[1] = (char *)0;
        {
            int rc = exec_child(args, env);

            if (rc != 0) {
                char detail[96];

                snprintf(detail, sizeof(detail),
                         "a vector of exactly %d bytes came back as status %d", KERNEL_MAX_STRINGS,
                         rc);

                return fail(detail);
            }
        }

        /* One byte more must be refused, and this program must still be here
         * to say so: that is the half the copy-in check exists for. */
        big_env[env_text] = 'z';
        big_env[env_text + 1] = '\0';
        if (execve(SELF, args, env) == 0) {
            return fail("an exec one byte over the budget was accepted");
        }
        if (errno != E2BIG) {
            return fail("one byte over the budget did not report E2BIG");
        }
    }

    /*
     * The exact-fill path: argv alone consumes the whole budget and another
     * argument follows, with an empty environment so the combined check has
     * nothing to add. That reaches the storage-exhausted branch, which is a
     * size limit and must say so; it answered EFAULT until this milestone,
     * which tells a program its addresses are bad when its command line is
     * merely too long.
     */
    {
        int fill = KERNEL_MAX_STRINGS - ((int)strlen(SELF) + 1) - ((int)strlen("ok") + 1) - 1;
        char *empty_env[1];

        empty_env[0] = (char *)0;
        memset(big_arg, 'y', (size_t)fill);
        big_arg[fill] = '\0';
        args[0] = (char *)SELF;
        args[1] = (char *)"ok";
        args[2] = big_arg;
        args[3] = (char *)"one-too-many";
        args[4] = (char *)0;
        if (execve(SELF, args, empty_env) == 0) {
            return fail("an exec whose arguments exactly filled the buffer took another");
        }
        if (errno != E2BIG) {
            char detail[80];

            snprintf(detail, sizeof(detail), "a full argument buffer reported errno %d, not E2BIG",
                     errno);

            return fail(detail);
        }
    }

    printf("EXECARGMUSL OK\n");

    return 0;
}
