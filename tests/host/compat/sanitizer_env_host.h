/* The explicit environment of an instrumented fixture's reexecuted child. */
#ifndef SANITIZER_ENV_HOST_H
#define SANITIZER_ENV_HOST_H

#ifndef _WIN32
#include <stdio.h>
#include <stdlib.h>

static char **host_sanitizer_environment(const char *tool)
{
    static char *environment[] = {"ASAN_OPTIONS=halt_on_error=1:exitcode=1",
                                  "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=1",
                                  NULL, NULL};
#ifdef __APPLE__
    static char symbolizer[4096];
    int length;

    if (tool == NULL || tool[0] != '/') {
        fprintf(stderr, "host fixture: prepared sanitizer symbolizer is missing\n");
        return NULL;
    }
    length = snprintf(symbolizer, sizeof(symbolizer), "ASAN_SYMBOLIZER_PATH=%s", tool);
    if (length < 0 || length >= (int)sizeof(symbolizer)) {
        fprintf(stderr, "host fixture: sanitizer symbolizer path is too long\n");
        return NULL;
    }
    environment[2] = symbolizer;
#else
    (void)tool;
#endif
    return environment;
}
#endif

#endif
