/*
 * runutil_group: runutil.c's `watch` with its setpgid replaced, for
 * runutil_group.sh, which checks how the supervisor treats a failed setpgid.
 */
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

static const char *mode; /* the case: assigned, denied, child or session */
static int barrier[2];   /* the pipe on which the parent releases the child */

/** Report a failure of the fixture itself and exit 125, a status runutil
 * does not use, so that it is not taken for runutil's own exit 2. */
static void fixture_error(const char *what)
{
    fprintf(stderr, "runutil-group: setup: %s\n", what);
    exit(125);
}

/**
 * The setpgid that runutil.c calls, both in the parent, as
 * `setpgid(child, child)`, and in the child, as `setpgid(0, 0)`.
 *
 * The parent's call goes first: in the `assigned` case it puts the child in
 * a group of its own and succeeds, and otherwise it fails with EPERM. It then
 * releases the child, whose call checks that the group is what the case set
 * up and then fails with EPERM, except in two cases: in `child` it is the
 * real call, which succeeds, and in `session` the child makes itself a
 * session leader first, so that the real call fails with EPERM while the
 * child already leads its own group. A setup that does not hold exits 125.
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

/**
 * `runutil_group CASE LEDGER` runs runutil's `watch` over this program's own
 * `report`, with LEDGER as its ledger, and returns runutil's status.
 *
 * `report` checks that it leads its own process group and exits 23, so that
 * runutil_group.sh can tell its status from runutil's. runutil should run it
 * whenever the child ends up leading a group, and exit 2 when it does not.
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
