/*
 * The fixture directory of a host test, as fixdir_host.h describes it. This
 * file includes the C library's headers, and the test's own file does not
 * need to.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fixdir_host.h"

#define FIX_PATH_MAX 1024
#define FIX_NAMES    64

#ifndef FIXDIR
/* The characters a shell passes through unchanged, which a directory the
 * tests put in their commands unquoted may have. */
#define FIX_PLAIN "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/._+-"

/* The longest $TMPDIR taken, so that every command the tests format has
 * room for the paths in it. */
#define FIX_TMP_MAX 100

static char own[FIX_PATH_MAX];
static int passed;

/** Whether $TMPDIR, `tmp`, can hold the fixture directory: an absolute path
 * of at most FIX_TMP_MAX characters, each one a shell passes unchanged. */
static int fix_plain(const char *tmp)
{
    return tmp != NULL && tmp[0] == '/' && strlen(tmp) <= FIX_TMP_MAX &&
           tmp[strspn(tmp, FIX_PLAIN)] == '\0';
}

/** At exit, remove the test's own directory if it passed, or else keep it
 * and say where, so that a failed test's fixtures can be read. */
static void fix_remove(void)
{
    char cmd[FIX_PATH_MAX + 16];

    if (!passed) {
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
    if (!fix_plain(tmp)) {
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
    fix_format(paths[i], sizeof(paths[i]), "%s/%s", fix_dir(), name);
    return paths[i];
}

void fix_passed(void)
{
#ifndef FIXDIR
    passed = 1;
#endif
}

void fix_format(char *buf, unsigned long size, const char *format, ...)
{
    va_list ap;
    int n;

    va_start(ap, format);
    n = vsnprintf(buf, size, format, ap);
    va_end(ap);
    if (n < 0 || (unsigned long)n >= size) {
        printf("fixtures: %d bytes do not fit in %lu: %.60s\n", n, size, buf);
        exit(1);
    }
}
