/** Create each named directory. */

#include <stdio.h>
#include <sys/stat.h>

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
