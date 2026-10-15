/**
 * execargmusl: check how much of a command line `execve` delivers, which is
 * what Linux delivers.
 *
 * The program's path, the strings of argv and envp and a pointer for each
 * string may take a quarter of the soft RLIMIT_STACK, at least 128 KiB and at
 * most 6 MiB, and one string may be 128 KiB with its NUL. The strings are
 * copied onto the new stack's pages before the old image is destroyed, so a
 * vector past a limit fails with E2BIG and the caller is still running to see
 * it: each exec here runs in a forked child, which exits 7 when `execve`
 * returns E2BIG and 9 when it returns anything else.
 *
 * Every limit is tested at its edge: the largest vector that must work and the
 * smallest that must not. On x86-64 an argument and a variable at 4 GiB and 8
 * GiB, whose addresses have zero low words, are delivered.
 *
 * The program execs itself. With no arguments it is the parent and runs the
 * checks; otherwise it is the child, in the mode its first argument names or,
 * with `EXECARG_EMPTY` set, the empty-argv mode, and it exits 0 only when it
 * was handed what the parent sent, so a vector delivered short fails the
 * check. The parent prints "EXECARGMUSL OK" and exits 0 on success, or
 * prints "EXECARGMUSL FAIL (detail)" and exits 1 at the first failure.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define KIB     1024UL
#define MIB     (1024UL * 1024UL)
#define MANY    5000
#define LONGEST 131071 /* the longest string: 128 KiB with its NUL */

static const char *const SELF = "/musl/execargmusl";

/* The arguments a0000 to a4999, then the variables E0000=v to E4999=v. */
static char names[MANY][8];

/* An argv or envp of up to 32768 strings and its null. */
static char *vec[32770];

/* Strings of up to LONGEST + 1 bytes and a NUL. */
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
 * image's, or 7 when `execve` returned E2BIG, 9 when it returned another
 * error, and 8 when the limit could not be set. Returns -1 when the fork
 * failed or the child did not exit.
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

/** Return 1 when `s` is `n` copies of `c` and its NUL, and 0 otherwise. */
static int all_of(const char *s, size_t n, char c)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] != c) {
            return 0;
        }
    }
    return s[n] == '\0';
}

/**
 * Return the length of the one variable, `E=` included, that with the path,
 * the three arguments of the budget case and four pointers fills a budget of
 * 256 KiB.
 */
static size_t budget_env_len(void)
{
    size_t fixed = 2 * (strlen(SELF) + 1) + strlen("budget") + 1 + 4 * sizeof(char *);

    return 256 * KIB - fixed - (LONGEST + 1) - 1;
}

/**
 * Check the `many` mode in the child: return 0 when the arguments after the
 * mode are the five thousand names, and 1 otherwise.
 */
static int child_many(int argc, char *argv[])
{
    int i;

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

/**
 * Check the `budget` mode in the child: return 0 when its one argument and
 * one variable came whole, and 1 otherwise.
 */
static int child_budget(int argc, char *argv[])
{
    return argc == 3 && all_of(argv[2], LONGEST, 'y') && environ[0] != NULL && environ[1] == NULL &&
                   strncmp(environ[0], "E=", 2) == 0 &&
                   all_of(environ[0] + 2, budget_env_len() - 2, 'z')
               ? 0
               : 1;
}

/**
 * Check the `wide` mode in the child: return 0 when the argument and the
 * variable it was sent came, and 1 otherwise.
 */
static int child_wide(int argc, char *argv[])
{
    return argc == 3 && strcmp(argv[2], "wide") == 0 && environ[0] != NULL &&
                   strcmp(environ[0], "W=wide") == 0 && environ[1] == NULL
               ? 0
               : 1;
}

/**
 * Check the `env` mode in the child: return 0 when the environment is the
 * five thousand variables, and 1 otherwise.
 */
static int child_env(void)
{
    char want[8];
    int i;

    for (i = 0; environ[i] != NULL; i++) {
        snprintf(want, sizeof(want), "E%04d", i);
        memcpy(want + 5, "=v", 3);
        if (i >= MANY || strcmp(environ[i], want) != 0) {
            return 1;
        }
    }
    return i == MANY ? 0 : 1;
}

/**
 * Run the child half: return 0 when this image was handed what its mode
 * says, and 1 otherwise or for an unknown mode.
 */
static int child(int argc, char *argv[])
{
    if (getenv("EXECARG_EMPTY") != NULL) {
        return argc == 1 && argv[0] != NULL && argv[0][0] == '\0' && argv[1] == NULL ? 0 : 1;
    }
    if (argc < 2) {
        return 1;
    }
    if (strcmp(argv[1], "many") == 0) {
        return child_many(argc, argv);
    }
    if (strcmp(argv[1], "long") == 0) {
        return argc == 3 && all_of(argv[2], LONGEST, 'x') ? 0 : 1;
    }
    if (strcmp(argv[1], "budget") == 0) {
        return child_budget(argc, argv);
    }
    if (strcmp(argv[1], "wide") == 0) {
        return child_wide(argc, argv);
    }
    if (strcmp(argv[1], "env") == 0) {
        return child_env();
    }
    return 1;
}

/** Fill `s` with `n` copies of `c` and a NUL, and return `s`. */
static char *text(char *s, size_t n, char c)
{
    memset(s, c, n);
    s[n] = '\0';
    return s;
}

/**
 * Check that five thousand arguments, and five thousand variables, are
 * delivered whole. Returns 0, or 1 after the failure is printed; `names`
 * holds the variables afterwards.
 */
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

/**
 * Check that one string of 128 KiB with its NUL is delivered and one a byte
 * longer fails with E2BIG. Returns 0, or 1 after the failure is printed.
 */
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
 * Check the budget under a stack limit of 1 MiB, which is 256 KiB: the path,
 * four strings and four pointers fill it to the byte, split across argv and
 * envp, and are delivered whole; a byte more fails with E2BIG. Returns 0, or
 * 1 after the failure is printed.
 */
static int budget(void)
{
    char *argv[4] = {(char *)SELF, (char *)"budget", NULL, NULL};
    char *envp[2] = {NULL, NULL};
    size_t env_len = budget_env_len();

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

/**
 * Check that pointers that take a 128 KiB budget by themselves fail with
 * E2BIG, and that an empty argv is given one empty argument. Returns 0, or 1
 * after the failure is printed.
 */
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

#if defined __x86_64__
/**
 * Check that an argument and a variable whose addresses are 4 GiB and 8 GiB,
 * zero in their low 32 bits, are delivered and do not end their vectors.
 * Returns 0, or 1 after the failure is printed.
 */
static int wide_pointers(void)
{
    char *arg = mmap((void *)0x100000000UL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    char *var = mmap((void *)0x200000000UL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    char *argv[4] = {(char *)SELF, (char *)"wide", NULL, NULL};
    char *envp[2] = {NULL, NULL};

    if (arg != (char *)0x100000000UL || var != (char *)0x200000000UL) {
        return fail("mapping pages at 4 GiB and 8 GiB");
    }
    strcpy(arg, "wide");
    strcpy(var, "W=wide");
    argv[2] = arg;
    envp[0] = var;
    if (run(argv, envp, 8 * MIB) != 0) {
        return fail("an argument or variable at an address of zero low bits was lost");
    }
    munmap(arg, 4096);
    munmap(var, 4096);
    return 0;
}
#endif

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
#if defined __x86_64__
    if (wide_pointers() != 0) {
        return 1;
    }
#endif

    printf("EXECARGMUSL OK\n");

    return 0;
}
