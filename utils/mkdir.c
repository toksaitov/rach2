/**
 * Create each named directory, with mode 0777 less the umask.
 *
 * Parents are not made. A directory that cannot be made is reported and the
 * rest are still tried; the status is 1 when any failed.
 */

#include <stdio.h>
#include <sys/stat.h>

/** Make each operand a directory. No operand is a usage error, status 1. */
int main(int argc, char *argv[])
{
    int i;
    int failed = 0;

    if (argc < 2) {
        fputs("Usage: mkdir <dir> [...]\n", stderr);
        return 1;
    }

    for (i = 1; i < argc; ++i) {
        if (mkdir(argv[i], 0777) < 0) {
            fprintf(stderr, "mkdir: failed to create directory '%s'\n", argv[i]);
            failed = 1;
        }
    }

    return failed;
}
