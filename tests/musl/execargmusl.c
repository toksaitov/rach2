/**
 * How much of a command line this machine's `execve` will carry, which is
 * what Linux carries.
 *
 * The program's path, the strings of argv and envp and a pointer for each
 * string may take a quarter of the soft RLIMIT_STACK, at least 128 KiB and at
 * most 6 MiB, and one string may be 128 KiB with its NUL. The strings are
 * copied onto the new stack's pages before the old image is destroyed, so a
 * vector past a limit is refused with E2BIG and the caller is still running
 * to see it: each exec here runs in a forked child, which exits 7 when
 * `execve` returns E2BIG and 9 when it returns anything else.
 *
 * Every limit is tested at its edge: the largest vector that must work and
 * the smallest that must not. The child half asserts, not reports: it exits
 * nonzero unless it was handed exactly what the parent sent, so a vector
 * delivered short is a failure and not a line in a log.
 *
 * Prints "EXECARGMUSL OK" on success and "EXECARGMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define KIB     1024UL
#define MIB     (1024UL * 1024UL)
#define MANY    5000
#define LONGEST 131071 /* the longest string: 128 KiB with its NUL */

static const char *const SELF = "/musl/execargmusl";

static char names[MANY][8];
static char *vec[32770];
static char big_a[LONGEST + 2];
static char big_b[LONGEST + 2];

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("EXECARGMUSL FAIL (%s)\n", what);

    return 1;
}

/**
 * Exec this program with `argv` and `envp` in a forked child whose soft
 * RLIMIT_STACK is `limit`, and return the child's exit status: the new
 * image's, or 7 when `execve` returned E2BIG and 9 when it returned another
 * error. -1 when the child did not exit.
 */
static int run(char *const argv[], char *const envp[], rlim_t limit)
{
    struct rlimit rl;
    int status = 0;
    pid_t pid = fork();

    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        rl.rlim_cur = limit;
        rl.rlim_max = RLIM_INFINITY;
        if (setrlimit(RLIMIT_STACK, &rl) != 0) {
            _exit(8);
        }
        execve(SELF, argv, envp);
        _exit(errno == E2BIG ? 7 : 9);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/** The child half: 0 when this image was handed what its mode says. */
static int child(int argc, char *argv[])
{
    int i;

    if (getenv("EXECARG_EMPTY") != NULL) {
        return argc == 1 && argv[0] != NULL && argv[0][0] == '\0' && argv[1] == NULL ? 0 : 1;
    }
    if (argc < 2) {
        return 1;
    }
    if (strcmp(argv[1], "many") == 0) {
        if (argc != MANY + 2) {
            return 1;
        }
        for (i = 0; i < MANY; i++) {
            if (strcmp(argv[i + 2], names[i]) != 0) {
                return 1;
            }
        }
        return 0;
    }
    if (strcmp(argv[1], "long") == 0) {
        return argc == 3 && strlen(argv[2]) == LONGEST && argv[2][LONGEST - 1] == 'x' ? 0 : 1;
    }
    if (strcmp(argv[1], "env") == 0) {
        char want[8];

        for (i = 0; environ[i] != NULL; i++) {
            snprintf(want, sizeof(want), "E%04d", i);
            memcpy(want + 5, "=v", 3);
            if (i >= MANY || strcmp(environ[i], want) != 0) {
                return 1;
            }
        }
        return i == MANY ? 0 : 1;
    }
    return strcmp(argv[1], "ok") == 0 ? 0 : 1;
}

/** Fill `s` with `n` copies of `c` and a NUL. */
static char *text(char *s, size_t n, char c)
{
    memset(s, c, n);
    s[n] = '\0';
    return s;
}

/** Five thousand arguments, and five thousand variables, are delivered
 * whole. */
static int many(void)
{
    char *none[1] = {NULL};
    int i;

    vec[0] = (char *)SELF;
    vec[1] = (char *)"many";
    for (i = 0; i < MANY; i++) {
        vec[i + 2] = names[i];
    }
    vec[MANY + 2] = NULL;
    if (run(vec, none, 8 * MIB) != 0) {
        return fail("five thousand arguments were not delivered whole");
    }
    for (i = 0; i < MANY; i++) {
        snprintf(names[i], sizeof(names[i]), "E%04d", i);
        memcpy(names[i] + 5, "=v", 3);
        vec[i] = names[i];
    }
    vec[MANY] = NULL;
    {
        char *argv[3] = {(char *)SELF, (char *)"env", NULL};

        if (run(argv, vec, 8 * MIB) != 0) {
            return fail("five thousand variables were not delivered whole");
        }
    }
    return 0;
}

/** One string of 128 KiB with its NUL is delivered; one a byte longer is
 * E2BIG. */
static int one_string(void)
{
    char *none[1] = {NULL};
    char *argv[4] = {(char *)SELF, (char *)"long", NULL, NULL};

    argv[2] = text(big_a, LONGEST, 'x');
    if (run(argv, none, 8 * MIB) != 0) {
        return fail("a string of 128 KiB with its NUL was not delivered");
    }
    argv[2] = text(big_a, LONGEST + 1, 'x');
    if (run(argv, none, 8 * MIB) != 7) {
        return fail("a string of 128 KiB and a NUL was not refused with E2BIG");
    }
    return 0;
}

/**
 * Under a stack limit of 1 MiB the budget is 256 KiB: the path, four strings
 * and four pointers fill it to the byte, split across argv and envp, and a
 * byte more is E2BIG.
 */
static int budget(void)
{
    char *argv[4] = {(char *)SELF, (char *)"ok", NULL, NULL};
    char *envp[2] = {NULL, NULL};
    size_t fixed = 2 * (strlen(SELF) + 1) + strlen("ok") + 1 + 4 * sizeof(char *);
    size_t env_len = 256 * KIB - fixed - (LONGEST + 1) - 1;

    argv[2] = text(big_a, LONGEST, 'y');
    envp[0] = text(big_b, env_len, 'z');
    memcpy(big_b, "E=", 2);
    if (run(argv, envp, 1 * MIB) != 0) {
        return fail("strings and pointers of exactly the budget were refused");
    }
    envp[0] = text(big_b, env_len + 1, 'z');
    memcpy(big_b, "E=", 2);
    if (run(argv, envp, 1 * MIB) != 7) {
        return fail("a byte past the budget was not refused with E2BIG");
    }
    return 0;
}

/** Pointers that take a 128 KiB budget by themselves are E2BIG, and an
 * empty argv is given one empty argument. */
static int pointers(void)
{
    char *empty_env[2] = {(char *)"EXECARG_EMPTY=1", NULL};
    char *none[1] = {NULL};
    int i;

    for (i = 0; i < 32768; i++) {
        vec[i] = (char *)"";
    }
    vec[32768] = NULL;
    if (run(vec, none, 256 * KIB) != 7) {
        return fail("32768 pointers under a 128 KiB budget were not refused with E2BIG");
    }
    if (run(none, empty_env, 8 * MIB) != 0) {
        return fail("an empty argv was not given one empty argument");
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int i;

    for (i = 0; i < MANY; i++) {
        snprintf(names[i], sizeof(names[i]), "a%04d", i);
    }
    if (argc != 1 || getenv("EXECARG_EMPTY") != NULL) {
        return child(argc, argv);
    }
    if (many() != 0 || one_string() != 0 || budget() != 0 || pointers() != 0) {
        return 1;
    }

    printf("EXECARGMUSL OK\n");

    return 0;
}
