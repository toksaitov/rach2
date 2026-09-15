#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *mode;
static int barrier[2];

/** Abort setup without mistaking a fixture failure for runutil's exit 2. */
static void fixture_error(const char *what)
{
    fprintf(stderr, "runutil-group: setup: %s\n", what);
    exit(125);
}

/** Release the child only after the parent has chosen its group state.
 * Real membership calls verify setup before an injected error reaches runutil.
 */
static int fixture_setpgid(pid_t pid, pid_t group)
{
    char ready;
    int assigned = !strcmp(mode, "assigned");
    if (pid != 0) {
        close(barrier[0]);
        if (assigned && setpgid(pid, group) != 0 && getpgid(pid) != group)
            fixture_error("the parent could not establish the child's group");
        if (write(barrier[1], "x", 1) != 1)
            fixture_error("the parent could not release the child");
        close(barrier[1]);
        if (assigned)
            return 0;
    } else {
        alarm(10);
        close(barrier[1]);
        if (read(barrier[0], &ready, 1) != 1 || ready != 'x')
            fixture_error("the child did not receive the parent's release");
        close(barrier[0]);
        if ((getpgrp() == getpid()) != assigned)
            fixture_error("the child's group does not match the setup");
        if (!strcmp(mode, "child"))
            return setpgid(pid, group);
        if (!strcmp(mode, "session")) {
            if (setsid() < 0)
                fixture_error("setsid failed");
            if (setpgid(pid, group) != -1 || errno != EPERM)
                fixture_error("a session leader did not receive EPERM");
            return -1;
        }
    }
    errno = EPERM;
    return -1;
}

#define setpgid fixture_setpgid
#define main    runutil_main
#include "runutil.c"
#undef main
#undef setpgid

/** Run the real supervisor with injected errors or a session-leader EPERM.
 * The executed command verifies its group and returns 23 to test status relay.
 */
int main(int argc, char **argv)
{
    char *watch[] = {argv[0], "watch", "group", "5", NULL, argv[0], "report", NULL};
    if (argc == 2 && !strcmp(argv[1], "report")) {
        if (getpgrp() != getpid())
            fixture_error("the executed command is in the wrong group");
        puts("runutil-group: result: command ran in its own group");
        return 23;
    }
    if (argc != 3)
        fixture_error("expected a case and a ledger path");
    mode = argv[1];
    if (strcmp(mode, "assigned") && strcmp(mode, "denied") && strcmp(mode, "child") &&
        strcmp(mode, "session"))
        fixture_error("unknown case");
    if (pipe(barrier) != 0)
        fixture_error("pipe failed");
    watch[4] = argv[2];
    return runutil_main(7, watch);
}
