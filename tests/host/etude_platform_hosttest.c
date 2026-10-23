/*
 * Check native execution on the desktop that runs the collection.
 * On Windows preparation substitutes winfixture and its declared helpers,
 * including these same complete-output assertions.
 *
 * host-deps: tests/host/compat/output_sinks.c tools/etude/capture.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c
 * host-cflags: -I tools/etude/include -Wall -Wextra -Werror
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "output_sinks.h"

int main(int argc, char **argv)
{
    char path[4096];
    const char *tmp = getenv("TMPDIR");
    size_t i;

    if (argc == 3 && strcmp(argv[1], "--big") == 0) {
        for (i = 0; i < 100000; i++)
            putchar((int)('a' + i % 23));
        fflush(stdout);
        fputs("big done\n", stderr);
        return 0;
    }
    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    if (argv[0][0] == '/')
        return etude_test_output_sinks(argv[0], "--big", tmp);
    if (getcwd(path, sizeof path) == NULL || strlen(path) + strlen(argv[0]) + 2 > sizeof path)
        return 1;
    strcat(path, "/");
    strcat(path, argv[0]);
    return etude_test_output_sinks(path, "--big", tmp);
}
