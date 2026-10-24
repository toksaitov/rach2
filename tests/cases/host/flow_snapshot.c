/* flow_snapshot.c: compare file identity, modes, times and contents. */
#define _DARWIN_C_SOURCE 1
#define _DEFAULT_SOURCE  1
#define _POSIX_C_SOURCE  200809L
#include "flow_check.h"
#include "sha256.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static int order(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static int snapshot(struct sha256 *hash, const char *path)
{
    struct stat st;
    char record[5000];
    int n;
    unsigned long ns;
    if (lstat(path, &st))
        return 0;
#ifdef __APPLE__
    ns = (unsigned long)st.st_mtimespec.tv_nsec;
#else
    ns = (unsigned long)st.st_mtim.tv_nsec;
#endif
    n = snprintf(record, sizeof(record), "%s %llu %o %llu %lld %lu\n", path,
                 (unsigned long long)st.st_ino, (unsigned)st.st_mode,
                 (unsigned long long)st.st_size, (long long)st.st_mtime, ns);
    if (n < 0 || n >= (int)sizeof(record))
        return 0;
    sha256_update(hash, (const unsigned char *)record, (size_t)n);
    if (S_ISREG(st.st_mode)) {
        FILE *f = fopen(path, "rb");
        unsigned char bytes[8192];
        size_t got;
        int ok;
        if (!f)
            return 0;
        while ((got = fread(bytes, 1, sizeof(bytes), f)))
            sha256_update(hash, bytes, got);
        ok = !ferror(f);
        fclose(f);
        return ok;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        char **names = NULL;
        size_t count = 0, i;
        int ok = 1;
        if (!d)
            return 0;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char **p = realloc(names, (count + 1) * sizeof(*names));
            if (!p) {
                ok = 0;
                break;
            }
            names = p;
            names[count++] = strdup(e->d_name);
        }
        closedir(d);
        if (count > 1)
            qsort(names, count, sizeof(*names), order);
        for (i = 0; i < count; i++) {
            if (snprintf(record, sizeof(record), "%s/%s", path, names[i]) >= (int)sizeof(record) ||
                !snapshot(hash, record))
                ok = 0;
            free(names[i]);
        }
        free(names);
        return ok;
    }
    return 1;
}
int flow_snapshot(const char *const paths[], char hex[65])
{
    struct sha256 hash;
    unsigned char bytes[32];
    unsigned i;
    sha256_init(&hash);
    for (i = 0; paths[i]; i++)
        if (!snapshot(&hash, paths[i]))
            return 0;
    sha256_final(&hash, bytes);
    sha256_hex(bytes, hex);
    return 1;
}
