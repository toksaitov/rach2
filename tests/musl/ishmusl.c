/**
 * The ISH shell's one-line form, `ish -c command`, and the status it reports.
 *
 * That form is not a convenience: it is how one program asks a shell to run one
 * thing as somebody, which is what `su -c` does, and the status it comes back
 * with is the only thing the caller learns. A shell that answered 0 for a
 * command it never found would have `su -c` report work done that never ran.
 *
 * The cases are the four answers a caller can get: the command's own status,
 * whether zero or not; 127 for a name that resolved to nothing; 126 for a
 * redirection that could not be opened, since a command run without the
 * redirection it asked for is not the command that was asked for; and 128 + N
 * for a command a signal killed.
 *
 * `/ish` and the helpers it runs are staged into the test image beside this
 * probe. Prints "ISHMUSL OK" on success and "ISHMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>

#define SHELL "/ish"

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("ISHMUSL FAIL (%s)\n", what);

    return 1;
}

/**
 * Run `line` through `ish -c` and return the shell's exit status, or -1.
 *
 * Use the supplied environment. Bits 0 and 1 of `closed` close stdin and
 * stdout; `output`, when nonnegative, replaces inherited stdout first.
 */
static int ish_run(const char *line, char **environment, int closed, int output)
{
    char *argv[4];
    int status = 0;
    int child = fork();

    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        if (output >= 0) {
            if (dup2(output, 1) < 0) {
                _exit(125);
            }
            close(output);
        }
        if (closed & 1) {
            close(0);
        }
        if (closed & 2) {
            close(1);
        }
        argv[0] = (char *)"-ish";
        argv[1] = (char *)"-c";
        argv[2] = (char *)line;
        argv[3] = NULL;
#ifdef ISH_HOSTTEST
        argv[2] = strdup(line);
        if (argv[2] == NULL) {
            _exit(125);
        }
        _exit(ish_main(3, argv, environment));
#else
        execve(SHELL, argv, environment);
#endif
        _exit(125);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return -1;
    }

    return WEXITSTATUS(status);
}

/** Run a command with the probe's inherited descriptors and environment. */
static int ish_status(const char *line)
{
    extern char **environ;

    return ish_run(line, environ, 0, -1);
}

/** Require an exact file value, including its length. */
static int file_equals(const char *path, const char *expected)
{
    char buf[64];
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        return 0;
    }
    n = read(fd, buf, sizeof(buf));
    close(fd);
    return n == (ssize_t)strlen(expected) && memcmp(buf, expected, (size_t)n) == 0;
}

/** Run input from a regular file and check the shell's output and status. */
static int check_input_case(const char *input, const char *output, int expected, const char *name)
{
    char *argv[] = {(char *)"ish", NULL};
    char *environment[] = {(char *)"PATH=/bin", NULL};
    int source = open("ishmusl.input", O_RDWR | O_CREAT | O_TRUNC, 0600);
    int capture = open("ishmusl.capture", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int child = -1;
    int status = 0;
    int result = 1;
    size_t length = strlen(input);

    if (source >= 0 && capture >= 0 && write(source, input, length) == (ssize_t)length &&
        lseek(source, 0, SEEK_SET) == 0) {
        fflush(NULL);
        child = fork();
        if (child == 0) {
            if (dup2(source, 0) < 0 || dup2(capture, 1) < 0) {
                _exit(125);
            }
            close(source);
            close(capture);
#ifdef ISH_HOSTTEST
            _exit(ish_main(1, argv, environment));
#else
            execve(SHELL, argv, environment);
            _exit(125);
#endif
        }
        if (child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
            WEXITSTATUS(status) == expected && file_equals("ishmusl.capture", output)) {
            result = 0;
        }
    }
    if (source >= 0) {
        close(source);
    }
    if (capture >= 0) {
        close(capture);
    }
    unlink("ishmusl.input");
    unlink("ishmusl.capture");
    return result ? fail(name) : 0;
}

/** Check complete lines, read boundaries, EOF and rejection of long lines. */
static int check_input(void)
{
    char boundary[280];
    char split[280];
    char overlong[540];
    int result = 0;

    result |= check_input_case("", "", 0, "empty input");
    result |= check_input_case("echo one\necho two\n", "one\ntwo\n", 0, "input lines");
    result |= check_input_case("echo final", "final\n", 0, "final input line");
    result |= check_input_case("no-such-command-anywhere\necho next\n", "next\n", 0,
                               "input command failure");
    result |=
        check_input_case("echo before\nexit 7\necho after\n", "before\n", 7, "input exit builtin");
    memset(split, '\n', 250);
    strcpy(split + 250, "echo split\n");
    result |= check_input_case(split, "split\n", 0, "split input line");
    memset(boundary, ' ', 255);
    memcpy(boundary, "echo limit", 10);
    strcpy(boundary + 255, "\necho next\n");
    result |= check_input_case(boundary, "limit\nnext\n", 0, "input line limit");
    boundary[255] = '\0';
    result |= check_input_case(boundary, "limit\n", 0, "input line limit at eof");
    memset(overlong, ' ', 510);
    memcpy(overlong, "echo prefix", 11);
    strcpy(overlong + 510, "echo suffix\necho next\n");
    result |= check_input_case(overlong, "next\n", 0, "long input line");
    overlong[256] = '\0';
    result |= check_input_case(overlong, "", 0, "long input line at eof");
    return result;
}

/** Redirection must work when open returns either standard descriptor. */
static int check_descriptors(void)
{
    char *environment[] = {"PATH=/bin", NULL};
    int closed;

    for (closed = 0; closed < 4; closed++) {
        int capture = open("ishmusl.capture", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int status;

        if (capture < 0) {
            return fail("cannot create output capture");
        }
        status = ish_run("/bin/echo hello > ishmusl.output", environment, closed, capture);
        close(capture);
        if (status != 0 || !file_equals("ishmusl.output", "hello\n") ||
            !file_equals("ishmusl.capture", "")) {
            return fail("output redirection lost a standard descriptor");
        }
    }
    unlink("ishmusl.output");
    unlink("ishmusl.capture");
    return 0;
}

/** HOME and PATH require an exact, case-sensitive name followed by '='. */
static int check_environment(void)
{
    char *prefix[] = {"HOMELESS=/no/such/home", "HOME=/", "PATHETIC=/no/such/bin", "PATH=/bin",
                      NULL};
    char *lower[] = {"home=/no/such/home", "HOME=/", "path=/no/such/bin", "PATH=/bin", NULL};
    char *missing[] = {"HOMELESS=/", "home=/", "PATHETIC=/bin", "path=/bin", NULL};
    char **cases[] = {prefix, lower};
    unsigned int i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (ish_run("cd", cases[i], 0, -1) != 0) {
            return fail("home matched a prefix or a different case");
        }
        if (ish_run("echo environment > ishmusl.environment", cases[i], 0, -1) != 0 ||
            !file_equals("ishmusl.environment", "environment\n")) {
            return fail("path matched a prefix or a different case");
        }
    }
    unlink("ishmusl.environment");
    if (ish_run("cd", missing, 0, -1) != 1 || ish_run("echo", missing, 0, -1) != 127) {
        return fail("a similarly named variable supplied home or path");
    }
    return 0;
}

/**
 * Run the cases and report.
 */
int main(void)
{
    int rc;

    if (check_descriptors() || check_environment() || check_input()) {
        return 1;
    }
    rc = ish_status("/musl/statushelper 0");
    if (rc != 0) {
        return fail("a command that succeeds did not report 0");
    }
    rc = ish_status("/musl/statushelper 3");
    if (rc != 3) {
        return fail("a command that fails did not report its own status");
    }
    rc = ish_status("no-such-command-anywhere");
    if (rc != 127) {
        return fail("a name that resolved to nothing did not report 127");
    }
    rc = ish_status("/musl/statushelper 0 < /no/such/input");
    if (rc != 126) {
        return fail("a redirection that could not be opened did not report 126");
    }
    rc = ish_status("/musl/statushelper signal");
    if (rc != 128 + SIGTERM) {
        return fail("a command a signal killed did not report 128 + the signal");
    }

    /* The builtins answer the same way. They are the shell's own work, so a
     * caller learns nothing about them from a child's status, and a `cd` that
     * went nowhere leaves the next command in the caller's line running
     * somewhere the caller did not choose. */
    rc = ish_status("cd /");
    if (rc != 0) {
        return fail("a cd that worked did not report 0");
    }
    rc = ish_status("cd /no/such/directory");
    if (rc != 1) {
        return fail("a cd to nowhere reported success");
    }
    rc = ish_status("cd / < /no/such/input");
    if (rc != 126) {
        return fail("a builtin ran with a redirection that could not be opened");
    }
    /* A redirection that DOES open must not eat the builtin's operand: pulling
     * it out of the argument array clears the slots it occupied, so a builtin
     * reading the operand from where it was before the pull would find a
     * redirection's leftovers. A `cd` with somewhere to go must still go there,
     * and a bare `cd` must do the same thing with a redirection as without one,
     * whatever that is: this probe runs with no `HOME` in its environment, so
     * here it is a refusal. */
    rc = ish_status("cd / > /ishmusl.redirect");
    if (rc != 0) {
        return fail("a redirection took the cd operand with it");
    }
    rc = ish_status("cd /no/such/directory > /ishmusl.redirect");
    if (rc != 1) {
        return fail("a cd to nowhere with a redirection reported success");
    }
    if (ish_status("cd > /ishmusl.redirect") != ish_status("cd")) {
        return fail("a redirection changed what a bare cd did");
    }
    rc = ish_status("exit > /ishmusl.redirect");
    if (rc != 0) {
        return fail("exit with a redirection did not report 0");
    }
    rc = ish_status("exit 7");
    if (rc != 7) {
        return fail("exit did not report the status it was given");
    }
    unlink("/ishmusl.redirect");

    printf("ISHMUSL OK\n");

    return 0;
}
