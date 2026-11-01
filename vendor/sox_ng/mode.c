/* Enter a multicall program with the name and arguments of this command. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef MULTICALL_PATH
#define MULTICALL_PATH "/bin/sox"
#endif

/**
 * Replace this process with MULTICALL_PATH, preserving its name and arguments.
 * The caller supplies a name the target accepts. Return 127 if execv fails.
 */
int main(int argc, char **argv)
{
    const char *name;
    (void)argc;
    execv(MULTICALL_PATH, argv);
    name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    fprintf(stderr, "%s: cannot execute %s: %s\n", name, MULTICALL_PATH, strerror(errno));
    return 127;
}
