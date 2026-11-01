/* Select a sox_ng mode by preserving the name used to invoke this program. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/**
 * Replace this process with /bin/sox, preserving its arguments and mode name.
 * The caller invokes it as play, rec or soxi. Return 127 if execv fails.
 */
int main(int argc, char **argv)
{
    const char *name;
    (void)argc;
    execv("/bin/sox", argv);
    name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    fprintf(stderr, "%s: cannot execute /bin/sox: %s\n", name, strerror(errno));
    return 127;
}
