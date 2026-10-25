/*
 * Check native execution on the desktop that runs the collection.
 * On Windows preparation substitutes winfixture and its declared helpers,
 * including these same complete-output assertions.
 *
 * host-alone: native platform fixture isolation
 * host-tools: git tar
 * host-deps: tests/host/compat/output_sinks.c tools/etude/capture.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c tools/etude/tooling_cache.c
 * host-deps: tests/host/compat/tooling_cache_fixture.c
 * host-deps: tests/host/compat/collection_tools_host.c
 * host-deps: tests/host/compat/command_host.c tools/etude/collection_tools.c
 * host-deps: tools/etude/json.c tools/etude/routine_proof.c
 * host-deps: tools/etude/digest.c utils/sha256.c
 * host-cflags: -I tools/etude -I tests/host/compat -I utils
 * host-cflags: -I tools/etude/include -Wall -Wextra -Werror
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "output_sinks.h"

int etude_test_tooling_cache(const char *tmp);
int etude_test_collection_tools(const char *self, const char *tmp);
int etude_test_collection_child(int argc, char **argv);

int main(int argc, char **argv)
{
    char path[4096];
    const char *tmp = getenv("TMPDIR");
    size_t i;

    int child = etude_test_collection_child(argc, argv);
    if (child >= 0)
        return child;
    if (argc == 3 && strcmp(argv[1], "--big") == 0) {
        for (i = 0; i < 100000; i++)
            putchar((int)('a' + i % 23));
        fflush(stdout);
        fputs("big done\n", stderr);
        return 0;
    }
    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    if (argc == 2 && !strcmp(argv[1], "--collection-only"))
        return etude_test_collection_tools(argv[0], tmp);
    if (etude_test_collection_tools(argv[0], tmp))
        return 1;
    if (etude_test_tooling_cache(tmp))
        return 1;
    if (argv[0][0] == '/')
        return etude_test_output_sinks(argv[0], "--big", tmp);
    if (getcwd(path, sizeof path) == NULL || strlen(path) + strlen(argv[0]) + 2 > sizeof path)
        return 1;
    strcat(path, "/");
    strcat(path, argv[0]);
    return etude_test_output_sinks(path, "--big", tmp);
}
