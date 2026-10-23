/* Execute GNU checks in a private tree; prepared source inputs stay immutable. */
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fixture.h"

static const char prepared[] = "/host-products/host-checks/tree";

/** Only the four declared GNU groups can reserve a workspace. */
static int valid_group(const char *group)
{
    return !strcmp(group, "tests") || !strcmp(group, "etude") || !strcmp(group, "checkers") ||
           !strcmp(group, "fuzz");
}

/** Become the ordinary account before copying or executing a writable fixture. */
static int ordinary(void)
{
    struct passwd *account = getpwnam("test");
    if (!account || setenv("HOME", account->pw_dir, 1) || setenv("USER", account->pw_name, 1) ||
        setenv("LOGNAME", account->pw_name, 1))
        return -1;
    if (geteuid() == 0 && (initgroups(account->pw_name, account->pw_gid) ||
                           setgid(account->pw_gid) || setuid(account->pw_uid)))
        return -1;
    return geteuid() == account->pw_uid ? 0 : -1;
}

/** Copy once per group in its private helper, never through links to inputs. */
static int workspace(const char *path)
{
    char marker[4096];
    struct stat st;
    int n = snprintf(marker, sizeof(marker), "%s/.etude-host-copy", path);
    if (n < 0 || n >= (int)sizeof(marker))
        return -1;
    if (mkdir(path, 0700) == 0) {
        fprintf(stderr, "GNU host checks: writable fixtures: %s\n", path);
        if (!fx_tree(prepared, path) || chmod(path, 0700) || !fx_text(marker, prepared))
            return -1;
    }
    return lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
                   !fx_equal(marker, prepared)
               ? -1
               : 0;
}

static int environment(const char *root)
{
    char tools[4096], scratch[4096];
    int n = snprintf(tools, sizeof(tools), "%s/build/tests/host-suite/tools", root);
    int size = snprintf(scratch, sizeof(scratch), "%s/.checks", root);
    return n < 0 || n >= (int)sizeof(tools) || size < 0 || size >= (int)sizeof(scratch) ||
                   !fx_mkdir(scratch) || setenv("HOST_TEST_WORKDIR", scratch, 1) || chdir(root) ||
                   setenv("ETUDE_TREE", root, 1) || setenv("HOST_TEST_TOOLS", tools, 1) ||
                   unsetenv("ETUDE_CASE") || unsetenv("RUNNER_TRACKING_ID")
               ? -1
               : 0;
}

int main(int argc, char **argv)
{
    char root[4096];
    char *args[] = {"/host-products/host", "--one", NULL, NULL, NULL, NULL};
    int n;

    if (argc != 4 || !valid_group(argv[1]) ||
        (strcmp(argv[3], "i686") && strcmp(argv[3], "x86_64")))
        return 2;
    n = snprintf(root, sizeof(root), "/host-work/host-checks.%s.%s", argv[1], argv[3]);
    if (n < 0 || n >= (int)sizeof(root) || ordinary() != 0)
        goto failed;
    if (!strcmp(argv[2], "--cleanup"))
        return fx_remove(root) ? 0 : 2;
    if (workspace(root) != 0 || environment(root) != 0)
        goto failed;
    args[2] = argv[1];
    args[3] = argv[2];
    args[4] = argv[3];
    execv(args[0], args);
failed:
    fprintf(stderr, "GNU host checks: private tree or driver: %s; evidence: %s\n", strerror(errno),
            root);
    return 2;
}
