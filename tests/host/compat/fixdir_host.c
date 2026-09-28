/*
 * The fixture directory of a host test, as fixdir_host.h describes it. This
 * file includes the C library's headers, and the test's own file does not
 * need to.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fixdir_host.h"

#define FIX_PATH_MAX 1024
#define FIX_NAMES    64

#ifndef FIXDIR
static char own[FIX_PATH_MAX];
static int keep;

/** Remove the test's own directory, unless `fix_keep` asked to keep it. */
static void fix_remove(void)
{
    char cmd[FIX_PATH_MAX + 16];

    if (keep) {
        printf("fixtures: kept in %s\n", own);
        return;
    }
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", own);
    if (system(cmd) != 0) {
        printf("fixtures: %s could not be removed\n", own);
    }
}
#endif

const char *fix_dir(void)
{
#ifdef FIXDIR
    return FIXDIR;
#else
    const char *tmp;
    size_t len;

    if (own[0] != '\0') {
        return own;
    }
    tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0' || strpbrk(tmp, " \t\n'\"\\$`") != NULL ||
        strlen(tmp) > FIX_PATH_MAX / 2) {
        tmp = "/tmp";
    }
    len = strlen(tmp);
    while (len > 0 && tmp[len - 1] == '/') {
        len--;
    }
    snprintf(own, sizeof(own), "%.*s/rach2_fixtures.XXXXXX", (int)len, tmp);
    if (mkdtemp(own) == NULL) {
        printf("fixtures: no directory could be made under %s\n", tmp);
        exit(1);
    }
    atexit(fix_remove);
    return own;
#endif
}

const char *fix_path(const char *name)
{
    static char names[FIX_NAMES][64];
    static char paths[FIX_NAMES][FIX_PATH_MAX + 64];
    int i;

    for (i = 0; i < FIX_NAMES && names[i][0] != '\0'; i++) {
        if (strcmp(names[i], name) == 0) {
            return paths[i];
        }
    }
    if (i == FIX_NAMES || strlen(name) >= sizeof(names[0])) {
        printf("fixtures: no room for the name %s\n", name);
        exit(1);
    }
    snprintf(names[i], sizeof(names[i]), "%s", name);
    snprintf(paths[i], sizeof(paths[i]), "%s/%s", fix_dir(), name);
    return paths[i];
}

void fix_keep(void)
{
#ifndef FIXDIR
    keep = 1;
#endif
}
