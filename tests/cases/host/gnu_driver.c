/* Enter the prepared GNU tree and execute one source-bound host assertion. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

int main(int argc, char **argv)
{
    const char root[] = "/host-products/host-checks/tree";
    char *args[] = {"/host-products/host", "--one", NULL, NULL, NULL, NULL};

    if (argc != 4 || (strcmp(argv[3], "i686") && strcmp(argv[3], "x86_64")))
        return 2;
    if (chdir(root) != 0 || setenv("ETUDE_TREE", root, 1) != 0 ||
        setenv("HOST_TEST_TOOLS", "/host-products/host-checks/tree/build/tests/host-suite/tools",
               1) != 0 ||
        unsetenv("ETUDE_CASE") != 0 || unsetenv("RUNNER_TRACKING_ID") != 0) {
        fprintf(stderr, "GNU host checks: prepared tree: %s\n", strerror(errno));
        return 2;
    }
    if (geteuid() == 0) {
        struct passwd *account = getpwnam("test");
        if (!account || setenv("HOME", account->pw_dir, 1) || setenv("USER", account->pw_name, 1) ||
            setenv("LOGNAME", account->pw_name, 1) ||
            initgroups(account->pw_name, account->pw_gid) || setgid(account->pw_gid) ||
            setuid(account->pw_uid)) {
            fprintf(stderr, "GNU host checks: cannot become the ordinary test account\n");
            return 2;
        }
    }
    args[2] = argv[1];
    args[3] = argv[2];
    args[4] = argv[3];
    execv(args[0], args);
    fprintf(stderr, "GNU host checks: driver: %s\n", strerror(errno));
    return 2;
}
