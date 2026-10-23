/* Copy a disk range with native file APIs, preserving bytes outside it. */
#include "fileio.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "../../../tools/etude/win/files.h"
#endif

int case_copy_range(const char *source, uint64_t offset, uint64_t length,
                    const char *destination, uint64_t at, int truncate)
{
    unsigned char buffer[16384];
    struct stat a, b;
    FILE *in = NULL, *out = NULL;
    int rc = -1;

    if (!source || !destination || stat(source, &a) != 0 || !S_ISREG(a.st_mode) ||
        a.st_size < 0 || offset > (uint64_t)a.st_size ||
        (strcmp(source, destination) == 0 ||
         (a.st_ino != 0 && stat(destination, &b) == 0 &&
          a.st_dev == b.st_dev && a.st_ino == b.st_ino)))
        return -1;
    if (length == UINT64_MAX)
        length = (uint64_t)a.st_size - offset;
    if (length > (uint64_t)a.st_size - offset || at > INT64_MAX || length > INT64_MAX - at)
        return -1;
    in = fopen(source, "rb");
    if (!in)
        return -1;
    out = fopen(destination, truncate ? "wb" : "r+b");
    if (!out || fseeko(in, (off_t)offset, SEEK_SET) != 0 ||
        fseeko(out, (off_t)at, SEEK_SET) != 0)
        goto done;
    while (length) {
        size_t n = length < sizeof(buffer) ? (size_t)length : sizeof(buffer);
        if (fread(buffer, 1, n, in) != n || fwrite(buffer, 1, n, out) != n)
            goto done;
        length -= n;
    }
    rc = 0;
done:
    if (out && fclose(out) != 0)
        rc = -1;
    if (fclose(in) != 0)
        rc = -1;
    return rc;
}
