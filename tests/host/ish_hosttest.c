/**
 * Run the ISH descriptor and environment probe against the hosted shell.
 *
 * host-deps: ish/ish_shell_utilities.c ish/ish_cstring_utilities.c
 * host-cflags: -DISH_USE_STDLIB
 *
 * The same cases run through execve of the freestanding shell in ishmusl.
 */
#define main ish_main
#include "../../ish/ish.c"
#undef main
#define ISH_HOSTTEST
#define main ish_probe_main
#include "../musl/ishmusl.c"
#undef main

/** Keep probe output files inside a private directory in this worktree. */
int main(void)
{
    char directory[] = "build/ish-hosttest.XXXXXX";
    int result;

    if (mkdtemp(directory) == NULL || chdir(directory) != 0) {
        perror("ish_hosttest: temporary directory");
        return 1;
    }
    result = check_descriptors() | check_environment();
    unlink("ishmusl.output");
    unlink("ishmusl.capture");
    unlink("ishmusl.environment");
    if (chdir("../..") != 0 || rmdir(directory) != 0) {
        perror("ish_hosttest: cleanup");
        return 1;
    }
    if (result == 0) {
        puts("ish_hosttest: ok");
    }
    return result;
}
