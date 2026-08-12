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
 * The environment is the one this probe was given, which carries the `PATH` the
 * shell resolves against.
 */
static int ish_status(const char *line)
{
    extern char **environ;
    char *argv[4];
    int status = 0;
    int child = fork();

    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        argv[0] = (char *)"-ish";
        argv[1] = (char *)"-c";
        argv[2] = (char *)line;
        argv[3] = NULL;
        execve(SHELL, argv, environ);
        _exit(125);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return -1;
    }

    return WEXITSTATUS(status);
}

/**
 * Run the cases and report.
 */
int main(void)
{
    int rc;

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

    printf("ISHMUSL OK\n");

    return 0;
}
