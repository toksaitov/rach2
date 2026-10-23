/* Read a prepared protocol fixture without running a compiler. */
#ifndef ETUDE_WORKLOAD_HOST_H
#define ETUDE_WORKLOAD_HOST_H

#include <stdio.h>
#include <stdlib.h>

static char *host_workload_read(size_t *size)
{
    const char *root = getenv("HOST_TEST_TOOLS");
    char path[4096], *bytes = NULL;
    FILE *file;
    long length;
    int n;

    *size = 0;
    n = snprintf(path, sizeof(path), "%s/etude-workload",
                 root ? root : "build/tests/host-suite/tools");
    if (n < 0 || n >= (int)sizeof(path) || (file = fopen(path, "rb")) == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 && length <= 64 * 1024 * 1024 &&
        fseek(file, 0, SEEK_SET) == 0) {
        bytes = malloc((size_t)length);
        if (bytes != NULL && fread(bytes, 1, (size_t)length, file) == (size_t)length)
            *size = (size_t)length;
        else {
            free(bytes);
            bytes = NULL;
        }
    }
    fclose(file);
    return bytes;
}

#endif
